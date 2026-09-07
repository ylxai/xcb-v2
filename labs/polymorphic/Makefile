CC      ?= cc
OBJCOPY ?= objcopy

all: payload.bin

# payload.c -> obj -> ekstrak .text jadi raw shellcode
payload.bin: payload.c
	$(CC) -nostdlib -fPIC -c $< -o payload.o
	$(OBJCOPY) -O binary -j .text payload.o $@

clean:
	rm -f payload.o payload.bin

.PHONY: all clean
