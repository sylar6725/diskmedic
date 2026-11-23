#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT HUP INT TERM
BIN="$TMPDIR_TEST/diskmedic"
cc -O2 -Wall -Wextra -Werror -std=c99 "$ROOT/diskmedic.c" -o "$BIN"
"$BIN" --version >"$TMPDIR_TEST/version.txt"
grep -q 'DiskMedic 1.12.0' "$TMPDIR_TEST/version.txt"
"$BIN" --help >"$TMPDIR_TEST/help.txt"
grep -q -- '--smart' "$TMPDIR_TEST/help.txt"
grep -q -- '--secure-erase' "$TMPDIR_TEST/help.txt"
grep -q -- '--trim' "$TMPDIR_TEST/help.txt"
grep -q -- '--force' "$TMPDIR_TEST/help.txt"
grep -q 'Space select.*Enter start.*q quit' "$TMPDIR_TEST/help.txt"

expect_status() {
    wanted=$1
    shift
    set +e
    "$BIN" "$@" </dev/null >"$TMPDIR_TEST/out.txt" 2>"$TMPDIR_TEST/err.txt"
    got=$?
    set -e
    if [ "$got" -ne "$wanted" ]; then
        echo "Expected exit $wanted, got $got for: $*" >&2
        cat "$TMPDIR_TEST/err.txt" >&2
        exit 1
    fi
}
expect_status 2
grep -q "interactive terminal" "$TMPDIR_TEST/err.txt"
expect_status 2 --tui
expect_status 2 /dev/null --tui --zero
expect_status 2 /dev/null --smart --log x.csv
expect_status 2 /dev/null --smart --zero
expect_status 1 /dev/null --verify
grep -q 'is not a block device' "$TMPDIR_TEST/err.txt"
expect_status 2 /dev/null --scan-slow --slow-factor 0.5
expect_status 2 --force
expect_status 2 /dev/null --force
expect_status 2 /dev/null --verify --force
expect_status 2 /dev/null --trim --zero
expect_status 2 --tui --trim
expect_status 1 /dev/null --trim
expect_status 1 /dev/null --zero --force

# SMART display on fabricated data (failing HDD, healthy SSD, healthy/failing NVMe, SAS).
cc -O2 -Wall -Wextra -Werror -std=c99 -Wno-unused-function "$ROOT/tests/smart_fixture_test.c" -o "$TMPDIR_TEST/smart"
"$TMPDIR_TEST/smart" >"$TMPDIR_TEST/smart.txt"
S="$TMPDIR_TEST/smart.txt"
section() { sed -n "/^=== $1\$/,/^=== /p" "$S"; }
expect_in() { # expect_in <section> <text>
    section "$1" | grep -qF -- "$2" || { echo "SMART [$1]: \"$2\" missing" >&2; section "$1" >&2; exit 1; }
}
expect_in HDD '━━ WDC WD40EFRX-68N32N0 ━━'
expect_in HDD 'Serial    : WD-WCC7K1234567'
expect_in HDD 'Firmware  : 82.00A82'
expect_in HDD '4.0 TB (3726.0 GiB, 4,000,787,030,016 bytes)'
expect_in HDD 'Hard disk 5400 rpm · SATA 6 Gb/s (current link 3 Gb/s)'
expect_in HDD 'Temperature 41 °C (min 20 / max 52) · Powered on 23,456 h (≈ 2 years 8 months)'
expect_in HDD '[CRITICAL] 3 pending sector(s)'
expect_in HDD '[WARNING] 12 CRC error(s)'
expect_in HDD '[WARNING] 8 reallocated sector(s)'
expect_in HDD '[WARNING] SATA link negotiated at 3 Gb/s'
expect_in HDD 'FAILED'
expect_in HDD 'VERDICT: CRITICAL'
expect_in SSD 'SSD · SATA 6 Gb/s (current link 6 Gb/s)'
expect_in SSD 'Written ≈ 48.2 TB* · Remaining life ≈ 92 %'
expect_in SSD 'Health (firmware assessment): unknown'
expect_in SSD 'None.'
expect_in SSD 'VERDICT: HEALTHY'
expect_in NVME 'Type      : NVMe SSD'
expect_in NVME '41 °C (warning 80 °C)'
expect_in NVME 'Written 48.2 TB · Remaining life ≈ 97 %'
expect_in NVME 'VERDICT: HEALTHY'
expect_in NVME_BAD '[CRITICAL] Degraded reliability'
expect_in NVME_BAD '[CRITICAL] 5 media/data integrity error(s)'
expect_in NVME_BAD 'VERDICT: CRITICAL'
expect_in SAS_BAD '━━ SEAGATE ST4000NM0023 ━━'
expect_in SAS_BAD 'Type      : SAS/SCSI hard disk 7200 rpm'
expect_in SAS_BAD 'Made      : 2016, week 12'
expect_in SAS_BAD 'Health (firmware assessment): FAILURE PREDICTED'
expect_in SAS_BAD 'Temperature 41 °C · Powered on 23,456 h (≈ 2 years 8 months) · 1,204 start/stop cycles · Written 48.2 TB'
expect_in SAS_BAD '[CRITICAL] The disk itself predicts an upcoming failure (ASC 5Dh/10h)'
expect_in SAS_BAD '[CRITICAL] 2 uncorrected read error(s)'
expect_in SAS_BAD '[WARNING] 12 grown defect(s)'
expect_in SAS_BAD 'Read: errors corr. / uncorr.          1,234 / 2'
expect_in SAS_BAD '1,204 / 50,000 (2 %)'
expect_in SAS_BAD 'VERDICT: CRITICAL'
expect_in SAS 'Health (firmware assessment): OK'
expect_in SAS 'VERDICT: HEALTHY'
expect_in SAS_NONE 'No readable health data'
expect_in SAS_NONE 'VERDICT: UNKNOWN'
expect_in STATUS 'hdd=2 ssd=0 nvme=0 nvme_bad=2 sas_bad=2 sas=0 sas_none=3'
expect_in STATUS 'exit: healthy=0 warning=3 critical=4 unknown=5 error=1'
expect_in SENSE 'desc valid=1 mid=4f high=c2'
expect_in SENSE 'fixed valid=1 mid=f4 high=2c'
expect_in ERASE 'crypto+block: 0 Sanitize, cryptographic erase'
expect_in ERASE 'block: 0 Sanitize, block erase'
expect_in ERASE 'format-crypto: 0 Format NVM, cryptographic erase'
expect_in ERASE 'format: 0 Format NVM, user data erase | The disk does not offer Sanitize'
expect_in ERASE 'multi-ns: 0 Format NVM, user data erase | Sanitize is not used'
expect_in ERASE 'multi-ns-format-global: -1 none | This NVMe device has several active namespaces'
expect_in ERASE 'ns-unknown: 0 Format NVM, user data erase'
expect_in ERASE 'overwrite-only: -1 none | This NVMe device offers neither Sanitize'
expect_in ERASE 'only Sanitize overwrite is offered'
expect_in ERASE 'nothing: -1 none'
if grep -q "$(printf '\033')" "$S"; then echo "SMART: color codes outside a terminal" >&2; exit 1; fi
if grep -E '^  ID|^  +[0-9]+ ' "$S" | LC_ALL=C.UTF-8 awk 'length($0) > 80 { bad = 1 } END { exit !bad }'; then
    echo "SMART: table wider than 80 columns" >&2; exit 1
fi
printf 'OK\n'
