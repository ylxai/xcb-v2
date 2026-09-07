#!/usr/bin/env bash
# test.sh — jalankan semua demo hooking + verifikasi hasil.
set -uo pipefail
cd "$(dirname "$0")"

echo "======================================================"
echo "DEMO 1: ptrace interceptor (block openat + anti-kill)"
echo "======================================================"
./ptrace-hook --block-path rahasia --anti-kill -- ./victim
echo
echo "======================================================"
echo "DEMO 2: seccomp user notification (sandbox supervisor)"
echo "======================================================"
./seccomp-notif -- ./victim
echo
echo "======================================================"
echo "DEMO 3: LD_PRELOAD sembunyikan file dari ls"
echo "======================================================"
rm -f /tmp/rahasia.txt
echo "isi rahasia" > /tmp/rahasia.txt
echo "--- tanpa preload (file terlihat):"
ls /tmp | grep rahasia && echo "  -> file TAMPAK"
echo "--- dengan LD_PRELOAD (file hilang):"
LD_PRELOAD="$PWD/preload-hide.so" ls /tmp | grep rahasia \
    && echo "  -> file MASIH TAMPAK (gagal)" \
    || echo "  -> file SEMBUNYI (ls tidak melihatnya)"
echo "--- akses langsung dengan preload (cat harus gagal):"
LD_PRELOAD="$PWD/preload-hide.so" cat /tmp/rahasia.txt \
    && echo "  -> terbaca (gagal)" || echo "  -> ENOENT, file tidak bisa dibaca"
echo
echo "SELESAI. Catatan: kmod/ butuh kernel headers, tidak diuji di devbox."
