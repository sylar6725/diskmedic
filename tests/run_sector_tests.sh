#!/bin/sh
# Tests per sector format: 512n (512/512), 512e (512 logical / 4096 physical) and
# 4Kn (4096/4096), on virtual in-memory SCSI disks (scsi_debug kernel module).
# Never touches real disks. Needs root and scsi_debug; otherwise: SKIP.
set -eu
if [ "$(id -u)" -ne 0 ] || ! modprobe -n scsi_debug 2>/dev/null; then
    echo "SKIP: sector format tests (root + scsi_debug module required)"; exit 0
fi
if grep -q '^scsi_debug ' /proc/modules; then
    echo "SKIP: scsi_debug is already loaded by something else"; exit 0
fi
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
LOADED=0
BIN="$T/diskmedic"
cc -O2 -Wall -Wextra -Werror -std=c99 "$ROOT/diskmedic.c" -o "$BIN"

unload() {
    [ "$LOADED" -eq 1 ] || return 0
    command -v udevadm >/dev/null 2>&1 && udevadm settle 2>/dev/null || true
    i=0
    while ! rmmod scsi_debug 2>/dev/null; do
        i=$((i + 1)); [ "$i" -ge 20 ] && { echo "cannot unload scsi_debug" >&2; return 1; }
        sleep 0.5
    done
    LOADED=0
}
cleanup() { unload || true; rm -rf "$T"; }
trap cleanup EXIT HUP INT TERM

fail() { echo "FAILED [$FMT]: $*" >&2; cat "$T/err" >&2 2>/dev/null || true; exit 1; }
run() { # run <stdin> <expected exit> <args…>
    input=$1; wanted=$2; shift 2
    set +e; printf '%s\n' "$input" | "$BIN" "$@" >"$T/out" 2>"$T/err"; got=$?; set -e
    [ "$got" -eq "$wanted" ] || fail "exit $got instead of $wanted for: $*"
}
load() { # load <logical sector size> <physical exponent> [<failing LBA> [<count>]]
    if [ $# -ge 3 ]; then
        modprobe scsi_debug dev_size_mb=64 sector_size="$1" physblk_exp="$2" opts=2 \
            medium_error_start="$3" medium_error_count="${4:-1}"
    else
        modprobe scsi_debug dev_size_mb=64 sector_size="$1" physblk_exp="$2" ${SD_EXTRA:-}
    fi
    LOADED=1
    command -v udevadm >/dev/null 2>&1 && udevadm settle 2>/dev/null || true
    DEV=""
    for i in 1 2 3 4 5 6 7 8 9 10; do
        for b in /sys/block/sd*; do
            [ -e "$b/device/model" ] && grep -q scsi_debug "$b/device/model" && DEV="/dev/${b##*/}"
        done
        [ -n "$DEV" ] && [ -b "$DEV" ] && break
        sleep 0.5
    done
    [ -n "$DEV" ] || fail "scsi_debug disk not found"
    LS=$(cat "/sys/block/${DEV#/dev/}/queue/logical_block_size")
    PS=$(cat "/sys/block/${DEV#/dev/}/queue/physical_block_size")
    [ "$LS" -eq "$1" ] || fail "logical sector $LS instead of $1"
    [ "$PS" -eq $(( $1 << $2 )) ] || fail "physical sector $PS instead of $(( $1 << $2 ))"
}
# Checksum of all logical sectors except [<first>, +<count>) (the unreadable sectors).
md5_except() { # md5_except <first excluded LBA> [<count>]
    total=$(( $(blockdev --getsize64 "$DEV") / LS )); cnt=${2:-1}
    # Direct reads in 1 MiB blocks, bounded in bytes so the excluded sectors are never touched.
    { dd if="$DEV" bs=1M iflag=direct,count_bytes count=$(( $1 * LS )) status=none
      dd if="$DEV" bs=1M iflag=direct,skip_bytes,count_bytes skip=$(( ($1 + cnt) * LS )) \
         count=$(( (total - $1 - cnt) * LS )) status=none; } | md5sum
}
# Number of failed reads reported by the kernel for $DEV since the mark set by kmsg_mark.
kmsg_mark() { MARK="diskmedic-mark-$$-$1"; echo "$MARK" >/dev/kmsg; }
kmsg_read_errors() {
    dmesg | sed -n "/$MARK/,\$p" | grep -c "critical medium error, dev ${DEV#/dev/}, sector .* op 0x0:(READ)" || true
}
nonzero() { tr -d '\000' <"$DEV" | wc -c | tr -d ' '; }
# Copies $T/rnd to $DEV over [first sector, +count), with O_DIRECT only: a partial buffered
# write would cause a read-modify-write of the 4 KiB page, which fails if that page
# contains the unreadable sector.
wr_range() { # wr_range <first sector> <number of sectors>
    s=$1; n=$2; per=$(( 1048576 / LS ))
    head=$(( (per - s % per) % per )); [ "$head" -gt "$n" ] && head=$n
    [ "$head" -gt 0 ] && dd if="$T/rnd" of="$DEV" bs="$LS" skip="$s" seek="$s" count="$head" oflag=direct status=none
    s=$(( s + head )); n=$(( n - head )); full=$(( n / per ))
    [ "$full" -gt 0 ] && dd if="$T/rnd" of="$DEV" bs=1M skip=$(( s / per )) seek=$(( s / per )) count="$full" oflag=direct status=none
    s=$(( s + full * per )); n=$(( n - full * per ))
    [ "$n" -gt 0 ] && dd if="$T/rnd" of="$DEV" bs="$LS" skip="$s" seek="$s" count="$n" oflag=direct status=none
    return 0
}

for spec in "512n 512 0" "512e 512 3" "4Kn 4096 0"; do
    set -- $spec; FMT=$1; SS=$2; EXP=$3
    echo "== $FMT (logical $SS B, physical $(( SS << EXP )) B)"

    # Healthy disk.
    load "$SS" "$EXP"
    dd if=/dev/urandom of="$DEV" bs=1M count=64 oflag=direct status=none
    echo "  - smart (SCSI: INQUIRY + LOG SENSE pages)"
    run "" 0 "$DEV" --smart
    grep -q "━━ Linux scsi_debug ━━" "$T/out" || fail "SCSI identity missing"
    grep -q "Sectors   : $FMT (" "$T/out" || fail "format $FMT not recognized"
    grep -q 'Health (firmware assessment): OK' "$T/out" || fail "page 2Fh not decoded"
    grep -q 'Temperature  *[0-9][0-9]* °C (max. [0-9]* °C)' "$T/out" || fail "page 0Dh not decoded"
    echo "  - verify";      run "" 0 "$DEV" --verify
    echo "  - scan-slow";   run "" 0 "$DEV" --scan-slow
    grep -q 'Note (SAS/SCSI SSD' "$T/err" || fail "SSD note missing from the slow scan"
    # scsi_debug presents itself as an SSD (VPD B1h): zeros and repair refused without --force.
    echo "  - SSD: zero and slow-repair refused without --force, data intact"
    before=$(md5sum <"$DEV")
    run "YES" 1 "$DEV" --zero;        grep -q 'Zero-fill not recommended' "$T/err" || fail "SSD refusal of --zero missing"
    run "YES" 1 "$DEV" --slow-repair; grep -q 'Slow-block repair not recommended' "$T/err" || fail "SSD refusal of --slow-repair missing"
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "a refusal modified the data"
    echo "  - trim without UNMAP: refused, data intact"
    run "YES" 1 "$DEV" --trim; grep -q 'does not support TRIM' "$T/err" || fail "TRIM refusal missing"
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "refused TRIM modified the data"
    echo "  - slow-repair --force: data intact"
    run "YES" 0 "$DEV" --slow-repair --force
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "slow-repair modified the data"
    echo "  - zero --force";  run "YES" 0 "$DEV" --zero --force
    [ "$(nonzero)" -eq 0 ] || fail "non-zero bytes after --zero"
    unload

    # TRIM: disk with UNMAP (lbpu=1), freed blocks read back as zeros (lbprz=1).
    SD_EXTRA="lbpu=1 lbprz=1"; load "$SS" "$EXP"; SD_EXTRA=""
    dd if=/dev/urandom of="$DEV" bs=1M count=64 oflag=direct status=none
    echo "  - trim: refused without YES, then the whole disk is freed"
    before=$(md5sum <"$DEV"); run "no" 2 "$DEV" --trim
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "unconfirmed trim modified the data"
    run "YES" 0 "$DEV" --trim
    grep -q 'everything is zero' "$T/err" || fail "zero read-back after TRIM not reported"
    [ "$(nonzero)" -eq 0 ] || fail "non-zero bytes after --trim"
    unload

    # One unreadable logical sector in the middle of a physical sector of block 5.
    BAD=$(( 5 * 1048576 / SS + 3 ))
    load "$SS" "$EXP" "$BAD"
    dd if=/dev/urandom of="$T/rnd" bs=1M count=64 status=none
    wr_range 0 "$BAD"
    wr_range $(( BAD + 1 )) $(( 64 * 1048576 / SS - BAD - 1 ))
    rm -f "$T/rnd"
    echo "  - verify: LBA $BAD reported"
    run "" 1 "$DEV" --verify --log "$T/v.csv"
    grep -q "LBA $BAD " "$T/err" || fail "LBA $BAD not reported"
    grep -q '1 unreadable sector(s)' "$T/err" || fail "1 unreadable sector expected"
    grep -q "\"$BAD\",\"DEFECTIVE" "$T/v.csv" || fail "LBA $BAD missing from the log"
    echo "  - slow-repair: a single physical sector rewritten, neighbours kept"
    before=$(md5_except "$BAD")
    run "YES" 1 "$DEV" --slow-repair --force   # scsi_debug keeps the sector failing: verification failure expected
    grep -q '(1 physical sector(s) rewritten' "$T/out" || fail "a single rewritten physical sector expected"
    [ "$before" = "$(md5_except "$BAD")" ] || fail "readable sectors were modified"
    unload
done

# Realistic 512e case: a whole PHYSICAL sector is lost, hence its 8 logical sectors.
FMT="512e physical sector lost"; echo "== $FMT"
BAD=10240
load 512 3 "$BAD" 8
dd if=/dev/urandom of="$T/rnd" bs=1M count=64 status=none
wr_range 0 "$BAD"
wr_range $(( BAD + 8 )) $(( 64 * 2048 - BAD - 8 ))
rm -f "$T/rnd"
echo "  - verify: 8 LBAs reported with at most 4 failed reads (instead of 9)"
sleep 6   # lets the kernel message rate limit drain
kmsg_mark verify
run "" 1 "$DEV" --verify --log "$T/p.csv"
grep -q '8 unreadable sector(s)' "$T/err" || fail "8 unreadable sectors expected"
for l in 10240 10241 10242 10243 10244 10245 10246 10247; do
    grep -q "\"$l\",\"DEFECTIVE" "$T/p.csv" || fail "LBA $l missing from the log"
done
nerr=$(kmsg_read_errors)
[ "$nerr" -le 4 ] || fail "$nerr failed reads (at most 4 expected)"
echo "    ($nerr failed reads)"
echo "  - slow-repair: a single physical sector rewritten, the rest intact"
before=$(md5_except "$BAD" 8)
run "YES" 1 "$DEV" --slow-repair --force
grep -q '(1 physical sector(s) rewritten' "$T/out" || fail "a single rewritten physical sector expected"
grep -q 'unreadable sectors: 8' "$T/out" || fail "8 unreadable sectors expected in the scan"
[ "$before" = "$(md5_except "$BAD" 8)" ] || fail "readable sectors were modified"
unload

# Multi-disk selection: two disks verified in parallel (pseudo-terminal provided by script).
# SAFETY: only the scsi_debug disks may be selected. The test does not rely on the position
# of the disks in the list (the menu order is not alphabetical: sdX before nvmeXnY): it reads
# the line under the cursor and only selects lines that show "scsi_debug".
# Before Enter, the screen must show exactly $NSEL selected disks; before YES, every target disk
# must be a scsi_debug disk. Otherwise the menu is left without starting anything, and the test fails.
# Each key is only sent once the expected screen is shown (start-up can be slow), and the
# pseudo-terminal is bounded in time: a hang makes the test fail instead of freezing it.
wait_screen() { # wait_screen <text> <maximum number of half-seconds>
    j=0; while ! grep -aq "$1" "$T/ts" 2>/dev/null && [ "$j" -lt "$2" ]; do sleep 0.5; j=$((j + 1)); done
}
cursor_frames() { grep -ac '> \[.\] /dev/' "$T/ts" 2>/dev/null || true; }
press() { # press <key>: sends a key and waits for the menu to be redrawn
    n=$(cursor_frames); printf "$1"; j=0
    while [ "$(cursor_frames)" -le "$n" ] && [ "$j" -lt 40 ]; do sleep 0.25; j=$((j + 1)); done
}
abort_tui() { echo "$1" >"$T/abort"; printf 'q'; sleep 0.5; printf 'q'; sleep 1; }
tui_run() { # tui_run <input function>: drives "diskmedic --tui" in a pseudo-terminal
    : >"$T/ts"; rm -f "$T/abort"
    "$1" | (cd "$T/par" && timeout -k 5 300 script -qfec "stty rows 30 cols 90; $BIN --tui" /dev/null) >"$T/ts" 2>&1 || true
    cp "$T/ts" "$T/err"
    if [ -e "$T/abort" ]; then fail "selection aborted for safety: $(cat "$T/abort")"; fi
}
select_disks() { # selects only the scsi_debug disks, confirms, then waits for the operation menu
    wait_screen 'MULTIPLE SELECTION' 60
    total=$(ls /sys/block | grep -cE '^(sd|hd|vd)[a-z]+$|^nvme[0-9]+n[0-9]+$')
    k=0
    while [ "$k" -lt "$total" ]; do
        case $(grep -a '> \[.\] /dev/' "$T/ts" | tail -1) in *scsi_debug*) press ' ' ;; esac
        press '\033[B'; k=$((k + 1))
    done
    case $(grep -a 'Selected: ' "$T/ts" | tail -1) in
        *"Selected: ${NSEL:-2} /"*) ;;
        *) abort_tui "the menu does not show exactly ${NSEL:-2} selected disk(s)"; return 1 ;;
    esac
    printf '\r'; wait_screen "CHOOSE THE OPERATION" 60
}
keys_verify() { select_disks || return 0; printf '2'; wait_screen 'press any key' 400; printf 'x'; sleep 1; }
keys_zero_trim() {
    select_disks || return 0
    printf '5'; wait_screen 'TRIM on these SSDs' 60
    if grep -a '^  /dev/.* (' "$T/ts" | grep -av scsi_debug | grep -q .; then
        printf '\r'; echo "a disk other than scsi_debug is in the selection" >"$T/abort"; sleep 1; return 0
    fi
    printf 't\r'; wait_screen 'Type exactly YES' 60
    # List of target disks ("/dev/x — model → operation"): all must be scsi_debug disks.
    if ! grep -a ' → ' "$T/ts" | grep -q . || grep -a ' → ' "$T/ts" | grep -av scsi_debug | grep -q .; then
        printf '\r'; echo "the list of disks to erase contains something other than scsi_debug" >"$T/abort"; sleep 1; return 0
    fi
    printf 'YES\r'; wait_screen 'press any key' 400; printf 'x'; sleep 1
}
if command -v script >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    FMT="parallel"; echo "== $FMT (2 disks, simultaneous verification from the menu)"
    modprobe scsi_debug num_tgts=2 dev_size_mb=64; LOADED=1
    command -v udevadm >/dev/null 2>&1 && udevadm settle 2>/dev/null || true
    sleep 1
    ndev=0
    for n in $(ls /sys/block | grep -E '^(sd|hd|vd)[a-z]+$|^nvme[0-9]+n[0-9]+$'); do
        grep -q scsi_debug "/sys/block/$n/device/model" 2>/dev/null && ndev=$((ndev + 1))
    done
    [ "$ndev" -eq 2 ] || fail "2 scsi_debug disks expected, $ndev found"
    mkdir "$T/par"
    tui_run keys_verify
    grep -q 'Verification · 2 disk(s) in parallel' "$T/ts" || fail "parallel dashboard missing"
    grep -q 'Running: 2 ' "$T/ts" || fail "the 2 disks did not run at the same time"
    [ "$(grep -c '^  \[OK\] ' "$T/ts")" -eq 2 ] || fail "2 OK disks expected in the summary"
    [ "$(ls "$T"/par/diskmedic-*/*.log | wc -l)" -eq 2 ] || fail "2 logs expected"
    [ "$(ls "$T"/par/diskmedic-*/*.csv | wc -l)" -eq 2 ] || fail "2 CSV logs expected"
    unload

    # Zero-fill requested on 2 SSDs: the menu offers TRIM instead (t), then YES.
    FMT="parallel SSD"; echo "== $FMT (zero-fill requested, TRIM chosen for the SSDs)"
    modprobe scsi_debug num_tgts=2 dev_size_mb=64 lbpu=1 lbprz=1; LOADED=1
    command -v udevadm >/dev/null 2>&1 && udevadm settle 2>/dev/null || true
    sleep 1
    DEVS=""
    for n in $(ls /sys/block | grep -E '^(sd|hd|vd)[a-z]+$|^nvme[0-9]+n[0-9]+$' | sort); do
        grep -q scsi_debug "/sys/block/$n/device/model" 2>/dev/null && DEVS="$DEVS /dev/$n"
    done
    for d in $DEVS; do dd if=/dev/urandom of="$d" bs=1M count=64 oflag=direct status=none; done
    rm -rf "$T/par"; mkdir "$T/par"
    tui_run keys_zero_trim
    grep -q 'SSDs in the selection' "$T/ts" || fail "SSDs not reported before the zero-fill"
    grep -q 'TRIM · 2 disk(s) in parallel' "$T/ts" || fail "TRIM dashboard missing"
    [ "$(grep -c '^  \[OK\] .*· TRIM' "$T/ts")" -eq 2 ] || fail "2 TRIM OK expected in the summary"
    for d in $DEVS; do [ "$(tr -d '\000' <"$d" | wc -c)" -eq 0 ] || fail "$d not emptied by the TRIM"; done
    unload

    # Single-disk menu (--tui /dev/sdX): a read verification writes its CSV log in diskmedic-*/.
    FMT="single-disk menu"; echo "== $FMT (verification from the menu, CSV log)"
    load 512 0
    rm -rf "$T/par"; mkdir "$T/par"; : >"$T/ts"
    keys_single() {
        wait_screen '· MENU' 60; printf '2'
        wait_screen 'Operation finished' 400; printf 'x'; sleep 0.5; printf 'q'; sleep 1
    }
    keys_single | (cd "$T/par" && timeout -k 5 300 script -qfec "stty rows 30 cols 90; $BIN --tui $DEV" /dev/null) >"$T/ts" 2>&1 || true
    cp "$T/ts" "$T/err"
    grep -aq 'Operation finished (code 0)' "$T/ts" || fail "verification from the single-disk menu did not succeed"
    CSV=$(ls "$T"/par/diskmedic-*/"${DEV#/dev/}.csv" 2>/dev/null | head -1)
    [ -n "$CSV" ] || fail "no CSV log created by the single-disk menu"
    head -1 "$CSV" | grep -q '^timestamp_iso8601,device,operation,LBA,status,action$' || fail "CSV log header missing"
    unload

    # SMART verdict: a disk that predicts a failure (Informational Exceptions test mode, enabled
    # with sdparm) must give exit code 4 and show as CRITICAL in the multi-disk summary, not OK.
    if command -v sdparm >/dev/null 2>&1; then
        FMT="SMART critical"; echo "== $FMT (predicted failure: exit code 4, CRITICAL in the summary)"
        # scsi_debug disks share one mode page: a single disk, healthy first, then failing.
        load 512 0
        run "" 0 "$DEV" --smart
        grep -q 'VERDICT: HEALTHY' "$T/out" || fail "HEALTHY verdict missing"
        sdparm -q --set MRIE=6 --set TEST=1 "$DEV" || fail "sdparm could not enable the failure prediction test mode"
        run "" 4 "$DEV" --smart
        grep -q 'VERDICT: CRITICAL' "$T/out" || fail "CRITICAL verdict missing"
        keys_smart() { select_disks || return 0; printf '1'; wait_screen 'press any key' 400; printf 'x'; sleep 1; }
        rm -rf "$T/par"; mkdir "$T/par"
        NSEL=1; tui_run keys_smart; NSEL=2
        grep -q "^  \[CRITICAL\] *$DEV " "$T/ts" || fail "$DEV not shown as CRITICAL in the summary"
        grep -q 'SMART alerts: 1' "$T/ts" || fail "SMART alert not counted on the dashboard"
        unload
    else
        echo "SKIP: SMART critical test (sdparm missing)"
    fi
else
    echo "SKIP: parallel multi-disk test (script or timeout command missing)"
fi
printf 'OK (sector formats)\n'
