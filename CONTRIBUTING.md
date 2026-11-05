# Kontribusi

Terima kasih sudah membantu. Dokumen ini menjelaskan alur kerja yang dipakai
repo ini supaya perubahan mudah ditinjau dan tidak merusak build/deploy.

## Prasyarat

- Linux x86_64 dengan AES-NI + AVX2 (atau aarch64 dengan crypto extensions).
- CMake ≥ 3.16, compiler C++17, OpenSSL dev headers, git.
- Submodule wajib di-init: `git submodule update --init --recursive`.

## Build & test

```bash
./setup.sh                 # deps + submodule + build + selftest
./build.sh                 # build cepat + selftest
./build/xcb --selftest     # verifikasi hash
(cd build && ctest --output-on-failure)
```

Untuk perubahan yang menyentuh deploy, uji juga:

```bash
git ls-files -z '*.sh' | xargs -0 -r shellcheck -S style
```

Artefak deploy fleet **wajib dibangun di Ubuntu 22.04** (glibc 2.35). Jangan
memakai binary hasil build host 24.04 untuk worker 22.04 — lihat
[`ops/fleet/README.md`](ops/fleet/README.md).

## Alur branch & PR

1. Buat branch dari `main` dengan nama yang menjelaskan perubahan:
   `feat/...`, `fix/...`, `ops/...`, atau `chore/...`.
2. Jangan push langsung ke `main`. Semua perubahan lewat pull request.
3. Pastikan CI hijau: glibc, CONN, Alpine/musl, Docker, artefak 22.04, dan
   ShellCheck.
4. Isi template PR: ringkasan, verifikasi, risiko, dan rollback.
5. Merge dilakukan setelah review; branch yang sudah merged boleh dihapus.

## Gaya commit

- Satu commit = satu perubahan logis.
- Awali dengan scope: `conn:`, `ops:`, `ci:`, `docs:`, `chore:`.
- Tulis alasan di body kalau tidak terlihat dari diff.
- Jangan menyertakan secret, token, alamat host internal, atau file konfigurasi
  lokal (`~/.config/xcb/`, `AGENTS.md`).

## Struktur singkat

| Path | Isi |
|---|---|
| `src/` | miner XCB (RandomY) + backend CONN (RandomX V2) |
| `external/` | submodule RandomY/FTXUI + RandomX V2 vendored |
| `labs/` | lab studi keamanan (rootkit, hook, packer) — bukan bagian runtime |
| `ops/fleet/` | deploy, rollback, monitoring fleet |
| `ops/instabox/` | kontrol sandbox instabox |
| `ops/systemd/` | unit systemd + contoh environment |
| `k8s/`, `docs/` | contoh deployment & screenshot |

## Lisensi

Dengan mengirim kontribusi, Anda menyetujui kontribusi tersebut dilisensikan
di bawah MIT License yang sama dengan proyek ini.
