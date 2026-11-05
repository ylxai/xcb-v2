// ptrace-hook.c — Interceptor syscall klasik via PTRACE_SYSCALL
//
// Teknik: parent me-ptrace anak, kernel menghentikan anak di SETIAP syscall
// (2 stop: entry + exit). Di stop entry kita bisa membaca/mengubah register;
// set orig_rax = -1 membatalkan syscall (kernel balikin -ENOSYS tanpa
// mengeksekusi handler asli). Ini "hooking" paling primitif tapi 100% nyata.
//
// Mode:
//   --block-path STR   batalkan openat() pada path yang mengandung STR
//   --anti-kill        batalkan kill/tgkill/tkill(pid_anak, SIGKILL)
//   --quiet            tanpa mode lain: tetap log semua syscall
//
// Usage: ptrace-hook [--block-path STR] [--anti-kill] -- PROGRAM [ARGS...]
//
// Build: gcc -O2 -o ptrace-hook ptrace-hook.c
// (tidak butuh root untuk me-ptrace proses child sendiri)
//
// CATATAN PENTING (state machine):
//   PTRACE_SYSCALL menghasilkan SIGTRAP|0x80 di ENTRY dan EXIT; keduanya
//   terlihat identik. Tanpa toggle entry/exit, syscall tercatat dua kali dan
//   "exit handler" tidak pernah jalan. Stop yang BUKAN syscall-stop adalah
//   signal-delivery-stop/group-stop: sinyalnya WAJIB diteruskan ke anak
//   lewat argumen keempat PTRACE_SYSCALL, kalau tidak SIGTERM/SIGINT ke anak
//   ditelan dan SIGSEGV berulang tanpa henti (proses hang).

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
#define SYS_tgkill   234
#define SYS_tkill    200
#define SYS_write    1
#define SYS_exit     60
#define SYS_exit_g   231
#define SYS_openat2  437
#define SYS_execveat 322

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
    case 200: return "tkill";
    case 217: return "getdents64";
    case 231: return "exit_group";
    case 234: return "tgkill";
    case 257: return "openat";
    case 262: return "newfstatat";
    case 322: return "execveat";
    case 437: return "openat2";
    default:  return "?";
    }
}

// Baca string dari memory proses anak (8 byte per PTRACE_PEEKDATA).
static int read_cstr(pid_t pid, unsigned long addr, char *out, size_t cap) {
    size_t i = 0;
    if (cap == 0) return 0;
    out[0] = '\0';
    while (i + 8 <= cap) {
        errno = 0;
        long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
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
    if (ptrace(PTRACE_SETOPTIONS, child_pid, NULL,
               (void *)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL)) == -1) {
        perror("PTRACE_SETOPTIONS");
        kill(child_pid, SIGKILL);
        return 1;
    }
    if (ptrace(PTRACE_SYSCALL, child_pid, NULL, NULL) == -1) {
        perror("PTRACE_SYSCALL");
        return 1;
    }

    struct user_regs_struct regs;
    int entering = 1;      // stop berikutnya: entry?
    int blocked = 0;       // syscall entry ini sudah dibatalkan
    long blocked_nr = -1;

    printf("[hook] memantau pid %d: %s%s%s\n", child_pid,
           block_str ? " block-path=" : "", block_str ? block_str : "",
           anti_kill ? " anti-kill" : "");

    for (;;) {
        if (waitpid(child_pid, &st, 0) == -1) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        if (WIFEXITED(st)) {
            printf("[hook] anak keluar status=%d\n", WEXITSTATUS(st));
            break;
        }
        if (WIFSIGNALED(st)) {
            printf("[hook] anak dibunuh sinyal=%d\n", WTERMSIG(st));
            break;
        }
        if (!WIFSTOPPED(st)) continue;

        const int sig = WSTOPSIG(st);

        // Syscall-stop (entry dan exit sama-sama SIGTRAP|0x80).
        if (sig == (SIGTRAP | 0x80)) {
            if (ptrace(PTRACE_GETREGS, child_pid, NULL, &regs) == -1) {
                perror("GETREGS");
                break;
            }

            if (entering) {
                const long nr = (long)regs.orig_rax;
                blocked = 0;
                blocked_nr = nr;

                if (nr == SYS_openat) {
                    char path[256];
                    if (read_cstr(child_pid, (unsigned long)regs.rsi, path, sizeof(path))) {
                        if (block_str && strstr(path, block_str)) {
                            printf("[hook] BLOKIR openat(%s) <- mengandung '%s'\n",
                                   path, block_str);
                            regs.orig_rax = -1; // kernel: return -ENOSYS
                            blocked = 1;
                            if (ptrace(PTRACE_SETREGS, child_pid, NULL, &regs) == -1)
                                perror("SETREGS");
                        } else {
                            printf("[hook] openat(%s, flags=0x%lx)\n",
                                   path, (unsigned long)regs.rdx);
                        }
                    }
                } else if ((nr == SYS_kill || nr == SYS_tgkill || nr == SYS_tkill) && anti_kill) {
                    long target = (nr == SYS_kill || nr == SYS_tkill)
                                      ? (long)regs.rdi
                                      : (long)regs.rsi; // tgkill: tgid, tid
                    long s = (nr == SYS_kill || nr == SYS_tkill)
                                 ? (long)regs.rsi
                                 : (long)regs.rdx;
                    if (target == child_pid && s == SIGKILL) {
                        printf("[hook] BLOKIR %s(%ld, SIGKILL) <- anti-kill\n",
                               syscall_name(nr), target);
                        regs.orig_rax = -1;
                        blocked = 1;
                        if (ptrace(PTRACE_SETREGS, child_pid, NULL, &regs) == -1)
                            perror("SETREGS");
                    }
                } else if (nr == SYS_execve || nr == SYS_execveat) {
                    char p[256];
                    const unsigned long ptr = (nr == SYS_execve) ? regs.rdi : regs.rsi;
                    if (read_cstr(child_pid, ptr, p, sizeof(p)))
                        printf("[hook] %s(%s)\n", syscall_name(nr), p);
                } else if (nr != SYS_write && nr != SYS_exit && nr != SYS_exit_g) {
                    printf("[hook] syscall %ld (%s)\n", nr, syscall_name(nr));
                }
                entering = 0;
            } else {
                // EXIT: rax berisi nilai balik (atau -ENOSYS bila dibatalkan).
                if (blocked) {
                    printf("[hook] %s dibatalkan, return=0x%llx (-ENOSYS=%d)\n",
                           syscall_name(blocked_nr), (unsigned long long)regs.rax, -ENOSYS);
                    blocked = 0;
                }
                entering = 1;
            }
            if (ptrace(PTRACE_SYSCALL, child_pid, NULL, NULL) == -1) {
                perror("PTRACE_SYSCALL");
                break;
            }
            continue;
        }

        // Signal-delivery-stop / group-stop / event-stop: TERUSKAN sinyalnya
        // ke anak. Mengirim 0 di sini akan menelan SIGTERM/SIGINT dst.
        const int deliver = (sig == SIGTRAP || sig == SIGSTOP || sig == SIGTSTP ||
                             sig == SIGTTIN || sig == SIGTTOU)
                                ? 0
                                : sig;
        if (ptrace(PTRACE_SYSCALL, child_pid, NULL, (void *)(long)deliver) == -1) {
            perror("PTRACE_SYSCALL(signal)");
            break;
        }
    }
    return 0;
}
