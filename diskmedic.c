/*
 * DiskMedic — diagnostic et maintenance de disques SATA, SAS et NVMe.
 * Copyright (c) 2026 Les contributeurs de DiskMedic
 * SPDX-License-Identifier: MIT — voir le fichier LICENSE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define DISKMEDIC_VERSION "1.10.0"
#define BUF_SIZE (1024U * 1024U)
#define ATA_TIMEOUT_MS (24U * 60U * 60U * 1000U)
#define ATA_IDENTIFY 0xEC
#define ATA_SMART 0xB0
#define SMART_READ_DATA 0xD0
#define SMART_READ_THRESHOLDS 0xD1
#define SMART_RETURN_STATUS 0xDA
#define ATA_SET_PASSWORD 0xF1
#define ATA_ERASE_PREPARE 0xF3
#define ATA_ERASE_UNIT 0xF4
#define ATA_DISABLE_PASSWORD 0xF6

typedef struct {
    int smart_only, secure_erase, zero, verify, slow_scan, slow_repair, tui, trim, force;
    double slow_factor;
    const char *device;
    const char *log_path;
} options_t;

static FILE *log_file = NULL;
static uint64_t log_entries = 0;
static int log_failed = 0;
static const char *log_path_active = NULL;
static unsigned log_sector_size = 512;

static unsigned phys_sector_size = 512;

/* État d'un bloc signalé par le scan des blocs lents. */
enum {
    SB_UNREADABLE,      /* erreur de lecture */
    SB_CONFIRMED,       /* lent aux passages 1 et 2, lenteur localisée */
    SB_INTERMITTENT,    /* lent à un seul passage : non confirmé */
    SB_NOT_REPRODUCED   /* lent aux deux passages mais pas lors de la localisation */
};
typedef struct { uint64_t index; double ms, ms1; int status; } slow_block_t;

static const char *ui_device = "(inconnu)";
static int ui_fd = -1;
static uint64_t ui_size = 0;
static int ui_tui = 0, ui_active = 0;
typedef struct {
    struct timespec start, last;
    uint64_t last_bytes;
    int initialized;
} progress_t;
/* Sortie du dessin TUI : stderr, ou un tampon mémoire pour composer une image d'écran entière. */
static FILE *ui_out = NULL;
#define UI_OUT (ui_out ? ui_out : stderr)

/*
 * Exécution parallèle (sélection multi-disques) : chaque disque est traité par un processus
 * fils qui publie son avancement dans une zone de mémoire partagée, lue par le tableau de bord.
 * seq est impair pendant une mise à jour (verrou de séquence : lecture sans attente).
 */
typedef struct {
    unsigned seq;
    char phase[96];
    uint64_t done, total, bad;
    double instant, average, eta;
    struct timespec updated;
} worker_slot_t;
static worker_slot_t *worker_slot = NULL;
static void worker_publish(const char *phase, uint64_t done, uint64_t total,
                           double instant, double average, double eta) {
    worker_slot_t *w = worker_slot;
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_ACQ_REL);
    if (phase) snprintf(w->phase, sizeof(w->phase), "%s", phase);
    w->done = done; w->total = total;
    w->instant = instant; w->average = average; w->eta = eta;
    clock_gettime(CLOCK_MONOTONIC, &w->updated);
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_RELEASE);
}
static void worker_add_bad(void) {
    if (!worker_slot) return;
    __atomic_add_fetch(&worker_slot->seq, 1, __ATOMIC_ACQ_REL);
    worker_slot->bad++;
    __atomic_add_fetch(&worker_slot->seq, 1, __ATOMIC_RELEASE);
}
static void worker_snapshot(const worker_slot_t *src, worker_slot_t *dst) {
    for (int tries = 0; tries < 1000; tries++) {
        unsigned s1 = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        memcpy(dst, src, sizeof(*dst));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&src->seq, __ATOMIC_RELAXED) == s1) break;
    }
    dst->phase[sizeof(dst->phase) - 1] = '\0';
}

static void usage(FILE *f) {
    fprintf(f,
        "DiskMedic %s — utilitaire Linux de diagnostic et maintenance ATA/bloc\n\n"
        "Usage :\n"
        "  sudo diskmedic /dev/sdX [OPTIONS]\n"
        "  diskmedic                          Ouvre la sélection interactive de disques\n"
        "  diskmedic --tui [ /dev/sdX ]      Menu interactif (sans disque : multi-sélection)\n"
        "  diskmedic --help | --version\n\n"
        "Options (une opération par invocation) :\n"
        "  --smart             Fiche du disque + rapport SMART lisible (SATA, SAS et NVMe)\n"
        "  --verify            Lecture complète du disque (sans écriture)\n"
        "  --scan-slow         Blocs lents : 2 passages de confirmation + localisation par zones de 64 Kio\n"
        "  --slow-factor N     Seuil = médiane × N (défaut : 4)\n"
        "  --log FICHIER       Journal CSV des secteurs défectueux/lents (scan/réparation)\n"
        "  --slow-repair       Scan puis réécriture des seules zones lentes confirmées (destructif)\n"
        "  --zero              Écrit des zéros sur tout le disque (destructif; refusé sur SSD)\n"
        "  --trim              TRIM du disque entier : vide un SSD sans l'user (destructif)\n"
        "  --secure-erase      Effacement firmware : ATA Security Erase, ou NVMe Sanitize /\n"
        "                      Format NVM choisi selon le disque (destructif)\n"
        "  --force             Avec --zero ou --slow-repair : lance quand même sur un SSD\n"
        "  --tui               Menu interactif; sans disque : disques traités en parallèle\n"
        "  TUI : ↑/↓ déplacer, Espace sélectionner, a tout sélectionner, Entrée lancer, q quitter\n"
        "  --help, -h          Affiche cette aide\n"
        "  --version           Affiche la version\n\n"
        "Les opérations destructives demandent la confirmation exacte OUI.\n"
        "Exemple : sudo diskmedic /dev/sdX --smart\n", DISKMEDIC_VERSION);
}
static int parse_options(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o)); o->slow_factor = 4.0;
    o->device = NULL; o->log_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--smart") || !strcmp(argv[i], "--smart-only")) o->smart_only = 1;
        else if (!strcmp(argv[i], "--secure-erase")) o->secure_erase = 1;
        else if (!strcmp(argv[i], "--zero")) o->zero = 1;
        else if (!strcmp(argv[i], "--trim")) o->trim = 1;
        else if (!strcmp(argv[i], "--force")) o->force = 1;
        else if (!strcmp(argv[i], "--verify")) o->verify = 1;
        else if (!strcmp(argv[i], "--tui")) o->tui = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc && argv[i+1][0] != '-') o->log_path = argv[++i];
        else if (!strncmp(argv[i], "--log=", 6) && argv[i][6]) o->log_path = argv[i] + 6;
        else if (!strcmp(argv[i], "--scan-slow") || !strcmp(argv[i], "--slow-scan")) o->slow_scan = 1;
        else if (!strcmp(argv[i], "--slow-repair")) o->slow_scan = o->slow_repair = 1;
        else if (!strcmp(argv[i], "--slow-factor") && i + 1 < argc) {
            char *end; errno = 0; double v = strtod(argv[++i], &end);
            if (errno || end == argv[i] || *end || !isfinite(v) || v < 1.0 || v > 1000000.0) return -1;
            o->slow_factor = v;
        } else if (argv[i][0] == '-' || o->device) return -1;
        else o->device = argv[i];
    }
    if (o->device && o->device[0] != '/') return -1;
    if (o->tui) {
        if (o->log_path || o->smart_only || o->secure_erase || o->zero || o->verify || o->slow_scan || o->trim || o->force) return -1;
        return 0;
    }
    if (!(o->smart_only || o->secure_erase || o->zero || o->verify || o->slow_scan || o->trim)) {
        if (!o->device && !o->force) { o->tui = 1; return 0; }
        return -1;
    }
    if (!o->device) return -1;
    if ((o->smart_only + o->secure_erase + o->zero + o->verify + o->slow_scan + o->trim) != 1) return -1;
    if (o->force && !(o->zero || o->slow_repair)) return -1;
    if (o->slow_repair && !o->slow_scan) return -1;
    if (o->log_path && !(o->verify || o->slow_scan)) return -1;
    return 0;
}
/* Width is taken from stdout as requested; stderr carries the UI output. */
static unsigned terminal_columns(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (unsigned)ws.ws_col;
    return 80U;
}
static size_t utf8_char_bytes(const unsigned char *s) {
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xc2 && s[0] <= 0xdf && s[1] && (s[1] & 0xc0) == 0x80) return 2;
    if (s[0] >= 0xe0 && s[0] <= 0xef && s[1] && s[2] && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) return 3;
    if (s[0] >= 0xf0 && s[0] <= 0xf4 && s[1] && s[2] && s[3] && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) return 4;
    return 1;
}
static void print_padded(const char *text, unsigned width) {
    const unsigned char *s = (const unsigned char *)text;
    unsigned used = 0;
    while (*s && used < width) {
        size_t n = utf8_char_bytes(s);
        if (n > width - used) break;
        (void)fwrite(s, 1, n, UI_OUT);
        s += n;
        used++;
    }
    while (used++ < width) fputc(' ', UI_OUT);
}
static void print_tui_line(const char *text) {
    unsigned cols = terminal_columns();
    if (cols >= 2) {
        fputs("║", UI_OUT);
        print_padded(text, cols - 2);
        fputs("║\n", UI_OUT);
    } else {
        print_padded(text, cols);
        fputc('\n', UI_OUT);
    }
}
static void print_tui_rule(char left, char right) {
    unsigned cols = terminal_columns();
    if (cols >= 2) {
        fprintf(UI_OUT, "%s", left == 'T' ? "╔" : left == 'B' ? "╚" : "╠");
        for (unsigned i = 2; i < cols; i++) fputs("═", UI_OUT);
        fprintf(UI_OUT, "%s\n", right == 'T' ? "╗" : right == 'B' ? "╝" : "╣");
    }
}
/* Début d'une étape : en exécution parallèle, l'étape est publiée immédiatement au tableau de bord. */
static void progress_begin(progress_t *p, const char *phase, uint64_t total) {
    if (worker_slot) worker_publish(phase, 0, total, 0, 0, 0);
    memset(p, 0, sizeof(*p));
    clock_gettime(CLOCK_MONOTONIC, &p->start);
    p->last = p->start;
    p->initialized = 1;
}
static double elapsed_seconds(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec-a->tv_sec) + (double)(b->tv_nsec-a->tv_nsec)/1e9;
}
static void format_eta(double sec, char *out, size_t n) {
    if (sec < 0 || sec > 359999.0) { snprintf(out, n, "--:--:--"); return; }
    unsigned long t = (unsigned long)sec;
    snprintf(out, n, "%02lu:%02lu:%02lu", t/3600, (t/60)%60, t%60);
}
static void progress_show(progress_t *p, const char *phase, uint64_t done, uint64_t total) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double duration = elapsed_seconds(&p->start, &now);
    double interval = elapsed_seconds(&p->last, &now);
    double instant = interval > 0.000001 ? (double)(done-p->last_bytes)/1048576.0/interval : 0.0;
    double average = duration > 0.000001 ? (double)done/1048576.0/duration : 0.0;
    double eta = average > 0.000001 && total >= done ? (double)(total-done)/1048576.0/average : 0.0;
    char etastr[32]; format_eta(eta, etastr, sizeof(etastr));
    double pct = total ? 100.0*(double)done/(double)total : 100.0;
    if (pct > 100.0) pct = 100.0;
    if (worker_slot) {
        worker_publish(phase, done, total, instant, average, eta);
        p->last = now; p->last_bytes = done;
        return;
    }
    if (ui_tui) {
        fputs("\033[H", stderr);
        unsigned cols=terminal_columns();
        print_tui_rule('T','T');
        print_tui_line("DiskMedic · MONITEUR");
        print_tui_rule('M','M');
        char row[PATH_MAX + 128];
        snprintf(row,sizeof(row),"Disque : %s",ui_device); print_tui_line(row);
        snprintf(row,sizeof(row),"Étape : %s",phase); print_tui_line(row);
        print_tui_rule('M','M');
        unsigned inner=cols>2?cols-2:cols;
        unsigned suffix=9U, bar=inner>11U?inner-11U:1U;
        char *barline=(char *)malloc((size_t)inner+32U);
        if(barline) {
            unsigned pos=0;
            if(inner>=2U) { barline[pos++]='['; }
            unsigned room=inner>suffix+1U?inner-suffix-1U:1U;
            if(bar>room) bar=room;
            for(unsigned i=0;i<bar;i++) barline[pos++]=(i<(unsigned)((pct/100.0)*bar))?'#':'.';
            int nr=snprintf(barline+pos,(size_t)inner+32U-pos,"] %6.2f%%",pct);
            if(nr<0) barline[pos]='\0';
            print_tui_line(barline);
            free(barline);
        }
        snprintf(row,sizeof(row),"Débit instantané : %.2f Mio/s | Moyenne : %.2f Mio/s",instant,average); print_tui_line(row);
        snprintf(row,sizeof(row),"Traité : %.2f / %.2f Mio | Temps restant : %s",(double)done/1048576.0,(double)total/1048576.0,etastr); print_tui_line(row);
        print_tui_rule('B','B');
        fputs("\033[J", stderr);
    } else {
        unsigned cols=terminal_columns();
        char tail[160];
        snprintf(tail,sizeof(tail)," %6.2f%% | %.2f/%.2f Mio/s | ETA %s",pct,instant,average,etastr);
        size_t phase_width=0;
        for(const unsigned char *q=(const unsigned char *)phase;*q;q+=utf8_char_bytes(q)) phase_width++;
        size_t fixed=strlen(tail)+3U;
        unsigned bar=cols>(unsigned)(phase_width+fixed)?cols-(unsigned)phase_width-(unsigned)fixed:1U;
        size_t capacity=(size_t)cols+512U;
        char *row=(char *)malloc(capacity);
        if(row) {
            size_t used=(size_t)snprintf(row,capacity,"%s [",phase);
            for(unsigned i=0;i<bar && used+1<capacity;i++) row[used++]=(i<(unsigned)((pct/100.0)*bar))?'#':'-';
            row[used]='\0';
            (void)snprintf(row+used,capacity-used,"]%s",tail);
            fputc('\r',stderr); print_padded(row,cols); free(row);
        }
    }
    fflush(stderr);
    p->last=now; p->last_bytes=done;
}
static void ui_begin(void) {
    if (ui_tui && isatty(STDERR_FILENO)) {
        ui_active=1; fprintf(stderr,"\033[?1049h\033[?25l\033[H");
        print_tui_rule('T','T');
        print_tui_line("DiskMedic — MONITEUR TERMINAL");
        char row[PATH_MAX + 128];
        snprintf(row,sizeof(row),"Disque : %s",ui_device); print_tui_line(row);
        print_tui_line("Interface sans ncurses (séquences ANSI).");
        print_tui_rule('B','B');
        fputs("\033[J", stderr);
        fflush(stderr);
    } else ui_tui=0;
}
static void ui_status(const char *phase, const char *detail) {
    if (worker_slot) { worker_publish(phase, 0, 0, 0, 0, 0); return; }
    if (!ui_tui) return;
    char row[PATH_MAX + 128];
    fputs("\033[H",stderr);
    print_tui_rule('T','T');
    print_tui_line("DiskMedic · MONITEUR");
    print_tui_rule('M','M');
    snprintf(row,sizeof(row),"Disque : %s",ui_device); print_tui_line(row);
    snprintf(row,sizeof(row),"Étape : %s",phase); print_tui_line(row);
    snprintf(row,sizeof(row),"Info : %s",detail); print_tui_line(row);
    print_tui_rule('B','B');
    fputs("\033[J",stderr);
    fflush(stderr);
}
static void ui_end(void) {
    if (ui_active) { fprintf(stderr,"\033[?25h\033[?1049l\n"); fflush(stderr); ui_active=0; }
}
static int confirm_mutation(void) {
    char line[64];
    fprintf(stderr, "ATTENTION : cette opération peut détruire irrémédiablement toutes les données.\nTapez exactement OUI pour continuer : ");
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    line[strcspn(line, "\r\n")] = 0;
    return strcmp(line, "OUI") == 0;
}
static int get_size(int fd, uint64_t *size) {
    unsigned long long n = 0;
    if (ioctl(fd, BLKGETSIZE64, &n) < 0) { perror("BLKGETSIZE64"); return -1; }
    *size = (uint64_t)n; return 0;
}
/* Tailles de secteur logique (adressage LBA) et physique (unité d'écriture réelle du disque). */
static void load_sector_sizes(int fd) {
    int ls = 0;
    unsigned int ps = 0;
    log_sector_size = 512;
    if (ioctl(fd, BLKSSZGET, &ls) == 0 && ls > 0) log_sector_size = (unsigned)ls;
    phys_sector_size = log_sector_size;
    if (ioctl(fd, BLKPBSZGET, &ps) == 0 && ps >= log_sector_size && ps % log_sector_size == 0 &&
        ps <= BUF_SIZE && (ps & (ps - 1)) == 0)
        phys_sector_size = ps;
}
static int full_pread(int fd, void *buf, size_t len, off_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (char *)buf + done, len - done, off + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}
static int full_pwrite(int fd, const void *buf, size_t len, off_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, (const char *)buf + done, len - done, off + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}
/*
 * Accès disque : un second descripteur O_DIRECT contourne le cache de pages pour
 * que les lectures/temps mesurés reflètent le support physique. Les accès non
 * alignés sur le secteur logique (fin de disque) passent par le descripteur tamponné.
 */
typedef struct { int fd, dfd; } disk_io_t;
static void dio_open(disk_io_t *io, int fd, int writable) {
    io->fd = fd; io->dfd = -1;
#ifdef O_DIRECT
    io->dfd = open(ui_device, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_DIRECT);
#else
    (void)writable;
#endif
}
static void dio_close(disk_io_t *io) { if (io->dfd >= 0) close(io->dfd); io->dfd = -1; }
static int dio_pick(const disk_io_t *io, uint64_t off, size_t n) {
    return (io->dfd >= 0 && off % log_sector_size == 0 && n % log_sector_size == 0) ? io->dfd : io->fd;
}
static int dio_pread(disk_io_t *io, void *buf, size_t n, uint64_t off) {
    int f = dio_pick(io, off, n);
    if (full_pread(f, buf, n, (off_t)off) == 0) return 0;
    if (f == io->dfd && errno == EINVAL) { dio_close(io); return full_pread(io->fd, buf, n, (off_t)off); }
    return -1;
}
static int dio_pwrite(disk_io_t *io, const void *buf, size_t n, uint64_t off) {
    int f = dio_pick(io, off, n);
    if (full_pwrite(f, buf, n, (off_t)off) == 0) return 0;
    if (f == io->dfd && (errno == EINVAL || errno == EOPNOTSUPP)) { dio_close(io); return full_pwrite(io->fd, buf, n, (off_t)off); }
    return -1;
}
/* Tampon aligné pour O_DIRECT (au moins 4096 et au moins la taille de secteur logique). */
static unsigned char *alloc_aligned(size_t n) {
    void *p = NULL;
    size_t a = log_sector_size > 4096U ? log_sector_size : 4096U;
    return posix_memalign(&p, a, n) ? NULL : (unsigned char *)p;
}
/* Écarte du cache de pages les données tamponnées pour forcer une relecture physique. */
static void drop_cache(int fd, uint64_t off, uint64_t len) {
    (void)posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_DONTNEED);
}

/* Revendique le disque en exclusivité (O_EXCL) : échoue s'il est monté ou utilisé (swap, RAID, LVM…). */
static int claim_device(const char *dev) {
    int fd = open(dev, O_RDONLY | O_EXCL | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EBUSY)
            fprintf(stderr, "Refus : %s est en cours d’utilisation (partition montée, swap, RAID/LVM…). "
                            "Démontez-le avant toute opération destructive.\n", dev);
        else perror(dev);
    }
    return fd;
}

static int zero_disk(int fd, uint64_t size) {
    disk_io_t io; dio_open(&io, fd, 1);
    unsigned char *buf = alloc_aligned(BUF_SIZE), *verify = alloc_aligned(BUF_SIZE);
    int rc = -1;
    if (!buf || !verify) { perror("allocation alignée"); goto out; }
    memset(buf, 0, BUF_SIZE);
    fprintf(stderr, "Écriture de zéros sur %" PRIu64 " octets… (%s)\n", size, io.dfd >= 0 ? "O_DIRECT" : "E/S tamponnée");
    progress_t progress; progress_begin(&progress, "Écriture", size);
    for (uint64_t off = 0; off < size;) {
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        if (dio_pwrite(&io, buf, n, off) < 0) {
            fprintf(stderr, "\nÉchec écriture à 0x%" PRIx64 ": %s\n", off, strerror(errno)); goto out;
        }
        off += n;
        if ((off % (16ULL * 1024 * 1024) < BUF_SIZE) || off == size)
            progress_show(&progress, "Écriture", off, size);
    }
    fprintf(stderr, "\nSynchronisation et vérification…\n");
    if (fdatasync(fd) < 0) { perror("fdatasync"); goto out; }
    drop_cache(fd, 0, 0);
    progress_begin(&progress, "Vérification", size);
    for (uint64_t off = 0; off < size;) {
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        if (dio_pread(&io, verify, n, off) < 0) {
            fprintf(stderr, "\nÉchec relecture à 0x%" PRIx64 ": %s\n", off, strerror(errno)); goto out;
        }
        for (size_t i = 0; i < n; i++) if (verify[i] != 0) {
            fprintf(stderr, "\nVérification échouée : donnée non nulle à 0x%" PRIx64 "\n", off + i); goto out;
        }
        off += n;
        if ((off % (16ULL * 1024 * 1024) < BUF_SIZE) || off == size)
            progress_show(&progress, "Vérification", off, size);
    }
    fprintf(stderr, "\nZéro + vérification terminés.\n");
    rc = 0;
out:
    free(buf); free(verify); dio_close(&io); return rc;
}

static int csv_field(const char *value) {
    if (fputc('"', log_file) == EOF) return -1;
    for (const char *p = value; *p; p++) {
        if (*p == '"' && fputc('"', log_file) == EOF) return -1;
        if (fputc((unsigned char)*p, log_file) == EOF) return -1;
    }
    return fputc('"', log_file) == EOF ? -1 : 0;
}
static int log_event(const char *operation, uint64_t lba, const char *status, const char *action) {
    if (!log_file) return 0;
    struct timespec now; struct tm utc; char stamp[40];
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || !gmtime_r(&now.tv_sec, &utc) ||
        !strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &utc)) goto fail;
    char timestamp[64], lba_text[32];
    snprintf(timestamp, sizeof(timestamp), "%s.%03luZ", stamp,
             (unsigned long)(now.tv_nsec / 1000000L));
    snprintf(lba_text, sizeof(lba_text), "%" PRIu64, lba);
    if (csv_field(timestamp) < 0 || fputc(',', log_file) == EOF ||
        csv_field(ui_device) < 0 || fputc(',', log_file) == EOF ||
        csv_field(operation) < 0 || fputc(',', log_file) == EOF ||
        csv_field(lba_text) < 0 || fputc(',', log_file) == EOF ||
        csv_field(status) < 0 || fputc(',', log_file) == EOF ||
        csv_field(action) < 0 || fputc('\n', log_file) == EOF || fflush(log_file) != 0) goto fail;
    log_entries++; return 0;
fail:
    log_failed = 1;
    fprintf(stderr, "Erreur d’écriture du journal CSV: %s\n", strerror(errno ? errno : EIO));
    return -1;
}
static int open_log(const char *path) {
    log_file = fopen(path, "a+");
    if (!log_file) { perror(path); return -1; }
    if (fseek(log_file, 0, SEEK_END) != 0) { perror("positionnement du journal"); fclose(log_file); log_file=NULL; return -1; }
    long end = ftell(log_file);
    if (end < 0) { perror("taille du journal"); fclose(log_file); log_file=NULL; return -1; }
    if (end == 0) {
        if (fputs("horodatage_iso8601,périphérique,type_operation,LBA,état,action\n", log_file) == EOF || fflush(log_file) != 0) {
            perror("en-tête journal CSV"); fclose(log_file); log_file=NULL; return -1;
        }
    }
    log_path_active = path;
    fprintf(stderr, "Journal CSV : %s\n", path);
    return 0;
}
static void close_log(void) {
    if (!log_file) return;
    fprintf(stderr, "Journal CSV terminé : %" PRIu64 " entrée(s) écrite(s) dans %s%s\n",
            log_entries, log_path_active ? log_path_active : "(inconnu)", log_failed ? " (erreur d’écriture)" : "");
    if (fclose(log_file) != 0) { perror("fermeture journal CSV"); log_failed=1; }
    log_file=NULL;
}

/* Enregistre un secteur logique illisible (zéros dans buf, badmap, message, journal). */
static int mark_bad_sector(unsigned char *buf, size_t s, size_t len, uint64_t off, int err,
                           const char *operation, const char *action, int log, unsigned char *badmap) {
    const size_t ss = log_sector_size;
    memset(buf + s, 0, len);
    if (badmap) badmap[(s / ss) / 8] |= (unsigned char)(1U << ((s / ss) % 8));
    if (log) worker_add_bad();
    fprintf(stderr, "Secteur illisible : LBA %" PRIu64 " (%s)\n", (off + s) / ss, strerror(err));
    if (log && log_event(operation, (off + s) / ss, "DEFECTUEUX: lecture impossible", action) < 0) return -1;
    return 0;
}

/*
 * Relit un bloc dont la lecture globale a échoué, afin de localiser les LBA exacts.
 * Les secteurs illisibles sont remplis de zéros dans buf; si badmap n'est pas NULL,
 * le bit i est mis à 1 pour chaque secteur logique i illisible du bloc.
 *
 * Le disque lit et corrige toujours un secteur PHYSIQUE entier, et chaque lecture en
 * échec peut lui coûter plusieurs secondes de nouvelles tentatives. La relecture se fait
 * donc d'abord par secteur physique ; seul un secteur physique en échec est examiné
 * secteur logique par secteur logique (512e). Si son premier ET son dernier secteur
 * logique sont illisibles, tout le secteur physique est considéré comme perdu sans
 * relire les autres : 3 lectures en échec au lieu de 8 sur un disque 512e.
 *
 * Renvoie le nombre de secteurs logiques illisibles, ou UINT64_MAX si le journal échoue.
 */
static uint64_t read_sectors(disk_io_t *io, unsigned char *buf, size_t n, uint64_t off,
                             const char *operation, const char *action, int log, unsigned char *badmap) {
    uint64_t bad = 0;
    const size_t ss = log_sector_size, ps = phys_sector_size;
    for (size_t p = 0; p < n; p += ps) {
        size_t plen = n - p < ps ? n - p : ps;
        if (dio_pread(io, buf + p, plen, off + p) == 0) continue;
        int err = errno;
        if (plen <= ss) {
            bad++;
            if (mark_bad_sector(buf, p, plen, off, err, operation, action, log, badmap) < 0) return UINT64_MAX;
            continue;
        }
        size_t last = p + ((plen - 1) / ss) * ss;
        int first_bad = dio_pread(io, buf + p, ss, off + p) < 0;
        int first_err = errno;
        int last_bad = dio_pread(io, buf + last, ss, off + last) < 0;
        int last_err = errno;
        if (first_bad && last_bad) {
            /* Secteur physique entièrement perdu : tous ses secteurs logiques le sont aussi. */
            for (size_t s = p; s < p + plen; s += ss) {
                bad++;
                if (mark_bad_sector(buf, s, ss, off, s == last ? last_err : first_err,
                                    operation, action, log, badmap) < 0) return UINT64_MAX;
            }
            continue;
        }
        for (size_t s = p; s < p + plen; s += ss) {
            int sbad, serr = 0;
            if (s == p) { sbad = first_bad; serr = first_err; }
            else if (s == last) { sbad = last_bad; serr = last_err; }
            else { sbad = dio_pread(io, buf + s, ss, off + s) < 0; serr = errno; }
            if (sbad) {
                bad++;
                if (mark_bad_sector(buf, s, ss, off, serr, operation, action, log, badmap) < 0) return UINT64_MAX;
            }
        }
    }
    return bad;
}

static int verify_disk(int fd, uint64_t size) {
    disk_io_t io; dio_open(&io, fd, 0);
    unsigned char *buf = alloc_aligned(BUF_SIZE);
    if (!buf) { perror("allocation"); dio_close(&io); return -1; }
    uint64_t bad = 0;
    progress_t progress; progress_begin(&progress, "Vérification", size);
    fprintf(stderr, "Lecture complète de vérification (%" PRIu64 " octets, %s)…\n", size, io.dfd >= 0 ? "O_DIRECT" : "E/S tamponnée");
    for (uint64_t off = 0; off < size;) {
        size_t n = size - off > BUF_SIZE ? BUF_SIZE : (size_t)(size - off);
        if (dio_pread(&io, buf, n, off) < 0) {
            fprintf(stderr, "\nErreur de lecture dans le bloc à 0x%" PRIx64 " (%s); analyse secteur par secteur…\n", off, strerror(errno));
            uint64_t b = read_sectors(&io, buf, n, off, "verify", "AUCUNE", 1, NULL);
            if (b == UINT64_MAX) { free(buf); dio_close(&io); return -1; }
            bad += b;
        }
        off += n;
        if (off % (16ULL*1024*1024) < BUF_SIZE || off == size) progress_show(&progress, "Vérification", off, size);
    }
    free(buf); dio_close(&io);
    if (bad) {
        fprintf(stderr, "\nVérification complète : %" PRIu64 " secteur(s) illisible(s) détecté(s).\n", bad);
        return -1;
    }
    fprintf(stderr, "\nVérification complète : aucune erreur de lecture détectée.\n");
    return 0;
}

static char *trim(char *s);
static void ata_ident_string(const unsigned char *ident, size_t byte_offset,
                             size_t byte_length, char *out, size_t out_size);
static int scsi_vpd_serial(int fd, char *out, size_t out_size);
/* À 1 pendant les sondages (détection du type de disque) : pas de message d'erreur ATA. */
static int ata_quiet = 0;

/* Registres ATA renvoyés par la passerelle SAT (descripteur 09h ou sense fixe 00/1Dh). */
typedef struct { uint8_t error, status, count, lba_low, lba_mid, lba_high, device; int valid; } ata_regs_t;

static int parse_ata_sense(const unsigned char *s, unsigned len, ata_regs_t *r) {
    memset(r, 0, sizeof(*r));
    if (len < 8) return 0;
    uint8_t code = s[0] & 0x7f;
    if (code == 0x72 || code == 0x73) {
        for (unsigned i = 8; i + 2 < len;) {
            unsigned dlen = s[i + 1];
            if (i + 2 + dlen > len) break;
            if (s[i] == 0x09 && dlen >= 0x0c) {
                r->error = s[i + 3]; r->count = s[i + 5]; r->lba_low = s[i + 7];
                r->lba_mid = s[i + 9]; r->lba_high = s[i + 11]; r->device = s[i + 12];
                r->status = s[i + 13]; r->valid = 1;
                return 1;
            }
            i += 2 + dlen;
        }
    } else if ((code == 0x70 || code == 0x71) && len >= 14 && s[12] == 0x00 && s[13] == 0x1d) {
        /* Format fixe SAT : ASC/ASCQ 00/1D « ATA pass-through information available ». */
        r->error = s[3]; r->status = s[4]; r->device = s[5]; r->count = s[6];
        r->lba_low = s[9]; r->lba_mid = s[10]; r->lba_high = s[11]; r->valid = 1;
        return 1;
    }
    return 0;
}

/* ATA PASS-THROUGH(16), via Linux SG_IO. Data direction is explicit. */
static int ata_pt_regs(int fd, uint8_t protocol, uint8_t flags, uint8_t feature,
                       uint8_t count, uint8_t lba_low, uint8_t lba_mid, uint8_t lba_high,
                       uint8_t command, void *data, unsigned data_len,
                       int direction, unsigned timeout_ms, ata_regs_t *out) {
    unsigned char cdb[16] = {0}; unsigned char sense[64] = {0};
    /* EXTEND=0 : toutes les commandes utilisées ici sont des commandes ATA 28 bits. */
    cdb[0] = 0x85; cdb[1] = (uint8_t)((protocol & 0x0f) << 1); cdb[2] = flags;
    cdb[4] = feature; cdb[6] = count; cdb[8] = lba_low; cdb[10] = lba_mid; cdb[12] = lba_high;
    cdb[13] = 0x40; cdb[14] = command;
    sg_io_hdr_t io; memset(&io, 0, sizeof(io));
    io.interface_id = 'S'; io.cmd_len = sizeof(cdb); io.mx_sb_len = sizeof(sense);
    io.dxfer_direction = direction; io.dxfer_len = data_len; io.dxferp = data;
    io.cmdp = cdb; io.sbp = sense; io.timeout = timeout_ms;
    if (ioctl(fd, SG_IO, &io) < 0) { if (!ata_quiet) perror("ioctl(SG_IO)"); return -1; }
    /* DRIVER_SENSE (0x08) signale seulement la présence de données sense, analysées plus bas. */
    if (io.host_status || (io.driver_status & ~0x08U)) {
        if (!ata_quiet)
            fprintf(stderr, "Commande ATA 0x%02x en échec (host=0x%x, driver=0x%x).\n", command, io.host_status, io.driver_status);
        errno = EIO; return -1;
    }
    ata_regs_t regs;
    parse_ata_sense(sense, io.sb_len_wr, &regs);
    if (io.status && (!regs.valid || (regs.status & 0x21))) {
        if (!ata_quiet) {
            fprintf(stderr, "Commande ATA 0x%02x en échec (SCSI status=0x%x, ATA status=0x%02x, sense=", command, io.status, regs.status);
            for (unsigned i=0; i<io.sb_len_wr; i++) fprintf(stderr, "%02x", sense[i]);
            fprintf(stderr, ").\n");
        }
        errno = EIO; return -1;
    }
    if (out) *out = regs;
    return 0;
}
static int ata_pt(int fd, uint8_t protocol, uint8_t flags, uint8_t feature,
                  uint8_t count, uint8_t lba_mid, uint8_t lba_high,
                  uint8_t command, void *data, unsigned data_len,
                  int direction, unsigned timeout_ms) {
    return ata_pt_regs(fd, protocol, flags, feature, count, 0, lba_mid, lba_high, command,
                       data, data_len, direction, timeout_ms, NULL);
}
static uint16_t word_le(const unsigned char *p, unsigned w) { return (uint16_t)p[w*2] | ((uint16_t)p[w*2+1] << 8); }
static int identify(int fd, unsigned char data[512]) {
    memset(data, 0, 512);
    /* PIO Data-In; flags 0x0e = T_DIR (vers l'hôte) | BYT_BLOK (en blocs) | T_LENGTH=sector count. */
    return ata_pt(fd, 4, 0x0e, 0, 1, 0, 0, ATA_IDENTIFY, data, 512, SG_DXFER_FROM_DEV, 20000);
}
/* IDENTIFY sans message : sert à détecter si le disque parle ATA (sinon SAS/SCSI ou NVMe). */
static int identify_quiet(int fd, unsigned char data[512]) {
    ata_quiet = 1;
    int r = identify(fd, data);
    ata_quiet = 0;
    return r;
}
static int smart_read_data(int fd, unsigned char data[512]) {
    memset(data, 0, 512);
    /* SMART READ DATA: command B0h, feature D0h, signature LBA mid/high = 4Fh/C2h. */
    return ata_pt(fd, 4, 0x0e, SMART_READ_DATA, 1, 0x4f, 0xc2, ATA_SMART, data, 512, SG_DXFER_FROM_DEV, 30000);
}
static int smart_read_thresholds(int fd, unsigned char data[512]) {
    memset(data, 0, 512);
    return ata_pt(fd, 4, 0x0e, SMART_READ_THRESHOLDS, 1, 0x4f, 0xc2, ATA_SMART, data, 512, SG_DXFER_FROM_DEV, 30000);
}
/*
 * SMART RETURN STATUS : avis du firmware lui-même. Avec CK_COND (0x20), la passerelle
 * renvoie les registres : LBA mid/high 4Fh/C2h = OK, F4h/2Ch = seuil dépassé.
 * Renvoie 1 = OK, 0 = défaillance annoncée, -1 = inconnu.
 */
static int smart_health(int fd) {
    ata_regs_t r;
    if (ata_pt_regs(fd, 3, 0x20, SMART_RETURN_STATUS, 0, 0, 0x4f, 0xc2, ATA_SMART, NULL, 0,
                    SG_DXFER_NONE, 30000, &r) < 0 || !r.valid) return -1;
    if (r.lba_mid == 0x4f && r.lba_high == 0xc2) return 1;
    if (r.lba_mid == 0xf4 && r.lba_high == 0x2c) return 0;
    return -1;
}

/* ---------- Mise en forme lisible ---------- */

enum { SEV_OK = 0, SEV_WARN = 1, SEV_CRIT = 2 };
static int color_mode = -1;
static int color_on(void) {
    if (color_mode < 0) {
        const char *t = getenv("TERM");
        color_mode = isatty(STDOUT_FILENO) && !getenv("NO_COLOR") && !(t && !strcmp(t, "dumb"));
    }
    return color_mode;
}
static const char *sev_color(int sev) {
    if (!color_on()) return "";
    return sev == SEV_CRIT ? "\033[1;31m" : sev == SEV_WARN ? "\033[1;33m" : "\033[32m";
}
static const char *c_bold(void) { return color_on() ? "\033[1m" : ""; }
static const char *c_dim(void) { return color_on() ? "\033[2m" : ""; }
static const char *c_reset(void) { return color_on() ? "\033[0m" : ""; }

/* Écrit text sur f en occupant exactement width colonnes (UTF-8), tronqué si nécessaire. */
static void fput_padded(FILE *f, const char *text, unsigned width) {
    const unsigned char *s = (const unsigned char *)text;
    unsigned used = 0;
    while (*s && used < width) {
        size_t n = utf8_char_bytes(s);
        (void)fwrite(s, 1, n, f);
        s += n;
        used++;
    }
    while (used++ < width) fputc(' ', f);
}
/* Entier avec séparateur de milliers : 1 234 567. */
static void fmt_int(char *out, size_t n, uint64_t v) {
    char tmp[32]; int len = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    size_t o = 0;
    for (int i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ' ';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}
/* Décimal à la française : 931,5. */
static void fmt_dec(char *out, size_t n, double v, int prec) {
    snprintf(out, n, "%.*f", prec, v);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}
static void fmt_bytes(char *out, size_t n, double bytes) {
    static const char *u[] = {"o", "Ko", "Mo", "Go", "To", "Po"};
    int k = 0;
    while (bytes >= 1000.0 && k < 5) { bytes /= 1000.0; k++; }
    char num[16]; fmt_dec(num, sizeof(num), bytes, k ? 1 : 0);
    snprintf(out, n, "%s %s", num, u[k]);
}
static void fmt_hours(char *out, size_t n, uint64_t h) {
    char hs[32]; fmt_int(hs, sizeof(hs), h);
    uint64_t years = h / 8766, months = (h % 8766) / 730, days = h / 24;
    if (years) snprintf(out, n, "%s h (≈ %" PRIu64 " an%s %" PRIu64 " mois)", hs, years, years > 1 ? "s" : "", months);
    else if (months) snprintf(out, n, "%s h (≈ %" PRIu64 " mois)", hs, months);
    else snprintf(out, n, "%s h (≈ %" PRIu64 " jour%s)", hs, days, days > 1 ? "s" : "");
}

/* Liste des points d'attention, avec un conseil pour chacun. */
typedef struct { int sev; char text[320]; } issue_t;
typedef struct { issue_t item[40]; size_t n; int worst; } issues_t;
static void issue_add(issues_t *is, int sev, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void issue_add(issues_t *is, int sev, const char *fmt, ...) {
    if (sev > is->worst) is->worst = sev;
    if (is->n >= sizeof(is->item) / sizeof(is->item[0])) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(is->item[is->n].text, sizeof(is->item[is->n].text), fmt, ap);
    va_end(ap);
    is->item[is->n++].sev = sev;
}
static const char *sev_label(int sev) { return sev == SEV_CRIT ? "CRITIQUE" : sev == SEV_WARN ? "ATTENTION" : "OK"; }

static void print_issues_and_verdict(const issues_t *is, int health_known) {
    printf("\n%sPoints d'attention :%s\n", c_bold(), c_reset());
    if (!is->n) printf("  Aucun.\n");
    for (int sev = SEV_CRIT; sev >= SEV_WARN; sev--)
        for (size_t i = 0; i < is->n; i++)
            if (is->item[i].sev == sev)
                printf("  %s[%s]%s %s\n", sev_color(sev), sev_label(sev), c_reset(), is->item[i].text);
    const char *verdict = is->worst == SEV_CRIT ? "CRITIQUE" : is->worst == SEV_WARN ? "ATTENTION" : "SAIN";
    printf("\n%sVERDICT : %s%s%s\n", c_bold(), sev_color(is->worst), verdict, c_reset());
    if (is->worst == SEV_CRIT)
        printf("Conseil : sauvegardez immédiatement les données, limitez l'utilisation et prévoyez le remplacement du disque.\n");
    else if (is->worst == SEV_WARN)
        printf("Conseil : sauvegardez les données et surveillez l'évolution des compteurs signalés.\n");
    else
        printf("Conseil : conservez des sauvegardes et contrôlez SMART régulièrement; ce verdict ne garantit pas l'absence de panne.\n");
    if (!health_known)
        printf("%sNote : l'avis global du firmware (SMART RETURN STATUS) n'a pas pu être lu via cette passerelle.%s\n", c_dim(), c_reset());
}

static void print_identity(const char *model, const char *serial, const char *fw, uint64_t bytes,
                           const char *type, const char *extra) {
    char cap[160] = "inconnue", gib[32], bi[40];
    if (bytes) {
        char dec[32]; fmt_bytes(dec, sizeof(dec), (double)bytes);
        fmt_dec(gib, sizeof(gib), (double)bytes / 1073741824.0, 1); fmt_int(bi, sizeof(bi), bytes);
        snprintf(cap, sizeof(cap), "%s (%s Gio, %s octets)", dec, gib, bi);
    }
    char fmtsec[96];
    if (log_sector_size == 512 && phys_sector_size == 512) snprintf(fmtsec, sizeof(fmtsec), "512n (logique 512 o, physique 512 o)");
    else if (log_sector_size == 512 && phys_sector_size == 4096) snprintf(fmtsec, sizeof(fmtsec), "512e (logique 512 o, physique 4096 o)");
    else if (log_sector_size == 4096 && phys_sector_size == 4096) snprintf(fmtsec, sizeof(fmtsec), "4Kn (logique 4096 o, physique 4096 o)");
    else snprintf(fmtsec, sizeof(fmtsec), "logique %u o, physique %u o", log_sector_size, phys_sector_size);
    printf("%s━━ %s ━━%s\n", c_bold(), model[0] ? model : "Modèle inconnu", c_reset());
    printf("  Série     : %s\n", serial[0] ? serial : "inconnue");
    printf("  Firmware  : %s\n", fw[0] ? fw : "inconnu");
    printf("  Capacité  : %s\n", cap);
    printf("  Secteurs  : %s\n", fmtsec);
    printf("  Type      : %s\n", type);
    if (extra && extra[0]) printf("%s", extra);
}

/* ---------- SMART ATA ---------- */

typedef struct { uint8_t id; const char *name; } attr_name_t;
static const attr_name_t attr_names[] = {
    {1, "Erreurs de lecture brutes"}, {2, "Performance de débit"}, {3, "Temps de démarrage moteur"},
    {4, "Démarrages/arrêts moteur"}, {5, "Secteurs réalloués"}, {7, "Erreurs de positionnement"},
    {8, "Perf. de positionnement"}, {9, "Heures de fonctionnement"}, {10, "Relances démarrage moteur"},
    {11, "Relances de recalibrage"}, {12, "Cycles d'alimentation"}, {13, "Erreurs lecture logiciel"},
    {170, "Réserve disponible"}, {171, "Échecs de programmation"}, {172, "Échecs d'effacement"},
    {173, "Usure moyenne"}, {174, "Coupures brutales"}, {175, "Échecs de programmation"},
    {176, "Échecs d'effacement"}, {177, "Usure (nivellement)"}, {178, "Blocs de réserve utilisés"},
    {179, "Blocs de réserve utilisés"}, {180, "Blocs de réserve restants"}, {181, "Échecs de programmation"},
    {182, "Échecs d'effacement"}, {183, "Rétrogradations SATA"}, {184, "Erreurs de bout en bout"},
    {187, "Incorrigibles signalées"}, {188, "Commandes expirées"}, {189, "Écritures en vol haut"},
    {190, "Température (flux d'air)"}, {191, "Chocs détectés"}, {192, "Rétractions d'urgence"},
    {193, "Chargements des têtes"}, {194, "Température"}, {195, "Corrections ECC"},
    {196, "Événements de réallocation"}, {197, "Secteurs en attente"}, {198, "Secteurs incorrigibles"},
    {199, "Erreurs CRC (câble)"}, {200, "Erreurs d'écriture"}, {202, "Durée de vie restante"},
    {206, "Hauteur de vol"}, {220, "Décalage des plateaux"}, {222, "Heures avec têtes chargées"},
    {223, "Relances de chargement"}, {224, "Friction au chargement"}, {225, "Cycles de chargement"},
    {226, "Temps de chargement"}, {230, "Amplitude des têtes"}, {231, "Durée de vie restante"},
    {232, "Réserve disponible"}, {233, "Indicateur d'usure média"}, {234, "Données écrites (fab.)"},
    {235, "Coupures (fabricant)"}, {240, "Heures de vol des têtes"}, {241, "Total écrit (LBA)"},
    {242, "Total lu (LBA)"}, {246, "Total écrit par l'hôte"}, {247, "Pages écrites par l'hôte"},
    {248, "Pages écrites (contrôleur)"}, {249, "Écritures NAND"}, {250, "Relances de lecture"},
    {254, "Chutes détectées"},
};
static const char *attr_name(uint8_t id) {
    for (size_t i = 0; i < sizeof(attr_names) / sizeof(attr_names[0]); i++)
        if (attr_names[i].id == id) return attr_names[i].name;
    return "Attribut fabricant";
}
static uint64_t attr_raw(const unsigned char *a) {
    uint64_t raw = 0;
    for (int k = 5; k >= 0; k--) raw = (raw << 8) | a[5 + k];
    return raw;
}
/* Température : octet 0 = °C courant; souvent octets 2 et 4 = min/max. */
static int decode_temp(uint64_t raw, int *mn, int *mx) {
    int t = (int)(raw & 0xff), lo = (int)((raw >> 16) & 0xff), hi = (int)((raw >> 32) & 0xff);
    *mn = *mx = -1;
    if (lo > 0 && lo <= t && t <= hi && hi < 128) { *mn = lo; *mx = hi; }
    return t;
}

/*
 * Affiche le rapport SMART ATA à partir des tampons bruts (IDENTIFY, SMART READ DATA,
 * seuils facultatifs, avis du firmware). Séparé des E/S pour pouvoir être testé.
 */
static int smart_print_ata(const unsigned char id[512], const unsigned char data[512],
                           const unsigned char *thr, int health, uint64_t bytes) {
    issues_t is; memset(&is, 0, sizeof(is));
    char model[41], serial[21], fw[9];
    ata_ident_string(id, 54, 40, model, sizeof(model));
    ata_ident_string(id, 20, 20, serial, sizeof(serial));
    ata_ident_string(id, 46, 8, fw, sizeof(fw));
    if (!bytes) {
        uint64_t lba48 = (uint64_t)word_le(id, 100) | ((uint64_t)word_le(id, 101) << 16) |
                         ((uint64_t)word_le(id, 102) << 32) | ((uint64_t)word_le(id, 103) << 48);
        bytes = lba48 * log_sector_size;
    }
    /* Type : mot 217 (vitesse de rotation), liaison SATA : mots 76/77. */
    uint16_t rot = word_le(id, 217), sata = word_le(id, 76), sata_cur = word_le(id, 77);
    int is_ssd = rot == 1, is_hdd = rot >= 0x0401 && rot <= 0xfffe;
    char type[128], link[64] = "";
    if (sata && sata != 0xffff) {
        const char *mx = (sata & 8) ? "6 Gb/s" : (sata & 4) ? "3 Gb/s" : (sata & 2) ? "1,5 Gb/s" : NULL;
        unsigned cur = (sata_cur >> 1) & 7;
        const char *cs = cur == 3 ? "6 Gb/s" : cur == 2 ? "3 Gb/s" : cur == 1 ? "1,5 Gb/s" : NULL;
        if (mx && cs) snprintf(link, sizeof(link), " · SATA %s (lien actuel %s)", mx, cs);
        else if (mx) snprintf(link, sizeof(link), " · SATA %s", mx);
        if (mx && cs && strcmp(mx, cs))
            issue_add(&is, SEV_WARN, "Liaison SATA négociée à %s alors que le disque supporte %s : vérifiez le câble et le port SATA.", cs, mx);
    }
    if (is_ssd) snprintf(type, sizeof(type), "SSD%s", link);
    else if (is_hdd) snprintf(type, sizeof(type), "Disque dur %u tr/min%s", rot, link);
    else snprintf(type, sizeof(type), "non précisé%s", link);
    uint16_t sec = word_le(id, 128);
    char extra[256];
    if (sec & 1)
        snprintf(extra, sizeof(extra), "  Sécurité  : ATA Security pris en charge · %s · %s%s%s\n",
                 sec & 2 ? "ACTIVÉE" : "non activée", sec & 8 ? "GELÉE (frozen)" : "non gelée",
                 sec & 4 ? " · VERROUILLÉE" : "", sec & 0x20 ? " · effacement enhanced disponible" : "");
    else snprintf(extra, sizeof(extra), "  Sécurité  : ATA Security non annoncé\n");
    print_identity(model, serial, fw, bytes, type, extra);
    if (sec & 4) issue_add(&is, SEV_WARN, "Le disque est verrouillé (ATA Security) : ses données sont inaccessibles sans mot de passe.");

    /* Avis du firmware. */
    printf("\n%sSanté (avis du firmware) :%s %s%s%s\n", c_bold(), c_reset(),
           health == 1 ? sev_color(SEV_OK) : health == 0 ? sev_color(SEV_CRIT) : c_dim(),
           health == 1 ? "OK" : health == 0 ? "DÉFAILLANCE ANNONCÉE" : "inconnue", c_reset());
    if (health == 0)
        issue_add(&is, SEV_CRIT, "Le firmware annonce lui-même une défaillance (un attribut a franchi son seuil) : sauvegardez immédiatement.");

    /* Attributs. */
    int temp = -1, tmin = -1, tmax = -1, life = -1, life_id = -1, has194 = 0;
    uint64_t hours = 0, cycles = 0, written = 0;
    int have_hours = 0, have_cycles = 0, have_written = 0;
    for (unsigned i = 0; i < 30; i++) {
        const unsigned char *a = data + 2 + i * 12;
        if (a[0] == 194) has194 = 1;
    }
    for (unsigned i = 0; i < 30; i++) {
        const unsigned char *a = data + 2 + i * 12;
        uint64_t raw = attr_raw(a);
        if (a[0] == 9) { hours = raw & 0xffffffffULL; have_hours = 1; }
        else if (a[0] == 12) { cycles = raw; have_cycles = 1; }
        else if (a[0] == 194 || (a[0] == 190 && !has194)) temp = decode_temp(raw, &tmin, &tmax);
        else if ((a[0] == 241 || (a[0] == 246 && !have_written)) && raw) { written = raw * 512U; have_written = 1; }
        if (is_ssd && life < 0 && (a[0] == 231 || a[0] == 233 || a[0] == 177 || a[0] == 202) && a[3] && a[3] <= 100) {
            life = a[3]; life_id = a[0];
        }
    }
    char summary[512] = "", part[128];
    if (temp >= 0) {
        if (tmin >= 0) snprintf(part, sizeof(part), "Température %d °C (min %d / max %d)", temp, tmin, tmax);
        else snprintf(part, sizeof(part), "Température %d °C", temp);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (have_hours) {
        char h[96]; fmt_hours(h, sizeof(h), hours);
        snprintf(part, sizeof(part), "%sAllumé %s", summary[0] ? " · " : "", h);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (have_cycles) {
        char c[32]; fmt_int(c, sizeof(c), cycles);
        snprintf(part, sizeof(part), "%s%s démarrages", summary[0] ? " · " : "", c);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (have_written) {
        char w[32]; fmt_bytes(w, sizeof(w), (double)written);
        snprintf(part, sizeof(part), "%sÉcrit ≈ %s*", summary[0] ? " · " : "", w);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (life >= 0) {
        snprintf(part, sizeof(part), "%sVie restante ≈ %d %%", summary[0] ? " · " : "", life);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (summary[0]) printf("%s\n", summary);

    printf("\n%s  ID  ", c_bold()); fput_padded(stdout, "Attribut", 27);
    printf("Val. Pire Seuil "); fput_padded(stdout, "Brut décodé", 22); printf("État%s\n", c_reset());
    for (unsigned i = 0; i < 30; i++) {
        const unsigned char *a = data + 2 + i * 12;
        if (!a[0]) continue;
        uint8_t aid = a[0], cur = a[3], worst = a[4];
        int t = -1;
        if (thr) for (unsigned j = 0; j < 30; j++) if (thr[2 + j * 12] == aid) { t = thr[2 + j * 12 + 1]; break; }
        uint64_t raw = attr_raw(a);
        char dec[112], rs[32];
        fmt_int(rs, sizeof(rs), raw);
        snprintf(dec, sizeof(dec), "%s", rs);
        int sev = SEV_OK;
        const char *state = "OK";
        const char *name = attr_name(aid);
        /* Seuils normalisés : valeur ≤ seuil = échec actuel; pire ≤ seuil = échec passé. */
        if (t > 0 && t < 0xfe && cur <= t) {
            sev = SEV_CRIT; state = "ÉCHEC";
            issue_add(&is, SEV_CRIT, "ID %u « %s » est sous son seuil de défaillance (%u ≤ %d).", aid, name, cur, t);
        } else if (t > 0 && t < 0xfe && worst <= t) {
            sev = SEV_WARN; state = "A ÉCHOUÉ";
            issue_add(&is, SEV_WARN, "ID %u « %s » est déjà passé sous son seuil par le passé (pire %u ≤ %d).", aid, name, worst, t);
        }
        int s2 = SEV_OK;
        switch (aid) {
        case 3: snprintf(dec, sizeof(dec), "%" PRIu64 " ms", raw & 0xffff); break;
        case 9: { char h[32]; fmt_int(h, sizeof(h), raw & 0xffffffffULL); snprintf(dec, sizeof(dec), "%s h", h); } break;
        case 190: case 194: {
            int mn, mx, tc = decode_temp(raw, &mn, &mx);
            if (mn >= 0) snprintf(dec, sizeof(dec), "%d °C (min %d/max %d)", tc, mn, mx);
            else snprintf(dec, sizeof(dec), "%d °C", tc);
            if (aid == 194 || !has194) {
                int warn = is_ssd ? 70 : 55, crit = is_ssd ? 80 : 60;
                if (tc >= crit) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "Température de %d °C : trop élevée, améliorez la ventilation sans attendre.", tc); }
                else if (tc >= warn) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "Température de %d °C : élevée, vérifiez la ventilation du boîtier.", tc); }
            }
        } break;
        case 241: case 242: case 246:
            if (raw) { char b[48]; fmt_bytes(b, sizeof(b), (double)raw * 512.0); snprintf(dec, sizeof(dec), "≈ %s*", b); }
            break;
        case 5:
            if (raw) {
                s2 = raw >= 100 ? SEV_CRIT : SEV_WARN;
                issue_add(&is, s2, "%s secteur(s) réalloué(s) : le disque a déjà remplacé des secteurs défectueux. "
                          "Sauvegardez et surveillez : une hausse régulière annonce une panne.", rs);
            } break;
        case 196:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s événement(s) de réallocation enregistrés.", rs); }
            break;
        case 197:
            if (raw) {
                s2 = SEV_CRIT;
                issue_add(&is, SEV_CRIT, "%s secteur(s) en attente (illisibles) : sauvegardez, puis lancez --scan-slow et "
                          "--slow-repair pour forcer leur réécriture/réallocation, et recontrôlez SMART.", rs);
            } break;
        case 198:
            if (raw) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "%s secteur(s) incorrigible(s) : des données sont déjà perdues à ces endroits.", rs); }
            break;
        case 187:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s erreur(s) incorrigible(s) signalée(s) à l'ordinateur.", rs); }
            break;
        case 184:
            if (raw) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "%s erreur(s) de bout en bout : corruption interne du disque (cache/contrôleur).", rs); }
            break;
        case 10:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s relance(s) de démarrage moteur : moteur ou alimentation en difficulté.", rs); }
            break;
        case 199:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s erreur(s) CRC : problème de câble ou de connecteur SATA (pas d'usure du disque). "
                                       "Changez le câble; si le compteur ne bouge plus ensuite, c'est réglé.", rs); }
            break;
        case 177: case 202: case 231: case 233:
            if (is_ssd && cur && cur <= 100) {
                snprintf(dec, sizeof(dec), "%s (vie ≈ %u %%)", rs, cur);
                if (aid == life_id) {
                    if (cur <= 10) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "SSD presque usé (vie restante ≈ %u %%) : prévoyez son remplacement.", cur); }
                    else if (cur <= 30) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "SSD bien usé (vie restante ≈ %u %%).", cur); }
                }
            } break;
        default: break;
        }
        if (s2 > sev) { sev = s2; state = sev_label(s2); }
        char ts[8]; if (t >= 0) snprintf(ts, sizeof(ts), "%d", t); else snprintf(ts, sizeof(ts), "-");
        printf("  %3u ", aid); fput_padded(stdout, name, 27);
        printf("%4u %4u %5s ", cur, worst, ts); fput_padded(stdout, dec, 22);
        printf("%s%s%s\n", sev_color(sev), state, c_reset());
    }
    if (have_written || strstr(summary, "*"))
        printf("%s* unité supposée : secteur de 512 o (certains fabricants comptent autrement).%s\n", c_dim(), c_reset());
    if (!thr) printf("%sSeuils SMART non lisibles : colonnes « Seuil » indisponibles.%s\n", c_dim(), c_reset());
    print_issues_and_verdict(&is, health >= 0);
    return is.worst;
}

/* ---------- SMART NVMe ---------- */

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t le64(const unsigned char *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void nvme_string(const unsigned char *p, size_t len, char *out, size_t out_size) {
    size_t n = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, p, n); out[n] = '\0';
    trim(out);
}
/* Commande d'administration NVMe; *status reçoit le code de statut NVMe (> 0) ou le retour de l'ioctl. */
static int nvme_admin_full(int fd, uint8_t opcode, uint32_t nsid, uint32_t cdw10, void *buf, uint32_t len,
                           uint32_t timeout_ms, int *status) {
    struct nvme_admin_cmd c;
    memset(&c, 0, sizeof(c));
    c.opcode = opcode; c.nsid = nsid; c.addr = (uint64_t)(uintptr_t)buf; c.data_len = len;
    c.cdw10 = cdw10; c.timeout_ms = timeout_ms;
    int r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c);
    if (status) *status = r;
    if (r > 0) { errno = EIO; return -1; }  /* r > 0 : code de statut NVMe */
    return r;
}
static int nvme_admin(int fd, uint8_t opcode, uint32_t nsid, uint32_t cdw10, void *buf, uint32_t len) {
    return nvme_admin_full(fd, opcode, nsid, cdw10, buf, len, 30000, NULL);
}
/* Affiche le rapport NVMe (Identify Controller facultatif + journal SMART/Health 02h). */
static int nvme_print(const unsigned char *ctrl, const unsigned char log[512], uint64_t bytes) {
    issues_t is; memset(&is, 0, sizeof(is));
    char model[41] = "", serial[21] = "", fw[9] = "";
    int wctemp = 0, cctemp = 0;
    if (ctrl) {
        nvme_string(ctrl + 24, 40, model, sizeof(model));
        nvme_string(ctrl + 4, 20, serial, sizeof(serial));
        nvme_string(ctrl + 64, 8, fw, sizeof(fw));
        if (le16(ctrl + 266)) wctemp = le16(ctrl + 266) - 273;
        if (le16(ctrl + 268)) cctemp = le16(ctrl + 268) - 273;
    }
    print_identity(model, serial, fw, bytes, "SSD NVMe", NULL);
    uint8_t cw = log[0], spare = log[3], spare_thr = log[4], used = log[5];
    int temp = le16(log + 1) ? (int)le16(log + 1) - 273 : -1;
    uint64_t rd = le64(log + 32), wr = le64(log + 48), cycles = le64(log + 112), hours = le64(log + 128);
    uint64_t unsafe = le64(log + 144), media = le64(log + 160), errlog = le64(log + 176);
    uint32_t warn_min = (uint32_t)log[192] | ((uint32_t)log[193] << 8) | ((uint32_t)log[194] << 16) | ((uint32_t)log[195] << 24);
    uint32_t crit_min = (uint32_t)log[196] | ((uint32_t)log[197] << 8) | ((uint32_t)log[198] << 16) | ((uint32_t)log[199] << 24);
    int life = used >= 100 ? 0 : 100 - used;

    printf("\n%sSanté (avis du firmware) :%s %s%s%s\n", c_bold(), c_reset(),
           sev_color(cw ? SEV_CRIT : SEV_OK), cw ? "AVERTISSEMENT CRITIQUE" : "OK", c_reset());
    char h[96], rds[32], wrs[32], cyc[32];
    fmt_hours(h, sizeof(h), hours); fmt_bytes(rds, sizeof(rds), (double)rd * 512000.0);
    fmt_bytes(wrs, sizeof(wrs), (double)wr * 512000.0); fmt_int(cyc, sizeof(cyc), cycles);
    if (temp >= 0) printf("Température %d °C · ", temp);
    printf("Allumé %s · %s démarrages · Écrit %s · Vie restante ≈ %d %%\n", h, cyc, wrs, life);

    printf("\n%s  ", c_bold()); fput_padded(stdout, "Indicateur", 38); fput_padded(stdout, "Valeur", 30); printf("État%s\n", c_reset());
#define NVME_ROW(label, value, sev) do { printf("  "); fput_padded(stdout, label, 38); fput_padded(stdout, value, 30); \
        printf("%s%s%s\n", sev_color(sev), sev_label(sev), c_reset()); } while (0)
    char v[96]; int s;
    if (!cw) snprintf(v, sizeof(v), "aucun");
    else snprintf(v, sizeof(v), "%s%s%s%s%s", cw & 1 ? "réserve basse " : "", cw & 2 ? "température " : "",
                  cw & 4 ? "fiabilité dégradée " : "", cw & 8 ? "lecture seule " : "", cw & 16 ? "sauvegarde volatile " : "");
    s = cw ? SEV_CRIT : SEV_OK;
    if (cw & 1) issue_add(&is, SEV_CRIT, "Réserve de blocs sous le seuil : le SSD arrive en fin de vie.");
    if (cw & 2) issue_add(&is, SEV_CRIT, "Le SSD signale une température hors limites.");
    if (cw & 4) issue_add(&is, SEV_CRIT, "Fiabilité dégradée (erreurs média ou internes) : sauvegardez immédiatement.");
    if (cw & 8) issue_add(&is, SEV_CRIT, "Le SSD est passé en lecture seule pour protéger les données : copiez-les et remplacez-le.");
    if (cw & 16) issue_add(&is, SEV_CRIT, "Échec du mécanisme de sauvegarde de la mémoire volatile.");
    NVME_ROW("Avertissement critique", v, s);
    if (temp >= 0) {
        int warn = wctemp > 0 ? wctemp : 70, crit = cctemp > 0 ? cctemp : 80;
        s = temp >= crit ? SEV_CRIT : temp >= warn ? SEV_WARN : SEV_OK;
        if (wctemp > 0) snprintf(v, sizeof(v), "%d °C (alerte %d °C)", temp, wctemp); else snprintf(v, sizeof(v), "%d °C", temp);
        if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Température de %d °C : critique, le SSD va ralentir ou s'arrêter; améliorez son refroidissement.", temp);
        else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Température de %d °C : élevée, ajoutez un dissipateur ou un flux d'air.", temp);
        NVME_ROW("Température", v, s);
    }
    snprintf(v, sizeof(v), "%u %% (seuil %u %%)", spare, spare_thr);
    s = spare < spare_thr ? SEV_CRIT : (spare_thr && spare < spare_thr + 10) ? SEV_WARN : SEV_OK;
    if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Réserve de blocs à %u %%, proche du seuil de %u %%.", spare, spare_thr);
    NVME_ROW("Réserve disponible", v, s);
    snprintf(v, sizeof(v), "%u %% (vie restante ≈ %d %%)", used, life);
    s = life <= 10 ? SEV_CRIT : life <= 30 ? SEV_WARN : SEV_OK;
    if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Endurance consommée à %u %% : prévoyez le remplacement du SSD.", used);
    else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Endurance consommée à %u %%.", used);
    NVME_ROW("Usure (endurance consommée)", v, s);
    fmt_int(v, sizeof(v), media);
    s = media ? SEV_CRIT : SEV_OK;
    if (media) issue_add(&is, SEV_CRIT, "%s erreur(s) média/intégrité des données : des données ont pu être corrompues.", v);
    NVME_ROW("Erreurs média / intégrité", v, s);
    fmt_int(v, sizeof(v), errlog); NVME_ROW("Entrées du journal d'erreurs", v, SEV_OK);
    fmt_int(v, sizeof(v), unsafe); NVME_ROW("Arrêts brutaux (coupures)", v, SEV_OK);
    snprintf(v, sizeof(v), "%s / %s", rds, wrs); NVME_ROW("Données lues / écrites", v, SEV_OK);
    NVME_ROW("Heures de fonctionnement", h, SEV_OK);
    NVME_ROW("Cycles d'alimentation", cyc, SEV_OK);
    snprintf(v, sizeof(v), "%" PRIu32 " min / %" PRIu32 " min", warn_min, crit_min);
    s = crit_min ? SEV_WARN : SEV_OK;
    if (crit_min) issue_add(&is, SEV_WARN, "Le SSD a déjà passé %" PRIu32 " min au-delà de sa température critique.", crit_min);
    NVME_ROW("Temps en alerte / critique (temp.)", v, s);
#undef NVME_ROW
    print_issues_and_verdict(&is, 1);
    return is.worst;
}
static int nvme_report(int fd, uint64_t bytes) {
    unsigned char *ctrl = alloc_aligned(4096), *log = alloc_aligned(4096);
    int rc = -1;
    if (!ctrl || !log) { perror("allocation"); goto out; }
    memset(ctrl, 0, 4096); memset(log, 0, 4096);
    int have_ctrl = nvme_admin(fd, 0x06, 0, 1, ctrl, 4096) == 0;
    /* Get Log Page 02h (SMART / Health), 512 octets : NUMDL = 128 dwords - 1. */
    if (nvme_admin(fd, 0x02, 0xffffffffU, (127U << 16) | 0x02, log, 512) < 0 &&
        nvme_admin(fd, 0x02, 0, (127U << 16) | 0x02, log, 512) < 0) {
        fprintf(stderr, "Lecture du journal SMART NVMe impossible : %s\n", strerror(errno));
        goto out;
    }
    nvme_print(have_ctrl ? ctrl : NULL, log, bytes);
    rc = 0;
out:
    free(ctrl); free(log); return rc;
}

/* ---------- SMART SAS / SCSI (pages LOG SENSE) ---------- */

/*
 * Commande SCSI générique via SG_IO, sans message. Une CHECK CONDITION avec la clé
 * RECOVERED ERROR est acceptée (données valides). Renvoie le nombre d'octets reçus, ou -1.
 */
static int scsi_cmd(int fd, const unsigned char *cdb, unsigned cdb_len, unsigned char *buf, unsigned len) {
    unsigned char sense[64] = {0};
    sg_io_hdr_t io; memset(&io, 0, sizeof(io));
    if (len) memset(buf, 0, len);
    io.interface_id = 'S'; io.cmd_len = (unsigned char)cdb_len; io.mx_sb_len = sizeof(sense);
    io.dxfer_direction = len ? SG_DXFER_FROM_DEV : SG_DXFER_NONE; io.dxfer_len = len; io.dxferp = buf;
    io.cmdp = (unsigned char *)cdb; io.sbp = sense; io.timeout = 30000;
    if (ioctl(fd, SG_IO, &io) < 0) return -1;
    if (io.host_status || (io.driver_status & ~0x08U)) return -1;
    if (io.status) {
        unsigned key = io.sb_len_wr < 3 ? 0 : ((sense[0] & 0x7f) >= 0x72 ? sense[1] & 0x0f : sense[2] & 0x0f);
        if (key != 0x01) return -1;
    }
    int got = (int)len - io.resid;
    return got < 0 ? 0 : got;
}
static uint64_t be_val(const unsigned char *p, unsigned len) {
    uint64_t v = 0;
    for (unsigned i = 0; i < len && i < 8; i++) v = (v << 8) | p[i];
    return v;
}
static int scsi_log_sense(int fd, uint8_t page, unsigned char *buf, unsigned len) {
    unsigned char cdb[10] = {0x4d, 0, (uint8_t)(0x40 | page), 0, 0, 0, 0, (uint8_t)(len >> 8), (uint8_t)len, 0};
    int got = scsi_cmd(fd, cdb, sizeof(cdb), buf, len);
    if (got < 4 || (buf[0] & 0x3f) != page) return -1;
    return got;
}

typedef struct {
    char vendor[9], product[17], rev[5], serial[256];
    int rotation;                       /* -1 inconnu, 1 SSD, sinon tr/min */
    int ie_known, ie_asc, ie_ascq;      /* page 2Fh : avis du firmware */
    int temp, temp_ref;                 /* page 0Dh, °C; -1 inconnu */
    int64_t grown_defects;              /* READ DEFECT DATA, liste G; -1 inconnu */
    int64_t corrected[3], uncorrected[3], processed[3];   /* lecture, écriture, vérification */
    int64_t non_medium;                 /* page 06h */
    int64_t ss_spec, ss_acc, lu_spec, lu_acc;             /* page 0Eh */
    char manufactured[24];
    int64_t power_on_minutes;           /* page 15h */
    int endurance_used;                 /* page 11h (SSD), % ; -1 inconnu */
} sas_info_t;

static void sas_init(sas_info_t *si) {
    memset(si, 0, sizeof(*si));
    si->rotation = si->temp = si->temp_ref = si->endurance_used = -1;
    si->grown_defects = si->non_medium = si->power_on_minutes = -1;
    si->ss_spec = si->ss_acc = si->lu_spec = si->lu_acc = -1;
    for (int k = 0; k < 3; k++) si->corrected[k] = si->uncorrected[k] = si->processed[k] = -1;
}

/* Décode une page LOG SENSE (en-tête de 4 octets puis paramètres code/contrôle/longueur/valeur). */
static void sas_parse_page(sas_info_t *si, const unsigned char *pg, unsigned len) {
    if (len < 4) return;
    unsigned code = pg[0] & 0x3f, end = 4 + (((unsigned)pg[2] << 8) | pg[3]);
    if (end > len) end = len;
    for (unsigned i = 4; i + 4 <= end;) {
        unsigned pc = ((unsigned)pg[i] << 8) | pg[i + 1], l = pg[i + 3];
        const unsigned char *v = pg + i + 4;
        if (i + 4 + l > end) break;
        switch (code) {
        case 0x2f:
            if (pc == 0 && l >= 2) {
                si->ie_known = 1; si->ie_asc = v[0]; si->ie_ascq = v[1];
                if (l >= 3 && v[2] && v[2] != 0xff && si->temp < 0) si->temp = v[2];
            }
            break;
        case 0x0d:
            if (pc == 0 && l >= 2 && v[1] != 0xff) si->temp = v[1];
            else if (pc == 1 && l >= 2 && v[1] != 0xff) si->temp_ref = v[1];
            break;
        case 0x03: case 0x02: case 0x05: {
            int k = code == 0x03 ? 0 : code == 0x02 ? 1 : 2;
            if (pc == 3) si->corrected[k] = (int64_t)be_val(v, l);
            else if (pc == 5) si->processed[k] = (int64_t)be_val(v, l);
            else if (pc == 6) si->uncorrected[k] = (int64_t)be_val(v, l);
        } break;
        case 0x06:
            if (pc == 0) si->non_medium = (int64_t)be_val(v, l);
            break;
        case 0x0e:
            if (pc == 1 && l >= 6 && isdigit(v[0]) && isdigit(v[4]))
                snprintf(si->manufactured, sizeof(si->manufactured), "%.4s, semaine %.2s", (const char *)v, (const char *)v + 4);
            else if (pc == 3) si->ss_spec = (int64_t)be_val(v, l);
            else if (pc == 4) si->ss_acc = (int64_t)be_val(v, l);
            else if (pc == 5) si->lu_spec = (int64_t)be_val(v, l);
            else if (pc == 6) si->lu_acc = (int64_t)be_val(v, l);
            break;
        case 0x15:
            if (pc == 0 && l >= 4) si->power_on_minutes = (int64_t)be_val(v, 4);
            break;
        case 0x11:
            if (pc == 1 && l >= 4) si->endurance_used = v[3];
            break;
        default: break;
        }
        i += 4 + l;
    }
}
/* En-tête READ DEFECT DATA(10) : octet 1 = GLISTV (bit 3) + format, octets 2-3 = longueur. */
static void sas_parse_defects(sas_info_t *si, const unsigned char *h, unsigned len) {
    if (len < 4 || !(h[1] & 0x08)) return;
    unsigned fmt = h[1] & 7, dlen = ((unsigned)h[2] << 8) | h[3];
    unsigned dsize = fmt == 0 ? 4 : 8;
    si->grown_defects = dlen / dsize;
}

/* Rassemble identité et santé d'un disque SCSI/SAS. Renvoie -1 si ce n'est pas un disque SCSI. */
static int sas_collect(int fd, sas_info_t *si) {
    unsigned char inq[96], pg[4096];
    sas_init(si);
    unsigned char icdb[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
    if (scsi_cmd(fd, icdb, sizeof(icdb), inq, sizeof(inq)) < 36 || (inq[0] & 0x1f) != 0) return -1;
    memcpy(si->vendor, inq + 8, 8); trim(si->vendor);
    memcpy(si->product, inq + 16, 16); trim(si->product);
    memcpy(si->rev, inq + 32, 4); trim(si->rev);
    if (scsi_vpd_serial(fd, si->serial, sizeof(si->serial)) < 0) si->serial[0] = '\0';
    /* VPD B1h : vitesse de rotation (1 = support non rotatif). */
    unsigned char bcdb[6] = {0x12, 1, 0xb1, 0, 64, 0}, b1[64];
    if (scsi_cmd(fd, bcdb, sizeof(bcdb), b1, sizeof(b1)) >= 6 && b1[1] == 0xb1) {
        unsigned r = ((unsigned)b1[4] << 8) | b1[5];
        if (r == 1 || (r >= 0x0401 && r <= 0xfffe)) si->rotation = (int)r;
    }
    /* Pages LOG SENSE prises en charge (page 00h), puis lecture de celles qui nous intéressent. */
    unsigned char have[64] = {0};
    int n = scsi_log_sense(fd, 0x00, pg, sizeof(pg));
    if (n >= 4) {
        unsigned plen = ((unsigned)pg[2] << 8) | pg[3];
        for (unsigned i = 0; i < plen && 4 + i < (unsigned)n; i++) have[pg[4 + i] & 0x3f] = 1;
    }
    static const uint8_t wanted[] = {0x2f, 0x0d, 0x03, 0x02, 0x05, 0x06, 0x0e, 0x15, 0x11};
    for (size_t k = 0; k < sizeof(wanted); k++) {
        if (!have[wanted[k]]) continue;
        n = scsi_log_sense(fd, wanted[k], pg, sizeof(pg));
        if (n > 0) sas_parse_page(si, pg, (unsigned)n);
    }
    /* READ DEFECT DATA(10) : liste G (défauts apparus), format « octets depuis l'index ». */
    unsigned char dcdb[10] = {0x37, 0, 0x08 | 0x04, 0, 0, 0, 0, 0, 8, 0}, d[8];
    if (scsi_cmd(fd, dcdb, sizeof(dcdb), d, sizeof(d)) >= 4) sas_parse_defects(si, d, 4);
    return 0;
}

static void sas_row(const char *label, const char *value, int sev) {
    printf("  "); fput_padded(stdout, label, 38); fput_padded(stdout, value, 30);
    printf("%s%s%s\n", sev_color(sev), sev_label(sev), c_reset());
}

/* Affiche le rapport SAS/SCSI. Séparé des E/S pour pouvoir être testé. */
static int sas_print(const sas_info_t *si, uint64_t bytes) {
    issues_t is; memset(&is, 0, sizeof(is));
    char model[48], type[96];
    snprintf(model, sizeof(model), "%s%s%s", si->vendor, si->vendor[0] && si->product[0] ? " " : "", si->product);
    int is_ssd = si->rotation == 1;
    if (is_ssd) snprintf(type, sizeof(type), "SSD SAS/SCSI");
    else if (si->rotation > 1) snprintf(type, sizeof(type), "Disque dur SAS/SCSI %d tr/min", si->rotation);
    else snprintf(type, sizeof(type), "SAS/SCSI (type non précisé)");
    char extra[96] = "";
    if (si->manufactured[0]) snprintf(extra, sizeof(extra), "  Fabriqué  : %s\n", si->manufactured);
    print_identity(model, si->serial, si->rev, bytes, type, extra);

    int known = si->ie_known || si->temp >= 0 || si->grown_defects >= 0 || si->non_medium >= 0 ||
                si->uncorrected[0] >= 0 || si->uncorrected[1] >= 0 || si->power_on_minutes >= 0 || si->endurance_used >= 0;
    int failing = si->ie_known && si->ie_asc != 0;
    printf("\n%sSanté (avis du firmware) :%s %s%s%s\n", c_bold(), c_reset(),
           !si->ie_known ? c_dim() : sev_color(failing ? SEV_CRIT : SEV_OK),
           !si->ie_known ? "inconnue" : failing ? "DÉFAILLANCE ANNONCÉE" : "OK", c_reset());
    if (!known) {
        printf("\nAucune donnée de santé lisible (passerelle USB ou contrôleur qui ne transmet pas les pages SCSI).\n");
        printf("\n%sVERDICT : INCONNU%s\n", c_bold(), c_reset());
        return SEV_OK;
    }
    /* Résumé. */
    char summary[384] = "", part[128];
    if (si->temp >= 0) {
        snprintf(part, sizeof(part), "Température %d °C", si->temp);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->power_on_minutes >= 0) {
        char h[96]; fmt_hours(h, sizeof(h), (uint64_t)si->power_on_minutes / 60);
        snprintf(part, sizeof(part), "%sAllumé %s", summary[0] ? " · " : "", h);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->ss_acc >= 0) {
        char c[32]; fmt_int(c, sizeof(c), (uint64_t)si->ss_acc);
        snprintf(part, sizeof(part), "%s%s démarrages", summary[0] ? " · " : "", c);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->processed[1] >= 0) {
        char w[32]; fmt_bytes(w, sizeof(w), (double)si->processed[1]);
        snprintf(part, sizeof(part), "%sÉcrit %s", summary[0] ? " · " : "", w);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->endurance_used >= 0) {
        snprintf(part, sizeof(part), "%sVie restante ≈ %d %%", summary[0] ? " · " : "", si->endurance_used >= 100 ? 0 : 100 - si->endurance_used);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (summary[0]) printf("%s\n", summary);

    printf("\n%s  ", c_bold()); fput_padded(stdout, "Indicateur", 38); fput_padded(stdout, "Valeur", 30); printf("État%s\n", c_reset());
    char v[96]; int s;
    if (si->ie_known) {
        if (!failing) snprintf(v, sizeof(v), "aucun");
        else snprintf(v, sizeof(v), "ASC %02Xh / ASCQ %02Xh", si->ie_asc, si->ie_ascq);
        if (failing)
            issue_add(&is, SEV_CRIT, "Le disque annonce lui-même une défaillance prochaine (ASC %02Xh/%02Xh) : sauvegardez immédiatement.",
                      si->ie_asc, si->ie_ascq);
        sas_row("Avertissement du firmware", v, failing ? SEV_CRIT : SEV_OK);
    }
    if (si->temp >= 0) {
        int warn = is_ssd ? 70 : 55, crit = is_ssd ? 80 : 60;
        if (si->temp_ref > 0) { crit = si->temp_ref; if (warn >= crit) warn = crit - 5; }
        s = si->temp >= crit ? SEV_CRIT : si->temp >= warn ? SEV_WARN : SEV_OK;
        if (si->temp_ref > 0) snprintf(v, sizeof(v), "%d °C (max. %d °C)", si->temp, si->temp_ref);
        else snprintf(v, sizeof(v), "%d °C", si->temp);
        if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Température de %d °C : trop élevée, améliorez la ventilation sans attendre.", si->temp);
        else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Température de %d °C : élevée, vérifiez la ventilation.", si->temp);
        sas_row("Température", v, s);
    }
    if (si->grown_defects >= 0) {
        fmt_int(v, sizeof(v), (uint64_t)si->grown_defects);
        s = si->grown_defects >= 100 ? SEV_CRIT : si->grown_defects > 0 ? SEV_WARN : SEV_OK;
        if (s != SEV_OK)
            issue_add(&is, s, "%s défaut(s) apparu(s) (liste G, équivalent des secteurs réalloués) : sauvegardez et "
                      "surveillez, une hausse régulière annonce une panne.", v);
        sas_row("Défauts apparus (secteurs réalloués)", v, s);
    }
    static const char *ops[3] = {"lecture", "écriture", "vérification"};
    for (int k = 0; k < 3; k++) {
        if (si->corrected[k] < 0 && si->uncorrected[k] < 0) continue;
        static const char *labels[3] = {"Lecture : erreurs corr. / non corr.", "Écriture : erreurs corr. / non corr.",
                                        "Vérif. : erreurs corr. / non corr."};
        char c[32] = "-", u[32] = "-";
        const char *label = labels[k];
        if (si->corrected[k] >= 0) fmt_int(c, sizeof(c), (uint64_t)si->corrected[k]);
        if (si->uncorrected[k] >= 0) fmt_int(u, sizeof(u), (uint64_t)si->uncorrected[k]);
        snprintf(v, sizeof(v), "%s / %s", c, u);
        s = si->uncorrected[k] > 0 ? (k == 2 ? SEV_WARN : SEV_CRIT) : SEV_OK;
        if (si->uncorrected[k] > 0)
            issue_add(&is, s, "%s erreur(s) de %s non corrigée(s) : des données %s.", u, ops[k],
                      k == 0 ? "n'ont pas pu être relues" : k == 1 ? "n'ont pas pu être écrites" : "n'ont pas pu être vérifiées");
        sas_row(label, v, s);
    }
    if (si->non_medium >= 0) { fmt_int(v, sizeof(v), (uint64_t)si->non_medium); sas_row("Erreurs hors support (liaison, etc.)", v, SEV_OK); }
    if (si->processed[0] >= 0 || si->processed[1] >= 0) {
        char r[32] = "-", w[32] = "-";
        if (si->processed[0] >= 0) fmt_bytes(r, sizeof(r), (double)si->processed[0]);
        if (si->processed[1] >= 0) fmt_bytes(w, sizeof(w), (double)si->processed[1]);
        snprintf(v, sizeof(v), "%s / %s", r, w);
        sas_row("Données lues / écrites", v, SEV_OK);
    }
    if (si->power_on_minutes >= 0) {
        fmt_hours(v, sizeof(v), (uint64_t)si->power_on_minutes / 60);
        sas_row("Heures de fonctionnement", v, SEV_OK);
    }
    const int64_t accs[2] = {si->ss_acc, si->lu_acc}, specs[2] = {si->ss_spec, si->lu_spec};
    static const char *cyc[2] = {"Démarrages/arrêts (sur durée prévue)", "Chargements des têtes (sur prévus)"};
    static const char *cyc_txt[2] = {"démarrages/arrêts", "chargements des têtes"};
    for (int k = 0; k < 2; k++) {
        if (accs[k] < 0) continue;
        char a[32], b[32]; fmt_int(a, sizeof(a), (uint64_t)accs[k]);
        s = SEV_OK;
        if (specs[k] > 0) {
            fmt_int(b, sizeof(b), (uint64_t)specs[k]);
            int pct = (int)((double)accs[k] * 100.0 / (double)specs[k]);
            snprintf(v, sizeof(v), "%s / %s (%d %%)", a, b, pct);
            s = pct >= 100 ? SEV_CRIT : pct >= 90 ? SEV_WARN : SEV_OK;
            if (s != SEV_OK) issue_add(&is, s, "%d %% des %s prévus par le fabricant sont consommés.", pct, cyc_txt[k]);
        } else snprintf(v, sizeof(v), "%s", a);
        sas_row(cyc[k], v, s);
    }
    if (si->endurance_used >= 0) {
        int life = si->endurance_used >= 100 ? 0 : 100 - si->endurance_used;
        snprintf(v, sizeof(v), "%d %% (vie restante ≈ %d %%)", si->endurance_used, life);
        s = life <= 10 ? SEV_CRIT : life <= 30 ? SEV_WARN : SEV_OK;
        if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Endurance consommée à %d %% : prévoyez le remplacement du SSD.", si->endurance_used);
        else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Endurance consommée à %d %%.", si->endurance_used);
        sas_row("Usure (endurance consommée)", v, s);
    }
    if (si->grown_defects < 0)
        printf("%sListe des défauts apparus non disponible sur ce disque.%s\n", c_dim(), c_reset());
    print_issues_and_verdict(&is, si->ie_known);
    return is.worst;
}

/* ---------- Fiches courtes pour l'en-tête du menu TUI ---------- */

#define CARD_LINES 6
typedef struct { char line[CARD_LINES][192]; int n; } card_t;
static void card_add(card_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void card_add(card_t *c, const char *fmt, ...) {
    if (c->n >= CARD_LINES) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->line[c->n], sizeof(c->line[c->n]), fmt, ap);
    va_end(ap);
    c->n++;
}
/* Fiche NVMe : Identify Controller + journal SMART/Health. */
static int nvme_card(int fd, card_t *c) {
    unsigned char *ctrl = alloc_aligned(4096), *log = alloc_aligned(4096);
    int rc = -1;
    memset(c, 0, sizeof(*c));
    if (!ctrl || !log) goto out;
    memset(ctrl, 0, 4096); memset(log, 0, 4096);
    if (nvme_admin(fd, 0x06, 0, 1, ctrl, 4096) < 0) goto out;
    char model[41], serial[21], fw[9];
    nvme_string(ctrl + 24, 40, model, sizeof(model));
    nvme_string(ctrl + 4, 20, serial, sizeof(serial));
    nvme_string(ctrl + 64, 8, fw, sizeof(fw));
    card_add(c, "Modèle : %s", model);
    card_add(c, "Série : %s | Firmware : %s", serial, fw);
    card_add(c, "Type : SSD NVMe · namespace %d", ioctl(fd, NVME_IOCTL_ID));
    if (nvme_admin(fd, 0x02, 0xffffffffU, (127U << 16) | 0x02, log, 512) == 0 ||
        nvme_admin(fd, 0x02, 0, (127U << 16) | 0x02, log, 512) == 0) {
        int temp = le16(log + 1) ? (int)le16(log + 1) - 273 : -1, used = log[5];
        char t[24] = "";
        if (temp >= 0) snprintf(t, sizeof(t), " · %d °C", temp);
        card_add(c, "Santé : %s%s · vie restante ≈ %d %% (détails : 1 = SMART)",
                 log[0] ? "AVERTISSEMENT CRITIQUE" : "OK", t, used >= 100 ? 0 : 100 - used);
    }
    rc = 0;
out:
    free(ctrl); free(log); return rc;
}
/* Fiche SAS/SCSI : INQUIRY + avis du firmware. */
static int sas_card(int fd, card_t *c) {
    sas_info_t si;
    memset(c, 0, sizeof(*c));
    if (sas_collect(fd, &si) < 0) return -1;
    card_add(c, "Modèle : %s %s", si.vendor, si.product);
    card_add(c, "Série : %s | Révision : %s", si.serial[0] ? si.serial : "inconnue", si.rev);
    if (si.rotation > 1) card_add(c, "Type : Disque dur SAS/SCSI %d tr/min", si.rotation);
    else card_add(c, "Type : %s", si.rotation == 1 ? "SSD SAS/SCSI" : "SAS/SCSI");
    if (si.ie_known || si.temp >= 0) {
        char t[24] = "";
        if (si.temp >= 0) snprintf(t, sizeof(t), " · %d °C", si.temp);
        card_add(c, "Santé : %s%s (détails : 1 = SMART)",
                 !si.ie_known ? "inconnue" : si.ie_asc ? "DÉFAILLANCE ANNONCÉE" : "OK", t);
    }
    return 0;
}

static int smart_report(int fd) {
    ui_status("Diagnostic SMART", "Lecture de l'identité et des attributs SMART");
    unsigned long long bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) bytes = 0;
    if (ioctl(fd, NVME_IOCTL_ID) > 0) { int r = nvme_report(fd, (uint64_t)bytes); fflush(stdout); return r; }
    unsigned char id[512], data[512], thr[512];
    if (identify_quiet(fd, id) < 0) {
        /* Pas de réponse ATA : disque SAS/SCSI (pages LOG SENSE) ? */
        sas_info_t si;
        if (sas_collect(fd, &si) == 0) { sas_print(&si, (uint64_t)bytes); fflush(stdout); return 0; }
        fprintf(stderr, "Ni IDENTIFY ATA, ni INQUIRY SCSI, ni NVMe : SMART indisponible pour ce périphérique.\n");
        return -1;
    }
    /* Mots 82/85 bit 0 : SMART pris en charge / activé. */
    if (!(word_le(id, 82) & 1)) { fprintf(stderr, "Ce disque n'annonce pas la prise en charge de SMART.\n"); return -1; }
    if (!(word_le(id, 85) & 1)) {
        fprintf(stderr, "SMART est désactivé sur ce disque. Activez-le avec : sudo smartctl -s on %s\n", ui_device);
        return -1;
    }
    if (smart_read_data(fd, data) < 0) { fprintf(stderr, "SMART READ DATA impossible (passerelle USB incompatible ?).\n"); return -1; }
    int have_thr = smart_read_thresholds(fd, thr) == 0;
    int health = smart_health(fd);
    smart_print_ata(id, data, have_thr ? thr : NULL, health, (uint64_t)bytes);
    fflush(stdout);
    return 0;
}
/*
 * Mot de passe ATA temporaire, volontairement connu et affiché : si l'effacement
 * est interrompu (coupure, plantage), le disque peut être déverrouillé avec hdparm.
 */
#define ATA_TEMP_PASSWORD "diskmedic"
static int sec_cmd(int fd, uint8_t command, uint8_t count, const unsigned char *payload, unsigned len, unsigned timeout) {
    uint8_t protocol = payload ? 5 : 3;
    uint8_t flags = payload ? 0x06 : 0x00;
    return ata_pt(fd, protocol, flags, 0, count, 0, 0, command,
                  (void *)payload, len, payload ? SG_DXFER_TO_DEV : SG_DXFER_NONE, timeout);
}
static int secure_erase(int fd, const unsigned char id[512]) {
    ui_status("ATA Secure Erase", "Commande firmware en cours; la durée dépend du disque");
    uint16_t s = word_le(id,128);
    if (!(s & 1)) { fprintf(stderr,"ATA Security non annoncé par IDENTIFY; abandon.\n"); return -1; }
    if (s & 8) { fprintf(stderr,"Le disque est FROZEN; aucun changement tenté. Déverrouillez-le avant de relancer.\n"); return -1; }
    if (s & 2) { fprintf(stderr,"La sécurité ATA est déjà activée; abandon par sécurité.\n"); return -1; }
    if (s & 4) { fprintf(stderr,"Le disque est LOCKED; abandon.\n"); return -1; }
    int enhanced = !!(s & 0x20);
    fprintf(stderr,"Effacement ATA %s demandé (peut durer plusieurs heures)…\n", enhanced ? "enhanced" : "standard");
    unsigned char payload[512] = {0};
    /*
     * Bloc de données sécurité ATA : mot 0 = identifiant (bit 0 : 0 = utilisateur) et
     * niveau (bit 8 : 0 = high), puis mot de passe de 32 octets complété par des zéros.
     */
    memcpy(payload + 2, ATA_TEMP_PASSWORD, strlen(ATA_TEMP_PASSWORD));
    fprintf(stderr,
            "Mot de passe ATA utilisateur temporaire : \"%s\".\n"
            "En cas d’interruption (coupure, plantage), déverrouillez le disque avec :\n"
            "  hdparm --user-master u --security-unlock %s %s\n"
            "  hdparm --user-master u --security-disable %s %s\n",
            ATA_TEMP_PASSWORD, ATA_TEMP_PASSWORD, ui_device, ATA_TEMP_PASSWORD, ui_device);
    if (sec_cmd(fd, ATA_SET_PASSWORD, 1, payload, sizeof(payload), 30000) < 0) {
        fprintf(stderr,"SET PASSWORD a échoué.\n"); return -1;
    }
    int result = -1;
    if (sec_cmd(fd, ATA_ERASE_PREPARE, 0, NULL, 0, 30000) < 0) {
        fprintf(stderr,"ERASE PREPARE a échoué; tentative de retrait du mot de passe.\n");
        goto cleanup;
    }
    /* SECURITY ERASE UNIT : le mode Enhanced se sélectionne par le bit 1 du mot 0 des données. */
    payload[0] = enhanced ? 0x02 : 0x00;
    if (sec_cmd(fd, ATA_ERASE_UNIT, 1, payload, sizeof(payload), ATA_TIMEOUT_MS) < 0) {
        fprintf(stderr,"SECURITY ERASE UNIT a échoué; tentative de retrait du mot de passe.\n");
        goto cleanup;
    }
    result = 0; fprintf(stderr,"ATA Secure Erase terminé (le disque retire lui-même le mot de passe).\n");
cleanup:
    if (result < 0) {
        /* SECURITY DISABLE PASSWORD avec le même mot de passe utilisateur. */
        payload[0] = 0;
        if (sec_cmd(fd, ATA_DISABLE_PASSWORD, 1, payload, sizeof(payload), 30000) < 0)
            fprintf(stderr,"AVERTISSEMENT : retrait du mot de passe échoué. Le disque peut rester verrouillé; "
                           "utilisez les commandes hdparm ci-dessus avec le mot de passe \"%s\".\n", ATA_TEMP_PASSWORD);
        else fprintf(stderr,"Mot de passe ATA retiré.\n");
    }
    memset(payload, 0, sizeof(payload));
    return result;
}

/* ---------- Type de support et opérations adaptées aux SSD (TRIM, Sanitize / Format NVM) ---------- */
static int read_sysfs_value(const char *path, char *out, size_t out_size);
enum { MEDIA_UNKNOWN, MEDIA_HDD, MEDIA_SSD };
typedef struct { int kind; char why[128]; } media_t;
/* Drapeau queue/rotational du noyau (disque entier, ou disque parent pour une partition). */
static int sysfs_rotational(int fd) {
    struct stat st;
    char path[128], v[16];
    if (fstat(fd, &st) < 0 || !S_ISBLK(st.st_mode)) return -1;
    unsigned ma = major(st.st_rdev), mi = minor(st.st_rdev);
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/queue/rotational", ma, mi);
    if (read_sysfs_value(path, v, sizeof(v)) < 0) {
        snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/../queue/rotational", ma, mi);
        if (read_sysfs_value(path, v, sizeof(v)) < 0) return -1;
    }
    return v[0] == '0' ? 0 : v[0] == '1' ? 1 : -1;
}
/*
 * SSD ou disque dur ? Le firmware fait foi : NVMe, mot 217 d'IDENTIFY (ATA), page VPD B1h (SCSI).
 * Le drapeau rotational du noyau ne sert qu'en dernier recours : il est faux pour un NVMe
 * monté sur un fichier loop, et vaut 1 par défaut pour beaucoup de périphériques virtuels.
 */
static media_t media_detect(int fd) {
    media_t m;
    m.kind = MEDIA_UNKNOWN;
    snprintf(m.why, sizeof(m.why), "type de support inconnu");
    if (ioctl(fd, NVME_IOCTL_ID) > 0) { m.kind = MEDIA_SSD; snprintf(m.why, sizeof(m.why), "SSD NVMe"); return m; }
    unsigned char id[512];
    unsigned r = 0;
    const char *bus = "";
    if (identify_quiet(fd, id) == 0) { r = word_le(id, 217); bus = "SATA"; }
    else {
        unsigned char cdb[6] = {0x12, 1, 0xb1, 0, 64, 0}, b1[64];
        if (scsi_cmd(fd, cdb, sizeof(cdb), b1, sizeof(b1)) >= 6 && b1[1] == 0xb1) {
            r = ((unsigned)b1[4] << 8) | b1[5];
            bus = "SAS/SCSI";
        }
    }
    if (r == 1) {
        m.kind = MEDIA_SSD;
        snprintf(m.why, sizeof(m.why), "SSD %s (le firmware annonce un support non rotatif)", bus);
        return m;
    }
    if (r >= 0x0401 && r <= 0xfffe) {
        m.kind = MEDIA_HDD;
        snprintf(m.why, sizeof(m.why), "disque dur %s %u tr/min", bus, r);
        return m;
    }
    int rot = sysfs_rotational(fd);
    if (rot == 0) { m.kind = MEDIA_SSD; snprintf(m.why, sizeof(m.why), "support non rotatif selon le noyau (SSD probable)"); }
    else if (rot == 1) { m.kind = MEDIA_HDD; snprintf(m.why, sizeof(m.why), "support rotatif selon le noyau"); }
    return m;
}
static void ssd_explain_zero(const media_t *m, int cli) {
    fprintf(stderr,
        "Écriture zéro déconseillée : ce disque est un %s.\n"
        "Sur un SSD, remplir le disque de zéros use la mémoire flash sans rien réparer. Ce n'est pas non plus\n"
        "un effacement sûr : les blocs de réserve gardent des copies des anciennes données. Et le contrôleur,\n"
        "qui croit alors le disque plein, ralentit jusqu'au prochain TRIM.\n", m->why);
    if (cli)
        fprintf(stderr,
            "À la place : --trim vide le SSD en quelques secondes, sans usure; --secure-erase l'efface de façon\n"
            "sûre par le firmware. Pour écrire les zéros quand même : --zero --force.\n");
}
static void ssd_explain_repair(const media_t *m, int cli) {
    fprintf(stderr,
        "Réparation lente déconseillée : ce disque est un %s.\n"
        "Réécrire les zones lentes soigne les secteurs faibles d'un disque dur. Un SSD déplace et rafraîchit\n"
        "déjà ses données lui-même : une lecture lente y signale plutôt un contrôleur ou une mémoire flash\n"
        "en difficulté, que la réécriture ne soigne pas.\n", m->why);
    if (cli)
        fprintf(stderr,
            "À la place : --scan-slow pour le diagnostic (lecture seule) et --smart pour l'état d'usure.\n"
            "Pour réécrire quand même : --slow-repair --force.\n");
}

/* Relit 1 Mio au début, au milieu et à la fin : 1 = tout à zéro, 0 = données non nulles, -1 = erreur. */
static int sample_is_zero(int fd, uint64_t size) {
    disk_io_t io; dio_open(&io, fd, 0);
    unsigned char *buf = alloc_aligned(BUF_SIZE);
    int rc = -1;
    if (!buf || !size) goto out;
    drop_cache(fd, 0, 0);
    uint64_t span = size < BUF_SIZE ? size : BUF_SIZE;
    uint64_t at[3] = {0, size / 2 / log_sector_size * log_sector_size, size - span};
    rc = 1;
    for (int k = 0; k < 3 && rc == 1; k++) {
        if (at[k] > size - span) at[k] = size - span;
        if (dio_pread(&io, buf, (size_t)span, at[k]) < 0) { rc = -1; break; }
        for (size_t i = 0; i < span; i++) if (buf[i]) { rc = 0; break; }
    }
out:
    free(buf); dio_close(&io); return rc;
}

/* TRIM du disque entier par tranches de 1 Gio (avancement visible, interruption possible). */
#define TRIM_CHUNK (1ULL << 30)
static int trim_disk(int fd, uint64_t size) {
    ui_status("TRIM", "Le disque marque tout son espace comme libre");
    fprintf(stderr, "TRIM de %" PRIu64 " octets : le disque marque tout son espace comme libre…\n", size);
    progress_t p; progress_begin(&p, "TRIM", size);
    for (uint64_t off = 0; off < size;) {
        uint64_t n = size - off > TRIM_CHUNK ? TRIM_CHUNK : size - off;
        uint64_t range[2] = {off, n};
        if (ioctl(fd, BLKDISCARD, range) < 0) {
            if (off == 0 && (errno == EOPNOTSUPP || errno == ENOTTY))
                fprintf(stderr, "Ce disque ne prend pas en charge TRIM/discard (%s) : rien n'a été modifié.\n", strerror(errno));
            else
                fprintf(stderr, "\nTRIM interrompu à 0x%" PRIx64 " : %s\n", off, strerror(errno));
            return -1;
        }
        off += n;
        progress_show(&p, "TRIM", off, size);
    }
    fprintf(stderr, "\n");
    int z = sample_is_zero(fd, size);
    if (z < 0) { fprintf(stderr, "Relecture après TRIM impossible : %s\n", strerror(errno)); return -1; }
    if (z) fprintf(stderr, "Relecture d'échantillons (début, milieu, fin) : tout est à zéro.\n");
    else fprintf(stderr, "Relecture d'échantillons : des données restent lisibles (ce disque ne garantit pas la lecture à zéro après TRIM).\n");
    fprintf(stderr, "TRIM terminé. Ce n'est pas un effacement sûr : pour rendre les données irrécupérables, utilisez Secure Erase.\n");
    return 0;
}

static uint32_t le32_at(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
enum { NVE_NONE, NVE_SANITIZE_CRYPTO, NVE_SANITIZE_BLOCK, NVE_FORMAT_CRYPTO, NVE_FORMAT_ERASE };
typedef struct { int method; char why[480]; } nvme_plan_t;
static const char *nvme_method_name(int m) {
    return m == NVE_SANITIZE_CRYPTO ? "Sanitize, effacement cryptographique"
         : m == NVE_SANITIZE_BLOCK ? "Sanitize, effacement des blocs"
         : m == NVE_FORMAT_CRYPTO ? "Format NVM, effacement cryptographique"
         : m == NVE_FORMAT_ERASE ? "Format NVM, effacement des données utilisateur" : "aucune";
}
/*
 * Choix de la méthode d'effacement d'après Identify Controller (OACS, SANICAP, FNA).
 * active_ns : nombre de namespaces actifs (< 0 : inconnu). Sanitize agit sur tout le
 * sous-système, et Format aussi quand FNA l'indique : avec plusieurs namespaces, ils
 * effaceraient aussi les autres, d'où un refus. L'écrasement Sanitize (qui use la flash)
 * n'est pas proposé. Les capacités sont vérifiées ici plutôt que de compter sur un refus
 * du disque : QEMU, par exemple, accepte un effacement cryptographique qu'il n'annonce pas.
 */
static int nvme_erase_choose(const unsigned char *ctrl, int active_ns, nvme_plan_t *p) {
    uint16_t oacs = le16(ctrl + 256);
    uint32_t sanicap = le32_at(ctrl + 328);
    uint8_t fna = ctrl[524];
    int single = active_ns == 1, can_format = (oacs & 2) != 0, format_all = (fna & 3) != 0;
    p->method = NVE_NONE;
    if (single && (sanicap & 1)) p->method = NVE_SANITIZE_CRYPTO;
    else if (single && (sanicap & 2)) p->method = NVE_SANITIZE_BLOCK;
    else if (can_format && (single || !format_all)) p->method = (fna & 4) ? NVE_FORMAT_CRYPTO : NVE_FORMAT_ERASE;
    switch (p->method) {
    case NVE_SANITIZE_CRYPTO:
        snprintf(p->why, sizeof(p->why), "Le disque change sa clé de chiffrement interne : instantané et sans usure. "
                 "Sanitize efface tout le disque, blocs de réserve et caches compris, et reprend seul après une coupure.");
        return 0;
    case NVE_SANITIZE_BLOCK:
        snprintf(p->why, sizeof(p->why), "Chaque bloc de mémoire flash est effacé (quelques secondes à quelques minutes). "
                 "Sanitize efface tout le disque, blocs de réserve et caches compris, et reprend seul après une coupure.");
        return 0;
    case NVE_FORMAT_CRYPTO:
    case NVE_FORMAT_ERASE:
        snprintf(p->why, sizeof(p->why), "%s%s Le format de secteur actuel est conservé.",
                 (sanicap & 3) ? "Sanitize n'est pas utilisé car le disque a plusieurs namespaces (ou un nombre inconnu). "
                               : "Le disque ne propose pas Sanitize. ",
                 p->method == NVE_FORMAT_CRYPTO ? "Le disque change sa clé de chiffrement interne : instantané et sans usure."
                                                : "Le disque n'annonce pas l'effacement cryptographique : il efface les données utilisateur.");
        return 0;
    default: break;
    }
    if (!can_format && !(sanicap & 3))
        snprintf(p->why, sizeof(p->why), "Ce NVMe ne propose ni Sanitize (cryptographique ou par blocs) ni Format NVM%s : "
                 "aucun effacement firmware possible, rien n'a été modifié.",
                 (sanicap & 4) ? " (seul l'écrasement Sanitize est proposé : il use la flash, DiskMedic ne l'utilise pas)" : "");
    else
        snprintf(p->why, sizeof(p->why), "Ce NVMe a plusieurs namespaces actifs (ou un nombre inconnu) : Sanitize ou Format "
                 "les effaceraient tous. Refus par sécurité, rien n'a été modifié.");
    return -1;
}
static int nvme_active_namespaces(int fd, const unsigned char *ctrl) {
    unsigned char *list = alloc_aligned(4096);
    int n = -1;
    if (list) {
        memset(list, 0, 4096);
        if (nvme_admin(fd, 0x06, 0, 2, list, 4096) == 0)   /* CNS 02h : liste des namespaces actifs */
            for (n = 0; n < 1024 && le32_at(list + 4 * n); n++) {}
        free(list);
    }
    if (n < 0 && le32_at(ctrl + 516) == 1) n = 1;          /* NN : un seul namespace possible */
    return n;
}
/* Prépare le plan d'effacement : 0 si une méthode est possible; ctrl reçoit Identify Controller. */
static int nvme_erase_plan(int fd, unsigned char *ctrl, nvme_plan_t *p) {
    p->method = NVE_NONE;
    memset(ctrl, 0, 4096);
    if (nvme_admin(fd, 0x06, 0, 1, ctrl, 4096) < 0) {
        snprintf(p->why, sizeof(p->why), "Identify Controller NVMe a échoué : effacement impossible.");
        return -1;
    }
    return nvme_erase_choose(ctrl, nvme_active_namespaces(fd, ctrl), p);
}
/* Annonce la méthode avant la confirmation; -1 si aucune n'est possible. */
static int nvme_erase_preview(int fd) {
    unsigned char *ctrl = alloc_aligned(4096);
    nvme_plan_t plan;
    if (!ctrl) { perror("allocation"); return -1; }
    int r = nvme_erase_plan(fd, ctrl, &plan);
    if (r == 0) fprintf(stderr, "Secure Erase NVMe : %s.\n%s\n", nvme_method_name(plan.method), plan.why);
    else fprintf(stderr, "%s\n", plan.why);
    free(ctrl);
    return r;
}
static void nvme_status_error(const char *what, int st) {
    if (st > 0) {
        int sct = (st >> 8) & 7, sc = st & 0xff;
        fprintf(stderr, "%s refusé par le disque (statut NVMe 0x%03x%s).\n", what, st & 0x7ff,
                !sct && sc == 0x01 ? " : commande non prise en charge" :
                !sct && sc == 0x02 ? " : paramètre refusé" :
                !sct && sc == 0x1d ? " : Sanitize interdit pendant une mise à jour du firmware" :
                sct == 1 && sc == 0x0a ? " : format invalide" : "");
    } else fprintf(stderr, "%s a échoué : %s\n", what, strerror(errno));
}
/* Journal Sanitize Status (81h) : SPROG = avancement sur 65536, SSTAT bits 2:0 = état. */
static int nvme_sanitize_status(int fd, unsigned *sprog, unsigned *sstat) {
    unsigned char *log = alloc_aligned(4096);
    if (!log) return -1;
    memset(log, 0, 4096);
    int r = nvme_admin(fd, 0x02, 0xffffffffU, (127U << 16) | 0x81, log, 512);
    if (r < 0) r = nvme_admin(fd, 0x02, 0, (127U << 16) | 0x81, log, 512);
    if (r == 0) { *sprog = le16(log); *sstat = le16(log + 2); }
    free(log);
    return r;
}
static int nvme_secure_erase(int fd, uint64_t size) {
    int nsid = ioctl(fd, NVME_IOCTL_ID), st = 0, rc = -1;
    unsigned char *ctrl = alloc_aligned(4096), *ns = alloc_aligned(4096);
    nvme_plan_t plan;
    if (!ctrl || !ns) { perror("allocation"); goto out; }
    memset(ns, 0, 4096);
    if (nvme_erase_plan(fd, ctrl, &plan) < 0) { fprintf(stderr, "%s\n", plan.why); goto out; }
    if (nsid <= 0 || nvme_admin(fd, 0x06, (uint32_t)nsid, 0, ns, 4096) < 0) {
        fprintf(stderr, "Identify Namespace NVMe a échoué : effacement impossible, rien n'a été modifié.\n"); goto out;
    }
    fprintf(stderr, "Méthode : %s.\n", nvme_method_name(plan.method));
    if (plan.method == NVE_SANITIZE_CRYPTO || plan.method == NVE_SANITIZE_BLOCK) {
        unsigned sp = 0, ss = 0;
        if (nvme_sanitize_status(fd, &sp, &ss) == 0 && (ss & 7) == 2) {
            fprintf(stderr, "Un Sanitize est déjà en cours sur ce disque : attendez sa fin avant d'en lancer un autre.\n"); goto out;
        }
        /* SANACT : 4 = cryptographique, 2 = blocs; AUSE (bit 3) : sortie libre du mode échec. */
        uint32_t cdw10 = (plan.method == NVE_SANITIZE_CRYPTO ? 4U : 2U) | (1U << 3);
        ui_status("NVMe Sanitize", "Commande firmware en cours; elle continue même si DiskMedic s'arrête");
        if (nvme_admin_full(fd, 0x84, 0, cdw10, NULL, 0, 30000, &st) < 0) { nvme_status_error("Sanitize", st); goto out; }
        progress_t p; progress_begin(&p, "Sanitize", size);
        for (int idle = 0;;) {
            struct timespec pause = {0, 500 * 1000 * 1000};
            nanosleep(&pause, NULL);
            if (nvme_sanitize_status(fd, &sp, &ss) < 0) {
                fprintf(stderr, "\nJournal Sanitize illisible : %s. Le disque poursuit l'effacement seul.\n", strerror(errno)); goto out;
            }
            unsigned state = ss & 7;
            if (state == 2) { progress_show(&p, "Sanitize", (uint64_t)((double)sp / 65536.0 * (double)size), size); continue; }
            if (state == 1 || state == 4) { progress_show(&p, "Sanitize", size, size); fprintf(stderr, "\nSanitize terminé.\n"); break; }
            if (state == 3) {
                fprintf(stderr, "\nLe disque signale l'échec du Sanitize : les données ne sont peut-être pas effacées. "
                                "Relancez Secure Erase; si l'échec persiste, le disque est probablement défaillant.\n");
                goto out;
            }
            if (++idle > 60) { fprintf(stderr, "\nLe disque n'indique aucun Sanitize en cours ni terminé : résultat inconnu.\n"); goto out; }
        }
    } else {
        uint8_t flbas = ns[26], dps = ns[29];
        uint32_t ses = plan.method == NVE_FORMAT_CRYPTO ? 2U : 1U;
        /* Format conservé : LBAF (FLBAS bits 3:0 et 6:5), métadonnées (bit 4), protection (DPS). */
        uint32_t cdw10 = (flbas & 0x0fU) | ((flbas & 0x10U) ? 1U << 4 : 0) | ((uint32_t)(dps & 7U) << 5) |
                         ((dps & 8U) ? 1U << 8 : 0) | (ses << 9) | ((uint32_t)((flbas >> 5) & 3U) << 12);
        ui_status("NVMe Format NVM", "Commande firmware en cours; la durée dépend du disque");
        fprintf(stderr, "Format NVM du namespace %d en cours…\n", nsid);
        if (nvme_admin_full(fd, 0x80, (uint32_t)nsid, cdw10, NULL, 0, ATA_TIMEOUT_MS, &st) < 0) {
            nvme_status_error("Format NVM", st); goto out;
        }
        fprintf(stderr, "Format NVM terminé.\n");
    }
    int z = sample_is_zero(fd, size);
    if (z == 1) fprintf(stderr, "Relecture d'échantillons (début, milieu, fin) : tout est à zéro.\n");
    else if (z == 0 && (plan.method == NVE_SANITIZE_CRYPTO || plan.method == NVE_FORMAT_CRYPTO))
        fprintf(stderr, "Relecture d'échantillons : données aléatoires, normal après un effacement cryptographique (l'ancienne clé est détruite).\n");
    else if (z == 0) fprintf(stderr, "Relecture d'échantillons : des données non nulles restent lisibles; ce disque ne remet pas à zéro après effacement.\n");
    else fprintf(stderr, "Relecture d'échantillons impossible (%s) : le noyau relit peut-être encore le disque.\n", strerror(errno));
    rc = 0;
out:
    free(ctrl); free(ns); return rc;
}
/* Secure Erase : NVMe (Sanitize ou Format NVM), sinon ATA Security Erase (SATA, SSD ou disque dur). */
static int secure_erase_any(int fd, uint64_t size) {
    if (ioctl(fd, NVME_IOCTL_ID) > 0) return nvme_secure_erase(fd, size);
    unsigned char id[512];
    if (identify(fd, id) < 0) {
        fprintf(stderr, "IDENTIFY DEVICE a échoué : Secure Erase impossible (disque SAS/SCSI ou passerelle USB ?).\n");
        return -1;
    }
    return secure_erase(fd, id);
}
static double elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return ((double)(b->tv_sec-a->tv_sec)*1e9 + (double)(b->tv_nsec-a->tv_nsec))/1e6;
}
static int cmp_double(const void *a,const void *b) { double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static uint64_t sample_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}
/* Trie v en place et renvoie sa médiane. */
static double median_of(double *v, size_t n) {
    qsort(v, n, sizeof(*v), cmp_double);
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}
static int timed_pread(disk_io_t *io, void *buf, size_t n, uint64_t off, double *ms) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int r = dio_pread(io, buf, n, off);
    clock_gettime(CLOCK_MONOTONIC, &b);
    *ms = elapsed_ms(&a, &b);
    return r;
}

#define ZONE_SIZE (64U * 1024U)
#define ZONES_PER_BLOCK (BUF_SIZE / ZONE_SIZE)
#define EVICT_BYTES (64ULL * 1024U * 1024U)
/*
 * Écart minimal pour retenir une zone sur le seul critère relatif (temps ≥ facteur × référence) :
 * sur un support très rapide (référence de quelques centièmes de ms), le bruit de mesure
 * dépasse sinon le seuil. Un secteur faible coûte des dizaines de ms : il reste détecté.
 */
#define ZONE_MIN_EXCESS_MS 1.0
/*
 * Même principe pour les blocs de 1 Mio : un bloc n'est lent que s'il dépasse la médiane
 * d'au moins 5 ms. Sur un NVMe ou un fichier en cache (médiane < 1 ms), les pauses de quelques
 * ms du système dépassent sinon médiane × facteur, parfois aux deux passages. Une relecture
 * de disque dur coûte au moins un tour de plateau (8 à 11 ms) : elle reste détectée.
 */
#define BLOCK_MIN_EXCESS_MS 5.0

/* Zone lente localisée à l'intérieur d'un bloc de 1 Mio. */
typedef struct {
    size_t block;           /* indice dans la liste des blocs signalés */
    uint64_t off;
    size_t len;
    double t, base, t_after;
    int written, verified, still_slow;
} zone_t;

/*
 * Vide au mieux le cache interne du disque en lisant 64 Mio dans une région éloignée :
 * sans cela, une relecture serait servie par la mémoire du disque et paraîtrait rapide.
 */
static void evict_drive_cache(disk_io_t *io, unsigned char *tmp, uint64_t size, uint64_t near) {
    uint64_t len = EVICT_BYTES;
    if (len > size / 4) len = (size / 4) - (size / 4) % BUF_SIZE;
    if (len < BUF_SIZE) return;
    uint64_t start = near >= size / 2 ? 0 : ((size - len) / BUF_SIZE) * BUF_SIZE;
    for (uint64_t o = start; o < start + len && o < size; o += BUF_SIZE) {
        size_t n = size - o > BUF_SIZE ? BUF_SIZE : (size_t)(size - o);
        (void)dio_pread(io, tmp, n, o);
    }
}

/*
 * Mesure chaque zone d'un bloc, de la dernière à la première : les disques ne font pas
 * de lecture anticipée à rebours, chaque zone est donc lue sur le support. Une lecture
 * préalable juste après le bloc positionne la tête sans précharger le bloc lui-même.
 */
static int measure_zones(disk_io_t *io, unsigned char *buf, unsigned char *tmp, uint64_t size,
                         uint64_t off, size_t n, size_t zsz, double *zt, size_t *nz) {
    *nz = (n + zsz - 1) / zsz;
    evict_drive_cache(io, tmp, size, off);
    if (off + n + log_sector_size <= size) (void)dio_pread(io, tmp, log_sector_size, off + n);
    for (size_t z = *nz; z-- > 0;) {
        size_t zo = z * zsz, zl = n - zo < zsz ? n - zo : zsz;
        if (timed_pread(io, buf + zo, zl, off + zo, &zt[z]) < 0) return -1;
    }
    return 0;
}

/* Référence : temps médian d'une zone sur trois blocs sains, mesurés de la même façon. */
static double calibrate_zone_ref(disk_io_t *io, unsigned char *buf, unsigned char *tmp, uint64_t size,
                                 uint64_t total, const float *pass1, double median, size_t zsz) {
    double v[3 * ZONES_PER_BLOCK];
    size_t nv = 0;
    for (uint64_t k = 1; k <= 3; k++) {
        uint64_t start = total * k / 4;
        for (uint64_t i = start; i < total && i < start + 1024; i++) {
            if ((i + 1) * (uint64_t)BUF_SIZE > size) break;
            if (pass1 && (pass1[i] < 0 || (double)pass1[i] > median)) continue;
            double zt[ZONES_PER_BLOCK];
            size_t nz = 0;
            if (measure_zones(io, buf, tmp, size, i * (uint64_t)BUF_SIZE, BUF_SIZE, zsz, zt, &nz) == 0)
                for (size_t z = 0; z < nz; z++) v[nv++] = zt[z];
            break;
        }
    }
    return nv ? median_of(v, nv) : -1.0;
}

static int zone_push(zone_t **zones, size_t *n, size_t *cap, const zone_t *z) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        if (nc < *cap || nc > SIZE_MAX / sizeof(**zones)) return -1;
        zone_t *p = realloc(*zones, nc * sizeof(*p));
        if (!p) return -1;
        *zones = p; *cap = nc;
    }
    (*zones)[(*n)++] = *z;
    return 0;
}

static void fmt_ms(char *out, size_t n, double ms) {
    if (ms < 0) snprintf(out, n, "n/d"); else snprintf(out, n, "%.3f ms", ms);
}

/*
 * Scan des blocs lents (et réparation si repair) :
 *   1. passage 1 : temps de chaque bloc de 1 Mio, médiane et seuil = médiane × facteur ;
 *   2. passage 2 : un bloc n'est confirmé que s'il dépasse le seuil aux deux passages
 *      (le disque entier est relu entre les deux, son cache ne peut pas fausser la mesure) ;
 *   3. localisation : chaque bloc confirmé est remesuré par zones de 64 Kio, comparées à
 *      une référence prise sur des blocs sains (3e mesure : si la lenteur ne se reproduit
 *      pas, rien n'est réécrit) ; en réparation, seules les zones lentes sont réécrites ;
 *   4. contrôle différé : après vidage du cache du disque, chaque zone réécrite est
 *      rechronométrée pour savoir si elle est toujours lente.
 * Dans un bloc illisible, seuls les secteurs physiques contenant un secteur perdu sont
 * réécrits (avec des zéros pour la partie illisible).
 */
static int slow_scan(int fd, uint64_t size, double factor, int repair) {
    const uint64_t total = size / BUF_SIZE + (size % BUF_SIZE != 0);
    const size_t sample_cap = (size_t)1U << 20;
    const char *op = repair ? "slow-repair" : "scan-slow";
    const size_t zsz = phys_sector_size > ZONE_SIZE ? phys_sector_size : ZONE_SIZE;
    const double zone_factor = factor > 2.0 ? factor : 2.0;
    if (!total || total > (uint64_t)INT64_MAX / BUF_SIZE) {
        fprintf(stderr, "Taille de périphérique invalide ou trop grande.\n");
        return -1;
    }
    disk_io_t io; dio_open(&io, fd, repair);
    unsigned char *buf = alloc_aligned(BUF_SIZE), *tmp = alloc_aligned(BUF_SIZE);
    double *sample = malloc(sample_cap * sizeof(*sample));
    float *pass1 = total <= SIZE_MAX / sizeof(float) ? malloc((size_t)total * sizeof(float)) : NULL;
    slow_block_t *slow = NULL;
    zone_t *zones = NULL;
    size_t nslow = 0, cap = 0, nzones = 0, zcap = 0;
    int rc = -1;
    if (!buf || !tmp || !sample) { perror("allocation"); goto out; }
    if (!pass1)
        fprintf(stderr, "Avertissement : mémoire insuffisante pour conserver les temps du passage 1; "
                        "les blocs ne seront pas confirmés par deux mesures.\n");
    size_t nsample = 0;
    uint64_t nvalid = 0, errors = 0, bad_sectors = 0;
    uint64_t rng = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)buf ^ size;
    if (!rng) rng = UINT64_C(0x9e3779b97f4a7c15);

    /* Passage 1 : mesure de référence. */
    fprintf(stderr, "Passage 1/2 : mesure de %" PRIu64 " blocs de 1 Mio (%s)…\n",
            total, io.dfd >= 0 ? "O_DIRECT, sans cache" : "E/S tamponnée, cache possible");
    progress_t progress; progress_begin(&progress, "Passage 1/2 : mesure", size);
    for (uint64_t i = 0; i < total; i++) {
        uint64_t off = i * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        double t;
        int r = timed_pread(&io, buf, n, off, &t);
        if (pass1) pass1[i] = r == 0 ? (float)t : -1.0f;
        if (r == 0) {
            nvalid++;
            if (nsample < sample_cap) sample[nsample++] = t;
            else {
                uint64_t j = sample_random(&rng) % nvalid;
                if (j < sample_cap) sample[(size_t)j] = t;
            }
        }
        if ((i + 1) % 16 == 0 || i + 1 == total)
            progress_show(&progress, "Passage 1/2 : mesure", (i + 1 == total) ? size : (i + 1) * (uint64_t)BUF_SIZE, size);
    }
    fprintf(stderr, "\n");
    if (!nsample) { fprintf(stderr, "Aucune lecture valide.\n"); goto out; }
    double median = median_of(sample, nsample);
    double threshold = median * factor;
    int floor_used = threshold < median + BLOCK_MIN_EXCESS_MS;
    if (floor_used) threshold = median + BLOCK_MIN_EXCESS_MS;
    free(sample); sample = NULL;
    if (floor_used)
        printf("Référence médiane : %.3f ms/bloc; seuil : %.3f ms (support rapide : écart minimal de %.0f ms).\n",
               median, threshold, BLOCK_MIN_EXCESS_MS);
    else printf("Référence médiane : %.3f ms/bloc; seuil : %.3f ms (facteur %.2f).\n", median, threshold, factor);

    /* Passage 2 : confirmation. */
    fprintf(stderr, "Passage 2/2 : confirmation des blocs lents…\n");
    progress_begin(&progress, "Passage 2/2 : confirmation", size);
    for (uint64_t i = 0; i < total; i++) {
        uint64_t off = i * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        double ms;
        if (timed_pread(&io, buf, n, off, &ms) < 0) {
            ms = -1.0; errors++;
            fprintf(stderr, "\nErreur de lecture dans le bloc %" PRIu64 "; analyse secteur par secteur…\n", i);
            uint64_t bad = read_sectors(&io, buf, n, off, op, repair ? "EN ATTENTE" : "AUCUNE", 1, NULL);
            if (bad == UINT64_MAX) goto out;
            bad_sectors += bad;
        }
        if (ms < 0 || ms > threshold) {
            if (nslow == cap) {
                size_t nc = cap ? cap * 2 : 16;
                if (nc < cap || nc > SIZE_MAX / sizeof(*slow)) { fprintf(stderr, "Liste des blocs lents trop volumineuse.\n"); goto out; }
                slow_block_t *p = realloc(slow, nc * sizeof(*p));
                if (!p) { perror("realloc"); goto out; }
                slow = p; cap = nc;
            }
            double ms1 = pass1 ? (double)pass1[i] : -2.0;
            int status = ms < 0 ? SB_UNREADABLE
                       : (!pass1 || ms1 < 0 || ms1 > threshold) ? SB_CONFIRMED : SB_INTERMITTENT;
            slow[nslow++] = (slow_block_t){.index = i, .ms = ms, .ms1 = ms1, .status = status};
        }
        if ((i + 1) % 16 == 0 || i + 1 == total)
            progress_show(&progress, "Passage 2/2 : confirmation", (i + 1 == total) ? size : (i + 1) * (uint64_t)BUF_SIZE, size);
    }
    fprintf(stderr, "\n");

    /* Localisation (et réécriture ciblée) dans les blocs confirmés. */
    size_t nconf = 0;
    for (size_t k = 0; k < nslow; k++) nconf += slow[k].status == SB_CONFIRMED;
    double ref = -1.0;
    if (nconf) {
        ref = calibrate_zone_ref(&io, buf, tmp, size, total, pass1, median, zsz);
        fprintf(stderr, "Localisation dans %zu bloc(s) confirmé(s), zones de %zu Kio", nconf, zsz / 1024);
        if (ref > 0) fprintf(stderr, " (zone saine de référence : %.3f ms)", ref);
        fprintf(stderr, "…\n");
    }
    free(pass1); pass1 = NULL;
    size_t done_conf = 0;
    progress_begin(&progress, repair ? "Localisation et réécriture" : "Localisation", (uint64_t)nconf * BUF_SIZE);
    for (size_t k = 0; k < nslow; k++) {
        if (slow[k].status != SB_CONFIRMED) continue;
        uint64_t off = slow[k].index * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        double zt[ZONES_PER_BLOCK];
        size_t nz = 0;
        int mr = measure_zones(&io, buf, tmp, size, off, n, zsz, zt, &nz);
        done_conf++;
        progress_show(&progress, repair ? "Localisation et réécriture" : "Localisation",
                      (uint64_t)done_conf * BUF_SIZE, (uint64_t)nconf * BUF_SIZE);
        if (mr < 0) {
            /* Devenu illisible depuis le passage 2 : traité comme un bloc illisible. */
            slow[k].status = SB_UNREADABLE; errors++;
            fprintf(stderr, "\nBloc %" PRIu64 " devenu illisible; analyse secteur par secteur…\n", slow[k].index);
            uint64_t bad = read_sectors(&io, buf, n, off, op, repair ? "EN ATTENTE" : "AUCUNE", 1, NULL);
            if (bad == UINT64_MAX) goto out;
            bad_sectors += bad;
            continue;
        }
        double base = ref;
        if (base <= 0) { double v[ZONES_PER_BLOCK]; memcpy(v, zt, nz * sizeof(*v)); base = median_of(v, nz); }
        double excess = slow[k].ms - median, sum = 0;
        size_t first = nzones;
        for (size_t z = 0; z < nz; z++) {
            sum += zt[z];
            if (zt[z] - base >= 0.5 * excess || (zt[z] >= zone_factor * base && zt[z] - base >= ZONE_MIN_EXCESS_MS)) {
                size_t zo = z * zsz;
                zone_t zn = {.block = k, .off = off + zo, .len = n - zo < zsz ? n - zo : zsz, .t = zt[z], .base = base, .t_after = -1};
                if (zone_push(&zones, &nzones, &zcap, &zn) < 0) { perror("allocation zones"); goto out; }
            }
        }
        if (nzones == first) {
            /* Aucune zone ne se détache : lenteur répartie sur le bloc entier, ou non reproduite. */
            if (sum - (double)nz * base >= 0.5 * excess) {
                for (size_t z = 0; z < nz; z++) {
                    size_t zo = z * zsz;
                    zone_t zn = {.block = k, .off = off + zo, .len = n - zo < zsz ? n - zo : zsz, .t = zt[z], .base = base, .t_after = -1};
                    if (zone_push(&zones, &nzones, &zcap, &zn) < 0) { perror("allocation zones"); goto out; }
                }
            } else { slow[k].status = SB_NOT_REPRODUCED; continue; }
        }
        if (!repair) continue;
        /* buf contient les données du bloc qui viennent d'être lues : seules les zones lentes sont réécrites. */
        for (size_t j = first; j < nzones; j++) {
            if (dio_pwrite(&io, buf + (zones[j].off - off), zones[j].len, zones[j].off) == 0) zones[j].written = 1;
            else fprintf(stderr, "\nÉchec écriture zone LBA %" PRIu64 ": %s\n", zones[j].off / log_sector_size, strerror(errno));
        }
        if (fdatasync(fd) < 0) {
            perror("fdatasync");
            for (size_t j = first; j < nzones; j++) zones[j].written = 0;
        }
        drop_cache(fd, off, n);
        for (size_t j = first; j < nzones; j++)
            if (zones[j].written && dio_pread(&io, tmp, zones[j].len, zones[j].off) == 0 &&
                memcmp(tmp, buf + (zones[j].off - off), zones[j].len) == 0)
                zones[j].verified = 1;
    }
    if (nconf) fprintf(stderr, "\n");

    /* Contrôle différé : le cache du disque est vidé avant chaque nouvelle mesure. */
    size_t nverif = 0;
    for (size_t j = 0; j < nzones; j++) nverif += zones[j].verified != 0;
    if (repair && nverif) {
        fprintf(stderr, "Contrôle différé de %zu zone(s) réécrite(s)…\n", nverif);
        size_t done = 0;
        progress_begin(&progress, "Contrôle différé", 0);
        for (size_t j = 0; j < nzones; j++) {
            if (!zones[j].verified) continue;
            evict_drive_cache(&io, tmp, size, zones[j].off);
            if (zones[j].off + zones[j].len + log_sector_size <= size)
                (void)dio_pread(&io, tmp, log_sector_size, zones[j].off + zones[j].len);
            if (timed_pread(&io, tmp, zones[j].len, zones[j].off, &zones[j].t_after) < 0) {
                zones[j].verified = 0; zones[j].t_after = -1;
            } else {
                zones[j].still_slow = zones[j].t_after - zones[j].base >= 0.5 * (zones[j].t - zones[j].base) &&
                                      zones[j].t_after > zones[j].base * 1.5;
            }
            done++;
            progress_show(&progress, "Contrôle différé", (uint64_t)done * zones[j].len, (uint64_t)nverif * zones[j].len);
        }
        fprintf(stderr, "\n");
    }

    /* Rapport et journal. */
    size_t n_unread = 0, n_conf = 0, n_inter = 0;
    size_t z_ok = 0, z_still = 0, z_fail = 0, u_fixed = 0, u_fail = 0;
    uint64_t zeroed = 0;
    for (size_t k = 0; k < nslow; k++) {
        n_unread += slow[k].status == SB_UNREADABLE;
        n_conf += slow[k].status == SB_CONFIRMED;
        n_inter += slow[k].status == SB_INTERMITTENT || slow[k].status == SB_NOT_REPRODUCED;
    }
    printf("Blocs signalés : %zu — lents confirmés : %zu, intermittents (non confirmés) : %zu, illisibles : %zu "
           "(secteurs illisibles : %" PRIu64 ").\n", nslow, n_conf, n_inter, n_unread, bad_sectors);
    size_t zi = 0;
    for (size_t k = 0; k < nslow; k++) {
        uint64_t off = slow[k].index * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        char m1[32], m2[32], state[160], action[160];
        fmt_ms(m1, sizeof(m1), slow[k].ms1); fmt_ms(m2, sizeof(m2), slow[k].ms);
        printf("  bloc %" PRIu64 " (LBA %" PRIu64 ") : ", slow[k].index, off / log_sector_size);
        if (slow[k].status == SB_INTERMITTENT || slow[k].status == SB_NOT_REPRODUCED) {
            if (slow[k].status == SB_INTERMITTENT)
                snprintf(state, sizeof(state), "INTERMITTENT: passage 1 %s, passage 2 %s", m1, m2);
            else
                snprintf(state, sizeof(state), "INTERMITTENT: passages 1/2 %s / %s, non reproduit à la localisation", m1, m2);
            printf("%s — non réécrit\n", state + strlen("INTERMITTENT: "));
            if (log_event(op, off / log_sector_size, state, repair ? "AUCUNE (non confirmé, non réécrit)" : "AUCUNE") < 0) goto out;
            continue;
        }
        if (slow[k].status == SB_UNREADABLE) {
            printf("illisible");
            if (!repair) { printf("\n"); continue; }
            /* Réécriture des seuls secteurs physiques contenant un secteur logique perdu. */
            unsigned char badmap[BUF_SIZE / 512 / 8];
            memset(badmap, 0, sizeof(badmap));
            uint64_t bad = read_sectors(&io, buf, n, off, op, "", 0, badmap);
            int wfail = 0;
            size_t nphys = 0;
            if (bad) {
                for (size_t p = 0; p < n; p += phys_sector_size) {
                    size_t pl = n - p < phys_sector_size ? n - p : phys_sector_size, need = 0;
                    for (size_t s = p; s < p + pl; s += log_sector_size) {
                        size_t idx = s / log_sector_size;
                        if (badmap[idx / 8] & (1U << (idx % 8))) need = 1;
                    }
                    if (!need) continue;
                    nphys++;
                    if (dio_pwrite(&io, buf + p, pl, off + p) < 0) wfail = 1;
                }
                if (!wfail && fdatasync(fd) < 0) wfail = 1;
                drop_cache(fd, off, n);
                if (!wfail && (dio_pread(&io, tmp, n, off) < 0 || memcmp(tmp, buf, n) != 0)) wfail = 2;
            }
            if (!bad) snprintf(action, sizeof(action), "AUCUNE: secteurs relus avec succès");
            else if (wfail == 1) { snprintf(action, sizeof(action), "ECHEC: écriture/sync"); u_fail++; }
            else if (wfail == 2) {
                snprintf(action, sizeof(action), "ECHEC: vérification (%zu secteur(s) physique(s) réécrit(s), toujours illisible)", nphys);
                u_fail++;
            }
            else {
                snprintf(action, sizeof(action), "REÉCRITURE VÉRIFIÉE; %" PRIu64 " secteur(s) illisible(s) remplacé(s) par des zéros "
                         "(%zu secteur(s) physique(s) réécrit(s))", bad, nphys);
                u_fixed++; zeroed += bad;
            }
            printf(" — %s\n", action);
            snprintf(state, sizeof(state), "DEFECTUEUX: lecture impossible (%" PRIu64 " secteur(s))", bad);
            if (log_event(op, off / log_sector_size, state, action) < 0) goto out;
            continue;
        }
        printf("lent (passage 1 : %s, passage 2 : %s)\n", m1, m2);
        for (; zi < nzones && zones[zi].block < k; zi++) {}
        for (; zi < nzones && zones[zi].block == k; zi++) {
            zone_t *z = &zones[zi];
            uint64_t lba = z->off / log_sector_size, nsec = z->len / log_sector_size;
            printf("    zone LBA %" PRIu64 "–%" PRIu64 " (%zu Kio) : %.3f ms (zone saine : %.3f ms)",
                   lba, lba + nsec - 1, z->len / 1024, z->t, z->base);
            if (!repair) snprintf(action, sizeof(action), "AUCUNE");
            else if (!z->written) { snprintf(action, sizeof(action), "ECHEC: écriture/sync"); z_fail++; }
            else if (!z->verified) { snprintf(action, sizeof(action), "ECHEC: vérification"); z_fail++; }
            else if (z->still_slow) { snprintf(action, sizeof(action), "REÉCRITURE VÉRIFIÉE, TOUJOURS LENT (%.3f ms)", z->t_after); z_ok++; z_still++; }
            else { snprintf(action, sizeof(action), "REÉCRITURE VÉRIFIÉE, RAPIDE (%.3f ms)", z->t_after); z_ok++; }
            if (repair) printf(" → %s", action);
            printf("\n");
            snprintf(state, sizeof(state), "LENT: zone %zu Kio %.3f ms (zone saine %.3f ms); bloc %s / %s",
                     z->len / 1024, z->t, z->base, m1, m2);
            if (log_event(op, lba, state, action) < 0) goto out;
        }
    }
    if (repair)
        printf("Rapport réparation : zones réécrites et vérifiées=%zu (dont toujours lentes=%zu), échecs=%zu; "
               "blocs illisibles réparés=%zu, échecs=%zu, secteurs remplacés par des zéros=%" PRIu64 "; "
               "blocs intermittents non réécrits=%zu.\n",
               z_ok, z_still, z_fail, u_fixed, u_fail, zeroed, n_inter);
    rc = repair ? ((z_fail || u_fail) ? -1 : 0) : (errors ? -1 : 0);
out:
    free(zones); free(slow); free(pass1); free(sample); free(buf); free(tmp); dio_close(&io); return rc;
}

#define MAX_DISKS 256
#define DISK_NAME_LEN 256
#define DISK_INFO_LEN 256
typedef struct {
    char name[DISK_NAME_LEN];
    char model[DISK_INFO_LEN];
    char vendor[DISK_INFO_LEN];
    char serial[DISK_INFO_LEN];
    char type[DISK_INFO_LEN];
    uint64_t bytes;
} disk_info_t;

/* Métadonnées réutilisées par le TUI; seules les cases sélectionnées sont conservées. */
static disk_info_t tui_disks[MAX_DISKS];

/* Supprime les espaces ASCII en début et fin; renvoie la chaîne normalisée. */
static char *trim(char *s) {
    if (!s) return s;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static void ata_ident_string(const unsigned char *ident, size_t byte_offset,
                             size_t byte_length, char *out, size_t out_size) {
    size_t n = byte_length < out_size - 1 ? byte_length : out_size - 1;
    for (size_t i = 0; i < n; i += 2) {
        if (i + 1 < n) {
            out[i] = (char)ident[byte_offset + i + 1];
            out[i + 1] = (char)ident[byte_offset + i];
        } else out[i] = (char)ident[byte_offset + i];
    }
    out[n] = '\0';
    trim(out);
}

/* SCSI INQUIRY EVPD, page 0x80 (Unit Serial Number). */
static int scsi_vpd_serial(int fd, char *out, size_t out_size) {
    unsigned char cdb[6] = {0x12, 0x01, 0x80, 0x00, 0xff, 0x00};
    unsigned char data[255] = {0}, sense[64] = {0};
    sg_io_hdr_t io;
    memset(&io, 0, sizeof(io));
    io.interface_id = 'S'; io.cmd_len = sizeof(cdb); io.mx_sb_len = sizeof(sense);
    io.dxfer_direction = SG_DXFER_FROM_DEV; io.dxfer_len = sizeof(data);
    io.dxferp = data; io.cmdp = cdb; io.sbp = sense; io.timeout = 20000;
    if (ioctl(fd, SG_IO, &io) < 0 || io.host_status || io.driver_status || io.status ||
        io.sb_len_wr || data[1] != 0x80) return -1;
    size_t len = ((size_t)data[2] << 8) | data[3];
    if (len > sizeof(data) - 4) len = sizeof(data) - 4;
    if (!len || out_size < 2) return -1;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, data + 4, len);
    out[len] = '\0';
    trim(out);
    return out[0] ? 0 : -1;
}

static int read_sysfs_value(const char *path, char *out, size_t out_size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)out_size, f)) { fclose(f); return -1; }
    fclose(f);
    trim(out);
    if (!out[0]) return -1;
    return 0;
}

static int supported_disk_name(const char *name) {
    const char *p;
    if (!strncmp(name, "nvme", 4)) {
        p = name + 4;
        if (!*p) return 0;
        while (*p >= '0' && *p <= '9') p++;
        if (p == name + 4 || *p++ != 'n' || !*p) return 0;
        while (*p >= '0' && *p <= '9') p++;
        return *p == '\0';
    }
    if (!strncmp(name, "sd", 2) || !strncmp(name, "hd", 2) || !strncmp(name, "vd", 2)) {
        p = name + 2;
        if (!*p) return 0;
        while (*p >= 'a' && *p <= 'z') p++;
        return *p == '\0';
    }
    return 0;
}

static void load_disk_info(const char *name, disk_info_t *disk) {
    char path[PATH_MAX + 128], value[DISK_INFO_LEN];
    memset(disk, 0, sizeof(*disk));
    snprintf(disk->name, sizeof(disk->name), "%s", name);
    snprintf(disk->model, sizeof(disk->model), "Inconnu");
    snprintf(disk->vendor, sizeof(disk->vendor), "Inconnu");
    snprintf(disk->serial, sizeof(disk->serial), "Inconnu");
    if (!strncmp(name, "nvme", 4)) snprintf(disk->type, sizeof(disk->type), "NVMe");
    else if (!strncmp(name, "sd", 2)) snprintf(disk->type, sizeof(disk->type), "SCSI/SATA (sd)");
    else if (!strncmp(name, "hd", 2)) snprintf(disk->type, sizeof(disk->type), "IDE (hd)");
    else snprintf(disk->type, sizeof(disk->type), "Virtuel (vd)");

    snprintf(path, sizeof(path), "/sys/block/%.255s/device/model", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->model, sizeof(disk->model), "%s", value);
    snprintf(path, sizeof(path), "/sys/block/%.255s/device/vendor", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->vendor, sizeof(disk->vendor), "%s", value);
    snprintf(path, sizeof(path), "/sys/block/%.255s/device/serial", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) {
        snprintf(disk->serial, sizeof(disk->serial), "%s", value);
    } else {
        /* Préférer le numéro de série SCSI VPD page 0x80 au WWID générique. */
        int vpd_found = 0;
        if (!strncmp(name, "sd", 2)) {
            char devpath[PATH_MAX + 128];
            snprintf(devpath, sizeof(devpath), "/dev/%.255s", name);
            int fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                if (scsi_vpd_serial(fd, value, sizeof(value)) == 0) {
                    snprintf(disk->serial, sizeof(disk->serial), "%s", value);
                    vpd_found = 1;
                }
                close(fd);
            }
        }
        if (!vpd_found) {
            snprintf(path, sizeof(path), "/sys/block/%.255s/device/wwid", name);
            if (read_sysfs_value(path, value, sizeof(value)) == 0)
                snprintf(disk->serial, sizeof(disk->serial), "%s", value);
        }
    }
    if (!strncmp(name, "nvme", 4)) {
        char controller[32];
        const char *ns = strchr(name + 4, 'n');
        size_t n = ns ? (size_t)(ns - name) : 0;
        if (n < sizeof(controller)) {
            memcpy(controller, name, n);
            controller[n] = '\0';
            if (!strcmp(disk->model, "Inconnu")) {
                snprintf(path, sizeof(path), "/sys/class/nvme/%.31s/model", controller);
                if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->model, sizeof(disk->model), "%s", value);
            }
            if (!strcmp(disk->serial, "Inconnu")) {
                snprintf(path, sizeof(path), "/sys/class/nvme/%.31s/serial", controller);
                if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->serial, sizeof(disk->serial), "%s", value);
            }
            if (!strcmp(disk->vendor, "Inconnu")) {
                snprintf(path, sizeof(path), "/sys/class/nvme/%.31s/device/vendor", controller);
                if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->vendor, sizeof(disk->vendor), "%s", value);
            }
        }
    }
    if (!strcmp(disk->serial, "Inconnu") && strncmp(name, "nvme", 4)) {
        char devpath[PATH_MAX + 128];
        int fd;
        snprintf(devpath, sizeof(devpath), "/dev/%.255s", name);
        fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            if (scsi_vpd_serial(fd, value, sizeof(value)) == 0)
                snprintf(disk->serial, sizeof(disk->serial), "%s", value);
            close(fd);
        }
    }
    snprintf(path, sizeof(path), "/sys/block/%.255s/size", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long long sectors = strtoull(value, &end, 10);
        if (!errno && end != value && *end == '\0' && sectors <= UINT64_MAX / 512U)
            disk->bytes = (uint64_t)sectors * 512U;
    }
}

/* Tri naturel : sdb < sdz < sdaa, nvme2n1 < nvme10n1. */
static int disk_name_cmp(const void *a, const void *b) {
    const unsigned char *x = (const unsigned char *)((const disk_info_t *)a)->name;
    const unsigned char *y = (const unsigned char *)((const disk_info_t *)b)->name;
    while (*x && *y) {
        int dx = isdigit(*x) != 0, dy = isdigit(*y) != 0;
        if (dx != dy) return dx ? -1 : 1;
        if (dx) {
            while (x[0] == '0' && isdigit(x[1])) x++;
            while (y[0] == '0' && isdigit(y[1])) y++;
        }
        size_t lx = 0, ly = 0;
        while (x[lx] && (isdigit(x[lx]) != 0) == dx) lx++;
        while (y[ly] && (isdigit(y[ly]) != 0) == dy) ly++;
        if (lx != ly) return lx < ly ? -1 : 1;
        int c = memcmp(x, y, lx);
        if (c) return c < 0 ? -1 : 1;
        x += lx; y += ly;
    }
    return (*x != 0) - (*y != 0);
}

static size_t scan_disks(disk_info_t disks[MAX_DISKS]) {
    DIR *dir = opendir("/sys/block");
    struct dirent *entry;
    size_t count = 0;
    if (!dir) { perror("/sys/block"); return 0; }
    while ((entry = readdir(dir)) != NULL && count < MAX_DISKS) {
        if (!supported_disk_name(entry->d_name)) continue;
        load_disk_info(entry->d_name, &disks[count]);
        count++;
    }
    closedir(dir);
    qsort(disks, count, sizeof(*disks), disk_name_cmp);
    return count;
}

static int tui_menu(int fd, uint64_t size, const char *dev) {
    struct termios saved, raw;
    int raw_enabled = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    if (!raw_enabled) { fprintf(stderr, "Le mode --tui nécessite un terminal interactif sur stdin.\n"); return -1; }
    if (raw_enabled) { raw = saved; raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0; tcsetattr(STDIN_FILENO,TCSANOW,&raw); }
    ui_tui = 1; ui_device = dev; ui_size = size; ui_fd = fd; ui_begin();
    /* Fiche d'en-tête : IDENTIFY pour l'ATA, sinon fiche NVMe ou SAS/SCSI (sondages silencieux). */
    const int is_nvme = ioctl(fd, NVME_IOCTL_ID) > 0;
    unsigned char ident[512];
    card_t card;
    int have_id = 0, have_card = 0;
    media_t media;
#define TUI_PROBE() do { \
        have_id = !is_nvme && identify_quiet(fd, ident) == 0; \
        have_card = !have_id && (is_nvme ? nvme_card(fd, &card) : sas_card(fd, &card)) == 0; \
        media = media_detect(fd); \
    } while (0)
    TUI_PROBE();
    int ret=0;
    for (;;) {
        unsigned long sectors=0; if (ioctl(fd, BLKGETSIZE, &sectors) < 0) sectors=0;
        fputs("\033[H",stderr);
        print_tui_rule('T','T');
        char menu_title[256];
        snprintf(menu_title,sizeof(menu_title),"DiskMedic %s · MENU",DISKMEDIC_VERSION);
        print_tui_line(menu_title);
        print_tui_rule('M','M');
        char menu_row[PATH_MAX + 256];
        snprintf(menu_row,sizeof(menu_row),"Disque : %s",dev); print_tui_line(menu_row);
        if (have_id) {
            /* IDENTIFY : série = mots 10-19 (20 octets), firmware = mots 23-26 (8 octets), modèle = mots 27-46 (40 octets). */
            char model[41], serial[DISK_INFO_LEN + 1], fw[9], vpd_serial[DISK_INFO_LEN];
            ata_ident_string(ident, 20, 20, serial, sizeof(serial));
            ata_ident_string(ident, 46, 8, fw, sizeof(fw));
            ata_ident_string(ident, 54, 40, model, sizeof(model));
            if (scsi_vpd_serial(fd, vpd_serial, sizeof(vpd_serial)) == 0)
                snprintf(serial, sizeof(serial), "%s", vpd_serial);
            snprintf(menu_row,sizeof(menu_row),"Modèle : %s",model); print_tui_line(menu_row);
            snprintf(menu_row,sizeof(menu_row),"Série : %s",serial); print_tui_line(menu_row);
            snprintf(menu_row,sizeof(menu_row),"Firmware : %s",fw); print_tui_line(menu_row);
            uint64_t lba48=(uint64_t)word_le(ident,100)|((uint64_t)word_le(ident,101)<<16)|((uint64_t)word_le(ident,102)<<32)|((uint64_t)word_le(ident,103)<<48);
            snprintf(menu_row,sizeof(menu_row),"LBA48 secteurs : %llu",(unsigned long long)lba48); print_tui_line(menu_row);
            uint16_t sec=word_le(ident,128);
            snprintf(menu_row,sizeof(menu_row),"Sécurité ATA : %s, activée=%s, gelée=%s, verrouillée=%s, effacement avancé=%s",
              sec&1?"supporté":"inconnu",sec&2?"oui":"non",sec&8?"oui":"non",sec&4?"oui":"non",sec&0x20?"oui":"non"); print_tui_line(menu_row);
        } else if (have_card) {
            for (int i = 0; i < card.n; i++) print_tui_line(card.line[i]);
        } else print_tui_line("Identité du disque indisponible (passerelle USB ?) : essayez 1 = SMART.");
        {
            const char *fmt = log_sector_size == 512 && phys_sector_size == 512 ? "512n"
                            : log_sector_size == 512 && phys_sector_size == 4096 ? "512e"
                            : log_sector_size == 4096 && phys_sector_size == 4096 ? "4Kn" : "autre";
            char gib[32]; fmt_dec(gib, sizeof(gib), (double)size/1073741824.0, 2);
            snprintf(menu_row,sizeof(menu_row),"Capacité : %s Gio · %llu secteurs de 512 o · %s (%u/%u o)",
                     gib,(unsigned long long)sectors,fmt,log_sector_size,phys_sector_size);
            print_tui_line(menu_row);
            snprintf(menu_row,sizeof(menu_row),"Support : %s",media.why); print_tui_line(menu_row);
        }
        print_tui_rule('M','M');
        print_tui_line("1 = SMART  | 2 = Vérification lecture | 3 = Scan lent");
        print_tui_line("Destructif : 4 = Slow-repair | 5 = Écriture zéro | 6 = Secure erase | 7 = TRIM");
        print_tui_line("r = Rafraîchir | q = Quitter");
        print_tui_rule('B','B');
        fputs("\033[J",stderr);
        fputs("Sélection : ",stderr); fflush(stderr);
        int c=getchar(); if(c=='q'||c=='Q'||c==EOF) break;
        if(c=='r'||c=='R') { TUI_PROBE(); continue; }
        if(c=='1') { smart_report(fd); fflush(stdout); fprintf(stderr,"\nAppuyez sur une touche…"); getchar(); continue; }
        int mutation=(c=='4'||c=='5'||c=='6'||c=='7');
        if(c!='2'&&c!='3'&&!mutation) continue;
        if((c=='4'||c=='5') && media.kind==MEDIA_SSD) {
            /* SSD : expliquer, puis proposer l'alternative adaptée ou l'opération quand même. */
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
            char answer[16]="";
            fputc('\n',stderr);
            if(c=='5') { ssd_explain_zero(&media,0); fputs("t = TRIM à la place | z = écrire les zéros quand même | Entrée = annuler : ",stderr); }
            else { ssd_explain_repair(&media,0); fputs("s = scan lent seul (lecture) | r = réparer quand même | Entrée = annuler : ",stderr); }
            fflush(stderr);
            if(!fgets(answer,sizeof(answer),stdin)) answer[0]=0;
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&raw);
            int a=tolower((unsigned char)answer[0]);
            if(c=='5' && a=='t') c='7';
            else if(c=='4' && a=='s') c='3';
            else if(!((c=='5' && a=='z') || (c=='4' && a=='r'))) {
                fprintf(stderr,"Annulé : aucune modification. Appuyez sur une touche…"); fflush(stderr); getchar(); continue;
            }
            mutation=c!='3';
        }
        if(mutation) {
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
            int claim=-1;
            if(c=='6' && !have_id && !is_nvme) {
                /* Prévenir avant toute confirmation : rien ne sera tenté. */
                fprintf(stderr,"Secure Erase indisponible pour ce disque (SAS/SCSI ou passerelle USB) : aucune modification.\n");
                ret=-1;
            }
            else if(c=='6' && is_nvme && nvme_erase_preview(fd)<0) ret=-1;
            else if((claim=claim_device(dev))<0) ret=-1;
            else if(!confirm_mutation()) { fprintf(stderr,"Confirmation refusée.\n"); }
            else if(c=='4') ret=slow_scan(fd,size,4.0,1);
            else if(c=='5') ret=zero_disk(fd,size);
            else if(c=='6') ret=secure_erase_any(fd,size);
            else if(c=='7') ret=trim_disk(fd,size);
            if(claim>=0) close(claim);
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&raw);
        } else if(c=='2') ret=verify_disk(fd,size);
        else ret=slow_scan(fd,size,4.0,0);
        fflush(stdout);
        fprintf(stderr,"\nOpération terminée (code %d). Appuyez sur une touche…",ret); fflush(stderr); getchar(); ret=0;
    }
    if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
    ui_end(); return ret;
#undef TUI_PROBE
}

/* ---------- Exécution parallèle multi-disques et tableau de bord plein écran ---------- */
enum { JOB_RUNNING, JOB_DONE };
enum { JR_OK, JR_FAIL, JR_KILLED, JR_NOT_STARTED };
typedef struct {
    size_t disk;                        /* indice dans tui_disks */
    char dev[PATH_MAX + 128];
    char log[PATH_MAX + 300], csv[PATH_MAX + 300];
    pid_t pid;
    int state, status;
    int op;                             /* opération de ce disque (TRIM ou scan seul sur SSD) */
    struct timespec start, end;
} job_t;
static const char *op_label(int op) {
    return op == 1 ? "SMART" : op == 2 ? "Vérification" : op == 3 ? "Scan lent" : op == 4 ? "Réparation lente" :
           op == 5 ? "Écriture zéro" : op == 6 ? "Secure Erase" : op == 7 ? "TRIM" : "?";
}
static int op_mutates(int op) { return op >= 4 && op <= 7; }

static volatile sig_atomic_t dash_signal = 0;
static void dash_on_signal(int sig) { dash_signal = sig; }

static unsigned terminal_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return (unsigned)ws.ws_row;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return (unsigned)ws.ws_row;
    return 24U;
}
static unsigned utf8_width(const char *text) {
    unsigned w = 0;
    for (const unsigned char *s = (const unsigned char *)text; *s; s += utf8_char_bytes(s)) w++;
    return w;
}
/* Ligne encadrée : texte à gauche, texte éventuellement coloré aligné à droite. */
static void print_tui_line_lr(const char *left, const char *right, const char *color) {
    unsigned cols = terminal_columns(), inner = cols > 2 ? cols - 2 : 1;
    unsigned rw = right && *right ? utf8_width(right) : 0;
    if (rw + 1 >= inner) rw = 0;
    fputs("║", UI_OUT);
    print_padded(left, rw ? inner - rw - 1 : inner);
    if (rw) {
        fputc(' ', UI_OUT);
        if (color) fputs(color, UI_OUT);
        print_padded(right, rw);
        if (color) fputs("\033[0m", UI_OUT);
    }
    fputs("║\n", UI_OUT);
}
/* Fichiers créés sous sudo : rendus à l'utilisateur qui a lancé sudo, pour qu'il puisse les supprimer. */
static void give_to_sudo_user(const char *path) {
    const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
    if (!u || !g || geteuid() != 0) return;
    char *e1, *e2;
    long uid = strtol(u, &e1, 10), gid = strtol(g, &e2, 10);
    if (*e1 || *e2 || uid <= 0 || gid < 0) return;
    if (chown(path, (uid_t)uid, (gid_t)gid) < 0) { /* sans conséquence */ }
}
/* Dossier des journaux : ./diskmedic-AAAAMMJJ-HHMMSS, ou dans /tmp si le dossier courant est en lecture seule. */
static int make_log_dir(char *out, size_t n) {
    char stamp[32], cwd[PATH_MAX];
    time_t t = time(NULL);
    struct tm tm;
    if (!localtime_r(&t, &tm) || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm))
        snprintf(stamp, sizeof(stamp), "%ld", (long)t);
    const char *bases[2] = { getcwd(cwd, sizeof(cwd)) ? cwd : NULL, "/tmp" };
    for (int b = 0; b < 2; b++) {
        if (!bases[b]) continue;
        const char *sep = strcmp(bases[b], "/") ? "/" : "";
        for (int k = 0; k < 100; k++) {
            if (k) snprintf(out, n, "%s%sdiskmedic-%s-%d", bases[b], sep, stamp, k);
            else snprintf(out, n, "%s%sdiskmedic-%s", bases[b], sep, stamp);
            if (mkdir(out, 0755) == 0) { give_to_sudo_user(out); return 0; }
            if (errno != EEXIST) break;
        }
    }
    return -1;
}
/* Dernière ligne non vide d'un journal (affichée sous la barre de progression). */
static void last_log_line(const char *path, char *out, size_t n) {
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    char buf[2048];
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        off_t off = st.st_size > (off_t)sizeof(buf) - 1 ? st.st_size - (off_t)(sizeof(buf) - 1) : 0;
        ssize_t r = pread(fd, buf, sizeof(buf) - 1, off);
        if (r > 0) {
            char *end = buf + r;
            *end = '\0';
            while (end > buf && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
            char *start = end;
            while (start > buf && start[-1] != '\n' && start[-1] != '\r') start--;
            size_t l = strlen(start);
            if (l >= n) l = n - 1;
            memcpy(out, start, l);
            out[l] = '\0';
            for (char *q = out; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
        }
    }
    close(fd);
}
/* Affiche un journal, limité à ses max_lines dernières lignes. */
static void print_log_excerpt(const char *path, unsigned max_lines) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        printf("  (journal illisible : %s)\n", strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    const off_t cap = 256 * 1024;
    off_t off = st.st_size > cap ? st.st_size - cap : 0;
    size_t len = (size_t)(st.st_size - off);
    char *b = malloc(len + 1);
    if (!b || (len && full_pread(fd, b, len, off) < 0)) { free(b); close(fd); printf("  (journal illisible)\n"); return; }
    close(fd);
    b[len] = '\0';
    char *start = b, *end = b + len;
    if (off) { char *nl = strchr(b, '\n'); start = nl ? nl + 1 : b; }
    while (end > start && end[-1] == '\n') end--;
    unsigned lines = 0;
    char *from = end;
    while (from > start) {
        if (from[-1] == '\n' && ++lines >= max_lines) break;
        from--;
    }
    if (from > start) printf("  … début omis — journal complet : %s\n", path);
    fwrite(from, 1, (size_t)(end - from), stdout);
    printf("\n");
    free(b);
}
/* Ligne de progression : barre, puis texte (pourcentage, débit…). */
static void dash_bar_line(int has_prog, int running, double pct, unsigned tick, const char *tail) {
    unsigned cols = terminal_columns(), inner = cols > 2 ? cols - 2 : 1;
    unsigned tw = utf8_width(tail);
    int bw = (int)inner - 2 - 1 - (int)tw - 1;
    if (bw > 60) bw = 60;
    char *row = malloc((size_t)inner * 3 + strlen(tail) + 16);
    if (!row) return;
    size_t u = 0;
    row[u++] = ' '; row[u++] = ' ';
    if (bw >= 10 && (has_prog || running)) {
        int filled = has_prog ? (int)(pct / 100.0 * bw + 0.5) : 0;
        /* Sans mesure d'avancement (SMART, Secure Erase) : un curseur se déplace. */
        int pos = (int)(tick % (unsigned)(2 * (bw - 3)));
        if (pos > bw - 3) pos = 2 * (bw - 3) - pos;
        for (int i = 0; i < bw; i++) {
            int on = has_prog ? i < filled : (i >= pos && i < pos + 3);
            memcpy(row + u, on ? "█" : "░", 3); u += 3;
        }
        row[u++] = ' ';
    }
    snprintf(row + u, strlen(tail) + 2, "%s", tail);
    print_tui_line(row);
    free(row);
}
static const char *job_label(const job_t *j, const char **color) {
    if (j->status == JR_NOT_STARTED) { *color = "\033[1;31m"; return "NON LANCÉ"; }
    if (j->state == JOB_RUNNING) { *color = "\033[1;36m"; return "EN COURS"; }
    if (j->status == JR_OK) { *color = "\033[1;32m"; return "OK"; }
    if (j->status == JR_KILLED) { *color = "\033[1;33m"; return "INTERROMPU"; }
    *color = "\033[1;31m"; return "ÉCHEC";
}
static void dash_draw(const job_t *jobs, size_t njobs, const worker_slot_t *slots, const char *op_name,
                      const char *logdir, const struct timespec *t0, const char *footer, unsigned tick) {
    char *frame = NULL;
    size_t flen = 0;
    FILE *mf = open_memstream(&frame, &flen);
    if (!mf) return;
    ui_out = mf;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned rows = terminal_rows();
    char left[PATH_MAX + 512], right[160], el[32];
    size_t running = 0, ok = 0, bad = 0;
    for (size_t k = 0; k < njobs; k++) {
        if (jobs[k].state == JOB_RUNNING) running++;
        else if (jobs[k].status == JR_OK) ok++;
        else bad++;
    }
    fputs("\033[H", mf);
    print_tui_rule('T', 'T');
    snprintf(left, sizeof(left), "DiskMedic %s · %s · %zu disque(s) en parallèle", DISKMEDIC_VERSION, op_name, njobs);
    format_eta(elapsed_seconds(t0, &now), el, sizeof(el));
    print_tui_line_lr(left, el, NULL);
    print_tui_rule('M', 'M');
    /* Mise en page : 4 lignes par disque si l'écran le permet, sinon 2 ; une ligne reste libre en bas. */
    const unsigned fixed = 3 + 5 + 1;
    int full = rows >= fixed + 5U * (unsigned)njobs - 1U;
    size_t shown = njobs;
    if (!full && rows < fixed + 2U * (unsigned)njobs)
        shown = rows > fixed + 1 ? (rows - fixed - 1) / 2 : 0;
    for (size_t k = 0; k < shown; k++) {
        const job_t *j = &jobs[k];
        const disk_info_t *d = &tui_disks[j->disk];
        worker_slot_t w;
        worker_snapshot(&slots[k], &w);
        int run = j->state == JOB_RUNNING, has_prog = w.total > 0;
        const char *color;
        const char *label = job_label(j, &color);
        format_eta(elapsed_seconds(&j->start, run ? &now : &j->end), el, sizeof(el));
        double pct = has_prog ? 100.0 * (double)w.done / (double)w.total : 0.0;
        if (pct > 100.0) pct = 100.0;
        if (!run && j->status == JR_OK) { pct = 100.0; }
        char pcts[16], sz[32], inst[32], avg[32], eta[32], tail[256], badtxt[64];
        fmt_dec(pcts, sizeof(pcts), pct, 1);
        fmt_bytes(sz, sizeof(sz), (double)d->bytes);
        fmt_dec(inst, sizeof(inst), w.instant, 1);
        fmt_dec(avg, sizeof(avg), w.average, 1);
        format_eta(w.eta, eta, sizeof(eta));
        badtxt[0] = '\0';
        if (w.bad) snprintf(badtxt, sizeof(badtxt), "illisibles : %" PRIu64, w.bad);
        if (full && k) print_tui_rule('M', 'M');
        snprintf(left, sizeof(left), "%s · %s · %s", j->dev, d->model, sz);
        if (full || !has_prog) snprintf(right, sizeof(right), "%s · %s", label, el);
        else snprintf(right, sizeof(right), "%s %s %% · %s", label, pcts, el);
        print_tui_line_lr(left, right, color);
        if (run && has_prog) snprintf(tail, sizeof(tail), "%s %% · %s Mio/s · reste %s", pcts, inst, eta);
        else if (has_prog) snprintf(tail, sizeof(tail), "%s %% · moyenne %s Mio/s", pcts, avg);
        else if (run) snprintf(tail, sizeof(tail), "%s", w.phase[0] ? w.phase : "Démarrage");
        else snprintf(tail, sizeof(tail), "terminé");
        if (!full && w.bad) {
            size_t tl = strlen(tail);
            snprintf(tail + tl, sizeof(tail) - tl, " · %s", badtxt);
        }
        if (full) {
            snprintf(left, sizeof(left), "  Étape : %s", w.phase[0] ? w.phase : "Démarrage");
            print_tui_line_lr(left, badtxt, "\033[1;31m");
        }
        dash_bar_line(has_prog, run, pct, tick, tail);
        if (full) {
            char last[512];
            last_log_line(j->log, last, sizeof(last));
            double stale = elapsed_seconds(&w.updated, &now);
            right[0] = '\0';
            if (run && has_prog && w.updated.tv_sec && stale >= 10.0)
                snprintf(right, sizeof(right), "mis à jour il y a %.0f s", stale);
            snprintf(left, sizeof(left), "  › %s", last);
            print_tui_line_lr(left, right, "\033[33m");
        }
    }
    if (shown < njobs) {
        snprintf(left, sizeof(left), "… et %zu autre(s) disque(s) : agrandissez la fenêtre pour les voir", njobs - shown);
        print_tui_line(left);
    }
    print_tui_rule('M', 'M');
    snprintf(left, sizeof(left), "En cours : %zu · Terminés : %zu (OK : %zu, échecs : %zu)", running, ok + bad, ok, bad);
    print_tui_line(left);
    snprintf(left, sizeof(left), "Journaux : %s", logdir);
    print_tui_line(left);
    print_tui_line(footer);
    print_tui_rule('B', 'B');
    fputs("\033[J", mf);
    ui_out = NULL;
    if (fclose(mf) == 0) {
        for (size_t o = 0; o < flen;) {
            ssize_t r = write(STDERR_FILENO, frame + o, flen - o);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            o += (size_t)r;
        }
    }
    free(frame);
}
/* Opération sur un disque (exécutée dans un processus fils, sortie redirigée vers son journal). */
static int run_disk_operation(int operation, const char *dev, int mutating) {
    int fd = open(dev, (mutating ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) { perror(dev); return -1; }
    struct stat st;
    uint64_t size = 0;
    int one = -1;
    if (fstat(fd, &st) < 0) perror("fstat");
    else if (!S_ISBLK(st.st_mode)) fprintf(stderr, "Refus : %s n’est pas un périphérique bloc.\n", dev);
    else if (get_size(fd, &size) == 0) {
        ui_device = dev; ui_size = size; ui_fd = fd;
        load_sector_sizes(fd);
        if (operation == 1) one = smart_report(fd);
        else if (operation == 2) one = verify_disk(fd, size);
        else if (operation == 3) one = slow_scan(fd, size, 4.0, 0);
        else if (operation == 4) one = slow_scan(fd, size, 4.0, 1);
        else if (operation == 5) one = zero_disk(fd, size);
        else if (operation == 6) one = secure_erase_any(fd, size);
        else if (operation == 7) one = trim_disk(fd, size);
    }
    close(fd);
    return one;
}

/* Sélection multi-disques, puis exécution en parallèle (un processus par disque). */
static int tui_multi(void) {
    size_t count = scan_disks(tui_disks), cursor = 0, selected_count = 0;
    unsigned char selected[MAX_DISKS] = {0};
    struct termios saved, raw;
    int ret = 0;
    if (!count) {
        fprintf(stderr, "Aucun disque pris en charge détecté dans /sys/block.\n");
        return -1;
    }
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) < 0) {
        fprintf(stderr, "Le mode TUI nécessite un terminal interactif sur stdin.\n");
        return -1;
    }
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) { perror("tcsetattr"); return -1; }
    ui_tui = 1;
    ui_begin();
    for (;;) {
        char row[PATH_MAX + 256];
        size_t first = cursor >= 8 ? cursor - 7 : 0;
        size_t last = first + 8 < count ? first + 8 : count;
        fputs("\033[H", stderr);
        print_tui_rule('T', 'T');
        snprintf(row, sizeof(row), "DiskMedic %s · SÉLECTION MULTIPLE", DISKMEDIC_VERSION);
        print_tui_line(row);
        print_tui_rule('M', 'M');
        print_tui_line("↑/↓ déplacer | Espace sélectionner | a tout sélectionner | Entrée lancer | q quitter");
        for (size_t i = first; i < last; i++) {
            snprintf(row, sizeof(row), "%c [%c] /dev/%s — %s (%.2f Gio)",
                     i == cursor ? '>' : ' ', selected[i] ? 'x' : ' ', tui_disks[i].name,
                     tui_disks[i].model, (double)tui_disks[i].bytes / 1073741824.0);
            if (i == cursor) fputs("\033[7m", stderr);
            print_tui_line(row);
            if (i == cursor) fputs("\033[0m", stderr);
        }
        print_tui_rule('M', 'M');
        snprintf(row, sizeof(row), "Sélectionnés : %zu / %zu", selected_count, count);
        print_tui_line(row);
        print_tui_rule('B', 'B');
        fputs("\033[J", stderr);
        fflush(stderr);
        int c = getchar();
        if (c == 'q' || c == 'Q' || c == EOF) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved); ui_end(); return 0;
        }
        if (c == '\033') {
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                if (c3 == 'A' && cursor > 0) cursor--;
                else if (c3 == 'B' && cursor + 1 < count) cursor++;
            }
        } else if (c == ' ' || c == 'x' || c == 'X') {
            selected[cursor] = (unsigned char)!selected[cursor];
            selected_count = 0;
            for (size_t i = 0; i < count; i++) selected_count += selected[i] != 0;
        } else if (c == 'a' || c == 'A') {
            int all = selected_count != count;
            for (size_t i = 0; i < count; i++) selected[i] = (unsigned char)all;
            selected_count = all ? count : 0;
        } else if (c == '\r' || c == '\n') {
            if (!selected_count) continue;
            break;
        }
    }

    int operation = 0;
    for (;;) {
        fputs("\033[H", stderr);
        print_tui_rule('T', 'T');
        print_tui_line("CHOIX DE L’OPÉRATION");
        print_tui_rule('M', 'M');
        print_tui_line("1 SMART | 2 Vérification lecture | 3 Scan lent | q retour");
        print_tui_line("Destructif : 4 Réparation lente | 5 Écriture zéro | 6 Secure Erase | 7 TRIM");
        print_tui_rule('B', 'B'); fputs("\033[J", stderr); fflush(stderr);
        int c = getchar();
        if (c == 'q' || c == 'Q' || c == EOF) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved); ui_end(); return 0;
        }
        if (c >= '1' && c <= '7') { operation = c - '0'; break; }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    ui_end();
    ui_tui = 0;

    /* Opération par disque : sur les SSD, TRIM au lieu des zéros, scan seul au lieu de la réparation. */
    int disk_op[MAX_DISKS];
    for (size_t i = 0; i < MAX_DISKS; i++) disk_op[i] = operation;
    if (operation == 4 || operation == 5) {
        unsigned char is_ssd[MAX_DISKS] = {0};
        size_t nssd = 0;
        for (size_t i = 0; i < count; i++) {
            if (!selected[i]) continue;
            char dev[PATH_MAX + 128];
            snprintf(dev, sizeof(dev), "/dev/%.255s", tui_disks[i].name);
            int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            media_t m = media_detect(fd);
            close(fd);
            if (m.kind != MEDIA_SSD) continue;
            if (!nssd) fprintf(stderr, "\nSSD dans la sélection :\n");
            fprintf(stderr, "  /dev/%s — %s (%s)\n", tui_disks[i].name, tui_disks[i].model, m.why);
            is_ssd[i] = 1; nssd++;
        }
        if (nssd) {
            const media_t generic = {MEDIA_SSD, "SSD"};
            if (operation == 5) {
                ssd_explain_zero(&generic, 0);
                fputs("t = TRIM sur ces SSD (zéros sur les autres) | z = zéros partout | Entrée = annuler : ", stderr);
            } else {
                ssd_explain_repair(&generic, 0);
                fputs("s = scan lent seul sur ces SSD (réparation sur les autres) | r = réparer partout | Entrée = annuler : ", stderr);
            }
            fflush(stderr);
            char answer[16] = "";
            if (!fgets(answer, sizeof(answer), stdin)) answer[0] = 0;
            int a = tolower((unsigned char)answer[0]);
            if ((operation == 5 && a == 't') || (operation == 4 && a == 's')) {
                for (size_t i = 0; i < count; i++) if (is_ssd[i]) disk_op[i] = operation == 5 ? 7 : 3;
            } else if (!((operation == 5 && a == 'z') || (operation == 4 && a == 'r'))) {
                fprintf(stderr, "Aucune opération lancée.\n");
                return 1;
            }
        }
    }
    int mutating = 0, other_op = 0;
    size_t same = 0;
    for (size_t i = 0; i < count; i++) {
        if (!selected[i]) continue;
        if (op_mutates(disk_op[i])) mutating = 1;
        if (disk_op[i] != operation) other_op = disk_op[i]; else same++;
    }
    char op_title[64];
    if (other_op && same) snprintf(op_title, sizeof(op_title), "%s + %s (SSD)", op_label(operation), op_label(other_op));
    else snprintf(op_title, sizeof(op_title), "%s", op_label(other_op ? other_op : operation));
    const char *op_name = op_title;

    int claims[MAX_DISKS];
    for (size_t i = 0; i < MAX_DISKS; i++) claims[i] = -1;
    if (mutating) {
        /* Refuser l'ensemble si un seul disque est monté/utilisé, avant toute confirmation. */
        int busy = 0;
        for (size_t i = 0; i < count; i++) {
            if (!selected[i] || !op_mutates(disk_op[i])) continue;
            char dev[PATH_MAX + 128];
            snprintf(dev, sizeof(dev), "/dev/%.255s", tui_disks[i].name);
            claims[i] = claim_device(dev);
            if (claims[i] < 0) busy = 1;
        }
        if (!busy) {
            fprintf(stderr, "\n%s simultané sur %zu disque(s) :\n", op_name, selected_count);
            for (size_t i = 0; i < count; i++)
                if (selected[i]) fprintf(stderr, "  /dev/%s — %s → %s\n", tui_disks[i].name, tui_disks[i].model, op_label(disk_op[i]));
        }
        if (busy || !confirm_mutation()) {
            if (!busy) fprintf(stderr, "Confirmation refusée; aucune opération lancée.\n");
            else fprintf(stderr, "Aucune opération lancée.\n");
            for (size_t i = 0; i < count; i++) if (claims[i] >= 0) close(claims[i]);
            return 1;
        }
    }

    /* Une tâche par disque; l'avancement est publié dans une zone de mémoire partagée. */
    char logdir[PATH_MAX];
    job_t *jobs = calloc(selected_count, sizeof(*jobs));
    worker_slot_t *slots = mmap(NULL, selected_count * sizeof(*slots), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!jobs || slots == MAP_FAILED || make_log_dir(logdir, sizeof(logdir)) < 0) {
        fprintf(stderr, "Préparation impossible (mémoire ou dossier des journaux) : %s\n", strerror(errno));
        free(jobs);
        if (slots != MAP_FAILED) munmap(slots, selected_count * sizeof(*slots));
        for (size_t i = 0; i < count; i++) if (claims[i] >= 0) close(claims[i]);
        return 1;
    }
    size_t njobs = 0;
    for (size_t i = 0; i < count; i++) {
        if (!selected[i]) continue;
        job_t *j = &jobs[njobs++];
        j->disk = i;
        j->op = disk_op[i];
        snprintf(j->dev, sizeof(j->dev), "/dev/%.255s", tui_disks[i].name);
        snprintf(j->log, sizeof(j->log), "%s/%.255s.log", logdir, tui_disks[i].name);
        if (j->op >= 2 && j->op <= 4) snprintf(j->csv, sizeof(j->csv), "%s/%.255s.csv", logdir, tui_disks[i].name);
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fflush(stdout); fflush(stderr);
    for (size_t k = 0; k < njobs; k++) {
        job_t *j = &jobs[k];
        clock_gettime(CLOCK_MONOTONIC, &j->start);
        j->state = JOB_RUNNING;
        pid_t pid = fork();
        if (pid == 0) {
            /* Processus fils : sortie vers le journal du disque, avancement vers la mémoire partagée. */
            worker_slot = &slots[k];
            ui_tui = 0; ui_active = 0; ui_out = NULL; color_mode = 0;
            int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
            if (!freopen(j->log, "w", stdout)) _exit(1);
            setvbuf(stdout, NULL, _IOLBF, 0);
            dup2(STDOUT_FILENO, STDERR_FILENO);
            give_to_sudo_user(j->log);
            const disk_info_t *d = &tui_disks[j->disk];
            printf("diskmedic %s — %s — %s (%s, n° de série %s)\n", DISKMEDIC_VERSION, op_label(j->op), j->dev, d->model, d->serial);
            worker_publish("Démarrage", 0, 0, 0, 0, 0);
            int one = 0;
            if (j->csv[0]) {
                if (open_log(j->csv) < 0) one = -1;
                else give_to_sudo_user(j->csv);
            }
            if (!one) one = run_disk_operation(j->op, j->dev, op_mutates(j->op));
            close_log();
            if (log_failed) one = -1;
            printf("\nRésultat : %s\n", one == 0 ? "OK" : "ÉCHEC");
            fflush(stdout);
            _exit(one == 0 ? 0 : 1);
        }
        if (pid < 0) {
            fprintf(stderr, "fork : %s\n", strerror(errno));
            j->state = JOB_DONE; j->status = JR_NOT_STARTED; j->end = j->start;
        }
        j->pid = pid;
    }

    /*
     * Tableau de bord plein écran. Le clavier est lu sans signaux (Ctrl+C arrive comme une
     * touche) : une interruption est toujours confirmée avant d'arrêter les opérations.
     */
    struct sigaction sa, old_int, old_term, old_hup;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dash_on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int); sigaction(SIGTERM, &sa, &old_term); sigaction(SIGHUP, &sa, &old_hup);
    struct termios dash = saved;
    dash.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    dash.c_cc[VMIN] = 1; dash.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &dash);
    ui_tui = 1;
    ui_begin();
    int draw = ui_active, asking = 0, stdin_dead = 0, hung_up = 0;
    const char *notice = NULL;
    unsigned tick = 0;
    for (;;) {
        int st;
        pid_t p;
        while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
            for (size_t k = 0; k < njobs; k++) {
                if (jobs[k].pid != p || jobs[k].state != JOB_RUNNING) continue;
                jobs[k].state = JOB_DONE;
                clock_gettime(CLOCK_MONOTONIC, &jobs[k].end);
                jobs[k].status = WIFEXITED(st) ? (WEXITSTATUS(st) == 0 ? JR_OK : JR_FAIL) : JR_KILLED;
                if (!draw) {
                    const char *c;
                    fprintf(stderr, "[%s] %s — %s\n", job_label(&jobs[k], &c), jobs[k].dev, op_label(jobs[k].op));
                }
            }
        }
        size_t running = 0;
        for (size_t k = 0; k < njobs; k++) running += jobs[k].state == JOB_RUNNING;
        int stop_now = 0;
        if (dash_signal == SIGTERM || dash_signal == SIGHUP) {
            if (dash_signal == SIGHUP) { hung_up = 1; draw = 0; }
            stop_now = 1; dash_signal = 0;
        } else if (dash_signal == SIGINT) { asking = 1; dash_signal = 0; }
        if (stop_now && running) {
            if (operation == 6) notice = "Secure Erase ne peut pas être interrompu : le disque exécute la commande lui-même.";
            else for (size_t k = 0; k < njobs; k++) if (jobs[k].state == JOB_RUNNING) kill(jobs[k].pid, SIGTERM);
        }
        const char *footer = !running ? "Terminé — appuyez sur une touche pour afficher les rapports et le résumé"
                           : asking ? "Interrompre toutes les opérations en cours ? o = oui, autre touche = non"
                           : notice ? notice : "q = interrompre toutes les opérations";
        if (draw) dash_draw(jobs, njobs, slots, op_name, logdir, &t0, footer, tick++);
        if (!running) break;
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        int pr = poll(&pfd, stdin_dead ? 0 : 1, 250);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            unsigned char ch;
            ssize_t r = read(STDIN_FILENO, &ch, 1);
            if (r <= 0) { if (r == 0 || errno != EINTR) stdin_dead = 1; }
            else if (asking) {
                asking = 0;
                if (ch == 'o' || ch == 'O') {
                    if (operation == 6) notice = "Secure Erase ne peut pas être interrompu : le disque exécute la commande lui-même.";
                    else {
                        for (size_t k = 0; k < njobs; k++) if (jobs[k].state == JOB_RUNNING) kill(jobs[k].pid, SIGTERM);
                        notice = "Interruption demandée…";
                    }
                }
            } else if (ch == 'q' || ch == 'Q' || ch == 3) asking = 1;
        } else if (pr > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) stdin_dead = 1;
    }
    if (draw && !stdin_dead) {
        unsigned char ch;
        while (read(STDIN_FILENO, &ch, 1) < 0 && errno == EINTR && !dash_signal) {}
    }
    if (!hung_up) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    ui_end();
    ui_tui = 0;
    sigaction(SIGINT, &old_int, NULL); sigaction(SIGTERM, &old_term, NULL); sigaction(SIGHUP, &old_hup, NULL);
    for (size_t i = 0; i < count; i++) if (claims[i] >= 0) close(claims[i]);

    /* Rapports de chaque disque (journaux), puis résumé. */
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (size_t k = 0; k < njobs; k++) {
        const char *c;
        char el[32];
        format_eta(elapsed_seconds(&jobs[k].start, &jobs[k].end), el, sizeof(el));
        printf("\n━━ %s · %s · %s en %s ━━\n", jobs[k].dev, tui_disks[jobs[k].disk].model, job_label(&jobs[k], &c), el);
        if (jobs[k].status != JR_NOT_STARTED) print_log_excerpt(jobs[k].log, 150);
    }
    fflush(stdout);
    char total_el[32];
    format_eta(elapsed_seconds(&t0, &t1), total_el, sizeof(total_el));
    fprintf(stderr, "\n=== Résumé : %s · %zu disque(s) en parallèle · %s ===\n", op_name, njobs, total_el);
    for (size_t k = 0; k < njobs; k++) {
        const char *c;
        const char *label = job_label(&jobs[k], &c);
        char el[32], extra[160] = "";
        worker_slot_t w;
        worker_snapshot(&slots[k], &w);
        format_eta(elapsed_seconds(&jobs[k].start, &jobs[k].end), el, sizeof(el));
        if (jobs[k].status == JR_KILLED && w.total) {
            char pcts[16];
            fmt_dec(pcts, sizeof(pcts), 100.0 * (double)w.done / (double)w.total, 1);
            snprintf(extra, sizeof(extra), " · arrêté à %s %% (%.60s)", pcts, w.phase);
        } else if (w.bad) snprintf(extra, sizeof(extra), " · %" PRIu64 " secteur(s) illisible(s)", w.bad);
        fprintf(stderr, "  [%s]%*s %s — %s (%s)%s%s%s\n", label, (int)(10 - utf8_width(label)), "",
                jobs[k].dev, tui_disks[jobs[k].disk].model, el, extra,
                jobs[k].op != operation ? " · " : "", jobs[k].op != operation ? op_label(jobs[k].op) : "");
        if (jobs[k].status != JR_OK) ret = 1;
    }
    fprintf(stderr, "Journaux : %s\n", logdir);
    munmap(slots, selected_count * sizeof(*slots));
    free(jobs);
    return ret;
}

int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--version")) { printf("DiskMedic %s\n",DISKMEDIC_VERSION); return 0; }
    if (argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) { usage(stdout); return 0; }
    options_t o; if(parse_options(argc,argv,&o)<0) { usage(stderr); return 2; }
    if (o.tui && !isatty(STDIN_FILENO)) {
        fprintf(stderr, "Le mode TUI nécessite un terminal interactif sur stdin.\n\n");
        usage(stderr); return 2;
    }
    if (o.tui && !o.device) {
        int tui_rc = tui_multi();
        return tui_rc < 0 ? 1 : tui_rc;
    }
    int mutating=o.secure_erase||o.zero||o.slow_repair||o.trim;
    int flags=((mutating || o.tui)?O_RDWR:O_RDONLY)|O_CLOEXEC;
    int fd=open(o.device,flags);
    if(fd<0) { perror(o.device); return 1; }
    struct stat st;
    if(fstat(fd,&st)<0) { perror("fstat");close(fd);return 1; }
    if(!S_ISBLK(st.st_mode)) { fprintf(stderr,"Refus : %s n'est pas un périphérique bloc.\n",o.device);close(fd);return 1; }
    uint64_t size; if(get_size(fd,&size)<0){close(fd);return 1;}
    load_sector_sizes(fd);
    ui_device=o.device; ui_size=size; ui_fd=fd; ui_tui=o.tui;
    if(o.tui) { int rc=tui_menu(fd,size,o.device); close(fd); return rc<0?1:rc; }
    /* SSD : pas d'écriture zéro ni de réécriture des zones lentes sans --force. */
    if(o.zero || o.slow_scan) {
        media_t media=media_detect(fd);
        if(media.kind==MEDIA_SSD) {
            if(o.zero && !o.force) { ssd_explain_zero(&media,1); close(fd); return 1; }
            if(o.slow_repair && !o.force) { ssd_explain_repair(&media,1); close(fd); return 1; }
            if(o.force) fprintf(stderr,"--force : opération lancée malgré le support (%s).\n",media.why);
            else fprintf(stderr,"Note : %s. Sur un SSD, une lecture lente signale le contrôleur ou la flash, "
                                "pas une surface abîmée; consultez aussi --smart.\n",media.why);
        }
    }
    if(o.secure_erase && ioctl(fd,NVME_IOCTL_ID)>0 && nvme_erase_preview(fd)<0) { close(fd); return 1; }
    /* Disque validé et revendiqué en exclusivité avant de demander la confirmation. */
    int claim=-1;
    if(mutating) {
        claim=claim_device(o.device);
        if(claim<0) { close(fd); return 1; }
        if(!confirm_mutation()) {
            fprintf(stderr,"Confirmation absente ou incorrecte; aucune opération effectuée.\n");
            close(claim); close(fd); return 2;
        }
    }
    if(o.log_path && open_log(o.log_path)<0) { if(claim>=0) close(claim); close(fd); return 1; }
    int rc=0;
    if(o.smart_only) rc=smart_report(fd)<0?1:0;
    if(!rc && o.secure_erase) rc=secure_erase_any(fd,size)<0?1:0;
    if(!rc && o.zero) rc=zero_disk(fd,size)<0?1:0;
    if(!rc && o.trim) rc=trim_disk(fd,size)<0?1:0;
    if(!rc && o.verify) rc=verify_disk(fd,size)<0?1:0;
    if(!rc && o.slow_scan) rc=slow_scan(fd,size,o.slow_factor,o.slow_repair)<0?1:0;
    if(claim>=0) close(claim);
    close(fd); close_log(); if(log_failed) rc=1; return rc;
}
