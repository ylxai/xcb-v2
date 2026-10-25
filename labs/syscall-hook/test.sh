#!/usr/bin/env bash
# test.sh — jalankan semua demo hooking + verifikasi hasil (assertion nyata).
#
# Test ini gagal (exit non-zero) bila hook tidak berperilaku sesuai klaim,
# bukan sekadar mencetak output. Semua demo dijalankan tanpa root.
set -uo pipefail
cd "$(dirname "$0")"

fail=0
pass() { echo "  PASS: $*"; }
bad()  { echo "  FAIL: $*"; fail=1; }

echo "======================================================"
echo "DEMO 1: ptrace interceptor (block /tmp/rahasia + anti-kill)"
echo "======================================================"
out1="$(./ptrace-hook --block-path rahasia --anti-kill -- ./victim 2>&1)"
echo "$out1" | sed -n '1,25p'
echo "$out1" | grep -q "BLOKIR openat(/tmp/rahasia.txt" \
    && pass "openat /tmp/rahasia.txt diblokir" \
    || bad "openat /tmp/rahasia.txt tidak diblokir"
out1_victim="$(echo "$out1" | sed -n '/\[victim\] pid=/,$p')"
echo "$out1_victim" | grep -q "open /tmp/rahasia.txt (baca).*GAGAL" \
    && pass "victim melihat ENOENT untuk /tmp/rahasia.txt" \
    || bad "victim masih bisa membuka /tmp/rahasia.txt"
echo "$out1_victim" | grep -q "masih hidup setelah SIGKILL" \
    && pass "anti-kill: proses tetap hidup setelah SIGKILL" \
    || bad "anti-kill gagal: proses mati"
echo "$out1_victim" | grep -q "execve /bin/echo (child).*status=0" \
    && pass "execve /bin/echo tetap lolos" \
    || bad "execve /bin/echo ikut terblokir (salah)"

echo
echo "======================================================"
echo "DEMO 2: seccomp user notification (sandbox supervisor)"
echo "======================================================"
out2="$(./seccomp-notif -- ./victim 2>&1)"
echo "$out2" | sed -n '1,25p'
out2_victim="$(echo "$out2" | sed -n '/\[victim\] pid=/,$p')"
echo "$out2_victim" | grep -q "open /etc/passwd (tulis).*GAGAL errno=13" \
    && pass "tulis /etc/passwd ditolak EACCES" \
    || bad "tulis /etc/passwd tidak ditolak EACCES"
echo "$out2_victim" | grep -q "open /tmp/ok.txt (tulis).*OK" \
    && pass "tulis /tmp/ok.txt tetap lolos" \
    || bad "tulis /tmp/ok.txt ikut diblokir (salah)"
echo "$out2_victim" | grep -q "masih hidup setelah SIGKILL" \
    && pass "anti-kill: proses tetap hidup" \
    || bad "anti-kill gagal lewat seccomp"
echo "$out2_victim" | grep -q "execve /bin/sh (child).*status=127" \
    && pass "execve /bin/sh ditolak" \
    || bad "execve /bin/sh tidak ditolak"

echo
echo "======================================================"
echo "DEMO 3: LD_PRELOAD sembunyikan file dari ls/cat/fopen"
echo "======================================================"
rm -f /tmp/rahasia.txt
echo "isi rahasia" > /tmp/rahasia.txt
ls /tmp | grep -q rahasia && pass "tanpa preload: file terlihat" || bad "tanpa preload: file hilang (aneh)"
LD_PRELOAD="$PWD/preload-hide.so" ls /tmp 2>/dev/null | grep -q rahasia \
    && bad "dengan preload: file MASIH terlihat" \
    || pass "dengan preload: ls tidak melihat file"
LD_PRELOAD="$PWD/preload-hide.so" cat /tmp/rahasia.txt >/dev/null 2>&1 \
    && bad "cat masih bisa membaca file rahasia" \
    || pass "cat gagal membuka file rahasia"
# fopen (stdio) harus ikut ter-hook: ini bypass yang ditemukan di audit.
LD_PRELOAD="$PWD/preload-hide.so" sh -c 'read x < /tmp/rahasia.txt' 2>/dev/null \
    && bad "shell redirect (<) masih bisa membaca file rahasia" \
    || pass "shell redirect juga tertolak"
# Mode O_CREAT harus diteruskan dengan benar (audit: mode 0700 -> 06710).
rm -f /tmp/create_mode_test
LD_PRELOAD="$PWD/preload-hide.so" ./modecheck 2>/dev/null || true
if [ -e /tmp/create_mode_test ]; then
    m="$(stat -c %a /tmp/create_mode_test)"
    [ "$m" = "700" ] && pass "mode O_CREAT 0700 tersimpan benar ($m)" \
                     || bad "mode O_CREAT salah: $m (harus 700)"
    rm -f /tmp/create_mode_test
else
    echo "  SKIP: modecheck tidak tersedia (make modecheck)"
fi
rm -f /tmp/rahasia.txt /tmp/ok.txt

echo
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
