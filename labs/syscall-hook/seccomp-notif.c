// seccomp-notif.c — Syscall hooking modern: SECCOMP user notification
//
// Prinsip: anak memasang filter BPF yang membuat KERNEL menghentikan syscall
// terpilih dan mengirim notifikasi ke supervisor (parent) via listener fd.
// Supervisor bisa menolak/meneruskan tiap syscall — persis hooking, tapi
// pakai mekanisme resmi kernel (dipakai Firejail, systemd, gVisor).
//
// Filter mencegat: openat, open, execve, kill.
// Kebijakan supervisor (ganti sesukamu):
//   - openat/open dengan akses tulis ke path mengandung "/etc/"  -> EACCES
//   - kill(pid_anak, SIGKILL)                                    -> dibatalkan
//   - execve("/bin/sh")                                          -> dibatalkan
//   - sisanya diteruskan
//
// Build: gcc -O2 -o seccomp-notif seccomp-notif.c
// Tidak butuh root (PR_SET_NO_NEW_PRIVS + NEW_LISTENER).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define NR_openat 257
#define NR_execve 59
#define NR_KILL    62

static pid_t child_pid;

// ---- filter BPF klasik: USER_NOTIF untuk syscall target ----
static int install_filter(void) {
    struct sock_filter filter[] = {
        // load nomor syscall
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, nr)),
        // biarkan exit/exit_group langsung (jangan notif, nanti hang)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit_group, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        // target yang kita "hook": openat, execve, kill
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_openat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_execve, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_KILL, 0, 1),
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
    // NEW_LISTENER: kernel membuat fd listener; flag tsync dipakai supaya
    // filter menular ke thread/exec berikutnya.
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
    struct iovec io = {.iov_base = "F", .iov_len = 1};
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
    // Exec program target — filter menetap.
    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
}

// Baca string dari memory anak (untuk melihat argumen path di notif).
static int read_cstr(pid_t pid, unsigned long addr, char *out, size_t cap) {
    size_t i = 0;
    while (i + 8 <= cap) {
        long chunk = 0;
        errno = 0;
        struct iovec local = {.iov_base = &chunk, .iov_len = 8};
        struct iovec remote = {.iov_base = (void *)(addr + i), .iov_len = 8};
        long n = syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0);
        // process_vm_readv mengembalikan jumlah byte yang terbaca
        if (n == -1 && errno != 0) break;
        if (n < 8) { // EOF/sisa <8 byte
            memcpy(out + i, &chunk, n > 0 ? (size_t)n : 0);
            break;
        }
        memcpy(out + i, &chunk, 8);
        if (memchr(out + i, '\0', 8)) return 1;
        i += 8;
    }
    out[cap - 1] = '\0';
    return (i > 0);
}

static const char *nr_name(int nr) {
    switch (nr) {
    case NR_openat: return "openat";
    case 2:          return "open";
    case NR_execve: return "execve";
    case NR_KILL:   return "kill";
    default:         return "?";
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
    int listener = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&listener, CMSG_DATA(c), sizeof(int));
    if (listener < 0) { fprintf(stderr, "tidak dapat listener fd\n"); return 1; }
    close(sv[0]);

    printf("[notif] supervisor aktif, pid anak=%d, listener fd=%d\n",
           child_pid, listener);
    printf("[notif] kebijakan: /etc/ tulis=ditolak, kill(SIGKILL)=ditolak, "
           "execve(/bin/sh)=ditolak\n\n");

    struct seccomp_notif *req =
        malloc(sizeof(*req) + sizeof(struct seccomp_notif_addfd));
    struct seccomp_notif_resp *resp =
        malloc(sizeof(*resp) + sizeof(struct seccomp_notif_addfd));
    if (!req || !resp) return 1;

    int child_done = 0;
    while (!child_done) {
        memset(req, 0, sizeof(*req));
        int rc = ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req);
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

        int nr = req->data.nr;
        unsigned long a0 = req->data.args[0], a1 = req->data.args[1],
                      a2 = req->data.args[2];

        if (nr == NR_openat || nr == 2) {
            char path[256] = "?";
            read_cstr(req->pid, nr == NR_openat ? a1 : a0, path, sizeof(path));
            int flags = (int)(nr == NR_openat ? a2 : a1);
            int write_ok = flags & (O_WRONLY | O_RDWR);
            if (write_ok && strstr(path, "/etc/")) {
                resp->error = -EACCES;
                resp->flags = 0;
                printf("[notif] TOLAK %s(%s, flags=0x%x) -> EACCES\n",
                       nr_name(nr), path, flags);
            } else {
                printf("[notif] teruskan %s(%s, flags=0x%x)\n",
                       nr_name(nr), path, flags);
            }
        } else if (nr == NR_KILL) {
            long target = (long)a0, sig = (long)a1;
            if (target == child_pid && sig == SIGKILL) {
                resp->error = -EPERM;
                resp->flags = 0;
                printf("[notif] TOLAK kill(%ld, SIGKILL) -> EPERM (anti-kill)\n",
                       target);
            } else {
                printf("[notif] teruskan kill(%ld, sig=%ld)\n", target, sig);
            }
        } else if (nr == NR_execve) {
            char path[256] = "?";
            read_cstr(req->pid, a0, path, sizeof(path));
            if (strstr(path, "/bin/sh")) {
                resp->error = -EACCES;
                resp->flags = 0;
                printf("[notif] TOLAK execve(%s) -> EACCES\n", path);
            } else {
                printf("[notif] teruskan execve(%s)\n", path);
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
