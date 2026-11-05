# Konstitusi xcb

## Prinsip Inti

### I. Integritas Wallet — Zero Dev Fee (NON-NEGOTIABLE)

Setiap hash yang dihitung miner harus masuk ke wallet yang dikonfigurasi pengguna. Tidak ada pengecualian.

- Tidak boleh ada wallet default di binary, image Docker, atau manifest deploy. Tanpa `WALLET`, miner berhenti dengan `No wallet configured`.
- Tidak boleh ada jalur kode yang mengalihkan share, mengganti alamat, atau memotong fee — tersembunyi maupun opsional.
- Precedence konfigurasi tetap: **env vars > `pool.cfg` > flag CLI**. Mengubah urutan ini adalah perubahan breaking dan wajib lewat PR terpisah.
- `pool.cfg` di repo memakai `wallet=` kosong; jangan pernah mengisinya dengan alamat nyata.

### II. Verifikasi Sebelum Dipercaya

Klaim "sudah benar" hanya sah kalau dibuktikan gate yang ada.

- `./build/xcb --selftest` wajib lolos dan memverifikasi implementasi hashing (SHA3-512/keccak) sebelum mining.
- `ctest --output-on-failure` wajib hijau.
- Perubahan pada jalur hashing (`RandomY`, `external/randomx-v2`, `Sha3_512`, `Submitter`) wajib disertai bukti selftest dan benchmark smoke yang benar-benar menghitung hash.
- Selftest harus tetap jalan dari direktori mana pun, termasuk tanpa `pool.cfg`.

### III. Build Portabel dan Dapat Direproduksi

- C++17, CMake ≥ 3.12, OpenSSL, pthread. Menaikkan standar bahasa butuh PR khusus.
- Dua target libc wajib tetap build: glibc dan musl/Alpine. Di musl, LTO dimatikan otomatis.
- Release glibc wajib memakai LTO dan baseline `-march=x86-64-v3`.
- Artefak deploy wajib dibangun di Ubuntu 22.04 (glibc 2.35). Binary hasil host 24.04 tidak boleh dipakai di worker 22.04.

### IV. Gagal Keras, Jangan Diam

- Deteksi otomatis hugepages/RAM harus memilih mode aman (`FULL_MEM=0`, `LARGE_PAGES=0`), bukan gagal alokasi.
- Proses tidak boleh keluar dengan status 0 tanpa pernah menghitung satu hash.
- Error konfigurasi, pool, dan kripto harus muncul eksplisit di log/stderr dengan konteks yang cukup untuk didiagnosis.

### V. Dependensi Minimal

- Dependensi eksternal hanya: RandomY (submodule), RandomX V2 (vendored), FTXUI (submodule), OpenSSL.
- Menambah dependency baru wajib dijelaskan di PR: apa yang tidak bisa dicapai dengan yang sudah ada.
- Kode runtime masuk `src/`; `labs/` adalah lab studi keamanan dan bukan bagian runtime.

### VI. Keamanan dan Kebersihan Secret

- Jangan commit secret, token, wallet, alamat host internal, SSH key, atau config lokal (`~/.config/xcb/`, `/etc/xcb/miner.env`).
- `AGENTS.md` adalah file lokal: jangan di-commit, jangan dihapus.
- Workflow CI memakai least privilege (`permissions: contents: read`); jangan menaikkannya tanpa alasan.
- `ENABLE_SELF_DEFENSE` OFF secara default dan tidak boleh aktif diam-diam.

## Batasan Teknis

- Target: Linux x86_64 dengan AES-NI + AVX2, atau aarch64 dengan crypto extensions.
- Satu binary, dua backend: `randomy` (XCB, default) dan `conn` (ConnectCoin, RandomX V2). Simbol RandomX V2 di-prefix `rxv2_` agar tidak bentrok dengan RandomY.
- Mode memori: light (~256MB) dan full (~264MB); pilihan otomatis dari RAM tersedia / cgroup limit.
- Dashboard TUI (FTXUI) tidak boleh menjadi ketergantungan wajib untuk mode headless.

## Alur Kerja dan Quality Gate

- Jangan push langsung ke `main`. Branch dari `main`, buka PR, tunggu CI hijau.
- Nama branch: `feat/`, `fix/`, `ops/`, `chore/`.
- Scope commit: `conn:`, `ops:`, `ci:`, `docs:`, `chore:`. Satu commit = satu perubahan logis.
- Build dan test dijalankan di host build (vps1), bukan di laptop lokal.
- Shell script wajib lolos `shellcheck -S style`.
- CI wajib hijau untuk: ShellCheck, build+selftest glibc, CONN, musl, Docker, dan artefak 22.04.
- Jangan mengganggu fleet produksi; hentikan service hanya sementara bila butuh CPU penuh untuk build.

## Governance

- Konstitusi ini mengikat semua perubahan. Bila bertentangan dengan dokumen lain, konstitusi yang menang.
- Amandemen lewat PR yang menjelaskan alasan dan dampaknya; versi dan tanggal diperbarui.
- Setiap PR harus diverifikasi terhadap prinsip di atas; kompleksitas tambahan wajib dibenarkan.
- `README.md`, `CONTRIBUTING.md`, dan `docs/HANDOFF.md` tetap menjadi panduan operasional harian.

**Version**: 1.0.0 | **Ratified**: 2026-11-06 | **Last Amended**: 2026-11-06
