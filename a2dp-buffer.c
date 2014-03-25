#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#define print(fileno, str) (write(fileno, str, strlen(str)))
#define perr(str) { print(2, str); exit(1); }

#define BUFSIZE 4*1024  // size of read buf
#define MINREAD 2*1024  // min buf we have to read before we can dump output
int main (int argc, char *argv[]) {
	int fd;
	void *buf = NULL;
	size_t bufsize = BUFSIZE;
	
	if (argc < 2) perr("Usage: a2dp-buffer /path/to/pipe\n");
	buf = malloc (bufsize);
	if (!buf) exit(1);
	
	fd = 0;
	while (fd != -1) {
		fd = open (argv[1], O_RDONLY);
		
		ssize_t readlen = 1;
		while (readlen > 0) {
			int total_read = 0;
			size_t to_read = BUFSIZE;
			char *p = (char *)buf;
			
			while (total_read < MINREAD) {
				readlen = read (fd, p, to_read);
				if (readlen <=0) 
					break;
				p = p + readlen;
				to_read -= readlen;
				total_read += readlen;
			}
			if (total_read > 0) 
				write (1, buf, total_read);
		}
		
		close (fd);
	}
}


