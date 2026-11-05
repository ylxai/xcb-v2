# Build semua komponen userland di lab ini
CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra
LDFLAGS :=

all: ptrace-hook seccomp-notif victim preload-hide.so modecheck

ptrace-hook: ptrace-hook.c
	$(CC) $(CFLAGS) -o $@ $<

seccomp-notif: seccomp-notif.c
	$(CC) $(CFLAGS) -o $@ $<

victim: victim.c
	$(CC) $(CFLAGS) -o $@ $<

preload-hide.so: preload-hide.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

# Pembantu uji K1: buat file O_CREAT 0700 lewat library preload.
modecheck: modecheck.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ptrace-hook seccomp-notif victim preload-hide.so modecheck

.PHONY: all clean
