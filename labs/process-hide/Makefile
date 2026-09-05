CC      ?= cc
CFLAGS  ?= -O2 -Wall -fPIC -shared -ldl

all: hideproc.so

hideproc.so: hideproc.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f hideproc.so

.PHONY: all clean
