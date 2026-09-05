// ptrace-hook.c — Interceptor syscall klasik via PTRACE_SYSCALL
//
// Teknik: parent me-ptrace anak, kernel menghentikan anak di SETIAP syscall
// (2 stop: entry + exit). Di stop entry kita bisa membaca/mengubah register;
// set orig_rax = -1 membatalkan syscall (kernel balikin -ENOSYS tanpa
// mengeksekusi handler asli). Ini "hooking" paling primitif tapi 100% nyata.
//
// Mode:
//   --block-path STR   batalkan openat() pada path yang mengandung STR
//   --anti-kill        batalkan kill(pid_anak, SIGKILL) -> anak kebal SIGKILL
//   --quiet            tanpa mode lain: tetap log semua syscall
//
// Usage: ptrace-hook [--block-path STR] [--anti-kill] -- PROGRAM [ARGS...]
//
// Build: gcc -O2 -o ptrace-hook ptrace-hook.c
// (tidak butuh root untuk me-ptrace proses child sendiri)

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

// Nomor syscall x86-64 yang relevan untuk demo
#define SYS_openat   257
#define SYS_execve   59
#define SYS_kill     62
#define SYS_write    1
#define SYS_exit     60
#define SYS_exit_g   231

static const char *block_str = NULL;
static int anti_kill = 0;
static pid_t child_pid;

static const char *syscall_name(long nr) {
    switch (nr) {
    case 0:   return "read";
    case 1:   return "write";
    case 2:   return "open";
    case 3:   return "close";
    case 9:   return "mmap";
    case 39:  return "getpid";
    case 59:  return "execve";
    case 60:  return "exit";
    case 62:  return "kill";
    case 63:  return "uname";
    case 72:  return "fcntl";
    case 78:  return "getdents";
    case 217: return "getdents64";
    case 231: return "exit_group";
    case 257: return "openat";
    case 262: return "newfstatat";
    default:  return "?";
    }
}

// Baca string dari memory proses anak (8 byte per PTRACE_PEEKDATA).
static int read_cstr(pid_t pid, unsigned long addr, char *out, size_t cap) {
    size_t i = 0;
    while (i + 8 <= cap) {
        errno = 0;
        long v = ptrace(PTRACE_PEEKDATA, pid, (void *)addr + i, NULL);
        if (v == -1 && errno != 0) break; // halaman tidak terbaca
        memcpy(out + i, &v, 8);
        if (memchr(out + i, '\0', 8)) return 1;
        i += 8;
    }
    out[cap - 1] = '\0';
    return (i > 0);
}

static void run_child(char *argv[]) {
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
        perror("PTRACE_TRACEME");
        _exit(127);
    }
    raise(SIGSTOP); // tunggu parent siap
    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
}

int main(int argc, char *argv[]) {
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--block-path") && argi + 1 < argc) {
            block_str = argv[++argi];
        } else if (!strcmp(argv[argi], "--anti-kill")) {
            anti_kill = 1;
        } else if (!strcmp(argv[argi], "--")) {
            argi++;
            break;
        } else {
            fprintf(stderr, "arg tak dikenal: %s\n", argv[argi]);
            return 2;
        }
        argi++;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [--block-path STR] [--anti-kill] -- PROGRAM [ARGS...]\n", argv[0]);
        return 2;
    }

    child_pid = fork();
    if (child_pid == 0) run_child(&argv[argi]);

    // anak berhenti di SIGSTOP dari raise() — lanjutkan
    int st;
    waitpid(child_pid, &st, 0);
    ptrace(PTRACE_SETOPTIONS, child_pid, NULL,
           (void *)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
    ptrace(PTRACE_SYSCALL, child_pid, NULL, NULL);

    struct user_regs_struct regs;
    unsigned long syscall_nr = -1;
    int blocked_this = 0;

    printf("[hook] memantau pid %d: %s%s%s\n", child_pid,
           block_str ? " block-path=" : "", block_str ? block_str : "",
           anti_kill ? " anti-kill" : "");

    for (;;) {
        if (waitpid(child_pid, &st, 0) == -1) { perror("waitpid"); break; }
        if (WIFEXITED(st)) {
            printf("[hook] anak keluar status=%d\n", WEXITSTATUS(st));
            break;
        }
        if (WIFSIGNALED(st)) {
            printf("[hook] anak dibunuh sinyal=%d\n", WTERMSIG(st));
            break;
        }
        if (ptrace(PTRACE_GETREGS, child_pid, NULL, &regs) == -1) {
            perror("GETREGS");
            break;
        }

        if (WSTOPSIG(st) == (SIGTRAP | 0x80)) {
            // ============ ENTRY syscall ============
            syscall_nr = (unsigned long)regs.orig_rax;
            blocked_this = 0;

            if (syscall_nr == SYS_openat) {
                char path[256];
                if (read_cstr(child_pid, (unsigned long)regs.rsi, path, sizeof(path))) {
                    if (block_str && strstr(path, block_str)) {
                        printf("[hook] BLOKIR openat(%s) <- mengandung '%s'\n",
                               path, block_str);
                        regs.orig_rax = -1; // kernel: return -ENOSYS
                        blocked_this = 1;
                        ptrace(PTRACE_SETREGS, child_pid, NULL, &regs);
                    } else {
                        printf("[hook] openat(%s, flags=0x%lx)\n",
                               path, (unsigned long)regs.rdx);
                    }
                }
            } else if (syscall_nr == SYS_kill && anti_kill) {
                long target = (long)regs.rdi, sig = (long)regs.rsi;
                if (target == child_pid && sig == SIGKILL) {
                    printf("[hook] BLOKIR kill(%ld, SIGKILL) <- anti-kill\n", target);
                    regs.orig_rax = -1;
                    blocked_this = 1;
                    ptrace(PTRACE_SETREGS, child_pid, NULL, &regs);
                }
            } else if (syscall_nr == SYS_execve) {
                char p[256];
                if (read_cstr(child_pid, (unsigned long)regs.rdi, p, sizeof(p)))
                    printf("[hook] execve(%s)\n", p);
            } else if (syscall_nr != SYS_write && syscall_nr != SYS_exit &&
                       syscall_nr != SYS_exit_g) {
                printf("[hook] syscall %ld (%s)\n", syscall_nr,
                       syscall_name(syscall_nr));
            }
        } else if (syscall_nr == SYS_openat && blocked_this) {
            // ============ EXIT dari syscall yang kita blokir ============
            printf("[hook] openat dibatalkan, return=0x%llx (-ENOSYS=%d)\n",
                   (unsigned long long)regs.rax, -ENOSYS);
            blocked_this = 0;
        }
        syscall_nr = -1;

        ptrace(PTRACE_SYSCALL, child_pid, NULL, NULL);
    }
    return 0;
}
