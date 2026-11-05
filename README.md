# xcb — Miner XCB (RandomY) + ConnectCoin (CONN / RandomX V2)

[![CI](https://github.com/ylxai/xcb-v2/actions/workflows/ci.yml/badge.svg)](https://github.com/ylxai/xcb-v2/actions/workflows/ci.yml)

Miner C++ pribadi untuk **Core Coin (XCB)** menggunakan algoritma **RandomY** (fork RandomX v1.2.1),
plus backend **ConnectCoin (CONN)** dengan RandomX V2. Dibangun dari nol — **zero dev fee**,
dependensi eksternal hanya RandomY, RandomX V2 (vendored), FTXUI, dan OpenSSL.

- **Zero dev fee** — semua hash 100% ke wallet yang dikonfigurasi, tidak ada switch wallet
- **ETHPROXY protocol** (`eth_submitLogin` / `eth_getWork` / `eth_submitWork`) untuk pool catchthatrabbit
- **Light mode** (256MB cache) atau **full mode** (264MB dataset), JIT compiler, huge pages auto-detect
- **CPU affinity + nice priority** — thread di-pin per core
- **Multi-pool config file** (`pool.cfg`) — failover otomatis antar server
- **Dashboard TUI (FTXUI)** — live stats interaktif, keyboard-driven (1/2/3 pindah tab, q quit)

---

## Docker Image

Image resmi di **Docker Hub**: [`ylxai/xcb`](https://hub.docker.com/r/ylxai/xcb)

```bash
docker pull ylxai/xcb:v1

# WALLET wajib; pool default sg
docker run --rm -e WALLET=<alamat_wallet> ylxai/xcb:v1

# Jalankan dengan override penuh
docker run --rm -d --name xcb \
  -e WALLET=<alamat_wallet> \
  -e POOL=sg.catchthatrabbit.com:8008 \
  -e WORKER=myworker \
  -e THREADS=4 \
  ylxai/xcb:v1
```

> **Wallet wajib di-set.** Image tidak punya wallet default; tanpa
> `-e WALLET=<alamat>` miner berhenti dengan `No wallet configured`. Ini sengaja
> agar image publik tidak pernah menambang ke alamat pembuat image.
>
> **Memori & huge pages: otomatis.** Miner membaca RAM tersedia (termasuk cgroup
> limit container) dan jumlah huge page bebas, lalu memilih sendiri: `FULL_MEM=1`
> kalau ada ≥3GB, `FULL_MEM=0` (light, ~256MB) kalau tidak; `LARGE_PAGES` hanya
> menyala kalau huge page-nya memang cukup. Pilihannya dicetak saat start:
> `[Config] auto FULL_MEM=0 (usable 512MB, ...)`.
> Set `-e FULL_MEM=1` atau `-e LARGE_PAGES=1` hanya kalau mau memaksa.

---

## Environment Variables

Semua env vars dibaca langsung (precedence ≥ config file & CLI):

| Variable | Default | Deskripsi |
|----------|---------|-----------|
| `WALLET` | — | Alamat wallet (wajib; tidak ada default di image) |
| `POOL` | `sg.catchthatrabbit.com:8008` | Host pool `host:port` |
| `WORKER` | `pool` | Nama worker (ditampilkan pool sebagai `wallet.worker`). `auto` = hostname mesin |
| `THREADS` | *(kosong)* | Jumlah thread. **Kosong = auto (jumlah CPU cores)** |
| `FULL_MEM` | *(auto)* | `1` = dataset full 264MB, `0`/`false` = light 256MB. Kosong = pilih otomatis dari RAM tersedia / cgroup limit (ambang 776MB) |
| `LARGE_PAGES` | *(auto)* | `1` = huge pages, `0`/`false` = memori biasa. Kosong = menyala hanya kalau huge page cukup |
| `LOG_SHARES` | *(false)* | `1` = print setiap share found/accepted (default quiet — pool difficulty rendah sangat noisy) |

---

## Build dari Source (Lokal)

### Persyaratan
- Linux x86_64 dengan **AES-NI + AVX2** (build memakai `-march=x86-64-v3`) atau aarch64 dengan crypto extensions
- CMake ≥ 3.12 (versi sistem yang lebih baru lebih baik — CMake ≥ 3.16 disarankan)
- Compiler C++17 (g++ ≥ 8 atau clang ≥ 7)
- OpenSSL dev headers (di-`find_package` oleh CMakeLists — wajib ada meski kode tidak memanggilnya)

### Build
```bash
git clone <url-repo> xcb
cd xcb
git submodule update --init --recursive   # Wajib: tarik RandomY + FTXUI

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
# Binary: build/xcb (stripped)
```

> Submodule **harus** di-init dulu sebelum `cmake ..` — kalau belum, CMake gagal
> dengan `RandomY not found` / `FTXUI not found`.

### Verifikasi Build
```bash
./build/xcb --selftest        # cek hash SHA3-512 vs vektor NIST/OpenSSL (30 cek)
./build/xcb --benchmark 10000 # smoke test tanpa pool, lalu Ctrl+C
```

### Proteksi Diri (Opsional, OFF default)

Build dengan anti-debugging + penyamaran proses:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_SELF_DEFENSE=ON
cmake --build build --parallel "$(nproc)"
```

Yang aktif di binary hasil build ini:

- **Deteksi debugger** — `TracerPid != 0` (strace/gdb sudah menempel) → proses
  keluar sebelum sempat membaca config/wallet dari memori.
- **Anti-dump** — `PR_SET_DUMPABLE=0` + `RLIMIT_CORE=0`: `/proc/<pid>/mem`
  dan `/proc/<pid>/environ` tidak bisa dibaca proses lain, tidak ada core file.
- **Anti-ptrace** — `PTRACE_TRACEME` dikunci: `strace -p <pid>` dan
  `gdb -p <pid>` ditolak kernel (`Operation not permitted`).
- **Title spoof (opsional)** — env `XCB_TITLE=nama` mengganti nama proses di
  `ps` (comm) dan menghapus cmdline asli (host/wallet/pool tidak terlihat):

```bash
XCB_TITLE=xd ./build/xcb -o pool:port -u wallet.worker
ps aux | grep xd   # proses tampil sebagai "xd", tanpa argumen
```

Catatan: proteksi ini menargetkan pengintaian tingkat userland. Kernel tetap
mengetahui prosesnya (root tetap bisa melihat/menghentikan). Build normal
(tanpa flag) tetap bersih dan tidak mengandung kode ini.

### Lab Studi Keamanan

- `labs/syscall-hook/` — hooking syscall: ptrace interceptor, seccomp user
  notification, LD_PRELOAD interposisi, kprobe tracefs (tanpa LKM).
- `labs/process-hide/` — menyembunyikan `/proc/<pid>` dari `ps`/`pgrep`/`ls`
  via LD_PRELOAD (interposisi `readdir`/`getdents64`). Batas tekniknya
  didokumentasikan di `labs/process-hide/README.md` — ini userland hiding,
  bukan stealth kernel.
- `labs/polymorphic/` — generator polymorphic binary: tiap run menghasilkan
  varian dengan sha256 berbeda tapi fungsi identik (enkripsi key stream acak
  + stub assembly yang di-generate ulang: register/body/counter/NOP berbeda).
- `labs/elfpack/` — ELF runtime packer gaya UPX untuk binary asli: mode
  static (in-memory loader assembly: mmap/decode/mprotect/auxv rewrite/jmp)
  dan mode dynamic (zlib + temp file + fork/execve, untuk xcb). xcb 830 KB
  -> 352 KB, output & selftest identik. Batas tekniknya di README lab.

`--selftest` wajib lolos sebelum mining: ia memverifikasi implementasi keccak
hot-path (`len%72==71` dan kasus edge lain yang pernah salah di picosha3).

---

## Cara Pakai (Local Binary)

```bash
./xcb                        # auto threads, lihat pool.cfg / env
./xcb -t 4                   # 4 thread
./xcb --light -t 1           # 1 thread light mode
./xcb -o pool.lain.com:8008 -u wallet.worker   # override pool+wallet
```

### Auto-setup hugepages: `./mine.sh`
Cek CPU/RAM, set `vm.nr_hugepages` + `vm.max_map_count`, beri izin mlock (setcap), lalu jalankan miner — tanpa perlu hafal command sysctl:

```bash
./mine.sh                 # full dataset, threads = semua core
./mine.sh --light         # hemat RAM (~256MB)
./mine.sh --persist       # tulis /etc/sysctl.d agar hugepages tetap setelah reboot
./mine.sh --benchmark 100000    # arg lain diteruskan ke miner
```

Idempotent: sysctl/setcap hanya dijalankan bila belum cukup. Butuh sudo sekali untuk sysctl & setcap.

### Konfigurasi `pool.cfg`
File `pool.cfg` (cwd, `/etc/syscache/pool.cfg`, atau `~/xcb/pool.cfg` — dicek otomatis) mendukung multi-server failover:

```ini
wallet=alamat_wallet_anda
worker=auto
server[1]=sg.catchthatrabbit.com
port[1]=8008
server[2]=hk.catchthatrabbit.com
port[2]=8008
server[3]=de.catchthatrabbit.com
port[3]=8008
threads=4
light=true
```

`worker=auto` memakai **hostname mesin** sebagai nama worker, jadi satu
`pool.cfg` bisa dipakai di beberapa mesin (devbox, VPS, laptop) tanpa diubah dan
tiap mesin tetap terlihat terpisah di dashboard pool. Wallet tidak terpengaruh.
Hostname dibersihkan dulu — hanya huruf/angka/`-`/`_`, dipotong di titik pertama
(`mesin.domain.tld` → `mesin`, karena titik adalah pemisah `wallet.worker`),
maksimal 32 char, dan jatuh ke `worker` bila tidak ada karakter yang sah. Nilai
`auto` juga berlaku lewat `WORKER=auto` maupun `-u wallet.auto`.

> **Wallet wajib diisi.** `pool.cfg` di repo sengaja memakai `wallet=` kosong;
> isi dengan alamat Anda atau lewatkan `-u <alamat>` saat menjalankan miner.
>
> Precedence: **env vars > pool.cfg > CLI flags**. `WALLET`+`POOL` env aktif = pool.cfg diabaikan.
> Commands flag: `-o host:port`, `-u wallet[.worker]`, `-p password`, `-t N`, `--light`, `--no-jit`, `--ui=MODE`, `-h`.

---

## ConnectCoin (CONN) — RandomX V2

Backend kedua di binary yang sama: **ConnectCoin (CONN)** memakai RandomX V2
(dataset 2 GiB, VM 384 instruksi) dengan key epoch dari `getblocktemplate`.
Pilih backend lewat `-a`.

```bash
# Pool stratum CONN (TLS)
./xcb -a conn --conn-pool eu.rplant.xyz:17194 \
      -u <alamat_wallet_conn> --worker "$(hostname)" -t 4

# Solo lewat node lokal (cookie RPC dibaca otomatis)
./xcb -a conn -u cc1... -t 4
./xcb -a conn --conn-rpc http://127.0.0.1:48172 --conn-user u --conn-pass p
```

| Flag / env | Arti |
|---|---|
| `-a conn` | pilih backend CONN (default `randomy`) |
| `--conn-pool host:port` | pool stratum CONN (TLS) |
| `--conn-rpc URL` | RPC `connectcoind` untuk mode solo (default `http://127.0.0.1:48172`) |
| `--conn-user` / `--conn-pass` | kredensial RPC kalau tidak memakai cookie |
| `--conn-cookie PATH` | cookie RPC (default `~/.connectcoin/.cookie`) |
| `CONN_TLS_INSECURE=1` | matikan verifikasi sertifikat pool (self-signed) |

Catatan operasional:

- **TLS diverifikasi penuh secara default** (sertifikat + hostname). Pool dengan
  sertifikat self-signed harus memakai `CONN_TLS_INSECURE=1`; miner menulis
  peringatan eksplisit di log saat mode itu aktif.
- Reconnect memakai backoff 2→30 detik. Delay baru di-reset setelah sesi
  bertahan **≥60 detik**, jadi pool yang menerima login lalu langsung menutup
  koneksi tidak memicu reconnect loop tiap 2 detik.
- DNS dicoba berurutan (IPv6/IPv4); kalau hasil pertama tidak reachable,
  miner lanjut ke hasil berikutnya.
- Binary membutuhkan **`librandomx_v2.so` di direktori yang sama** (RPATH
  `$ORIGIN`). `cmake --install` menaruh `xcb` + library di `bin/`, dan
  Dockerfile menyalin keduanya ke `/miner`.
- Dataset FAST butuh ~2 GiB RAM per proses (bukan per thread); `--light`
  memakai cache ~256 MB dengan hashrate lebih rendah.
- `--version` mencetak backend + commit, mis.
  `xcb 1.0.0 (ConnectCoin CONN - RandomX V2, 62186fd)`.

---

## Dashboard / UI Mode

Secara default di terminal (TTY) miner menampilkan **dashboard FTXUI** full-screen yang hidup:

| Tab Overview — benchmark progress + hashrate | Tab Threads — rate & share per worker |
|-----------------------------------------------|---------------------------------------|
| ![Overview](docs/screenshots/dashboard-overview.png) | ![Threads](docs/screenshots/dashboard-threads.png) |

| Tab Shares — statistik share + event + rate history |
|-----------------------------------------------------|
| ![Shares](docs/screenshots/dashboard-shares.png) |

> Screenshot diambil dari mode `--benchmark` (tanpa pool). Di mode mining biasa,
> tab Overview menampilkan **sparkline hashrate live** (48 detik) alih-alih gauge benchmark.

**Keyboard:**

| Tombol | Aksi |
|--------|------|
| `1` | Tab **Overview** — hashrate cur/avg/best + sparkline + ringkasan |
| `2` | Tab **Threads** — hashrate & share per worker |
| `3` | Tab **Shares** — statistik share lengkap + event + rate history |
| `q` / `x` / `Esc` | Keluar (Ctrl+C juga berfungsi) |

**Mode via `--ui=`:**

| Mode | Kapan | Deskripsi |
|------|-------|-----------|
| `auto` *(default)* | TTY | FTXUI di terminal, log line jika bukan TTY (docker/CI) |
| `ftxui` | TTY | Paksa dashboard FTXUI |
| `ansi` | TTY | Dashboard ANSI sederhana (redraw in-place, tanpa keyboard) |
| `log` | di mana saja | Hanya log line (one-liner tiap 5s + detail tiap 60s) |

> Mode `auto` sudah aman untuk docker/CI: tanpa TTY otomatis jatuh ke log line,
> dan `LOG_FORMAT=json` menghasilkan satu baris JSON per sample untuk monitoring.

---

## Docker (Build Image Sendiri)

```bash
cd xcb
git submodule update --init --recursive
docker build -t ylxai/xcb:v1 .
docker run --rm -e WALLET=<alamat_wallet> ylxai/xcb:v1
```

Image docker **multi-stage** (builder → runtime minimal), jalan sebagai user non-root
`sysd`, entrypoint `./xcb`. `pool.cfg` ikut di-copy ke `/etc/syscache/pool.cfg`
sebagai fallback, tetapi wallet di file contoh sengaja kosong — isi dulu atau
pakai `-e WALLET`.

---

## Operasi & Deployment

Fleet miner (CONN) dikelola dengan skrip di [`ops/fleet/`](ops/fleet/README.md).
Konfigurasi host **tidak** disimpan di repo publik; simpan di
`~/.config/xcb/fleet.hosts`.

```bash
cd ops/fleet
./deploy.sh --from-ci --expect <commit>   # artefak Ubuntu 22.04 dari CI
./deploy.sh --only fl --dry-run           # lihat rencana tanpa menyentuh host
./deploy.sh --rollback                    # kembali ke backup terbaru
./watch.sh --dry-run                      # cek monitoring tanpa kirim alert
```

Artefak deploy wajib dibangun di **Ubuntu 22.04 / glibc 2.35** karena worker
memakai 22.04. Jangan memakai binary hasil build host 24.04.

Unit systemd contoh ada di [`ops/systemd/`](ops/systemd/README.md); monitoring
cron, threshold, dan alert dijelaskan di [`ops/fleet/README.md`](ops/fleet/README.md).

---

## Kubernetes / Akash

### Akash SDL
```yaml
version: "2.0"
services:
  service-1:
    image: ylxai/xcb:v1
    env:
      - WALLET=<alamat_wallet>
      - POOL=sg.catchthatrabbit.com:8008
      - WORKER=akash
      - THREADS=16        # harus ≤ cpu.units
      # FULL_MEM / LARGE_PAGES tidak perlu di-set: miner membaca memori
      # yang dialokasikan provider dan memilih light/full sendiri.
    expose:
      - port: 80
        as: 80
        to:
          - global: true
profiles:
  compute:
    service-1:
      resources:
        cpu:
          units: 16
        memory:
          size: 8Gi
        storage:
          - size: 5Gi
  placement:
    dcloud:
      pricing:
        service-1:
          denom: uact
          amount: 100000
deployment:
  service-1:
    dcloud:
      profile: service-1
      count: 1
```

> `expose` port 80 sebenarnya tidak dipakai (tidak ada web server) — boleh dihapus.

### Kubernetes (Deployment + Secret)
Contoh lengkap ada di [`k8s/deployment.yaml`](k8s/deployment.yaml):
```bash
kubectl create namespace mining
read -r -s -p "Wallet: " WALLET; echo
printf '%s' "$WALLET" > /tmp/xcb-wallet.txt; unset WALLET
kubectl -n mining create secret generic xcb-wallet \
  --from-file=wallet=/tmp/xcb-wallet.txt
rm -f /tmp/xcb-wallet.txt
kubectl apply -f k8s/deployment.yaml
kubectl logs -n mining deploy/xcb-miner -f
```
Wallet disimpan sebagai **Secret** dan tidak ditulis di manifest. **`THREADS` wajib =
`limits.cpu`** (default di kode = `nproc` NODE, bukan limit pod — salah set akan oversubscribe).

---

## Benchmark

| Thread | Mode | Hashrate | RAM |
|--------|------|----------|-----|
| 1 | Light | ~68 H/s | ~256MB |
| 2 | Light | ~122 H/s | ~256MB |
| 4 | Light | **~291 H/s** | ~256MB |
| 16 | Light | ~1.26 KH/s | ~256MB |
| 16 | **Full** | **~7.9 KH/s** | ~264MB |

>  Full mode ±6x hashrate light mode (diverifikasi di VPS 16 core, 8GB RAM).
>
>  RandomY bukan RandomX: `DATASET_BASE_SIZE` 256MiB + extra 8MiB, jadi dataset
>  penuh hanya 264MB — bukan 2.6GB seperti RandomX. Puncak pemakaian ada saat
>  init (dataset 264MB + cache 256MB hidup bersamaan, lalu cache dilepas):
>  terukur RSS 524MB, dan mode full masih jalan di cgroup 536MB. Setelah init
>  turun ke ~266MB. Jumlah thread tidak menambah RAM — scratchpad RandomY hanya
>  128KiB per VM.

---

## Optimasi untuk Mesin Sendiri

```bash
# Huge pages (2MB) — cukup pakai ./mine.sh (set sysctl + mlock otomatis):
#   ./mine.sh --persist     # sekalian tahan reboot
# Manual (kalau mau):
sudo sysctl vm.nr_hugepages=1280

# CPU performance governor
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Realtime priority
sudo chrt -rr 1 ./xcb
```

---

## Logging

- **Non-TTY / `--ui=log`** — one-liner tiap 5 detik (`HR cur (avg) | A n R n W n | diff | pool | job`) + tabel detail tiap 60 detik
- **Shares accepted** — per-share jika `LOG_SHARES=1`, selain itu level debug
- **Rejected / wasted** — selalu ditampilkan (dengan alasan)
- **`LOG_FORMAT=json`** — satu objek JSON per baris (hashrate, A/R/W, diff, pool, job) untuk docker/CI
- **`LOG_LEVEL`** — `debug | info | warn | error` (default `info`)

---

## Troubleshooting

| Gejala | Penyebab | Solusi |
|--------|----------|--------|
| `cache alloc failed` | Huge pages / RAM tidak cukup **dan** `LARGE_PAGES`/`FULL_MEM` dipaksa via env | Lepas env-nya — auto-detect memilih light + tanpa huge pages sendiri |
| Crash `stoul` / `Config` gagal | Env var kosong (`-e THREADS=`) | Biarkan env kosong atau isi nilai valid — versi v1 sudah menangani string kosong |
| Cuma 1 thread padahal banyak core | Default kode lama `threads=1` | Set `THREADS=<n>` atau gunakan v1 (auto = semua cores) |
| Hashrate ~50% dari harapan | Auto-detect memilih light karena RAM < 3GB (lihat `[Config] auto FULL_MEM=0`) | Tambah RAM/limit, atau paksa `FULL_MEM=1` kalau yakin cukup |
| Worker stuck (0.0x H/s) | Core sibuk/di luar cpuset provider (CPU affinity) | Cek `lscpu`/`Cpus_allowed_list`; jalankan berkali atau kurangi THREADS |
| Build gagal `RandomY` tidak ditemukan | Submodule belum di-init | `git submodule update --init --recursive` |
| Log kapah (ribuan share/detik) | Pool difficulty rendah + log per-share | Default v1 quiet; aktifkan verbose hanya untuk debug (`LOG_SHARES=1`) |

---

## Struktur Proyek

```
xcb/
├── CMakeLists.txt           # Build system (-march=x86-64-v3 / armv8-a+crypto, -O3, LTO, stripped)
├── Dockerfile               # Multi-stage: builder → runtime minimal, user non-root
├── build.sh / setup.sh      # Build cepat / setup lengkap (deps + submodule + selftest)
├── mine.sh                  # Auto hugepages + mlock + jalankan miner
├── pool.cfg                 # Multi-server config (wallet + 3 pool failover)
├── CONTRIBUTING.md          # Alur branch, build/test, gaya commit
├── .github/
│   ├── workflows/ci.yml     # CI: glibc, CONN, musl, Docker, artefak 22.04, ShellCheck
│   ├── dependabot.yml       # Update actions + submodule
│   └── pull_request_template.md
├── .entire/                 # Konfigurasi checkpoint Entire
├── .opencode/plugins/       # Plugin Entire untuk OpenCode
├── ops/
│   ├── fleet/               # deploy, rollback, monitoring fleet
│   ├── instabox/            # kontrol sandbox instabox
│   └── systemd/             # unit systemd + contoh environment
├── labs/                    # Lab studi keamanan (rootkit, hook, packer)
├── docs/
│   └── screenshots/         # Screenshot dashboard (README)
├── k8s/
│   └── deployment.yaml      # Deployment contoh (wallet lewat Secret)
├── external/
│   ├── RandomY/             # RandomY library (submodule, BSD 3-Clause)
│   ├── randomx-v2/          # RandomX V2 untuk CONN (vendored, BSD 3-Clause)
│   └── FTXUI/               # Terminal TUI library (submodule, MIT)
└── src/
    ├── main.cpp             # Entrypoint + signal handler + selftest
    ├── Config.hpp/.cpp      # Config parser (env vars + file + CLI)
    ├── StratumClient.hpp/.cpp # ETHPROXY protocol client
    ├── Miner.hpp/.cpp       # Thread pool, VM management, mining loop
    ├── Stats.hpp/.cpp       # Telemetri: rolling hashrate, sparkline, summary, dashboard
    ├── Dashboard.hpp/.cpp   # TUI FTXUI full-screen (tab Overview/Threads/Shares)
    ├── Log.hpp/.cpp         # Logger thread-safe (level, warna, JSON)
    ├── Submitter.hpp/.cpp   # Async share submitter (bounded queue)
    ├── ConnPool.hpp/.cpp    # Stratum CONN (TLS + verifikasi + backoff)
    ├── ConnRpc.hpp/.cpp     # JSON-RPC connectcoind (solo) + parser JSON
    ├── ConnMiner.hpp/.cpp   # Backend CONN: template loop + worker RandomX V2
    ├── Hashing/RandomXV2.*  # Wrapper Context/VM RandomX V2
    └── Sha3_512.hpp/.cpp    # SHA3-512 keccak satu-blok (hot path 40B)
```

---

## Lisensi

Kode ini dilisensikan di bawah **MIT License** — lihat [LICENSE](LICENSE).
RandomY library © RandomX contributors — [BSD 3-Clause](external/RandomY/LICENSE).
FTXUI © ArthurSonzogni — [MIT](external/FTXUI/LICENSE).

Kontribusi, aturan branch/PR, dan perintah verifikasi ada di
[CONTRIBUTING.md](CONTRIBUTING.md).
