#!/usr/bin/env bash
# sandbox-status.sh — status semua sandbox instabox (service, hashrate, share)
#
#   ./sandbox-status.sh              pakai daftar default
#   ./sandbox-status.sh xcb2 xcb5    cek sandbox tertentu
#
# Yang dicek per sandbox:
#   state    : state mesin di instabox (running/suspended/stopped/...)
#   svc      : unit miner (syscached) active?
#   alive    : proses miner hidup? (dari systemd MainPID)
#   hr       : hashrate terakhir dari journal (format CONN: "HR 854 H/s")
#   sh       : share diterima/ditolak (format CONN: "A 0 R 0")
#   worker   : nama worker dari "Pool login OK (...) sebagai <worker>"
#   run_left : sisa waktu sebelum batas max-run (8 jam) tercapai
set -u
# shellcheck source=/dev/null
source "$HOME/.instabox/env" 2>/dev/null || { echo "env instabox tidak ada (~/.instabox/env)"; exit 1; }
IB="$HOME/.local/bin/instabox"
MACHINES="${*:-analisa1 xcb2 xcb3 xcb4 xcb5}"
UNIT="${UNIT:-syscached}"

# shellcheck disable=SC2016  # probe sengaja single-quote: diekspansi di sandbox
probe='
svc=$(systemctl is-active '"$UNIT"' 2>&1)
pid=$(systemctl show -p MainPID --value '"$UNIT"' 2>/dev/null)
alive=no
[ -n "${pid:-}" ] && [ "$pid" != 0 ] && [ -d "/proc/$pid" ] && alive=yes
j=$(sudo journalctl -u '"$UNIT"' -n 60 --no-pager 2>/dev/null)
hr=$(echo "$j" | grep -oE "HR [0-9]+ H/s" | tail -1 | tr " " "_")
sh=$(echo "$j" | grep -oE "A [0-9]+ R [0-9]+" | tail -1 | tr " " "_")
auth=$(echo "$j" | grep -o "sebagai .*" | tail -1 | sed "s/^sebagai //")
printf "svc=%s alive=%s hr=%s sh=%s auth=%s\n" "$svc" "$alive" "${hr:-n/a}" "${sh:-n/a}" "${auth:-n/a}"
'
pick() { sed -n "s/.*$1=\([^ ]*\).*/\1/p"; }

printf "%-9s %-10s %-8s %-6s %-13s %-11s %-10s %s\n" \
  NAME STATE SVC ALIVE HASH SHARES WORKER RUN_LEFT
printf "%s\n" "---------------------------------------------------------------------------------------------------"

fails=0
for m in $MACHINES; do
  info=$("$IB" get "$m" --json 2>/dev/null)
  if [ -z "$info" ]; then
    printf "%-9s %-10s %s\n" "$m" "?" "TIDAK BISA DIHUBUNGI"; fails=$((fails+1)); continue
  fi
  state=$(jq -r '.state // "?"' <<<"$info")
  runat=$(jq -r '.runStartedAt // ""' <<<"$info")
  maxrun=$(jq -r '.maxRunMinutes // 0' <<<"$info")

  left="n/a"
  if [ "$state" = "running" ] && [ -n "$runat" ] && [ "$maxrun" -gt 0 ] 2>/dev/null; then
    start=$(date -d "$runat" +%s 2>/dev/null || echo 0)
    rem=$(( maxrun * 60 - $(date +%s) + start ))
    [ "$rem" -lt 0 ] && rem=0
    left="$rem"
  fi

  out=""
  if [ "$state" = "running" ]; then
    out=$("$IB" exec "$m" -- bash -c "$probe" 2>/dev/null)
  fi
  if [ -z "$out" ]; then
    printf "%-9s %-10s %s\n" "$m" "$state" "EXEC GAGAL/UNREACHABLE"; fails=$((fails+1)); continue
  fi

  svc=$(echo "$out" | pick svc)
  alive=$(echo "$out" | pick alive)
  hr=$(echo "$out" | pick hr)
  sh=$(echo "$out" | pick sh)
  auth=$(echo "$out" | pick auth)

  if [ "$svc" = "active" ] && [ "$alive" = "yes" ]; then st="OK"; else st="PROBLEM"; fails=$((fails+1)); fi

  printf "%-9s %-10s %-8s %-6s %-13s %-11s %-10s %ss\n" \
    "$m" "$state" "$st" "$alive" "${hr:-n/a}" "${sh:-n/a}" "${auth:-n/a}" "$left"
done

echo
[ "$fails" -eq 0 ] && echo "SEMUA SANDBOX SEHAT" || echo "$fails sandbox bermasalah"
exit 0
