# Lab: ELF Runtime Packer (gaya UPX) — packing penuh binary asli

Packer yang membungkus binary ELF apa pun (termasuk xcb) menjadi binary
baru yang lebih kecil dan ter-obfuscate. Binary asli tidak pernah ada di
disk dalam bentuk plain — hanya versi terkompresi (zlib/RLE + XOR) di
dalam stub. Saat dijalankan, stub memulihkan binary di memori lalu
mengeksekusinya.

Hasil terukur di devbox (xcb 830 KB, dynamic):

    xcb asli : 830120 bytes
    xcb packed: 352416 bytes   (42.5%)
    output --help & --selftest identik, temp file selalu bersih

## Dua mode

### Mode static — in-memory loader (`-m static`)

Untuk ET_EXEC / static binary. Stub **assembly murni** (syscall langsung,
tanpa libc, PIE):

```
mmap(range RW)                        # satu mapping untuk seluruh range PT_LOAD
  └─ tiap PT_LOAD: decode blob (RLE+XOR) ke base+(p_vaddr-min_v)
  └─ zero-fill bss (memsz - filesz)
  └─ mprotect(segment, prot asli, range page-aligned + clamp)
auxv rewrite                          # AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY
                                      # diarahkan ulang ke payload (fix-up
                                      # stack yang sama dilakukan UPX)
reset GPR (kecuali rsp)               # kernel set semua reg = 0 saat exec;
                                      # glibc _start baca rdx=rtld_fini
jmp entry                             # stack utuh: argc/argv/envp/auxv asli
```

- Payload **tidak pernah menyentuh disk** — murni di memori.
- Stub PIE supaya tidak bentrok dengan p_vaddr payload (0x400000);
  untuk ET_EXEC, mmap MAP_FIXED di min_v; untuk ET_DYN, mmap anon bebas.
- mprotect butuh addr page-aligned: range di-round + di-clamp ke region
  (p_vaddr segmen terakhir sering tidak aligned).
- Tanpa auxv rewrite, glibc static crash NULL-deref di `__libc_start_main`
  (membaca program headers lewat auxv untuk cari PT_TLS).
- Tanpa reset register, rdx sisa stub (prot=3 dari mprotect) dibaca glibc
  sebagai rtld_fini → garbage.

### Mode dynamic — temp file + execve (`-m dynamic`)

Untuk ET_DYN / dynamic binary (PIE, linked libc/openssl — termasuk xcb).
Binary dynamic butuh kernel loader normal (PT_INTERP, relokasi GOT/PLT,
shared library), jadi pendekatan in-memory penuh terlalu besar — UPX juga
pakai trik serupa. Stub C kecil (dynamic-linked, `-lz`):

```
decode zlib+XOR seluruh file ELF -> buffer anon
tulis /tmp/.elx_<pid> (O_EXCL, 0700)
fork
  child : execve(path, argv, envp)   # argv asli -> argv[0] utuh
  parent: wait4 -> unlink(path)      # temp hidup hanya milidetik
exit(status child)
```

## Kompresi

- **static**: RLE (control byte: bit7=1 -> run count (c&0x7f)+1 + value;
  bit7=0 -> literal). Efektif untuk data berulang; untuk kode padat
  overhead. Self-test roundtrip (encoder == decoder assembly) di setiap
  pack.
- **dynamic**: zlib level 9 + XOR 1-byte key acak tiap run -> sha256 hasil
  selalu beda.

## Cara pakai

    make hello_static hello_dyn          # payload test
    python3 elfpack.py -m static  -p hello_static -o packed_static
    python3 elfpack.py -m dynamic -p hello_dyn    -o packed_dyn
    ./packed_static    # output & rc identik dgn hello_static
    ./packed_dyn       # output & rc identik dgn hello_dyn

    # binary asli (xcb):
    python3 elfpack.py -m dynamic -p ../../build/xcb -o xcb_packed
    ./xcb_packed --selftest

Verifikasi penuh: `./test.sh` (membutuhkan `zlib1g-dev`).

## Batas yang jujur

1. Mode dynamic menulis payload plain ke /tmp selama beberapa milidetik.
   Proses yang berjalan tetap visible — packing menyamarkan file di disk,
   bukan proses di memori (kombinasikan dgn `labs/process-hide` +
   `ENABLE_SELF_DEFENSE` untuk itu).
2. XOR 1-byte + zlib = obfuscation statis, bukan kripto kuat (known-
   plaintext ELF header `\x7fELF`). Packer produksi (UPX) pakai UCL/LZMA
   + anti-tamper; lab ini fokus arsitektur loader.
3. Mode static MAP_FIXED di p_vaddr payload — aman di lab (stub PIE di
   address tinggi); di lingkungan nyata cek bentrok mapping dulu.
4. Dual-use: packing dipakai software protection (anti-crack, anti-hash-
   signature) dan juga malware. Lab ini untuk binary sendiri.

## Perbandingan dengan UPX

| Aspek            | UPX                    | Lab ini                          |
|------------------|------------------------|----------------------------------|
| Kompresi         | UCL/LZMA               | zlib (dynamic) / RLE (static)    |
| Dynamic ELF      | Dukungan terbatas      | Temp+execve (robust)             |
| In-memory (statik)| Ya                    | Ya (assembly murni, tanpa libc)  |
| Stack fix-up     | Ya (auxv/register)     | Ya (auxv rewrite + reset GPR)    |
| Anti-tamper      | Ada (checksum, header) | Tidak                            |
| Stub             | ~3 KB                  | ~2 KB (static asm) / ~16 KB (dyn)|
