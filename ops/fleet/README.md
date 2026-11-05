# ops/fleet — deploy & monitoring fleet xcb CONN

Skrip untuk fleet miner (fl + ai-workers-1..4). Daftar host **tidak** disimpan
di repo (repo publik); simpan di `~/.config/xcb/fleet.hosts`.

## Setup

```bash
mkdir -p ~/.config/xcb
cp ops/fleet/hosts.conf.example ~/.config/xcb/fleet.hosts
$EDITOR ~/.config/xcb/fleet.hosts
```

Format: `nama=alamat`, dengan `local` untuk host tempat skrip dijalankan.

## Artefak build (glibc 2.35 / Ubuntu 22.04)

Worker memakai Ubuntu 22.04 (glibc 2.35), `fl` 24.04. Binary yang dibuild di
22.04 jalan di keduanya, jadi itu satu-satunya artefak deploy.

CI job `build-2204` membuild + upload artefak `xcb-2204` (`xcb`,
`librandomx_v2.so`, `VERSION`) pada setiap push ke `main`.

## Deploy

```bash
cd ops/fleet
./deploy.sh --from-ci --expect <commit>   # artefak CI run main terakhir
./deploy.sh --from ../../build            # artefak lokal
./deploy.sh --only fl --dry-run           # lihat rencana
```

Alur per host: stop → backup (`xcb.bak-STAMP`) → install → cek `ldd` +
`--version` → start → tunggu `RandomX V2 siap`. Kalau ada yang gagal,
rollback otomatis dan deploy berhenti — host lain tidak disentuh.

Host pertama adalah canary. Setelah canary OK, sisanya jalan berurutan.

Rollback manual:

```bash
./deploy.sh --rollback              # backup terbaru di tiap host
./deploy.sh --rollback 20261105-112843
```

## Monitoring

```bash
./watch.sh --dry-run       # lihat hasil tanpa kirim alert
./watch.sh --test-alert    # uji channel alert
```

Cron tiap 5 menit:

```cron
*/5 * * * * $HOME/xcb-build/ops/fleet/watch.sh >> $HOME/xcb-watch.log 2>&1
```

Konfigurasi `~/.config/xcb/watch.env` (lihat `watch.env.example`):

```sh
ALERT_WEBHOOK="https://hooks.slack.com/services/..."
# atau Telegram:
ALERT_TELEGRAM_TOKEN="123:abc"
ALERT_TELEGRAM_CHAT="123456789"
MIN_HR=400
MAX_REJECT_PCT=5
RECONNECT_LIMIT=3
```

Yang dicek per host:

- service `syscached` harus `active`;
- ada log `HR` dalam 4 menit terakhir (mendeteksi miner hang);
- HR di bawah `MIN_HR`;
- reject rate di atas `MAX_REJECT_PCT` (minimal 20 share);
- reconnect / login gagal lebih dari `RECONNECT_LIMIT` per 15 menit;
- `TLS verifikasi gagal` (langsung alert);
- fallback ke `regular pages` (hugepages tidak terpakai).

Alert di-cooldown 30 menit per host, dan mengirim pesan "pulih" saat host
kembali normal.

## Troubleshooting

- **`GLIBC_2.38 not found`** — artefak dibuild di 24.04. Pakai `--from-ci`
  (job `build-2204`) atau build di host 22.04.
- **`librandomx_v2.so: cannot open shared object file`** — library tidak ada
  di samping binary. `apply.sh` mengecek `ldd` dan otomatis rollback.
- **Deploy berhenti di canary** — cek output host tersebut; host lain belum
  disentuh. Perbaiki, lalu ulangi `--only <host>`.
