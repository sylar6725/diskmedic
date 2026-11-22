#!/bin/sh
# Tests NVMe sur des périphériques virtuels uniquement (jamais sur un disque réel) :
#  1. nvme-loop : cible NVMe du noyau (nvmet) sur un fichier. Elle ne propose ni Format NVM
#     ni Sanitize : Secure Erase doit être refusé proprement; TRIM et SMART fonctionnent.
#  2. QEMU : machine virtuelle avec trois contrôleurs NVMe émulés (secteurs de 512 o, de 4 Kio,
#     et un contrôleur à deux namespaces). Format NVM y est réellement exécuté.
# Nécessite root. Partie 2 : qemu-system-x86_64, busybox statique, cpio, gzip, libc statique et
# un noyau avec le pilote NVMe intégré (DISKMEDIC_TEST_KERNEL, noyau WSL ou /boot/vmlinuz-*).
# Chaque partie dont les prérequis manquent est ignorée (SKIP).
set -eu
if [ "$(id -u)" -ne 0 ]; then echo "SKIP : tests NVMe (root requis)"; exit 0; fi
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

fail() { echo "ÉCHEC [$PART] : $*" >&2; cat "$T/err" >&2 2>/dev/null || true; exit 1; }
run() { # run <stdin> <exit attendu> <args…>
    input=$1; wanted=$2; shift 2
    set +e; printf '%s\n' "$input" | "$BIN" "$@" >"$T/out" 2>"$T/err"; got=$?; set -e
    [ "$got" -eq "$wanted" ] || fail "exit $got au lieu de $wanted pour : $*"
}
nonzero() { tr -d '\000' <"$1" | wc -c | tr -d ' '; }

# ---------- 1. nvme-loop ----------
PART="nvme-loop"
if modprobe nvmet 2>/dev/null && modprobe nvme-loop 2>/dev/null && [ -d /sys/kernel/config/nvmet ]; then
    echo "== $PART (NVMe sans Format ni Sanitize)"
    C=/sys/kernel/config/nvmet
    truncate -s 64M "$T/loop.img"
    LOOPDEV=$(losetup -f --show "$T/loop.img")
    NQN="diskmedic-test-$$"
    mkdir "$C/subsystems/$NQN"
    echo 1 >"$C/subsystems/$NQN/attr_allow_any_host"
    mkdir "$C/subsystems/$NQN/namespaces/1"
    printf '%s' "$LOOPDEV" >"$C/subsystems/$NQN/namespaces/1/device_path"
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
    [ -n "$DEV" ] || fail "namespace nvme-loop introuvable"
    echo "  - smart ($DEV)"
    run "" 0 "$DEV" --smart
    grep -q 'Type      : SSD NVMe' "$T/out" || fail "fiche NVMe absente"
    dd if=/dev/urandom of="$DEV" bs=1M count=64 oflag=direct status=none
    before=$(md5sum <"$DEV")
    echo "  - zero refusé : SSD NVMe reconnu (le noyau le dit rotatif : $(cat "/sys/block/${DEV#/dev/}/queue/rotational"))"
    run "OUI" 1 "$DEV" --zero
    grep -q 'Écriture zéro déconseillée : ce disque est un SSD NVMe' "$T/err" || fail "refus SSD de --zero absent"
    echo "  - secure-erase refusé avant confirmation (ni Format ni Sanitize)"
    run "OUI" 1 "$DEV" --secure-erase
    grep -q 'ni Sanitize' "$T/err" || fail "refus Secure Erase absent"
    grep -q 'Tapez exactement OUI' "$T/err" && fail "confirmation demandée alors que rien n'est possible"
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "un refus a modifié les données"
    echo "  - trim"
    run "OUI" 0 "$DEV" --trim
    grep -q 'tout est à zéro' "$T/err" || fail "relecture à zéro après TRIM non signalée"
    [ "$(nonzero "$DEV")" -eq 0 ] || fail "octets non nuls après --trim"
    teardown_loop
else
    echo "SKIP : $PART (modules nvmet / nvme-loop absents)"
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
if [ -z "$BB" ] || ldd "$BB" >/dev/null 2>&1; then missing="$missing busybox-statique"; fi
[ -n "$KERNEL" ] || missing="$missing noyau"
if [ -n "$missing" ]; then echo "SKIP : $PART (absent :$missing)"; echo "OK (NVMe)"; exit 0; fi
R="$T/vm/root"
mkdir -p "$R/bin" "$R/proc" "$R/sys" "$R/dev" "$R/tmp"
cp "$BB" "$R/bin/busybox"
cc -O2 -Wall -Wextra -Werror -std=c99 -static "$ROOT/diskmedic.c" -o "$R/bin/diskmedic" 2>"$T/err" ||
    { echo "SKIP : $PART (compilation statique impossible : libc statique absente ?)"; echo "OK (NVMe)"; exit 0; }
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
run() { # run <stdin> <exit attendu> <args…> ; 0 si le code de sortie est celui attendu
    input=$1; wanted=$2; shift 2
    printf '%s\n' "$input" | diskmedic "$@" >/tmp/out 2>/tmp/err; got=$?
    [ "$got" -eq "$wanted" ] || { echo "FAIL: exit $got au lieu de $wanted pour : $*"; sed 's/^/    /' /tmp/err; FAILS=$((FAILS + 1)); return 1; }
}
dev_of() { # dev_of <série> <nsid>
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
A=$(dev_of dm512 1) && B=$(dev_of dm4k 1) && C1=$(dev_of dm2ns 1) && C2=$(dev_of dm2ns 2) || { echo "FAIL: NVMe émulés introuvables"; ls /sys/block; exit 1; }

echo "-- $A : contrôleur QEMU, secteurs de 512 o"
run "" 0 "$A" --smart && grep -q 'QEMU NVMe Ctrl' /tmp/out && ok "smart" || ko "smart : fiche QEMU absente"
fill "$A"; before=$(sum "$A")
run "OUI" 1 "$A" --zero && grep -q 'ce disque est un SSD NVMe' /tmp/err && ok "zero refusé (SSD)" || ko "zero non refusé"
run "OUI" 1 "$A" --slow-repair && grep -q 'Réparation lente déconseillée' /tmp/err && ok "slow-repair refusé (SSD)" || ko "slow-repair non refusé"
run "non" 2 "$A" --secure-erase && grep -q 'Secure Erase NVMe : Format NVM, effacement des données utilisateur' /tmp/err &&
    ok "secure-erase : méthode annoncée avant confirmation" || ko "méthode non annoncée"
[ "$before" = "$(sum "$A")" ] && ok "refus et annulation sans modification" || ko "données modifiées par un refus"
run "OUI" 0 "$A" --zero --force && [ "$(nonzero "$A")" -eq 0 ] && ok "zero --force" || ko "zero --force"
fill "$A"
run "OUI" 0 "$A" --trim && grep -q 'tout est à zéro' /tmp/err && [ "$(nonzero "$A")" -eq 0 ] && ok "trim" || ko "trim"
fill "$A"
run "OUI" 0 "$A" --secure-erase && grep -q 'Méthode : Format NVM, effacement des données utilisateur' /tmp/err &&
    grep -q 'tout est à zéro' /tmp/err && [ "$(nonzero "$A")" -eq 0 ] && ok "secure-erase (Format NVM, SES=1)" || ko "secure-erase"

echo "-- $B : secteurs de 4 Kio"
ls0=$(cat /sys/block/${B#/dev/}/queue/logical_block_size); sz0=$(cat /sys/block/${B#/dev/}/size)
fill "$B"
run "OUI" 0 "$B" --secure-erase && [ "$(nonzero "$B")" -eq 0 ] && ok "secure-erase 4Kn" || ko "secure-erase 4Kn"
[ "$ls0" = 4096 ] && [ "$(cat /sys/block/${B#/dev/}/queue/logical_block_size)" = 4096 ] &&
    [ "$(cat /sys/block/${B#/dev/}/size)" = "$sz0" ] && ok "format 4Kn et taille conservés" || ko "format de secteur modifié ($ls0)"

echo "-- $C1 / $C2 : un contrôleur, deux namespaces"
fill "$C1"; fill "$C2"; other=$(sum "$C2")
run "OUI" 0 "$C1" --secure-erase && [ "$(nonzero "$C1")" -eq 0 ] && ok "namespace 1 effacé" || ko "namespace 1 non effacé"
[ "$other" = "$(sum "$C2")" ] && ok "namespace 2 intact" || ko "namespace 2 modifié"
[ "$FAILS" -eq 0 ]
EOF
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc 2>/dev/null | gzip) >"$T/vm/initrd.gz"
for d in a b c1 c2; do truncate -s 64M "$T/vm/$d.img"; done
ACCEL="-accel tcg"
[ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
echo "== $PART (3 contrôleurs NVMe émulés, noyau : $KERNEL, ${ACCEL%% *})"
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
if grep -q '=== NO NVME' "$T/vm.log"; then echo "SKIP : $PART (le noyau invité n'a pas le pilote NVMe intégré)"
elif ! grep -q '=== RESULT PASS' "$T/vm.log"; then
    : >"$T/err"; tail -40 "$T/vm.log" >&2; fail "tests dans la machine virtuelle en échec"
fi
echo "OK (NVMe)"
