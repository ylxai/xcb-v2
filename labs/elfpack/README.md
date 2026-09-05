# Lab: ELF Runtime Packer (gaya UPX) — packing penuh binary asli

Packer yang membungkus binary ELF apa pun (termasuk xcb) menjadi binary
baru yang lebih kecil dan ter-obfuscate. Binary asli tidak pernah ada di
disk dalam bentuk plain — hanya versi terkompresi (RLE + XOR) di dalam
stub. Saat dijalankan, stub memulihkan binary di memori dan mengeksekusinya.

## Dua mode

### Mode static — in-memory loader (`-m static`)

Untuk ET_EXEC / static binary. Stub **assembly murni** (syscall langsung,
tanpa libc, PIE):

```
mmap(range RW)          # satu mapping untuk seluruh range PT_LOAD
  └─ tiap PT_LOAD: decode blob (RLE+XOR) ke base+(p_vaddr-min_v)
  └─ zero-fill bss (memsz - filesz)
  └─ mprotect(segment, prot asli: R/W/X)
jmp base + entry_off    # stack TIDAK disentuh -> argc/argv/envp utuh dari kernel
```

- Payload **tidak pernah menyentuh disk** — murni di memori.
- Stub di-link PIE supaya tidak bentrok dengan p_vaddr payload (0x400000).
- Untuk ET_EXEC: mmap MAP_FIXED di min_v; untuk ET_DYN: mmap anon bebas
  dan entry = base + (e_entry - min_v).

### Mode dynamic — temp file + execve (`-m dynamic`)

Untuk ET_DYN / dynamic binary (PIE, linked libc/openssl — termasuk xcb).
Binary dynamic butuh kernel loader normal untuk PT_INTERP, relokasi GOT/PLT,
dan pemetaan shared library, jadi loader in-memory penuh terlalu besar.
Stub C (`-static -nostdlib`):

```
decode seluruh file ELF -> buffer anon
tulis /tmp/.elx_<pid> (O_EXCL, 0700)
fork
  child : execve(path, argv, envp)   # kernel loader normal -> payload jalan
  parent: wait4 -> unlink(path)      # temp hidup hanya milidetik
exit(status child)
```

## Kompresi: RLE + XOR

- RLE: control byte. `bit7=1` -> run `count=(c&0x7f)+1` diikuti value;
  `bit7=0` -> literal. Efektif untuk ELF (banyak byte 0 / alignment padding).
- XOR 1-byte key acak per run -> sha256 hasil beda tiap run.
- Batas jujur: ini bukan kripto kuat. Tujuan lab = menyamarkan signature
  statis + mengurangi ukuran, bukan enkripsi isi yang aman.

## Cara pakai

    make hello_static hello_dyn          # payload test
    python3 elfpack.py -m static  -p hello_static -o packed_static
    python3 elfpack.py -m dynamic -p hello_dyn    -o packed_dyn
    ./packed_static   # output & rc identik dgn hello_static
    ./packed_dyn      # output & rc identik dgn hello_dyn

    # binary asli (xcb):
    python3 elfpack.py -m dynamic -p ../../build/xcb -o xcb_packed
    ./xcb_packed --help

Verifikasi penuh: `./test.sh`

## Batas yang jujur

1. Mode dynamic menulis payload plain ke /tmp selama beberapa milidetik
   (antara write dan exec). Process yang berjalan tetap visible — packing
   menyamarkan file di disk, bukan proses di memori. Untuk stealth proses,
   kombinasikan dengan `labs/process-hide` + `ENABLE_SELF_DEFENSE`.
2. RLE+XOR 1-byte bisa dipecahkan dengan statistik / known-plaintext
   (ELF header `\x7fELF` ada di offset 0). Packer produksi (UPX) memakai
   algoritma lebih kuat + anti-tamper; di sini fokusnya arsitektur loader.
3. Mode static memetakan payload dengan MAP_FIXED — kalau ada library yang
   kebetulan di-map di p_vaddr payload, mapping akan tertimpa. Untuk lab ini
   aman (stub PIE di address tinggi, payload di 0x400000).
4. Dual-use: packing dipakai software protection (anti-crack, anti-hash-
   signature) dan juga malware. Lab ini untuk binary sendiri.

## Perbandingan dengan UPX

| Aspek            | UPX                          | Lab ini                        |
|------------------|------------------------------|--------------------------------|
| Kompresi         | UCL/LZMA                    | RLE (sederhana, edukatif)      |
| Dynamic ELF      | Dukungan terbatas           | Temp+execve (robust)           |
| In-memory (statik)| Ya                          | Ya (assembly murni, tanpa libc)|
| Anti-tamper      | Ada (checksum, header)      | Tidak                          |
| Ukuran stub      | ~3 KB                        | ~1-2 KB (static)               |
