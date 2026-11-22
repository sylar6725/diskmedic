#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT HUP INT TERM
BIN="$TMPDIR_TEST/diskmedic"
cc -O2 -Wall -Wextra -Werror -std=c99 "$ROOT/diskmedic.c" -o "$BIN"
"$BIN" --version >"$TMPDIR_TEST/version.txt"
grep -q 'DiskMedic 1.10.0' "$TMPDIR_TEST/version.txt"
"$BIN" --help >"$TMPDIR_TEST/help.txt"
grep -q -- '--smart' "$TMPDIR_TEST/help.txt"
grep -q -- '--secure-erase' "$TMPDIR_TEST/help.txt"
grep -q -- '--trim' "$TMPDIR_TEST/help.txt"
grep -q -- '--force' "$TMPDIR_TEST/help.txt"
grep -q 'Espace sélectionner.*Entrée lancer.*q quitter' "$TMPDIR_TEST/help.txt"

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
grep -q "terminal interactif" "$TMPDIR_TEST/err.txt"
expect_status 2 --tui
expect_status 2 /dev/null --tui --zero
expect_status 2 /dev/null --smart --log x.csv
expect_status 2 /dev/null --smart --zero
expect_status 1 /dev/null --verify
grep -q 'n’est pas un périphérique bloc\|n.est pas un périphérique bloc' "$TMPDIR_TEST/err.txt"
expect_status 2 /dev/null --scan-slow --slow-factor 0.5
expect_status 2 --force
expect_status 2 /dev/null --force
expect_status 2 /dev/null --verify --force
expect_status 2 /dev/null --trim --zero
expect_status 2 --tui --trim
expect_status 1 /dev/null --trim
expect_status 1 /dev/null --zero --force

# Affichage SMART sur données fabriquées (HDD en mauvais état, SSD sain, NVMe sain/défaillant).
cc -O2 -Wall -Wextra -Werror -std=c99 -Wno-unused-function "$ROOT/tests/smart_fixture_test.c" -o "$TMPDIR_TEST/smart"
"$TMPDIR_TEST/smart" >"$TMPDIR_TEST/smart.txt"
S="$TMPDIR_TEST/smart.txt"
section() { sed -n "/^=== $1\$/,/^=== /p" "$S"; }
expect_in() { # expect_in <section> <texte>
    section "$1" | grep -qF -- "$2" || { echo "SMART [$1] : « $2 » absent" >&2; section "$1" >&2; exit 1; }
}
expect_in HDD '━━ WDC WD40EFRX-68N32N0 ━━'
expect_in HDD 'Série     : WD-WCC7K1234567'
expect_in HDD 'Firmware  : 82.00A82'
expect_in HDD '4,0 To (3726,0 Gio, 4 000 787 030 016 octets)'
expect_in HDD 'Disque dur 5400 tr/min · SATA 6 Gb/s (lien actuel 3 Gb/s)'
expect_in HDD 'Température 41 °C (min 20 / max 52) · Allumé 23 456 h (≈ 2 ans 8 mois)'
expect_in HDD '[CRITIQUE] 3 secteur(s) en attente'
expect_in HDD '[ATTENTION] 12 erreur(s) CRC'
expect_in HDD '[ATTENTION] 8 secteur(s) réalloué(s)'
expect_in HDD '[ATTENTION] Liaison SATA négociée à 3 Gb/s'
expect_in HDD 'A ÉCHOUÉ'
expect_in HDD 'VERDICT : CRITIQUE'
expect_in SSD 'SSD · SATA 6 Gb/s (lien actuel 6 Gb/s)'
expect_in SSD 'Écrit ≈ 48,2 To* · Vie restante ≈ 92 %'
expect_in SSD 'Santé (avis du firmware) : inconnue'
expect_in SSD 'Aucun.'
expect_in SSD 'VERDICT : SAIN'
expect_in NVME 'Type      : SSD NVMe'
expect_in NVME '41 °C (alerte 80 °C)'
expect_in NVME 'Écrit 48,2 To · Vie restante ≈ 97 %'
expect_in NVME 'VERDICT : SAIN'
expect_in NVME_BAD '[CRITIQUE] Fiabilité dégradée'
expect_in NVME_BAD '[CRITIQUE] 5 erreur(s) média'
expect_in NVME_BAD 'VERDICT : CRITIQUE'
expect_in SAS_BAD '━━ SEAGATE ST4000NM0023 ━━'
expect_in SAS_BAD 'Type      : Disque dur SAS/SCSI 7200 tr/min'
expect_in SAS_BAD 'Fabriqué  : 2016, semaine 12'
expect_in SAS_BAD 'Santé (avis du firmware) : DÉFAILLANCE ANNONCÉE'
expect_in SAS_BAD 'Température 41 °C · Allumé 23 456 h (≈ 2 ans 8 mois) · 1 204 démarrages · Écrit 48,2 To'
expect_in SAS_BAD '[CRITIQUE] Le disque annonce lui-même une défaillance prochaine (ASC 5Dh/10h)'
expect_in SAS_BAD '[CRITIQUE] 2 erreur(s) de lecture non corrigée(s)'
expect_in SAS_BAD '[ATTENTION] 12 défaut(s) apparu(s)'
expect_in SAS_BAD 'Lecture : erreurs corr. / non corr.   1 234 / 2'
expect_in SAS_BAD '1 204 / 50 000 (2 %)'
expect_in SAS_BAD 'VERDICT : CRITIQUE'
expect_in SAS 'Santé (avis du firmware) : OK'
expect_in SAS 'VERDICT : SAIN'
expect_in SAS_NONE 'Aucune donnée de santé lisible'
expect_in SAS_NONE 'VERDICT : INCONNU'
expect_in SENSE 'desc valid=1 mid=4f high=c2'
expect_in SENSE 'fixed valid=1 mid=f4 high=2c'
expect_in ERASE 'crypto+blocs: 0 Sanitize, effacement cryptographique'
expect_in ERASE 'blocs: 0 Sanitize, effacement des blocs'
expect_in ERASE 'format-crypto: 0 Format NVM, effacement cryptographique'
expect_in ERASE 'format: 0 Format NVM, effacement des données utilisateur | Le disque ne propose pas Sanitize'
expect_in ERASE "multi-ns: 0 Format NVM, effacement des données utilisateur | Sanitize n'est pas utilisé"
expect_in ERASE 'multi-ns-format-global: -1 aucune | Ce NVMe a plusieurs namespaces'
expect_in ERASE 'ns-inconnu: 0 Format NVM, effacement des données utilisateur'
expect_in ERASE "ecrasement-seul: -1 aucune | Ce NVMe ne propose ni Sanitize"
expect_in ERASE "seul l'écrasement Sanitize est proposé"
expect_in ERASE 'rien: -1 aucune'
if grep -q "$(printf '\033')" "$S"; then echo "SMART : codes couleur hors terminal" >&2; exit 1; fi
if grep -E '^  ID|^  +[0-9]+ ' "$S" | LC_ALL=C.UTF-8 awk 'length($0) > 80 { bad = 1 } END { exit !bad }'; then
    echo "SMART : tableau plus large que 80 colonnes" >&2; exit 1
fi
printf 'OK\n'
