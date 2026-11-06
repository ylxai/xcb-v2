# 3. Temuan Prioritas Rendah, Hygiene Kode & Catatan Positif

Dokumen ini merinci temuan berprioritas rendah (`L1` – `L5`), rekomendasi penambahan pengujian otomatis, serta daftar aspek arsitektur yang sudah terverifikasi baik.

---

## L1. Potensi *Null Pointer Dereference* pada `getpwuid(getuid())` di `Config.cpp`

- **Tingkat Keparahan:** Low
- **Lokasi:** `src/Config.cpp:70-75`

### Bukti Kode

```cpp
    const char* home = std::getenv("HOME");
    if (!home) {
        home = getpwuid(getuid())->pw_dir;
    }
    return std::string(home) + "/.Custom-miner";
```

### Analisis Masalah & Dampak

- Jika variabel environment `HOME` tidak di-set dan UID proses tidak terdaftar di `/etc/passwd` (kasus umum pada container Docker minimal atau unit systemd dengan `DynamicUser=yes`), `getpwuid(getuid())` mengembalikan `nullptr`.
- Mengakses `->pw_dir` secara langsung akan menyebabkan *segmentation fault* saat startup.
- Bandingkan dengan `src/ConnRpc.cpp:200-204` yang sudah menangani kondisi ini secara aman:
  ```cpp
  if (struct passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
      return std::string(pw->pw_dir);
  ```

### Rekomendasi Perbaikan

Gunakan pengecekan pointer sebelum mengakses `pw_dir`, dan berikan fallback `"/tmp"` bila keduanya tidak tersedia:
```cpp
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (struct passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) {
            home = pw->pw_dir;
        } else {
            home = "/tmp";
        }
    }
```

---

## L2. Potensi *Out-of-Bounds Read* 1 Byte pada Trailing Backslash JSON di `StratumClient.cpp`

- **Tingkat Keparahan:** Low
- **Lokasi:** `src/StratumClient.cpp:34-40` dan `src/StratumClient.cpp:65-69`

### Bukti Kode

Di `src/StratumClient.cpp:34-40` (`json_find_str`):
```cpp
    std::string val;
    size_t end = q1 + 1;
    while (end < json.size() && json[end] != '"') {
        if (json[end] == '\\') { val += json[end + 1]; end += 2; continue; }
        val += json[end++];
    }
```

Hal serupa juga ada di `json_result_array()` (`src/StratumClient.cpp:65-69`).

### Analisis Masalah & Dampak

- Jika pesan JSON yang terpotong/malformed berakhir tepat pada karakter backslash `'\\'` tanpa kutip penutup (`end == json.size() - 1`), ekspresi `json[end + 1]` membaca byte di indeks `json.size()` (terminator `\0` pada `std::string` C++11+, namun melompatkan `end` menjadi `json.size() + 1`).
- Bandingkan dengan `src/ConnPool.cpp:365` dan `src/ConnRpc.cpp:427` yang sudah memeriksa `if (json[p] == '\\' && p + 1 < json.size())`.

### Rekomendasi Perbaikan

Ubah kondisi di `json_find_str()` dan `json_result_array()` menjadi:
```cpp
if (json[end] == '\\' && end + 1 < json.size()) {
    val += json[end + 1];
    end += 2;
    continue;
}
```

---

## L3. Teks Bantuan `--help` Masih Menyebut Nama Binary Lama `rk_sysd`

- **Tingkat Keparahan:** Low
- **Lokasi:**
  - `src/Config.cpp:417`
  - `src/Miner.cpp:278`

### Bukti Kode

Di `src/Config.cpp:417`:
```cpp
void Config::printHelp() {
    std::cout <<
        "Usage: rk_sysd [options]\n\n"
```

### Analisis Masalah & Dampak

- Target binary pada `CMakeLists.txt:90` adalah `xcb`, namun output `xcb --help` masih menampilkan `Usage: rk_sysd [options]`.

### Rekomendasi Perbaikan

Perbarui teks bantuan di `Config::printHelp()` menjadi `Usage: xcb [options]`.

---

## L4. Titik Awal Nonce di `ConnMiner` Selalu Mulai dari `0`

- **Tingkat Keparahan:** Low
- **Lokasi:** `src/ConnMiner.cpp:226`

### Bukti Kode

Di `src/ConnMiner.cpp:220-227`:
```cpp
    struct Work {
        uint64_t height = 0;
        std::string jobId;
        std::array<uint8_t, 80> header{};
        std::array<uint8_t, 32> targetLE{};
        std::array<uint8_t, 32> seedHash{};
        bool hasSeed = false;
        std::atomic<uint32_t> nonce{0};
```

### Analisis Masalah & Dampak

- Pada mode **solo RPC** (`--conn-rpc`), beberapa instance miner yang terhubung ke node `connectcoind` yang sama pada detik yang sama (`curtime` identik dan daftar transaksi identik) dapat menyusun `header` 80-byte yang identik.
- Karena `work->nonce` selalu dimulai dari `0`, beberapa mesin bisa menghitung rentang nonce yang sama persis.
- Pada `Miner.cpp:357` (backend `randomy`), titik awal nonce diacak per job menggunakan `std::mt19937_64 m_nonceRng{std::random_device{}()}`.

### Rekomendasi Perbaikan

Inisialisasi `work->nonce` pada `ConnMiner::Impl::buildWork()` (atau saat `installPoolWork()` jika `nonceOffset` tidak membagi habis ruang nonce per worker) menggunakan generator angka acak `std::random_device` / `std::mt19937`.

---

## L5. Cakupan Pengujian Otomatis (`ctest`) Masih Terbatas pada `--selftest`

- **Tingkat Keparahan:** Low
- **Lokasi:**
  - `CMakeLists.txt:139-140`
  - `src/main.cpp:202-204`

### Analisis Masalah & Dampak

- Saat ini `ctest` hanya menjalankan `xcb --selftest`, yang memverifikasi:
  1. Vektor SHA3-512 (`"abc"`).
  2. Satu vektor RandomX V2 mode LIGHT.
- Komponen yang rentan terhadap regresi — seperti `Config::parse()`, `ConnPool::parseJobDoc()`, `ConnMiner::targetFromCompact()`, `ConnMiner::cashAddrToScriptPubKey()`, dan `StratumClient` JSON parser — belum memiliki unit test otomatis.

### Rekomendasi Perbaikan

Tambahkan suite test ringan di `CMakeLists.txt` (atau perluas `--selftest`) untuk menguji:
1. Konversi CashAddr ConnectCoin (`conn:...`) ke `scriptPubKey` P2PKH/P2SH.
2. Konversi `nBits` (`targetFromCompact`) ke target 256-bit little-endian dan perbandingan `hashMeetsTarget`.
3. Parsing respons JSON `login` dan `job` dari `ConnPool` (termasuk kasus *escaped characters* dan input terpotong).
4. Precedence konfigurasi CLI vs Environment di `Config::parse()`.

---

## Catatan Positif (Bagian yang Sudah Terverifikasi Baik)

Selama audit dilakukan, area-area berikut telah diperiksa dan terbukti sudah diimplementasikan dengan benar:

1. **Pencegahan Kehabisan HugePages Saat Rotasi Dataset (`src/ConnMiner.cpp:544-563`):**
   `ensureSeed()` memanggil `m_engine.reset()` untuk membebaskan dataset 2 GiB lama sebelum mengalokasikan `Engine` baru, lalu menaikkan `m_engineGen` di bawah `m_engineMu` agar seluruh thread worker membuat ulang `Vm` terhadap dataset baru tanpa *use-after-free*.
2. **Keamanan Thread pada Socket OpenSSL (`src/ConnPool.cpp:69-74, 152-227`):**
   `sendRaw()` dan `readLine()` tidak memegang `m_ioMu` selama `poll()` menunggu data jaringan, melainkan hanya saat memanggil `SSL_read` / `SSL_write`. Ini mencegah deadlock di mana thread `submit()` tertahan oleh thread `readerLoop()` yang sedang idle menunggu job baru dari pool.
3. **Verifikasi Sertifikat & Hostname TLS (`src/ConnPool.cpp:102-119, 134-149`):**
   Ketika `m_tlsVerify` aktif (default), `ConnPool` mengaktifkan `SSL_CTX_set_verify(SSL_CTX, SSL_VERIFY_PEER, nullptr)`, memvalidasi IP atau DNS hostname dengan `X509_VERIFY_PARAM_set1_ip_asc` / `SSL_set1_host`, serta memeriksa `SSL_get_verify_result(ssl) == X509_V_OK`.
4. **Hot Path SHA3-512 & RandomY/RandomX V2 (`src/Sha3_512.cpp`, `src/Miner.cpp`, `src/ConnMiner.cpp`):**
   Fungsi `Sha3_512::hash()` bekerja sepenuhnya di stack tanpa `std::vector` atau `malloc`. Worker thread melakukan batching update statistik setiap `256` hash (`BATCH_SIZE = 256`) dengan `std::memory_order_relaxed` sehingga tidak ada *cache line bouncing* pada counter global di setiap hash.
