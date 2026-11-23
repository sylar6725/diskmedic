#!/bin/sh
# Tests fonctionnels sur un disque virtuel (loop device adossé à un fichier).
# Ne touche jamais aux disques réels. Nécessite root et losetup; sinon : SKIP.
set -eu
if [ "$(id -u)" -ne 0 ] || ! command -v losetup >/dev/null 2>&1; then
    echo "SKIP : tests loop device (root + losetup requis)"; exit 0
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

fail() { echo "ÉCHEC : $*" >&2; cat "$T/err" >&2 2>/dev/null || true; exit 1; }
run() { # run <stdin> <exit attendu> <args…>
    input=$1; wanted=$2; shift 2
    set +e; printf '%s\n' "$input" | "$BIN" "$@" >"$T/out" 2>"$T/err"; got=$?; set -e
    [ "$got" -eq "$wanted" ] || fail "exit $got au lieu de $wanted pour : $*"
}
# Ces tests couvrent le chemin « disque dur » : le loop device est déclaré rotatif. Sinon, sur une
# machine dont le fichier image est sur SSD, le noyau le dit non rotatif et DiskMedic le traite
# (à juste titre) comme un SSD : écriture zéro et réparation lente seraient refusées.
set_rotational() { # set_rotational <périphérique> <0|1>
    b=$(basename "$(readlink -f "$1")")
    echo "$2" >"/sys/block/$b/queue/rotational" 2>/dev/null || fail "queue/rotational non modifiable pour $b"
}
attach() { # attach <taille>
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
grep -q 'O_DIRECT' "$T/err" || fail "verify sans O_DIRECT"
echo "- scan-slow"; run "" 0 "$LOOP" --scan-slow --log "$T/s.csv"
echo "- smart sur périphérique non ATA"; run "" 1 "$LOOP" --smart
echo "- zero refusé sans OUI"; before=$(md5sum <"$LOOP"); run "non" 2 "$LOOP" --zero
[ "$before" = "$(md5sum <"$LOOP")" ] || fail "données modifiées sans confirmation"
echo "- slow-repair sur disque sain : données intactes, rien de confirmé"
run "OUI" 0 "$LOOP" --slow-repair --log "$T/r.csv"
[ "$before" = "$(md5sum <"$LOOP")" ] || fail "slow-repair a modifié les données"
grep -q 'lents confirmés : 0' "$T/out" || fail "faux positif confirmé sur disque sain"
! grep -q 'REÉCRITURE' "$T/r.csv" || fail "réécriture sur disque sain"
echo "- zero"; run "OUI" 0 "$LOOP" --zero
[ "$(nonzero "$LOOP")" -eq 0 ] || fail "octets non nuls après --zero"
echo "- non rotatif selon le noyau : zero refusé (SSD probable)"
set_rotational "$LOOP" 0
run "OUI" 1 "$LOOP" --zero
grep -q 'support non rotatif selon le noyau' "$T/err" || fail "refus SSD (noyau) absent"
set_rotational "$LOOP" 1

echo "- disque monté refusé"
if command -v mkfs.ext4 >/dev/null 2>&1; then
    mkfs.ext4 -q -F "$LOOP"; mkdir -p "$T/mnt"; mount "$LOOP" "$T/mnt"
    run "OUI" 1 "$LOOP" --zero; grep -q 'cours d’utilisation' "$T/err" || fail "message disque utilisé absent"
    run "OUI" 1 "$LOOP" --slow-repair
    [ "$(nonzero "$LOOP")" -gt 0 ] || fail "disque monté effacé"
    umount "$T/mnt"
else echo "  SKIP (mkfs.ext4 absent)"; fi

echo "- taille non multiple de 1 Mio"; attach 3000320
run "OUI" 0 "$LOOP" --zero; run "" 0 "$LOOP" --verify
[ "$(nonzero "$LOOP")" -eq 0 ] || fail "fin de disque non effacée"

echo "- secteurs défectueux simulés (dm-error)"
if command -v dmsetup >/dev/null 2>&1; then
    attach 16M
    S=$(blockdev --getsz "$LOOP")
    DM="diskmedic-test-$$"
    if printf '0 4096 linear %s 0\n4096 8 error\n4104 %s linear %s 4104\n' "$LOOP" $((S - 4104)) "$LOOP" \
        | dmsetup create "$DM" 2>/dev/null; then
        run "" 1 "/dev/mapper/$DM" --verify --log "$T/bad.csv"
        grep -q 'LBA 4096 ' "$T/err" || fail "LBA 4096 non signalé"
        grep -q '8 secteur(s) illisible(s)' "$T/err" || fail "nombre de secteurs illisibles incorrect"
        [ "$(grep -c DEFECTUEUX "$T/bad.csv")" -eq 8 ] || fail "journal CSV : 8 entrées attendues"
        run "" 1 "/dev/mapper/$DM" --scan-slow
        grep -q 'secteurs illisibles : 8' "$T/out" || fail "scan-slow : secteurs illisibles non comptés"
        dmsetup remove --retry "$DM"; DM=""
    else echo "  SKIP (device-mapper indisponible)"; DM=""; fi
else echo "  SKIP (dmsetup absent)"; fi

echo "- secteurs lents simulés (dm-delay) : localisation et réécriture ciblée"
if command -v dmsetup >/dev/null 2>&1; then
    attach 64M
    S=$(blockdev --getsz "$LOOP")
    DM="diskmedic-delay-$$"
    # 8 Kio lents (+30 ms) à LBA 21120 (bloc 10 + 320 Kio) et bloc 30 entièrement lent.
    if printf '0 21120 linear %s 0\n21120 16 delay %s 21120 30\n21136 %s linear %s 21136\n61440 2048 delay %s 61440 30\n63488 %s linear %s 63488\n' \
        "$LOOP" "$LOOP" $((61440 - 21136)) "$LOOP" "$LOOP" $((S - 63488)) "$LOOP" | dmsetup create "$DM" 2>/dev/null; then
        D="/dev/mapper/$DM"
        set_rotational "$D" 1
        run "" 0 "$D" --scan-slow
        grep -q 'lents confirmés : 2' "$T/out" || fail "2 blocs lents confirmés attendus"
        grep -q 'zone LBA 21120–21247' "$T/out" || fail "zone lente du bloc 10 mal localisée"
        nz=$(grep -c '    zone LBA' "$T/out" || true)
        [ "$nz" -eq 17 ] || { grep '    zone LBA' "$T/out" >&2; fail "17 zones attendues (1 + bloc 30 entier), $nz trouvée(s)"; }
        before=$(md5sum <"$D")
        run "OUI" 0 "$D" --slow-repair --log "$T/d.csv"
        [ "$before" = "$(md5sum <"$D")" ] || fail "slow-repair a modifié les données"
        [ "$(grep -c 'REÉCRITURE VÉRIFIÉE, TOUJOURS LENT' "$T/d.csv")" -eq 17 ] || fail "contrôle différé : 17 zones toujours lentes attendues"
        dmsetup remove --retry "$DM"; DM=""
    else echo "  SKIP (cible dm-delay indisponible)"; DM=""; fi
else echo "  SKIP (dmsetup absent)"; fi

echo "- secteurs illisibles (dm-flakey error_reads) : seuls ces secteurs sont réécrits"
if command -v dmsetup >/dev/null 2>&1; then
    attach 32M
    S=$(blockdev --getsz "$LOOP")
    cp "$T/disk.img" "$T/orig.img"
    DM="diskmedic-flakey-$$"
    if printf '0 10248 linear %s 0\n10248 8 flakey %s 10248 0 3600 1 error_reads\n10256 %s linear %s 10256\n' \
        "$LOOP" "$LOOP" $((S - 10256)) "$LOOP" | dmsetup create "$DM" 2>/dev/null; then
        set_rotational "/dev/mapper/$DM" 1
        # Les lectures restent en erreur après écriture : la vérification échoue (exit 1), c'est attendu.
        run "OUI" 1 "/dev/mapper/$DM" --slow-repair
        grep -q 'illisibles : 1 (secteurs illisibles : 8)' "$T/out" || fail "8 secteurs illisibles attendus"
        dmsetup remove --retry "$DM"; DM=""; sync
        changed=$(cmp -l "$T/orig.img" "$T/disk.img" | awk '{print int(($1-1)/512)}' | sort -un | tr '\n' ' ')
        [ "$changed" = "10248 10249 10250 10251 10252 10253 10254 10255 " ] || fail "secteurs modifiés inattendus : $changed"
        [ "$(dd if="$T/disk.img" bs=512 skip=10248 count=8 status=none | tr -d '\000' | wc -c)" -eq 0 ] || fail "secteurs illisibles non remis à zéro"
    else echo "  SKIP (cible dm-flakey indisponible)"; DM=""; fi
    rm -f "$T/orig.img"
else echo "  SKIP (dmsetup absent)"; fi
printf 'OK (loop)\n'
