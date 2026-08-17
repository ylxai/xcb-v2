# miner-saya — Core Coin (XCB) RandomY Miner

Miner C++ pribadi untuk **Core Coin (XCB)** menggunakan algoritma **RandomY** (fork RandomX v1.2.1).
Dibangun dari nol — **zero dev fee**, dependensi eksternal hanya RandomY + FTXUI.

- **Zero dev fee** — semua hash 100% ke wallet yang dikonfigurasi, tidak ada switch wallet
- **ETHPROXY protocol** (`eth_submitLogin` / `eth_getWork` / `eth_submitWork`) untuk pool catchthatrabbit
- **Light mode** (256MB cache) atau **full mode** (~2.6GB dataset), JIT compiler, huge pages auto-detect
- **CPU affinity + nice priority** — thread di-pin per core
- **Multi-pool config file** (`pool.cfg`) — failover otomatis antar server
- **Dashboard TUI (FTXUI)** — live stats interaktif, keyboard-driven (1/2/3 pindah tab, q quit)

---

## Docker Image

Image resmi di **Docker Hub**: [`ylxai/xcb`](https://hub.docker.com/r/ylxai/xcb)

```bash
docker pull ylxai/xcb:v1

# Jalankan cepat (env dari image default: wallet + pool sg)
docker run --rm ylxai/xcb:v1

# Jalankan dengan override penuh
docker run --rm -d --name xcb \
  -e WALLET=<alamat_wallet> \
  -e POOL=sg.catchthatrabbit.com:8008 \
  -e WORKER=myworker \
  -e THREADS=4 \
  -e FULL_MEM=0 \
  -e LARGE_PAGES=0 \
  ylxai/xcb:v1
```

> **⚠️ Wajib di container:** `LARGE_PAGES=0` (container tanpa hugepages/mlock gagal alloc cache).
> **Disarankan:** `FULL_MEM=0` (light, 256MB) — full mode butuh ~2.6GB RAM.

---

## Environment Variables

Semua env vars dibaca langsung (precedence ≥ config file & CLI):

| Variable | Default | Deskripsi |
|----------|---------|-----------|
| `WALLET` | *(dari image)* | Alamat wallet Core Coin (XCB) |
| `POOL` | `sg.catchthatrabbit.com:8008` | Host pool `host:port` |
| `WORKER` | `pool` | Nama worker (ditampilkan pool sebagai `wallet.worker`) |
| `THREADS` | *(kosong)* | Jumlah thread. **Kosong = auto (jumlah CPU cores)** |
| `FULL_MEM` | *(true)* | `1` = dataset full 2.6GB, `0`/`false` = light 256MB |
| `LARGE_PAGES` | *(true)* | `1` = huge pages, **`0`/`false` wajib di container** |
| `LOG_SHARES` | *(false)* | `1` = print setiap share found/accepted (default quiet — pool difficulty rendah sangat noisy) |

---

## Build dari Source (Lokal)

### Persyaratan
- Linux x86_64 dengan **AES-NI + AVX2** (build memakai `-march=x86-64-v3`) atau aarch64 dengan crypto extensions
- CMake ≥ 3.10
- Compiler C++17 (g++ ≥ 8 atau clang ≥ 7)
- OpenSSL dev headers

### Build
```bash
git clone <url-repo> xcb
cd xcb
git submodule update --init --recursive   # Wajib: tarik RandomY + FTXUI

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Binary: build/miner-saya (stripped)
```

---

## Cara Pakai (Local Binary)

```bash
./miner-saya                        # auto threads, lihat pool.cfg / env
./miner-saya -t 4                   # 4 thread
./miner-saya --light -t 1           # 1 thread light mode
./miner-saya -o pool.lain.com:8008 -u wallet.worker   # override pool+wallet
```

### Konfigurasi `pool.cfg`
File `pool.cfg` (cwd, `/miner/pool.cfg`, atau `~/xcb/pool.cfg` — dicek otomatis) mendukung multi-server failover:

```ini
wallet=alamat_wallet_anda
worker=pool
server[1]=sg.catchthatrabbit.com
port[1]=8008
server[2]=hk.catchthatrabbit.com
port[2]=8008
server[3]=de.catchthatrabbit.com
port[3]=8008
threads=4
light=true
```

> Precedence: **env vars > pool.cfg > CLI flags**. `WALLET`+`POOL` env aktif = pool.cfg diabaikan.
> Commands flag: `-o host:port`, `-u wallet[.worker]`, `-p password`, `-t N`, `--light`, `--no-jit`, `--ui=MODE`, `-h`.

---

## Dashboard / UI Mode

Secara default di terminal (TTY) miner menampilkan **dashboard FTXUI** full-screen yang hidup:

```
┌───────────────────────────────────────────────────────────────┐
│ miner-saya v2 | RandomY | pool sg.catch... | wallet XCB... │
│ diff 1.2M | job abc123 | uptime 00:12:34                     │
├───────────────────────────────────────────────────────────────┤
│ 1 Overview   2 Threads   3 Shares      [1/2/3] switch tab [q] quit │
│  HASHRATE                                                      │
│  cur 0.42 H/s  avg 0.40 H/s  best 0.50 H/s                    │
│  ▁▂▃▅▇█▇▅▃▂ (sparkline 48 detik)                              │
│  THREADS  T0 0.21 H/s total 1.2M A 10 R 0 W 0 ...              │
│  SHARES   A 20 (100%) R 0 W 0 found 5 | EVENTS ...             │
└───────────────────────────────────────────────────────────────┘
```

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
docker run --rm ylxai/xcb:v1
```

Image docker **multi-stage** (builder → runtime ~32MB), jalan sebagai user non-root `miner`, entrypoint `./miner-saya`.
`pool.cfg` ikut di-copy ke `/miner/pool.cfg` sebagai fallback.

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
      - FULL_MEM=0
      - LARGE_PAGES=0
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
kubectl apply -f k8s/deployment.yaml
kubectl logs -n mining deploy/xcb-miner -f
```
Wallet disimpan sebagai **Secret** (tidak di-env image). **`THREADS` wajib = `limits.cpu`**
(default di kode = `nproc` NODE, bukan limit pod — salah set akan oversubscribe).

---

## Benchmark

| Thread | Mode | Hashrate | RAM |
|--------|------|----------|-----|
| 1 | Light | ~68 H/s | ~256MB |
| 2 | Light | ~122 H/s | ~256MB |
| 4 | Light | **~291 H/s** | ~256MB |
| 16 | Light | ~1.26 KH/s | ~256MB |
| 16 | **Full** | **~7.9 KH/s** | ~2.6GB |

> Full mode ±6x hashrate light mode (diverifikasi di VPS 16 core, 8GB RAM).

---

## Optimasi untuk Mesin Sendiri

```bash
# Huge pages (2MB)
sudo sysctl vm.nr_hugepages=1280

# CPU performance governor
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Realtime priority
sudo chrt -rr 1 ./miner-saya
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
| `cache alloc failed` | Huge pages tidak tersedia di container | Set `LARGE_PAGES=0` |
| Crash `stoul` / `Config` gagal | Env var kosong (`-e THREADS=`) | Biarkan env kosong atau isi nilai valid — versi v1 sudah menangani string kosong |
| Cuma 1 thread padahal banyak core | Default kode lama `threads=1` | Set `THREADS=<n>` atau gunakan v1 (auto = semua cores) |
| Hashrate ~50% dari harapan | Mode light (FULL_MEM=0) | `FULL_MEM=1` + RAM cukup |
| Worker stuck (0.0x H/s) | Core sibuk/di luar cpuset provider (CPU affinity) | Cek `lscpu`/`Cpus_allowed_list`; jalankan berkali atau kurangi THREADS |
| Build gagal `RandomY` tidak ditemukan | Submodule belum di-init | `git submodule update --init --recursive` |
| Log kapah (ribuan share/detik) | Pool difficulty rendah + log per-share | Default v1 quiet; aktifkan verbose hanya untuk debug (`LOG_SHARES=1`) |

---

## Struktur Proyek

```
xcb/
├── CMakeLists.txt           # Build system (-march=x86-64-v3 / armv8-a+crypto, -O3, LTO, stripped)
├── Dockerfile               # Multi-stage: builder → runtime ~32MB, user non-root
├── .gitlab-ci.yml           # CI Puzl RunMyJob (test 2m saat push, mine 55m/jam saat schedule)
├── k8s/
│   └── deployment.yaml      # Deployment + Secret contoh
├── external/
│   ├── RandomY/             # RandomY library (submodule, BSD 3-Clause)
│   └── FTXUI/               # Terminal TUI library (submodule, MIT)
├── pool.cfg                 # Multi-server config (wallet + 3 pool failover)
└── src/
    ├── main.cpp             # Entrypoint + signal handler + selftest
    ├── Config.hpp/.cpp      # Config parser (env vars + file + CLI)
    ├── StratumClient.hpp/.cpp # ETHPROXY protocol client
    ├── Miner.hpp/.cpp       # Thread pool, VM management, mining loop
    ├── Stats.hpp/.cpp       # Telemetri: rolling hashrate, sparkline, summary, dashboard
    ├── Dashboard.hpp/.cpp   # TUI FTXUI full-screen (tab Overview/Threads/Shares)
    ├── Log.hpp/.cpp         # Logger thread-safe (level, warna, JSON)
    ├── Submitter.hpp/.cpp   # Async share submitter (bounded queue)
    └── Sha3_512.hpp/.cpp    # SHA3-512 keccak satu-blok (hot path 40B)
```

---

## Lisensi

Kode sendiri — bebas digunakan untuk keperluan pribadi.
RandomY library © RandomX contributors — BSD 3-Clause.