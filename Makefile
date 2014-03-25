CFLAGS := $(CFLAGS) -O0 -g -DDEBUG -Wall
LDFLAGS := $(LDFLAGS)

all: a2dp-alsa libsbc.a a2dp-buffer

clean:
	-rm -f a2dp-alsa a2dp-buffer
	-rm -f sbc/*.a sbc/*.o

libsbc.a: sbc/libsbc.a

sbc/libsbc.a: sbc/sbc.o sbc/sbc_primitives_armv6.o sbc/sbc_primitives.o \
              sbc/sbc_primitives_iwmmxt.o sbc/sbc_primitives_mmx.o \
              sbc/sbc_primitives_neon.o
	ar rcvs $@ sbc/sbc.o sbc/sbc_primitives_armv6.o sbc/sbc_primitives.o \
              sbc/sbc_primitives_iwmmxt.o sbc/sbc_primitives_mmx.o \
              sbc/sbc_primitives_neon.o
              
%.o: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -c -o $@ $<

a2dp-alsa: a2dp-alsa.c sbc/libsbc.a
	$(CC) $(shell pkg-config --cflags dbus-1) $(CFLAGS) $(shell pkg-config --libs dbus-1) $(LDFLAGS) -o $@ $< sbc/libsbc.a

a2dp-buffer: a2dp-buffer.c
	$(CC) -o $@ $<

.PHONY: all clean libsbc.a
