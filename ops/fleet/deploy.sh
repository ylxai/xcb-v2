#!/usr/bin/env bash
# deploy.sh — deploy xcb CONN ke fleet: checksum, backup, rollback, canary.
#
#   ./deploy.sh --from-ci                 # artefak 22.04 dari CI run main terakhir
#   ./deploy.sh --from ../../build        # artefak lokal (xcb + librandomx_v2.so)
#   ./deploy.sh --only fl --dry-run       # lihat rencana tanpa menyentuh host
#   ./deploy.sh --rollback                # kembalikan backup terbaru di semua host
#   ./deploy.sh --rollback 20261105-112843
#
# Daftar host: ~/.config/xcb/fleet.hosts (atau --hosts FILE).
# Format: nama=alamat, alamat "local" untuk host tempat skrip dijalankan.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
REPO_ROOT="$(cd ../.. && pwd)"

SSH_USER="${SSH_USER:-ubuntu}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_rsa}"
REPO="${REPO:-ylxai/xcb-v2}"
ARTIFACT_NAME="${ARTIFACT_NAME:-xcb-2204}"
SVC="${SVC:-syscached}"
WAIT_S="${WAIT_S:-120}"

FROM=""
FROM_CI=0
HOSTS_FILE="${HOSTS_FILE:-}"
ONLY=""
CANARY=""
SKIP_CANARY=0
DRY=0
EXPECT=""
DO_ROLLBACK=0
ROLLBACK_STAMP=""
REMOTE_DIR=""

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }
die() { log "FATAL: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
deploy.sh — deploy xcb CONN ke fleet.

Opsi:
  --from DIR        direktori berisi xcb + librandomx_v2.so (default: build/)
  --from-ci         unduh artefak dari CI run terakhir branch main (butuh gh)
  --hosts FILE      daftar host (default: ~/.config/xcb/fleet.hosts)
  --only A,B        deploy hanya host tersebut
  --canary NAME     host canary (default: host pertama)
  --skip-canary     langsung semua host (tidak disarankan)
  --expect STR      pastikan --version mengandung STR (mis. commit)
  --wait SECONDS    batas tunggu miner siap per host (default 120)
  --rollback [STAMP]  kembalikan backup (default: backup terbaru)
  --dry-run         cetak rencana, tidak menyentuh host
  -h, --help        bantuan ini
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --from) FROM="${2:-}"; shift 2 || shift ;;
        --from-ci) FROM_CI=1; shift ;;
        --hosts) HOSTS_FILE="${2:-}"; shift 2 || shift ;;
        --only) ONLY="${2:-}"; shift 2 || shift ;;
        --canary) CANARY="${2:-}"; shift 2 || shift ;;
        --skip-canary) SKIP_CANARY=1; shift ;;
        --expect) EXPECT="${2:-}"; shift 2 || shift ;;
        --wait) WAIT_S="${2:-120}"; shift 2 || shift ;;
        --dry-run) DRY=1; shift ;;
        --rollback)
            DO_ROLLBACK=1
            if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then
                ROLLBACK_STAMP="$2"
                shift
            fi
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) die "opsi tidak dikenal: $1 (pakai --help)" ;;
    esac
done

resolve_hosts() {
    if [ -z "$HOSTS_FILE" ]; then
        for f in "$HOME/.config/xcb/fleet.hosts" ./hosts.conf /etc/xcb/fleet.hosts; do
            [ -f "$f" ] && { HOSTS_FILE="$f"; break; }
        done
    fi
    if [ -z "$HOSTS_FILE" ] || [ ! -f "$HOSTS_FILE" ]; then
        die "hosts file tidak ditemukan; buat ~/.config/xcb/fleet.hosts (lihat hosts.conf.example)"
    fi
    mapfile -t HOSTS < <(grep -vE '^[[:space:]]*(#|$)' "$HOSTS_FILE")
    [ ${#HOSTS[@]} -gt 0 ] || die "hosts file kosong: $HOSTS_FILE"
    log "hosts: $HOSTS_FILE (${#HOSTS[@]} host)"
}

filter_only() {
    [ -z "$ONLY" ] && return
    local want=",$ONLY," line name out=()
    for line in "${HOSTS[@]}"; do
        name="${line%%=*}"
        case "$want" in *",$name,"*) out+=("$line") ;; esac
    done
    [ ${#out[@]} -gt 0 ] || die "--only $ONLY tidak cocok dengan host mana pun"
    HOSTS=("${out[@]}")
}

order_canary() {
    [ "$SKIP_CANARY" = 1 ] && return
    local canary_name="${CANARY:-${HOSTS[0]%%=*}}"
    local line found="" out=()
    for line in "${HOSTS[@]}"; do
        [ "${line%%=*}" = "$canary_name" ] && found="$line"
    done
    [ -n "$found" ] || die "canary '$canary_name' tidak ada di daftar host"
    out=("$found")
    for line in "${HOSTS[@]}"; do
        [ "$line" = "$found" ] && continue
        out+=("$line")
    done
    HOSTS=("${out[@]}")
    log "canary: $canary_name"
}

prepare_artifact() {
    if [ "$FROM_CI" = 1 ]; then
        command -v gh >/dev/null || die "gh CLI tidak ada (dibutuhkan --from-ci)"
        FROM=$(mktemp -d)
        log "unduh artefak '$ARTIFACT_NAME' dari run terakhir $REPO@main"
        gh run download --repo "$REPO" --branch main --name "$ARTIFACT_NAME" --dir "$FROM" >/dev/null ||
            die "gagal unduh artefak (run main terakhir belum punya $ARTIFACT_NAME?)"
    fi
    [ -n "$FROM" ] || FROM="$REPO_ROOT/build"
    [ -x "$FROM/xcb" ] || die "$FROM/xcb tidak ada"
    [ -f "$FROM/librandomx_v2.so" ] || die "$FROM/librandomx_v2.so tidak ada"

    SHA_XCB=$(sha256sum "$FROM/xcb" | awk '{print $1}')
    if [ -f "$FROM/SHA256SUMS" ]; then
        # Manifest dibuat step staging CI dan ikut di dalam artefak. Verifikasi
        # terhadap referensi build-time ini, bukan terhadap hash yang baru saja
        # dihitung dari file yang sama (itu hanya mendeteksi unduhan rusak).
        grep -q ' xcb$' "$FROM/SHA256SUMS" || die "SHA256SUMS tidak memuat xcb"
        grep -q ' librandomx_v2.so$' "$FROM/SHA256SUMS" ||
            die "SHA256SUMS tidak memuat librandomx_v2.so"
        ( cd "$FROM" && sha256sum -c SHA256SUMS >/dev/null ) ||
            die "checksum artefak tidak cocok dengan SHA256SUMS"
        log "checksum: cocok dengan SHA256SUMS"
    else
        log "SHA256SUMS tidak ada (build lokal); integritas tidak diverifikasi"
    fi
    LOCAL_VER=$("$FROM/xcb" --version 2>&1 | head -1) ||
        die "artefak tidak bisa dijalankan di host ini (target glibc lebih baru?)"
    log "artefak: $LOCAL_VER"
    log "sha256 xcb: $SHA_XCB"
    if [ -n "$EXPECT" ] && ! printf '%s' "$LOCAL_VER" | grep -qF -- "$EXPECT"; then
        die "versi artefak tidak cocok dengan --expect '$EXPECT'"
    fi
}

ssh_run() {
    local addr=$1; shift
    ssh -o BatchMode=yes -o ConnectTimeout=8 -i "$SSH_KEY" "$SSH_USER@$addr" "$@"
}

stage() {
    local addr=$1
    # mktemp -d: direktori staging tidak boleh punya nama tetap di /tmp yang
    # bisa ditulisi siapa saja (symlink/hijack). Path-nya disimpan di
    # REMOTE_DIR dan dipakai run_apply().
    if [ "$addr" = local ]; then
        REMOTE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/xcb-fleet.XXXXXX") || return 1
        cp apply.sh "$REMOTE_DIR/" || return 1
        if [ "$DO_ROLLBACK" != 1 ]; then
            cp "$FROM/xcb" "$FROM/librandomx_v2.so" "$REMOTE_DIR/" || return 1
        fi
        return 0
    fi
    REMOTE_DIR=$(ssh_run "$addr" 'mktemp -d /tmp/xcb-fleet.XXXXXX') || return 1
    [ -n "$REMOTE_DIR" ] || { log "$addr: mktemp tidak mengembalikan path"; return 1; }
    if [ "$DO_ROLLBACK" = 1 ]; then
        scp -q -o BatchMode=yes -i "$SSH_KEY" apply.sh "$SSH_USER@$addr:$REMOTE_DIR/" || return 1
        return 0
    fi
    scp -q -o BatchMode=yes -i "$SSH_KEY" \
        "$FROM/xcb" "$FROM/librandomx_v2.so" apply.sh "$SSH_USER@$addr:$REMOTE_DIR/" || return 1
    local rsha
    rsha=$(ssh_run "$addr" "sha256sum '$REMOTE_DIR/xcb'" | awk '{print $1}')
    [ "$rsha" = "$SHA_XCB" ] || { log "$addr: checksum xcb beda setelah scp"; return 1; }
    return 0
}

cleanup_stage() {
    local addr=$1
    [ -n "$REMOTE_DIR" ] || return 0
    if [ "$addr" = local ]; then
        rm -rf "$REMOTE_DIR"
    else
        ssh_run "$addr" "rm -rf '$REMOTE_DIR'" || true
    fi
    REMOTE_DIR=""
}

run_apply() {
    local addr=$1 cmd a
    shift
    printf -v cmd 'SVC=%q WAIT_S=%q bash %q' "$SVC" "$WAIT_S" "$REMOTE_DIR/apply.sh"
    for a in "$@"; do printf -v cmd '%s %q' "$cmd" "$a"; done
    if [ "$addr" = local ]; then
        bash -c "$cmd"
    else
        ssh_run "$addr" "$cmd"
    fi
}

deploy_one() {
    local name=$1 addr=$2 stamp=$3 out
    if [ "$DRY" = 1 ]; then
        log "[dry-run] $name ($addr): install $LOCAL_VER + backup/rollback"
        return 0
    fi
    stage "$addr" || { log "$name: gagal menyiapkan file di host"; return 1; }
    out=$(run_apply "$addr" install "$stamp" "$REMOTE_DIR" "$EXPECT" 2>&1)
    printf '%s\n' "$out" | sed 's/^/  /'
    if printf '%s' "$out" | grep -q 'HASIL=OK'; then
        cleanup_stage "$addr"
        return 0
    fi
    log "$name: staging ditinggal di $REMOTE_DIR untuk inspeksi"
    return 1
}

rollback_one() {
    local name=$1 addr=$2 out
    if [ "$DRY" = 1 ]; then
        log "[dry-run] $name ($addr): rollback ${ROLLBACK_STAMP:-terbaru}"
        return 0
    fi
    stage "$addr" || { log "$name: gagal menyiapkan file di host"; return 1; }
    out=$(run_apply "$addr" rollback "$ROLLBACK_STAMP" 2>&1)
    printf '%s\n' "$out" | sed 's/^/  /'
    if printf '%s' "$out" | grep -q 'svc=active'; then
        cleanup_stage "$addr"
        return 0
    fi
    log "$name: staging ditinggal di $REMOTE_DIR untuk inspeksi"
    return 1
}

resolve_hosts
filter_only
order_canary

if [ "$DO_ROLLBACK" = 1 ]; then
    log "mode rollback (${ROLLBACK_STAMP:-backup terbaru})"
    total=0; ok=0
    for line in "${HOSTS[@]}"; do
        name="${line%%=*}"; addr="${line#*=}"
        total=$((total + 1))
        log "rollback $name ($addr)"
        if rollback_one "$name" "$addr"; then
            ok=$((ok + 1))
        else
            log "GAGAL rollback $name"
        fi
    done
    log "selesai: $ok/$total host"
    [ "$ok" = "$total" ]
    exit
fi

prepare_artifact

STAMP=$(date +%Y%m%d-%H%M%S)
log "stamp: $STAMP"
total=0; ok=0
for line in "${HOSTS[@]}"; do
    name="${line%%=*}"; addr="${line#*=}"
    total=$((total + 1))
    log "deploy $name ($addr)"
    if deploy_one "$name" "$addr" "$STAMP"; then
        ok=$((ok + 1))
        if [ "$total" = 1 ] && [ "$SKIP_CANARY" != 1 ] && [ "$DRY" != 1 ]; then
            log "canary $name OK, lanjut ke host berikutnya"
            sleep 5
        fi
    else
        log "GAGAL $name — berhenti, host lain tidak disentuh"
        break
    fi
done

log "selesai: $ok/$total host OK"
[ "$ok" = "$total" ]
