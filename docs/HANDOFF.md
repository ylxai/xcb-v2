# Handoff — Agent Coding Berikutnya

Dokumen ini merangkum kondisi repo, keputusan operasional, dan pekerjaan yang
masih terbuka. Ini bukan pengganti `README.md`; baca juga:

- [`README.md`](../README.md) — fitur miner, build, dan deploy.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — alur branch/PR dan gaya commit.
- [`ops/fleet/README.md`](../ops/fleet/README.md) — deploy, rollback, monitoring.
- [`ops/systemd/README.md`](../ops/systemd/README.md) — unit systemd + environment.
- [`ops/instabox/README.md`](../ops/instabox/README.md) — deployment sandbox.
- [`docs/FLEET-TOPOLOGY.md`](FLEET-TOPOLOGY.md) — peran `fl` dan `security-learning`,
  host di baliknya, dan aturan build di `fl` (bukan lokal).

**Status terakhir:** 2026-11-06, `main` = `870433f` (merge PR #17).

---

## 1. Snapshot kondisi

| Item | Kondisi |
|---|---|
| `main` lokal | `870433f`, sinkron dengan `origin/main` |
| CI `main` | Semua job hijau: glibc, CONN, musl, artefak 22.04, ShellCheck (run `42162993415`) |
| PR #8–#17 | **MERGED** — Dependabot #8/#9, Spec Kit #11, audit #12–#16, page-mode #17 |
| PR terbuka | Tidak ada |
| Branch `chore/repo-hygiene` | Sudah dihapus (lokal, remote, dan di vps1) |
| Entire | Checkpoint tracking aktif; **0 checkpoint**; semua gate dinonaktifkan |
| Fleet produksi | 6 host `active`. `fl` menjalankan build page-mode `e9fd6e2`; 5 host lain build 2026-10-25. Rollout ditahan sampai observasi `fl` selesai |
| Host build/test | `fl` (`~/xcb-build`) untuk build + `ctest`; vps1 tetap fetch-only (`push` URL sengaja dimatikan) |

---

## 2. Aturan kerja yang wajib diikuti

1. **Jangan push langsung ke `main`.** Buat branch dari `main`, buka PR, tunggu
   CI hijau. Branch yang sudah merged boleh dihapus.
2. **Jangan build/test di laptop lokal.** Build dan test dilakukan di host
   build (vps1). Di vps1, `git push` sudah dimatikan; hanya fetch/build/test.
3. **Jangan ganggu fleet produksi.** Kalau butuh CPU penuh untuk build,
   hentikan service miner hanya sementara dan nyalakan lagi setelah selesai.
   Jangan pernah deploy binary build 24.04 ke worker 22.04.
4. **Jangan commit secret, wallet, alamat host internal, atau SSH key.**
   Konfigurasi host ada di `~/.config/xcb/fleet.hosts`; environment unit ada di
   `/etc/xcb/miner.env`. Keduanya tidak masuk repo.
5. **`AGENTS.md` adalah file lokal** (sudah di `.gitignore`). Jangan commit,
   jangan hapus, dan jangan menganggapnya dokumentasi proyek.
6. **Shell script wajib lolos ShellCheck style terendah:**

   ```bash
   git ls-files -z '*.sh' | xargs -0 -r shellcheck -S style
   ```

7. **Artefak deploy wajib dibangun di Ubuntu 22.04 / glibc 2.35.** Gunakan job
   CI `build-2204` atau build di host 22.04; jangan memakai binary host 24.04.

---

## 3. Arsitektur singkat

- **Bahasa/build:** C++17, CMake ≥ 3.12, OpenSSL, pthread.
- **Dua backend dalam satu binary:**
  - default `randomy` — Core Coin (XCB), library RandomY.
  - `-a conn` — ConnectCoin (CONN), RandomX V2, dataset besar / light mode.
- **Sumber utama:** `src/` (`Config`, `StratumClient`, `Miner`, `ConnPool`,
  `ConnRpc`, `ConnMiner`, `Stats`, `Dashboard`, `Submitter`, `Hashing/RandomXV2`).
- **Dependensi:** `external/RandomY` dan `external/FTXUI` adalah submodule;
  `external/randomx-v2` adalah vendored source.
- **Library runtime:** `librandomx_v2.so` harus berada di direktori yang sama
  dengan binary `xcb` karena RPATH `$ORIGIN`.
- **Config precedence (aktual):** **CLI > env `WALLET`+`POOL` > `pool.cfg`**.
  - Jalur env hanya dipakai kalau `WALLET` **dan** `POOL` sama-sama tidak kosong;
    kalau salah satu kosong/tidak ada, `pool.cfg` yang dimuat.
  - CLI diproses terakhir: `-u` menimpa wallet, sedangkan `-o` **menambahkan**
    pool baru (bukan mengganti pool yang sudah ada). Pool tambahan ikut failover
    setelah sekitar 3 kegagalan; kalau wallet-nya tidak di-set, login pool itu
    bisa gagal. Hindari mencampur `-o` dengan env/`pool.cfg` kecuali memang
    memahami perilaku ini.
  - Unit systemd memakai flag `-u/--worker/-t` eksplisit karena `WALLET` saja
    tidak mengaktifkan jalur env untuk backend CONN.

---

## 4. Build & test

Di host build (vps1). **Untuk memverifikasi PR, build commit PR — bukan `main`.**
Ganti `<branch-pr>` dan `<PR-head-sha>` dengan nilai PR yang sedang diuji:

```bash
git fetch origin --prune
git switch <branch-pr>        # atau: git fetch origin pull/<N>/head:pr-<N> && git switch pr-<N>
git pull --ff-only
test "$(git rev-parse HEAD)" = "<PR-head-sha>" || { echo "SALAH COMMIT"; exit 1; }

./setup.sh                  # deps + submodule + build + selftest
./build.sh --clean --no-test -j4
./build/xcb --selftest
(cd build && ctest --output-on-failure)
./build/xcb --version       # commit di output harus sama dengan PR head
```

Untuk baseline `main` saja (bukan verifikasi PR):

```bash
git switch main && git pull --ff-only
```

Cek tambahan:

```bash
git ls-files -z '*.sh' | xargs -0 -r shellcheck -S style
```

Catatan:

- `ctest` saat ini hanya membungkus `--selftest` (1 test). Belum ada unit test
  untuk parser config/stratum. Menambah test nyata adalah pekerjaan terbuka.
- Build Release memakai LTO dan `-march=x86-64-v3`; build Debug tidak.
- Ada warning non-fatal dari linker tentang `.note.GNU-stack` pada
  `jit_compiler_x86_static.S.o` (vendored RandomX) dan warning CMake
  `PUBLIC_HEADER` pada target `randomx_v2`. Bukan kegagalan build.
- Job CI glibc kadang butuh ~7 menit; itu normal.

Job CI yang harus hijau:

1. `ShellCheck`
2. `Build + selftest (glibc)`
3. `Build + selftest (CONN / RandomX V2)`
4. `Build + selftest (Alpine/musl)`
5. `Build + artefak deploy (Ubuntu 22.04 / glibc 2.35)`

---

## 5. Deploy, systemd, dan monitoring

### Fleet

- Skrip: `ops/fleet/deploy.sh`, `apply.sh`, `watch.sh`.
- Daftar host **tidak** di repo: `~/.config/xcb/fleet.hosts`.
- Alur deploy: canary host pertama → stop → backup binary + library → install →
  cek `ldd`/`--version` → start → tunggu `RandomX V2 siap` → rollback otomatis
  bila gagal.
- Backup tersimpan di samping binary: `xcb.bak-STAMP` dan
  `librandomx_v2.so.bak-STAMP`.
- Kondisi 2026-11-06: `fl` menjalankan build page-mode `e9fd6e2` (lokal, sebelum
  PR #17 merged). Build `870433f` sudah dibuat di `~/xcb-build/build` tetapi
  **belum dipasang**; lima worker lain masih build 2026-10-25.
- Contoh:

  ```bash
  cd ops/fleet
  ./deploy.sh --from-ci --expect <commit>
  ./deploy.sh --only <host> --dry-run
  ./deploy.sh --rollback
  ./watch.sh --dry-run
  ```

### systemd

- Template: `ops/systemd/syscached.service`.
- Environment: `/etc/xcb/miner.env` (mode 600, tidak di repo).
- `ExecStartPre` menolak `WALLET`/`WORKER` placeholder dan `CONN_POOL` kosong.
- Flag `--conn-pool`, `-u`, `--worker`, `-t` **sengaja eksplisit**; jangan
  menghapusnya saat “menyederhanakan” unit.
- `EnvironmentFile` sengaja tanpa prefix `-`: file hilang = service gagal start,
  bukan diam-diam memakai default.

### Instabox

- `ops/instabox/sandbox-deploy.sh` memakai unit dasar yang sama dengan
  `ops/systemd/syscached.service` + drop-in
  `syscached.service.d/10-instabox.conf` untuk `LD_PRELOAD` dan hugepages.
- Jangan menjalankan deploy instabox di host yang juga dikelola `ops/fleet`;
  drop-in akan mengubah environment service fleet. Hapus drop-in sebelum
  memakai deploy fleet biasa.
- `WALLET` wajib di-set; tidak ada default.

### Monitoring

- `watch.sh` mengecek service, HR, reject delta, reconnect/login gagal, TLS,
  dan fallback regular pages.
- Cron 5 menit dan channel alert (`ALERT_WEBHOOK` / Telegram) ada di
  `~/.config/xcb/watch.env`; **jangan berasumsi monitoring sudah aktif** —
  konfigurasi ini tidak ikut repo.

---

## 6. Entire (checkpoint & gates)

- `.entire/settings.json` + plugin OpenCode tetap ada dan aktif untuk checkpoint.
- Saat handoff ini ditulis: **0 checkpoint** di branch `main`.
- **Semua gate Entire sudah dinonaktifkan**:
  `approvals`, `base_checks`, `checks`, `findings`, `up_to_date`.
- `forgeCheckRunEnabled=false`, jadi check `Entire Gates` tidak lagi diposting;
  check yang lama berstatus `skipping`.
- Gate bawaan tidak bisa dihapus permanen lewat API
  (`built-in gates can't be deleted; disable the gate instead`), hanya
  dinonaktifkan.
- Kalau nanti gate ingin diaktifkan lagi: repo ini hanya punya satu akun
  (`ylxai`) dan gate `approvals` memakai `allow_self_approval=false`. Harus
  pilih: aktifkan self-approval atau tambahkan reviewer akun lain.
- **Peringatan checkpoint:** repo ini publik. Checkpoint berisi transkrip sesi
  agent; saat checkpoint dibuat/di-push, isinya bisa dibaca siapa pun. Tinjau
  `entire checkpoint explain` sebelum push, atau arahkan checkpoint ke repo
  privat / mode lokal.

---

## 7. Branch & PR yang masih ada

Remote branches:

| Branch | Status |
|---|---|
| `main` | aktif, `870433f` |
| `feat/yespower-ratr` | eksperimen, sengaja belum di-merge |
| `fix/src-runtime-hardening` | PR #1 sudah ditutup; branch lama |
| `ops/fleet-deploy-watch` | sudah merged ke `main` via PR #5; branch remote masih ada |
| `setup/entire-base` | sudah merged ke `main` via PR #6; branch remote masih ada |

Branch dependabot (`actions/checkout-7`, `actions/upload-artifact-7`) sudah
dihapus setelah PR #8/#9 merged.

Local clone masih menyimpan beberapa branch lama:

| Branch lokal | Status |
|---|---|
| `feat/config-parser-tests` | Spec Kit 001, paused, belum di-push |
| `feat/conn-address-required` | Spec Kit 002, paused, belum di-push |
| `feat/conn-randomx-v2` | upstream sudah hilang |
| `fix/conn-ssl-io-mutex` | sudah merged via PR #16, upstream sudah hilang |
| `merge/fleet-main` | tertinggal 73 commit dari `main` |
| `ops/fleet-deploy-watch` | sinkron dengan remote |

Jangan hapus branch yang belum dipastikan merged; gunakan `git branch -d`
(bukan `-D`) setelah memeriksa.

Tag yang ada: `v0.1`, `archive/vps1-2026-10-25`.

---

## 8. Pekerjaan berikutnya (prioritas)

1. **Verifikasi ganti seed page-mode di `fl`.** PR #17 (merged `870433f`) baru
   terbukti lewat harness. Pada ganti seed berikutnya log harus tetap
   `RandomX V2 siap (FAST, hugepages)`, tanpa `jatuh ke 4K page`, dan
   `HugePages_Free` kembali ke ~156 — bukan melompat ke 1200. Ganti seed
   mengikuti tinggi blok ≡ 64 (mod 12288), kira-kira tiap 34 jam.
2. **Deploy `870433f` ke lima worker lain.** Masih build 2026-10-25 (tanpa kolom
   `S`, `stale share` ~8,5%). Rollout ditahan selama `fl` diamati. Jalur deploy
   harus dari jump host: `fleet.hosts` di `fl` masih menunjuk `ai-workers-1..4`
   yang sudah mati, dan key `fl` tidak diotorisasi di worker.
3. **H4 — TLS untuk `StratumClient`.** Backend XCB masih `connect()` polos dan
   mengirim wallet dalam cleartext. Butuh keputusan: implementasi TLS atau catat
   sebagai risiko yang diterima.
4. **systemd hardening.** `ops/systemd/syscached.service` belum punya `User=`,
   jadi miner jalan sebagai root di 6 host. Perlu uji per host karena
   menyangkut `/etc/xcb/miner.env`, `LD_PRELOAD`, dan akses hugepage.
5. **`deploy.sh` — deteksi sukses.** `ops/fleet/deploy.sh:222` masih menyimpulkan
   sukses dari `grep -q 'HASIL=OK'` atas stdout+stderr, bukan exit code
   `apply.sh`.
6. **Tambah unit test nyata.** `ctest` baru membungkus `--selftest`. Spec Kit 001
   (parser config) dan 002 (alamat CONN wajib) ada di branch lokal dan belum
   di-push; konflik test keduanya belum diselesaikan.
7. **Aktifkan monitoring produksi.** Pasang cron `watch.sh` dan isi channel
   alert di `~/.config/xcb/watch.env`; uji dengan `--test-alert`.
8. **Release pipeline.** Belum ada build tag + checksum + retensi artefak
   panjang. Artefak CI saat ini hanya disimpan 14 hari.
9. **Housekeeping branch.** Hapus branch remote yang sudah merged/abandoned
   setelah dipastikan tidak ada Trail yang masih membutuhkannya.
10. **Entire checkpoint pertama.** Jika checkpoint ingin dipakai, mulai sesi
    OpenCode dari root repo, lakukan commit nyata, lalu tinjau checkpoint sebelum
    push (ingat repo publik).

---

## 9. Checklist sebelum menyerahkan pekerjaan

- [ ] Branch dibuat dari `main`, bukan push langsung.
- [ ] `git status` bersih; tidak ada file lokal/`AGENTS.md` yang ikut ter-commit.
- [ ] `shellcheck -S style` bersih.
- [ ] Build + `--selftest` + `ctest` lolos di host build.
- [ ] CI PR hijau: 5 job build/selftest + ShellCheck (bot review berjalan di sampingnya).
- [ ] Tidak ada wallet/secret/IP internal baru di diff.
- [ ] Dokumentasi/README diperbarui bila perilaku berubah.
- [ ] PR body berisi ringkasan, verifikasi, risiko, dan rollback.
- [ ] Branch/remote yang sudah merged dihapus bila tidak diperlukan.

---

## 10. Jika ada yang tidak jelas

Jangan menebak konfigurasi produksi. Periksa `~/.config/xcb/fleet.hosts`,
`~/.config/xcb/watch.env`, dan unit yang benar-benar terpasang di host
(`systemctl cat syscached`). Untuk perubahan yang menyentuh deploy, jalankan
`--dry-run` lebih dulu dan pastikan canary host pertama sehat.
