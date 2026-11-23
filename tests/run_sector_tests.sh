#!/bin/sh
# Tests par format de secteur : 512n (512/512), 512e (512 logique / 4096 physique) et
# 4Kn (4096/4096), sur des disques SCSI virtuels en mémoire (module noyau scsi_debug).
# Ne touche jamais aux disques réels. Nécessite root et scsi_debug; sinon : SKIP.
set -eu
if [ "$(id -u)" -ne 0 ] || ! modprobe -n scsi_debug 2>/dev/null; then
    echo "SKIP : tests formats de secteur (root + module scsi_debug requis)"; exit 0
fi
if grep -q '^scsi_debug ' /proc/modules; then
    echo "SKIP : scsi_debug est déjà chargé par autre chose"; exit 0
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
        i=$((i + 1)); [ "$i" -ge 20 ] && { echo "impossible de décharger scsi_debug" >&2; return 1; }
        sleep 0.5
    done
    LOADED=0
}
cleanup() { unload || true; rm -rf "$T"; }
trap cleanup EXIT HUP INT TERM

fail() { echo "ÉCHEC [$FMT] : $*" >&2; cat "$T/err" >&2 2>/dev/null || true; exit 1; }
run() { # run <stdin> <exit attendu> <args…>
    input=$1; wanted=$2; shift 2
    set +e; printf '%s\n' "$input" | "$BIN" "$@" >"$T/out" 2>"$T/err"; got=$?; set -e
    [ "$got" -eq "$wanted" ] || fail "exit $got au lieu de $wanted pour : $*"
}
load() { # load <taille secteur logique> <exposant physique> [<LBA en erreur> [<nombre>]]
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
    [ -n "$DEV" ] || fail "disque scsi_debug introuvable"
    LS=$(cat "/sys/block/${DEV#/dev/}/queue/logical_block_size")
    PS=$(cat "/sys/block/${DEV#/dev/}/queue/physical_block_size")
    [ "$LS" -eq "$1" ] || fail "secteur logique $LS au lieu de $1"
    [ "$PS" -eq $(( $1 << $2 )) ] || fail "secteur physique $PS au lieu de $(( $1 << $2 ))"
}
# Empreinte de tous les secteurs logiques sauf [<premier>, +<nombre>) (les secteurs illisibles).
md5_except() { # md5_except <premier LBA exclu> [<nombre>]
    total=$(( $(blockdev --getsize64 "$DEV") / LS )); cnt=${2:-1}
    # Lectures directes par blocs de 1 Mio, bornées en octets pour ne jamais toucher les secteurs exclus.
    { dd if="$DEV" bs=1M iflag=direct,count_bytes count=$(( $1 * LS )) status=none
      dd if="$DEV" bs=1M iflag=direct,skip_bytes,count_bytes skip=$(( ($1 + cnt) * LS )) \
         count=$(( (total - $1 - cnt) * LS )) status=none; } | md5sum
}
# Nombre de lectures en échec signalées par le noyau pour $DEV depuis la marque posée par kmsg_mark.
kmsg_mark() { MARK="diskmedic-mark-$$-$1"; echo "$MARK" >/dev/kmsg; }
kmsg_read_errors() {
    dmesg | sed -n "/$MARK/,\$p" | grep -c "critical medium error, dev ${DEV#/dev/}, sector .* op 0x0:(READ)" || true
}
nonzero() { tr -d '\000' <"$DEV" | wc -c | tr -d ' '; }
# Copie $T/rnd vers $DEV sur [premier secteur, +nombre), uniquement en O_DIRECT : une écriture
# tamponnée partielle provoquerait une lecture-modification-écriture de la page de 4 Kio,
# qui échoue si cette page contient le secteur illisible.
wr_range() { # wr_range <premier secteur> <nombre de secteurs>
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
    echo "== $FMT (logique $SS o, physique $(( SS << EXP )) o)"

    # Disque sain.
    load "$SS" "$EXP"
    dd if=/dev/urandom of="$DEV" bs=1M count=64 oflag=direct status=none
    echo "  - smart (SCSI : INQUIRY + pages LOG SENSE)"
    run "" 0 "$DEV" --smart
    grep -q "━━ Linux scsi_debug ━━" "$T/out" || fail "fiche SCSI absente"
    grep -q "Secteurs  : $FMT (" "$T/out" || fail "format $FMT non reconnu"
    grep -q 'Santé (avis du firmware) : OK' "$T/out" || fail "page 2Fh non décodée"
    grep -q 'Température  *[0-9][0-9]* °C (max. [0-9]* °C)' "$T/out" || fail "page 0Dh non décodée"
    echo "  - verify";      run "" 0 "$DEV" --verify
    echo "  - scan-slow";   run "" 0 "$DEV" --scan-slow
    grep -q 'Note : SSD SAS/SCSI' "$T/err" || fail "note SSD absente du scan lent"
    # scsi_debug se présente comme un SSD (VPD B1h) : zéros et réparation refusés sans --force.
    echo "  - SSD : zero et slow-repair refusés sans --force, données intactes"
    before=$(md5sum <"$DEV")
    run "OUI" 1 "$DEV" --zero;        grep -q 'Écriture zéro déconseillée' "$T/err" || fail "refus SSD de --zero absent"
    run "OUI" 1 "$DEV" --slow-repair; grep -q 'Réparation lente déconseillée' "$T/err" || fail "refus SSD de --slow-repair absent"
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "un refus a modifié les données"
    echo "  - trim sans UNMAP : refusé, données intactes"
    run "OUI" 1 "$DEV" --trim; grep -q 'ne prend pas en charge TRIM' "$T/err" || fail "refus TRIM absent"
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "TRIM refusé a modifié les données"
    echo "  - slow-repair --force : données intactes"
    run "OUI" 0 "$DEV" --slow-repair --force
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "slow-repair a modifié les données"
    echo "  - zero --force";  run "OUI" 0 "$DEV" --zero --force
    [ "$(nonzero)" -eq 0 ] || fail "octets non nuls après --zero"
    unload

    # TRIM : disque avec UNMAP (lbpu=1), lecture à zéro des blocs libérés (lbprz=1).
    SD_EXTRA="lbpu=1 lbprz=1"; load "$SS" "$EXP"; SD_EXTRA=""
    dd if=/dev/urandom of="$DEV" bs=1M count=64 oflag=direct status=none
    echo "  - trim : refusé sans OUI, puis disque entier libéré"
    before=$(md5sum <"$DEV"); run "non" 2 "$DEV" --trim
    [ "$before" = "$(md5sum <"$DEV")" ] || fail "trim non confirmé a modifié les données"
    run "OUI" 0 "$DEV" --trim
    grep -q 'tout est à zéro' "$T/err" || fail "relecture à zéro après TRIM non signalée"
    [ "$(nonzero)" -eq 0 ] || fail "octets non nuls après --trim"
    unload

    # Un secteur logique illisible au milieu d'un secteur physique du bloc 5.
    BAD=$(( 5 * 1048576 / SS + 3 ))
    load "$SS" "$EXP" "$BAD"
    dd if=/dev/urandom of="$T/rnd" bs=1M count=64 status=none
    wr_range 0 "$BAD"
    wr_range $(( BAD + 1 )) $(( 64 * 1048576 / SS - BAD - 1 ))
    rm -f "$T/rnd"
    echo "  - verify : LBA $BAD signalé"
    run "" 1 "$DEV" --verify --log "$T/v.csv"
    grep -q "LBA $BAD " "$T/err" || fail "LBA $BAD non signalé"
    grep -q '1 secteur(s) illisible(s)' "$T/err" || fail "1 secteur illisible attendu"
    grep -q "\"$BAD\",\"DEFECTUEUX" "$T/v.csv" || fail "LBA $BAD absent du journal"
    echo "  - slow-repair : un seul secteur physique réécrit, voisins conservés"
    before=$(md5_except "$BAD")
    run "OUI" 1 "$DEV" --slow-repair --force   # scsi_debug garde le secteur en erreur : vérification en échec attendue
    grep -q '(1 secteur(s) physique(s) réécrit(s)' "$T/out" || fail "un seul secteur physique réécrit attendu"
    [ "$before" = "$(md5_except "$BAD")" ] || fail "des secteurs lisibles ont été modifiés"
    unload
done

# Cas réaliste 512e : un secteur PHYSIQUE entier est perdu, donc ses 8 secteurs logiques.
FMT="512e physique perdu"; echo "== $FMT"
BAD=10240
load 512 3 "$BAD" 8
dd if=/dev/urandom of="$T/rnd" bs=1M count=64 status=none
wr_range 0 "$BAD"
wr_range $(( BAD + 8 )) $(( 64 * 2048 - BAD - 8 ))
rm -f "$T/rnd"
echo "  - verify : 8 LBA signalés avec au plus 4 lectures en échec (au lieu de 9)"
sleep 6   # laisse se vider la limite de débit des messages noyau
kmsg_mark verify
run "" 1 "$DEV" --verify --log "$T/p.csv"
grep -q '8 secteur(s) illisible(s)' "$T/err" || fail "8 secteurs illisibles attendus"
for l in 10240 10241 10242 10243 10244 10245 10246 10247; do
    grep -q "\"$l\",\"DEFECTUEUX" "$T/p.csv" || fail "LBA $l absent du journal"
done
nerr=$(kmsg_read_errors)
[ "$nerr" -le 4 ] || fail "$nerr lectures en échec (4 au plus attendues)"
echo "    ($nerr lectures en échec)"
echo "  - slow-repair : un seul secteur physique réécrit, reste intact"
before=$(md5_except "$BAD" 8)
run "OUI" 1 "$DEV" --slow-repair --force
grep -q '(1 secteur(s) physique(s) réécrit(s)' "$T/out" || fail "un seul secteur physique réécrit attendu"
grep -q 'secteurs illisibles : 8' "$T/out" || fail "8 secteurs illisibles attendus au scan"
[ "$before" = "$(md5_except "$BAD" 8)" ] || fail "des secteurs lisibles ont été modifiés"
unload

# Sélection multi-disques : deux disques vérifiés en parallèle (pseudo-terminal fourni par script).
# SÉCURITÉ : seuls les disques scsi_debug doivent être sélectionnés. Le test ne se fie pas à la
# position des disques dans la liste (l'ordre du menu n'est pas l'ordre alphabétique : sdX avant
# nvmeXnY) : il lit la ligne sous le curseur et ne coche que celles qui affichent « scsi_debug ».
# Avant Entrée, l'écran doit indiquer exactement 2 disques sélectionnés; avant OUI, chaque disque
# visé doit être un scsi_debug. Sinon le menu est quitté sans rien lancer, et le test échoue.
# Chaque touche n'est envoyée qu'une fois l'écran attendu affiché (le démarrage peut être lent),
# et le pseudo-terminal est borné dans le temps : un blocage fait échouer le test sans le figer.
wait_screen() { # wait_screen <texte> <nombre de demi-secondes au plus>
    j=0; while ! grep -aq "$1" "$T/ts" 2>/dev/null && [ "$j" -lt "$2" ]; do sleep 0.5; j=$((j + 1)); done
}
cursor_frames() { grep -ac '> \[.\] /dev/' "$T/ts" 2>/dev/null || true; }
press() { # press <touche> : envoie une touche et attend que le menu soit redessiné
    n=$(cursor_frames); printf "$1"; j=0
    while [ "$(cursor_frames)" -le "$n" ] && [ "$j" -lt 40 ]; do sleep 0.25; j=$((j + 1)); done
}
abort_tui() { echo "$1" >"$T/abort"; printf 'q'; sleep 0.5; printf 'q'; sleep 1; }
tui_run() { # tui_run <fonction de saisie> : pilote « diskmedic --tui » dans un pseudo-terminal
    : >"$T/ts"; rm -f "$T/abort"
    "$1" | (cd "$T/par" && timeout -k 5 300 script -qfec "stty rows 30 cols 90; $BIN --tui" /dev/null) >"$T/ts" 2>&1 || true
    cp "$T/ts" "$T/err"
    if [ -e "$T/abort" ]; then fail "sélection interrompue par sécurité : $(cat "$T/abort")"; fi
}
select_disks() { # coche les seuls disques scsi_debug, valide, puis attend le choix de l'opération
    wait_screen 'SÉLECTION MULTIPLE' 60
    total=$(ls /sys/block | grep -cE '^(sd|hd|vd)[a-z]+$|^nvme[0-9]+n[0-9]+$')
    k=0
    while [ "$k" -lt "$total" ]; do
        case $(grep -a '> \[.\] /dev/' "$T/ts" | tail -1) in *scsi_debug*) press ' ' ;; esac
        press '\033[B'; k=$((k + 1))
    done
    case $(grep -a 'Sélectionnés : ' "$T/ts" | tail -1) in
        *'Sélectionnés : 2 /'*) ;;
        *) abort_tui "le menu n'indique pas exactement 2 disques sélectionnés"; return 1 ;;
    esac
    printf '\r'; wait_screen "CHOIX DE L" 60
}
keys_verify() { select_disks || return 0; printf '2'; wait_screen 'appuyez sur une touche' 400; printf 'x'; sleep 1; }
keys_zero_trim() {
    select_disks || return 0
    printf '5'; wait_screen 'TRIM sur ces SSD' 60
    if grep -a '^  /dev/.* (' "$T/ts" | grep -av scsi_debug | grep -q .; then
        printf '\r'; echo "un disque autre que scsi_debug est dans la sélection" >"$T/abort"; sleep 1; return 0
    fi
    printf 't\r'; wait_screen 'Tapez exactement OUI' 60
    # Liste des disques visés (« /dev/x — modèle → opération ») : tous doivent être des scsi_debug.
    if ! grep -a ' → ' "$T/ts" | grep -q . || grep -a ' → ' "$T/ts" | grep -av scsi_debug | grep -q .; then
        printf '\r'; echo "la liste des disques à effacer contient autre chose que scsi_debug" >"$T/abort"; sleep 1; return 0
    fi
    printf 'OUI\r'; wait_screen 'appuyez sur une touche' 400; printf 'x'; sleep 1
}
if command -v script >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    FMT="parallèle"; echo "== $FMT (2 disques, vérification simultanée via le menu)"
    modprobe scsi_debug num_tgts=2 dev_size_mb=64; LOADED=1
    command -v udevadm >/dev/null 2>&1 && udevadm settle 2>/dev/null || true
    sleep 1
    ndev=0
    for n in $(ls /sys/block | grep -E '^(sd|hd|vd)[a-z]+$|^nvme[0-9]+n[0-9]+$'); do
        grep -q scsi_debug "/sys/block/$n/device/model" 2>/dev/null && ndev=$((ndev + 1))
    done
    [ "$ndev" -eq 2 ] || fail "2 disques scsi_debug attendus, $ndev trouvé(s)"
    mkdir "$T/par"
    tui_run keys_verify
    grep -q 'Vérification · 2 disque(s) en parallèle' "$T/ts" || fail "tableau de bord parallèle absent"
    grep -q 'En cours : 2 ' "$T/ts" || fail "les 2 disques n'ont pas tourné simultanément"
    [ "$(grep -c '^  \[OK\] ' "$T/ts")" -eq 2 ] || fail "2 disques OK attendus dans le résumé"
    [ "$(ls "$T"/par/diskmedic-*/*.log | wc -l)" -eq 2 ] || fail "2 journaux attendus"
    [ "$(ls "$T"/par/diskmedic-*/*.csv | wc -l)" -eq 2 ] || fail "2 journaux CSV attendus"
    unload

    # Écriture zéro demandée sur 2 SSD : le menu propose TRIM à la place (t), puis OUI.
    FMT="parallèle SSD"; echo "== $FMT (écriture zéro demandée, TRIM choisi pour les SSD)"
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
    grep -q 'SSD dans la sélection' "$T/ts" || fail "SSD non signalés avant l'écriture zéro"
    grep -q 'TRIM · 2 disque(s) en parallèle' "$T/ts" || fail "tableau de bord TRIM absent"
    [ "$(grep -c '^  \[OK\] .*· TRIM' "$T/ts")" -eq 2 ] || fail "2 TRIM OK attendus dans le résumé"
    for d in $DEVS; do [ "$(tr -d '\000' <"$d" | wc -c)" -eq 0 ] || fail "$d non vidé par le TRIM"; done
    unload
else
    echo "SKIP : test multi-disques parallèle (commande script ou timeout absente)"
fi
printf 'OK (formats de secteur)\n'
