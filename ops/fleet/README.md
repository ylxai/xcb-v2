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

## Tuning jumlah thread (`xcb-thread-sweep.sh`)

Dijalankan **sebagai root di host miner** (mis. `fl`), bukan dari mesin lokal:
skrip ini butuh `systemctl` dan harus menghitung proses root — dengan
`hidepid=2`, user biasa tidak melihat miner sama sekali.

```bash
scp xcb-thread-sweep.sh ubuntu@<fl>:~/
ssh ubuntu@<fl> 'sudo -n setsid sh -c "sh ~/xcb-thread-sweep.sh >/root/sweep-console.log 2>&1 </dev/null"'
# atau: sudo NONCES=400000 THREADS_LIST="2 4" sh ~/xcb-thread-sweep.sh
```

`setsid` + redirect + `</dev/null` mencegah HUP dari koneksi SSH yang putus
lebih dulu dari trap skrip; trap tetap memulihkan service sebagai jaring kedua.

Skrip ini **khusus XCB/RandomY**: benchmark selalu dipaksa `-a randomy` dan
host yang service-nya berjalan `-a conn` ditolak. Alasannya di `main.cpp`:
cabang `algo == conn` ditangani sebelum jalur benchmark, jadi `--benchmark` di
host CONN bukan benchmark sama sekali — skrip akan menambang, bukan mengukur.
Sweep juga memegang lock host-wide (`/run/xcb-thread-sweep.lock`) agar tidak
bertabrakan dengan sweep/deploy lain, berhenti bila masih ada benchmark yatim
dari sweep sebelumnya (proses itu berebut CPU dan membuat angka tidak
sebanding), dan menolak nilai di luar rentang biner:
`NONCES` 1..1000000000 (di luar itu jadi mode mining) dan tiap `-t` 1..4096
(di atas itu jadi auto-threads).

Alurnya: cek kondisi awal → `systemctl stop syscached` → tunggu 0 proses →
`xcb --ui=log --benchmark <NONCES> -t <N>` untuk tiap N → ringkasan H/s →
selalu `systemctl start syscached` lalu verifikasi tepat satu proses. Trap
`EXIT`/`INT`/`TERM`/`HUP` memulihkan service walau skrip gagal atau koneksi
SSH putus. Hasil per run ada di `/root/xcb-bench-t<N>-<stamp>.log`, ringkasan
di `/root/xcb-thread-sweep-<stamp>.log`.

Satu host berhenti menambang selama sweep (±2 menit per nilai `-t` pada
200.000 nonce). Hasil di `fl` (4 vCPU, 17 Nov 2026): `-t 2` 1.176 H/s,
`-t 4` 2.296 H/s, `-t 6` 2.296 H/s — scaling linear sampai 4 thread dan `-t 6`
tidak menambah apa pun karena kuota CPU host 4 vCPU, jadi `THREADS=4` sudah
optimal di host itu.

### Polling job (`POLL_MS`): 500 ms vs 1000 ms

Menambah `POLL_MS=500` (miner resmi memakai 500 ms, default kita 1000 ms).
Diuji di `fl` 45 menit dan dibandingkan slot jam yang sama sehari sebelumnya:

| setelan | share (A) | stale (W/R) | job baru | HR |
|---|---|---|---|---|
| `POLL_MS=1000` (16 Nov 17:55–18:40) | +1 | 0 / 0 | 452 (603/jam) | 2,30–2,33 kH/s |
| `POLL_MS=500` (17 Nov 17:55–18:40) | +1 | 0 / 0 | 435 (580/jam) | 2,34 kH/s |

Tidak ada perbedaan yang terdeteksi, tetapi jendela 45 menit dengan **1 share per
jendela** tidak cukup untuk menyimpulkan "sama": yang bisa dikatakan adalah
tidak ada bukti perbaikan, bukan bukti kesetaraan (lihat catatan daya statistik
di bawah). Setelan terbukti aktif — laju request naik 1,0 → 1,87
getWork/detik (`bytes_received` dari `ss -tinH state established '( dport = :8008 )'`)
— dan pool tidak menerbitkan jobId baru per request (churn tetap ~600/jam,
bukan berlipat). Baseline 24 jam pada 1000 ms: 65 share, 0 stale drop, 0 reject,
artinya pool masih menerima share untuk jobId sebelumnya sehingga jendela buta
antar-poll tidak berubah menjadi kerugian. Setelan dikembalikan ke default
(tanpa `POLL_MS`); backup env: `/etc/xcb/miner.env.bak-<stamp>`.

Kesimpulan praktis: jangan ulangi A/B ini kecuali saat share rate naik. Pada
diff 3,3M hanya ~2,7 share/jam, jadi mendeteksi efek ~3% butuh ~2000 share —
sekitar **31 hari**, bukan jam. Di diff ~0,5M dulu sampelnya ~6,6x lebih cepat
(~200 share/hari), masih bukan uji sekali jalan.

### RandomY: sudah optimal (diverifikasi 17 Nov 2026)

Semua knob RandomY sudah di posisi terbaik, jadi ~2,3 kH/s (≈260 H/s/GHz/core
pada 2,19 GHz) adalah batas hardware VM ini:

- `JIT=1 HARD_AES=1 FULL_MEM=1 LARGE_PAGES=1`; RandomY di-link statis ke
  binary `xcb`, dan binary itu memang berisi kode AES/AVX (`objdump -d
  /usr/local/bin/xcb` → 26 `aesenc`, 249 `vpxor`), jadi flag-nya tidak
  diabaikan. Catatan: `librandomx_v2.so` adalah backend CONN (`rxv2_*`), bukan
  library XCB — jangan dipakai sebagai bukti untuk RandomY.
- Hugepages betul terpakai: `HugePages_Free` turun 136 halaman = dataset 132
  (264 MB) + scratchpad 4 × 2 MB (1 per worker). Cache 256 MB dilepas dengan
  `randomx_release_cache()` setelah dataset siap, jadi tidak ada RAM terbuang.
- Worker dipin 1:1 ke CPU 0-3 (`pthread_setaffinity_np` + `nice -10`); scaling
  thread linear sampai 4 thread ⇒ 4 vCPU memang 4 core, bukan 2 core × SMT.
- `steal` 0% dan frekuensi terkunci 2.194 MHz (tidak ada turbo di guest).

Yang tersisa hanya di luar guest: menambah vCPU (linear, +4 vCPU ≈ +2,3 kH/s)
atau MSR prefetcher yang diblokir hypervisor. Rebuild dengan `-DARCH=native`
tidak menolong karena path AVX2/AES sudah aktif saat runtime.

### MSR prefetcher tidak tersedia di fleet ini

Mematikan hardware prefetcher (`wrmsr 0x1a4`, preset Intel xmrig `0xf`)
memberi 2–5% di RandomX/RandomY. Di `fl` (Xeon Silver 4214, **KVM**) MSR itu
tidak diekspos hypervisor: `rdmsr -a 0x1a4` gagal `cannot read MSR
0x000001a4` di semua vCPU, sementara `0x10` (TSC) dan `0x1a0` terbaca — jadi
driver `msr` sehat dan blokirnya spesifik per-MSR. Karena nilai aslinya tidak
bisa dibaca, perubahan itu juga tidak bisa di-rollback: **jangan dicoba di
host ini**. Efeknya hanya terukur di bare metal atau VM yang mem-pass-through
MSR. Jangan `apt-get install msr-tools` untuk keperluan ini sebelum `rdmsr`
terbukti berhasil.

## Troubleshooting

- **`GLIBC_2.38 not found`** — artefak dibuild di 24.04. Pakai `--from-ci`
  (job `build-2204`) atau build di host 22.04.
- **`librandomx_v2.so: cannot open shared object file`** — library tidak ada
  di samping binary. `apply.sh` mengecek `ldd` dan otomatis rollback.
- **Deploy berhenti di canary** — cek output host tersebut; host lain belum
  disentuh. Perbaiki, lalu ulangi `--only <host>`.
