#pragma once

namespace xcb::selfdefense {

// Aktifkan proteksi diri (dipanggil dari main saat XCB_SELF_DEFENSE):
//   1. Deteksi debugger aktif (TracerPid != 0) -> exit dengan pesan jelas
//   2. PR_SET_DUMPABLE=0 -> /proc/<pid>/mem tidak bisa dibaca user lain,
//      core dump dimatikan
//   3. RLIMIT_CORE=0      -> tidak ada core file
// Tidak memakai PTRACE_TRACEME: dengan systemd sebagai parent, SIGTERM berhenti
// di TASK_TRACED dan stop menggantung sampai TimeoutStopSec.
void init();

// Scrub terarah: hanya NILAI variabel dalam daftar rahasia yang dinolkan di
// tempat; nama variabel dan batas record /proc/<pid>/environ tetap utuh.
// HOME, LOG_SHARES, dan CONN_TLS_INSECURE dibaca setelah Config::parse, jadi
// mengosongkan seluruh area environment mematikan fitur itu diam-diam.
// Panggil setelah Config::parse (setelah semua getenv) dan sebelum thread lain
// membaca environment.
void scrub_environ();

}  // namespace xcb::selfdefense
