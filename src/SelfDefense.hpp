#pragma once

namespace xcb::selfdefense {

// Aktifkan proteksi diri (dipanggil dari main saat XCB_SELF_DEFENSE):
//   1. Deteksi debugger aktif (TracerPid != 0) -> exit dengan pesan jelas
//   2. PR_SET_DUMPABLE=0 -> /proc/<pid>/mem tidak bisa dibaca user lain,
//      core dump dimatikan
//   3. RLIMIT_CORE=0      -> tidak ada core file
//   4. PTRACE_TRACEME     -> setelah ini tidak ada proses lain yang bisa
//      attach ptrace (strace/gdb -p gagal EPERM)
void init();

// Ganti title proses di ps (/proc/<pid>/comm + cmdline). Menimpa area argv
// asli: argumen dan env asli tidak lagi terlihat di /proc/<pid>/cmdline.
// Panggil PALING AKHIR (setelah semua getenv) karena env ikut tertimpa.
void set_process_title(int argc, char** argv, const char* title);

}  // namespace xcb::selfdefense
