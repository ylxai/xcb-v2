#!/usr/bin/env bash
# mine.sh — auto cek CPU/RAM, set hugepages (sysctl + mlock), lalu jalankan miner.
#
#   ./mine.sh                  full dataset, threads = jumlah core
#   ./mine.sh --light          dataset kecil (~256MB, hemat RAM)
#   ./mine.sh -t 16            override jumlah thread
#   ./mine.sh --persist        tulis /etc/sysctl.d agar hugepages tetap setelah reboot
#   ./mine.sh --benchmark 100000    arg lain diteruskan ke miner (--benchmark, --selftest, ...)
#
# Idempotent: sysctl/setcap hanya dijalankan kalau memang belum cukup.
# Butuh root/sudo HANYA untuk sysctl & setcap; miner tetap jalan sebagai user kamu.
set -uo pipefail
cd "$(dirname "$0")"

BIN="./build/miner-saya"
[ -x "$BIN" ] || { echo "[setup] Binary tidak ada: $BIN — build dulu (lihat README)."; exit 1; }

PAGESZ_KB=$(awk '/Hugepagesize/{print $2}' /proc/meminfo)   # biasanya 2048 KB (2MB)
[ -z "$PAGESZ_KB" ] && PAGESZ_KB=2048

# ---------- 1. Deteksi CPU & RAM ----------
CORES=$(nproc)
MEM_TOTAL_MB=$(free -m | awk '/^Mem:/{print $2}')
MEM_AVAIL_MB=$(free -m | awk '/^Mem:/{print $7}')
echo "[setup] CPU: $CORES core | RAM: ${MEM_TOTAL_MB}MB total / ${MEM_AVAIL_MB}MB available"

# ---------- 2. Mode: full / light ----------
LIGHT=0
PERSIST=0
EXTRA=()
for a in "$@"; do
  case "$a" in
    --light)  LIGHT=1 ;;
    --persist) PERSIST=1 ;;
    *) EXTRA+=("$a") ;;
  esac
done

if [ "$LIGHT" = 1 ]; then
  NEED_PAGES=$(( (256 + 512) * 1024 / PAGESZ_KB ))   # dataset light ~256MB + buffer
  echo "[setup] Mode: LIGHT (dataset ~256MB)"
  # --light harus ikut ke miner. Tanpa ini skrip cuma menyetel hugepages untuk
  # light lalu menjalankan miner dalam mode full — kebalikan dari yang diminta.
  EXTRA+=(--light)
else
  NEED_PAGES=$(( (2560 + 640) * 1024 / PAGESZ_KB ))  # dataset full ~2.2GiB + cache 256MB + buffer
  echo "[setup] Mode: FULL (dataset ~2.5GiB)"
  if [ "$MEM_AVAIL_MB" -lt 3500 ]; then
    echo "[setup] ⚠ RAM available < 3.5GB — full mode berisiko. Pakai: ./mine.sh --light"
  fi
fi

# Jangan minta lebih dari RAM yang tersedia (sisakan ~1GB untuk OS).
MAX_PAGES=$(( (MEM_AVAIL_MB - 1024) * 1024 / PAGESZ_KB ))
[ "$NEED_PAGES" -gt "$MAX_PAGES" ] && NEED_PAGES=$MAX_PAGES
[ "$NEED_PAGES" -lt 1 ] && NEED_PAGES=1

# ---------- 3. sysctl: vm.nr_hugepages + vm.max_map_count ----------
HP_TOTAL=$(awk '/HugePages_Total/{print $2}' /proc/meminfo)
echo "[setup] HugePages saat ini: ${HP_TOTAL} halaman | dibutuhkan: >= ${NEED_PAGES}"

SUDO=""
if [ "$(id -u)" != "0" ] && command -v sudo >/dev/null; then SUDO="sudo"; fi

if [ "$HP_TOTAL" -lt "$NEED_PAGES" ]; then
  if [ -n "$SUDO" ] || [ "$(id -u)" = "0" ]; then
    echo "[setup] Set vm.nr_hugepages=$NEED_PAGES (${NEED_PAGES}x${PAGESZ_KB}KB)..."
    $SUDO sysctl -w vm.nr_hugepages="$NEED_PAGES" vm.max_map_count=262144
  else
    echo "[setup] ❌ Tidak root & tanpa sudo — jalankan sekali:"
    echo "        sudo sysctl -w vm.nr_hugepages=$NEED_PAGES vm.max_map_count=262144"
    echo "        lalu ulangi: ./mine.sh"
  fi
  HP_TOTAL=$(awk '/HugePages_Total/{print $2}' /proc/meminfo)
  echo "[setup] HugePages setelah set: ${HP_TOTAL} halaman"
else
  echo "[setup] HugePages sudah cukup — sysctl dilewati."
fi

# ---------- 4. Persist (opsional): tahan reboot ----------
if [ "$PERSIST" = 1 ] && { [ -n "$SUDO" ] || [ "$(id -u)" = "0" ]; }; then
  CONF="/etc/sysctl.d/99-miner-hugepages.conf"
  echo "[setup] Tulis $CONF (hugepages permanen setelah reboot)..."
  $SUDO sh -c "echo 'vm.nr_hugepages=$NEED_PAGES' > '$CONF' && echo 'vm.max_map_count=262144' >> '$CONF'"
fi

# ---------- 5. Izin mlock (wajib agar LARGE_PAGES bisa dipakai) ----------
NEED_KB=$(( NEED_PAGES * PAGESZ_KB ))
MLOCK_OK=0

if [ "$(id -u)" = "0" ]; then
  ulimit -l unlimited 2>/dev/null
  MLOCK_OK=1
  # setcap supaya run berikutnya (non-root) tetap bisa mlock.
  # Hanya kalau CAP_IPC_LOCK ada di bounding set: di container/sandbox yang
  # men-drop cap itu, binary bercap justru gagal exec dengan EPERM (exit 126).
  if capsh --has-p=cap_ipc_lock 2>/dev/null; then
    setcap cap_ipc_lock=+ep "$BIN" 2>/dev/null && echo "[setup] setcap cap_ipc_lock=+ep $BIN OK"
  else
    echo "[setup] CAP_IPC_LOCK tidak ada di bounding set — setcap dilewati"
    echo "        (binary bercap tidak bisa di-exec di sini; miner tetap jalan tanpa hugepages)"
  fi
elif [ -n "$SUDO" ]; then
  # coba pasang cap sekali via sudo (butuh password 1x)
  if ! getcap "$BIN" 2>/dev/null | grep -q cap_ipc_lock; then
    if capsh --has-p=cap_ipc_lock 2>/dev/null; then
      $SUDO setcap cap_ipc_lock=+ep "$BIN" 2>/dev/null
    else
      echo "[setup] CAP_IPC_LOCK tidak tersedia di lingkungan ini — setcap dilewati"
    fi
  fi
fi

ULIMIT_KB=$(ulimit -l)
if getcap "$BIN" 2>/dev/null | grep -q cap_ipc_lock || [ "${ULIMIT_KB:-0}" = "unlimited" ] || [ "${ULIMIT_KB:-0}" -ge "$NEED_KB" ] 2>/dev/null; then
  MLOCK_OK=1
fi

# ---------- 6. Putuskan LARGE_PAGES ----------
# Miner sudah bisa auto-detect sendiri (baca HugePages_Free), tapi di sini kita
# set eksplisit karena skrip ini punya info yang tidak bisa dilihat miner:
# hasil sysctl yang baru saja dijalankan + status izin mlock.
export LARGE_PAGES=0
if [ "$HP_TOTAL" -ge "$NEED_PAGES" ] && [ "$MLOCK_OK" = 1 ]; then
  export LARGE_PAGES=1
  echo "[setup] ✅ HugePages ($HP_TOTAL) + mlock OK → LARGE_PAGES=1"
else
  echo "[setup] ⚠ HugePages tidak dapat dipakai (halaman=$HP_TOTAL, mlock=$MLOCK_OK)"
  echo "        → LARGE_PAGES=0 (memori biasa, performa -2-5%)."
  echo "        Fix: sudo ./mine.sh --persist  (set sysctl + cap sekali, lalu jalan lagi)"
fi

# ---------- 7. Jalankan miner (semua arg diteruskan) ----------
echo "[setup] Jalankan: $BIN ${EXTRA[*]}  (threads auto = $CORES)"
echo
exec env LARGE_PAGES="$LARGE_PAGES" "$BIN" "${EXTRA[@]}"
