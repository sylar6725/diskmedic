#!/bin/sh
# Functional tests on a virtual disk (loop device backed by a file).
# Never touches real disks. Needs root and losetup; otherwise: SKIP.
set -eu
if [ "$(id -u)" -ne 0 ] || ! command -v losetup >/dev/null 2>&1; then
    echo "SKIP: loop device tests (root + losetup required)"; exit 0
fi
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
LOOP=""; DM=""
cleanup() {
    umount "$T/mnt" 2>/dev/null || true
    [ -n "$DM" ] && dmsetup remove --retry "$DM" 2>/dev/null || true
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT HUP INT TERM
BIN="$T/diskmedic"
cc -O2 -Wall -Wextra -Werror -std=c99 "$ROOT/diskmedic.c" -o "$BIN"

fail() { echo "FAILED: $*" >&2; cat "$T/err" >&2 2>/dev/null || true; exit 1; }
run() { # run <stdin> <expected exit> <args…>
    input=$1; wanted=$2; shift 2
    set +e; printf '%s\n' "$input" | "$BIN" "$@" >"$T/out" 2>"$T/err"; got=$?; set -e
    [ "$got" -eq "$wanted" ] || fail "exit $got instead of $wanted for: $*"
}
# These tests cover the "hard disk" path: the loop device is declared rotational. Otherwise, on a
# machine whose image file is on an SSD, the kernel reports it as non-rotational and DiskMedic
# (rightly) treats it as an SSD: zero-fill and slow-block repair would be refused.
set_rotational() { # set_rotational <device> <0|1>
    b=$(basename "$(readlink -f "$1")")
    echo "$2" >"/sys/block/$b/queue/rotational" 2>/dev/null || fail "queue/rotational cannot be changed for $b"
}
attach() { # attach <size>
    [ -n "$LOOP" ] && losetup -d "$LOOP"
    rm -f "$T/disk.img"; truncate -s "$1" "$T/disk.img"
    LOOP=$(losetup -f --show "$T/disk.img")
    set_rotational "$LOOP" 1
    dd if=/dev/urandom of="$LOOP" bs=64K count=$(( $(blockdev --getsize64 "$LOOP") / 65536 )) status=none
    sync
}
nonzero() { tr -d '\000' <"$1" | wc -c | tr -d ' '; }

attach 64M
echo "- verify"; run "" 0 "$LOOP" --verify --log "$T/v.csv"
grep -q 'O_DIRECT' "$T/err" || fail "verify without O_DIRECT"
echo "- scan-slow"; run "" 0 "$LOOP" --scan-slow --log "$T/s.csv"
echo "- smart on a non-ATA device"; run "" 1 "$LOOP" --smart
echo "- zero refused without YES"; before=$(md5sum <"$LOOP"); run "no" 2 "$LOOP" --zero
[ "$before" = "$(md5sum <"$LOOP")" ] || fail "data modified without confirmation"
echo "- slow-repair on a healthy disk: data intact, nothing confirmed"
run "YES" 0 "$LOOP" --slow-repair --log "$T/r.csv"
[ "$before" = "$(md5sum <"$LOOP")" ] || fail "slow-repair modified the data"
grep -q 'confirmed slow: 0' "$T/out" || fail "false positive confirmed on a healthy disk"
! grep -q 'REWRITE' "$T/r.csv" || fail "rewrite on a healthy disk"
echo "- zero"; run "YES" 0 "$LOOP" --zero
[ "$(nonzero "$LOOP")" -eq 0 ] || fail "non-zero bytes after --zero"
echo "- non-rotational according to the kernel: zero refused (probably an SSD)"
set_rotational "$LOOP" 0
run "YES" 1 "$LOOP" --zero
grep -q 'non-rotational according to the kernel' "$T/err" || fail "SSD refusal (kernel) missing"
set_rotational "$LOOP" 1

echo "- mounted disk refused"
if command -v mkfs.ext4 >/dev/null 2>&1; then
    mkfs.ext4 -q -F "$LOOP"; mkdir -p "$T/mnt"; mount "$LOOP" "$T/mnt"
    run "YES" 1 "$LOOP" --zero; grep -q 'is in use' "$T/err" || fail "disk-in-use message missing"
    run "YES" 1 "$LOOP" --slow-repair
    [ "$(nonzero "$LOOP")" -gt 0 ] || fail "mounted disk was erased"
    umount "$T/mnt"
else echo "  SKIP (mkfs.ext4 missing)"; fi

echo "- size not a multiple of 1 MiB"; attach 3000320
run "YES" 0 "$LOOP" --zero; run "" 0 "$LOOP" --verify
[ "$(nonzero "$LOOP")" -eq 0 ] || fail "end of disk not erased"

echo "- simulated bad sectors (dm-error)"
if command -v dmsetup >/dev/null 2>&1; then
    attach 16M
    S=$(blockdev --getsz "$LOOP")
    DM="diskmedic-test-$$"
    if printf '0 4096 linear %s 0\n4096 8 error\n4104 %s linear %s 4104\n' "$LOOP" $((S - 4104)) "$LOOP" \
        | dmsetup create "$DM" 2>/dev/null; then
        run "" 1 "/dev/mapper/$DM" --verify --log "$T/bad.csv"
        grep -q 'LBA 4096 ' "$T/err" || fail "LBA 4096 not reported"
        grep -q '8 unreadable sector(s)' "$T/err" || fail "wrong number of unreadable sectors"
        [ "$(grep -c DEFECTIVE "$T/bad.csv")" -eq 8 ] || fail "CSV log: 8 entries expected"
        run "" 1 "/dev/mapper/$DM" --scan-slow
        grep -q 'unreadable sectors: 8' "$T/out" || fail "scan-slow: unreadable sectors not counted"
        dmsetup remove --retry "$DM"; DM=""
    else echo "  SKIP (device-mapper unavailable)"; DM=""; fi
else echo "  SKIP (dmsetup missing)"; fi

echo "- simulated slow sectors (dm-delay): location and targeted rewrite"
if command -v dmsetup >/dev/null 2>&1; then
    attach 64M
    S=$(blockdev --getsz "$LOOP")
    DM="diskmedic-delay-$$"
    # 8 KiB slow (+30 ms) at LBA 21120 (block 10 + 320 KiB) and block 30 entirely slow.
    if printf '0 21120 linear %s 0\n21120 16 delay %s 21120 30\n21136 %s linear %s 21136\n61440 2048 delay %s 61440 30\n63488 %s linear %s 63488\n' \
        "$LOOP" "$LOOP" $((61440 - 21136)) "$LOOP" "$LOOP" $((S - 63488)) "$LOOP" | dmsetup create "$DM" 2>/dev/null; then
        D="/dev/mapper/$DM"
        set_rotational "$D" 1
        run "" 0 "$D" --scan-slow
        grep -q 'confirmed slow: 2' "$T/out" || fail "2 confirmed slow blocks expected"
        grep -q 'zone LBA 21120–21247' "$T/out" || fail "slow zone of block 10 located incorrectly"
        nz=$(grep -c '    zone LBA' "$T/out" || true)
        [ "$nz" -eq 17 ] || { grep '    zone LBA' "$T/out" >&2; fail "17 zones expected (1 + whole block 30), $nz found"; }
        before=$(md5sum <"$D")
        run "YES" 0 "$D" --slow-repair --log "$T/d.csv"
        [ "$before" = "$(md5sum <"$D")" ] || fail "slow-repair modified the data"
        [ "$(grep -c 'REWRITE VERIFIED, STILL SLOW' "$T/d.csv")" -eq 17 ] || fail "deferred check: 17 zones still slow expected"
        dmsetup remove --retry "$DM"; DM=""
    else echo "  SKIP (dm-delay target unavailable)"; DM=""; fi
else echo "  SKIP (dmsetup missing)"; fi

echo "- unreadable sectors (dm-flakey error_reads): only these sectors are rewritten"
if command -v dmsetup >/dev/null 2>&1; then
    attach 32M
    S=$(blockdev --getsz "$LOOP")
    cp "$T/disk.img" "$T/orig.img"
    DM="diskmedic-flakey-$$"
    if printf '0 10248 linear %s 0\n10248 8 flakey %s 10248 0 3600 1 error_reads\n10256 %s linear %s 10256\n' \
        "$LOOP" "$LOOP" $((S - 10256)) "$LOOP" | dmsetup create "$DM" 2>/dev/null; then
        set_rotational "/dev/mapper/$DM" 1
        # Reads keep failing after the write: verification fails (exit 1), as expected.
        run "YES" 1 "/dev/mapper/$DM" --slow-repair
        grep -q 'unreadable: 1 (unreadable sectors: 8)' "$T/out" || fail "8 unreadable sectors expected"
        dmsetup remove --retry "$DM"; DM=""; sync
        changed=$(cmp -l "$T/orig.img" "$T/disk.img" | awk '{print int(($1-1)/512)}' | sort -un | tr '\n' ' ')
        [ "$changed" = "10248 10249 10250 10251 10252 10253 10254 10255 " ] || fail "unexpected modified sectors: $changed"
        [ "$(dd if="$T/disk.img" bs=512 skip=10248 count=8 status=none | tr -d '\000' | wc -c)" -eq 0 ] || fail "unreadable sectors not reset to zero"
    else echo "  SKIP (dm-flakey target unavailable)"; DM=""; fi
    rm -f "$T/orig.img"
else echo "  SKIP (dmsetup missing)"; fi
printf 'OK (loop)\n'
