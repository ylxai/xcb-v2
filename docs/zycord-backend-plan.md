# Rencana Implementasi Backend Zycord (ZCD) pada xcb-v2

Tanggal: 2026-11-11 — cabang `feat/zycord-backend`

## 1. Ringkasan & keputusan arsitektur

Zycord (ZCD) adalah RandomX **v2.0.1** (tevador v2.0.1: VM 384-instruksi, dataset 2 GiB,
salt Argon2 `RandomX\x03`) yang di-rekey tiap **2048 block** (lag 64). Ini adalah
**algoritma yang SAMA PERSIS** dengan engine RandomX V2 yang sudah dipakai backend CONN
di xcb-v2 — dibuktikan lewat bacaan source:

- `external/randomx-v2/src/configuration.h:41` → `RANDOMX_ARGON_SALT "RandomX\x03"`
- `configuration.h:50` → `RANDOMX_DATASET_BASE_SIZE 2147483648` (2 GiB)
- `configuration.h:56-57` → `RANDOMX_PROGRAM_SIZE_V1 256` / `RANDOMX_PROGRAM_SIZE_V2 384`
- `randomx.h:55` → `RANDOMX_FLAG_V2 = 128`

Dok Zycord (`docs/decisions/randomx-v2.md`) mencantumkan konstanta v2.0.1 yang identik.
Jadi **tidak ada engine hashing baru** yang perlu dibuat.

Lebih jauh: jalur **pool rplant** untuk CONN (`ConnMiner` + `ConnPool`) sudah merupakan
miner RandomX V2 + stratum xmrig/CryptoNote yang lengkap, dan SEMUA bagiannya
chain-agnostik:

- Hashing: `rxv2::Context` / `VM::hash()` dengan `RANDOMX_FLAG_V2` — sama untuk ZCD.
- Blob/job: `ConnPool` parse `blob` (76/80 byte) + `seed_hash` (64 hex = key 32 byte)
  + `target` 8-byte LE (`ConnPool.cpp:419-441`). Key diambil dari pool, bukan dihitung
  miner, sehingga **selisih epoch 2048 vs 12288 tidak relevan** — miner pakai seed yang
  dikirim pool per job.
- Target: `leqTarget64` bandingkan limb ke-4 hash (byte 24..31) vs target 64-bit
  (`ConnMiner.cpp:556-560`) — standar CryptoNote/xmrig, sama untuk ZCD.
- Submit: `ConnPool::submit` sudah pakai `job.algo.empty() ? "rx/2b" : job.algo`
  (`ConnPool.cpp:528`) — per-job `algo` sudah diparse (`ConnPool.cpp:419`).

**Kesimpulan:** Zycord BUKAN backend baru. Ia adalah jalur pool `ConnMiner` yang sama
dengan string `algo` diganti `"rx/2b"` → `"rx/2"` dan endpoint pool diganti ke rplant ZCD.
Perubahan sesungguhnya kecil dan terlokalisasi (lihat §3).

## 2. Verifikasi pra-kondisi (lakukan sebelum coding)

Cek sisa yang belum 100% dipastikan — near-certain karena rplant ZCD & CONN sama-sama
pool NOMP rplant dengan dialek stratum xmrig identik, tapi layak dikonfirmasi sekali:

1. **Ambil port rplant ZCD** dari halaman connect
   `https://pool.rplant.xyz/#zycord#connect` (atau `/api/currencies` field `port`).
   CONN pakai `eu.rplant.xyz:17194`; ZCD port berbeda — jangan hardcode 17194.
2. **Cocokkan field job ZCD** dengan yang diparse `ConnPool::parseJob`
   (`ConnPool.cpp:413-441`): `blob`, `seed_hash` (64 hex), `target`, `height`, dan
   `algo`. Jika rplant ZCD mengirim field dengan nama berbeda (mis. `seedHash` bukan
   `seed_hash`, atau `target` dalam format beda), catat di sini dan sesuaikan parser.
   XMRig terhubung ke rplant untuk CONN maupun ZCD, jadi formatnya hampir pasti sama.
3. **Format wallet/login ZCD**: rplant memakai wallet address sebagai `login`. CONN pool
   pakai `cfg.address` sebagai login (`ConnMiner.cpp:906`). ZCD cukup pakai wallet ZCD
   sebagai `zycordAddress`.

## 3. Daftar perubahan (file-by-file)

### 3.1 `src/ConnPool.hpp` + `src/ConnPool.cpp` — jadikan `algo` terkonfigurasi
- `ConnPool.hpp`: tambah member `std::string m_algo = "rx/2b";` dan param `algo`
  di constructor (default `"rx/2b"` agar CONN tetap kompatibel mundur).
- `ConnPool.cpp:248` (`connectAndLogin`): ganti hardcoded `"algo":["rx/2b"]` menjadi
  `"algo":["` + m_algo + `"]`.
- `ConnPool.cpp:528` (`submit`): ganti fallback `"rx/2b"` menjadi `m_algo`
  (`job.algo.empty() ? m_algo : job.algo`).

### 3.2 `src/ConnMiner.hpp` — bawa `poolAlgo` ke config
- Di `struct ConnMinerConfig` (sekitar baris 31-34, blok field pool), tambah:
  `std::string poolAlgo = "rx/2b";  // algo stratum rplant (rx/2b=CONN, rx/2=ZCD)`

### 3.3 `src/ConnMiner.cpp` — teruskan `poolAlgo` ke `ConnPool`
- `start()` dekat `ConnMiner.cpp:906`:
  ```cpp
  m_impl->pool = std::make_unique<ConnPool>(cfg.poolHost, cfg.poolPort,
                                            cfg.address, cfg.poolWorker,
                                            cfg.poolTls, cfg.poolAlgo);
  ```

### 3.4 `src/Config.hpp` — field Zycord
- Di dekat baris 44-48 (blok pool CONN), tambah:
  ```cpp
  // Pool rplant Zycord (RandomX V2, algo rx/2). Isi host untuk mode pool.
  std::string zycordPoolHost;
  int zycordPoolPort = 0;       // diisi dari --zycord-pool / env ZYORD_POOL
  bool zycordPoolTls = true;
  std::string zycordPoolWorker; // label worker ke pool
  std::string zycordAddress;    // wallet ZCD = login pool
  ```

### 3.5 `src/Config.cpp` — env, CLI, help
- **Env** (mirror blok `envConnPool` `Config.cpp:375-377`): baca `ZYKORD_POOL`
  → `cfg.zycordPoolHost/Port`; `ZYKORD_WORKER` → `cfg.zycordPoolWorker`;
  `ZYKORD_ADDRESS` → `cfg.zycordAddress`.
- **CLI** (mirror `--conn-pool` `Config.cpp:519-523` dan `--address` `516`):
  - `--zycord-address WALLET` → `cfg.zycordAddress`
  - `--zycord-pool HOST[:PORT]` → `parseHostPort(..., defaultPortZCD, "ZYKORD_POOL port")`
    (default port = hasil §2.1)
  - `--zycord-pool-plain` → `cfg.zycordPoolTls = false`
  - `--zycord-pool-worker NAME` → `cfg.zycordPoolWorker`
- **Help** (`Config.cpp` sekitar 557-563): tambah baris untuk flag `--zycord-*`.

### 3.6 `src/main.cpp` — cabang Zycord
Dua opsi, pilih yang lebih disukai:

- **Opsi A (terendah risiko, duplikasi kecil):** tambah cabang baru di `main()`,
  mirip blok `cfg.algo == "conn"` + `!cfg.poolHost.empty()` (`main.cpp:204-236`)
  tapi hanya jalur pool, dengan `poolAlgo = "rx/2"`:
  ```cpp
  if (!cfg.zycordPoolHost.empty()) {
      lg::info("main", "xcb v2 (Zycord ZCD — RandomX V2, pool rplant rx/2)");
      ConnMinerConfig mc;
      mc.poolHost = cfg.zycordPoolHost;
      mc.poolPort = cfg.zycordPoolPort;
      mc.poolTls  = cfg.zycordPoolTls;
      mc.poolWorker = cfg.zycordPoolWorker.empty() ? "xcb" : cfg.zycordPoolWorker;
      mc.poolAlgo = "rx/2";
      mc.address = cfg.zycordAddress;
      mc.threads = cfg.threads;
      mc.fastMem = cfg.fullMem;
      mc.largePages = cfg.largePages;
      mc.nonceOffset = cfg.nonceOffset;
      ConnMiner m;
      if (!m.start(mc)) { lg::error("main","Zycord miner gagal start"); return 1; }
      while (g_running.load() && m.isRunning())
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (m.isRunning()) { lg::info("main","Stopping..."); m.stop(); }
      lg::info("sysd", m.finalSummary());
      return 0;
  }
  ```
- **Opsi B (lebih bersih, sentuh cabang CONN):** ekstrak helper
  `runPoolMiner(const ConnMinerConfig&)` dari blok CONN-pool dan pakai untuk
  CONN maupun Zycord. Disarankan Opsi B bila ingin hindari duplikasi; Opsi A
  jika ingin blast-radius minimal.

### 3.7 `docs/` — dokumentasi penggunaan (opsional tapi disarankan)
- Tambah `docs/zycord.md` (atau bagian di `README.md`): contoh perintah
  `--zycord-pool`, `--zycord-address`, dan catatan bahwa engine = RandomX V2
  (sama dengan CONN), hanya endpoint + algo beda.

## 4. Urutan implementasi

1. §2 (verifikasi pra-kondisi) — pastikan port ZCD & field job cocok.
2. §3.1 `ConnPool` (algo terkonfigurasi) — perubahan inti, kompatibel mundur.
3. §3.2 + §3.3 `ConnMiner` terima & teruskan `poolAlgo`.
4. §3.4 + §3.5 `Config` (field + env + CLI + help).
5. §3.6 `main.cpp` (cabang Zycord; pilih Opsi A atau B).
6. §3.7 docs.
7. Build + `--selftest` (lihat §5).

## 5. Verifikasi & deploy (ikuti prosedur fleet yang ada)

- **Build** di `fl` (host build miner, sesuai memori deploy). Jalankan `--selftest`:
  `rxv2::selftest()` harus PASS (engine tidak diubah — hanya string algo yang
  dilewatkan). Ini sekaligus membuktikan Zycord pakai engine yang sama persis.
- **Uji jalur ZCD** ~5-10 menit di `fl` (canary):
  `./xcb --zycord-pool <host>:<port> --zycord-address <walletZCD> --zycord-pool-worker fl-z`
  - Cek `A` (accepted) di log lokal naik.
  - **Verifikasi pool-side** (wajib, per memori): cek worker ZCD muncul online dan
    `lastShare` bergerak di API rplant — jangan puas dengan `A` lokal saja, karena
    pool meng-ACK share meski tidak valid. Bandingkan `found` vs `A`; `A == found`
    berarti tidak ada share hilang di jalur submit.
- **Rollout ke 10 VPS** mengikuti prosedur deploy tetap: fl canary → sisanya, setiap
  host di-stop, di-backup ke `/root/xcb-fix-<stamp>` dengan `rollback.sh`, binary
  diganti, di-restart; pastikan tepat satu proses miner per host.
- Pantau per-host `A` + status pool selama ~30 menit sebelum nyatakan sehat.

## 6. Risiko & cek sisa

- **Risiko utama:** field job rplant ZCD berbeda dari CONN (§2.2). Jika `seed_hash`
  atau `target` dikirim dengan nama/format beda, `buildEngine`/`leqTarget64` bisa
  salah. Mitigasi: verifikasi §2.2 sebelum coding; jika beda, sesuaikan `parseJob`.
- **Wallet/login:** ZCD pakai wallet address sebagai login (sama pola CONN pool).
  Pastikan alamat ZCD valid untuk rplant.
- **Harga ZCD sangat fresh** (~$0.012546, listing baru) — yield ~17 ZCD/hari ≈
  ~$0.21/hari sebelum fee + variansi. Re-konfirmasi harga sebelum komitmen produksi.
- **Tidak ada engine baru & tidak ada test file baru** diperlukan: `rxv2::selftest`
  sudah menutupi engine; perubahan murni parameter/alur pool.
