# Menambang Zycord (ZCD) dengan xcb-v2

Zycord (ZCD) memakai **RandomX V2.0.1** — engine hashing yang SAMA PERSIS dengan
yang dipakai backend CONN di xcb-v2. Perbedaannya hanya di lapisan pool:

- **Algo stratum**: CONN = `rx/2b`, Zycord = `rx/2` (keduanya dialek xmrig/CryptoNote rplant).
- **Endpoint pool**: Zycord rplant = `randomx.rplant.xyz:17197` (TLS).
- **Login**: alamat wallet ZCD (bukan RPC node — Zycord tidak punya stratum solo).

Karena engine identik, tidak ada kode hashing baru; xcb-v2 memakai ulang jalur
`ConnMiner` pool rplant dan hanya mengganti string `algo` + endpoint.

## Cara menjalankan

Minimal — butuh wallet ZCD sebagai login pool:

```
./xcb --zycord-pool randomx.rplant.xyz:17197 \
      --zycord-address <alamatZCD> \
      --zycord-pool-worker fl-z
```

Flag:

| Flag | Default | Keterangan |
|------|---------|-----------|
| `--zycord-pool HOST[:PORT]` | (kosong) | Pool rplant ZCD; default port `17197`, TLS. Kosong = tidak menambang ZCD. |
| `--zycord-pool-plain` | TLS aktif | Matikan TLS. |
| `--zycord-pool-worker NAME` | `xcb` | Label worker di dashboard pool. |
| `--zycord-address ADDR` | (kosong) | Wallet ZCD = login pool. **Wajib.** |

Env (setara CLI): `ZYKORD_POOL`, `ZYKORD_ADDRESS`, `ZYKORD_WORKER`.
File (`pool.cfg`): `zycord_pool`, `zycord_address`, `zycord_pool_worker`, `zycord_pool_plain`.

Alamat ZCD diambil dari wallet key Anda (mis. `~/zycord-wallet.json`), alamat
persisten `0x02…`. Pastikan alamat valid untuk rplant.

## Verifikasi

1. Log lokal: cek `A` (accepted) naik. Engine RandomX V2 siap dalam ~10 s
   (`RandomX V2 siap (FAST, hugepages)` atau mode light bila hugepages gagal).
2. **Wajib** cek pool-side: worker ZCD muncul online dan `lastShare` bergerak di
   API rplant. Jangan puas dengan `A` lokal saja — pool meng-ACK share meski
   tidak valid. Bandingkan `found` vs `A`; `A == found` berarti tidak ada share
   hilang di jalur submit.

## Catatan

- `--selftest` (`rxv2::selftest`) tetap PASS — membuktikan ZCD pakai engine yang
  sama persis dengan CONN.
- Yield perkiraan ~17 ZCD/hari dari fleet (≈ $0.21/hari pada harga ~$0.012546,
  listing baru & volatil). Re-konfirmasi harga sebelum produksi.
- Sama seperti CONN pool: tepat satu proses miner per host. Hentikan miner lain
  di host yang sama sebelum menjalankan ini.
