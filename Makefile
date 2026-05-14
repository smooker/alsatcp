CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lasound -lpthread

.PHONY: all clean

all: alsatcp

alsatcp: alsatcp.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f alsatcp
