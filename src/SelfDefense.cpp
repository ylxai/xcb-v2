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

void scrub_environ() {
    if (!environ) return;

    // Hanya variabel yang isinya identitas/rahasia yang dihapus. Area
    // environment tidak dikosongkan seluruhnya: HOME, LOG_SHARES, dan
    // CONN_TLS_INSECURE dibaca jauh setelah Config::parse (dipakai untuk
    // ekspansi "~" path cookie, verbose share, dan verifikasi sertifikat
    // pool). Menghapus semuanya membuat fitur itu mati diam-diam.
    //
    // Yang dihapus hanya byte nilainya; nama variabel tetap utuh supaya
    // getenv() untuk variabel lain tidak terganggu dan supaya batas
    // /proc/<pid>/environ tidak bergeser.
    static const char* const kSecret[] = {
        "WALLET",     // alamat reward
        "CONN_ADDRESS",
        "CONN_PASS",  // kredensial RPC CONN
        "CONN_COOKIE",
        "CONN_USER",
        "CONN_RPC",   // bisa memuat user:pass di dalam URL
    };

    for (char** e = environ; *e; e++) {
        const char* eq = strchr(*e, '=');
        if (!eq || eq[1] == '\0') continue;
        const size_t nlen = static_cast<size_t>(eq - *e);
        for (const char* name : kSecret) {
            if (strlen(name) != nlen || strncmp(*e, name, nlen) != 0) continue;
            // Nolkan nilai termasuk terminator-nya: getenv() lalu mengembalikan
            // string kosong, dan isi aslinya hilang dari area environment yang
            // dibaca /proc/<pid>/environ.
            memset(const_cast<char*>(eq + 1), 0, strlen(eq + 1));
            break;
        }
    }
}

}  // namespace xcb::selfdefense
