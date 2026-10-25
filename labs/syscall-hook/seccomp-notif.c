// seccomp-notif.c — Syscall hooking modern: SECCOMP user notification
//
// Prinsip: anak memasang filter BPF yang membuat KERNEL menghentikan syscall
// terpilih dan mengirim notifikasi ke supervisor (parent) via listener fd.
// Supervisor bisa menolak/meneruskan tiap syscall — persis hooking, tapi
// pakai mekanisme resmi kernel (dipakai Firejail, systemd, gVisor).
//
// Filter mencegat: open, openat, openat2, execve, execveat,
//                  kill, tgkill, tkill.
// Kebijakan supervisor:
//   - buka path di /etc dengan akses tulis ATAU O_CREAT      -> EACCES
//   - execve/execveat("/bin/sh")                             -> EACCES
//   - kill/tgkill/tkill(pid_anak, SIGKILL)                   -> EPERM
//   - sisanya diteruskan
//
// BATAS YANG JUJUR:
//   - Filter memakai nomor syscall x86-64 dan memeriksa arch; syscall 32-bit
//     (int 0x80) dan x32 akan memakai nomor berbeda dan ditolak (KILL_PROCESS)
//     alih-alih lolos.
//   - Kebijakan path berbasis string. Symlink/chdir/TOCTOU tidak sepenuhnya
//     tertutup: supervisor membaca path dari memory anak SEBELUM kernel
//     mengeksekusi syscall. Untuk boundary keamanan sejati gunakan LSM atau
//     openat2(RESOLVE_*) di proses target, bukan user-notif.
//   - pidfd_send_signal tidak dicegat (butuh menerjemahkan pidfd -> pid).
//
// Build: gcc -O2 -o seccomp-notif seccomp-notif.c
// Tidak butuh root (PR_SET_NO_NEW_PRIVS + NEW_LISTENER).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif

#define NR_open     2
#define NR_openat   257
#define NR_openat2  437
#define NR_execve   59
#define NR_execveat 322
#define NR_kill     62
#define NR_tgkill   234
#define NR_tkill    200

static pid_t child_pid;

// ---- filter BPF klasik: USER_NOTIF untuk syscall target ----
static int install_filter(void) {
    struct sock_filter filter[] = {
        // Tolak arsitektur lain: nomor syscall 32-bit/x32 berbeda sehingga
        // filter di bawah tidak berlaku untuknya.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        // load nomor syscall
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        // biarkan exit/exit_group langsung (jangan notif, nanti hang)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit_group, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        // target yang kita "hook"
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_open, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_openat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_openat2, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_execve, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_execveat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_kill, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_tgkill, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_tkill, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        // sisanya boleh
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        perror("prctl(NO_NEW_PRIVS)");
        return -1;
    }
    // NEW_LISTENER: kernel membuat fd listener. TSYNC TIDAK boleh digabung
    // dengan NEW_LISTENER (kernel mengembalikan EINVAL). Filter dipasang di
    // anak yang masih single-thread sebelum exec, jadi TSYNC tidak diperlukan.
    return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                        SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

// Kirim fd via SCM_RIGHTS (anak -> parent)
static void send_fd(int sock, int fd) {
    struct msghdr msg = {0};
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } u;
    struct iovec io = {.iov_base = (void *)"F", .iov_len = 1};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    if (sendmsg(sock, &msg, 0) == -1) perror("sendmsg");
}

static void child_main(int sock, char *argv[]) {
    int listener = install_filter();
    if (listener < 0) {
        perror("install_filter");
        _exit(127);
    }
    send_fd(sock, listener);
    close(listener);
    close(sock);
    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
}

// Baca string dari memory anak (untuk melihat argumen path di notif).
static int read_cstr(pid_t pid, unsigned long addr, char *out, size_t cap) {
    size_t i = 0;
    if (cap == 0) return 0;
    out[0] = '\0';
    while (i + 8 <= cap) {
        long chunk = 0;
        errno = 0;
        struct iovec local = {.iov_base = &chunk, .iov_len = 8};
        struct iovec remote = {.iov_base = (void *)(addr + i), .iov_len = 8};
        long n = syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0);
        if (n == -1) break;
        if (n < 8) {
            memcpy(out + i, &chunk, n > 0 ? (size_t)n : 0);
            out[cap - 1] = '\0';
            return 1;
        }
        memcpy(out + i, &chunk, 8);
        if (memchr(out + i, '\0', 8)) return 1;
        i += 8;
    }
    out[cap - 1] = '\0';
    return (i > 0);
}

// Baca struct open_how milik openat2 (flags di +0, mode di +8).
static int read_open_how(pid_t pid, unsigned long addr, unsigned long long *flags,
                         unsigned long long *mode) {
    struct {
        unsigned long long flags;
        unsigned long long mode;
        unsigned long long resolve;
    } how;
    struct iovec local = {.iov_base = &how, .iov_len = sizeof(how)};
    struct iovec remote = {.iov_base = (void *)addr, .iov_len = sizeof(how)};
    if (syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0) != (long)sizeof(how))
        return -1;
    *flags = how.flags;
    *mode = how.mode;
    return 0;
}

// Apakah path mengarah ke /etc? Path relatif diselesaikan terhadap cwd anak
// (/proc/<pid>/cwd) supaya `chdir("/etc"); open("passwd", O_WRONLY)` tidak
// lolos. Symlink tetap tidak ter-resolve (lihat batas di header).
static int targets_etc(pid_t pid, const char *path) {
    char full[512];
    if (!path || !*path) return 0;
    if (path[0] == '/') {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        char cwd[256];
        char link[64];
        snprintf(link, sizeof(link), "/proc/%d/cwd", (int)pid);
        const ssize_t n = readlink(link, cwd, sizeof(cwd) - 1);
        if (n <= 0) return 0;
        cwd[n] = '\0';
        snprintf(full, sizeof(full), "%s/%s", cwd, path);
    }
    return strncmp(full, "/etc/", 5) == 0 || strcmp(full, "/etc") == 0;
}

static const char *nr_name(int nr) {
    switch (nr) {
    case NR_open:     return "open";
    case NR_openat:   return "openat";
    case NR_openat2:  return "openat2";
    case NR_execve:   return "execve";
    case NR_execveat: return "execveat";
    case NR_kill:     return "kill";
    case NR_tgkill:   return "tgkill";
    case NR_tkill:    return "tkill";
    default:          return "?";
    }
}

int main(int argc, char *argv[]) {
    int argi = 1;
    if (argi < argc && !strcmp(argv[argi], "--")) argi++;
    if (argi >= argc) {
        fprintf(stderr, "usage: %s -- PROGRAM [ARGS...]\n", argv[0]);
        return 2;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) { perror("socketpair"); return 1; }

    child_pid = fork();
    if (child_pid == 0) child_main(sv[1], &argv[argi]);
    close(sv[1]);

    // Terima listener fd dari anak
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } u;
    char one = 'X'; // recvmsg MENULIS ke sini — harus variabel, bukan literal
    struct iovec io = {.iov_base = &one, .iov_len = 1};
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    if (recvmsg(sv[0], &msg, 0) == -1) {
        perror("recvmsg");
        kill(child_pid, SIGKILL);
        return 1;
    }
    if (msg.msg_flags & MSG_CTRUNC) {
        fprintf(stderr, "recvmsg: control data terpotong (MSG_CTRUNC)\n");
        kill(child_pid, SIGKILL);
        return 1;
    }
    int listener = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&listener, CMSG_DATA(c), sizeof(int));
    if (listener < 0) {
        fprintf(stderr, "tidak dapat listener fd\n");
        kill(child_pid, SIGKILL);
        return 1;
    }
    close(sv[0]);

    printf("[notif] supervisor aktif, pid anak=%d, listener fd=%d\n",
           child_pid, listener);
    printf("[notif] kebijakan: tulis/O_CREAT ke /etc=ditolak, kill(SIGKILL)=ditolak, "
           "execve(/bin/sh)=ditolak\n\n");

    struct seccomp_notif *req =
        malloc(sizeof(*req) + sizeof(struct seccomp_notif_addfd));
    struct seccomp_notif_resp *resp =
        malloc(sizeof(*resp) + sizeof(struct seccomp_notif_addfd));
    if (!req || !resp) {
        free(req);
        free(resp);
        fprintf(stderr, "malloc gagal\n");
        return 1;
    }

    int child_done = 0;
    while (!child_done) {
        // Tunggu notifikasi dengan batas waktu: RECV tidak dijamin terbangun
        // saat anak keluar (tanpa notifikasi tertunda), sehingga supervisor
        // bisa menggantung selamanya dan test harness ikut macet.
        if (waitpid(child_pid, NULL, WNOHANG) == child_pid) break;
        struct pollfd pfd = {.fd = listener, .events = POLLIN};
        const int pr = poll(&pfd, 1, 200);
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll(listener)");
            break;
        }
        // POLLHUP muncul saat semua task pemakai filter sudah selesai; RECV
        // akan memblokir selamanya di kondisi itu.
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;
        memset(req, 0, sizeof(*req));
        const int rc = ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req);
        if (rc == -1) {
            if (errno == ENOENT) break;      // anak sudah selesai
            if (errno == EINTR) continue;
            perror("NOTIF_RECV");
            break;
        }
        memset(resp, 0, sizeof(*resp));
        resp->id = req->id;
        resp->val = 0;
        resp->error = 0;
        resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; // default: teruskan

        const int nr = req->data.nr;
        const unsigned long a0 = req->data.args[0], a1 = req->data.args[1],
                            a2 = req->data.args[2];

        if (nr == NR_open || nr == NR_openat || nr == NR_openat2) {
            char path[256];
            unsigned long long flags = 0, mode = 0;
            int ok = 0;
            if (nr == NR_open)
                ok = read_cstr(req->pid, a0, path, sizeof(path));
            else if (nr == NR_openat)
                ok = read_cstr(req->pid, a1, path, sizeof(path));
            else
                ok = read_cstr(req->pid, a1, path, sizeof(path));

            if (ok) {
                if (nr == NR_open) {
                    flags = (unsigned)a1;
                } else if (nr == NR_openat) {
                    flags = (unsigned)a2;
                } else if (read_open_how(req->pid, a2, &flags, &mode) != 0) {
                    ok = 0;
                }
            }

            if (!ok) {
                // Fail-closed: tidak bisa membaca argumen bukan alasan untuk
                // meneruskan operasi sensitif.
                resp->error = -EACCES;
                resp->flags = 0;
                printf("[notif] TOLAK %s(<tak terbaca>) -> EACCES\n", nr_name(nr));
            } else {
                const int is_write = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
                if (is_write && targets_etc(req->pid, path)) {
                    resp->error = -EACCES;
                    resp->flags = 0;
                    printf("[notif] TOLAK %s(%s, flags=0x%llx) -> EACCES\n",
                           nr_name(nr), path, flags);
                } else {
                    printf("[notif] teruskan %s(%s, flags=0x%llx)\n", nr_name(nr), path,
                           flags);
                }
            }
        } else if (nr == NR_kill || nr == NR_tgkill || nr == NR_tkill) {
            const long target = (nr == NR_kill || nr == NR_tkill) ? (long)a0 : (long)a1;
            const long sig = (nr == NR_kill || nr == NR_tkill) ? (long)a1 : (long)a2;
            if (target == child_pid && sig == SIGKILL) {
                resp->error = -EPERM;
                resp->flags = 0;
                printf("[notif] TOLAK %s(%ld, SIGKILL) -> EPERM (anti-kill)\n",
                       nr_name(nr), target);
            } else {
                printf("[notif] teruskan %s(%ld, sig=%ld)\n", nr_name(nr), target, sig);
            }
        } else if (nr == NR_execve || nr == NR_execveat) {
            char path[256] = {0};
            const unsigned long ptr = (nr == NR_execve) ? a0 : a1;
            if (!read_cstr(req->pid, ptr, path, sizeof(path)) || strstr(path, "/bin/sh")) {
                resp->error = -EACCES;
                resp->flags = 0;
                printf("[notif] TOLAK %s(%s) -> EACCES\n", nr_name(nr),
                       path[0] ? path : "<tak terbaca>");
            } else {
                printf("[notif] teruskan %s(%s)\n", nr_name(nr), path);
            }
        } else {
            printf("[notif] teruskan %s\n", nr_name(nr));
        }

        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) == -1) {
            if (errno != ENOENT) perror("NOTIF_SEND");
        }
        // cek anak sudah mati?
        if (waitpid(child_pid, NULL, WNOHANG) == child_pid) child_done = 1;
    }
    printf("\n[notif] selesai\n");
    free(req);
    free(resp);
    return 0;
}
