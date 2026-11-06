# Topologi Build & Fleet

Dokumen ini menjelaskan dua titik masuk ke infrastruktur miner dan host apa saja
yang ada di baliknya. Alamat IP, wallet, dan key **tidak** ditulis di sini —
repo ini publik. Alamat host ada di `~/.config/xcb/fleet.hosts` pada masing-masing
host; key ada di `~/.ssh/`.

---

## Aturan utama

1. **Jangan build atau test di mesin lokal.** Semua build, `ctest`, dan
   `shellcheck` dijalankan di `fl`. Mesin lokal hanya untuk membaca file,
   `git`, dan menyunting kode.
2. **Jangan jalankan dua miner di satu host.** HR akan terbagi. Setiap host
   harus punya tepat satu proses `xcb` di bawah `syscached.service`.
3. **Deploy fleet dijalankan dari `security-learning`**, bukan dari `fl`.
   `fl` hanya bisa men-deploy dirinya sendiri.

---

## `fl` — host build + worker

`fl` adalah satu VM yang merangkap dua peran:

| Peran | Keterangan |
|---|---|
| Host build | sumber di `~/xcb-build`, build dir `~/xcb-build/build` |
| Worker | menjalankan `syscached.service` seperti host lain |

Build dan test:

```bash
rsync -a --delete --exclude='.git' --exclude='/build' --exclude='/build-*' \
    <repo-lokal>/ ubuntu@<fl>:/home/ubuntu/xcb-build/

ssh ubuntu@<fl> 'cd ~/xcb-build && cmake --build build --parallel 4'
ssh ubuntu@<fl> 'cd ~/xcb-build/build && ctest --output-on-failure'
ssh ubuntu@<fl> '~/xcb-build/build/xcb --selftest'
```

Catatan:

- `--exclude` wajib: `--delete` tanpa itu akan menghapus `build/` di sana.
- SHA di `xcb --version` di-bake saat **configure**, jadi build inkremental
  tetap melaporkan SHA lama. Jangan pakai string itu untuk memastikan revisi;
  bandingkan tanggal file atau artefak.
- Build memakai 4 core dan bersaing dengan miner yang sedang jalan. HR akan
  turun sementara selama build; itu wajar, bukan regresi.

Deploy dari `fl` hanya untuk dirinya sendiri:

```bash
cd ~/xcb-build/ops/fleet
./deploy.sh --from <dir-artefak> --only fl --expect <commit>
```

`fl` **tidak bisa** men-deploy worker lain: key-nya tidak diotorisasi di sana
(`Permission denied (publickey)`), dan `~/.config/xcb/fleet.hosts` di `fl` masih
menunjuk `ai-workers-1..4` yang sudah tidak ada.

---

## `security-learning` — jump host + worker

`security-learning` adalah VM kedua yang juga merangkap dua peran:

| Peran | Keterangan |
|---|---|
| Jump host | satu-satunya pintu masuk ke empat worker `agents-*` |
| Worker | menjalankan `syscached.service` seperti host lain |

Akses berlapis:

```bash
# dari lokal: key ecdsa hanya berlaku untuk jump host
ssh -i ~/.ssh/id_ecdsa ubuntu@<security-learning>

# dari jump host: key rsa milik host itu menjangkau worker
ssh -i ~/.ssh/id_rsa ubuntu@<worker>
```

Host yang ada di baliknya:

| Worker | Peran |
|---|---|
| `agents-1` | worker |
| `agents-2` | worker |
| `agents-3` | worker |
| `agents-4` | worker |

Contoh perintah menyeluruh dari jump host:

```bash
for h in <agents-1> <agents-2> <agents-3> <agents-4>; do
    ssh -i ~/.ssh/id_rsa ubuntu@"$h" 'systemctl is-active syscached'
done
```

Deploy fleet dijalankan dari sini:

```bash
scp -i ~/.ssh/id_ecdsa -r <dir-artefak> ubuntu@<security-learning>:~/
ssh -i ~/.ssh/id_ecdsa ubuntu@<security-learning>
# tulis ulang ~/.config/xcb/fleet.hosts untuk enam host, lalu:
cd ~/xcb-build/ops/fleet && ./deploy.sh --from ~/artefak --expect <commit>
```

---

## Peta ringkas

```
lokal
 ├── id_rsa    → fl                  (build + worker fl)
 └── id_ecdsa  → security-learning   (jump host + worker)
                     └── id_rsa (milik host itu)
                          ├── agents-1
                          ├── agents-2
                          ├── agents-3
                          └── agents-4
```

Total enam worker: `fl`, `security-learning`, `agents-1..4`.

---

## Yang sering salah

| Anggapan | Kenyataan |
|---|---|
| `fl` dan `security-learning` satu mesin dengan dua IP | Dua VM berbeda; machine-id, MAC, dan SSH host key semuanya beda |
| Deploy bisa dijalankan dari `fl` ke semua host | Hanya `--only fl` yang jalan; sisanya ditolak publickey |
| Build di lokal lebih cepat | Host build yang benar adalah `fl`; lokal tidak dipakai untuk build/test |
| Miner boleh dijalankan manual untuk uji | Hanya lewat `syscached.service`, satu proses per host |
