#!/usr/bin/env bash
# test.sh — verifikasi lab elfpack
#   1. packed static  -> output & exit code sama dengan asli (in-memory, no disk)
#   2. packed dynamic -> output & exit code sama, file temp /tmp/.elx_* bersih
#   3. xcb asli (kalau build/xcb ada) -> packed jalan dengan output identik
set -uo pipefail
cd "$(dirname "$0")"

echo "== build payload test =="
make hello_static hello_dyn >/dev/null || { echo "GAGAL build payload"; exit 1; }

echo "== pack =="
python3 elfpack.py -m static -p hello_static -o packed_static || exit 1
python3 elfpack.py -m dynamic -p hello_dyn -o packed_dyn || exit 1

echo
echo "== verifikasi STATIC: output + exit code identik =="
exp_s=$(./hello_static); exp_c=$?
got_s=$(./packed_static); got_c=$?
echo "  asli : [$exp_s] rc=$exp_c"
echo "  pack : [$got_s] rc=$got_c"
if [ "$exp_s" = "$got_s" ] && [ "$exp_c" -eq "$got_c" ]; then
    echo "  OK static (in-memory loader)"
else
    echo "  GAGAL static"; exit 1
fi

echo
echo "== verifikasi DYNAMIC: output + exit code + temp bersih =="
rm -f /tmp/.elx_*
exp_d=$(./hello_dyn); exp_c=$?
got_d=$(./packed_dyn); got_c=$?
echo "  asli : [$exp_d] rc=$exp_c"
echo "  pack : [$got_d] rc=$got_c"
if [ "$exp_d" = "$got_d" ] && [ "$exp_c" -eq "$got_c" ]; then
    echo "  OK dynamic (temp+execve)"
else
    echo "  GAGAL dynamic"; exit 1
fi
sleep 0.3
left=$(ls /tmp/.elx_* 2>/dev/null | wc -l)
if [ "$left" -eq 0 ]; then
    echo "  OK temp file dibersihkan (0 sisa)"
else
    echo "  GAGAL: $left sisa /tmp/.elx_*"; exit 1
fi

echo
echo "== ukuran =="
for f in hello_static packed_static hello_dyn packed_dyn; do
    printf "  %-14s %8d bytes\n" "$f" "$(stat -c%s "$f")"
done

echo
echo "== pack binary xcb asli (kalau ada) =="
if [ -f ../../build/xcb ]; then
    python3 elfpack.py -m dynamic -p ../../build/xcb -o xcb_packed || exit 1
    a=$(../../build/xcb --help 2>&1 | head -4); c1=$?
    b=$(./xcb_packed --help 2>&1 | head -4); c2=$?
    if [ "$a" = "$b" ]; then
        echo "  OK xcb: output --help identik (rc asli=$c1 rc packed=$c2)"
        printf "  asli: %8d bytes | packed: %8d bytes\n" \
            "$(stat -c%s ../../build/xcb)" "$(stat -c%s xcb_packed)"
    else
        echo "  GAGAL xcb: output beda"; exit 1
    fi
else
    echo "  (build/xcb belum ada di devbox — skip)"
fi

echo
echo "SELESAI: elfpack OK"
