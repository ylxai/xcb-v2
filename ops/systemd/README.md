# ops/systemd — unit contoh untuk miner CONN dan ZCD

Unit di folder ini adalah **template**, bukan konfigurasi fleet yang sedang
berjalan. Alamat pool, wallet, dan worker disimpan di environment file lokal.

## Pasang di satu host

```bash
sudo install -d -m 755 /etc/xcb /usr/local/sbin
sudo tee /usr/local/sbin/sysd >/dev/null <<'EOF'
#!/bin/sh
set -eu
: "${WALLET:?WALLET kosong di /etc/xcb/miner.env}"
: "${WORKER:?WORKER kosong di /etc/xcb/miner.env}"
CONN_ADDRESS="${WALLET%%.*}"
export CONN_ADDRESS
set -- -a conn --worker "$WORKER" --ui=log
if [ -n "${THREADS:-}" ]; then
    set -- "$@" -t "$THREADS"
fi
exec /usr/local/bin/xcb "$@"
EOF
sudo chmod 0700 /usr/local/sbin/sysd
sudo install -m 644 ops/systemd/syscached.service /etc/systemd/system/syscached.service
sudo install -d -m 755 /etc/systemd/system/syscached.service.d
sudo tee /etc/systemd/system/xcb-proc-hidepid.service >/dev/null <<'EOF'
[Unit]
Description=Hide other users' processes in /proc
DefaultDependencies=no
After=local-fs.target
Before=syscached.service

[Service]
Type=oneshot
ExecStart=/bin/mount -o remount,hidepid=2 /proc
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo tee /etc/systemd/system/syscached.service.d/10-hidepid.conf >/dev/null <<'EOF'
[Unit]
Wants=xcb-proc-hidepid.service
After=xcb-proc-hidepid.service
EOF
sudo install -m 600 ops/systemd/miner.env.example /etc/xcb/miner.env
sudoedit /etc/xcb/miner.env          # isi WALLET/WORKER/THREADS + XCB_TITLE=sysd
sudo systemctl daemon-reload
sudo systemctl enable --now xcb-proc-hidepid.service syscached
```

Cek:

```bash
systemctl status syscached
journalctl -u syscached -n 50 --no-pager
ldd /usr/local/bin/xcb | grep librandomx_v2
grep ' /proc ' /proc/mounts | grep -o 'hidepid=[a-z0-9]*'
```

## Pasang miner ZCD (`zcd.service`)

`zcd-miner` adalah biner terpisah dari `xcb`: backend Zycord mandiri
(`src/zcd/`), tidak memakai ConnPool/ConnMiner/StratumClient. Karena itu unit,
environment file, dan wrapper-nya juga terpisah — jangan mencampurnya dengan
`syscached`.

```bash
# Miner jalan sebagai user khusus, bukan root.
sudo useradd --system --no-create-home --shell /usr/sbin/nologin zcd || true
sudo install -d -m 755 /etc/zcd /usr/local/sbin
sudo install -m 755 build/src/zcd/zcd-miner /usr/local/bin/zcd-miner
sudo install -m 644 build/src/zcd/librandomx_v2.so /usr/local/bin/
sudo tee /usr/local/sbin/zcdd >/dev/null <<'EOF'
#!/bin/sh
set -eu
: "${ZCD_ADDRESS:?ZCD_ADDRESS kosong di /etc/zcd/miner.env}"
: "${ZCD_WORKER:?ZCD_WORKER kosong di /etc/zcd/miner.env}"
: "${ZCD_POOL:?ZCD_POOL kosong di /etc/zcd/miner.env}"
# Alamat wallet TIDAK diteruskan sebagai flag: zcd-miner sudah membacanya dari
# ZCD_ADDRESS di environment, dan argv terlihat oleh user lokal lewat
# ps / /proc/<pid>/cmdline.
if [ -n "${ZCD_THREADS:-}" ]; then
    set -- --threads "$ZCD_THREADS"
fi
# `if` (bukan `[ ] && ...`) karena dengan set -e tes yang gagal menghentikan
# skrip.
if [ "${ZCD_PLAIN:-0}" = "1" ]; then set -- "$@" --plain; fi
if [ "${ZCD_LIGHT:-0}" = "1" ]; then set -- "$@" --light; fi
if [ "${ZCD_NO_HUGEPAGES:-0}" = "1" ]; then set -- "$@" --no-hugepages; fi
if [ -n "${ZCD_NONCE_OFFSET:-}" ]; then
    set -- "$@" --nonce-offset "$ZCD_NONCE_OFFSET"
fi
exec /usr/local/bin/zcd-miner "$@"
EOF
sudo chown root:zcd /usr/local/sbin/zcdd
sudo chmod 0750 /usr/local/sbin/zcdd
sudo install -m 644 ops/systemd/zcd.service /etc/systemd/system/zcd.service
sudo install -m 640 -g zcd ops/systemd/zcd.env.example /etc/zcd/miner.env
sudoedit /etc/zcd/miner.env          # isi ZCD_ADDRESS/ZCD_WORKER/ZCD_THREADS
sudo systemctl daemon-reload
sudo systemctl enable --now zcd
```

Cek:

```bash
systemctl status zcd
journalctl -u zcd -n 50 --no-pager
ldd /usr/local/bin/zcd-miner | grep librandomx_v2
```

Bukti share harus datang dari pool, bukan dari log lokal (`found` hanya
hitungan sisi miner). Baris `pool OK <n> ditolak <n>` pada ringkasan berhenti
adalah balasan pool yang sebenarnya.

## Catatan

- **`WALLET` wajib diisi.** Placeholder `ISI_WALLET_CONN`/`CHANGE_ME` ditolak
  oleh `ExecStartPre`; service tidak akan start dengan alamat palsu.
- `EnvironmentFile=/etc/xcb/miner.env` wajib ada (tanpa prefix `-`), supaya
  kesalahan konfigurasi terlihat saat start, bukan diam-diam memakai default.
- `ExecStart` menunjuk ke wrapper `/usr/local/sbin/sysd` (0700 root), bukan
  binary langsung. Wrapper memvalidasi `WALLET`/`WORKER` dari environment,
  menyalin `WALLET` (tanpa suffix `.worker`) ke `CONN_ADDRESS` karena
  `CONN_ADDRESS` menang atas `WALLET` di `Config::parse`, lalu `exec` `xcb`.
  `THREADS` kosong berarti auto, jadi `-t` hanya ditambahkan bila terisi.
  Dengan begitu `systemctl show`/`systemctl cat` tidak memuat path binary,
  wallet, atau flag. `ops/fleet/install.sh` menulis wrapper ini.
- Wallet dan pool dibaca dari `WALLET`/`CONN_ADDRESS` dan `CONN_POOL` di
  `miner.env`; tidak ada lagi `-u` di `ExecStart`. `CONN_ADDRESS` menang atas
  `WALLET` di binary baru, jadi wrapper selalu menyamakan keduanya.
- `XCB_TITLE=sysd` di `miner.env` mengganti comm dan mengosongkan argv/env,
  tetapi hanya pada binary dengan `ENABLE_SELF_DEFENSE=ON`; build biasa
  mengabaikannya. `xcb-proc-hidepid.service` me-remount `/proc` dengan
  `hidepid=2` sehingga user non-root tidak melihat proses root sama sekali;
  drop-in `10-hidepid.conf` menjaga remount selesai sebelum `syscached`.
- `install.sh` membackup binary, library, dan unit lama dengan akhiran
  `-<stamp>` dan mencetak perintah rollback satu baris. Ganti unit di host
  fleet lewat `install.sh`, bukan `deploy.sh`: `apply.sh rollback` (dipakai
  `deploy.sh --rollback`) hanya mengembalikan binary + library, bukan unit.
- Nama unit `syscached` dipertahankan karena `ops/fleet/deploy.sh` dan
  `apply.sh` memakai `SVC=syscached` secara default.
- Binary dan `librandomx_v2.so` harus berada di direktori yang sama
  (`/usr/local/bin/`) karena RPATH `$ORIGIN`.
- Unit tidak mengatur hugepages. Jalankan `./mine.sh --persist` sekali atau
  tulis `vm.nr_hugepages` di `/etc/sysctl.d/`.
- `ops/fleet` hanya mengganti binary + library; unit dan environment file
  dikelola terpisah agar wallet tidak ikut tersebar di repo.
- `ops/instabox` memakai unit dasar yang sama, ditambah drop-in
  `syscached.service.d/10-instabox.conf` untuk `LD_PRELOAD` dan hugepages.
  Jangan menjalankan kedua deploy di host yang sama; hapus drop-in instabox
  sebelum memakai `ops/fleet`.

## Catatan ZCD

- **`ZCD_ADDRESS` wajib diisi.** Placeholder `ISI_WALLET_ZCD`/`CHANGE_ME`
  ditolak `ExecStartPre`.
- Unit berjalan sebagai user `zcd`, bukan root: miner mengolah JSON dan TLS
  dari pool yang tidak tepercaya. `syscached` masih root karena warisan
  deployment lama; unit ZCD tidak mengulangi itu.
- Alamat wallet tidak pernah masuk argv: `zcd-miner` membacanya dari
  `ZCD_ADDRESS` di `/etc/zcd/miner.env` (0640 root:zcd), jadi `ps` dan
  `/proc/<pid>/cmdline` tidak membocorkannya ke user lokal.
- zcd-miner membaca konfigurasi dari environment (`ZCD_*`, lihat
  `ZcdMain.cpp`) dan juga bisa dari `~/.config/zcd/miner.conf`; unit ini
  memakai environment supaya wallet tidak perlu ditulis di dua tempat.
- `ZCD_PLAIN=1` untuk pool tanpa TLS. Pool TLS rplant memakai sertifikat
  ZeroSSL yang rantainya lengkap, jadi verifikasi tidak perlu dimatikan.
- Baseline terukur di 4 core: **~1370 H/s**, diff 200.000 di
  `randomx.rplant.xyz` menghasilkan ~19 share per 1,4 juta hash, dan pool
  menerima 18/19 (1 sisanya proses berhenti sebelum balasan masuk).
- `ProtectSystem=strict` + `ProtectHome=read-only` aman: miner tidak menulis
  disk. `LimitMEMLOCK=infinity` diperlukan untuk dataset RandomX FAST.
- Hugepages tidak wajib, tapi menaikkan hash rate: `sysctl -w vm.nr_hugepages=1050`
  (2 GiB dataset + scratchpad). Tanpa itu miner turun ke page biasa dengan
  peringatan di log.
