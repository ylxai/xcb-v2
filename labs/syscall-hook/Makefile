# Build semua komponen userland di lab ini
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra
LDFLAGS :=

all: ptrace-hook seccomp-notif victim preload-hide.so

ptrace-hook: ptrace-hook.c
	$(CC) $(CFLAGS) -o $@ $<

seccomp-notif: seccomp-notif.c
	$(CC) $(CFLAGS) -o $@ $<

victim: victim.c
	$(CC) $(CFLAGS) -o $@ $<

preload-hide.so: preload-hide.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

clean:
	rm -f ptrace-hook seccomp-notif victim preload-hide.so

.PHONY: all clean
