# Handoff — Agent Coding Berikutnya

Dokumen ini merangkum kondisi repo, keputusan operasional, dan pekerjaan yang
masih terbuka. Ini bukan pengganti `README.md`; baca juga:

- [`README.md`](../README.md) — fitur miner, build, Docker, k8s.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — alur branch/PR dan gaya commit.
- [`ops/fleet/README.md`](../ops/fleet/README.md) — deploy, rollback, monitoring.
- [`ops/systemd/README.md`](../ops/systemd/README.md) — unit systemd + environment.
- [`ops/instabox/README.md`](../ops/instabox/README.md) — deployment sandbox.

**Status terakhir:** 2026-11-05, `main` = `55f7207` (merge PR #7).

---

## 1. Snapshot kondisi

| Item | Kondisi |
|---|---|
| `main` lokal | `55f7207`, sinkron dengan `origin/main` |
| CI `main` | Semua job hijau: glibc, CONN, musl, Docker, artefak 22.04, ShellCheck |
| PR #7 | **MERGED** (`chore: rapikan struktur repo, CI, dan konvensi kontribusi`) |
| PR terbuka | #8 dan #9 (Dependabot, bump GitHub Actions) |
| Branch `chore/repo-hygiene` | Sudah dihapus (lokal, remote, dan di vps1) |
| Entire | Checkpoint tracking aktif; **0 checkpoint**; semua gate dinonaktifkan |
| Fleet produksi | Semua host `active`; jangan diubah tanpa alasan operasional |
| Host build/test | vps1, fetch-only (`push` URL sengaja dimatikan) |

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
- **Docker:** image tidak punya wallet default. `docker run` wajib
  `-e WALLET=<alamat>`; `POOL` punya default di image.
- **Kubernetes:** wallet dibaca dari Secret `xcb-wallet`; manifest tidak
  menyimpan Secret. Lihat [`k8s/deployment.yaml`](../k8s/deployment.yaml).

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
5. `Docker image + selftest`
6. `Build + artefak deploy (Ubuntu 22.04 / glibc 2.35)`

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
| `main` | aktif, `55f7207` |
| `dependabot/github_actions/actions/checkout-7` | PR #8, terbuka |
| `dependabot/github_actions/actions/upload-artifact-7` | PR #9, terbuka |
| `feat/yespower-ratr` | eksperimen, sengaja belum di-merge |
| `fix/src-runtime-hardening` | PR #1 sudah ditutup; branch lama |
| `ops/fleet-deploy-watch` | sudah merged ke `main` via PR #5 |
| `setup/entire-base` | sudah merged ke `main` via PR #6 |

Local clone masih menyimpan beberapa branch lama (`feat/conn-randomx-v2`,
`merge/fleet-main`, `ops/fleet-deploy-watch`, dan lainnya). Jangan hapus branch
yang belum dipastikan merged; gunakan `git branch -d` (bukan `-D`) setelah
memeriksa.

Tag yang ada: `v0.1`, `archive/vps1-2026-10-25`.

---

## 8. Pekerjaan berikutnya (prioritas)

1. **Review Dependabot PR #8 dan #9.** Cek release notes `actions/checkout` dan
   `actions/upload-artifact`, pastikan runner/Node compatibility, lalu tunggu
   CI hijau sebelum merge.
2. **Tambah unit test nyata.** `ctest` baru membungkus `--selftest`; belum ada
   test untuk parser `pool.cfg`/env, validasi wallet, dan parsing stratum.
3. **Aktifkan monitoring produksi.** Pasang cron `watch.sh` dan isi channel
   alert di `~/.config/xcb/watch.env`; uji dengan `--test-alert`.
4. **Release pipeline.** Belum ada build tag + checksum + retensi artefak
   panjang. Artefak CI saat ini hanya disimpan 14 hari.
5. **Docker image multi-arch (opsional).** CI saat ini hanya build satu
   arsitektur, sedangkan komentar di `k8s/deployment.yaml` menyebut Docker Hub
   multi-arch. Kalau multi-arch memang dibutuhkan, tambahkan
   `docker/build-push-action`/buildx; kalau tidak, perbaiki komentar tersebut.
6. **Housekeeping branch.** Hapus branch remote yang sudah merged/abandoned
   setelah dipastikan tidak ada Trail yang masih membutuhkannya.
7. **Entire checkpoint pertama.** Jika checkpoint ingin dipakai, mulai sesi
   OpenCode dari root repo, lakukan commit nyata, lalu tinjau checkpoint sebelum
   push (ingat repo publik).

---

## 9. Checklist sebelum menyerahkan pekerjaan

- [ ] Branch dibuat dari `main`, bukan push langsung.
- [ ] `git status` bersih; tidak ada file lokal/`AGENTS.md` yang ikut ter-commit.
- [ ] `shellcheck -S style` bersih.
- [ ] Build + `--selftest` + `ctest` lolos di host build.
- [ ] CI PR hijau: 6 job, termasuk ShellCheck.
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
