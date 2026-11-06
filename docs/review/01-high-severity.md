# 1. Temuan Prioritas Tinggi (High Severity)

Dokumen ini memuat rincian teknis, analisis akar masalah, skenario dampak, dan rekomendasi perbaikan untuk seluruh temuan berprioritas tinggi (`H1` – `H4`).

---

## H1. `set_process_title()` Menghapus `argv` dan `environ` Sebelum `Config::parse()` Dijalankan

- **Tingkat Keparahan:** High
- **Lokasi:**
  - `src/main.cpp:195-200`
  - `src/SelfDefense.cpp:61-90`

### Bukti Kode

Di `src/main.cpp:193-200`:
```cpp
int main(int argc, char** argv) {
#ifdef XCB_SELF_DEFENSE
    if (const char* t = std::getenv("XCB_TITLE"); t && *t)
        xcb::selfdefense::set_process_title(argc, argv, t);
    xcb::selfdefense::init();
#endif
    MinerConfig cfg = Config::parse(argc, argv);
```

Di `src/SelfDefense.cpp:61-90`:
```cpp
void set_process_title(int argc, char** argv, const char* title) noexcept {
    if (!title || !*title || argc <= 0 || !argv || !argv[0]) return;

    (void)prctl(PR_SET_NAME,
                reinterpret_cast<unsigned long>(title), 0UL, 0UL, 0UL);

    size_t total = 0;
    for (int i = 0; i < argc; ++i) {
        if (argv[i]) total += std::strlen(argv[i]) + 1;
    }
    if (environ) {
        for (char** e = environ; *e; ++e) {
            total += std::strlen(*e) + 1;
        }
    }
    if (total == 0) return;

    std::memset(argv[0], 0, total);
    std::strncpy(argv[0], title, total - 1);
    for (int i = 1; i < argc; ++i) argv[i] = nullptr;
}
```

### Analisis Masalah & Dampak

1. **Penghapusan Argumen CLI & Environment Sebelum Dibaca:**
   `std::memset(argv[0], 0, total)` dan `argv[i] = nullptr` dipanggil pada baris `197` di `main.cpp`, sedangkan `Config::parse(argc, argv)` baru dipanggil pada baris `200`. Akibatnya:
   - Seluruh argumen baris perintah (`-a conn`, `--conn-pool`, `-u`, `-t`, dll.) menjadi `nullptr` atau string kosong ketika `Config::parse()` membacanya.
   - Seluruh isi string di `environ` (`WALLET=...`, `CONN_POOL=...`, `XCB_LOG=...`) ikut tertimpa byte `\0` sehingga `std::getenv()` mengembalikan `nullptr` atau string kosong.
2. **Asumsi Kontiguitas `argv` dan `environ` (`memset` Over-write):**
   Menghitung `total` dengan menjumlahkan `strlen(argv[i]) + 1` dan `strlen(*e) + 1` mengasumsikan semua string `argv` dan `environ` selalu bersebelahan secara rapat di memori dimulai dari `argv[0]`. Jika sebelum pemanggilan terjadi realokasi `environ` (misalnya oleh konstruktor global library atau `setenv`) atau terdapat *padding* antar blok, `std::memset(argv[0], 0, total)` akan menulis melampaui batas blok `argv` (*buffer over-write*).

### Rekomendasi Perbaikan

1. Pindahkan pemanggilan `xcb::selfdefense::set_process_title()` dan `xcb::selfdefense::init()` ke **setelah** `Config::parse(argc, argv)` dan `lg::init()` selesai membaca `argv` serta `environ` (atau simpan nilai `XCB_TITLE` terlebih dahulu, jalankan `Config::parse` & `lg::init`, baru panggil `set_process_title`).
2. Di dalam `set_process_title()`, hitung hanya rentang memori yang benar-benar kontigu dari `argv[0]`:
   ```cpp
   char* start = argv[0];
   char* end   = start + std::strlen(start) + 1;
   for (int i = 1; i < argc; ++i) {
       if (argv[i] == end) {
           end += std::strlen(argv[i]) + 1;
       }
   }
   size_t argSpan = static_cast<size_t>(end - start);
   std::memset(start, 0, argSpan);
   std::strncpy(start, title, argSpan - 1);
   for (int i = 1; i < argc; ++i) argv[i] = nullptr;

   // Bersihkan variabel environ sensitif secara individual tanpa mengasumsikan kontiguitas dengan argv[0]
   if (environ) {
       for (char** e = environ; *e; ++e) {
           std::memset(*e, 0, std::strlen(*e));
       }
   }
   ```

---

## H2. `ptrace(PTRACE_TRACEME)` di `SelfDefense::init()` Membekukan Proses Saat Menerima Sinyal

- **Tingkat Keparahan:** High
- **Lokasi:** `src/SelfDefense.cpp:50-55`

### Bukti Kode

```cpp
    // Claim the single ptrace slot for our own process. Ignore return value:
    // fails under restricted seccomp/Yama profiles or when already traced;
    // neither is fatal to the workload.
    (void)ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
```

### Analisis Masalah & Dampak

- Di kernel Linux, `ptrace(PTRACE_TRACEME, ...)` menjadikan *parent process* (contohnya `systemd`, `bash`, atau *container init*) sebagai *tracer* dari proses yang memanggilnya.
- Ketika sebuah proses berada dalam status *traced* (`PT_PTRACED`), **setiap sinyal** yang dikirimkan ke proses tersebut (kecuali `SIGKILL`) — termasuk `SIGTERM`, `SIGINT`, `SIGHUP`, `SIGWINCH`, maupun `SIGCHLD` — akan menginterupsi proses dan memasukkannya ke state `TASK_TRACED` (`T` pada `ps`/`top`), lalu memberi tahu *parent process* melalui `SIGCHLD`.
- Proses *tracee* tidak akan pernah mengeksekusi `signal_handler` miliknya sendiri sampai *parent process* memanggil `waitpid()` dan `ptrace(PTRACE_CONT, pid, 0, sig)`.
- Karena `systemd` atau shell biasa bukan debugger dan tidak memanggil `ptrace(PTRACE_CONT)`, proses miner akan langsung **membeku (hang)** begitu menerima sinyal apa pun, gagal melakukan *graceful shutdown*, dan akhirnya harus dipaksa mati dengan `SIGKILL` setelah timeout systemd habis.

### Rekomendasi Perbaikan

Hapus pemanggilan `ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)` dari `src/SelfDefense.cpp`. Pemanggilan `prctl(PR_SET_DUMPABLE, 0UL, ...)` pada baris `42` sudah memadai untuk mencegah proses non-root melakukan `PTRACE_ATTACH` serta memblokir pembacaan `/proc/<pid>/mem` dan `/proc/<pid>/environ` oleh user lain.

---

## H3. `Config::autoDetect()` Menghitung Kebutuhan RAM & HugePages dari Konstanta `RandomY` (264 MB), Bukan `RandomX V2` (~2.3 GiB) Saat `-a conn`

- **Tingkat Keparahan:** High
- **Lokasi:** `src/Config.cpp:607-648`

### Bukti Kode

```cpp
    // ── RAM & Hugepages: decide full-mem vs light, large-pages vs normal ─
    size_t ramMb    = getAvailableRamMb();
    size_t hpFreeMb = getFreeHugepagesMb();

    // RandomY dataset + cache size dynamically computed from randomx.h constants
    size_t datasetMb = (randomx_dataset_item_count() * RANDOMX_DATASET_ITEM_SIZE) / (1024 * 1024);
    size_t cacheMb   = (RANDOMX_ARGON_MEMORY * 1024) / (1024 * 1024); // Argon2 memory is in KiB

    size_t minRamForFullMem = datasetMb + cacheMb + 256; // +256 MB safety margin for OS
    size_t minHpForFullMem  = datasetMb + cacheMb;
    size_t minHpForLight    = cacheMb;
```

### Analisis Masalah & Dampak

- Header `randomx.h` di `external/randomy/src/randomx.h` telah dimodifikasi khusus untuk **RandomY** (CoreCoin XCB) di mana ukuran dataset adalah **~264 MB** dan cache adalah **256 MB** (`minRamForFullMem` ≈ 776 MB, `minHpForFullMem` ≈ 520 MB).
- Sebaliknya, ketika dijalankan dengan `-a conn` (`ConnMiner` menggunakan `xcb::rxv2::Engine` dari `external/randomx-v2`), ukuran dataset FAST adalah **2080 MiB** (2048 MiB + 32 MiB extra) dan cache **256 MiB** (~2336 MiB total hugepages, atau butuh minimal ~2600 MiB RAM tersedia).
- Dampak nyata di lapangan:
  1. Pada host dengan RAM tersedia 1–2 GiB, `autoDetect()` melihat `ramMb >= 776` sehingga menyetel `cfg.fullMem = true`. Saat `ConnMiner` mencoba mengalokasikan dataset 2080 MiB, alokasi gagal atau memicu *Out-Of-Memory (OOM) killer*.
  2. Pada host yang hanya memiliki 300–1000 *free hugepages* (600–2000 MB), `autoDetect()` melihat `hpFreeMb >= 520` sehingga menyetel `cfg.largePages = true` dan mengklaim hugepages siap, namun `rxv2_alloc_dataset(RXV2_FLAG_LARGE_PAGES)` gagal di tengah jalan karena hugepages tidak cukup untuk 2080 MiB + 256 MiB.

### Rekomendasi Perbaikan

Cabangkan perhitungan `datasetMb` dan `cacheMb` di `Config::autoDetect(MinerConfig& cfg)` berdasarkan `cfg.algo`:

```cpp
    size_t datasetMb = 0;
    size_t cacheMb   = 256;
    if (cfg.algo == "conn") {
        // RandomX V2 (ConnectCoin): 2080 MiB dataset + 256 MiB cache
        datasetMb = 2080;
        cacheMb   = 256;
    } else {
        // RandomY (XCB): dynamically computed from randomx.h constants (~264 MiB + 256 MiB)
        datasetMb = (randomx_dataset_item_count() * RANDOMX_DATASET_ITEM_SIZE) / (1024 * 1024);
        cacheMb   = (RANDOMX_ARGON_MEMORY * 1024) / (1024 * 1024);
    }
```

---

## H4. `ConnMiner` Belum Melakukan CPU Affinity Pinning dan Belum Memakai `Config::usableCpus()`

- **Tingkat Keparahan:** High
- **Lokasi:**
  - `src/ConnMiner.cpp:659-720`
  - Bandingkan dengan `src/Miner.cpp:506-524`

### Bukti Kode

Di `src/Miner.cpp:506-524` (backend `randomy` memiliki CPU pinning dan `setpriority`):
```cpp
void Miner::workerLoop(int id, randomx_vm* vm) {
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, -10);
    const auto& cpus = Config::usableCpus();
    if (!cpus.empty()) {
        int targetCpu = cpus[static_cast<size_t>(id) % cpus.size()];
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(targetCpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
```

Sedangkan di `src/ConnMiner.cpp:659-670` (backend `conn` tidak memiliki CPU pinning sama sekali):
```cpp
    void workerLoop(int id) {
        std::unique_ptr<rxv2::Vm> vm;
        uint64_t vmGen = 0;
        ...
```

### Analisis Masalah & Dampak

- `RandomX V2` (`conn`) sangat bergantung pada lokalitas cache L1/L2/L3 (2 MiB scratchpad per VM).
- Tanpa `pthread_setaffinity_np`, scheduler Linux bebas memigrasikan thread worker antar core atau antar CCX/NUMA node. Setiap migrasi core menyebabkan *cache miss* penuh pada scratchpad 2 MiB dan menurunkan hashrate serta efisiensi per core.
- Selain itu, jika proses dijalankan di dalam cgroup/container dengan subset CPU (`Config::usableCpus()`), `ConnMiner` tidak mem-pin worker ke daftar CPU yang diizinkan.

### Rekomendasi Perbaikan

Tambahkan blok `setpriority` dan `pthread_setaffinity_np` berbasis `Config::usableCpus()` di awal `ConnMiner::Impl::workerLoop(int id)` (serta sertakan header `<pthread.h>`, `<sched.h>`, dan `<sys/resource.h>` untuk `#ifdef __linux__`).
