# Makefile untuk LKM demo (jalankan di mesin dengan kernel headers)
#
#   make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
#   sudo insmod hook_syscall.ko hide_mark=rahasia target_pid=<pid>
#   sudo rmmod hook_syscall
#
# Catatan: kernel >= 5.7 tidak mengekspor kallsyms_lookup_name; modul ini
# memakai trik kprobe untuk mendapatkannya (lihat resolve_syscall_table).

obj-m += hook_syscall.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
