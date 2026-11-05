#!/bin/sh
# kprobe-tracefs.sh — Kernel-level syscall hooking TANPA modul kernel.
#
# Mengapa ada: LKM (kmod/) butuh kernel headers + gcc + make + /lib/modules
# yang cocok dengan kernel BERJALAN. Pengganti yang setara dan lebih modern:
# kprobe via tracefs — mekanisme kernel resmi yang dipakai eBPF/perf. Kita
# hook do_sys_openat2 (jalur inti open/openat modern) dan membaca argumennya
# dari register CPU saat syscall dieksekusi.
#
# ANATOMI ARGUMEN (kernel 6.8, fs/open.c):
#   long do_sys_openat2(int dfd, const char __user *filename,
#                       struct open_how *how)
# Hanya TIGA argumen. `how` adalah pointer; flags ada di how->flags (+0) dan
# mode di how->mode (+8). Versi lama memakai %dx sebagai flags (itu pointer!)
# dan %cx sebagai mode (bukan argumen), dan :string untuk pointer user —
# semuanya menghasilkan (fault)/garbage.
#
# Prasyarat: CAP_SYS_ADMIN (root), tracefs ter-mount, CONFIG_KPROBE_EVENTS.
#
# Pakai:  sh ./kprobe-tracefs.sh "ls /etc"     # hook satu command
#         sh ./kprobe-tracefs.sh               # hook sampai Ctrl-C
set -e

TR=/sys/kernel/tracing
[ -w "$TR/kprobe_events" ] || TR=/sys/kernel/debug/tracing
EVENT=hook_open

cleanup() {
    # Hanya event MILIK script ini; jangan hapus kprobe milik tracing lain.
    echo 0 > "$TR/events/kprobes/$EVENT/enable" 2>/dev/null || true
    echo "-:kprobes/$EVENT" >> "$TR/kprobe_events" 2>/dev/null || true
    echo 0 > "$TR/tracing_on" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM

[ -w "$TR/kprobe_events" ] || { echo "ERROR: $TR/kprobe_events tidak writable (butuh root + tracefs)" >&2; exit 1; }

cleanup

# Daftarkan kprobe. filename adalah pointer user -> :ustring; flags/mode
# dibaca dari struct open_how lewat pointer %dx.
echo "p:kprobes/$EVENT do_sys_openat2 dfd=%di filename=+0(%si):ustring flags=+0(%dx):u64 mode=+8(%dx):u64" \
    > "$TR/kprobe_events"
echo "kprobe terdaftar: do_sys_openat2 -> events/kprobes/$EVENT"

echo 1 > "$TR/events/kprobes/$EVENT/enable"
echo 1 > "$TR/tracing_on"
echo ">>> HOOK AKTIF — setiap open/openat di kernel tercatat <<<"

if [ $# -gt 0 ]; then
    echo ">>> menjalankan: $* <<<"
    "$@"
    sleep 0.2
    echo 0 > "$TR/tracing_on"
    echo ">>> TRACE (beberapa baris pertama) <<<"
    # Hindari grep|head (SIGPIPE menggagalkan pipeline di set -e).
    awk -v ev="$EVENT" '!/^#/ && index($0, ev) { print; n++; if (n >= 40) exit }' "$TR/trace" \
        || echo "(tidak ada event tertangkap)"
else
    echo ">>> tekan Ctrl-C untuk berhenti <<<"
    while :; do sleep 1; done
fi
