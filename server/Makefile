CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror

LDFLAGS += $(TARGET_LDFLAGS)

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CFLAGS) aesdsocket.c -o aesdsocket $(LDFLAGS)

clean:
	rm -f aesdsocket *.o