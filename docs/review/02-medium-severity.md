# 2. Temuan Prioritas Menengah (Medium Severity)

Dokumen ini memuat rincian teknis, analisis akar masalah, skenario dampak, dan rekomendasi perbaikan untuk temuan berprioritas menengah (`M1` – `M6`).

---

## M1. Precedence Konfigurasi `-a conn`: Env `WALLET`, `WORKER`, dan `THREADS` Diabaikan Bila `POOL` Tidak Di-set

- **Tingkat Keparahan:** Medium
- **Lokasi:**
  - `src/Config.cpp:235-295`
  - `ops/systemd/syscached.service:12-14`

### Bukti Kode

Di `src/Config.cpp:259-294`:
```cpp
    const char* envWallet = std::getenv("WALLET");
    const char* envPool   = std::getenv("POOL");
    const char* envWorker = std::getenv("WORKER");
    const char* envPass   = std::getenv("PASSWORD");
    const char* envThreads = std::getenv("THREADS");

    if (envWallet && envPool) {
        cfg.wallet = envWallet;
        if (envWorker) cfg.worker   = envWorker;
        if (envPass)   cfg.password = envPass;
        if (envThreads) {
            int t = 0;
            if (safeParseInt(envThreads, t, 0, 1024)) cfg.threads = t;
        }
        ...
        applyEnvFlags();
        return cfg;
    }
```

Di `ops/systemd/syscached.service:12-14`:
```ini
EnvironmentFile=/etc/xcb/miner.env
ExecStart=/opt/sys-Caching/xcb -a ${ALGO} \
    --conn-pool ${CONN_POOL} -u ${WALLET} --worker ${WORKER} -t ${THREADS}
```

### Analisis Masalah & Dampak

- Pada deployment ConnectCoin (`ALGO=conn`), file `/etc/xcb/miner.env` mendefinisikan `CONN_POOL`, `WALLET`, `WORKER`, dan `THREADS` tanpa `POOL`.
- Karena blok `if (envWallet && envPool)` bernilai `false`, variabel environment `WALLET`, `WORKER`, dan `THREADS` sama sekali tidak dibaca oleh `applyEnvFlags()`.
- Akibatnya, `ops/systemd/syscached.service` terpaksa meneruskan `-u ${WALLET} --worker ${WORKER} -t ${THREADS}` melalui argumen CLI `ExecStart`, yang membuat alamat wallet dan nama worker terekspos di `/proc/<pid>/cmdline` dan output `ps aux` (kecuali `XCB_SELF_DEFENSE` diaktifkan dan berfungsi).

### Rekomendasi Perbaikan

Di dalam `applyEnvFlags()` (`src/Config.cpp:235-256`):
1. Baca `THREADS` jika diset dan `cfg.threads == 0`.
2. Baca `WORKER` untuk mengisi `cfg.worker` (dan `cfg.connPoolWorker` bila `CONN_WORKER` tidak diset).
3. Baca `WALLET` sebagai fallback `cfg.connAddress` bila `CONN_ADDRESS` tidak diset.
4. Dengan perubahan ini, `ExecStart` di `ops/systemd/syscached.service` cukup menjadi `/opt/sys-Caching/xcb` (seluruh konfigurasi dibaca dari environment `/etc/xcb/miner.env` tanpa membocorkan alamat wallet di `ps aux`).

---

## M2. Penyusunan JSON Manual Tanpa Escaping di `ConnPool` dan `ConnRpc`

- **Tingkat Keparahan:** Medium
- **Lokasi:**
  - `src/ConnPool.cpp:244-247`
  - `src/ConnPool.cpp:522-527`
  - `src/ConnRpc.cpp:521, 539, 543`

### Bukti Kode

Di `src/ConnPool.cpp:244-247`:
```cpp
        std::string who = m_worker.empty() ? m_wallet : (m_wallet + "." + m_worker);
        std::string req =
            std::string("{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{") +
            "\"login\":\"" + who + "\",\"pass\":\"x\",\"agent\":\"xcb-conn/1.0\"}}\n";
```

Di `src/ConnPool.cpp:522-527`:
```cpp
        std::string req =
            std::string("{\"id\":") + std::to_string(reqId) +
            ",\"jsonrpc\":\"2.0\",\"method\":\"submit\",\"params\":{" +
            "\"id\":\""     + m_loginId + "\"," +
            "\"job_id\":\"" + job.id    + "\"," +
            "\"nonce\":\""  + nonceHex  + "\"," +
            "\"result\":\"" + resultHex + "\"," +
            "\"algo\":\""   + job.algo  + "\"}}\n";
```

Di `src/ConnRpc.cpp:521`:
```cpp
        std::string body = "{\"jsonrpc\":\"1.0\",\"id\":\"1\",\"method\":\"validateaddress\",\"params\":[\""
                         + address + "\"]}";
```

### Analisis Masalah & Dampak

- Berbeda dengan `StratumClient.cpp:16-30` yang sudah menggunakan `json_escape()`, `ConnPool` dan `ConnRpc` menggabungkan string (`m_wallet`, `m_worker`, `m_loginId`, `job.id`, `job.algo`, `address`) langsung ke dalam template string JSON.
- Jika input konfigurasi (`WORKER`, `WALLET`, `CONN_ADDRESS`) atau respons pool (`job.id`, `m_loginId`) mengandung tanda kutip `"`, backslash `\`, atau karakter kontrol, payload JSON menjadi tidak valid dan ditolak oleh server tanpa pesan diagnostik yang jelas.

### Rekomendasi Perbaikan

Pindahkan `json_escape()` ke header utilitas bersama (mis. `src/JsonUtil.hpp` atau di masing-masing TU) dan bungkus semua nilai string dinamis saat membangun payload JSON di `ConnPool` dan `ConnRpc`.

---

## M3. `StratumClient` Belum Mendukung TLS & Tidak Mengiterasi Seluruh Hasil `getaddrinfo()` (Termasuk `ConnRpc`)

- **Tingkat Keparahan:** Medium
- **Lokasi:**
  - `src/StratumClient.cpp:257-292`
  - `src/ConnRpc.cpp:330-338`

### Bukti Kode

Di `src/StratumClient.cpp:262-284`:
```cpp
    struct addrinfo* res = nullptr;
    if (getaddrinfo(m_host.c_str(), std::to_string(m_port).c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    m_sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    ...
    int rc = ::connect(m_sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
```

### Analisis Masalah & Dampak

1. **Absennya TLS di `StratumClient`:** Seluruh komunikasi stratum XCB (`eth_submitLogin`, `eth_getWork`, `eth_submitWork`) dikirim dalam bentuk *plaintext* TCP, sehingga rentan terhadap *man-in-the-middle* (penggantian wallet saat login atau pembajakan share).
2. **Hanya Mencoba Record DNS Pertama (`res`):** Baik `StratumClient::openSocket()` maupun `ConnRpc::httpPost()` hanya mencoba `res` pertama dari `getaddrinfo()`. Jika hostname mengembalikan beberapa record (misalnya `AAAA` IPv6 di urutan pertama pada host tanpa konektivitas IPv6, diikuti `A` IPv4), koneksi langsung gagal tanpa mencoba record berikutnya (berbeda dengan `ConnPool.cpp:123` yang sudah mengiterasi `for (struct addrinfo* ai = res; ai; ai = ai->ai_next)`).

### Rekomendasi Perbaikan

- Ubah `StratumClient::openSocket()` dan `ConnRpc::httpPost()` agar mengiterasi seluruh linked list `addrinfo` hasil `getaddrinfo()`.
- Ekstrak lapisan socket + OpenSSL dari `ConnPool` menjadi transport bersama agar `StratumClient` juga dapat menggunakan TLS (`stratum+ssl://` atau flag `--tls`).

---

## M4. `ops/fleet/deploy.sh` Mengandalkan `grep 'HASIL=OK'` Tanpa Memeriksa Exit Code & Tidak Membersihkan Direktori `mktemp -d`

- **Tingkat Keparahan:** Medium
- **Lokasi:**
  - `ops/fleet/deploy.sh:126`
  - `ops/fleet/deploy.sh:219-244`

### Bukti Kode

Di `ops/fleet/deploy.sh:219-243`:
```bash
deploy_one() {
    local name="$1" addr="$2" stamp="$3"
    local out
    out=$(run_apply "$addr" install "$stamp" "$REMOTE_DIR" "$EXPECT" 2>&1) || true
    if printf '%s' "$out" | grep -q 'HASIL=OK'; then
        log "[$name] DEPLOY OK ($(printf '%s' "$out" | grep '^HASIL=' | tail -n1))"
        return 0
    fi
    ...
}

rollback_one() {
    local name="$1" addr="$2"
    local out
    out=$(run_apply "$addr" rollback "$REMOTE_DIR" 2>&1) || true
    if printf '%s' "$out" | grep -q 'svc=active'; then
        log "[$name] ROLLBACK OK"
    ...
}
```

### Analisis Masalah & Dampak

1. Karena `|| true` digunakan pada `out=$(run_apply ...)`, exit code dari SSH dan `apply.sh` diabaikan. Di `ops/fleet/apply.sh:108-109`, output `journalctl` dicetak ke stdout; bila log lama atau pesan lain memuat teks `HASIL=OK` atau `svc=active`, `deploy.sh` dapat menganggap deploy/rollback berhasil walaupun `apply.sh` keluar dengan kode non-zero.
2. Di `prepare_artifact()` (`ops/fleet/deploy.sh:126`), ketika `--from-ci` dipakai, `FROM=$(mktemp -d)` membuat direktori temporer di `/tmp` yang tidak pernah dihapus setelah skrip selesai.

### Rekomendasi Perbaikan

1. Simpan exit code secara eksplisit:
   ```bash
   local rc=0
   out=$(run_apply "$addr" install "$stamp" "$REMOTE_DIR" "$EXPECT" 2>&1) || rc=$?
   if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q '^HASIL=OK '; then
   ```
   Dan untuk `rollback_one()`, periksa `[ "$rc" -eq 0 ]` serta `grep -q '^ROLLBACK_TO=.* svc=active$'`.
2. Tambahkan `trap` pembersihan untuk direktori temporer `FROM` ketika dibuat dengan `mktemp -d`.

---

## M5. Unit Systemd `ops/systemd/syscached.service` Berjalan Sebagai `root` Tanpa Sandboxing

- **Tingkat Keparahan:** Medium
- **Lokasi:** `ops/systemd/syscached.service:7-19`

### Bukti Kode

```ini
[Service]
Type=simple
EnvironmentFile=/etc/xcb/miner.env
ExecStart=/opt/sys-Caching/xcb -a ${ALGO} \
    --conn-pool ${CONN_POOL} -u ${WALLET} --worker ${WORKER} -t ${THREADS}
Restart=always
RestartSec=10
Nice=-5
LimitMEMLOCK=infinity
StandardOutput=journal
StandardError=journal
```

### Analisis Masalah & Dampak

- Tanpa direktif `User=`, systemd menjalankan binary `xcb` sebagai `root` (`UID 0`).
- Binary ini membuka koneksi jaringan keluar ke pool pihak ketiga dan mem-parsing respons JSON dari jaringan. Jika terdapat celah keamanan pada parser atau library OpenSSL, penyerang mendapatkan eksekusi kode sebagai `root`.

### Rekomendasi Perbaikan

Tambahkan pembatasan hak akses systemd pada `ops/systemd/syscached.service` (setelah memastikan izin baca `/etc/xcb/miner.env` dan alokasi hugepages di host):
```ini
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
CapabilityBoundingSet=CAP_IPC_LOCK CAP_SYS_NICE
AmbientCapabilities=CAP_IPC_LOCK CAP_SYS_NICE
```

---

## M6. `mine.sh` Mengasumsikan Ukuran Dataset 2.5 GiB untuk Semua Algoritma

- **Tingkat Keparahan:** Medium
- **Lokasi:** `mine.sh:42-67`

### Bukti Kode

```bash
    # Calculate hugepages based on mode
    # Full mode: ~2.5GB dataset + 256MB cache = ~1600 pages (2MB each)
    # Light mode: ~256MB cache only = ~200 pages
    if [ "$LIGHT_MODE" -eq 1 ]; then
        NEED_PAGES=$(( (256 + 128) * 1024 / PAGESZ_KB ))
    else
        NEED_PAGES=$(( (2560 + 640) * 1024 / PAGESZ_KB ))
    fi
```

### Analisis Masalah & Dampak

- Saat `mine.sh` dijalankan untuk menambang XCB (`randomy`, yang merupakan algoritma default bila `-a conn` tidak diberikan), dataset `RandomY` hanya berukuran **~264 MB** (+ 256 MB cache = ~520 MB total, atau sekitar **300 hugepages** ukuran 2 MB).
- Karena `mine.sh` selalu menghitung `NEED_PAGES` sebesar `(2560 + 640) MB` = **1600 hugepages (3.2 GB)**, pada VPS dengan RAM 2–4 GB skrip akan mengunci sebagian besar RAM menjadi hugepages yang tidak pernah terpakai oleh `randomy` atau mencetak peringatan palsu `Not enough RAM for full dataset`.

### Rekomendasi Perbaikan

Deteksi algoritma dari argumen CLI (`-a conn` / `--algo conn`) atau environment `ALGO=conn` di dalam `mine.sh`:
- Jika `conn` (RandomX V2): `NEED_PAGES` untuk full mode = `$(( (2080 + 256 + 128) * 1024 / PAGESZ_KB ))` (~1232 halaman).
- Jika `randomy` (default): `NEED_PAGES` untuk full mode = `$(( (264 + 256 + 128) * 1024 / PAGESZ_KB ))` (~324 halaman).
