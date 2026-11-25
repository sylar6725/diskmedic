# DiskMedic

[English](README.md) · **Français**

Utilitaire Linux en C pour **diagnostiquer et entretenir des disques** SATA, SAS/SCSI et NVMe. Il fait un rapport SMART lisible, vérifie le disque en lecture, détecte et répare les blocs lents, et efface les disques avec la méthode adaptée à leur type : zéros pour un disque dur, TRIM ou effacement firmware (ATA Secure Erase, NVMe Sanitize ou Format NVM) pour un SSD. On l'utilise en ligne de commande ou par un menu interactif qui traite **plusieurs disques en parallèle**.

- Un seul fichier source C99, sans bibliothèque externe.
- Un binaire statique d'environ 1 Mo, qui fonctionne sur toute distribution Linux x86-64.
- L'interface du programme est en anglais.
- Licence MIT : libre d'utiliser, de copier, de modifier et de partager.

> [!NOTE]
> DiskMedic a été en grande partie « vibe codé » avec un assistant IA. Il est couvert par des tests automatisés, mais il n'a pas été testé sur tous les types de disques : prudence avec des disques qui contiennent des données importantes.

> [!WARNING]
> `--zero`, `--slow-repair`, `--trim` et `--secure-erase` **détruisent des données**. Chacune demande de taper exactement `YES`, et un disque monté ou utilisé est refusé. Vérifiez quand même le périphérique visé (`lsblk -o NAME,MODEL,SERIAL,SIZE`) : aucune récupération n'est possible.

---

## Sommaire

- [Fonctionnalités](#fonctionnalités)
- [Installation](#installation)
- [Utilisation](#utilisation)
- [Interface interactive (TUI)](#interface-interactive-tui)
- [Détails techniques](#détails-techniques)
- [Tests et CI](#tests-et-ci)
- [Limites connues](#limites-connues)
- [Licence](#licence)

---

## Fonctionnalités

| Fonction | SATA | SAS / SCSI | NVMe |
|---|:-:|:-:|:-:|
| `--verify` : lecture intégrale | ✅ | ✅ | ✅ |
| `--scan-slow` / `--slow-repair` : blocs lents | ✅ | ✅ | ✅ |
| `--zero` : écriture de zéros + relecture | ✅ | ✅ | ✅ |
| `--trim` : TRIM du disque entier + relecture | ✅ | ✅ (UNMAP) | ✅ (Deallocate) |
| `--smart` : rapport de santé | ✅ attributs SMART ATA | ✅ pages LOG SENSE + liste des défauts | ✅ journal SMART/Health |
| Fiche du disque dans le menu TUI | ✅ | ✅ | ✅ |
| `--secure-erase` | ✅ ATA Security Erase | ❌ refusé | ✅ Sanitize ou Format NVM |

Les trois formats de secteur sont gérés : **512n** (512/512), **512e** (512 logique / 4096 physique) et **4Kn** (4096/4096).

### SSD et NVMe

Écrire des zéros sur un SSD ne le soigne pas et ne l'efface pas de façon sûre. La mémoire flash s'use à chaque écriture, les blocs de réserve gardent des copies des anciennes données, et le contrôleur, qui croit le disque plein, ralentit jusqu'au prochain TRIM. DiskMedic détecte donc le type de support avant toute écriture :

| Opération | Disque dur | SSD (SATA, SAS ou NVMe) |
|---|---|---|
| `--zero` | Exécutée | **Refusée**, avec une explication. `--zero --force` l'exécute quand même |
| `--slow-repair` | Exécutée | **Refusée** : un SSD rafraîchit lui-même ses données, et une lenteur y signale le contrôleur ou la flash. `--slow-repair --force` l'exécute quand même |
| `--scan-slow` | Exécuté | Exécuté, avec une note sur l'interprétation |
| `--trim` | Rarement géré (refus propre, rien n'est modifié) | Vide le disque en quelques secondes, sans usure |
| `--secure-erase` | ATA Security Erase | ATA Security Erase (SATA), ou méthode NVMe choisie selon le disque |

Le type de support est lu dans le firmware : tout NVMe est un SSD, puis vient le mot 217 d'IDENTIFY en ATA, puis la page VPD `B1h` en SCSI. Le drapeau `rotational` du noyau ne sert qu'en dernier recours, car il peut être faux : un NVMe monté sur un fichier loop, par exemple, y apparaît comme rotatif.

Pour un NVMe, `--secure-erase` choisit la méthode d'après les capacités annoncées par le contrôleur (Identify Controller : `OACS`, `SANICAP`, `FNA`), dans cet ordre :

1. **Sanitize cryptographique** : le disque change sa clé de chiffrement interne. C'est instantané et sans usure, et cela couvre aussi les blocs de réserve et les caches.
2. **Sanitize par effacement des blocs**.
3. **Format NVM cryptographique** (`SES=2`), si le disque l'annonce.
4. **Format NVM avec effacement des données utilisateur** (`SES=1`), en conservant le format de secteur actuel.

La méthode est annoncée avant la confirmation `YES`. Sanitize agit sur tout le contrôleur : avec plusieurs namespaces actifs, DiskMedic ne l'utilise pas et passe à Format NVM, qui n'efface que le namespace visé, quand le disque le permet. Sinon, il refuse. L'écrasement par Sanitize n'est jamais utilisé, car il use la flash. Les capacités sont vérifiées par DiskMedic lui-même, sans compter sur un refus du disque : l'émulation NVMe de QEMU, par exemple, accepte un effacement cryptographique qu'elle n'annonce pas.

## Installation

### Option 1 : binaire précompilé (aucune dépendance)

Téléchargez l'archive et `SHA256SUMS` depuis la page [Releases](../../releases/latest). Le binaire est lié statiquement et ne dépend d'aucune bibliothèque de la machine.

```sh
sha256sum -c SHA256SUMS --ignore-missing   # facultatif : contrôle d'intégrité
tar -xzf diskmedic-1.12.0-linux-x86_64.tar.gz
cd diskmedic-1.12.0-linux-x86_64
sudo ./diskmedic --help
```

Seuls prérequis : Linux x86-64, noyau 3.2 ou plus récent, et les droits root.

### Option 2 : compilation depuis les sources

**Dépendances de compilation** : un compilateur C99 (gcc), `make` et les en-têtes du noyau Linux. Le code utilise `<linux/fs.h>`, `<linux/nvme_ioctl.h>` et `<scsi/sg.h>`. Pour un binaire statique, il faut aussi la libc statique.

| Distribution | Paquets |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential linux-libc-dev` |
| Fedora / RHEL | `sudo dnf install gcc make kernel-headers glibc-static` |
| Arch Linux | `sudo pacman -S base-devel linux-api-headers` |
| openSUSE | `sudo zypper install gcc make linux-glibc-devel glibc-devel-static` |

```sh
make                 # compile ./diskmedic (-O2 -Wall -Wextra -Werror -std=c99)
make test            # tests sans root

# Binaire statique, portable d'une distribution à l'autre :
cc -O2 -Wall -Wextra -Werror -std=c99 -static diskmedic.c -o diskmedic && strip diskmedic
```

Le code a été compilé et testé avec gcc 13 et la glibc sur Ubuntu 24.04. D'autres compilateurs ou libc (clang, musl) devraient fonctionner, mais n'ont pas été testés.

**Exécution** : rien n'est nécessaire en dehors du noyau. Il faut être root, ou disposer de `CAP_SYS_RAWIO` plus des droits sur le périphérique, pour envoyer des commandes ATA, SCSI ou NVMe brutes et ouvrir les disques en exclusivité.

## Utilisation

```sh
sudo ./diskmedic /dev/sdX --smart                          # fiche + rapport SMART
sudo ./diskmedic /dev/sdX --verify --log verif.csv         # lecture complète
sudo ./diskmedic /dev/sdX --scan-slow --slow-factor 4      # recherche de blocs lents
sudo ./diskmedic /dev/sdX --slow-repair --log repar.csv    # scan + réécriture ciblée  ⚠
sudo ./diskmedic /dev/sdX --zero                           # zéros (disque dur)        ⚠
sudo ./diskmedic /dev/nvme0n1 --trim                       # TRIM complet (SSD)        ⚠
sudo ./diskmedic /dev/nvme0n1 --secure-erase               # effacement firmware       ⚠
sudo ./diskmedic                                           # menu multi-disques
sudo ./diskmedic --tui /dev/sdX                            # menu pour un seul disque
```

Une seule opération est acceptée par invocation.

| Option | Rôle |
|---|---|
| `--smart` (alias `--smart-only`) | Fiche du disque et rapport SMART lisible |
| `--verify` | Lecture intégrale, sans écriture |
| `--scan-slow` (alias `--slow-scan`) | Recherche des blocs anormalement lents (lecture seule) |
| `--slow-factor N` | Seuil de lenteur = médiane × N (défaut : 4, minimum : 1) |
| `--log FICHIER` | Journal CSV des secteurs défectueux ou lents (`--verify`, `--scan-slow`, `--slow-repair`) |
| `--slow-repair` | Scan, puis réécriture des seules zones lentes confirmées (destructif) |
| `--zero` | Écrit des zéros sur tout le disque, puis relit pour vérifier (destructif ; refusé sur SSD) |
| `--trim` | TRIM du disque entier par tranches de 1 Gio, puis relecture d'échantillons (destructif) |
| `--secure-erase` | ATA Security Erase (mode *enhanced* si proposé), ou NVMe Sanitize / Format NVM (destructif) |
| `--force` | Avec `--zero` ou `--slow-repair` : lance l'opération même sur un SSD |
| `--tui` | Menu interactif ; sans périphérique, sélection de plusieurs disques |
| `--help`, `-h` / `--version` | Aide / version |

**Codes de sortie**

| Code | Signification |
|---|---|
| `0` | Succès |
| `1` | Erreur, opération refusée (SSD, Secure Erase impossible, TRIM non géré), secteurs illisibles trouvés, ou au moins un disque en échec (mode multi-disques) |
| `2` | Options invalides, confirmation `YES` absente, ou mode TUI demandé sans terminal |
| `3` / `4` / `5` | `--smart` seulement : le rapport a été lu, et son verdict est WARNING / CRITICAL / UNKNOWN (`0` = HEALTHY) |

**Journal CSV** : il contient les colonnes `timestamp_iso8601,device,operation,LBA,status,action`. Les LBA sont exprimés en secteurs logiques.

- Chaque LBA illisible est notée sur sa propre ligne dès qu'elle est trouvée, au passage 1 ou au passage 2 d'un scan.
- Un bloc en erreur au passage 1 mais lisible au passage 2 reçoit une ligne `TRANSIENT READ ERROR` ; le scan se termine alors avec le code `1`.
- Après `--slow-repair`, chaque LBA perdue reçoit une deuxième ligne avec son résultat final (`REWRITE VERIFIED…` ou `FAILED…`).
- Une zone lente est notée avec sa plage de LBA (`SLOW: zone LBA 21120-21247 (64 KiB) …`).

## Interface interactive (TUI)

`sudo ./diskmedic` ouvre la sélection des disques :

| Touche | Action |
|---|---|
| ↑ / ↓ | Se déplacer |
| Espace | Sélectionner ou désélectionner un disque |
| `a` | Tout sélectionner ou tout désélectionner |
| Entrée | Valider, puis choisir l'opération (1 à 7, 7 = TRIM) |
| `q` | Quitter |

Les disques sélectionnés sont **traités en parallèle**, un processus par disque, et suivis sur un tableau de bord plein écran :

```
╔════════════════════════════════════════════════════════════════════════════════════════╗
║DiskMedic 1.11.0 · Slow scan · 3 disk(s) in parallel                            00:00:03║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║/dev/sde · scsi_debug · 201.3 MB                                      RUNNING · 00:00:03║
║  Step: Pass 1/2: timing                                                   unreadable: 1║
║  ██████████████████████████████████████████████ 100.0 % · 158.4 MiB/s · left 00:00:00  ║
║  › Unreadable sector: LBA 20483 (No data available)                                    ║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║  … one block per disk …                                                                ║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║Running: 3 · Finished: 0 (OK: 0, failed: 0)                                             ║
║Logs: /root/diskmedic-20261121-232657                                                   ║
║q = interrupt all operations                                                            ║
╚════════════════════════════════════════════════════════════════════════════════════════╝
```

Pour chaque disque, le tableau de bord montre :

- l'état et le temps écoulé ;
- l'étape en cours et le nombre de secteurs illisibles trouvés ;
- le débit et le temps restant ;
- le dernier message de l'opération ;
- une alerte « updated N s ago » quand le disque n'avance plus (nouvelles tentatives sur un secteur défectueux).

Si la fenêtre est petite, l'affichage passe à deux lignes par disque.

- **Interruption** : `q` ou Ctrl+C, puis `y` pour confirmer. Le résumé indique l'étape et le pourcentage atteints par chaque disque. Un Secure Erase ne peut pas être interrompu, car c'est le firmware du disque qui l'exécute.
- **Journaux** : un dossier `diskmedic-AAAAMMJJ-HHMMSS/` est créé dans le dossier courant (ou dans `/tmp`). Il contient un `sdX.log` par disque, plus un `sdX.csv` pour la vérification et le scan. Sous `sudo`, les fichiers sont attribués à l'utilisateur appelant.
- **Fin** : le terminal normal affiche le rapport de chaque disque, puis un résumé `[OK]` / `[FAILED]` / `[INTERRUPTED]`. Pour SMART, un disque dont le verdict n'est pas sain apparaît en `[WARNING]`, `[CRITICAL]` ou `[UNKNOWN]`, et le tableau de bord le compte comme alerte SMART.
- **Sécurité** : avant une opération destructive, la liste des disques visés s'affiche, avec l'opération prévue pour chacun. Si un seul d'entre eux est monté ou utilisé, rien n'est lancé.
- **SSD dans la sélection** : pour une écriture zéro, DiskMedic liste les SSD et propose `t` (TRIM sur les SSD, zéros sur les disques durs), `z` (zéros partout) ou l'annulation. Pour une réparation lente, il propose `s` (scan seul sur les SSD), `r` (réparer partout) ou l'annulation.

Avec `--tui /dev/sdX`, le menu porte sur un seul disque. Son en-tête affiche la fiche du disque, quel que soit son type :

- SATA : modèle, série, firmware ;
- NVMe : santé, température, vie restante ;
- SAS : avis du firmware, température.

Il indique aussi la capacité, le format de secteur et le type de support (SSD ou disque dur). Sur un SSD, les choix 4 et 5 proposent l'alternative adaptée (scan seul, TRIM) ou l'opération quand même. La vérification, le scan lent et la réparation écrivent leur journal CSV dans un nouveau dossier `diskmedic-AAAAMMJJ-HHMMSS/`, comme en mode multi-disques.

## Détails techniques

### Accès au matériel

| Élément | Mise en œuvre |
|---|---|
| Commandes ATA (SATA, y compris derrière une passerelle USB/SAT) | `SG_IO` + ATA PASS-THROUGH(16), opcode `85h`. Registres de retour lus dans le descripteur sense `09h` ou dans le sense fixe (ASC/ASCQ `00/1Dh`) |
| SMART ATA | IDENTIFY DEVICE (`ECh`), SMART READ DATA (`D0h`), READ THRESHOLDS (`D1h`), RETURN STATUS (`DAh`) |
| NVMe | `NVME_IOCTL_ADMIN_CMD` : Identify Controller / Namespace / liste des namespaces actifs (CNS `01h`, `00h`, `02h`), Get Log Page `02h` (SMART/Health) et `81h` (état du Sanitize), Sanitize (`84h`), Format NVM (`80h`) |
| SAS / SCSI | INQUIRY + pages VPD `80h`/`B1h`, LOG SENSE (`2Fh`, `0Dh`, `03h`, `02h`, `05h`, `06h`, `0Eh`, `15h`, `11h`), READ DEFECT DATA(10) pour la liste des défauts apparus (liste G) |
| Détection du type | NVMe (`NVME_IOCTL_ID`), puis IDENTIFY ATA, puis INQUIRY SCSI |
| Détection SSD / disque dur | NVMe, mot 217 d'IDENTIFY (vitesse de rotation), VPD `B1h`, puis `/sys/dev/block/M:m/queue/rotational` |
| TRIM | `BLKDISCARD` par tranches de 1 Gio (le noyau le traduit en DATA SET MANAGEMENT, UNMAP ou Deallocate) |
| Géométrie | `BLKGETSIZE64`, `BLKSSZGET` (secteur logique), `BLKPBSZGET` (secteur physique) |

### Fiabilité des mesures et sécurité

- **Revendication exclusive** : avant toute écriture, le disque est ouvert avec `O_EXCL`. Le noyau refuse cette ouverture si le disque, ou l'une de ses partitions, est monté, utilisé en swap, en RAID ou en LVM.
- **`O_DIRECT`** : les lectures et les écritures contournent le cache de pages, avec des tampons alignés et `posix_fadvise(DONTNEED)`. Le programme revient aux E/S tamponnées si le périphérique ne gère pas `O_DIRECT`.
- **Relecture par secteur physique** : un bloc illisible est relu d'abord secteur physique par secteur physique, puis secteur logique par secteur logique pour les seuls secteurs physiques en échec. Sur un disque 512e, un secteur physique entièrement perdu coûte ainsi 4 lectures en échec au lieu de 9, chaque échec pouvant prendre plusieurs secondes de nouvelles tentatives au disque.
- **Secure Erase** : le mot de passe ATA temporaire est `diskmedic`, et il est affiché avant l'opération. Après une interruption, le disque se déverrouille avec :
  `hdparm --user-master u --security-unlock diskmedic /dev/sdX`, puis `--security-disable diskmedic`.
  Un disque verrouillé par une version antérieure au renommage du projet (zero-fix) a pour mot de passe `zero-fix`.
- **Sanitize NVMe** : il est lancé avec `AUSE=1`, ce qui permet de sortir du mode d'échec si l'effacement échoue. Son avancement est lu dans le journal `81h`. Le disque le poursuit seul, même après une coupure de courant ou l'arrêt de DiskMedic.

### Algorithme du scan de blocs lents

1. **Passage 1, mesure** : chaque bloc de 1 Mio est chronométré. La médiane est calculée sur un échantillon de 1 million de mesures au plus, et le seuil vaut médiane × `--slow-factor`, avec un écart minimal de 5 ms au-dessus de la médiane. Sans ce plancher, sur un support très rapide (NVMe, médiane inférieure à 1 ms), les pauses ordinaires du système suffiraient à dépasser le seuil. Une relecture sur disque dur coûte au moins un tour de plateau (8 à 11 ms) et reste détectée.
2. **Passage 2, confirmation** : un bloc n'est retenu que s'il dépasse le seuil aux deux passages. Un bloc lent à un seul passage est classé *intermittent* et n'est jamais réécrit. Une erreur de lecture est localisée secteur par secteur dès qu'elle survient, au passage 1 ou 2 ; un bloc en erreur au passage 1 et lisible au passage 2 est signalé comme *erreur de lecture passagère*.
3. **Localisation** : chaque bloc confirmé est relu par zones de 64 Kio, ou de la taille du secteur physique si elle est plus grande, de la dernière zone à la première. Avant cela, 64 Mio sont lus ailleurs sur le disque pour vider son cache. Chaque zone est comparée à une zone saine de référence : elle est lente si elle porte au moins la moitié du retard du bloc, ou si elle dépasse le facteur × la référence avec au moins 1 ms d'écart.
4. **Réécriture ciblée** (`--slow-repair`) : seules les zones lentes sont réécrites, avec leurs propres données, puis relues et comparées. Dans un bloc illisible, seuls les secteurs physiques qui contiennent un secteur perdu sont réécrits ; la partie perdue est remplacée par des zéros, ce qui permet au firmware de réallouer le secteur.
5. **Contrôle différé** : chaque zone réécrite est rechronométrée après un nouveau vidage du cache, puis classée `FAST` ou `STILL SLOW`.

### Exécution parallèle

- **Isolation** : chaque disque est traité par un processus fils (`fork`), qui a ses propres tailles de secteur, son propre journal CSV et son propre descripteur `O_DIRECT`. Aucun état n'est partagé entre disques.
- **Avancement** : chaque processus publie son étape, ses octets traités, ses débits et ses secteurs illisibles dans une zone `mmap(MAP_SHARED)`. Cette zone est protégée par un verrou de séquence, que le tableau de bord lit sans bloquer, quatre fois par seconde.
- **Sorties** : la sortie standard et la sortie d'erreur de chaque processus sont redirigées vers son journal `.log`.
- **Clavier** : il est lu sans signaux, si bien que Ctrl+C devient une touche et que l'interruption est toujours confirmée. `SIGTERM` et `SIGHUP` arrêtent proprement les processus fils.

### Ressources utilisées

- **Mémoire** :
  - vérification et écriture zéro : quelques Mio ;
  - scan lent : environ 4 Mio de RAM par To de disque, plus 8 Mio pour l'échantillon de mesures.
- **E/S** : des blocs de 1 Mio. En mode multi-disques, les disques d'un même contrôleur, d'un même hub USB ou d'un même boîtier se partagent sa bande passante.

### Organisation du dépôt

```
diskmedic.c                 Source unique (CLI, SMART ATA/NVMe/SAS, E/S disque, scan, TUI)
Makefile                   make / make test / make test-loop / make clean
tests/run_tests.sh         Tests sans root (options, codes de sortie, rendu SMART)
tests/smart_fixture_test.c Rapports SMART ATA/NVMe/SAS générés à partir de données fabriquées
tests/run_loop_tests.sh    Tests root sur disques virtuels (loop + device-mapper)
tests/run_sector_tests.sh  Tests root 512n/512e/4Kn, SSD, TRIM et multi-disques (scsi_debug)
tests/run_nvme_tests.sh    Tests root NVMe (nvme-loop + machine virtuelle QEMU)
.github/workflows/ci.yml   CI GitHub Actions
dist/                      Archive de release et SHA256SUMS (hors git, publiés dans les Releases)
LICENSE                    Licence MIT
```

## Tests et CI

Les tests n'utilisent **jamais de disque réel**, seulement des fichiers, des loop devices, des disques SCSI virtuels en mémoire et des contrôleurs NVMe émulés.

| Commande | Droits | Contenu |
|---|---|---|
| `make test` | utilisateur | Options, codes de sortie, rendu SMART sur données fabriquées (HDD/SSD SATA, NVMe, SAS), décodage SAT, absence de couleurs hors terminal, largeur ≤ 80 colonnes |
| `sudo sh tests/run_loop_tests.sh` | root | Vérification, scan, réparation, écriture zéro et refus d'un disque monté, sur loop device. Secteurs illisibles simulés par `dm-error` et `dm-flakey`, secteurs lents par `dm-delay`, et une erreur de lecture passagère (la table en erreur est remplacée par une table saine entre les deux passages). Vérifie le journal CSV ligne par ligne |
| `sudo sh tests/run_sector_tests.sh` | root | Mêmes opérations en 512n, 512e et 4Kn avec `scsi_debug`, qui se présente comme un SSD : refus de `--zero` et `--slow-repair` sans `--force`, TRIM refusé sans UNMAP puis réussi avec. Vérifie aussi le LBA exact signalé, qu'un seul secteur physique est réécrit, le nombre de lectures en échec, le menu multi-disques piloté au clavier dans un pseudo-terminal (dont le choix TRIM sur SSD), le journal CSV du menu d'un seul disque, et un disque qui annonce une défaillance (mode test activé avec `sdparm`) : code de sortie `4` et `[CRITICAL]` dans le résumé |
| `sudo sh tests/run_nvme_tests.sh` | root | **nvme-loop** (cible NVMe du noyau, sans Format ni Sanitize) : SMART, refus de `--zero`, refus de Secure Erase avant confirmation, TRIM. **QEMU** (machine virtuelle de quelques secondes, avec trois contrôleurs NVMe émulés) : Format NVM réellement exécuté sur des secteurs de 512 o et de 4 Kio (format conservé), et sur un contrôleur à deux namespaces (l'autre namespace reste intact) |

`make test-loop` lance les trois scripts root. Ceux-ci s'ignorent eux-mêmes (`SKIP`) si les droits, les modules du noyau ou les outils manquent.

Le choix de la méthode NVMe (Sanitize cryptographique ou par blocs, Format, cas à plusieurs namespaces) est aussi testé par `make test`, sur des données Identify fabriquées.

**Outils requis pour les tests root** :

- `util-linux` : `losetup`, `blockdev`, `script` ;
- `dmsetup` (paquet `dmsetup` ou `device-mapper`) ;
- `e2fsprogs` : `mkfs.ext4` ;
- `coreutils` : `dd`, `md5sum`, `timeout` ;
- `udevadm` ;
- `sdparm` (facultatif : test SMART CRITICAL) ;
- les modules noyau `dm-delay`, `dm-flakey` et `scsi_debug`.

Sur Ubuntu, `scsi_debug` est fourni par `linux-modules-extra-$(uname -r)`.

**Outils supplémentaires pour les tests NVMe** :

- les modules noyau `nvmet` et `nvme-loop` (partie nvme-loop) ;
- `qemu-system-x86`, `busybox-static`, `cpio`, `gzip` et la libc statique (partie QEMU) ;
- un noyau invité avec le pilote NVMe intégré : `DISKMEDIC_TEST_KERNEL`, sinon le noyau WSL (`C:\Program Files\WSL\tools\kernel`), sinon `/boot/vmlinuz-$(uname -r)`. KVM est utilisé s'il est disponible ; sinon, l'émulation logicielle est plus lente mais fonctionne.

Sous Ubuntu : `sudo apt install qemu-system-x86 busybox-static cpio`.

La **CI GitHub Actions** (`ubuntu-latest`) compile avec les avertissements traités comme erreurs, puis lance les quatre suites à chaque push et à chaque pull request.

## Limites connues

- **Secure Erase** : ATA Security Erase et, pour NVMe, Sanitize et Format NVM. ATA Sanitize (SATA) et FORMAT UNIT / SANITIZE (SAS) ne sont pas implémentés : l'option est refusée sur les disques SAS, sans rien modifier.
- **Sanitize NVMe** : ni nvme-loop ni QEMU 8.2 n'émulent Sanitize, les tests automatisés ne couvrent donc que le choix de la méthode, sur données fabriquées. La commande elle-même suit la spécification NVMe. Format NVM est testé dans QEMU.
- **Détection SSD** : elle repose sur ce que le firmware annonce. Un disque qui ne renseigne pas sa vitesse de rotation, derrière une passerelle USB par exemple, est jugé d'après le noyau, qui peut se tromper. Les rares disques durs NVMe seraient traités comme des SSD.
- **`--zero`** écrit jusqu'à la taille vue par le noyau. Les zones cachées (HPA/DCO) et les blocs réservés du firmware ne sont pas atteints ; le Secure Erase, lui, les couvre en principe.
- **Secteurs réalloués** : ils ne peuvent pas être « réparés ». Une réécriture peut seulement provoquer la réallocation d'un secteur *en attente*.
- **Passerelles USB** : certains boîtiers ne transmettent ni les commandes ATA ni les pages SMART. Le rapport indique alors `UNKNOWN` plutôt que `HEALTHY`.
- **Types de disques** : DiskMedic n'a pas été testé sur tous les types de disques (modèles, interfaces, passerelles USB, contrôleurs). Les retours accompagnés de la sortie de `smartctl -x` sont les bienvenus.
- **Plate-forme** : Linux uniquement (ioctl `SG_IO`, `BLK*`, NVMe).

## Licence

Distribué sous **licence MIT** : voir [LICENSE](LICENSE).

Vous pouvez librement utiliser, copier, modifier, fusionner, publier, distribuer, sous-licencier et vendre ce logiciel, y compris dans un cadre commercial. La seule condition est de conserver la mention de copyright et le texte de la licence dans toute copie ou partie substantielle du code.

Le logiciel est fourni **« tel quel », sans aucune garantie**. Les auteurs ne sont pas responsables des pertes de données ni des dommages matériels qui pourraient résulter de son utilisation. Cela vaut tout particulièrement pour un outil capable d'effacer des disques.
