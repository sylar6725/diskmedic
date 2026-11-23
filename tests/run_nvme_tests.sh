#!/bin/sh
# NVMe tests on virtual devices only (never on a real disk):
#  1. nvme-loop: the kernel's NVMe target (nvmet), stored in a file. It offers neither Format NVM
#     nor Sanitize: Secure Erase must be refused cleanly; TRIM and SMART work.
#  2. QEMU: virtual machine with three emulated NVMe controllers (512 B sectors, 4 KiB sectors,
#     and a controller with two namespaces). Format NVM is actually run there.
# Needs root. Part 2: qemu-system-x86_64, static busybox, cpio, gzip, static libc and
# a kernel with the NVMe driver built in (DISKMEDIC_TEST_KERNEL, WSL kernel or /boot/vmlinuz-*).
# Each part whose requirements are missing is skipped (SKIP).
set -eu
if [ "$(id -u)" -ne 0 ]; then echo "SKIP: NVMe tests (root required)"; exit 0; fi
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
BIN="$T/diskmedic"
PART="compilation"
LOOPDEV="" NQN="" CTRL=""
cc -O2 -Wall -Wextra -Werror -std=c99 "$ROOT/diskmedic.c" -o "$BIN"

teardown_loop() {
    C=/sys/kernel/config/nvmet
    [ -n "$CTRL" ] && [ -e "/sys/class/nvme/$CTRL/delete_controller" ] && echo 1 >"/sys/class/nvme/$CTRL/delete_controller" 2>/dev/null || true
    if [ -n "$NQN" ]; then
        rm -f "$C/ports/9/subsystems/$NQN" 2>/dev/null || true
        rmdir "$C/ports/9" 2>/dev/null || true
        [ -e "$C/subsystems/$NQN/namespaces/1/enable" ] && echo 0 >"$C/subsystems/$NQN/namespaces/1/enable" 2>/dev/null || true
        rmdir "$C/subsystems/$NQN/namespaces/1" "$C/subsystems/$NQN" 2>/dev/null || true
    fi
    [ -n "$LOOPDEV" ] && losetup -d "$LOOPDEV" 2>/dev/null || true
    CTRL="" NQN="" LOOPDEV=""
}
cleanup() { teardown_loop; rm -rf "$T"; }
trap cleanup EXIT HUP INT TERM

fail() {
    echo "FAILED [$PART]: $*" >&2; cat "$T/err" >&2 2>/dev/null || true
    echo "--- last kernel messages:" >&2; dmesg 2>/dev/null | tail -25 >&2 || true
    exit 1
}
run() { # run <stdin> <expected exit> <args…>
    input=$1; wanted=$2; shift 2
    set +e; printf '%s\n' "$input" | "$BIN" "$@" >"$T/out" 2>"$T/err"; got=$?; set -e
    [ "$got" -eq "$wanted" ] || fail "exit $got instead of $wanted for: $*"
}
nonzero() { tr -d '\000' <"$1" | wc -c | tr -d ' '; }

# ---------- 1. nvme-loop ----------
PART="nvme-loop"
if modprobe nvmet 2>/dev/null && modprobe nvme-loop 2>/dev/null && [ -d /sys/kernel/config/nvmet ]; then
    echo "== $PART (NVMe without Format or Sanitize)"
    C=/sys/kernel/config/nvmet
    truncate -s 64M "$T/loop.img"
    NQN="diskmedic-test-$$"
    mkdir "$C/subsystems/$NQN"
    echo 1 >"$C/subsystems/$NQN/attr_allow_any_host"
    mkdir "$C/subsystems/$NQN/namespaces/1"
    # nvmet file backend (no loop device: on some kernels, the nvmet target returns
    # "Internal Error" on every write when it goes through a loop device).
    printf '%s' "$T/loop.img" >"$C/subsystems/$NQN/namespaces/1/device_path"
    echo 1 >"$C/subsystems/$NQN/namespaces/1/enable"
    mkdir "$C/ports/9"
    echo loop >"$C/ports/9/addr_trtype"
    ln -s "$C/subsystems/$NQN" "$C/ports/9/subsystems/$NQN"
    echo "transport=loop,nqn=$NQN" >/dev/nvme-fabrics
    DEV=""
    for i in 1 2 3 4 5 6 7 8 9 10; do
        for c in /sys/class/nvme/nvme*; do
            [ "$(cat "$c/subsysnqn" 2>/dev/null)" = "$NQN" ] && CTRL=${c##*/}
        done
        if [ -n "$CTRL" ]; then
            for b in /sys/block/nvme*n1; do
                case ${b##*/} in *c*) continue ;; esac
                [ "$(cat "$b/device/subsysnqn" 2>/dev/null)" = "$NQN" ] && DEV="/dev/${b##*/}"
            done
        fi
        [ -n "$DEV" ] && [ -b "$DEV" ] && break
        sleep 0.5
    done
    [ -n "$DEV" ] || fail "nvme-loop namespace not found"
    echo "  - smart ($DEV)"
    run "" 0 "$DEV" --smart
    grep -q 'Type      : NVMe SSD' "$T/out" || fail "NVMe identity missing"
    dd if=/dev/urandom of="$DEV" bs=1M count=64 oflag=direct status=none 2>"$T/err" || fail "cannot write to $DEV"
    before=$(md5sum <"$DEV")
    echo "  - zero refused: NVMe SSD recognized by the firmware (kernel rotational flag: $(cat "/sys/block/${DEV#/dev/}/queue/rotational"))"
    run "YES" 1 "$DEV" --zero
    grep -q 'Zero-fill not recommended on this disk (NVMe SSD)' "$T/err" || fail "SSD refusal of --zero missing"
    echo "  - secure-erase refused before confirmation (neither Format nor Sanitize)"
    run "YES" 1 "$DEV" --secure-erase
    grep -q 'offers neither Sanitize' "$T/err" || fail "Secure Erase refusal missing"
    grep -q 'Type exactly YES' "$T/err" && fail "confirmation asked although nothing is possible"
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "a refusal modified the data"
    echo "  - trim"
    run "YES" 0 "$DEV" --trim
    grep -q 'everything is zero' "$T/err" || fail "zero read-back after TRIM not reported"
    [ "$(nonzero "$DEV")" -eq 0 ] || fail "non-zero bytes after --trim"
    teardown_loop
else
    echo "SKIP: $PART (nvmet / nvme-loop modules missing)"
fi

# ---------- 2. QEMU ----------
PART="QEMU"
KERNEL=${DISKMEDIC_TEST_KERNEL:-}
if [ -z "$KERNEL" ]; then
    for k in "/mnt/c/Program Files/WSL/tools/kernel" "/boot/vmlinuz-$(uname -r)"; do
        [ -r "$k" ] && { KERNEL=$k; break; }
    done
fi
BB=$(command -v busybox 2>/dev/null || true)
missing=""
command -v qemu-system-x86_64 >/dev/null 2>&1 || missing="$missing qemu-system-x86_64"
command -v cpio >/dev/null 2>&1 || missing="$missing cpio"
command -v gzip >/dev/null 2>&1 || missing="$missing gzip"
if [ -z "$BB" ] || ldd "$BB" >/dev/null 2>&1; then missing="$missing static-busybox"; fi
[ -n "$KERNEL" ] || missing="$missing kernel"
if [ -n "$missing" ]; then echo "SKIP: $PART (missing:$missing)"; echo "OK (NVMe)"; exit 0; fi
R="$T/vm/root"
mkdir -p "$R/bin" "$R/proc" "$R/sys" "$R/dev" "$R/tmp"
cp "$BB" "$R/bin/busybox"
cc -O2 -Wall -Wextra -Werror -std=c99 -static "$ROOT/diskmedic.c" -o "$R/bin/diskmedic" 2>"$T/err" ||
    { echo "SKIP: $PART (static build failed: static libc missing?)"; echo "OK (NVMe)"; exit 0; }
cat >"$R/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
n=0
while [ "$(ls /sys/block | grep -c '^nvme[0-9]*n[0-9]*$')" -lt 4 ] && [ "$n" -lt 40 ]; do sleep 0.25; n=$((n + 1)); done
if ! ls /sys/block | grep -q '^nvme'; then echo "=== NO NVME"; poweroff -f; fi
if sh /test.sh; then echo "=== RESULT PASS"; else echo "=== RESULT FAIL"; fi
poweroff -f
EOF
cat >"$R/test.sh" <<'EOF'
FAILS=0
ok() { echo "OK: $*"; }
ko() { echo "FAIL: $*"; sed 's/^/    /' /tmp/err; FAILS=$((FAILS + 1)); }
run() { # run <stdin> <expected exit> <args…>; 0 if the exit code is the expected one
    input=$1; wanted=$2; shift 2
    printf '%s\n' "$input" | diskmedic "$@" >/tmp/out 2>/tmp/err; got=$?
    [ "$got" -eq "$wanted" ] || { echo "FAIL: exit $got instead of $wanted for: $*"; sed 's/^/    /' /tmp/err; FAILS=$((FAILS + 1)); return 1; }
}
dev_of() { # dev_of <serial> <nsid>
    for b in /sys/block/nvme*; do
        n=${b##*/}; case $n in *c*) continue ;; esac
        [ "$(tr -d ' ' <"$b/device/serial")" = "$1" ] && [ "$(cat "$b/nsid")" = "$2" ] && { echo "/dev/$n"; return 0; }
    done
    return 1
}
fill() { dd if=/dev/urandom of="$1" bs=1M count=64 conv=fsync 2>/dev/null; }
fresh() { echo 3 >/proc/sys/vm/drop_caches; }
nonzero() { fresh; tr -d '\000' <"$1" | wc -c; }
sum() { fresh; md5sum <"$1"; }
A=$(dev_of dm512 1) && B=$(dev_of dm4k 1) && C1=$(dev_of dm2ns 1) && C2=$(dev_of dm2ns 2) || { echo "FAIL: emulated NVMe devices not found"; ls /sys/block; exit 1; }

echo "-- $A: QEMU controller, 512 B sectors"
run "" 0 "$A" --smart && grep -q 'QEMU NVMe Ctrl' /tmp/out && ok "smart" || ko "smart: QEMU identity missing"
fill "$A"; before=$(sum "$A")
run "YES" 1 "$A" --zero && grep -q 'on this disk (NVMe SSD)' /tmp/err && ok "zero refused (SSD)" || ko "zero not refused"
run "YES" 1 "$A" --slow-repair && grep -q 'Slow-block repair not recommended' /tmp/err && ok "slow-repair refused (SSD)" || ko "slow-repair not refused"
run "no" 2 "$A" --secure-erase && grep -q 'NVMe Secure Erase: Format NVM, user data erase' /tmp/err &&
    ok "secure-erase: method announced before confirmation" || ko "method not announced"
[ "$before" = "$(sum "$A")" ] && ok "refusals and cancellation without any change" || ko "data modified by a refusal"
run "YES" 0 "$A" --zero --force && [ "$(nonzero "$A")" -eq 0 ] && ok "zero --force" || ko "zero --force"
fill "$A"
run "YES" 0 "$A" --trim && grep -q 'everything is zero' /tmp/err && [ "$(nonzero "$A")" -eq 0 ] && ok "trim" || ko "trim"
fill "$A"
run "YES" 0 "$A" --secure-erase && grep -q 'Method: Format NVM, user data erase' /tmp/err &&
    grep -q 'everything is zero' /tmp/err && [ "$(nonzero "$A")" -eq 0 ] && ok "secure-erase (Format NVM, SES=1)" || ko "secure-erase"

echo "-- $B: 4 KiB sectors"
ls0=$(cat /sys/block/${B#/dev/}/queue/logical_block_size); sz0=$(cat /sys/block/${B#/dev/}/size)
fill "$B"
run "YES" 0 "$B" --secure-erase && [ "$(nonzero "$B")" -eq 0 ] && ok "secure-erase 4Kn" || ko "secure-erase 4Kn"
[ "$ls0" = 4096 ] && [ "$(cat /sys/block/${B#/dev/}/queue/logical_block_size)" = 4096 ] &&
    [ "$(cat /sys/block/${B#/dev/}/size)" = "$sz0" ] && ok "4Kn format and size kept" || ko "sector format changed ($ls0)"

echo "-- $C1 / $C2: one controller, two namespaces"
fill "$C1"; fill "$C2"; other=$(sum "$C2")
run "YES" 0 "$C1" --secure-erase && [ "$(nonzero "$C1")" -eq 0 ] && ok "namespace 1 erased" || ko "namespace 1 not erased"
[ "$other" = "$(sum "$C2")" ] && ok "namespace 2 intact" || ko "namespace 2 modified"
[ "$FAILS" -eq 0 ]
EOF
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc 2>/dev/null | gzip) >"$T/vm/initrd.gz"
for d in a b c1 c2; do truncate -s 64M "$T/vm/$d.img"; done
ACCEL="-accel tcg"
[ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
echo "== $PART (3 emulated NVMe controllers, kernel: $KERNEL, ${ACCEL%% *})"
set +e
# shellcheck disable=SC2086
timeout 900 qemu-system-x86_64 $ACCEL -m 512 -smp 2 -nographic -no-reboot \
    -kernel "$KERNEL" -initrd "$T/vm/initrd.gz" -append "console=ttyS0 rdinit=/init quiet panic=-1" \
    -drive "file=$T/vm/a.img,if=none,id=a,format=raw,discard=unmap" -device nvme,serial=dm512,drive=a \
    -drive "file=$T/vm/b.img,if=none,id=b,format=raw" -device nvme,id=nv2,serial=dm4k \
    -device nvme-ns,drive=b,bus=nv2,logical_block_size=4096,physical_block_size=4096 \
    -drive "file=$T/vm/c1.img,if=none,id=c1,format=raw" -drive "file=$T/vm/c2.img,if=none,id=c2,format=raw" \
    -device nvme,id=nv3,serial=dm2ns -device nvme-ns,drive=c1,bus=nv3,nsid=1 -device nvme-ns,drive=c2,bus=nv3,nsid=2 \
    </dev/null >"$T/vm.log" 2>&1
set -e
tr -d '\r' <"$T/vm.log" | sed -n '/^-- /p;/^OK: /p;/^FAIL: /,/^[^ ]/p' | sed 's/^OK: /  - /;s/^-- /  /'
if grep -q '=== NO NVME' "$T/vm.log"; then echo "SKIP: $PART (the guest kernel does not have the NVMe driver built in)"
elif ! grep -q '=== RESULT PASS' "$T/vm.log"; then
    : >"$T/err"; tail -40 "$T/vm.log" >&2; fail "tests in the virtual machine failed"
fi
echo "OK (NVMe)"
