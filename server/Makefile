CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CFLAGS) aesdsocket.c -o aesdsocket

clean:
	rm -f aesdsocket *.o