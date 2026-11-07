# ops/systemd — unit contoh untuk miner CONN

Unit di folder ini adalah **template**, bukan konfigurasi fleet yang sedang
berjalan. Alamat pool, wallet, dan worker disimpan di environment file lokal.

## Pasang di satu host

```bash
sudo install -d -m 755 /etc/xcb
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
- Flag `--conn-pool`, `-u`, `--worker`, `-t` di `ExecStart` **sengaja
  eksplisit**. Binary baru sudah membaca `WALLET`/`WORKER`/`THREADS` dari
  environment pada jalur CONN, tetapi flag ini dipertahankan agar unit tetap
  benar di binary lama yang belum membacanya — selama fleet belum seragam,
  jangan hapus. `CONN_POOL` hanya mengisi nama host; kalau nilainya memuat
  `:port`, bagian port tidak dipisah dan port tetap default 17194.
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
