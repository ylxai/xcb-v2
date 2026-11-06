# Audit Codebase `xcb-v2` — Indeks & Ringkasan Eksekutif

Tanggal Audit: `2026-11-06`
Cakupan Audit: `src/`, `ops/`, skrip root (`mine.sh`, `setup.sh`, `build.sh`), dan `CMakeLists.txt`

---

## Ringkasan Eksekutif

Fondasi arsitektur `xcb-v2` secara umum sudah cukup matang dan memperlihatkan hasil *hardening* yang baik dari iterasi sebelumnya:
- **Hot Path Hashing (`src/Miner.cpp`, `src/ConnMiner.cpp`, `src/Sha3_512.cpp`):** Bebas alokasi heap dan bebas lock pada *per-hash loop*. Implementasi `Sha3_512` beroperasi langsung pada buffer stack.
- **Konversi Numerik & Hex (`src/Config.cpp`, `src/StratumClient.cpp`):** Sudah menggunakan `std::strtoul` / `std::strtoull` dengan pengecekan `errno` dan batas nilai, menghindari `std::stoi`/`std::stoul` telanjang yang dapat memicu `std::terminate` dari input eksternal.
- **Konkurensi Socket TLS (`src/ConnPool.cpp`):** Akses `SSL_read`/`SSL_write` dilindungi oleh `m_ioMu` sementara `poll()` dilakukan di luar lock sehingga `submit()` tidak tertahan oleh `readLine()`.
- **Manajemen Memori Dataset 2 GiB (`src/ConnMiner.cpp`):** Rotasi `seed_hash` melepaskan `Engine` lama (`m_engine.reset()`) sebelum mengalokasikan dataset baru agar hugepages tidak habis akibat tumpukan dua dataset.
- **Antrian Submitter (`src/Submitter.cpp`):** Memiliki batas ukuran (`MAX_QUEUE = 64`) dan mekanisme *drop oldest* serta berkoordinasi dengan `m_submitting` untuk menghindari antrian tumbuh tak terbatas.

Meskipun demikian, ditemukan **4 temuan prioritas tinggi (High)**, **6 temuan prioritas menengah (Medium)**, dan **5 temuan prioritas rendah/hygiene (Low)** yang perlu ditindaklanjuti.

---

## Daftar Dokumen Review

1. [`01-high-severity.md`](./01-high-severity.md) — Temuan Prioritas Tinggi (`H1` – `H4`)
2. [`02-medium-severity.md`](./02-medium-severity.md) — Temuan Prioritas Menengah (`M1` – `M6`)
3. [`03-low-severity-and-hygiene.md`](./03-low-severity-and-hygiene.md) — Temuan Prioritas Rendah, Hygiene (`L1` – `L5`), dan Catatan Positif
4. [`04-verifikasi.md`](./04-verifikasi.md) — Verifikasi independen per temuan: verdict, bukti di kode, dan koreksi

> **Status verifikasi (2026-11-06, `main` = `b1f0b2a`):** 14 dari 15 temuan
> valid — H3, M1–M6, L1, L3, L4, L5 aktif; H1 dan H2 hanya aktif bila
> `ENABLE_SELF_DEFENSE=ON`; H4 valid tetapi severity-nya berlebihan.
> **L2 tidak valid** (bukan out-of-bounds read). Beberapa kutipan kode di
> dokumen 01–03 tidak cocok dengan repo dan tidak boleh dipakai sebagai dasar
> patch — rinciannya di `04-verifikasi.md`.

---

## Matriks Ringkasan Temuan

| ID | Severity | Lokasi Utama | Ringkasan Masalah |
| :--- | :--- | :--- | :--- |
| **H1** | High | `src/main.cpp:195-200`, `src/SelfDefense.cpp:61-90` | `set_process_title()` menimpa `argv` dan `environ` dengan `0` sebelum `Config::parse()` dijalankan + potensi *buffer over-write* |
| **H2** | High | `src/SelfDefense.cpp:53` | `ptrace(PTRACE_TRACEME)` tanpa proses tracer sendiri membekukan proses (`TASK_TRACED`) saat menerima sinyal apa pun |
| **H3** | High | `src/Config.cpp:607-648` | `Config::autoDetect()` menghitung kebutuhan RAM & HugePages dari konstanta `RandomY` (~264 MB), bukan `RandomX V2` (~2080 MB) saat `-a conn` |
| **H4** | High | `src/ConnMiner.cpp:659-720` | `ConnMiner` belum menerapkan CPU affinity pinning (`Config::usableCpus()`) maupun prioritas penjadwalan |
| **M1** | Medium | `src/Config.cpp:235-295`, `ops/systemd/syscached.service:12-14` | Env `WALLET`, `WORKER`, dan `THREADS` diabaikan pada jalur `-a conn` jika `POOL` tidak diset, memaksa `syscached.service` menaruh `${WALLET}` di CLI |
| **M2** | Medium | `src/ConnPool.cpp:244-247, 522-527`, `src/ConnRpc.cpp:521, 539, 543` | Penyusunan string JSON manual tanpa *escaping* pada `ConnPool` dan `ConnRpc` |
| **M3** | Medium | `src/StratumClient.cpp:257-292`, `src/ConnRpc.cpp:330-338` | `StratumClient` belum mendukung TLS & tidak mengiterasi seluruh hasil `getaddrinfo()` (begitu pula `ConnRpc`) |
| **M4** | Medium | `ops/fleet/deploy.sh:126, 220-242` | `deploy_one()` dan `rollback_one()` mengabaikan exit code `apply.sh` dan hanya mengandalkan `grep 'HASIL=OK'` + kebocoran `mktemp -d` |
| **M5** | Medium | `ops/systemd/syscached.service:7-19` | Service systemd berjalan penuh sebagai `root` tanpa pembatasan hak akses (`User=`, `ProtectSystem=`, `Capabilities`) |
| **M6** | Medium | `mine.sh:46-50` | `mine.sh` selalu mengalokasikan hugepages sebesar ~3.2 GB walaupun menjalankan algoritma `randomy` yang hanya butuh ~520 MB |
| **L1** | Low | `src/Config.cpp:72` | Potensi *null pointer dereference* pada `getpwuid(getuid())->pw_dir` bila `HOME` tidak di-set |
| **L2** | Low | `src/StratumClient.cpp:34-40, 65-69` | Potensi *out-of-bounds read* 1 byte saat string JSON berakhir dengan karakter `'\\'` |
| **L3** | Low | `src/Config.cpp:417`, `src/Miner.cpp:278` | Teks bantuan `--help` masih menyebut binary `rk_sysd` dan tag log internal `"sysd"` |
| **L4** | Low | `src/ConnMiner.cpp:226` | Titik awal nonce di `ConnMiner` selalu dimulai dari `0` (berisiko duplikasi kerja antar mesin pada mode solo RPC / pool tanpa extranonce unik) |
| **L5** | Low | `CMakeLists.txt:140`, `src/main.cpp:202` | Pengujian otomatis (`ctest`) baru mencakup `--selftest` kriptografi, belum menguji parser Config/JSON dan pembuatan blok |

---

## Rekomendasi Urutan Remediasi

1. **Fase 1 — Stabilitas & Performa Inti ConnectCoin (`H3`, `H4`):**
   - Perbaiki kalkulasi ukuran dataset & hugepages `Config::autoDetect()` berdasarkan `cfg.algo` (`conn` vs `randomy`).
   - Tambahkan CPU affinity pinning menggunakan `Config::usableCpus()` di `ConnMiner::Impl::workerLoop()`.
2. **Fase 2 — Keamanan & Keandalan Self-Defense (`H1`, `H2`):**
   - Pindahkan `xcb::selfdefense::set_process_title()` ke setelah `Config::parse()` dan `lg::init()`, serta perbaiki cara menghitung panjang buffer `argv`/`environ`.
   - Hapus `ptrace(PTRACE_TRACEME)` dari `SelfDefense.cpp`.
3. **Fase 3 — Konfigurasi, Jaringan & Operasional Fleet (`M1`, `M2`, `M3`, `M4`, `M5`, `M6`):**
   - Perbaiki pembacaan env `WALLET`/`WORKER`/`THREADS` untuk `-a conn` dan hapus argumen `-u ${WALLET}` dari `syscached.service`.
   - Tambahkan `jsonEscape()` di `ConnPool` dan `ConnRpc`.
   - Perbaiki pengecekan exit code di `ops/fleet/deploy.sh` dan ukuran hugepages di `mine.sh`.
   - Tambahkan dukungan TLS dan iterasi `addrinfo` di `StratumClient` / `ConnRpc`.
4. **Fase 4 — Kebersihan Kode & Unit Test (`L1` – `L5`):**
   - Perbaiki *null-check* `getpwuid`, batas indeks JSON `StratumClient`, randomisasi awal nonce `ConnMiner`, penamaan `--help`, dan perluas cakupan `ctest`.
