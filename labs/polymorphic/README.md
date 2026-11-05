# Lab: Polymorphic Binary

Setiap run menghasilkan binary dengan **bentuk berbeda** (sha256 beda) tetapi
**fungsi identik** (output sama). Ini adalah inti teknik polymorphic.

## Arsitektur

```
payload.c ──(cc -nostdlib + objcopy)──▶ payload.bin  (shellcode raw, PIC)
                                              │
morph.py ── enkripsi XOR dgn key stream acak ──▶ enc[] (ciphertext)
   │        + generate stub assembly VARIAN:
   │           - register src/key/counter dipilih acak
   │           - body dekripsi: byte-wise / qword / xor-langsung-ke-mem
   │           - counter: dec+jnz / sub+jne / add(-1)+jne
   │           - NOP sled acak (nop, xchg %ax,%ax, nopw, lea 0(reg),reg)
   │           - init & jump: lea rip-rel / mov imm / push+ret
   ▼
stub.s ──(as + ld -nostdlib)──▶ variant_N   (ELF executable)
```

Runtime: stub (di section `.text` ber-flag `awx` = writable+executable)
mendekripsi `enc[]` di tempat, lalu lompat ke payload yang sudah plaintext.
Payload menulis `POLYMORPHIC-DEMO-OK\n` ke stdout via syscall (tanpa libc).

## Cara pakai

    make payload.bin
    python3 morph.py -p payload.bin -n 6 -o variant
    ./variant_1        # POLYMORPHIC-DEMO-OK
    sha256sum variant_*  # semua beda

Atau langsung verifikasi penuh:

    ./test.sh 6

## Kenapa ini "polymorphic", bukan sekadar enkripsi?

Enkripsi dengan key acak saja = *oligomorphic* (payload berubah, decryptor
tetap). Di sini **decryptor-nya sendiri** (stub) di-generate ulang tiap run:
instruksi berbeda, register berbeda, urutan berbeda, NOP sled acak — jadi
dua varian tidak punya bytecode decryptor yang sama. Itu polymorphic.

## Batas yang jujur

1. Level ini menyamarkan **signature statis** (hash/byte-pattern). Runtime
   tetap bisa dianalisis: stub harus mendekripsi di memori, jadi memory
   forensik / debugger pada proses hidup tetap melihat payload asli.
2. Anti-debugging (PTRACE_TRACEME, dumpable=0 — lihat `ENABLE_SELF_DEFENSE`
   di project utama) adalah lapisan pelengkap, bukan pengganti.
3. Packing polymorphic adalah teknik dual-use: dipakai software protection
   (anti-crack) dan juga malware. Lab ini untuk studi proteksi di binary
   sendiri; menggunakannya untuk menyebarkan payload berbahaya tidak etis
   dan ilegal.

## Variasi yang bisa dieksplorasi

- Enkripsi AES (bukan XOR) via `openssl enc` sebelum disisipkan.
- Stub multi-pass: dekripsi 2 lapis (XOR lalu byte-swap).
- Anti-disassembly: sisipkan byte `0xEB 0x01` (jmp pendek) di tengah
  instruksi agar disassembler linear salah parse.
- Self-modifying: stub menulis ulang instruksinya sendiri sebelum eksekusi.
