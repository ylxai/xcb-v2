#!/bin/sh
# xcb-thread-sweep.sh — A/B jumlah thread (-t) miner XCB/RandomY di satu host.
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
# KHUSUS XCB/RandomY. Host yang menjalankan backend CONN (`-a conn`, yang
# ditulis install.sh) ditolak: main.cpp menangani `algo == conn` sebelum jalur
# benchmark, jadi `--benchmark` di sana bukan benchmark sama sekali dan sweep
# akan menambang, bukan mengukur.
#
# Alur:
#   1. cek kondisi awal (harus tepat 1 miner, atau 0 dengan peringatan)
#   2. systemctl stop <UNIT>, tunggu sampai 0 proses xcb/sysd
#   3. untuk tiap -t: <BIN> --ui=log --benchmark <NONCES> -t <N> -a randomy
#   4. ringkasan H/s per -t (dan H/s per thread)
#   5. SELALU systemctl start <UNIT> lagi (kalau memang aktif sebelumnya),
#      lalu verifikasi tepat satu proses + baris HR baru dari PID baru
#      (trap EXIT/INT/TERM, jadi Ctrl+C pun memulihkan service)
#
# Pakai:
#   sudo sh xcb-thread-sweep.sh                      # -t 2 4 6, 200k nonce
#   sudo NONCES=400000 THREADS_LIST="2 4" sh xcb-thread-sweep.sh
#   sudo DRY_RUN=1 sh xcb-thread-sweep.sh            # cetak rencana, tanpa stop
#
# Env: UNIT (syscached), BIN (/usr/local/bin/xcb),
#      NONCES (200000; 1..1000000000 — di luar itu biner memakai 0 = mining),
#      THREADS_LIST ("2 4 6"; tiap nilai 1..4096, di atas itu jadi auto),
#      BENCH_TIMEOUT (900 detik per run), WAIT_STOP (30), WAIT_START (60),
#      POST_WAIT (30), OUTDIR (/root), LOCK (/run/xcb-thread-sweep.lock),
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
# Pakai `-` (bukan `:-`): nilai kosong eksplisit harus DITOLAK, bukan diam-diam
# berubah jadi default. `THREADS_LIST= sh ...` pernah menjalankan sweep penuh
# 3x200k karena `:-` menganggap string kosong sebagai "tidak diset".
NONCES="${NONCES-200000}"
THREADS_LIST="${THREADS_LIST-2 4 6}"
BENCH_TIMEOUT="${BENCH_TIMEOUT:-900}"
WAIT_STOP="${WAIT_STOP:-30}"
WAIT_START="${WAIT_START:-60}"
POST_WAIT="${POST_WAIT:-30}"
OUTDIR="${OUTDIR:-/root}"
LOCK="${LOCK:-/run/xcb-thread-sweep.lock}"
DRY_RUN="${DRY_RUN:-0}"

STAMP="$(date -u +%Y%m%d-%H%M%S)"
LOG="$OUTDIR/xcb-thread-sweep-$STAMP.log"
TSV="$OUTDIR/xcb-thread-sweep-$STAMP.tsv"

STARTED=0       # 1 sejak perintah stop dijalankan (restore wajib mencoba start)
RESTORED=0      # 1 hanya setelah start + verifikasi sukses (trap boleh retry)
VERIFY_OK=0     # 1 kalau pasca-sweep terverifikasi: 1 proses + HR baru
FAILS=0         # jumlah run benchmark yang gagal
RAN=0           # jumlah benchmark yang benar-benar dieksekusi
SINCE_START=""  # timestamp lokal saat service dinyalakan ulang (filter HR)
WAS_ACTIVE=""   # status unit sebelum sweep; unit yang mati tidak dinyalakan
BENCH_PID=""    # PID benchmark yang sedang jalan (dibunuh saat sinyal berhenti)

log() {
    printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$LOG"
}

die() {
    log "FATAL: $*"
    exit 1
}

is_uint() {
    case $1 in
        ''|*[!0-9]*) return 1 ;;
    esac
    return 0
}

# comm proses XCB: "sysd" kalau XCB_TITLE=sysd, "xcb" kalau build biasa.
# pgrep -x mencocokkan nama proses persis. Skrip ini root karena hidepid=2
# menyembunyikan proses miner dari user lain.
count_miners() {
    n_sysd="$(pgrep -x sysd 2>/dev/null | wc -l)"
    n_xcb="$(pgrep -x xcb 2>/dev/null | wc -l)"
    printf '%s\n' "$((n_sysd + n_xcb))"
}

# Algoritma yang dipakai service. Tiga sumber, berurutan:
#   1. cmdline miner — biasanya TIDAK bisa dipakai: biner menimpa argv menjadi
#      "sysd" (XCB_TITLE), jadi hanya berguna untuk build tanpa judul itu.
#   2. ExecStart dari unit + isi script wrapper-nya. install.sh menulis
#      `set -- -a conn ...` untuk CONN, sedangkan instalasi lama (fl) hanya
#      `exec /usr/local/bin/xcb --ui=log` (XCB default).
# "unknown" = tidak ada sumber yang bisa dibaca; jangan menebak.
# "xcb" = sumber terbaca dan memang tidak ada `-a` (default XCB).
probe_algo() {
    pid="$(pgrep -x sysd 2>/dev/null | head -n 1)"
    [ -n "$pid" ] || pid="$(pgrep -x xcb 2>/dev/null | head -n 1)"
    if [ -n "$pid" ]; then
        args="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null || true)"
        case $args in
            *--ui=*)
                algo="$(printf '%s\n' "$args" | awk '
                    $0 == "-a" || $0 == "--algo" { getline; print; exit }
                    index($0, "--algo=") == 1 { sub("^--algo=", ""); print; exit }
                    index($0, "-a=") == 1 { sub("^-a=", ""); print; exit }
                ')"
                if [ -n "$algo" ]; then
                    printf '%s\n' "$algo"
                else
                    printf 'xcb\n'
                fi
                return 0
                ;;
        esac
    fi

    blob="$(systemctl show "$UNIT" -p ExecStart --value 2>/dev/null || true)"
    [ -n "$blob" ] || { printf 'unknown\n'; return 0; }
    exec_path="$(printf '%s\n' "$blob" | sed -n 's/.*path=\([^ ;]*\).*/\1/p')"
    if [ -n "$exec_path" ] && [ -f "$exec_path" ] \
       && head -c 2 "$exec_path" 2>/dev/null | grep -q '#!'; then
        blob="$blob $(tr '\n' ' ' < "$exec_path" 2>/dev/null || true)"
    fi
    algo="$(printf '%s\n' "$blob" | sed -n 's/.*--algo=\([a-z0-9][a-z0-9]*\).*/\1/p' | head -n 1)"
    if [ -z "$algo" ]; then
        algo="$(printf '%s\n' "$blob" \
                | sed -n 's/.*[[:space:]]-a[[:space:]]\{1,\}\([a-z0-9][a-z0-9]*\).*/\1/p' | head -n 1)"
    fi
    if [ -n "$algo" ]; then
        printf '%s\n' "$algo"
    else
        printf 'xcb\n'
    fi
}

# Proses benchmark yatim dari sweep sebelumnya (mis. skrip di-SIGKILL saat
# `timeout xcb --benchmark` masih jalan). Mereka memakai CPU yang sama, jadi
# angka sweep baru tidak sebanding — deteksi, jangan diukur.
stray_benchmarks() {
    for p in $(pgrep -x xcb 2>/dev/null); do
        if tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- '--benchmark'; then
            printf '%s\n' "$p"
        fi
    done
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
    # Batasi ke log sejak start DAN ke PID baru: baris HR milik proses sebelum
    # sweep akan lolos kalau hanya memakai `journalctl -n 200`.
    hrline="$(journalctl -u "$UNIT" --since "$SINCE_START" --no-pager 2>/dev/null \
              | grep 'HR ' | grep -F "[$mp]" | tail -n 1 || true)"
    if [ -n "$hrline" ]; then
        log "HR terakhir (pid $mp): $hrline"
    else
        log "GAGAL: belum ada baris HR dari pid $mp sejak $SINCE_START"
        VERIFY_OK=0
        return 1
    fi
    VERIFY_OK=1
    return 0
}

# Dipanggil dari trap EXIT/INT/TERM dan sekali di akhir alur normal.
# RESTORED baru diset setelah start + verifikasi sukses, supaya trap masih
# boleh mencoba lagi kalau percobaan pertama gagal di tengah jalan.
restore() {
    if [ "$RESTORED" = 1 ]; then
        return 0
    fi
    if [ "$STARTED" != 1 ]; then
        return 0
    fi
    if [ "$WAS_ACTIVE" != "active" ]; then
        log "unit tidak aktif sebelum sweep — dibiarkan mati (kondisi awal dipertahankan)"
        RESTORED=1
        return 0
    fi
    n="$(count_miners)"
    if [ "$n" -gt 0 ]; then
        log "GAGAL: $n proses miner masih berjalan; $UNIT TIDAK dinyalakan (hindari dua miner)"
        VERIFY_OK=0
        return 1
    fi
    SINCE_START="$(date '+%Y-%m-%d %H:%M:%S')"
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
    verify_single || return 1
    RESTORED=1
    return 0
}

trap 'restore' EXIT
# HUP ikut ditangkap: kalau skrip dijalankan lewat SSH lalu koneksinya putus,
# service harus tetap dinyalakan kembali (modul ini dijalankan dengan miner mati).
# Benchmark yang sedang jalan dibunuh dulu: kalau tidak, ia menjadi proses
# yatim yang terus berebut CPU dengan miner yang baru dinyalakan.
# Benchmark dibunuh dengan KILL: SIGTERM tidak dihentikan biner ini seketika
# (terbukti di fl — proses masih hidup 2 detik setelah TERM), dan benchmark
# yatim yang tertinggal akan berebut CPU dengan miner yang baru dinyalakan.
trap 'log "sinyal berhenti (INT/TERM/HUP)"; if [ -n "$BENCH_PID" ]; then kill -9 "$BENCH_PID" 2>/dev/null; fi; exit 130' INT TERM HUP

# ------------------------------------------------------------------
# 1. Preflight — semua validasi sebelum ada downtime
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

is_uint "$NONCES" || die "NONCES harus angka >= 1, dapat '$NONCES'"
# Di luar 1..1000000000 biner jatuh ke 0 -> mode mining biasa, bukan benchmark.
if [ "$NONCES" -lt 1 ] || [ "$NONCES" -gt 1000000000 ]; then
    die "NONCES harus 1..1000000000 (0 atau terlalu besar = mode mining, bukan benchmark)"
fi
for pair in "BENCH_TIMEOUT:$BENCH_TIMEOUT" "WAIT_STOP:$WAIT_STOP" \
            "WAIT_START:$WAIT_START" "POST_WAIT:$POST_WAIT"; do
    name="${pair%%:*}"
    val="${pair#*:}"
    is_uint "$val" || die "$name harus angka >= 1, dapat '$val'"
    [ "$val" -ge 1 ] || die "$name harus >= 1, dapat '$val'"
done
[ -n "$THREADS_LIST" ] || die "THREADS_LIST kosong — tidak ada yang bisa diukur"
for t in $THREADS_LIST; do
    is_uint "$t" || die "THREADS_LIST berisi '$t' (bukan angka)"
    # -t di luar 1..4096 diparse jadi 0 = auto-threads, bukan nilai yang diminta.
    if [ "$t" -lt 1 ] || [ "$t" -gt 4096 ]; then
        die "THREADS_LIST berisi $t; harus 1..4096 (di luar itu biner memakai auto)"
    fi
done

mkdir -p "$OUTDIR" 2>/dev/null || die "tidak bisa menulis $OUTDIR"

# Kunci host-wide: dua sweep (atau sweep + deploy) di host yang sama akan
# saling menyalakan/mematikan miner dan mencampur angka.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK" || die "tidak bisa membuat lock $LOCK"
    if ! flock -n 9; then
        die "sweep lain sedang berjalan (lock $LOCK) — tunggu sampai selesai"
    fi
fi

BEFORE="$(count_miners)"
WAS_ACTIVE="$(systemctl is-active "$UNIT" 2>/dev/null || true)"
log "unit=$UNIT bin=$BIN nonces=$NONCES threads=\"$THREADS_LIST\""
log "PROSES sebelum sweep: $BEFORE; unit=$WAS_ACTIVE"

if [ "$BEFORE" -gt 1 ]; then
    die "$BEFORE proses xcb/sysd sudah berjalan; bereskan dulu (jangan tambah beban)"
fi
STRAY="$(stray_benchmarks | tr '\n' ' ')"
if [ -n "$STRAY" ]; then
    die "benchmark yatim masih jalan (PID: $STRAY) — hentikan dulu (kill -9), angka sweep ini tidak akan sebanding"
fi
if [ "$BEFORE" -eq 0 ]; then
    log "PERINGATAN: tidak ada miner berjalan sebelum sweep — tidak ada baseline"
fi

# Sweep ini hanya untuk RandomY. Backend CONN tidak punya jalur benchmark
# (main.cpp menangani algo == conn lebih dulu), jadi mengukurnya di sini salah.
ALGO_SVC="$(probe_algo)"
case $ALGO_SVC in
    conn) die "host ini menjalankan backend CONN (-a conn); sweep ini hanya untuk XCB/RandomY" ;;
    unknown) log "PERINGATAN: algo service tidak terdeteksi — benchmark dipaksa -a randomy" ;;
    xcb) log "algo service: XCB/RandomY" ;;
    *) log "algo service: $ALGO_SVC (benchmark tetap -a randomy)" ;;
esac

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
        log "  $BIN --ui=log --benchmark $NONCES -t $t -a randomy"
    done
    log "lalu start $UNIT (kalau aktif sebelumnya) dan verifikasi 1 proses + HR baru. Tidak ada yang diubah."
    exit 0
fi

# ------------------------------------------------------------------
# 2. Stop service — tidak ada benchmark sebelum 0 proses
# ------------------------------------------------------------------
log "systemctl stop $UNIT"
# STARTED diset SEBELUM stop: kalau stop mengembalikan non-zero padahal unit
# sudah berhenti, trap tetap wajib menyalakannya kembali.
STARTED=1
if ! systemctl stop "$UNIT" >>"$LOG" 2>&1; then
    if [ "$(systemctl is-active "$UNIT" 2>/dev/null || true)" = inactive ]; then
        log "systemctl stop non-zero tapi unit inactive — lanjut (restore tetap start)"
    else
        die "systemctl stop $UNIT gagal dan unit masih aktif — benchmark dibatalkan"
    fi
fi
if ! wait_stopped; then
    die "masih ada proses xcb/sysd setelah ${WAIT_STOP}s — benchmark dibatalkan"
fi
log "0 proses xcb/sysd; mulai benchmark"

# ------------------------------------------------------------------
# 3. Benchmark tiap jumlah thread
# ------------------------------------------------------------------
: >"$TSV"
for t in $THREADS_LIST; do
    out="$OUTDIR/xcb-bench-t$t-$STAMP.log"
    log "benchmark: -t $t, $NONCES nonce, timeout ${BENCH_TIMEOUT}s -> $out"
    RAN=$((RAN + 1))
    t0="$(date +%s)"
    # Env miner sengaja dibuang: THREADS/FULL_MEM/LARGE_PAGES/ALGO dari
    # /etc/xcb/miner.env tidak boleh diam-diam mengubah hasil A/B. Algo
    # dipaksa randomy (XCB) dan log level dipaksa info supaya baris "Done"
    # tidak hilang kalau host punya LOG_LEVEL=warn.
    env -u THREADS -u FULL_MEM -u LARGE_PAGES -u BENCHMARK -u POLL_MS -u ALGO \
        -u REPORT_HASHRATE -u WALLET -u POOL -u WORKER -u CONN_ADDRESS \
        LOG_LEVEL=info \
        timeout "$BENCH_TIMEOUT" "$BIN" --ui=log --benchmark "$NONCES" \
        -t "$t" -a randomy >"$out" 2>&1 &
    BENCH_PID=$!
    wait "$BENCH_PID"
    rc=$?
    BENCH_PID=""
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
if [ "$RAN" -eq 0 ]; then
    log "GAGAL: tidak ada benchmark yang dijalankan"
    exit 1
fi
if [ "$FAILS" -gt 0 ] || [ "$VERIFY_OK" != 1 ]; then
    log "selesai DENGAN MASALAH (gagal=$FAILS, verifikasi=$VERIFY_OK)"
    exit 1
fi
log "selesai: $UNIT aktif, tepat 1 miner, log di $LOG"
exit 0
