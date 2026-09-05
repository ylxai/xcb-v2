# Lab: Syscall Hooking (edukasi)

Implementasi nyata syscall hooking, dari teknik klasik sampai modern.
Semua berjalan di userland KECUALI `kmod/` yang butuh kernel headers.

## Isi

| File | Teknik | Lapisan | Butuh root? |
|------|--------|---------|-------------|
| `ptrace-hook.c` | PTRACE_SYSCALL interceptor | userland -> kernel (ptrace) | tidak |
| `seccomp-notif.c` | SECCOMP user notification | kernel (filter BPF) -> userland supervisor | tidak |
| `preload-hide.c` | LD_PRELOAD interposisi libc | userland | tidak |
| `victim.c` | program korban untuk demo | - | - |
| `kprobe-tracefs.sh` | kprobe via tracefs (hook `do_sys_openat2`) | kernel (resmi, tanpa LKM) | YA (CAP_SYS_ADMIN) |
| `kmod/hook_syscall.c` | timpa `sys_call_table` (CR0.WP) | kernel (LKM) | YA + kernel headers |

## Build

    make

## Demo 1 — ptrace interceptor (block openat + anti-kill)

    ./ptrace-hook --block-path rahasia --anti-kill -- ./victim

## Demo 2 — seccomp user notification (sandbox supervisor)

    ./seccomp-notif -- ./victim

## Demo 3 — LD_PRELOAD: sembunyikan file dari ls

    touch /tmp/rahasia.txt
    ls /tmp | grep rahasia                # muncul
    LD_PRELOAD=$PWD/preload-hide.so ls /tmp | grep rahasia   # hilang

## Demo 4 — kprobe tracefs: hook syscall di kernel TANPA modul

    sh kprobe-tracefs.sh "ls /etc"

Kernel-level hook `do_sys_openat2` via tracefs/kprobe — mekanisme resmi yang
dipakai eBPF/LTTng/perf. Bekerja di Namespace machine (terbukti di
4u0e8o29rdoma, kernel 7.1.4) karena TIDAK butuh kernel headers, gcc, atau
/lib/modules — hanya CAP_SYS_ADMIN + tracefs ter-mount.

## Demo 5 — LKM kernel (butuh distro dengan linux-headers)

    cd kmod && make
    sudo insmod hook_syscall.ko hide_mark=rahasia
    # ... uji: ls, cat file rahasia, kill ...
    sudo rmmod hook_syscall

## Peringatan

- `kmod/` TIDAK bisa dikompilasi di Namespace (kernel custom tanpa headers,
  /lib/modules kosong, tidak ada gcc/make — terverifikasi di devbox dan nsc
  machine 4u0e8o29rdoma). Jalankan di distro normal dengan
  `linux-headers-$(uname -r)`.
- Alternatif kernel-level yang BISA jalan di Namespace: kprobe tracefs
  (`kprobe-tracefs.sh`) atau eBPF (butuh bpftool/clang, umumnya tersedia di
  distro normal dengan kernel >= 5.x).
- Menimpa `sys_call_table` sangat kentara (CR0.WP toggle, rodata patch).
  Deteksi: LKRG, tripwire kernel, integrity checker memory vs vmlinuz.
- Materi ini untuk studi keamanan. Jangan dipakai di sistem yang bukan milikmu.
