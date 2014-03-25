/****************************************
 * a2dp-alsa.c
 * Enable A2DP sink and source on ALSA devices using bluez DBus API.
 * In short - it enables remote devices to send sound to the computer (sink)
 * and enables the computer (source) to send sound to bluetooth speakers.
 * 
 * For bluez 4.x.
 * 
 * Copyright (C) James Budiono 2013
 * License: GNU GPL Version 3 or later
 * Version 1: June 2013
 ****************************************/
// std includes
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <poll.h>
#include "uthash.h"

// our own defines
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

//#define DEBUG
#ifdef DEBUG
	#define debug_print(...) (fprintf (stderr, __VA_ARGS__))
#else
	#define debug_print(...)
#endif

// bluez specific defines & includes
#define FIND_ADAPTER "FindAdapter"
#define DEFAULT_ADAPTER "DefaultAdapter"
#define ACCESS_TYPE "rw"

// sink and source uuid and endpoints
#define A2DP_SINK_UUID		"0000110b-0000-1000-8000-00805f9b34fb"
#define A2DP_SINK_ENDPOINT "/MediaEndpoint/A2DPSink" // bt --> alsa (sink for bt)
#define A2DP_SOURCE_UUID	"0000110a-0000-1000-8000-00805f9b34fb"
#define A2DP_SOURCE_ENDPOINT "/MediaEndpoint/A2DPSource" // alsa --> bt (source for bt)

#include "a2dp-codecs.h"	// from bluez - some sbc constants
#include "ipc.h"			// from bluez - some sbc constants
#include "rtp.h"			// from bluez - packet headers

// sbc stuff
#include "sbc/sbc.h"

// structs and prototypes

typedef struct {
	// sync and command management
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	pthread_t t_handle; //thread handle
	volatile enum {
		IO_CMD_IDLE = 0,
		IO_CMD_RUNNING,
		IO_CMD_TERMINATE
	} command;
	enum {
		STATE_DISCONNECTED = 0,
		STATE_CONNECTED,
		STATE_PLAYING
	} prev_state;
	
	// transport_path - required to get fd and mtus
	char *transport_path;	// also the hash key
	char *dev_path;			// so that audiosource/sink event can find us
	
	// the actual fd and mtus for streaming
	int fd, read_mtu, write_mtu;
	int write; //false = read, true - write	
	
	// codec stuff
	a2dp_sbc_t cap;
	sbc_t sbc;
	
	// persistent stuff for encoding purpose
	uint16_t seq_num;   //cumulative packet number
	uint32_t timestamp; //timestamp
	
	//hashtable management
	UT_hash_handle hh;
} io_thread_tcb_s; //the I/O thread control block.

void *io_thread_run(void *ptr);
void io_thread_set_command (io_thread_tcb_s *data, int command);
io_thread_tcb_s *create_io_thread();
void destroy_io_thread(io_thread_tcb_s *p);
int transport_acquire (DBusConnection *conn, char *transport_path, int *fd, int *read_mtu, int *write_mtu);
int transport_release (DBusConnection *conn, char *transport_path);

// globals
int quit=0;       // when set to 1, program terminates
int run_once = 0; // only run output once, then exit

//////////////////////////////// DBUS HELPERS ////////////////////////////////

/*****************//**
 * Handle dbus error and clear error message block
 * 
 * @param [in] The error object
 * @param [in] function where the error happens
 * @param [in] line number where the error happens
 * @returns TRUE if successful, FALSE otherwise
 ********************/
int handle_dbus_error (DBusError *err, const char *func, int line) {
	if (dbus_error_is_set (err)) {
		fprintf(stderr, "DBus error %s at %u: %s\n", func, line, err->message);
		dbus_error_free(err);
		return 1;
	}
	return 0;
}

/*****************//**
 * Connect to the system message bus
 * 
 * @param [out] connection object, if function is successful, other wise it is unchanged.
 * @returns TRUE if successful, FALSE otherwise
 ********************/
int get_system_bus(DBusConnection **system_bus) {
	DBusError err;
	DBusConnection* conn;

	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	handle_dbus_error (&err, __FUNCTION__, __LINE__);
	if (NULL == conn) return 0;
	
	*system_bus = conn;
	debug_print("Name %s\n", dbus_bus_get_unique_name (conn));
	return 1;
}

/*****************//**
 * Get the bluetooth device's path
 * 
 * @param [in] connection to use
 * @param [in] adapter to use (hci0, or bluetooth address, or NULL to use default)
 * @param [out] the object path, if function is successful, other wise it is unchanged.
 * @returns TRUE if successful, FALSE otherwise
 ********************/
int get_bluetooth_object(DBusConnection* conn, char *device, char **dev_path) {
	DBusMessage *msg, *reply;
	DBusMessageIter iter;
	DBusError err;
	char *s, *method = FIND_ADAPTER;

	dbus_error_init(&err);
	debug_print("Getting object path for adapter %s\n", device);

	// create a new method call msg
	if (!device) method = DEFAULT_ADAPTER;
	msg = dbus_message_new_method_call("org.bluez",
		"/",					  // object to call on
		"org.bluez.Manager",	  // interface to call on
		method);				  // method name

	// append arguments
	dbus_message_iter_init_append(msg, &iter);
	if (device) dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &device);

	// send message and wait for reply, -1 means wait forever
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
	handle_dbus_error (&err, __FUNCTION__, __LINE__);
	if (!reply) {
		fprintf(stderr, "Reply Null\n");
		return 0;
	}

	// read the parameters
	if (dbus_message_iter_init(reply, &iter) &&
		DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(&iter))
			dbus_message_iter_get_basic(&iter, &s);
	else return 0;
	
	// free reply and close connection
	debug_print("Object path for %s is %s\n", device, s);
	dbus_message_unref(msg);
	dbus_message_unref(reply);
	*dev_path = strdup (s);
	return 1;
}

/*****************//**
 * Add a dict entry of a variant of simple types
 * 
 * @param [in] array iter to add to 
 * @param [in] key
 * @param [in] type (must be simple types)
 * @param [in] value
 ********************/
void util_add_dict_variant_entry (DBusMessageIter *iter, char *key, int type, void *value) {
	DBusMessageIter dict, variant;
	dbus_message_iter_open_container (iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict); 
		dbus_message_iter_append_basic (&dict, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container (&dict, DBUS_TYPE_VARIANT, (char *)&type, &variant);
			dbus_message_iter_append_basic (&variant, type, &value);
		dbus_message_iter_close_container (&dict, &variant);
	dbus_message_iter_close_container (iter, &dict);
}

/*****************//**
 * Add a dict entry of an array of of simple types
 * 
 * @param [in] array iter to add to 
 * @param [in] key
 * @param [in] type (must be simple types)
 * @param [in] pointer to the array
 * @param [in] number of elements (not size in bytes!)
 ********************/
void util_add_dict_array_entry (DBusMessageIter *iter, char *key, int type, void *buf, int elements) {
	DBusMessageIter dict, variant, array;
	char array_type[5] = "a";
	strncat (array_type, (char*)&type, sizeof(array_type));
	
	dbus_message_iter_open_container (iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
		dbus_message_iter_append_basic (&dict, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container (&dict, DBUS_TYPE_VARIANT, array_type, &variant);
			dbus_message_iter_open_container (&variant, DBUS_TYPE_ARRAY, (char *)&type, &array);
				dbus_message_iter_append_fixed_array (&array, type, &buf, elements);
			dbus_message_iter_close_container (&variant, &array);
		dbus_message_iter_close_container (&dict, &variant);
	dbus_message_iter_close_container (iter, &dict);
}



//////////////////////////////// BLUEZ AUDIO/MEDIA HELPERS ////////////////////////////////

/*****************//**
 * Register our "endpoint" handler to bluez audio system.
 * As part of its job, it returns supported codecs and codec parameters, as well
 * as what functions are we doing here (A2DP source, A2DP sink, HFP, etc - for
 * this program it will be A2DP sink).
 * 
 * @param [in] system bus connection
 * @param [in] bluetooth object to register to
 * @returns TRUE means ok, FALSE means something is wrong
 ********************/
int media_register_endpoint(DBusConnection* conn, char *bt_object, char *endpoint, char *uuid) {
	DBusMessage *msg, *reply;
	DBusMessageIter iter, iterarray;
	DBusError err;
	
	a2dp_sbc_t capabilities;
	capabilities.channel_mode = BT_A2DP_CHANNEL_MODE_MONO | BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL |
								BT_A2DP_CHANNEL_MODE_STEREO | BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
	capabilities.frequency = BT_SBC_SAMPLING_FREQ_16000 | BT_SBC_SAMPLING_FREQ_32000 |
							 BT_SBC_SAMPLING_FREQ_44100 | BT_SBC_SAMPLING_FREQ_48000;
	capabilities.allocation_method = BT_A2DP_ALLOCATION_SNR | BT_A2DP_ALLOCATION_LOUDNESS;
	capabilities.subbands = BT_A2DP_SUBBANDS_4 | BT_A2DP_SUBBANDS_8;
	capabilities.block_length = BT_A2DP_BLOCK_LENGTH_4 | BT_A2DP_BLOCK_LENGTH_8 |
								BT_A2DP_BLOCK_LENGTH_12 | BT_A2DP_BLOCK_LENGTH_16;
	capabilities.min_bitpool = MIN_BITPOOL;
	capabilities.max_bitpool = MAX_BITPOOL;

	dbus_error_init(&err);	
	msg = dbus_message_new_method_call("org.bluez",
		bt_object,			  // object to call on
		"org.bluez.Media",	  // interface to call on
		"RegisterEndpoint");  // method name
	
	//build the parameters
	dbus_message_iter_init_append (msg, &iter);
	
	//first param - object path
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_OBJECT_PATH, &endpoint);
	//second param - properties
	dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, "{sv}", &iterarray);
		util_add_dict_variant_entry (&iterarray, "UUID", DBUS_TYPE_STRING, uuid);
		util_add_dict_variant_entry (&iterarray, "Codec", DBUS_TYPE_BYTE, A2DP_CODEC_SBC);
		util_add_dict_array_entry (&iterarray, "Capabilities", DBUS_TYPE_BYTE, &capabilities, sizeof (capabilities));
	dbus_message_iter_close_container (&iter, &iterarray); 
	
	//char *buf; int buflen; dbus_message_marshal (msg, &buf, &buflen); write (1, buf, buflen); return 0;
	
	//make the call
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
	handle_dbus_error (&err, __FUNCTION__, __LINE__);
	if (!reply) {
		fprintf(stderr, "Reply Null\n");
		return 0;
	}
	
	dbus_message_unref(msg);
	dbus_message_unref(reply);
	return 1;
}

/*****************//**
 * Get the transport (ie, the actual file descriptor) for streaming (ie, read/write)
 * the audio data
 * 
 * @param [in] system bus connection
 * @param [in] transport object path (must come from MediaEndpoint.SetConfiguration)
 * @param [out] file descriptor
 * @param [out] maximum size to read per transaction
 * @param [out] maximum size to write per transaction
 * @returns TRUE if ok, FALSE means something is wrong
 ********************/
int transport_acquire (DBusConnection *conn, char *transport_path, int *fd, int *read_mtu, int *write_mtu) {
	DBusMessage *msg, *reply;
	DBusMessageIter iter;
	DBusError err;
	char *access_type = ACCESS_TYPE;
	
	debug_print ("acquire %s\n", transport_path);
	dbus_error_init(&err);	
	msg = dbus_message_new_method_call("org.bluez",
		transport_path,			 	// object to call on
		"org.bluez.MediaTransport",	  // interface to call on
		"Acquire");  			// method name
	
	//build the parameters
	dbus_message_iter_init_append (msg, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &access_type);

	//make the call
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
	handle_dbus_error (&err, __FUNCTION__, __LINE__);
	if (!reply) {
		fprintf(stderr, "Reply Null\n");
		return 0;
	}
	
	//read the reply
	if (!dbus_message_get_args(reply, &err, 
			DBUS_TYPE_UNIX_FD, fd, 
			DBUS_TYPE_UINT16, read_mtu, 
			DBUS_TYPE_UINT16, write_mtu, 
			DBUS_TYPE_INVALID)) 	
	{
		handle_dbus_error (&err, __FUNCTION__, __LINE__);
		return 0;
	}
	
	//clean up
	dbus_message_unref(msg);
	dbus_message_unref(reply);
	return 1;
}

/*****************//**
 * Release the transport (ie, the file descriptor) 
 * Note: this doesn't need to be called if transport is closed by 
 * "MediaEndpoint.ClearConfiguration". It is only needed if the app wishes to
 * release control of the fd while the stream is still active (e.g - suspend 
 * stream, pausing, etc).
 * 
 * @param [in] system bus connection
 * @param [in] transport object path (must come from MediaEndpoint.SetConfiguration)
 ********************/
int transport_release (DBusConnection *conn, char *transport_path) {
	DBusMessage *msg, *reply;
	DBusMessageIter iter;
	DBusError err;
	char *access_type = ACCESS_TYPE;
	
	debug_print ("release %s\n", transport_path);
	dbus_error_init(&err);	
	msg = dbus_message_new_method_call("org.bluez",
		transport_path,		 	// object to call on
		"org.bluez.MediaTransport",	  // interface to call on
		"Release");  			// method name
	
	//build the parameters
	dbus_message_iter_init_append (msg, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &access_type);

	//make the call
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &err);
	handle_dbus_error (&err, __FUNCTION__, __LINE__);
	if (!reply) {
		fprintf(stderr, "Reply Null\n");
		return 0;
	}
		
	//clean up
	dbus_message_unref(msg);
	dbus_message_unref(reply);
	return 1;
}

/*****************//**
 * Helper to calculate the optimum bitpool, given the sampling frequency,
 * and number of channels.
 * Taken verbatim from pulseaudio 2.1 
 * (which took it from bluez audio - a2dp.c & pcm_bluetooth.c - default_bitpool)
 * 
 * @param [in] frequency
 * @param [in] channel mode
 * @returns coded SBC bitpool
 *********************/
static uint8_t a2dp_default_bitpool(uint8_t freq, uint8_t mode) {

    switch (freq) {
        case BT_SBC_SAMPLING_FREQ_16000:
        case BT_SBC_SAMPLING_FREQ_32000:
            return 53;

        case BT_SBC_SAMPLING_FREQ_44100:

            switch (mode) {
                case BT_A2DP_CHANNEL_MODE_MONO:
                case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
                    return 31;

                case BT_A2DP_CHANNEL_MODE_STEREO:
                case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
                    return 53;

                default:
                    fprintf (stderr, "Invalid channel mode %u", mode);
                    return 53;
            }

        case BT_SBC_SAMPLING_FREQ_48000:

            switch (mode) {
                case BT_A2DP_CHANNEL_MODE_MONO:
                case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
                    return 29;

                case BT_A2DP_CHANNEL_MODE_STEREO:
                case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
                    return 51;

                default:
                    fprintf (stderr, "Invalid channel mode %u", mode);
                    return 51;
            }

        default:
            fprintf (stderr, "Invalid sampling freq %u", freq);
            return 53;
    }
}

/*****************//**
 * Helper to setup sbc params from a2dp_sbc_t
 * Modified from pulseaudio 2.1 (which took it from bluez - pcm_bluetooth.c
 * - bluetooth_a2dp_setup)
 * 
 * @param [in] sbc codec configuration
 * @param [in] bluez codec capability configuration
 *********************/
void setup_sbc(sbc_t *sbc, a2dp_sbc_t *cap)  {
	
    switch (cap->frequency) {
        case BT_SBC_SAMPLING_FREQ_16000:
            sbc->frequency = SBC_FREQ_16000;
            break;
        case BT_SBC_SAMPLING_FREQ_32000:
            sbc->frequency = SBC_FREQ_32000;
            break;
        case BT_SBC_SAMPLING_FREQ_44100:
            sbc->frequency = SBC_FREQ_44100;
            break;
        case BT_SBC_SAMPLING_FREQ_48000:
            sbc->frequency = SBC_FREQ_48000;
            break;
        default:
			fprintf (stderr, "No supported frequency");
    }

    switch (cap->channel_mode) {
        case BT_A2DP_CHANNEL_MODE_MONO:
            sbc->mode = SBC_MODE_MONO;
            break;
        case BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL:
            sbc->mode = SBC_MODE_DUAL_CHANNEL;
            break;
        case BT_A2DP_CHANNEL_MODE_STEREO:
            sbc->mode = SBC_MODE_STEREO;
            break;
        case BT_A2DP_CHANNEL_MODE_JOINT_STEREO:
            sbc->mode = SBC_MODE_JOINT_STEREO;
            break;
        default:
			fprintf (stderr, "No supported channel_mode");
    }

    switch (cap->allocation_method) {
        case BT_A2DP_ALLOCATION_SNR:
            sbc->allocation = SBC_AM_SNR;
            break;
        case BT_A2DP_ALLOCATION_LOUDNESS:
            sbc->allocation = SBC_AM_LOUDNESS;
            break;
        default:
			fprintf (stderr, "No supported allocation");
    }

    switch (cap->subbands) {
        case BT_A2DP_SUBBANDS_4:
            sbc->subbands = SBC_SB_4;
            break;
        case BT_A2DP_SUBBANDS_8:
            sbc->subbands = SBC_SB_8;
            break;
        default:
			fprintf (stderr, "No supported subbands");
    }

    switch (cap->block_length) {
        case BT_A2DP_BLOCK_LENGTH_4:
            sbc->blocks = SBC_BLK_4;
            break;
        case BT_A2DP_BLOCK_LENGTH_8:
            sbc->blocks = SBC_BLK_8;
            break;
        case BT_A2DP_BLOCK_LENGTH_12:
            sbc->blocks = SBC_BLK_12;
            break;
        case BT_A2DP_BLOCK_LENGTH_16:
            sbc->blocks = SBC_BLK_16;
            break;
        default:
			fprintf (stderr, "No supported block length");
    }

	sbc->bitpool = cap->max_bitpool;
}

//////////////////////////////// BLUEZ AUDIO CALLBACK HANDLER ////////////////////////////////


/*****************//**
 * Implement MediaEndpoint.SelectConfiguration.
 * Called by bluez to negotiate which configuration (=codec, codec parameter)
 * for audio streaming.
 * This function will examine what the requested configuration and returns back
 * a reply with the supported / agreed configuration.
 * 
 * Chosen configuration isn't cached because it will be returned with SetConfiguration.
 * 
 * Contains modified code taken from pulseaudio 2.1 (which took it from 
 * bluez audio, select_sbc_params (a2dp.c)
 * 
 * @param [in] original "call" message from bluez
 * @returns reply message (success or failure)
 *********************/
DBusMessage* endpoint_select_configuration (DBusMessage *msg) {
    a2dp_sbc_t *cap, config;
    uint8_t *pconf = (uint8_t *) &config;
    int size;
    DBusMessage *reply;
    DBusError err;

	debug_print ("Select configuration\n");
    dbus_error_init(&err);

    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
		handle_dbus_error (&err, __FUNCTION__, __LINE__);
		goto fail;
	}

	//taken from pulseaudio with modification
    memset(&config, 0, sizeof(config));
    config.frequency = BT_SBC_SAMPLING_FREQ_44100;
	if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_JOINT_STEREO)
		config.channel_mode = BT_A2DP_CHANNEL_MODE_JOINT_STEREO;
	else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_STEREO)
		config.channel_mode = BT_A2DP_CHANNEL_MODE_STEREO;
	else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL)
		config.channel_mode = BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL;
	else if (cap->channel_mode & BT_A2DP_CHANNEL_MODE_MONO) {
		config.channel_mode = BT_A2DP_CHANNEL_MODE_MONO;
	} else {
		fprintf (stderr, "No supported channel modes");
		goto fail;
	}

    if (cap->block_length & BT_A2DP_BLOCK_LENGTH_16)
        config.block_length = BT_A2DP_BLOCK_LENGTH_16;
    else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_12)
        config.block_length = BT_A2DP_BLOCK_LENGTH_12;
    else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_8)
        config.block_length = BT_A2DP_BLOCK_LENGTH_8;
    else if (cap->block_length & BT_A2DP_BLOCK_LENGTH_4)
        config.block_length = BT_A2DP_BLOCK_LENGTH_4;
    else {
        fprintf (stderr, "No supported block lengths");
        goto fail;
    }

    if (cap->subbands & BT_A2DP_SUBBANDS_8)
        config.subbands = BT_A2DP_SUBBANDS_8;
    else if (cap->subbands & BT_A2DP_SUBBANDS_4)
        config.subbands = BT_A2DP_SUBBANDS_4;
    else {
        fprintf (stderr, "No supported subbands");
        goto fail;
    }

    if (cap->allocation_method & BT_A2DP_ALLOCATION_LOUDNESS)
        config.allocation_method = BT_A2DP_ALLOCATION_LOUDNESS;
    else if (cap->allocation_method & BT_A2DP_ALLOCATION_SNR)
        config.allocation_method = BT_A2DP_ALLOCATION_SNR;

    config.min_bitpool = (uint8_t) MAX(MIN_BITPOOL, cap->min_bitpool);
    config.max_bitpool = (uint8_t) MIN(a2dp_default_bitpool(config.frequency, config.channel_mode), cap->max_bitpool);

	reply = dbus_message_new_method_return(msg);
    dbus_message_append_args (reply,
                             DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pconf, size,
                             DBUS_TYPE_INVALID);
    return reply;

fail:
    return dbus_message_new_error(msg, "org.bluez.MediaEndpoint.Error.InvalidArguments",
                                       "Unable to select configuration");
}

/*****************//**
 * Implement MediaEndpoint.SetConfiguration.
 * Called by bluez to confirm that this will be the configuration chosen.
 * The most important thing here is the "transport object path", which we will
 * need to get the actual file-descriptor for streaming later (see transport_acquire).
 * 
 * In theory the transport_acquire could be called from here with some delays,
 * in reality it is a lot better to do it after we've received confirmation that
 * audio is "connected" (this is done by watching AudioSource.PropertyChange signal)
 * 
 * This function is too complicated for what it does, for our (simple) purpose
 * we actually only need the transport_path so we don't have to parse all the parameters,
 * but we do anyway.
 * 
 * @param [in] original "call" message from bluez
 * @param [in] io thread hashtable
 * @returns reply message (success or failure)
 *********************/
DBusMessage* endpoint_set_configuration (DBusMessage *msg, io_thread_tcb_s **io_threads_table) {
	const char *transport_path, *dev_path = NULL, *uuid = NULL, *cmd_path = NULL;
    uint8_t *config = NULL;
    int size = 0;
    DBusMessageIter iter, iterprop, iterentry, itervalue, iterarray;
    io_thread_tcb_s *head = *io_threads_table;
    io_thread_tcb_s *io_data = NULL;

    dbus_message_iter_init(msg, &iter);
    dbus_message_iter_get_basic(&iter, &transport_path);
    if (!dbus_message_iter_next(&iter))
        goto fail;

    dbus_message_iter_recurse(&iter, &iterprop);
    if (dbus_message_iter_get_arg_type(&iterprop) != DBUS_TYPE_DICT_ENTRY)
        goto fail;

    /* Read transport properties */
    while (dbus_message_iter_get_arg_type(&iterprop) == DBUS_TYPE_DICT_ENTRY) {
        const char *key;
        int var;

        dbus_message_iter_recurse(&iterprop, &iterentry);
        dbus_message_iter_get_basic(&iterentry, &key);

        dbus_message_iter_next(&iterentry);
        dbus_message_iter_recurse(&iterentry, &itervalue);

        var = dbus_message_iter_get_arg_type(&itervalue);
        if (strcasecmp(key, "UUID") == 0) {
            if (var != DBUS_TYPE_STRING)
                goto fail;
            dbus_message_iter_get_basic(&itervalue, &uuid);
        } else if (strcasecmp(key, "Device") == 0) {
            if (var != DBUS_TYPE_OBJECT_PATH)
                goto fail;
            dbus_message_iter_get_basic(&itervalue, &dev_path);
        } else if (strcasecmp(key, "Configuration") == 0) {
            if (var != DBUS_TYPE_ARRAY)
                goto fail;
            dbus_message_iter_recurse(&itervalue, &iterarray);
            dbus_message_iter_get_fixed_array(&iterarray, &config, &size);
        }
        dbus_message_iter_next(&iterprop);
    }
    debug_print ("Set configuration %s\n",transport_path);

    //capture the transport_path and allocate the transport later, when the audiosource is "connected".
    HASH_FIND_STR (head, transport_path, io_data);
    if (!io_data) {
		io_data = create_io_thread();
		io_data->dev_path = strdup (dev_path);
		io_data->transport_path = strdup (transport_path);
		io_data->cap = *((a2dp_sbc_t*) config);
		
		//read or write
		cmd_path = (char *)dbus_message_get_path (msg);
		if ( strcasecmp (cmd_path, A2DP_SINK_ENDPOINT) == 0)
			io_data->write = 0;
		else if ( strcasecmp (cmd_path, A2DP_SOURCE_ENDPOINT) == 0)
			io_data->write = 1;
			
		HASH_ADD_KEYPTR (hh, head, io_data->transport_path, strlen(io_data->transport_path), io_data);
		*io_threads_table = head;
	}
	
    return dbus_message_new_method_return(msg);

fail:
	return dbus_message_new_error(msg, "org.bluez.MediaEndpoint.Error.InvalidArguments",
									   "Unable to select configuration");
}

/*****************//**
 * Implement MediaEndpoint.ClearConfiguration.
 * Called by bluez to let us know that the audio streaming process has been reset
 * for whatever reason, and we should do our own clean-up.
 * Here we tell our I/O thread to stop.
 * 
 * It is not necessary to call transport_release here because by the time we got here,
 * the 'transport' has already been released.
 * 
 * @param [in] original "call" message from bluez
 * @param [in] io thread's data - to command I/O thread to stop.
 * @returns reply message (success or failure)
 *********************/
DBusMessage* endpoint_clear_configuration (DBusMessage *msg, io_thread_tcb_s **io_threads_table) {
	DBusMessage *reply;
	DBusError err;
	DBusMessageIter iter;
	char *transport_path;
	io_thread_tcb_s *head = *io_threads_table;
	io_thread_tcb_s *io_data = NULL;

	dbus_error_init(&err);
    dbus_message_iter_init(msg, &iter);
    dbus_message_iter_get_basic(&iter, &transport_path);
	debug_print ("Clear configuration %s\n",transport_path);    
    
    // stop stream
    HASH_FIND_STR (head, transport_path, io_data);
    if (io_data) {
		debug_print ("stopping thread %p\n",io_data);
		HASH_DEL (head, io_data);
		*io_threads_table = head;
		destroy_io_thread (io_data); 
	}
	
	reply = dbus_message_new_method_return(msg);
	return reply;
}

/*****************//**
 * Implement MediaEndpoint.Release
 * Called by bluez to let us know our registration (see register_endpoint) has been
 * cancelled (or 'released'). The next logical action after this, is either:
 * a) to exit
 * b) to re-register.
 * 
 * There is no need to 'Unregister' because by the time we get here, our endpoint
 * has already been de-registered.
 * 
 * @param [in] original "call" message from bluez
 * @returns reply message (success or failure)
 *********************/
DBusMessage* endpoint_release (DBusMessage *msg) {
	debug_print ("Release endpoint\n");
	DBusMessage *reply;
	DBusError err;

	dbus_error_init(&err);
	reply = dbus_message_new_method_return(msg);
	return reply;
}



//////////////////////////////// BLUEZ-DBUS SIGNAL HANDLERS ////////////////////////////////

/*****************//**
 * Handle AudioSource.PropertyChanged
 * Signalled by bluez to let us know that the state of the audio source has changed.
 * We use this signal as a trigger for 'delayed' transport_acquire to get the file
 * descriptor, as well as to start the I/O thread.
 * 
 * We don't use the corresponding transport_release because for the time being
 * we will never willingly release a transport, until it is closed by 
 * MediaEndpoint.ClearConfiguration (the I/O thread will be stopped there).
 * 
 * Note: The 'source' and 'sink' terms used in bluez is super-confusing because
 * they are not consistent - sometimes they view it from the bluez side
 * (in this case, it's a 'source' because the bluez is the 'source' of the data,
 * sometimes they view it as 'sink' because this application receives and acts as data sink
 * for the remote-end.
 * 
 * Note: There is a corresponding signal for AudioSink, which we don't use.
 * 
 * @param [in] connection object to talk to DBus
 * @param [in] original "call" message from bluez
 * @param [in] write==0 -> audiosink, write==1 --> audiosource
 * @param [in] head of I/O thread hashtable
 * @returns reply message (success or failure)
 *********************/
 #define audiosink_property_changed audiosource_property_changed
void audiosource_property_changed (DBusConnection *conn, DBusMessage *msg, int write, io_thread_tcb_s **io_threads_table) {
	DBusMessageIter iter, itervariant;
	char *key;
	char *dev_path;
	char *state;
	io_thread_tcb_s *head = *io_threads_table;
	io_thread_tcb_s *io_data;
	int new_state, transition, when_to_acquire, when_to_release;
	
	dbus_message_iter_init (msg, &iter);
    dbus_message_iter_get_basic (&iter, &key);
    if (strcasecmp (key, "State") != 0) return; //we are only interested in this.
    
    if (!dbus_message_iter_next(&iter)) goto fail;
    dbus_message_iter_recurse(&iter, &itervariant);
    if (dbus_message_iter_get_arg_type(&itervariant) != DBUS_TYPE_STRING) goto fail;
    dbus_message_iter_get_basic (&itervariant, &state);
    
	dev_path = (char *)dbus_message_get_path (msg);
    debug_print ("state for %s: %s\n", dev_path, state);
  
	//look for our thread
	if (!head) return;
	io_data = head;
	do {
		if (strcasecmp (dev_path, io_data->dev_path) == 0 && 
			io_data->write == write) 
			break;
		else io_data = io_data->hh.next;
	} while (io_data && io_data != head);
	if (!io_data) return;
    
    //decode state & transition
    new_state = transition = -1;
    if ( strcasecmp (state, "connected") == 0 ) new_state = STATE_CONNECTED;
    else if ( strcasecmp (state, "playing") == 0 ) new_state = STATE_PLAYING;
    else if ( strcasecmp (state, "disconnected") == 0 ) new_state = STATE_DISCONNECTED;
    if (new_state >= 0) {
		transition = io_data->prev_state << 4 | new_state;
		io_data->prev_state = new_state;
	}
    
    //our treatment of sink and source is a bit different
    switch (write) {
		case 0: // bt sink: bt --> alsa
			when_to_acquire = STATE_CONNECTED << 4 | STATE_PLAYING;
			when_to_release = STATE_PLAYING << 4 | STATE_CONNECTED;
			break;
			
		case 1: // bt source: alsa --> bt
			when_to_acquire = STATE_DISCONNECTED << 4 | STATE_CONNECTED;
			when_to_release = STATE_CONNECTED << 4 | STATE_DISCONNECTED;		
			break;
	}
    
    //acquire or release transport depending on the transitions
    if (transition == when_to_acquire) {
		if (transport_acquire (conn, io_data->transport_path, &io_data->fd, &io_data->read_mtu, &io_data->write_mtu)) {
			debug_print ("fd: %d read mtu %d write mtu %d\n", io_data->fd, io_data->read_mtu, io_data->write_mtu);
			io_thread_set_command (io_data, IO_CMD_RUNNING);
		}
	} else if (transition == when_to_release) {
		transport_release (conn, io_data->transport_path);
		io_thread_set_command (io_data, IO_CMD_IDLE);
	}	
    return;
    
fail:
	debug_print ("bad signal\n");
}



//////////////////////////////// IO THREAD HELPERS ////////////////////////////////


/*****************//**
 * Send command to I/O thread in a thread-safe manner.
 * Once given, it will also trigger I/O thread to start running (if it is not
 * already is).
 * 
 * Note: Once IO_CMD_TERMINATE is issued, it cannot be cancelled.
 * 
 * @param [in] I/O thread control block
 * @param [in] command to send
 * @returns reply message (success or failure)
 *********************/
void io_thread_set_command (io_thread_tcb_s *data, int command) {
	pthread_mutex_lock (&data->mutex);
	debug_print ("io cmd: %d\n", command);
	if (data->command != IO_CMD_TERMINATE)
		data->command = command;
	pthread_mutex_unlock (&data->mutex);
	pthread_cond_signal (&data->cond);
}


/*****************//**
 * Create an I/O thread.
 * This will create the thread control block and the thread itself.
 * The created thread is already running but in suspended state.
 * 
 * @returns newly created thread control block
 *********************/
void *io_thread_run(void *ptr);
io_thread_tcb_s *create_io_thread() {
	io_thread_tcb_s *p;
	
	p = malloc (sizeof (io_thread_tcb_s));
	memset (p, 0, sizeof (io_thread_tcb_s));
	pthread_cond_init (&p->cond, NULL);
	pthread_mutex_init (&p->mutex, NULL);
	pthread_create (&p->t_handle, NULL, io_thread_run, p);
	return p;
}

/*****************//**
 * Destroy and existing an I/O thread.
 * This will stop the terminate the thread, wait until it is really terminated,
 * release the resources held by the thread, then free the tcb itself.
 * 
 * @param [in] thread control block
 *********************/
void destroy_io_thread(io_thread_tcb_s *p) {
	if (p) {
		io_thread_set_command (p, IO_CMD_TERMINATE);
		pthread_join (p->t_handle, NULL);
		pthread_cond_destroy (&p->cond);
		pthread_mutex_destroy (&p->mutex);		
		if (p->transport_path) {
			free (p->transport_path);
			p->transport_path = NULL;
		}
		if (p->dev_path) {
			free (p->dev_path);
			p->dev_path = NULL;
		}
		free (p);
	}
}


//////////////////////////////// IO THREAD ////////////////////////////////

/*****************//**
 * Read stdin, encode it, and write output to bluez stream output
 * Encoding function is taken from pulseaudio 2.1
 * 
 * @param [in] I/O thread control block
 * @param [in] run_once global variable
 * @param [out] quit global variable (if the app should quit)
 *********************/
void stream_bt_output(io_thread_tcb_s *data) {
	void *buf, *encode_buf;
	size_t bufsize, encode_bufsize;
	struct pollfd pollin = { 0, POLLIN, 0 }, pollout = { data->fd, POLLOUT, 0 };
	int timeout;

	debug_print ("write to bt\n");
	
	// get buffers
	encode_bufsize = data->write_mtu;
	encode_buf = malloc (encode_bufsize);
	bufsize = (encode_bufsize / sbc_get_frame_length (&data->sbc)) * // max frames allowed in a packet
	           sbc_get_codesize(&data->sbc); // ensure all of our source will fit in a single packet
	buf = malloc (bufsize);	
	//debug_print ("encode_buf %d buf %d", encode_bufsize, bufsize);
	    
	// stream
	while (data->command == IO_CMD_RUNNING) {
		ssize_t readlen;
		
		// wait until stdin has some data
		//debug_print ("waiting stdin\n");
		pthread_mutex_unlock (&data->mutex);
		timeout = poll (&pollin, 1, 1000); //delay 1s to allow others to update our state
		pthread_mutex_lock (&data->mutex);
		if (timeout == 0) continue;
		if (timeout < 0) {
			fprintf (stderr, "bt_write/stdin: %d\n", errno);
			break;
		}
		
		// read stdin
		readlen = read (0, buf, bufsize);
		if (readlen == 0) { // stream closed
			data->command = IO_CMD_TERMINATE;
			if (run_once) quit = 1;
			continue;
		}
		if (readlen < 0) continue;
		
		// encode bluetooth
		// all of our source is guaranteed to fit  in a single packet
		//debug_print ("encoding\n");
		
		struct rtp_header *header;
		struct rtp_payload *payload;
		size_t nbytes;

		header = encode_buf;
		payload = (struct rtp_payload*) ((uint8_t*) encode_buf + sizeof(*header));

		void *p = buf;
		void *d = encode_buf + sizeof(*header) + sizeof(*payload);
		size_t to_write = encode_bufsize - sizeof(*header) - sizeof(*payload);
		size_t to_encode = readlen;
		unsigned frame_count = 0;

		while (to_encode >= sbc_get_codesize(&data->sbc)) {
			//fprintf (stderr, "%zu ", to_encode);
			ssize_t written;
			ssize_t encoded;
			
			//debug_print ("%p %d %d\n", d, to_write, sbc_get_frame_length (&data->sbc));
			encoded = sbc_encode(&data->sbc,
								 p, to_encode,
								 d, to_write,
								 &written);

			if (encoded <= 0) {
				fprintf (stderr, "SBC encoding error %zd\n", encoded);
				break; // make do with what have
			}

			p = (uint8_t*) p + encoded;
			to_encode -= encoded;
			d = (uint8_t*) d + written;
			to_write -= written;

			frame_count++;			
		}

		// encapsulate it in a2dp RTP packets
		memset(encode_buf, 0, sizeof(*header) + sizeof(*payload));
		header->v = 2;
		header->pt = 1;
		header->sequence_number = htons(data->seq_num++);
		header->timestamp = htonl(data->timestamp);
		header->ssrc = htonl(1);
		payload->frame_count = frame_count;
		
		// next timestamp
		data->timestamp += sbc_get_frame_duration(&data->sbc) * frame_count;			

		// how much to output
		nbytes = (uint8_t*) d - (uint8_t*) encode_buf;

		//debug_print ("nbytes: %zu\n", nbytes);
		if (!nbytes) break; // don't write if there is nothing to write
	
		// wait until bluetooth is ready
		while (data->command == IO_CMD_RUNNING) {
			//debug_print ("waiting for bluetooth\n");
			pthread_mutex_unlock (&data->mutex);
			timeout = poll (&pollout, 1, 1000); //delay 1s to allow others to update our state
			pthread_mutex_lock (&data->mutex);
			if (timeout == 0) continue;
			if (timeout < 0) fprintf (stderr, "bt_write/bluetooth: %d\n", errno);
			break;
		}
		
		// write bluetooth
		if (timeout > 0) {
			//debug_print ("flush bluetooth\n");
			write (data->fd, encode_buf, nbytes);
		}
	}
	
	// cleanup		
	free (buf);
	free (encode_buf);
}

/*****************//**
 * Read bluez stream data input, decode it, and write output to stdout
 * Decoding function is taken from pulseaudio 2.1
 * 
 * @param [in] I/O thread control block
 *********************/
void stream_bt_input(io_thread_tcb_s *data) {
	void *buf, *decode_buf;
	size_t bufsize, decode_bufsize;
	struct pollfd pollin = { data->fd, POLLIN, 0 }, pollout = { 1, POLLOUT, 0 };
	int timeout;
	
	debug_print ("read from bt\n");
	
	// get buffers
	bufsize = data->read_mtu;
	buf = malloc (bufsize);
	decode_bufsize = (bufsize / sbc_get_frame_length (&data->sbc) + 1 ) * //max frames in a packet
	                 sbc_get_codesize(&data->sbc);
	decode_buf = malloc (decode_bufsize);
	//debug_print ("codesize %d framelen %d", sbc_get_codesize(&data->sbc), sbc_get_frame_length(&data->sbc));
	
	// stream
	while (data->command == IO_CMD_RUNNING) {
		ssize_t readlen=0;
		
		// wait until bluetooth has some data
		//debug_print ("waiting bluetooth\n");
		pthread_mutex_unlock (&data->mutex);
		timeout = poll (&pollin, 1, 1000); //delay 1s to allow others to update our state
		pthread_mutex_lock (&data->mutex);
		if (timeout == 0) continue;
		if (timeout < 0) {
			fprintf (stderr, "bt_read/bluetooth: %d\n", errno);
			break;
		}
		
		// read bluetooth
		readlen = read (data->fd, buf, bufsize);
		if (readlen == 0) { // stream closed
			data->command = IO_CMD_TERMINATE; 
			continue;
		}
		if (readlen < 0) continue;
		
		// decode a2dp/sbc from pulseaudio
		//debug_print ("decoding\n");
		void *p = buf + sizeof(struct rtp_header) + sizeof(struct rtp_payload);
		size_t to_decode = readlen - sizeof(struct rtp_header) - sizeof(struct rtp_payload);
		void *d = decode_buf;
		size_t to_write = decode_bufsize;
		
		while (to_decode > 0) {
			size_t written;
			size_t decoded;

			decoded = sbc_decode(&data->sbc,
								 p, to_decode,
								 d, to_write,
								 &written);

			if (decoded <= 0) {
				fprintf (stderr, "SBC decoding error %zd\n", decoded);
				break; // make do with what we have
			}

			p = (uint8_t*) p + decoded;
			to_decode -= decoded;
			d = (uint8_t*) d + written;
			to_write -= written;
			//debug_print ("%zu ", decode_bufsize - to_write);
		}
		// debug_print ("\n");
		
		// wait until stdout is ready
		while (data->command == IO_CMD_RUNNING) {
			//debug_print ("waiting for stdout\n");
			pthread_mutex_unlock (&data->mutex);
			timeout = poll (&pollout, 1, 1000); //delay 1s to allow others to update our state
			pthread_mutex_lock (&data->mutex);
			if (timeout == 0) continue;
			else if (timeout < 0) fprintf (stderr, "bt_read/stdout: %d\n", errno);
			break;
		}
		
		// write stdout
		if (timeout > 0) {
			//debug_print ("flushing stdout\n");
			write (1, decode_buf, decode_bufsize - to_write);
		}
	}
	
	// cleanup
	free (buf); 
	free (decode_buf);
}

/*****************//**
 * Main I/O thread function
 * This function will perform actual streaming of data.
 * It is simple: read the data from bluez' descriptor, and write to stdout!
 * 
 * The rest is just management overhead.
 * 
 * In output mode, the output (on stdout) will raw S16_LE, 44.1kHz, stereo/mono (depends on source).
 * See "endpoint_select_configuration" if you want to change this.
 * aplay -f cd can play this correctly
 * 
 * In input mode, the input (on stdin) must be raw S16_LE, 48kHz, stereo/mono (depends on target).
 * See "endpoint_select_configuration" if you want to change this.
 * The encoding function is taken from pulseaudio 2.1 (simplified).
 * 
 * @param [in] control-block for this thread.
 * @returns NULL
 *********************/
void *io_thread_run(void *ptr) {
	io_thread_tcb_s *data = ptr;
	
	// prepare
	debug_print ("starting %p\n", ptr);
	pthread_mutex_lock (&data->mutex);
	sbc_init (&data->sbc, 0);
	
	// run
	while (1) {
		switch (data->command) {
			case IO_CMD_IDLE:
				pthread_cond_wait (&data->cond, &data->mutex);
				break;
				
			case IO_CMD_RUNNING:
				debug_print ("running %p\n", ptr);
				setup_sbc (&data->sbc, &data->cap);
				if (data->write) 
					stream_bt_output(data);
				else 
					stream_bt_input(data);
				break;

			case IO_CMD_TERMINATE:
				debug_print ("terminate %p\n", ptr);
				goto end;
		}
		
	}
	
end:
	// cleanup
	sbc_finish(&data->sbc);
	pthread_mutex_unlock (&data->mutex);
	return NULL;
}


//////////////////////////////// MAIN ////////////////////////////////

int main(int argc, char** argv)
{
	// Control variables
	DBusConnection* system_bus;
	char *bt_object;	// bluetooth device objectpath
	io_thread_tcb_s *io_threads_table = NULL; // hashtable of io_threads
	int msg_waiting_time = -1; //default is wait forever
	
	// scratch variables
	DBusMessage *msg, *reply;
	DBusError err;
		
	// 0. check options
	const struct option options[] = {
		{"help",    no_argument, 0,  'h' },
		{"sink",    no_argument, 0,  's' },
		{"source",  no_argument, 0,  'o' },
		{"run-once", no_argument, 0, 'r' },
		{0,         0,           0,  0 }
	};
	int option, run_sink=0, run_source=0;
	while ((option = getopt_long (argc, argv, "h", (const struct option *) &options, NULL)) != -1) {
		switch (option) {
			case 'h':
				fprintf (stderr,"Usage: a2dp-alsa [--sink|--source|--run-once] [hciX]\n");
				return 0;
			case 's':
				debug_print ("run sink\n");
				run_sink=1;
				break;
			case 'o':
				debug_print ("run source\n");
				run_source=1;
				break;
			case 'r':
				debug_print ("run once\n");
				run_once=1;
				msg_waiting_time = 1000; //1000 ms = 1 s
				break;
		}
	}
	
	// 1. init - get bus and adapter
	debug_print ("a2dp started\n");
	dbus_threads_init_default();
	dbus_error_init(&err);
	if (get_system_bus(&system_bus)) {
		if (!get_bluetooth_object(system_bus, argv[optind], &bt_object)) return 1;
	} else return 1;

	// 2. register endpoint
	if (run_sink) 
		run_sink = media_register_endpoint(system_bus, bt_object, A2DP_SINK_ENDPOINT, A2DP_SINK_UUID); // bt --> alsa
	if (run_source)
		run_source = media_register_endpoint(system_bus, bt_object, A2DP_SOURCE_ENDPOINT, A2DP_SOURCE_UUID); // alsa --> bt
	if (!run_source && !run_sink) return 1;
	
	// 3. capture signals
	if (run_sink) { // bt --> alsa
		dbus_bus_add_match (system_bus, "type='signal',interface='org.bluez.AudioSource',member='PropertyChanged'", &err);
		handle_dbus_error (&err, __FUNCTION__, __LINE__);
	}
	if (run_source) { // alsa --> bt
		dbus_bus_add_match (system_bus, "type='signal',interface='org.bluez.AudioSink',member='PropertyChanged'", &err);
		handle_dbus_error (&err, __FUNCTION__, __LINE__);
	}

	// 4. main-loop
	while (!quit && dbus_connection_read_write (system_bus, msg_waiting_time)) { //block
		while (!quit && (msg = dbus_connection_pop_message (system_bus))) { //get the message
			// dispatch
			reply = NULL;
			if (dbus_message_is_signal (msg, "org.bluez.AudioSource", "PropertyChanged")) // bt --> alsa
				audiosource_property_changed (system_bus, msg, 0, &io_threads_table);
			else if (dbus_message_is_signal (msg, "org.bluez.AudioSink", "PropertyChanged")) // alsa --> bt
				audiosink_property_changed (system_bus, msg, 1, &io_threads_table);
			else if (dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint", "SetConfiguration"))
				reply = endpoint_set_configuration (msg, &io_threads_table);
			else if (dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint", "SelectConfiguration"))
				reply = endpoint_select_configuration (msg);
			else if(dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint", "ClearConfiguration"))
				reply = endpoint_clear_configuration (msg, &io_threads_table);
			else if (dbus_message_is_method_call (msg, "org.bluez.MediaEndpoint", "Release"))
				{ reply = endpoint_release (msg); quit=1; }
			if (reply) {
				// send the reply
				dbus_connection_send(system_bus, reply, NULL);
				dbus_message_unref(reply);
			}
			dbus_message_unref(msg);
		}
	}

	// 5. destroy all existing I/O threads
	if (io_threads_table) {
		io_thread_tcb_s *p = io_threads_table, *next;
		do {
			next = p->hh.next;
			destroy_io_thread (p);
			p = next;
		} while (p && p != io_threads_table);
	}
	
	// 6. cleanup and exit
	dbus_connection_flush (system_bus);
	dbus_connection_unref (system_bus);
	debug_print ("a2dp ended\n\n");
	return 0;
}
