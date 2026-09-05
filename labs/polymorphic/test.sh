#!/usr/bin/env bash
# test.sh — verifikasi lab polymorphic
#   1. SEMUA varian menghasilkan output yang SAMA (fungsi identik)
#   2. SEMUA varian punya sha256 yang BERBEDA (bentuk berbeda)
set -uo pipefail
cd "$(dirname "$0")"

N=${1:-6}
EXPECT="POLYMORPHIC-DEMO-OK"

echo "== build payload =="
make payload.bin >/dev/null || { echo "GAGAL: build payload"; exit 1; }
echo "payload.bin: $(wc -c < payload.bin) bytes"

echo "== generate $N varian polymorphic =="
rm -f variant_*
python3 morph.py -p payload.bin -n "$N" -o variant || exit 1

echo
echo "== verifikasi 1: semua output SAMA =="
fail=0
for v in variant_*; do
    out=$("$v" 2>&1)
    if [ "$out" = "$EXPECT" ]; then
        echo "  OK   $v -> $out"
    else
        echo "  GAGAL $v -> '$out'"
        fail=1
    fi
done

echo
echo "== verifikasi 2: semua sha256 BERBEDA =="
uniq=$(sha256sum variant_* | awk '{print $1}' | sort -u | wc -l)
total=$(ls variant_* | wc -l)
echo "  hash unik: $uniq / $total"
[ "$uniq" -eq "$total" ] || fail=1

echo
echo "== contoh disassembly satu varian (potongan) =="
objdump -d variant_1 2>/dev/null | head -20

[ "$fail" -eq 0 ] && echo "SELESAI: polymorphic OK" || echo "ADA YANG GAGAL"
exit $fail
