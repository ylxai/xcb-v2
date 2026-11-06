# 4. Verifikasi Independen atas Audit

- **Tanggal verifikasi:** 2026-11-06
- **Commit yang diverifikasi:** `b1f0b2a` (`main`)
- **Metode:** setiap temuan dibaca ulang langsung dari kode di commit tersebut, bukan dari kutipan di dokumen audit. Verdict hanya diberikan kalau baris yang dirujuk benar-benar berisi perilaku yang diklaim.

---

## Ringkasan Verdict

| Verdict | Jumlah | Temuan |
|---|---|---|
| Valid, aktif di produksi | 9 | H3, M1, M2, M3, M4, M5, M6, L1, L3 |
| Valid, tetapi hanya aktif bila `ENABLE_SELF_DEFENSE=ON` | 2 | H1, H2 |
| Valid, severity berlebihan | 1 | H4 |
| Valid, dampak rendah | 2 | L4, L5 |
| **Tidak valid** | 1 | L2 |

Dari 15 temuan, **14 dapat diterima** dan **1 harus ditolak**. Satu di antaranya (M4) benar dalam substansi tetapi mekanismenya salah dikutip.

---

## Verdict per Temuan

### H1 — `set_process_title()` sebelum `Config::parse()`

**Valid, laten.**

`src/main.cpp:195-197` memanggil `selfdefense::init()` dan `set_process_title()`, lalu `Config::parse()` baru di `:200`. `src/SelfDefense.cpp:84` melakukan `memset(argv[0], 0, total)` dengan `total` mencakup seluruh string `argv` dan `environ`, sehingga argumen CLI menjadi string kosong dan `getenv()` kehilangan semua variabel.

Dua koreksi:

1. Kode asli **tidak** memakai `noexcept`, **tidak** memeriksa `t && *t` (hanya `if (const char* t = getenv("XCB_TITLE"))`), dan **tidak** melakukan `argv[i] = nullptr` seperti di kutipan.
2. Klaim *buffer over-write* belum terbukti. Di Linux `argv` dan `environ` bersebelahan, dan `grep -rn "setenv\|putenv" src/` tidak menemukan pemanggilan yang bisa merealokasi `environ`. Risikonya tetap ada secara teori, bukan bug yang sedang terjadi.

Dampak nyata: nol pada build sekarang. `ENABLE_SELF_DEFENSE` default `OFF` (`CMakeLists.txt:3`), tidak dipakai CI, dan binary yang terpasang di fleet tidak memuat string `selfdefense`.

### H2 — `ptrace(PTRACE_TRACEME)` membekukan proses

**Valid, laten.**

`src/SelfDefense.cpp:53-56` memang memanggil `PTRACE_TRACEME`. Di bawah systemd, parent menjadi tracer; sinyal seperti `SIGTERM` menaruh proses di `TASK_TRACED` sampai ada `PTRACE_CONT`, yang tidak pernah dikirim systemd. Proses akhirnya hanya mati lewat `SIGKILL`.

Koreksi: kutipan `(void)ptrace(...)` salah. Kode asli memeriksa hasil dan `_exit(1)` bila gagal.

Gating sama dengan H1: hanya berlaku pada build dengan `-DENABLE_SELF_DEFENSE=ON`.

### H3 — `autoDetect()` memakai konstanta RandomY untuk semua algo

**Valid dan aktif.**

- `src/Config.cpp:529` memanggil `autoDetect(cfg)` untuk semua algo, termasuk `conn`.
- `src/Config.cpp:620-622` dan `:640-642` menghitung `datasetMb` dari `randomx_dataset_item_count()` milik RandomY: 264 MB dataset + 256 MB cache → ambang 776 MB.
- CONN memakai RandomX V2: 2080 MB dataset + 256 MB cache = 2336 MB (`external/randomx-v2/src/configuration.h:50,53`).
- Nilainya diteruskan ke `ConnMiner` di `src/main.cpp:215-216` (`mc.fastMem`, `mc.largePages`).

Akibatnya host dengan RAM 0,8–2,5 GB memilih FAST 2 GiB, dan host dengan 520–2336 MB hugepage bebas memilih `largePages` yang tidak cukup. Di fleet sekarang bug ini tertutup karena host punya RAM 8 GB dan 2400 MB hugepages — bukan karena logikanya benar.

Catatan: kegagalan alokasi berujung fallback (LIGHT atau 4K page), bukan crash, kecuali OOM killer ikut campur. Karena itu severity lebih tepat **Medium**, bukan High.

### H4 — `ConnMiner` tanpa CPU affinity dan prioritas

**Valid, severity berlebihan.**

`src/ConnMiner.cpp` tidak memanggil `setpriority` maupun `pthread_setaffinity_np`; `src/Miner.cpp:508-524` melakukannya. Di produksi `taskset -cp 130150` menunjukkan `0-3` dan nice 0.

Tetapi pada fleet ini setiap host 4 vCPU dengan `-t 4` dan tidak ada beban lain, jadi migrasi thread hampir tidak terjadi. Ini tuning, bukan cacat. Severity lebih tepat **Low/Medium**.

### M1 — Env `WALLET`/`WORKER`/`THREADS` diabaikan pada jalur CONN

**Valid dan aktif.**

`src/Config.cpp:264` hanya membaca ketiganya di dalam `if (envWallet && envPool)`. `applyEnvFlags()` (`:235-262`) membaca `FULL_MEM`, `LARGE_PAGES`, `REPORT_HASHRATE`, `POLL_MS`, `BENCHMARK`, `ALGO`, dan `CONN_*` — **tidak** `WALLET`, `WORKER`, atau `THREADS`.

Terbukti di produksi: unit memasang wallet di `ExecStart` (`ops/systemd/syscached.service:14`) dan `ps` menampilkan `-u <wallet>` pada cmdline proses. Unit sendiri sudah mencatat batasan ini di komentar `:12-13`, jadi temuan ini benar dan sudah diketahui.

### M2 — JSON tanpa escaping di `ConnPool` dan `ConnRpc`

**Valid.**

- `src/ConnPool.cpp:240-248` menyusun login dengan `who` (wallet+worker) apa adanya.
- `src/ConnPool.cpp:518-528` menyusun submit dengan `m_loginId`, `job.id`, dan `job.algo` apa adanya.
- `src/ConnRpc.cpp:522/538/543` menyusun `["<blockHex>"]` dan `["<address>"]` apa adanya.
- Hanya `src/StratumClient.cpp:17` yang punya `json_escape()`.

Nilainya umumnya alfanumerik, jadi kejadiannya jarang, tetapi payload bisa rusak tanpa diagnostik kalau pool mengirim `job_id` dengan kutip atau backslash.

### M3 — `StratumClient` tanpa TLS dan hanya record DNS pertama

**Valid.**

`src/StratumClient.cpp` tidak memuat satu pun referensi SSL. `openSocket()` (`:257-292`) dan `ConnRpc` (`:325-345`) memakai `res` pertama dari `getaddrinfo()` tanpa iterasi `ai_next`, sedangkan `ConnPool` sudah mengiterasi.

Ini sama dengan item H4 pada audit sebelumnya: keputusannya masih terbuka (implementasi TLS atau catat sebagai risiko diterima).

### M4 — `deploy.sh` mengabaikan exit code dan membocorkan `mktemp`

**Valid, mekanisme dikutip salah.**

- `ops/fleet/deploy.sh:222` memang hanya `grep -q 'HASIL=OK'`. Exit code tidak pernah diperiksa: skrip memakai `set -uo pipefail` (`:12`) tanpa `-e`, dan `out=$(run_apply ...)` tidak menangkap `$?`.
- Tidak ada `|| true` seperti di kutipan; pengabaian terjadi karena `set -e` memang tidak dipakai.
- `grep` mencocokkan di mana saja dalam output, termasuk baris `journalctl` yang dicetak `apply.sh`, jadi positif palsu mungkin terjadi.
- `FROM=$(mktemp -d)` di `:126` tidak pernah dihapus; `cleanup_stage()` hanya membersihkan `REMOTE_DIR`.

### M5 — Unit systemd berjalan sebagai root

**Valid, kutipan salah.**

`ops/systemd/syscached.service:7-18` tidak punya `User=`, `ProtectSystem=`, atau `CapabilityBoundingSet=`. Proses di produksi berjalan sebagai `root` (UID 0).

Koreksi: unit **tidak** memuat `Nice=-5` seperti di kutipan, dan **sudah** memuat `NoNewPrivileges=true` (`:18`) yang tidak disebut dokumen. Jadi "tanpa pembatasan sama sekali" tidak akurat; yang benar adalah pembatasannya sangat lemah.

### M6 — `mine.sh` memakai asumsi 2,5 GiB untuk semua algo

**Valid.**

`mine.sh:46-47` selalu memakai `(2560 + 640) MB` untuk full mode; skrip tidak mendeteksi `-a conn` maupun `ALGO`. Untuk `randomy` (default) kebutuhan sebenarnya ~520 MB, jadi reservasi ~5× lebih besar. Skrip membatasi dengan `MAX_PAGES` dari RAM tersedia, sehingga tidak overcommit, tetapi di host kecil ia mencetak peringatan palsu dan mengunci RAM yang tidak terpakai.

### L1 — `getpwuid(getuid())->pw_dir` tanpa cek null

**Valid.**

`src/Config.cpp:71-72`:
```cpp
const char* home = getenv("HOME");
if (!home) home = getpwuid(getuid())->pw_dir;
```
`HOME` tidak di-set dan UID tidak ada di `/etc/passwd` → dereference null. `src/ConnRpc.cpp:200-204` sudah menangani kasus ini dengan benar, jadi perbaikannya tinggal meniru pola itu.

### L2 — Out-of-bounds read pada trailing backslash

**Tidak valid.**

`src/StratumClient.cpp:35-37` dan `:65-68` memang mengakses `json[end + 1]` tanpa cek batas. Tetapi `std::string::operator[](size())` dijamin mengembalikan referensi ke `'\0'` sejak C++11, jadi tidak ada pembacaan di luar buffer. Efeknya hanya menambahkan satu byte NUL ke nilai dan mengakhiri loop.

Dokumen sendiri menyebut bahwa indeks `json.size()` adalah terminator; kesimpulan "out-of-bounds" tidak mengikuti premis itu. Tidak perlu tindakan.

### L3 — Teks bantuan menyebut `rk_sysd`

**Valid.**

`src/Config.cpp:417`: `"Usage: rk_sysd [options]\n"`. Binary-nya `xcb`. Kosmetik.

### L4 — Nonce selalu mulai dari 0

**Valid, dampak rendah.**

`src/ConnMiner.cpp:226` `std::atomic<uint32_t> nonce{0}`, dan tidak ada penanganan extranonce di `ConnPool` maupun `ConnMiner`. Di jalur pool, blob tiap koneksi tampaknya berbeda (enam miner tidak saling menolak share sebagai duplikat), jadi ini terutama berlaku untuk solo RPC dengan beberapa mesin.

### L5 — `ctest` hanya membungkus `--selftest`

**Valid.**

Tidak ada perubahan sejak audit sebelumnya: satu test, `xcb-selftest`.

---

## Kutipan Kode yang Tidak Cocok

Kesimpulan audit sebagian besar benar, tetapi kutipan kodenya tidak dapat dipakai sebagai dasar patch:

| Lokasi di dokumen | Yang dikutip | Yang ada di `main` |
|---|---|---|
| H1 | `if (const char* t = std::getenv("XCB_TITLE"); t && *t)` | `if (const char* t = getenv("XCB_TITLE"))` (`main.cpp:196`) |
| H1 | `void set_process_title(...) noexcept` | Tanpa `noexcept` (`SelfDefense.cpp:61`) |
| H1 | `for (int i = 1; i < argc; ++i) argv[i] = nullptr;` | Tidak ada di kode asli |
| H2 | `(void)ptrace(PTRACE_TRACEME, ...)` "ignore return value" | Diperiksa, `_exit(1)` bila gagal (`SelfDefense.cpp:53-56`) |
| M5 | `Nice=-5` | Tidak ada di unit |
| M5 | "tanpa pembatasan hak akses" | Ada `NoNewPrivileges=true` |
| M4 | `out=$(run_apply ...) \|\| true` | Tidak ada `\|\| true`; penyebabnya `set -e` tidak dipakai |
| Positif #1 | `ensureSeed()`, `m_engine.reset()`, `m_engineGen`, `m_engineMu` | Tidak ada; padanan sekarang `buildEngine()`/`engineGen` (`ConnMiner.cpp:280`, `:243`) |
| Positif #2 | `sendRaw()`, `readLine()` | `sendLineLocked()` (`ConnPool.cpp:289`), `recvLine()` (`:317`) |
| Positif #4 | `BATCH_SIZE = 256` | `(i & 255)` (`ConnMiner.cpp:706`) |

Selain itu, nomor baris bergeser cukup jauh di beberapa tempat: `autoDetect()` ada di `Config.cpp:586`, bukan `607`; `set_process_title` di `SelfDefense.cpp:61`, bukan `61-90` dengan isi yang berbeda.

Catatan positif #1 tidak dapat dipakai sama sekali: ia menyebut simbol yang tidak ada di `main` maupun di `merge/fleet-main`, dan mendeskripsikan perilaku yang baru masuk lewat PR #17.

---

## Urutan Remediasi yang Disarankan

1. **H3** — satu-satunya temuan aktif yang memengaruhi pemilihan mode dan hugepages di produksi.
2. **M1 + M5** — sekaligus: baca `WALLET`/`WORKER`/`THREADS` di `applyEnvFlags()`, hapus `-u ${WALLET}` dari `ExecStart`, lalu tambahkan `User=` dan sandboxing.
3. **M4** — periksa exit code `apply.sh` dan tambahkan `trap` untuk `mktemp -d`.
4. **M6, L1, L3** — perbaikan kecil dan aman.
5. **H1, H2** — wajib diperbaiki sebelum `ENABLE_SELF_DEFENSE` dipakai di build mana pun.
6. **H4, L4** — opsional; tuning, bukan cacat.
7. **L2** — tidak perlu tindakan.
