#!/bin/sh
# xcb-thread-sweep.sh — A/B jumlah thread (-t) miner XCB di satu host.
#
# Dijalankan SEBAGAI ROOT DI HOST MINER (mis. fl), bukan dari mesin lokal:
# skrip ini butuh `systemctl stop/start` dan harus menghitung proses root
# (dengan hidepid=2, user non-root tidak melihat proses miner sama sekali).
#
# Kenapa mengukur, bukan menebak: RandomY memakan ~2 ms per hash per thread,
# sedangkan SHA3-512 + cek target < 0,3 us (<0,02%). Satu-satunya knob yang
# masih bisa mengubah hashrate adalah jumlah thread dan topologi CPU
# (SMT/NUMA/L3) — dan angkanya berbeda per host, jadi harus diukur di host.
#
# Alur:
#   1. cek kondisi awal (harus tepat 1 miner, atau 0 dengan peringatan)
#   2. systemctl stop <UNIT>, tunggu sampai 0 proses xcb/sysd
#   3. untuk tiap -t: <BIN> --ui=log --benchmark <NONCES> -t <N>
#   4. ringkasan H/s per -t (dan H/s per thread)
#   5. SELALU systemctl start <UNIT> lagi, lalu verifikasi tepat satu proses
#      (trap EXIT/INT/TERM, jadi Ctrl+C pun memulihkan service)
#
# Pakai:
#   sudo sh xcb-thread-sweep.sh                      # -t 2 4 6, 200k nonce
#   sudo NONCES=400000 THREADS_LIST="2 4" sh xcb-thread-sweep.sh
#   sudo DRY_RUN=1 sh xcb-thread-sweep.sh            # cetak rencana, tanpa stop
#
# Env: UNIT (syscached), BIN (/usr/local/bin/xcb), NONCES (200000),
#      THREADS_LIST ("2 4 6"), BENCH_TIMEOUT (900 detik per run),
#      WAIT_STOP (30), WAIT_START (60), POST_WAIT (30), OUTDIR (/root),
#      DRY_RUN (0).
#
# Hasil: ringkasan di stdout + log $OUTDIR/xcb-thread-sweep-<stamp>.log,
#        output mentah tiap run $OUTDIR/xcb-bench-t<N>-<stamp>.log, dan tabel
#        TSV $OUTDIR/xcb-thread-sweep-<stamp>.tsv
#        (kolom: threads, H/s, "Total:" dari biner, exit code, FULL_MEM,
#         LARGE_PAGES).

set -u

UNIT="${UNIT:-syscached}"
BIN="${BIN:-/usr/local/bin/xcb}"
NONCES="${NONCES:-200000}"
THREADS_LIST="${THREADS_LIST:-2 4 6}"
BENCH_TIMEOUT="${BENCH_TIMEOUT:-900}"
WAIT_STOP="${WAIT_STOP:-30}"
WAIT_START="${WAIT_START:-60}"
POST_WAIT="${POST_WAIT:-30}"
OUTDIR="${OUTDIR:-/root}"
DRY_RUN="${DRY_RUN:-0}"

STAMP="$(date -u +%Y%m%d-%H%M%S)"
LOG="$OUTDIR/xcb-thread-sweep-$STAMP.log"
TSV="$OUTDIR/xcb-thread-sweep-$STAMP.tsv"

STARTED=0    # 1 setelah service berhasil di-stop (restore wajib start)
RESTORED=0   # penjaga agar restore idempoten (EXIT + pemanggilan eksplisit)
VERIFY_OK=0  # 1 hanya kalau pasca-sweep terverifikasi: 1 proses, unit active
FAILS=0

log() {
    printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$LOG"
}

die() {
    log "FATAL: $*"
    exit 1
}

# comm proses XCB: "sysd" kalau XCB_TITLE=sysd, "xcb" kalau build biasa.
# pgrep -x mencocokkan nama proses persis. Skrip ini root karena hidepid=2
# menyembunyikan proses miner dari user lain.
count_miners() {
    n_sysd="$(pgrep -x sysd 2>/dev/null | wc -l)"
    n_xcb="$(pgrep -x xcb 2>/dev/null | wc -l)"
    printf '%s\n' "$((n_sysd + n_xcb))"
}

wait_stopped() {
    waited=0
    while [ "$(count_miners)" -gt 0 ]; do
        waited=$((waited + 1))
        if [ "$waited" -ge "$WAIT_STOP" ]; then
            return 1
        fi
        sleep 1
    done
    return 0
}

wait_active() {
    waited=0
    while [ "$(systemctl is-active "$UNIT" 2>/dev/null || true)" != "active" ]; do
        waited=$((waited + 1))
        if [ "$waited" -ge "$WAIT_START" ]; then
            return 1
        fi
        sleep 1
    done
    return 0
}

verify_single() {
    mp="$(systemctl show "$UNIT" -p MainPID --value 2>/dev/null || true)"
    n="$(count_miners)"
    log "verifikasi: state=$(systemctl is-active "$UNIT" 2>/dev/null || true) MainPID=${mp:-0} proses_xcb=$n"
    if [ "${mp:-0}" -eq 0 ]; then
        log "GAGAL: MainPID 0 — unit tidak menjalankan miner"
        VERIFY_OK=0
        return 1
    fi
    if [ "$n" -ne 1 ]; then
        log "GAGAL: ada $n proses xcb/sysd, harus tepat 1 (dua miner = hashrate terbagi)"
        VERIFY_OK=0
        return 1
    fi
    # Bukti bahwa hashing benar-benar jalan lagi, bukan hanya unit "active".
    hrline="$(journalctl -u "$UNIT" -n 200 --no-pager 2>/dev/null | grep 'HR ' | tail -n 1 || true)"
    if [ -n "$hrline" ]; then
        log "HR terakhir: $hrline"
    fi
    VERIFY_OK=1
    return 0
}

# Dipanggil dari trap EXIT/INT/TERM dan sekali di akhir alur normal.
restore() {
    if [ "$RESTORED" = 1 ]; then
        return 0
    fi
    RESTORED=1
    if [ "$STARTED" != 1 ]; then
        return 0
    fi
    log "memulihkan service: systemctl start $UNIT"
    if ! systemctl start "$UNIT" >>"$LOG" 2>&1; then
        log "GAGAL: systemctl start $UNIT — intervensi manual diperlukan"
        VERIFY_OK=0
        return 1
    fi
    if ! wait_active; then
        log "GAGAL: $UNIT tidak 'active' setelah ${WAIT_START}s"
        VERIFY_OK=0
        return 1
    fi
    log "menunggu ${POST_WAIT}s agar statistik pertama keluar"
    sleep "$POST_WAIT"
    verify_single
    return 0
}

trap 'restore' EXIT
# HUP ikut ditangkap: kalau skrip dijalankan lewat SSH lalu koneksinya putus,
# service harus tetap dinyalakan kembali (modul ini dijalankan dengan miner mati).
trap 'log "sinyal berhenti (INT/TERM/HUP)"; exit 130' INT TERM HUP

# ------------------------------------------------------------------
# 1. Preflight
# ------------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    printf 'harus root: systemctl stop/start + proses miner ter-hide dari user biasa\n' >&2
    exit 2
fi
if [ ! -x "$BIN" ]; then
    die "binary tidak ditemukan/eksekutabel: $BIN"
fi
if ! command -v pgrep >/dev/null 2>&1; then
    die "pgrep tidak ada (paket procps) — dipakai menghitung proses miner"
fi
if ! systemctl cat "$UNIT" >/dev/null 2>&1; then
    die "unit systemd tidak ada: $UNIT (pakai UNIT=<nama> bila berbeda)"
fi
case $NONCES in
    ''|*[!0-9]*) die "NONCES harus angka, dapat '$NONCES'" ;;
esac

mkdir -p "$OUTDIR" 2>/dev/null || die "tidak bisa menulis $OUTDIR"

BEFORE="$(count_miners)"
log "unit=$UNIT bin=$BIN nonces=$NONCES threads=\"$THREADS_LIST\""
log "PROSES sebelum sweep: $BEFORE"

if [ "$BEFORE" -gt 1 ]; then
    die "$BEFORE proses xcb/sysd sudah berjalan; bereskan dulu (jangan tambah beban)"
fi
if [ "$BEFORE" -eq 0 ]; then
    log "PERINGATAN: tidak ada miner berjalan sebelum sweep — tidak ada baseline"
fi

HP_TOTAL="$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo 2>/dev/null || true)"
HP_FREE="$(awk '/^HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null || true)"
log "hugepages: total=${HP_TOTAL:-?} free=${HP_FREE:-?}"
case ${HP_FREE:-0} in
    ''|*[!0-9]*) HP_FREE=0 ;;
esac
if [ "$HP_FREE" -lt 300 ]; then
    log "PERINGATAN: HugePages_Free < 300. RandomY full mode butuh ~260 halaman saat init;"
    log "             kalau alloc gagal, run akan jatuh ke 4K page dan turun jauh — hasil tidak sebanding."
fi
if [ "$BEFORE" -ge 1 ]; then
    log "HR baseline (journal): $(journalctl -u "$UNIT" -n 200 --no-pager 2>/dev/null | grep 'HR ' | tail -n 1 || true)"
fi

if [ "$DRY_RUN" = 1 ]; then
    log "DRY_RUN=1: rencana = stop $UNIT, lalu:"
    for t in $THREADS_LIST; do
        log "  $BIN --ui=log --benchmark $NONCES -t $t"
    done
    log "lalu start $UNIT dan verifikasi tepat 1 proses. Tidak ada yang diubah."
    exit 0
fi

# ------------------------------------------------------------------
# 2. Stop service — tidak ada benchmark sebelum 0 proses
# ------------------------------------------------------------------
log "systemctl stop $UNIT"
if ! systemctl stop "$UNIT" >>"$LOG" 2>&1; then
    die "systemctl stop $UNIT gagal (service tidak disentuh)"
fi
STARTED=1
if ! wait_stopped; then
    die "masih ada proses xcb/sysd setelah ${WAIT_STOP}s — benchmark dibatalkan"
fi
log "0 proses xcb/sysd; mulai benchmark"

# ------------------------------------------------------------------
# 3. Benchmark tiap jumlah thread
# ------------------------------------------------------------------
: >"$TSV"
for t in $THREADS_LIST; do
    case $t in
        ''|*[!0-9]*) log "lewati -t '$t' (bukan angka)"; continue ;;
    esac
    [ "$t" -lt 1 ] && { log "lewati -t $t (minimal 1)"; continue; }

    out="$OUTDIR/xcb-bench-t$t-$STAMP.log"
    log "benchmark: -t $t, $NONCES nonce, timeout ${BENCH_TIMEOUT}s -> $out"
    t0="$(date +%s)"
    # Env miner sengaja dibuang: THREADS/FULL_MEM/LARGE_PAGES/ALGO dari
    # /etc/xcb/miner.env tidak boleh diam-diam mengubah hasil A/B.
    if env -u THREADS -u FULL_MEM -u LARGE_PAGES -u BENCHMARK -u POLL_MS -u ALGO \
           -u REPORT_HASHRATE -u WALLET -u POOL -u WORKER -u CONN_ADDRESS \
           timeout "$BENCH_TIMEOUT" "$BIN" --ui=log --benchmark "$NONCES" -t "$t" \
           >"$out" 2>&1
    then
        rc=0
    else
        rc=$?
    fi
    t1="$(date +%s)"

    # Baris akhir benchmark: "Done <N> nonces in <S>s" -> H/s = N / S.
    # Tiap ekspresi sed butuh -e sendiri: tanpa itu ekspresi kedua dibaca
    # sebagai nama file ("sed: can't read s/..."), hasilnya kosong.
    hs="$(sed -n -e 's/\033\[[0-9;]*m//g' \
                  -e 's/.*Done \([0-9][0-9]*\) nonces in \([0-9][0-9.]*\)s.*/\1 \2/p' "$out" \
          | tail -n 1 | awk '{ if ($2 > 0) printf "%.1f", $1 / $2 }')"
    tot="$(sed -n -e 's/\033\[[0-9;]*m//g' -e 's/.*Total: //p' "$out" | tail -n 1)"
    fm="$(sed -n -e 's/\033\[[0-9;]*m//g' -e 's/.*auto FULL_MEM=\([01]\).*/\1/p' "$out" | head -n 1)"
    lp="$(sed -n -e 's/\033\[[0-9;]*m//g' -e 's/.*auto LARGE_PAGES=\([01]\).*/\1/p' "$out" | head -n 1)"

    if [ "$rc" -ne 0 ] || [ -z "$hs" ]; then
        log "  -t $t GAGAL (exit $rc setelah $((t1 - t0))s; lihat $out)"
        printf '%s\tNA\tNA\t%s\t%s\t%s\n' "$t" "$rc" "${fm:-?}" "${lp:-?}" >>"$TSV"
        FAILS=$((FAILS + 1))
        continue
    fi

    log "  -t $t: $hs H/s (dilaporkan: ${tot:-?}), wall $((t1 - t0))s, FULL_MEM=${fm:-?} LARGE_PAGES=${lp:-?}"
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$t" "$hs" "${tot:-NA}" "$rc" "${fm:-?}" "${lp:-?}" >>"$TSV"

    # Run tanpa full mem / hugepages tidak sebanding dengan yang memakai:
    # bedanya bisa sampai 6x (light) dan puluhan persen (4K page).
    if [ "${fm:-1}" = 0 ] || [ "${lp:-1}" = 0 ]; then
        log "  PERINGATAN: run ini tidak memakai FULL_MEM=1/LARGE_PAGES=1 — jangan dibandingkan langsung"
    fi
done

# ------------------------------------------------------------------
# 4. Ringkasan
# ------------------------------------------------------------------
log "ringkasan:"
if [ -s "$TSV" ]; then
    awk -F'\t' '$2 != "NA" { printf "  -t %-3s %10s H/s  (%7.1f H/s/thread)\n", $1, $2, $2 / $1 }' "$TSV" \
        | tee -a "$LOG"
    best="$(awk -F'\t' '$2 != "NA" && $2 > max { max = $2; t = $1 } END { if (t != "") printf "-t %s (%.1f H/s)", t, max }' "$TSV")"
    if [ -n "$best" ]; then
        log "tercepat: $best"
    fi
    log "tabel: $TSV"
else
    log "  (tidak ada hasil)"
fi

# ------------------------------------------------------------------
# 5. Selalu pulihkan, lalu putuskan exit code
# ------------------------------------------------------------------
restore
if [ "$FAILS" -gt 0 ] || [ "$VERIFY_OK" != 1 ]; then
    log "selesai DENGAN MASALAH (gagal=$FAILS, verifikasi=$VERIFY_OK)"
    exit 1
fi
log "selesai: $UNIT aktif, tepat 1 miner, log di $LOG"
exit 0
