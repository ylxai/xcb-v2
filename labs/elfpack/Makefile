CC ?= cc

all: hello_static hello_dyn

# ET_EXEC (non-PIE, static) — untuk mode in-memory loader
hello_static: hello_static.c
	$(CC) -static -o $@ $<

# ET_DYN (PIE, dynamic) — untuk mode temp+execve
hello_dyn: hello_dyn.c
	$(CC) -o $@ $<

clean:
	rm -f hello_static hello_dyn packed_* variant_* xcb_packed

.PHONY: all clean
