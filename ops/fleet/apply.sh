#!/usr/bin/env bash
# apply.sh — pasang atau rollback xcb di satu host.
#
#   apply.sh install STAMP NEWDIR [EXPECT_VERSION]
#   apply.sh rollback [STAMP]
#
# Dipanggil deploy.sh lewat ssh. Untuk pemakaian darurat bisa dijalankan
# manual: backup dibuat di samping binary (xcb.bak-STAMP).
set -uo pipefail

SVC="${SVC:-syscached}"
BIN="${BIN:-/usr/local/bin/xcb}"
LIB="${BIN%/*}/librandomx_v2.so"
WAIT_S="${WAIT_S:-120}"

die() { echo "FATAL: $*" >&2; exit 1; }
say() { echo "$*"; }

lib_backup_for() {
    echo "${LIB}.bak-$1"
}

newest_stamp() {
    local f
    # shellcheck disable=SC2012  # BIN path terkontrol, tanpa spasi
    f=$(ls -1t "${BIN}".bak-* 2>/dev/null | head -1)
    [ -n "$f" ] || return 1
    echo "${f##*.bak-}"
}

wait_ready() {
    local since=$1 deadline=$(( $(date +%s) + WAIT_S ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if sudo -n journalctl -u "$SVC" --since "@$since" --no-pager 2>/dev/null |
             grep -q "RandomX V2 siap"; then
            return 0
        fi
        sleep 3
    done
    return 1
}

cmd_rollback() {
    local stamp=${1:-} binbak libbak
    if [ -z "$stamp" ]; then
        stamp=$(newest_stamp) || die "tidak ada backup ${BIN}.bak-*"
    fi
    binbak="${BIN}.bak-$stamp"
    libbak=$(lib_backup_for "$stamp")
    [ -f "$binbak" ] || die "backup $binbak tidak ada"
    # Binary dan library adalah satu paket (RPATH $ORIGIN). Jangan pernah
    # mengembalikan salah satunya saja: pasangan beda versi bisa gagal load.
    [ -f "$libbak" ] || die "backup library $libbak tidak ada (rollback dibatalkan agar versi xcb dan library tetap sepasang)"

    sudo -n systemctl stop "$SVC" || true
    sudo -n install -m 755 "$binbak" "$BIN" || die "restore xcb gagal"
    sudo -n install -m 644 "$libbak" "$LIB" || die "restore library gagal"
    sudo -n systemctl start "$SVC" || true
    sleep 2
    say "restored=$(basename "$binbak")"
    say "ver=$("$BIN" --version 2>&1 | head -1)"
    say "svc=$(systemctl is-active "$SVC")"
}

cmd_install() {
    local stamp=$1 newdir=$2 expect=${3:-} oldver ok libok ver since
    [ -x "$newdir/xcb" ] || die "$newdir/xcb tidak ada"
    [ -f "$newdir/librandomx_v2.so" ] || die "$newdir/librandomx_v2.so tidak ada"

    oldver=$("$BIN" --version 2>&1 | head -1)
    say "old=$oldver"

    sudo -n systemctl stop "$SVC" || true
    sudo -n cp -a "$BIN" "${BIN}.bak-$stamp" || die "backup xcb gagal"
    sudo -n cp -a "$LIB" "$(lib_backup_for "$stamp")" || die "backup library gagal"

    ok=1
    sudo -n install -m 755 "$newdir/xcb" "$BIN" || ok=0
    sudo -n install -m 644 "$newdir/librandomx_v2.so" "$LIB" || ok=0

    libok=$(ldd "$BIN" 2>/dev/null | grep -c "librandomx_v2.so =>")
    ver=$("$BIN" --version 2>&1 | head -1)
    say "new=$ver"
    say "libok=$libok"

    if [ "$ok" != 1 ] || [ "$libok" != 1 ] ||
       { [ -n "$expect" ] && ! printf '%s' "$ver" | grep -qF -- "$expect"; }; then
        say "HASIL=ROLLBACK"
        cmd_rollback "$stamp"
        return 1
    fi

    since=$(date +%s)
    if ! sudo -n systemctl start "$SVC"; then
        say "service gagal start"
        say "HASIL=ROLLBACK"
        cmd_rollback "$stamp"
        return 1
    fi
    if ! wait_ready "$since"; then
        say "miner tidak siap dalam ${WAIT_S}s"
        say "HASIL=ROLLBACK"
        cmd_rollback "$stamp"
        return 1
    fi

    say "svc=$(systemctl is-active "$SVC")"
    sudo -n journalctl -u "$SVC" --since "@$since" --no-pager 2>/dev/null |
        grep -E "Pool login|RandomX V2 siap|Mining pool|HR " | tail -4
    say "HASIL=OK"
}

case "${1:-}" in
    install)
        shift
        [ $# -ge 2 ] || die "usage: apply.sh install STAMP NEWDIR [EXPECT_VERSION]"
        cmd_install "$@"
        ;;
    rollback)
        shift
        cmd_rollback "${1:-}"
        ;;
    *)
        echo "usage: apply.sh install STAMP NEWDIR [EXPECT_VERSION]" >&2
        echo "       apply.sh rollback [STAMP]" >&2
        exit 2
        ;;
esac
