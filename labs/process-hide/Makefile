CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?= -shared
LDLIBS  ?= -ldl

all: hideproc.so

hideproc.so: hideproc.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f hideproc.so

.PHONY: all clean
