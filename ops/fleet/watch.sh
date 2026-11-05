#!/usr/bin/env bash
# watch.sh — cek kesehatan fleet xcb CONN, kirim alert saat ada masalah.
#
#   ./watch.sh                 # sekali jalan (cocok untuk cron */5)
#   ./watch.sh --dry-run       # tampilkan alert tanpa mengirim
#   ./watch.sh --test-alert    # kirim satu pesan uji
#
# Alert channel (pilih salah satu / keduanya):
#   ALERT_WEBHOOK           URL webhook (Discord memakai "content", Slack
#                           memakai "text"; keduanya dikirim)
#   ALERT_TELEGRAM_TOKEN    token bot Telegram
#   ALERT_TELEGRAM_CHAT     chat_id tujuan
#
# Env lain: MIN_HR (default 400), MAX_REJECT_PCT (5), REJECT_MIN_DELTA (20),
#           RECONNECT_LIMIT (3), ALERT_COOLDOWN (1800 detik),
#           STATE_DIR (~/.local/state/xcb-watch).
# Konfigurasi dibaca dari ~/.config/xcb/watch.env kalau ada.
set -uo pipefail
cd "$(dirname "$0")" || exit 1

WATCH_ENV="${WATCH_ENV:-$HOME/.config/xcb/watch.env}"
# shellcheck disable=SC1090
[ -f "$WATCH_ENV" ] && . "$WATCH_ENV"

SSH_USER="${SSH_USER:-ubuntu}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_rsa}"
HOSTS_FILE="${HOSTS_FILE:-}"
MIN_HR="${MIN_HR:-400}"
MAX_REJECT_PCT="${MAX_REJECT_PCT:-5}"
REJECT_MIN_DELTA="${REJECT_MIN_DELTA:-20}"
RECONNECT_LIMIT="${RECONNECT_LIMIT:-3}"
ALERT_COOLDOWN="${ALERT_COOLDOWN:-1800}"
STATE_DIR="${STATE_DIR:-$HOME/.local/state/xcb-watch}"
DRY=0
TEST_ALERT=0

usage() {
    cat <<'EOF'
watch.sh — kesehatan fleet xcb CONN.

Opsi:
  --hosts FILE    daftar host (default: ~/.config/xcb/fleet.hosts)
  --dry-run       tampilkan alert tanpa mengirim
  --test-alert    kirim satu pesan uji lalu keluar
  -h, --help      bantuan ini
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --hosts) HOSTS_FILE="${2:-}"; shift 2 || shift ;;
        --dry-run) DRY=1; shift ;;
        --test-alert) TEST_ALERT=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "opsi tidak dikenal: $1 (pakai --help)" >&2; exit 2 ;;
    esac
done

json_escape() {
    local s=$1
    s=${s//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    s=${s//$'\r'/}
    printf '%s' "$s"
}

send_alert() {
    local msg=$1 sent=0
    if [ "$DRY" = 1 ]; then
        echo "[alert] $msg"
        return 0
    fi
    if [ -n "${ALERT_WEBHOOK:-}" ]; then
        # Discord membaca field "content", Slack membaca "text".
        # Kirim keduanya supaya satu webhook generik cocok untuk dua-duanya.
        local esc
        esc=$(json_escape "$msg")
        if curl -fsS -m 10 -H 'Content-Type: application/json' \
             --data "{\"content\":\"$esc\",\"text\":\"$esc\"}" "$ALERT_WEBHOOK" >/dev/null; then
            sent=1
        else
            echo "PERINGATAN: kirim ke ALERT_WEBHOOK gagal" >&2
        fi
    fi
    if [ -n "${ALERT_TELEGRAM_TOKEN:-}" ] && [ -n "${ALERT_TELEGRAM_CHAT:-}" ]; then
        if curl -fsS -m 10 --data-urlencode "text=$msg" \
             "https://api.telegram.org/bot${ALERT_TELEGRAM_TOKEN}/sendMessage?chat_id=${ALERT_TELEGRAM_CHAT}" >/dev/null; then
            sent=1
        else
            echo "PERINGATAN: kirim ke Telegram gagal" >&2
        fi
    fi
    # Kembalikan nonzero kalau tidak ada channel yang berhasil menerima, supaya
    # pemanggil tidak menandai alert sebagai sudah terkirim.
    if [ "$sent" = 1 ]; then
        return 0
    fi
    echo "PERINGATAN: tidak ada channel alert yang berhasil menerima (set ALERT_WEBHOOK atau ALERT_TELEGRAM_*)" >&2
    return 1
}

resolve_hosts() {
    if [ -z "$HOSTS_FILE" ]; then
        for f in "$HOME/.config/xcb/fleet.hosts" ./hosts.conf /etc/xcb/fleet.hosts; do
            [ -f "$f" ] && { HOSTS_FILE="$f"; break; }
        done
    fi
    [ -n "$HOSTS_FILE" ] && [ -f "$HOSTS_FILE" ] ||
        { echo "hosts file tidak ditemukan; buat ~/.config/xcb/fleet.hosts (lihat hosts.conf.example)" >&2; exit 2; }
    mapfile -t HOSTS < <(grep -vE '^[[:space:]]*(#|$)' "$HOSTS_FILE")
    [ ${#HOSTS[@]} -gt 0 ] || { echo "hosts file kosong: $HOSTS_FILE" >&2; exit 2; }
    # Nama host juga menjadi nama file state; duplikat akan saling menabrak
    # cooldown antar-host. Tolak sejak awal.
    local line nm seen=""
    for line in "${HOSTS[@]}"; do
        nm="${line%%=*}"
        case " $seen " in
            *" $nm "*) echo "nama host duplikat di $HOSTS_FILE: $nm" >&2; exit 2 ;;
        esac
        seen="$seen $nm"
    done
}

PROBE='
svc=$(systemctl is-active syscached 2>&1)
hp_free=$(awk "/HugePages_Free/{print \$2}" /proc/meminfo)
j4=$(sudo -n journalctl -u syscached --since "-4 min" --no-pager 2>/dev/null)
j15=$(sudo -n journalctl -u syscached --since "-15 min" --no-pager 2>/dev/null)
hr=$(printf "%s\n" "$j4" | grep -oE "HR [0-9]+ H/s \| A [0-9]+ R [0-9]+" | tail -1)
reconn=$(printf "%s\n" "$j15" | grep -cE "Koneksi pool terputus|pool login gagal")
tlsfail=$(printf "%s\n" "$j15" | grep -c "TLS verifikasi gagal")
regular=$(printf "%s\n" "$j15" | grep -c "regular pages")
printf "svc=%s\nhp_free=%s\nhr=%s\nreconn=%s\ntlsfail=%s\nregular=%s\n" \
    "$svc" "$hp_free" "$hr" "$reconn" "$tlsfail" "$regular"
'

probe_host() {
    local addr=$1
    if [ "$addr" = local ]; then
        bash -c "$PROBE"
    else
        ssh -o BatchMode=yes -o ConnectTimeout=8 -i "$SSH_KEY" "$SSH_USER@$addr" "bash -s" <<<"$PROBE"
    fi
}

val() {
    printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1
}

resolve_hosts

if [ "$TEST_ALERT" = 1 ]; then
    if send_alert "xcb watch: pesan uji dari $(hostname) pada $(date '+%F %T')"; then
        exit 0
    fi
    exit 1
fi

mkdir -p "$STATE_DIR"
now=$(date +%s)
problems_total=0

printf "%-14s %-7s %-9s %-9s %-7s %-6s %s\n" \
    HOST STATUS HR REJECT RECONNECT HUGE NOTE
printf '%s\n' "---------------------------------------------------------------------------------------"

for line in "${HOSTS[@]}"; do
    name="${line%%=*}"
    addr="${line#*=}"
    out=$(probe_host "$addr" 2>/dev/null)
    if [ -z "$out" ]; then
        out="svc=unreachable
hp_free=?
hr=
reconn=0
tlsfail=0
regular=0"
    fi

    svc=$(val "$out" svc)
    hp_free=$(val "$out" hp_free)
    hr=$(val "$out" hr)
    reconn=$(val "$out" reconn)
    tlsfail=$(val "$out" tlsfail)
    regular=$(val "$out" regular)

    problems=()
    hr_num=""
    reject_pct=""
    a=""
    r=""
    prev_a=""
    prev_r=""
    if [ "$svc" != "active" ]; then
        problems+=("service=$svc")
    fi
    if [ -z "$hr" ]; then
        problems+=("tidak ada log HR 4 menit terakhir")
    else
        hr_num=$(printf '%s' "$hr" | sed -nE 's/.*HR ([0-9]+).*/\1/p' | head -1)
        a=$(printf '%s' "$hr" | sed -nE 's/.*[| ]A ([0-9]+).*/\1/p' | head -1)
        r=$(printf '%s' "$hr" | sed -nE 's/.*[| ]R ([0-9]+).*/\1/p' | head -1)
        [ -n "$hr_num" ] && [ "$hr_num" -lt "$MIN_HR" ] && problems+=("HR ${hr_num} < ${MIN_HR}")
    fi
    [ "${tlsfail:-0}" -gt 0 ] && problems+=("TLS verifikasi gagal ${tlsfail}x/15m")
    [ "${reconn:-0}" -ge "$RECONNECT_LIMIT" ] && problems+=("reconnect ${reconn}x/15m")
    [ "${regular:-0}" -gt 0 ] && problems+=("fallback regular pages")

    sf="$STATE_DIR/$name"
    prev_status=ok
    prev_alert=0
    if [ -f "$sf" ]; then
        prev_status=$(sed -n 's/^status=//p' "$sf" | tail -1)
        prev_status=${prev_status:-ok}
        prev_alert=$(sed -n 's/^alert=//p' "$sf" | tail -1)
        prev_alert=${prev_alert:-0}
        prev_a=$(sed -n 's/^a=//p' "$sf" | tail -1)
        prev_r=$(sed -n 's/^r=//p' "$sf" | tail -1)
    fi

    # Reject rate dihitung dari delta counter sejak run sebelumnya, bukan
    # dari total seumur proses. Counter yang turun berarti miner restart,
    # jadi jendelanya mulai dari counter saat ini.
    if [ -n "$a" ] && [ -n "$r" ] && [ -n "$prev_a" ] && [ -n "$prev_r" ]; then
        da=$((a - prev_a))
        dr=$((r - prev_r))
        if [ "$da" -lt 0 ] && [ "$dr" -lt 0 ]; then
            da=$a
            dr=$r
        fi
        if [ $((da + dr)) -ge "$REJECT_MIN_DELTA" ]; then
            reject_pct=$(( dr * 100 / (da + dr) ))
            [ "$reject_pct" -gt "$MAX_REJECT_PCT" ] &&
                problems+=("reject ${reject_pct}% (${dr}/$((da + dr)) sejak cek sebelumnya)")
        fi
    fi

    if [ ${#problems[@]} -gt 0 ]; then
        problems_total=$((problems_total + 1))
        note=""
        for p in "${problems[@]}"; do
            [ -n "$note" ] && note="$note; "
            note="$note$p"
        done
        status=problem
        if [ "$prev_status" != "problem" ] || [ $((now - prev_alert)) -ge "$ALERT_COOLDOWN" ]; then
            # prev_alert hanya maju kalau operator benar-benar menerima pesan.
            if [ "$DRY" = 1 ]; then
                # tampilkan saja; cooldown tidak diubah pada mode dry-run
                send_alert "🔴 ${name} (${addr}): ${note}"
            elif send_alert "🔴 ${name} (${addr}): ${note}"; then
                prev_alert=$now
            else
                # belum tersampaikan: run berikutnya langsung mencoba lagi
                prev_alert=0
            fi
        fi
    else
        note="ok"
        status=ok
        if [ "$prev_status" = "problem" ]; then
            if [ "$DRY" = 1 ]; then
                send_alert "🟢 ${name} (${addr}): pulih"
            elif send_alert "🟢 ${name} (${addr}): pulih"; then
                prev_alert=0
            else
                # pesan pulih belum tersampaikan; status problem dipertahankan
                status=problem
                prev_alert=0
            fi
        else
            prev_alert=0
        fi
    fi
    if [ "$DRY" != 1 ]; then
        printf 'status=%s\nalert=%s\na=%s\nr=%s\n' "$status" "$prev_alert" "${a:-}" "${r:-}" >"$sf"
    fi

    printf "%-14s %-7s %-9s %-9s %-7s %-6s %s\n" \
        "$name" "$status" "${hr_num:-?}" "${reject_pct:-?}" "${reconn:-?}" "${hp_free:-?}" "$note"
done

echo
if [ "$problems_total" = 0 ]; then
    echo "SEMUA HOST SEHAT"
else
    echo "$problems_total host bermasalah"
fi
[ "$problems_total" = 0 ]
