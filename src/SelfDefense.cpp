#include "SelfDefense.hpp"

#include "Log.hpp"

#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

extern char** environ;

namespace xcb::selfdefense {

static int read_tracer_pid() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("TracerPid:", 0) == 0)
            return std::stoi(line.substr(10));
    }
    return 0;
}

void init() {
    // 1. Debugger sudah menempel (gdb/strace attach): TracerPid != 0.
    //    Keluar sebelum sempat mencuri config/wallet dari memori.
    int tp = read_tracer_pid();
    if (tp != 0) {
        lg::error("selfdefense", "Debugger terdeteksi (TracerPid=" +
                                     std::to_string(tp) + "), keluar");
        _exit(1);
    }

    // 2. Dumpable=0: /proc/<pid>/mem & /proc/<pid>/environ tidak bisa
    //    dibaca proses lain, dan tidak ada core dump.
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    // 3. Core dump dimatikan juga lewat rlimit (lapis kedua).
    struct rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rl);

    // 4. Tidak memakai PTRACE_TRACEME: dengan systemd sebagai parent, setiap
    //    sinyal (termasuk SIGTERM saat stop/restart) berhenti di
    //    TASK_TRACED dan tidak pernah sampai ke signal handler — unit
    //    menggantung sampai TimeoutStopSec lalu SIGKILL. PR_SET_DUMPABLE=0
    //    sudah menolak ptrace attach dari proses non-root; attach oleh root
    //    tetap mungkin dan itu batas yang memang tidak bisa ditutup.

    lg::info("selfdefense", "aktif (anti-debug: dumpable=0, core=0)");
}

void set_process_title(int argc, char** argv, const char* title) {
    if (!argv || !argv[0] || !title) return;

    // Salin dulu: title biasanya menunjuk ke string env (getenv), dan area
    // env ikut di-zero oleh memset di bawah — memakai title setelah memset
    // berarti prctl menerima string kosong.
    char buf[64];
    size_t tlen = strlen(title);
    if (tlen >= sizeof buf) tlen = sizeof buf - 1;
    memcpy(buf, title, tlen);
    buf[tlen] = '\0';

    // Luas area kontigu argv+env (Linux: argv dan env berurutan di stack).
    size_t total = 0;
    for (int i = 0; i < argc && argv[i]; i++) total += strlen(argv[i]) + 1;
    for (char** e = environ; *e; e++) total += strlen(*e) + 1;
    if (total == 0) return;

    size_t need = tlen + 1;
    if (need > total) need = total;

    // Zero seluruh area: argumen asli (host/wallet/pool) hilang dari
    // /proc/<pid>/cmdline dan /proc/<pid>/environ.
    memset(argv[0], 0, total);
    memcpy(argv[0], buf, need - 1);
    argv[0][need - 1] = '\0';

    // /proc/<pid>/comm (kolom CMD di ps) ikut diganti.
    prctl(PR_SET_NAME, buf, 0, 0, 0);
}

}  // namespace xcb::selfdefense
