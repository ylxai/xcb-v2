# Lab: process hiding userland (LD_PRELOAD)

Sembunyikan entry `/proc/<pid>` dari tool yang membaca direktori /proc
dengan meng-interposisi `readdir()` dan `getdents64()` di libc.

## Build

    make          # -> hideproc.so

## Cara pakai

Sembunyikan diri sendiri (proses yang memuat library):

    LD_PRELOAD=$PWD/hideproc.so ./xcb --benchmark 200000

Sembunyikan proses LAIN dari tool inspeksi (tool juga harus di-preload):

    # xcb jalan normal (PID 1234)
    HIDE_PID=1234 LD_PRELOAD=$PWD/hideproc.so ps aux | grep xcb   # kosong
    HIDE_PID=1234 LD_PRELOAD=$PWD/hideproc.so pgrep -f xcb        # kosong
    HIDE_PID=1234 LD_PRELOAD=$PWD/hideproc.so ls /proc | grep 1234  # kosong

## Verifikasi

    # Sebelum (tanpa preload di tool inspeksi):
    ps aux | grep xcb          # TERLIHAT
    # Sesudah (tool ikut preload):
    HIDE_PID=<pid> LD_PRELOAD=$PWD/hideproc.so ps aux | grep xcb  # hilang
    kill -0 <pid>              # proses tetap hidup & bisa dihentikan

## Batas teknik ini (jujur)

1. **Userland**: kernel tetap tahu proses ada. `/proc/<pid>` tetap eksis;
   akses langsung (`kill <pid>`, `cat /proc/<pid>/stat`) tetap jalan.
2. **Per-proses**: hanya tool yang dijalankan dengan `LD_PRELOAD` yang
   buta. `ps` polos (tanpa preload) tetap melihat proses.
3. **Terlihat di environ**: `LD_PRELOAD` itu sendiri bisa terlihat via
   `/proc/<pid>/environ` oleh user lain — di devbox/container biasanya
   tidak (dumpable=0 atau same-uid).
4. Hiding penuh (global, tidak tergantung preload) butuh level kernel:
   kprobe/LKM hook `getdents64` (lihat `../syscall-hook/kprobe-tracefs.sh`)
   atau PID namespace — dan itu di luar scope lab ini.

## Catatan etis

Teknik ini untuk studi proteksi/anti-tamper di infrastruktur sendiri.
Menyembunyikan proses mining dari mesin yang bukan milikmu (VPS sewa
tanpa izin, cloud gratis) adalah penyalahgunaan resource dan melanggar
ToS penyedia.
