#!/bin/sh
# pool-workers.sh — status worker XCB di pool CatchThatRabbit.
#
# POSIX sh (jalan dengan `sh pool-workers.sh`), butuh curl dan jq.
# Wallet diambil dari -w, lalu $WALLET, lalu /etc/xcb/miner.env.
#
#   sh pool-workers.sh                    # sekali tampil
#   sh pool-workers.sh -i 60              # ulang tiap 60 detik
#   sh pool-workers.sh -w <wallet> -r eu  # wallet dan region lain
#
# Catatan: `hr` pool dihitung dari jumlah share dalam jendela pendek, bukan
# kecepatan instan. Pada diff 2,78M satu share bernilai ~1,5 kH/s, jadi angka
# hr melompat dalam kelipatan itu. `offline` berarti belum ada share dalam
# ~30 menit, bukan miner berhenti — cek service di host untuk memastikan.

set -eu

REGION=${XCB_POOL_REGION:-sg}
API="https://${REGION}-api.catchthatrabbit.com/v2/api"
UA='Mozilla/5.0'
INTERVAL=0
WALLET=${WALLET:-}

usage() {
    cat <<'EOF'
Pakai: sh pool-workers.sh [-w WALLET] [-r REGION] [-i DETIK]

  -w WALLET   alamat wallet (default: $WALLET atau /etc/xcb/miner.env)
  -r REGION   region API pool: sg (default), eu, us
  -i DETIK    ulangi tiap DETIK detik (0 = sekali, default)
  -h          tampilkan bantuan ini
EOF
}

while getopts "w:r:i:h" opt; do
    case $opt in
        w) WALLET=$OPTARG ;;
        r) REGION=$OPTARG; API="https://${REGION}-api.catchthatrabbit.com/v2/api" ;;
        i) INTERVAL=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

if [ -z "$WALLET" ] && [ -r /etc/xcb/miner.env ]; then
    WALLET=$(sed -n 's/^WALLET=//p' /etc/xcb/miner.env | head -n 1)
fi
WALLET=${WALLET%%.*}
if [ -z "$WALLET" ]; then
    echo "wallet kosong: pakai -w, \$WALLET, atau jalankan di host dengan /etc/xcb/miner.env" >&2
    exit 2
fi

command -v curl >/dev/null 2>&1 || { echo "curl tidak ditemukan" >&2; exit 1; }
command -v jq >/dev/null 2>&1 || { echo "jq tidak ditemukan" >&2; exit 1; }

fetch() {
    curl -fsS -A "$UA" --max-time 15 "$API/$1/$WALLET"
}

render() {
    now=$(date +%s)
    acc=$(fetch accounts) || { echo "gagal mengambil data akun dari $API" >&2; return 1; }
    wrk=$(fetch workers) || { echo "gagal mengambil data worker dari $API" >&2; return 1; }

    printf 'Pool CatchThatRabbit %s — %s\n' "$REGION" "$(date -u '+%Y-%m-%d %H:%M:%SZ')"
    printf 'Wallet %s\n\n' "$WALLET"

    printf '%-18s %-8s %8s %8s %12s\n' WORKER STATUS HR HR2 "SHARE AKHIR"
    printf '%s\n' "$wrk" | jq -r --argjson now "$now" '
        [.workers | to_entries[] | {
            name: .key,
            status: (if .value.offline then "offline" else "online" end),
            hr: .value.hr,
            hr2: .value.hr2,
            age: ((($now - .value.lastBeat) / 60) | floor)
        }]
        | sort_by([(if .status == "online" then 0 else 1 end), -.hr, .name])
        | .[]
        | [.name, .status, (.hr | tostring), (.hr2 | tostring), (.age | tostring)]
        | @tsv' |
    while IFS="$(printf '\t')" read -r name status hr hr2 age; do
        printf '%-18s %-8s %8s %8s %9sm lalu\n' "$name" "$status" "$hr" "$hr2" "$age"
    done

    printf '\n'
    printf '%s\n' "$acc" | jq -r --argjson now "$now" '
        "Online \(.workersOnline)/\(.workersOnline + .workersOffline)"
        + " | current \(.currentHashrate) H/s | hashrate \(.hashrate) H/s"
        + "\nroundShares \(.roundShares) | immature \(.stats.immature)"
        + " | balance \(.stats.balance)"
        + " | share terakhir \((($now - .stats.lastShare) / 60) | floor) menit lalu"'
}

if [ "$INTERVAL" -gt 0 ] 2>/dev/null; then
    while :; do
        printf '\033[H\033[2J'
        render || exit 1
        printf '\nCtrl-C untuk berhenti. Refresh tiap %s detik.\n' "$INTERVAL"
        sleep "$INTERVAL"
    done
else
    render
fi
