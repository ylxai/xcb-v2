#!/usr/bin/env bash
# vps-setup.sh — siapkan VPS fresh (Ubuntu/Debian) dari nol sampai siap build.
# Dijalankan DI VPS, bukan di lokal.
#
#   ./vps-setup.sh              full setup: deps -> submodule -> hugepages -> build miner -> build rootkit
#   ./vps-setup.sh --deps-only  hanya install dependencies
#   ./vps-setup.sh --no-build   deps + submodule + hugepages, tanpa compile
#
# Idempotent: aman dijalankan ulang, yang sudah ada dilewati.
set -u
cd "$(dirname "$0")"

SUDO=""
[ "$(id -u)" = "0" ] || { command -v sudo >/dev/null && SUDO="sudo"; }

MODE="full"
for a in "$@"; do case "$a" in
  --deps-only) MODE="deps";;
  --no-build)  MODE="nobuild";;
  -h|--help) sed -n '2,9p' "$0"; exit 0;;
  *) echo "flag tak dikenal: $a"; exit 1;;
esac; done

echo "=== [1/5] dependencies ==="
if command -v apt-get >/dev/null; then
  $SUDO apt-get update -qq
  $SUDO apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ca-certificates \
    libssl-dev "linux-headers-$(uname -r)"
else
  echo "bukan apt-based — install manual: gcc/g++, cmake, git, openssl-dev, kernel-headers"; exit 1
fi
for c in cmake g++ git pkg-config; do
  command -v "$c" >/dev/null && echo "  OK $c" || { echo "  GAGAL: $c hilang"; exit 1; }
done
[ -d "/lib/modules/$(uname -r)/build" ] && echo "  OK kernel headers $(uname -r)" \
  || { echo "  GAGAL: headers $(uname -r) — reboot dulu bila kernel baru diinstall?"; exit 1; }
[ "$MODE" = "deps" ] && { echo "deps-only: selesai"; exit 0; }

echo "=== [2/5] submodules ==="
git submodule update --init --recursive 2>/dev/null || echo "  (bukan git repo / offline — lewati)"

echo "=== [3/5] hugepages (butuh root) ==="
if [ -n "$SUDO" ] || [ "$(id -u)" = "0" ]; then
  $SUDO sysctl -w vm.nr_hugepages=1280 vm.max_map_count=262144
  grep -H . /proc/sys/vm/nr_hugepages 2>/dev/null || true
else
  echo "  lewati (tanpa root): sudo sysctl -w vm.nr_hugepages=1280 vm.max_map_count=262144"
fi

[ "$MODE" = "nobuild" ] && { echo "selesai (tanpa build)"; exit 0; }

echo "=== [4/5] build miner ==="
./build.sh --no-test && ./build/xcb --selftest | tail -n 1

echo "=== [5/5] build rootkit ==="
make -C "labs/rootkit" 2>&1 | grep -E "error|LD \[M\]" || true

echo; echo "SELESAI. Uji cepat:"
echo "  ./build/xcb --benchmark 20000 --ui=log -t 2"
echo "  cd labs/rootkit && ./rk.sh load && ./rk.sh test && ./rk.sh unload"
