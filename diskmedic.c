/*
 * DiskMedic — diagnostics and maintenance for SATA, SAS and NVMe disks.
 * Copyright (c) 2026 The DiskMedic contributors
 * SPDX-License-Identifier: MIT — see the LICENSE file.
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

#define DISKMEDIC_VERSION "1.14.0"
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
    int smart_only, secure_erase, zero, verify, slow_scan, slow_repair, tui, trim, force, no_color;
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

/* State of a block reported by the slow-block scan. */
enum {
    SB_UNREADABLE,      /* read error */
    SB_CONFIRMED,       /* slow on passes 1 and 2, slowness located */
    SB_INTERMITTENT,    /* slow on one pass only: not confirmed */
    SB_NOT_REPRODUCED,  /* slow on both passes but not during location */
    SB_TRANSIENT        /* read error on pass 1, readable again on pass 2 */
};
typedef struct { uint64_t index; double ms, ms1; int status; } slow_block_t;

static const char *ui_device = "(unknown)";
static int ui_fd = -1;
static uint64_t ui_size = 0;
static int ui_tui = 0;
static volatile sig_atomic_t ui_active = 0;   /* alternate screen shown, cursor hidden */
typedef struct {
    struct timespec start, last;
    uint64_t last_bytes;
    int initialized;
} progress_t;
/* TUI drawing output: stderr, or a memory buffer used to compose a whole screen image. */
static FILE *ui_out = NULL;
#define UI_OUT (ui_out ? ui_out : stderr)

/* ---------- Terminal: restore on exit or fatal signal, key reading, colors, glyphs ---------- */
static struct termios term_saved;
static volatile sig_atomic_t term_saved_ok = 0;
static volatile sig_atomic_t ui_resized = 0;
static int opt_no_color = 0;

/* Async-signal-safe: leaves the alternate screen and gives back the saved terminal mode. */
static void term_leave_screen(void) {
    if (ui_active) {
        static const char s[] = "\033[0m\033[?25h\033[?1049l\n";
        ssize_t r = write(STDERR_FILENO, s, sizeof(s) - 1);
        (void)r;
        ui_active = 0;
    }
}
static void term_restore(void) {
    term_leave_screen();
    if (term_saved_ok) tcsetattr(STDIN_FILENO, TCSANOW, &term_saved);
}
static void term_on_fatal(int sig) {
    term_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}
static void term_on_winch(int sig) { (void)sig; ui_resized = 1; }
/* Remembers the terminal mode to restore, and installs the handlers (once). */
static void term_remember(const struct termios *saved) {
    static int installed = 0;
    term_saved = *saved;
    term_saved_ok = 1;
    if (installed) return;
    installed = 1;
    atexit(term_restore);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = term_on_fatal;
    const int fatal[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        struct sigaction old;
        if (sigaction(fatal[i], NULL, &old) == 0 && old.sa_handler == SIG_DFL) sigaction(fatal[i], &sa, NULL);
    }
    sa.sa_handler = term_on_winch;          /* no SA_RESTART: a blocked read_key() wakes up */
    sigaction(SIGWINCH, &sa, NULL);
}
/* In a child process: no terminal to restore, default signal behaviour. */
static void term_forget(void) {
    term_saved_ok = 0;
    ui_active = 0;
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
    signal(SIGQUIT, SIG_DFL); signal(SIGWINCH, SIG_DFL);
}

enum { KEY_UP = 0x100, KEY_DOWN, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, KEY_ESC, KEY_RESIZE };
/* One byte from stdin, or -1 after timeout_ms (-1 = wait forever), -2 at end of input. */
static int read_byte(int timeout_ms) {
    for (;;) {
        if (timeout_ms < 0 && ui_resized) return -1;
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        int r = poll(&p, 1, timeout_ms);
        if (r < 0) { if (errno == EINTR) { if (timeout_ms < 0 && ui_resized) return -1; continue; } return -2; }
        if (r == 0) return -1;
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -2;
        return c;
    }
}
/*
 * Next key: a byte, or KEY_* for arrows, Page Up/Down, Home/End, a lone Escape and a
 * window resize; EOF at end of input. A lone Escape no longer waits for a following key.
 */
static int read_key(void) {
    for (;;) {
        if (ui_resized) { ui_resized = 0; return KEY_RESIZE; }
        int c = read_byte(-1);
        if (c == -2) return EOF;
        if (c == -1) continue;
        if (c != 0x1b) return c;
        int c2 = read_byte(100);
        if (c2 == -2) return EOF;
        if (c2 != '[' && c2 != 'O') return KEY_ESC;
        int c3 = read_byte(100);
        if (c3 < 0) return KEY_ESC;
        switch (c3) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: break;
        }
        if (c3 >= '0' && c3 <= '9') {
            int num = c3 - '0', t;
            while ((t = read_byte(100)) >= '0' && t <= '9') num = num * 10 + (t - '0');
            while (t >= 0 && t != '~' && !(t >= 0x40 && t <= 0x7e)) t = read_byte(100);
            if (t == '~') {
                if (num == 1 || num == 7) return KEY_HOME;
                if (num == 4 || num == 8) return KEY_END;
                if (num == 5) return KEY_PGUP;
                if (num == 6) return KEY_PGDN;
            }
        }
        /* Other sequences (function keys…) are ignored. */
    }
}
/* "Press any key": a window resize does not count as a key. */
static void wait_any_key(void) {
    int c;
    do c = read_key(); while (c == KEY_RESIZE);
}

/* TUI colors (stderr): disabled by --no-color, NO_COLOR, TERM=dumb or a non-terminal stderr. */
#define COL_RED    "\033[1;31m"
#define COL_YELLOW "\033[1;33m"
#define COL_GREEN  "\033[1;32m"
#define COL_CYAN   "\033[1;36m"
#define COL_DIM    "\033[2m"
#define COL_BOLD   "\033[1m"
#define COL_REV    "\033[7m"
#define COL_RESET  "\033[0m"
static int ui_color_mode = -1;
static int ui_color_on(void) {
    if (ui_color_mode < 0) {
        const char *t = getenv("TERM");
        ui_color_mode = !opt_no_color && isatty(STDERR_FILENO) && !getenv("NO_COLOR") && !(t && !strcmp(t, "dumb"));
    }
    return ui_color_mode;
}
static const char *ui_col(const char *seq) { return ui_color_on() ? seq : ""; }

/* Frame and bar glyphs: Unicode with a UTF-8 locale, plain ASCII otherwise. */
typedef struct { const char *tl, *tr, *bl, *br, *ml, *mr, *h, *v, *full, *empty; } glyphs_t;
static const glyphs_t glyphs_utf8 = { "╔", "╗", "╚", "╝", "╠", "╣", "═", "║", "█", "░" };
static const glyphs_t glyphs_ascii = { "+", "+", "+", "+", "+", "+", "-", "|", "#", "." };
static const glyphs_t *ui_glyphs(void) {
    static const glyphs_t *g = NULL;
    if (!g) {
        const char *vars[3] = { "LC_ALL", "LC_CTYPE", "LANG" }, *loc = "";
        for (int i = 0; i < 3; i++) { const char *v = getenv(vars[i]); if (v && *v) { loc = v; break; } }
        int utf8 = 0;
        for (const char *p = loc; *p && !utf8; p++)
            utf8 = (tolower((unsigned char)p[0]) == 'u' && tolower((unsigned char)p[1]) == 't' &&
                    tolower((unsigned char)p[2]) == 'f' && (p[3] == '8' || (p[3] == '-' && p[4] == '8')));
        g = utf8 ? &glyphs_utf8 : &glyphs_ascii;
    }
    return g;
}

/*
 * Parallel execution (multi-disk selection): each disk is handled by a child process
 * that publishes its progress in a shared memory area, read by the dashboard.
 * seq is odd during an update (sequence lock: wait-free reads).
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
        "DiskMedic %s — Linux disk diagnostics and maintenance tool (ATA/SAS/NVMe/block)\n\n"
        "Usage:\n"
        "  sudo diskmedic /dev/sdX [OPTIONS]\n"
        "  diskmedic                          Opens the interactive disk selection\n"
        "  diskmedic --tui [ /dev/sdX ]      Interactive menu (no disk: multi-selection)\n"
        "  diskmedic --help | --version\n\n"
        "Read-only operations (one operation per run):\n"
        "  --smart             Disk identity + readable SMART report (SATA, SAS and NVMe)\n"
        "  --verify            Full read of the disk (no writes)\n"
        "  --scan-slow         Slow blocks: 2 confirmation passes + location in 64 KiB zones\n\n"
        "Destructive operations (typing exactly YES is required to confirm):\n"
        "  --slow-repair       Scan, then rewrite confirmed slow zones only\n"
        "  --zero              Writes zeros over the whole disk (refused on SSD)\n"
        "  --trim              TRIM of the whole disk: empties an SSD without wear\n"
        "  --secure-erase      Firmware erase: ATA Security Erase, or NVMe Sanitize /\n"
        "                      Format NVM chosen from the disk's capabilities\n\n"
        "Options:\n"
        "  --slow-factor N     Threshold = median × N (default: 4)\n"
        "  --log FILE          CSV log of bad/slow sectors (scan/repair)\n"
        "  --force             With --zero or --slow-repair: run anyway on an SSD\n"
        "  --tui               Interactive menu; without a disk: disks processed in parallel\n"
        "  --no-color          No colors (the NO_COLOR environment variable does the same)\n"
        "  --help, -h          Shows this help\n"
        "  --version           Shows the version\n\n"
        "Interactive interface (TUI):\n"
        "  Selection: ↑/↓ move, Space select, a select all, Enter start, q quit\n"
        "  Menus: ↑/↓ + Enter, or 1-7 directly; q goes back\n"
        "  Dashboard: ↑/↓ choose a disk, Enter view its live log, q interrupt all\n"
        "  Reports and logs: ↑/↓ scroll, q or Esc to close\n"
        "  PgUp/PgDn and Home/End work in every list\n\n"
        "Example: sudo diskmedic /dev/sdX --smart\n", DISKMEDIC_VERSION);
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
        else if (!strcmp(argv[i], "--no-color")) o->no_color = 1;
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
    /* stdout redirected to a file: the screen is still on stderr. */
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
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
/* Length of an ANSI CSI sequence ("\033[1;31m") at s, 0 if there is none: it takes no column. */
static size_t ansi_seq_len(const unsigned char *s) {
    if (s[0] != 0x1b || s[1] != '[') return 0;
    size_t n = 2;
    while (s[n] && (s[n] < 0x40 || s[n] > 0x7e)) n++;
    return s[n] ? n + 1 : n;
}
/* Writes text on exactly width columns; returns 1 if it contained color sequences. */
static int print_padded(const char *text, unsigned width) {
    const unsigned char *s = (const unsigned char *)text;
    unsigned used = 0;
    int esc = 0;
    while (*s && used < width) {
        size_t a = ansi_seq_len(s);
        if (a) { (void)fwrite(s, 1, a, UI_OUT); s += a; esc = 1; continue; }
        size_t n = utf8_char_bytes(s);
        if (n > width - used) break;
        (void)fwrite(s, 1, n, UI_OUT);
        s += n;
        used++;
    }
    /* Text cut off: its color sequences (a final reset…) are still written. */
    while (*s) {
        size_t a = ansi_seq_len(s);
        if (a) { (void)fwrite(s, 1, a, UI_OUT); s += a; esc = 1; } else s++;
    }
    while (used++ < width) fputc(' ', UI_OUT);
    return esc;
}
/* Framed line; attr (color, reverse video…) applies inside the frame only. */
static void print_tui_line_attr(const char *text, const char *attr) {
    unsigned cols = terminal_columns();
    const glyphs_t *g = ui_glyphs();
    int on = attr && *attr;
    if (cols >= 2) {
        fputs(g->v, UI_OUT);
        if (on) fputs(attr, UI_OUT);
        int esc = print_padded(text, cols - 2);
        if (on || esc) fputs(COL_RESET, UI_OUT);
        fprintf(UI_OUT, "%s\n", g->v);
    } else {
        print_padded(text, cols);
        fputc('\n', UI_OUT);
    }
}
static void print_tui_line(const char *text) { print_tui_line_attr(text, NULL); }
static void print_tui_rule(char left, char right) {
    unsigned cols = terminal_columns();
    const glyphs_t *g = ui_glyphs();
    if (cols >= 2) {
        fputs(left == 'T' ? g->tl : left == 'B' ? g->bl : g->ml, UI_OUT);
        for (unsigned i = 2; i < cols; i++) fputs(g->h, UI_OUT);
        fprintf(UI_OUT, "%s\n", right == 'T' ? g->tr : right == 'B' ? g->br : g->mr);
    }
}
static void dash_bar_line(int has_prog, int running, double pct, unsigned tick, const char *tail, const char *color);
/* Start of a step: in parallel mode, the step is published to the dashboard immediately. */
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
        print_tui_rule('T','T');
        print_tui_line("DiskMedic · MONITOR");
        print_tui_rule('M','M');
        char row[PATH_MAX + 128];
        snprintf(row,sizeof(row),"Disk: %s",ui_device); print_tui_line(row);
        snprintf(row,sizeof(row),"Step: %s",phase); print_tui_line(row);
        print_tui_rule('M','M');
        snprintf(row,sizeof(row),"%.2f %%",pct);
        dash_bar_line(1,1,pct,0,row,ui_col(COL_CYAN));
        snprintf(row,sizeof(row),"Current rate: %.2f MiB/s | Average: %.2f MiB/s",instant,average); print_tui_line(row);
        snprintf(row,sizeof(row),"Processed: %.2f / %.2f MiB | Time left: %s",(double)done/1048576.0,(double)total/1048576.0,etastr); print_tui_line(row);
        print_tui_rule('B','B');
        fputs("\033[J", stderr);
    } else {
        unsigned cols=terminal_columns();
        char tail[160];
        snprintf(tail,sizeof(tail)," %6.2f%% | %.2f/%.2f MiB/s | ETA %s",pct,instant,average,etastr);
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
        print_tui_line("DiskMedic — TERMINAL MONITOR");
        char row[PATH_MAX + 128];
        snprintf(row,sizeof(row),"Disk: %s",ui_device); print_tui_line(row);
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
    print_tui_line("DiskMedic · MONITOR");
    print_tui_rule('M','M');
    snprintf(row,sizeof(row),"Disk: %s",ui_device); print_tui_line(row);
    snprintf(row,sizeof(row),"Step: %s",phase); print_tui_line(row);
    snprintf(row,sizeof(row),"Info: %s",detail); print_tui_line(row);
    print_tui_rule('B','B');
    fputs("\033[J",stderr);
    fflush(stderr);
}
static void ui_end(void) {
    if (ui_active) { fflush(stderr); term_leave_screen(); }
}
static int confirm_mutation(void) {
    char line[64];
    fprintf(stderr, "WARNING: this operation can irreversibly destroy all data.\nType exactly YES to continue: ");
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    line[strcspn(line, "\r\n")] = 0;
    return strcmp(line, "YES") == 0;
}
static int get_size(int fd, uint64_t *size) {
    unsigned long long n = 0;
    if (ioctl(fd, BLKGETSIZE64, &n) < 0) { perror("BLKGETSIZE64"); return -1; }
    *size = (uint64_t)n; return 0;
}
/* Logical (LBA addressing) and physical (the disk's real write unit) sector sizes. */
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
 * Disk access: a second O_DIRECT descriptor bypasses the page cache so that
 * reads and measured times reflect the physical medium. Accesses not aligned
 * on the logical sector (end of disk) go through the buffered descriptor.
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
/* Buffer aligned for O_DIRECT (at least 4096 and at least the logical sector size). */
static unsigned char *alloc_aligned(size_t n) {
    void *p = NULL;
    size_t a = log_sector_size > 4096U ? log_sector_size : 4096U;
    return posix_memalign(&p, a, n) ? NULL : (unsigned char *)p;
}
/* Drops buffered data from the page cache to force a physical re-read. */
static void drop_cache(int fd, uint64_t off, uint64_t len) {
    (void)posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_DONTNEED);
}

/* Claims the disk exclusively (O_EXCL): fails if it is mounted or in use (swap, RAID, LVM…). */
static int claim_device(const char *dev) {
    int fd = open(dev, O_RDONLY | O_EXCL | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EBUSY)
            fprintf(stderr, "Refused: %s is in use (mounted partition, swap, RAID/LVM…). "
                            "Unmount it before any destructive operation.\n", dev);
        else perror(dev);
    }
    return fd;
}

static int zero_disk(int fd, uint64_t size) {
    disk_io_t io; dio_open(&io, fd, 1);
    unsigned char *buf = alloc_aligned(BUF_SIZE), *verify = alloc_aligned(BUF_SIZE);
    int rc = -1;
    if (!buf || !verify) { perror("aligned allocation"); goto out; }
    memset(buf, 0, BUF_SIZE);
    fprintf(stderr, "Writing zeros over %" PRIu64 " bytes… (%s)\n", size, io.dfd >= 0 ? "O_DIRECT" : "buffered I/O");
    progress_t progress; progress_begin(&progress, "Writing", size);
    for (uint64_t off = 0; off < size;) {
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        if (dio_pwrite(&io, buf, n, off) < 0) {
            fprintf(stderr, "\nWrite failed at 0x%" PRIx64 ": %s\n", off, strerror(errno)); goto out;
        }
        off += n;
        if ((off % (16ULL * 1024 * 1024) < BUF_SIZE) || off == size)
            progress_show(&progress, "Writing", off, size);
    }
    fprintf(stderr, "\nSyncing and verifying…\n");
    if (fdatasync(fd) < 0) { perror("fdatasync"); goto out; }
    drop_cache(fd, 0, 0);
    progress_begin(&progress, "Verification", size);
    for (uint64_t off = 0; off < size;) {
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        if (dio_pread(&io, verify, n, off) < 0) {
            fprintf(stderr, "\nRead-back failed at 0x%" PRIx64 ": %s\n", off, strerror(errno)); goto out;
        }
        for (size_t i = 0; i < n; i++) if (verify[i] != 0) {
            fprintf(stderr, "\nVerification failed: non-zero data at 0x%" PRIx64 "\n", off + i); goto out;
        }
        off += n;
        if ((off % (16ULL * 1024 * 1024) < BUF_SIZE) || off == size)
            progress_show(&progress, "Verification", off, size);
    }
    fprintf(stderr, "\nZero-fill + verification complete.\n");
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
    fprintf(stderr, "CSV log write error: %s\n", strerror(errno ? errno : EIO));
    return -1;
}
static int open_log(const char *path) {
    log_file = fopen(path, "a+");
    if (!log_file) { perror(path); return -1; }
    if (fseek(log_file, 0, SEEK_END) != 0) { perror("log seek"); fclose(log_file); log_file=NULL; return -1; }
    long end = ftell(log_file);
    if (end < 0) { perror("log size"); fclose(log_file); log_file=NULL; return -1; }
    if (end == 0) {
        if (fputs("timestamp_iso8601,device,operation,LBA,status,action\n", log_file) == EOF || fflush(log_file) != 0) {
            perror("CSV log header"); fclose(log_file); log_file=NULL; return -1;
        }
    }
    log_path_active = path;
    fprintf(stderr, "CSV log: %s\n", path);
    return 0;
}
static void close_log(void) {
    if (!log_file) return;
    fprintf(stderr, "CSV log complete: %" PRIu64 " entr%s written to %s%s\n",
            log_entries, log_entries == 1 ? "y" : "ies", log_path_active ? log_path_active : "(unknown)", log_failed ? " (write error)" : "");
    if (fclose(log_file) != 0) { perror("closing CSV log"); log_failed=1; }
    log_file=NULL;
}

/* Records an unreadable logical sector (zeros in buf, badmap, message, log). */
static int mark_bad_sector(unsigned char *buf, size_t s, size_t len, uint64_t off, int err,
                           const char *operation, const char *action, int log, unsigned char *badmap) {
    const size_t ss = log_sector_size;
    memset(buf + s, 0, len);
    if (badmap) badmap[(s / ss) / 8] |= (unsigned char)(1U << ((s / ss) % 8));
    if (log) worker_add_bad();
    fprintf(stderr, "Unreadable sector: LBA %" PRIu64 " (%s)\n", (off + s) / ss, strerror(err));
    if (log && log_event(operation, (off + s) / ss, "DEFECTIVE: read failed", action) < 0) return -1;
    return 0;
}

/*
 * Re-reads a block whose full read failed, to locate the exact LBAs.
 * Unreadable sectors are filled with zeros in buf; if badmap is not NULL,
 * bit i is set for each unreadable logical sector i of the block.
 *
 * The disk always reads and corrects a whole PHYSICAL sector, and each failed read
 * can cost it several seconds of retries. The re-read is therefore done physical
 * sector by physical sector first; only a failed physical sector is examined
 * logical sector by logical sector (512e). If its first AND last logical sectors
 * are unreadable, the whole physical sector is considered lost without reading
 * the others: 3 failed reads instead of 8 on a 512e disk.
 *
 * Returns the number of unreadable logical sectors, or UINT64_MAX if logging fails.
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
            /* Physical sector entirely lost: all its logical sectors are lost too. */
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
    progress_t progress; progress_begin(&progress, "Verification", size);
    fprintf(stderr, "Full verification read (%" PRIu64 " bytes, %s)…\n", size, io.dfd >= 0 ? "O_DIRECT" : "buffered I/O");
    for (uint64_t off = 0; off < size;) {
        size_t n = size - off > BUF_SIZE ? BUF_SIZE : (size_t)(size - off);
        if (dio_pread(&io, buf, n, off) < 0) {
            fprintf(stderr, "\nRead error in the block at 0x%" PRIx64 " (%s); checking sector by sector…\n", off, strerror(errno));
            uint64_t b = read_sectors(&io, buf, n, off, "verify", "NONE", 1, NULL);
            if (b == UINT64_MAX) { free(buf); dio_close(&io); return -1; }
            bad += b;
        }
        off += n;
        if (off % (16ULL*1024*1024) < BUF_SIZE || off == size) progress_show(&progress, "Verification", off, size);
    }
    free(buf); dio_close(&io);
    if (bad) {
        fprintf(stderr, "\nVerification complete: %" PRIu64 " unreadable sector(s) found.\n", bad);
        return -1;
    }
    fprintf(stderr, "\nVerification complete: no read errors found.\n");
    return 0;
}

static char *trim(char *s);
static void ata_ident_string(const unsigned char *ident, size_t byte_offset,
                             size_t byte_length, char *out, size_t out_size);
static int scsi_vpd_serial(int fd, char *out, size_t out_size);
/* Set to 1 during probes (disk type detection): no ATA error messages. */
static int ata_quiet = 0;

/* ATA registers returned by the SAT bridge (descriptor 09h or fixed sense 00/1Dh). */
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
        /* SAT fixed format: ASC/ASCQ 00/1D "ATA pass-through information available". */
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
    /* EXTEND=0: all commands used here are 28-bit ATA commands. */
    cdb[0] = 0x85; cdb[1] = (uint8_t)((protocol & 0x0f) << 1); cdb[2] = flags;
    cdb[4] = feature; cdb[6] = count; cdb[8] = lba_low; cdb[10] = lba_mid; cdb[12] = lba_high;
    cdb[13] = 0x40; cdb[14] = command;
    sg_io_hdr_t io; memset(&io, 0, sizeof(io));
    io.interface_id = 'S'; io.cmd_len = sizeof(cdb); io.mx_sb_len = sizeof(sense);
    io.dxfer_direction = direction; io.dxfer_len = data_len; io.dxferp = data;
    io.cmdp = cdb; io.sbp = sense; io.timeout = timeout_ms;
    if (ioctl(fd, SG_IO, &io) < 0) { if (!ata_quiet) perror("ioctl(SG_IO)"); return -1; }
    /* DRIVER_SENSE (0x08) only signals that sense data is present; it is parsed below. */
    if (io.host_status || (io.driver_status & ~0x08U)) {
        if (!ata_quiet)
            fprintf(stderr, "ATA command 0x%02x failed (host=0x%x, driver=0x%x).\n", command, io.host_status, io.driver_status);
        errno = EIO; return -1;
    }
    ata_regs_t regs;
    parse_ata_sense(sense, io.sb_len_wr, &regs);
    if (io.status && (!regs.valid || (regs.status & 0x21))) {
        if (!ata_quiet) {
            fprintf(stderr, "ATA command 0x%02x failed (SCSI status=0x%x, ATA status=0x%02x, sense=", command, io.status, regs.status);
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
    /* PIO Data-In; flags 0x0e = T_DIR (to host) | BYT_BLOK (in blocks) | T_LENGTH=sector count. */
    return ata_pt(fd, 4, 0x0e, 0, 1, 0, 0, ATA_IDENTIFY, data, 512, SG_DXFER_FROM_DEV, 20000);
}
/* Silent IDENTIFY: used to detect whether the disk speaks ATA (otherwise SAS/SCSI or NVMe). */
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
 * SMART RETURN STATUS: the firmware's own assessment. With CK_COND (0x20), the bridge
 * returns the registers: LBA mid/high 4Fh/C2h = OK, F4h/2Ch = threshold exceeded.
 * Returns 1 = OK, 0 = failure predicted, -1 = unknown.
 */
static int smart_health(int fd) {
    ata_regs_t r;
    if (ata_pt_regs(fd, 3, 0x20, SMART_RETURN_STATUS, 0, 0, 0x4f, 0xc2, ATA_SMART, NULL, 0,
                    SG_DXFER_NONE, 30000, &r) < 0 || !r.valid) return -1;
    if (r.lba_mid == 0x4f && r.lba_high == 0xc2) return 1;
    if (r.lba_mid == 0xf4 && r.lba_high == 0x2c) return 0;
    return -1;
}

/* ---------- Human-readable formatting ---------- */

enum { SEV_OK = 0, SEV_WARN = 1, SEV_CRIT = 2, SEV_UNKNOWN = 3 };
/* Exit code of --smart (and of a multi-disk SMART job): 0 healthy, 3 warning, 4 critical, 5 unknown, 1 error. */
static int smart_exit_code(int sev) {
    return sev < 0 ? 1 : sev == SEV_WARN ? 3 : sev == SEV_CRIT ? 4 : sev == SEV_UNKNOWN ? 5 : 0;
}
static int color_mode = -1;
static int color_on(void) {
    if (color_mode < 0) {
        const char *t = getenv("TERM");
        color_mode = !opt_no_color && isatty(STDOUT_FILENO) && !getenv("NO_COLOR") && !(t && !strcmp(t, "dumb"));
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

/* Writes text to f using exactly width columns (UTF-8), truncated if needed. */
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
/* Integer with thousands separators: 1,234,567. */
static void fmt_int(char *out, size_t n, uint64_t v) {
    char tmp[32]; int len = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    size_t o = 0;
    for (int i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}
/* Decimal number with a fixed precision: 931.5. */
static void fmt_dec(char *out, size_t n, double v, int prec) {
    snprintf(out, n, "%.*f", prec, v);
    (void)out;
}
static void fmt_bytes(char *out, size_t n, double bytes) {
    static const char *u[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    int k = 0;
    while (bytes >= 1000.0 && k < 5) { bytes /= 1000.0; k++; }
    char num[16]; fmt_dec(num, sizeof(num), bytes, k ? 1 : 0);
    snprintf(out, n, "%s %s", num, u[k]);
}
/* Capacity shown everywhere in the TUI: "1.0 TB (931.5 GiB)". */
static void fmt_capacity(char *out, size_t n, uint64_t bytes) {
    char dec[32], gib[32];
    fmt_bytes(dec, sizeof(dec), (double)bytes);
    fmt_dec(gib, sizeof(gib), (double)bytes / 1073741824.0, 1);
    snprintf(out, n, "%s (%s GiB)", dec, gib);
}
static void fmt_hours(char *out, size_t n, uint64_t h) {
    char hs[32]; fmt_int(hs, sizeof(hs), h);
    uint64_t years = h / 8766, months = (h % 8766) / 730, days = h / 24;
    if (years) snprintf(out, n, "%s h (≈ %" PRIu64 " year%s %" PRIu64 " month%s)", hs, years, years > 1 ? "s" : "", months, months == 1 ? "" : "s");
    else if (months) snprintf(out, n, "%s h (≈ %" PRIu64 " month%s)", hs, months, months > 1 ? "s" : "");
    else snprintf(out, n, "%s h (≈ %" PRIu64 " day%s)", hs, days, days == 1 ? "" : "s");
}

/* List of points to watch, each with a piece of advice. */
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
static const char *sev_label(int sev) { return sev == SEV_CRIT ? "CRITICAL" : sev == SEV_WARN ? "WARNING" : "OK"; }

static void print_issues_and_verdict(const issues_t *is, int health_known) {
    printf("\n%sPoints to watch:%s\n", c_bold(), c_reset());
    if (!is->n) printf("  None.\n");
    for (int sev = SEV_CRIT; sev >= SEV_WARN; sev--)
        for (size_t i = 0; i < is->n; i++)
            if (is->item[i].sev == sev)
                printf("  %s[%s]%s %s\n", sev_color(sev), sev_label(sev), c_reset(), is->item[i].text);
    const char *verdict = is->worst == SEV_CRIT ? "CRITICAL" : is->worst == SEV_WARN ? "WARNING" : "HEALTHY";
    printf("\n%sVERDICT: %s%s%s\n", c_bold(), sev_color(is->worst), verdict, c_reset());
    if (is->worst == SEV_CRIT)
        printf("Advice: back up your data now, limit use of the disk and plan to replace it.\n");
    else if (is->worst == SEV_WARN)
        printf("Advice: back up your data and watch how the reported counters evolve.\n");
    else
        printf("Advice: keep backups and check SMART regularly; this verdict does not guarantee the disk will not fail.\n");
    if (!health_known)
        printf("%sNote: the firmware's overall assessment (SMART RETURN STATUS) could not be read through this bridge.%s\n", c_dim(), c_reset());
}

static void print_identity(const char *model, const char *serial, const char *fw, uint64_t bytes,
                           const char *type, const char *extra) {
    char cap[160] = "unknown", gib[32], bi[40];
    if (bytes) {
        char dec[32]; fmt_bytes(dec, sizeof(dec), (double)bytes);
        fmt_dec(gib, sizeof(gib), (double)bytes / 1073741824.0, 1); fmt_int(bi, sizeof(bi), bytes);
        snprintf(cap, sizeof(cap), "%s (%s GiB, %s bytes)", dec, gib, bi);
    }
    char fmtsec[96];
    if (log_sector_size == 512 && phys_sector_size == 512) snprintf(fmtsec, sizeof(fmtsec), "512n (logical 512 B, physical 512 B)");
    else if (log_sector_size == 512 && phys_sector_size == 4096) snprintf(fmtsec, sizeof(fmtsec), "512e (logical 512 B, physical 4096 B)");
    else if (log_sector_size == 4096 && phys_sector_size == 4096) snprintf(fmtsec, sizeof(fmtsec), "4Kn (logical 4096 B, physical 4096 B)");
    else snprintf(fmtsec, sizeof(fmtsec), "logical %u B, physical %u B", log_sector_size, phys_sector_size);
    printf("%s━━ %s ━━%s\n", c_bold(), model[0] ? model : "Unknown model", c_reset());
    printf("  Serial    : %s\n", serial[0] ? serial : "unknown");
    printf("  Firmware  : %s\n", fw[0] ? fw : "unknown");
    printf("  Capacity  : %s\n", cap);
    printf("  Sectors   : %s\n", fmtsec);
    printf("  Type      : %s\n", type);
    if (extra && extra[0]) printf("%s", extra);
}

/* ---------- SMART ATA ---------- */

typedef struct { uint8_t id; const char *name; } attr_name_t;
static const attr_name_t attr_names[] = {
    {1, "Raw read errors"}, {2, "Throughput performance"}, {3, "Spin-up time"},
    {4, "Start/stop count"}, {5, "Reallocated sectors"}, {7, "Seek errors"},
    {8, "Seek performance"}, {9, "Power-on hours"}, {10, "Spin-up retries"},
    {11, "Recalibration retries"}, {12, "Power cycles"}, {13, "Soft read errors"},
    {170, "Available reserve"}, {171, "Program failures"}, {172, "Erase failures"},
    {173, "Average wear"}, {174, "Unexpected power loss"}, {175, "Program failures"},
    {176, "Erase failures"}, {177, "Wear leveling count"}, {178, "Used reserve blocks"},
    {179, "Used reserve blocks"}, {180, "Unused reserve blocks"}, {181, "Program failures"},
    {182, "Erase failures"}, {183, "SATA downshifts"}, {184, "End-to-end errors"},
    {187, "Reported uncorrectable"}, {188, "Command timeouts"}, {189, "High fly writes"},
    {190, "Airflow temperature"}, {191, "G-sense errors"}, {192, "Emergency retracts"},
    {193, "Load cycles"}, {194, "Temperature"}, {195, "ECC corrections"},
    {196, "Reallocation events"}, {197, "Pending sectors"}, {198, "Uncorrectable sectors"},
    {199, "CRC errors (cable)"}, {200, "Write errors"}, {202, "Remaining life"},
    {206, "Flying height"}, {220, "Disk shift"}, {222, "Loaded hours"},
    {223, "Load retries"}, {224, "Load friction"}, {225, "Load cycles"},
    {226, "Load-in time"}, {230, "Head amplitude"}, {231, "Remaining life"},
    {232, "Available reserve"}, {233, "Media wearout indicator"}, {234, "Data written (vendor)"},
    {235, "Power loss (vendor)"}, {240, "Head flying hours"}, {241, "Total written (LBA)"},
    {242, "Total read (LBA)"}, {246, "Total host writes"}, {247, "Host pages written"},
    {248, "Pages written (controller)"}, {249, "NAND writes"}, {250, "Read retries"},
    {254, "Free fall events"},
};
static const char *attr_name(uint8_t id) {
    for (size_t i = 0; i < sizeof(attr_names) / sizeof(attr_names[0]); i++)
        if (attr_names[i].id == id) return attr_names[i].name;
    return "Vendor specific";
}
static uint64_t attr_raw(const unsigned char *a) {
    uint64_t raw = 0;
    for (int k = 5; k >= 0; k--) raw = (raw << 8) | a[5 + k];
    return raw;
}
/* Temperature: byte 0 = current °C; bytes 2 and 4 are often min/max. */
static int decode_temp(uint64_t raw, int *mn, int *mx) {
    int t = (int)(raw & 0xff), lo = (int)((raw >> 16) & 0xff), hi = (int)((raw >> 32) & 0xff);
    *mn = *mx = -1;
    if (lo > 0 && lo <= t && t <= hi && hi < 128) { *mn = lo; *mx = hi; }
    return t;
}

/*
 * Prints the ATA SMART report from the raw buffers (IDENTIFY, SMART READ DATA,
 * optional thresholds, firmware assessment). Kept separate from I/O so it can be tested.
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
    /* Type: word 217 (rotation rate); SATA link: words 76/77. */
    uint16_t rot = word_le(id, 217), sata = word_le(id, 76), sata_cur = word_le(id, 77);
    int is_ssd = rot == 1, is_hdd = rot >= 0x0401 && rot <= 0xfffe;
    char type[128], link[64] = "";
    if (sata && sata != 0xffff) {
        const char *mx = (sata & 8) ? "6 Gb/s" : (sata & 4) ? "3 Gb/s" : (sata & 2) ? "1.5 Gb/s" : NULL;
        unsigned cur = (sata_cur >> 1) & 7;
        const char *cs = cur == 3 ? "6 Gb/s" : cur == 2 ? "3 Gb/s" : cur == 1 ? "1.5 Gb/s" : NULL;
        if (mx && cs) snprintf(link, sizeof(link), " · SATA %s (current link %s)", mx, cs);
        else if (mx) snprintf(link, sizeof(link), " · SATA %s", mx);
        if (mx && cs && strcmp(mx, cs))
            issue_add(&is, SEV_WARN, "SATA link negotiated at %s although the disk supports %s: check the SATA cable and port.", cs, mx);
    }
    if (is_ssd) snprintf(type, sizeof(type), "SSD%s", link);
    else if (is_hdd) snprintf(type, sizeof(type), "Hard disk %u rpm%s", rot, link);
    else snprintf(type, sizeof(type), "not specified%s", link);
    uint16_t sec = word_le(id, 128);
    char extra[256];
    if (sec & 1)
        snprintf(extra, sizeof(extra), "  Security  : ATA Security supported · %s · %s%s%s\n",
                 sec & 2 ? "ENABLED" : "not enabled", sec & 8 ? "FROZEN" : "not frozen",
                 sec & 4 ? " · LOCKED" : "", sec & 0x20 ? " · enhanced erase available" : "");
    else snprintf(extra, sizeof(extra), "  Security  : ATA Security not reported\n");
    print_identity(model, serial, fw, bytes, type, extra);
    if (sec & 4) issue_add(&is, SEV_WARN, "The disk is locked (ATA Security): its data cannot be accessed without the password.");

    /* Firmware assessment. */
    printf("\n%sHealth (firmware assessment):%s %s%s%s\n", c_bold(), c_reset(),
           health == 1 ? sev_color(SEV_OK) : health == 0 ? sev_color(SEV_CRIT) : c_dim(),
           health == 1 ? "OK" : health == 0 ? "FAILURE PREDICTED" : "unknown", c_reset());
    if (health == 0)
        issue_add(&is, SEV_CRIT, "The firmware itself predicts a failure (an attribute crossed its threshold): back up now.");

    /* Attributes. */
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
        if (tmin >= 0) snprintf(part, sizeof(part), "Temperature %d °C (min %d / max %d)", temp, tmin, tmax);
        else snprintf(part, sizeof(part), "Temperature %d °C", temp);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (have_hours) {
        char h[96]; fmt_hours(h, sizeof(h), hours);
        snprintf(part, sizeof(part), "%sPowered on %s", summary[0] ? " · " : "", h);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (have_cycles) {
        char c[32]; fmt_int(c, sizeof(c), cycles);
        snprintf(part, sizeof(part), "%s%s power cycles", summary[0] ? " · " : "", c);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (have_written) {
        char w[32]; fmt_bytes(w, sizeof(w), (double)written);
        snprintf(part, sizeof(part), "%sWritten ≈ %s*", summary[0] ? " · " : "", w);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (life >= 0) {
        snprintf(part, sizeof(part), "%sRemaining life ≈ %d %%", summary[0] ? " · " : "", life);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (summary[0]) printf("%s\n", summary);

    printf("\n%s  ID  ", c_bold()); fput_padded(stdout, "Attribute", 27);
    printf("Val. Wrst Thres "); fput_padded(stdout, "Decoded raw", 22); printf("State%s\n", c_reset());
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
        /* Normalized thresholds: value ≤ threshold = failing now; worst ≤ threshold = failed in the past. */
        if (t > 0 && t < 0xfe && cur <= t) {
            sev = SEV_CRIT; state = "FAILING";
            issue_add(&is, SEV_CRIT, "ID %u \"%s\" is below its failure threshold (%u ≤ %d).", aid, name, cur, t);
        } else if (t > 0 && t < 0xfe && worst <= t) {
            sev = SEV_WARN; state = "FAILED";
            issue_add(&is, SEV_WARN, "ID %u \"%s\" has dropped below its threshold in the past (worst %u ≤ %d).", aid, name, worst, t);
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
                if (tc >= crit) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "Temperature of %d °C: too high, improve cooling right away.", tc); }
                else if (tc >= warn) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "Temperature of %d °C: high, check the case airflow.", tc); }
            }
        } break;
        case 241: case 242: case 246:
            if (raw) { char b[48]; fmt_bytes(b, sizeof(b), (double)raw * 512.0); snprintf(dec, sizeof(dec), "≈ %s*", b); }
            break;
        case 5:
            if (raw) {
                s2 = raw >= 100 ? SEV_CRIT : SEV_WARN;
                issue_add(&is, s2, "%s reallocated sector(s): the disk has already replaced bad sectors. "
                          "Back up and keep watching: a steady increase means the disk is failing.", rs);
            } break;
        case 196:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s reallocation event(s) recorded.", rs); }
            break;
        case 197:
            if (raw) {
                s2 = SEV_CRIT;
                issue_add(&is, SEV_CRIT, "%s pending sector(s) (unreadable): back up, then run --scan-slow and "
                          "--slow-repair to force their rewrite/reallocation, and check SMART again.", rs);
            } break;
        case 198:
            if (raw) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "%s uncorrectable sector(s): data at these locations is already lost.", rs); }
            break;
        case 187:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s uncorrectable error(s) reported to the host.", rs); }
            break;
        case 184:
            if (raw) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "%s end-to-end error(s): internal corruption in the disk (cache/controller).", rs); }
            break;
        case 10:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s spin-up retr%s: the motor or power supply is struggling.", rs, raw == 1 ? "y" : "ies"); }
            break;
        case 199:
            if (raw) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "%s CRC error(s): SATA cable or connector problem (not disk wear). "
                                       "Replace the cable; if the counter stops increasing afterwards, it is fixed.", rs); }
            break;
        case 177: case 202: case 231: case 233:
            if (is_ssd && cur && cur <= 100) {
                snprintf(dec, sizeof(dec), "%s (life ≈ %u %%)", rs, cur);
                if (aid == life_id) {
                    if (cur <= 10) { s2 = SEV_CRIT; issue_add(&is, SEV_CRIT, "SSD almost worn out (remaining life ≈ %u %%): plan to replace it.", cur); }
                    else if (cur <= 30) { s2 = SEV_WARN; issue_add(&is, SEV_WARN, "SSD significantly worn (remaining life ≈ %u %%).", cur); }
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
        printf("%s* assumed unit: 512-byte sector (some vendors count differently).%s\n", c_dim(), c_reset());
    if (!thr) printf("%sSMART thresholds unreadable: \"Thres\" column unavailable.%s\n", c_dim(), c_reset());
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
/* NVMe admin command; *status receives the NVMe status code (> 0) or the ioctl return value. */
static int nvme_admin_full(int fd, uint8_t opcode, uint32_t nsid, uint32_t cdw10, void *buf, uint32_t len,
                           uint32_t timeout_ms, int *status) {
    struct nvme_admin_cmd c;
    memset(&c, 0, sizeof(c));
    c.opcode = opcode; c.nsid = nsid; c.addr = (uint64_t)(uintptr_t)buf; c.data_len = len;
    c.cdw10 = cdw10; c.timeout_ms = timeout_ms;
    int r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c);
    if (status) *status = r;
    if (r > 0) { errno = EIO; return -1; }  /* r > 0: NVMe status code */
    return r;
}
static int nvme_admin(int fd, uint8_t opcode, uint32_t nsid, uint32_t cdw10, void *buf, uint32_t len) {
    return nvme_admin_full(fd, opcode, nsid, cdw10, buf, len, 30000, NULL);
}
/* Prints the NVMe report (optional Identify Controller + SMART/Health log 02h). */
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
    print_identity(model, serial, fw, bytes, "NVMe SSD", NULL);
    uint8_t cw = log[0], spare = log[3], spare_thr = log[4], used = log[5];
    int temp = le16(log + 1) ? (int)le16(log + 1) - 273 : -1;
    uint64_t rd = le64(log + 32), wr = le64(log + 48), cycles = le64(log + 112), hours = le64(log + 128);
    uint64_t unsafe = le64(log + 144), media = le64(log + 160), errlog = le64(log + 176);
    uint32_t warn_min = (uint32_t)log[192] | ((uint32_t)log[193] << 8) | ((uint32_t)log[194] << 16) | ((uint32_t)log[195] << 24);
    uint32_t crit_min = (uint32_t)log[196] | ((uint32_t)log[197] << 8) | ((uint32_t)log[198] << 16) | ((uint32_t)log[199] << 24);
    int life = used >= 100 ? 0 : 100 - used;

    printf("\n%sHealth (firmware assessment):%s %s%s%s\n", c_bold(), c_reset(),
           sev_color(cw ? SEV_CRIT : SEV_OK), cw ? "CRITICAL WARNING" : "OK", c_reset());
    char h[96], rds[32], wrs[32], cyc[32];
    fmt_hours(h, sizeof(h), hours); fmt_bytes(rds, sizeof(rds), (double)rd * 512000.0);
    fmt_bytes(wrs, sizeof(wrs), (double)wr * 512000.0); fmt_int(cyc, sizeof(cyc), cycles);
    if (temp >= 0) printf("Temperature %d °C · ", temp);
    printf("Powered on %s · %s power cycles · Written %s · Remaining life ≈ %d %%\n", h, cyc, wrs, life);

    printf("\n%s  ", c_bold()); fput_padded(stdout, "Indicator", 38); fput_padded(stdout, "Value", 30); printf("State%s\n", c_reset());
#define NVME_ROW(label, value, sev) do { printf("  "); fput_padded(stdout, label, 38); fput_padded(stdout, value, 30); \
        printf("%s%s%s\n", sev_color(sev), sev_label(sev), c_reset()); } while (0)
    char v[96]; int s;
    if (!cw) snprintf(v, sizeof(v), "none");
    else snprintf(v, sizeof(v), "%s%s%s%s%s", cw & 1 ? "low spare " : "", cw & 2 ? "temperature " : "",
                  cw & 4 ? "degraded reliability " : "", cw & 8 ? "read-only " : "", cw & 16 ? "volatile backup " : "");
    s = cw ? SEV_CRIT : SEV_OK;
    if (cw & 1) issue_add(&is, SEV_CRIT, "Spare blocks below threshold: the SSD is reaching the end of its life.");
    if (cw & 2) issue_add(&is, SEV_CRIT, "The SSD reports an out-of-range temperature.");
    if (cw & 4) issue_add(&is, SEV_CRIT, "Degraded reliability (media or internal errors): back up now.");
    if (cw & 8) issue_add(&is, SEV_CRIT, "The SSD switched to read-only mode to protect the data: copy it and replace the SSD.");
    if (cw & 16) issue_add(&is, SEV_CRIT, "The volatile memory backup mechanism failed.");
    NVME_ROW("Critical warning", v, s);
    if (temp >= 0) {
        int warn = wctemp > 0 ? wctemp : 70, crit = cctemp > 0 ? cctemp : 80;
        s = temp >= crit ? SEV_CRIT : temp >= warn ? SEV_WARN : SEV_OK;
        if (wctemp > 0) snprintf(v, sizeof(v), "%d °C (warning %d °C)", temp, wctemp); else snprintf(v, sizeof(v), "%d °C", temp);
        if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Temperature of %d °C: critical, the SSD will throttle or shut down; improve its cooling.", temp);
        else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Temperature of %d °C: high, add a heatsink or airflow.", temp);
        NVME_ROW("Temperature", v, s);
    }
    snprintf(v, sizeof(v), "%u %% (threshold %u %%)", spare, spare_thr);
    s = spare < spare_thr ? SEV_CRIT : (spare_thr && spare < spare_thr + 10) ? SEV_WARN : SEV_OK;
    if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Spare blocks at %u %%, close to the %u %% threshold.", spare, spare_thr);
    NVME_ROW("Available spare", v, s);
    snprintf(v, sizeof(v), "%u %% (remaining life ≈ %d %%)", used, life);
    s = life <= 10 ? SEV_CRIT : life <= 30 ? SEV_WARN : SEV_OK;
    if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Endurance %u %% used: plan to replace the SSD.", used);
    else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Endurance %u %% used.", used);
    NVME_ROW("Wear (endurance used)", v, s);
    fmt_int(v, sizeof(v), media);
    s = media ? SEV_CRIT : SEV_OK;
    if (media) issue_add(&is, SEV_CRIT, "%s media/data integrity error(s): some data may have been corrupted.", v);
    NVME_ROW("Media / integrity errors", v, s);
    fmt_int(v, sizeof(v), errlog); NVME_ROW("Error log entries", v, SEV_OK);
    fmt_int(v, sizeof(v), unsafe); NVME_ROW("Unsafe shutdowns", v, SEV_OK);
    snprintf(v, sizeof(v), "%s / %s", rds, wrs); NVME_ROW("Data read / written", v, SEV_OK);
    NVME_ROW("Power-on hours", h, SEV_OK);
    NVME_ROW("Power cycles", cyc, SEV_OK);
    snprintf(v, sizeof(v), "%" PRIu32 " min / %" PRIu32 " min", warn_min, crit_min);
    s = crit_min ? SEV_WARN : SEV_OK;
    if (crit_min) issue_add(&is, SEV_WARN, "The SSD has already spent %" PRIu32 " min above its critical temperature.", crit_min);
    NVME_ROW("Time at warning / critical temp.", v, s);
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
    /* Get Log Page 02h (SMART / Health), 512 bytes: NUMDL = 128 dwords - 1. */
    if (nvme_admin(fd, 0x02, 0xffffffffU, (127U << 16) | 0x02, log, 512) < 0 &&
        nvme_admin(fd, 0x02, 0, (127U << 16) | 0x02, log, 512) < 0) {
        fprintf(stderr, "Cannot read the NVMe SMART log: %s\n", strerror(errno));
        goto out;
    }
    rc = nvme_print(have_ctrl ? ctrl : NULL, log, bytes);
out:
    free(ctrl); free(log); return rc;
}

/* ---------- SAS / SCSI SMART (LOG SENSE pages) ---------- */

/*
 * Last SCSI sense seen (for diagnostics), and failure prediction reported in sense data:
 * depending on its MRIE setting, a disk reports "failure prediction threshold exceeded"
 * (ASC 5Dh) as a CHECK CONDITION with the RECOVERED ERROR or NO SENSE key on ordinary
 * commands, instead of (or as well as) in log page 2Fh.
 */
static int scsi_last_key = -1, scsi_last_asc = -1, scsi_last_ascq = -1;
static int scsi_ie_asc = 0, scsi_ie_ascq = 0;
/*
 * Generic SCSI command through SG_IO, without messages. A CHECK CONDITION with the
 * RECOVERED ERROR or NO SENSE key is accepted (the data is valid). Returns the number
 * of bytes received, or -1.
 */
static int scsi_cmd(int fd, const unsigned char *cdb, unsigned cdb_len, unsigned char *buf, unsigned len) {
    /*
     * A UNIT ATTENTION (key 6h: reset, power on, mode parameters changed…) only reports an
     * event and is cleared once returned: the command is simply sent again, a few times.
     */
    for (int attempt = 0;; attempt++) {
        unsigned char sense[64] = {0};
        sg_io_hdr_t io; memset(&io, 0, sizeof(io));
        if (len) memset(buf, 0, len);
        io.interface_id = 'S'; io.cmd_len = (unsigned char)cdb_len; io.mx_sb_len = sizeof(sense);
        io.dxfer_direction = len ? SG_DXFER_FROM_DEV : SG_DXFER_NONE; io.dxfer_len = len; io.dxferp = buf;
        io.cmdp = (unsigned char *)cdb; io.sbp = sense; io.timeout = 30000;
        if (ioctl(fd, SG_IO, &io) < 0) return -1;
        if (io.host_status || (io.driver_status & ~0x08U)) return -1;
        if (io.status) {
            int desc = (sense[0] & 0x7f) >= 0x72;
            unsigned key = io.sb_len_wr < 3 ? 0 : desc ? sense[1] & 0x0f : sense[2] & 0x0f;
            int asc = -1, ascq = -1;
            if (desc && io.sb_len_wr >= 4) { asc = sense[2]; ascq = sense[3]; }
            else if (!desc && io.sb_len_wr >= 14) { asc = sense[12]; ascq = sense[13]; }
            scsi_last_key = (int)key; scsi_last_asc = asc; scsi_last_ascq = ascq;
            if (asc == 0x5d) { scsi_ie_asc = asc; scsi_ie_ascq = ascq; }
            if (key == 0x06 && attempt < 3) continue;
            if (key != 0x00 && key != 0x01) return -1;
        }
        int got = (int)len - io.resid;
        return got < 0 ? 0 : got;
    }
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
    int rotation;                       /* -1 unknown, 1 SSD, otherwise rpm */
    int ie_known, ie_asc, ie_ascq;      /* page 2Fh: firmware assessment */
    int temp, temp_ref;                 /* page 0Dh, °C; -1 unknown */
    int64_t grown_defects;              /* READ DEFECT DATA, grown list; -1 unknown */
    int64_t corrected[3], uncorrected[3], processed[3];   /* read, write, verify */
    int64_t non_medium;                 /* page 06h */
    int64_t ss_spec, ss_acc, lu_spec, lu_acc;             /* page 0Eh */
    char manufactured[24];
    int64_t power_on_minutes;           /* page 15h */
    int endurance_used;                 /* page 11h (SSD), %; -1 unknown */
    int sense_key, sense_asc, sense_ascq;   /* last CHECK CONDITION seen; -1 none */
} sas_info_t;

static void sas_init(sas_info_t *si) {
    memset(si, 0, sizeof(*si));
    si->rotation = si->temp = si->temp_ref = si->endurance_used = -1;
    si->sense_key = si->sense_asc = si->sense_ascq = -1;
    si->grown_defects = si->non_medium = si->power_on_minutes = -1;
    si->ss_spec = si->ss_acc = si->lu_spec = si->lu_acc = -1;
    for (int k = 0; k < 3; k++) si->corrected[k] = si->uncorrected[k] = si->processed[k] = -1;
}

/* Decodes a LOG SENSE page (4-byte header, then code/control/length/value parameters). */
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
                snprintf(si->manufactured, sizeof(si->manufactured), "%.4s, week %.2s", (const char *)v, (const char *)v + 4);
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
/* READ DEFECT DATA(10) header: byte 1 = GLISTV (bit 3) + format, bytes 2-3 = length. */
static void sas_parse_defects(sas_info_t *si, const unsigned char *h, unsigned len) {
    if (len < 4 || !(h[1] & 0x08)) return;
    unsigned fmt = h[1] & 7, dlen = ((unsigned)h[2] << 8) | h[3];
    unsigned dsize = fmt == 0 ? 4 : 8;
    si->grown_defects = dlen / dsize;
}

/* Collects the identity and health of a SCSI/SAS disk. Returns -1 if it is not a SCSI disk. */
static int sas_collect(int fd, sas_info_t *si) {
    unsigned char inq[96], pg[4096];
    sas_init(si);
    scsi_last_key = scsi_last_asc = scsi_last_ascq = -1;
    scsi_ie_asc = scsi_ie_ascq = 0;
    unsigned char icdb[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
    if (scsi_cmd(fd, icdb, sizeof(icdb), inq, sizeof(inq)) < 36 || (inq[0] & 0x1f) != 0) return -1;
    memcpy(si->vendor, inq + 8, 8); trim(si->vendor);
    memcpy(si->product, inq + 16, 16); trim(si->product);
    memcpy(si->rev, inq + 32, 4); trim(si->rev);
    if (scsi_vpd_serial(fd, si->serial, sizeof(si->serial)) < 0) si->serial[0] = '\0';
    /* VPD B1h: rotation rate (1 = non-rotating medium). */
    unsigned char bcdb[6] = {0x12, 1, 0xb1, 0, 64, 0}, b1[64];
    if (scsi_cmd(fd, bcdb, sizeof(bcdb), b1, sizeof(b1)) >= 6 && b1[1] == 0xb1) {
        unsigned r = ((unsigned)b1[4] << 8) | b1[5];
        if (r == 1 || (r >= 0x0401 && r <= 0xfffe)) si->rotation = (int)r;
    }
    /* Supported LOG SENSE pages (page 00h), then read the ones we need. */
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
    /* READ DEFECT DATA(10): grown list (G list), "bytes from index" format. */
    unsigned char dcdb[10] = {0x37, 0, 0x08 | 0x04, 0, 0, 0, 0, 0, 8, 0}, d[8];
    if (scsi_cmd(fd, dcdb, sizeof(dcdb), d, sizeof(d)) >= 4) sas_parse_defects(si, d, 4);
    /* Failure prediction reported in sense data (ASC 5Dh) counts like page 2Fh. */
    if (scsi_ie_asc && !(si->ie_known && si->ie_asc)) {
        si->ie_known = 1; si->ie_asc = scsi_ie_asc; si->ie_ascq = scsi_ie_ascq;
    }
    si->sense_key = scsi_last_key; si->sense_asc = scsi_last_asc; si->sense_ascq = scsi_last_ascq;
    return 0;
}

static void sas_row(const char *label, const char *value, int sev) {
    printf("  "); fput_padded(stdout, label, 38); fput_padded(stdout, value, 30);
    printf("%s%s%s\n", sev_color(sev), sev_label(sev), c_reset());
}

/* Prints the SAS/SCSI report. Kept separate from I/O so it can be tested. */
static int sas_print(const sas_info_t *si, uint64_t bytes) {
    issues_t is; memset(&is, 0, sizeof(is));
    char model[48], type[96];
    snprintf(model, sizeof(model), "%s%s%s", si->vendor, si->vendor[0] && si->product[0] ? " " : "", si->product);
    int is_ssd = si->rotation == 1;
    if (is_ssd) snprintf(type, sizeof(type), "SAS/SCSI SSD");
    else if (si->rotation > 1) snprintf(type, sizeof(type), "SAS/SCSI hard disk %d rpm", si->rotation);
    else snprintf(type, sizeof(type), "SAS/SCSI (type not specified)");
    char extra[96] = "";
    if (si->manufactured[0]) snprintf(extra, sizeof(extra), "  Made      : %s\n", si->manufactured);
    print_identity(model, si->serial, si->rev, bytes, type, extra);

    int known = si->ie_known || si->temp >= 0 || si->grown_defects >= 0 || si->non_medium >= 0 ||
                si->uncorrected[0] >= 0 || si->uncorrected[1] >= 0 || si->power_on_minutes >= 0 || si->endurance_used >= 0;
    int failing = si->ie_known && si->ie_asc != 0;
    printf("\n%sHealth (firmware assessment):%s %s%s%s\n", c_bold(), c_reset(),
           !si->ie_known ? c_dim() : sev_color(failing ? SEV_CRIT : SEV_OK),
           !si->ie_known ? "unknown" : failing ? "FAILURE PREDICTED" : "OK", c_reset());
    if (!known) {
        printf("\nNo readable health data (USB bridge or controller that does not pass SCSI pages through).\n");
        if (si->sense_key >= 0)
            printf("%sLast SCSI sense: key %Xh, ASC %02Xh, ASCQ %02Xh.%s\n", c_dim(), (unsigned)si->sense_key,
                   (unsigned)(si->sense_asc & 0xff), (unsigned)(si->sense_ascq & 0xff), c_reset());
        printf("\n%sVERDICT: UNKNOWN%s\n", c_bold(), c_reset());
        return SEV_UNKNOWN;
    }
    /* Summary. */
    char summary[384] = "", part[128];
    if (si->temp >= 0) {
        snprintf(part, sizeof(part), "Temperature %d °C", si->temp);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->power_on_minutes >= 0) {
        char h[96]; fmt_hours(h, sizeof(h), (uint64_t)si->power_on_minutes / 60);
        snprintf(part, sizeof(part), "%sPowered on %s", summary[0] ? " · " : "", h);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->ss_acc >= 0) {
        char c[32]; fmt_int(c, sizeof(c), (uint64_t)si->ss_acc);
        snprintf(part, sizeof(part), "%s%s start/stop cycles", summary[0] ? " · " : "", c);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->processed[1] >= 0) {
        char w[32]; fmt_bytes(w, sizeof(w), (double)si->processed[1]);
        snprintf(part, sizeof(part), "%sWritten %s", summary[0] ? " · " : "", w);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (si->endurance_used >= 0) {
        snprintf(part, sizeof(part), "%sRemaining life ≈ %d %%", summary[0] ? " · " : "", si->endurance_used >= 100 ? 0 : 100 - si->endurance_used);
        strncat(summary, part, sizeof(summary) - strlen(summary) - 1);
    }
    if (summary[0]) printf("%s\n", summary);

    printf("\n%s  ", c_bold()); fput_padded(stdout, "Indicator", 38); fput_padded(stdout, "Value", 30); printf("State%s\n", c_reset());
    char v[96]; int s;
    if (si->ie_known) {
        if (!failing) snprintf(v, sizeof(v), "none");
        else snprintf(v, sizeof(v), "ASC %02Xh / ASCQ %02Xh", si->ie_asc, si->ie_ascq);
        if (failing)
            issue_add(&is, SEV_CRIT, "The disk itself predicts an upcoming failure (ASC %02Xh/%02Xh): back up now.",
                      si->ie_asc, si->ie_ascq);
        sas_row("Firmware warning", v, failing ? SEV_CRIT : SEV_OK);
    }
    if (si->temp >= 0) {
        int warn = is_ssd ? 70 : 55, crit = is_ssd ? 80 : 60;
        if (si->temp_ref > 0) { crit = si->temp_ref; if (warn >= crit) warn = crit - 5; }
        s = si->temp >= crit ? SEV_CRIT : si->temp >= warn ? SEV_WARN : SEV_OK;
        if (si->temp_ref > 0) snprintf(v, sizeof(v), "%d °C (max. %d °C)", si->temp, si->temp_ref);
        else snprintf(v, sizeof(v), "%d °C", si->temp);
        if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Temperature of %d °C: too high, improve cooling right away.", si->temp);
        else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Temperature of %d °C: high, check the airflow.", si->temp);
        sas_row("Temperature", v, s);
    }
    if (si->grown_defects >= 0) {
        fmt_int(v, sizeof(v), (uint64_t)si->grown_defects);
        s = si->grown_defects >= 100 ? SEV_CRIT : si->grown_defects > 0 ? SEV_WARN : SEV_OK;
        if (s != SEV_OK)
            issue_add(&is, s, "%s grown defect(s) (G list, the equivalent of reallocated sectors): back up and "
                      "keep watching, a steady increase means the disk is failing.", v);
        sas_row("Grown defects (reallocated)", v, s);
    }
    static const char *ops[3] = {"read", "write", "verify"};
    for (int k = 0; k < 3; k++) {
        if (si->corrected[k] < 0 && si->uncorrected[k] < 0) continue;
        static const char *labels[3] = {"Read: errors corr. / uncorr.", "Write: errors corr. / uncorr.",
                                        "Verify: errors corr. / uncorr."};
        char c[32] = "-", u[32] = "-";
        const char *label = labels[k];
        if (si->corrected[k] >= 0) fmt_int(c, sizeof(c), (uint64_t)si->corrected[k]);
        if (si->uncorrected[k] >= 0) fmt_int(u, sizeof(u), (uint64_t)si->uncorrected[k]);
        snprintf(v, sizeof(v), "%s / %s", c, u);
        s = si->uncorrected[k] > 0 ? (k == 2 ? SEV_WARN : SEV_CRIT) : SEV_OK;
        if (si->uncorrected[k] > 0)
            issue_add(&is, s, "%s uncorrected %s error(s): some data %s.", u, ops[k],
                      k == 0 ? "could not be read back" : k == 1 ? "could not be written" : "could not be verified");
        sas_row(label, v, s);
    }
    if (si->non_medium >= 0) { fmt_int(v, sizeof(v), (uint64_t)si->non_medium); sas_row("Non-medium errors (link, etc.)", v, SEV_OK); }
    if (si->processed[0] >= 0 || si->processed[1] >= 0) {
        char r[32] = "-", w[32] = "-";
        if (si->processed[0] >= 0) fmt_bytes(r, sizeof(r), (double)si->processed[0]);
        if (si->processed[1] >= 0) fmt_bytes(w, sizeof(w), (double)si->processed[1]);
        snprintf(v, sizeof(v), "%s / %s", r, w);
        sas_row("Data read / written", v, SEV_OK);
    }
    if (si->power_on_minutes >= 0) {
        fmt_hours(v, sizeof(v), (uint64_t)si->power_on_minutes / 60);
        sas_row("Power-on hours", v, SEV_OK);
    }
    const int64_t accs[2] = {si->ss_acc, si->lu_acc}, specs[2] = {si->ss_spec, si->lu_spec};
    static const char *cyc[2] = {"Start/stop cycles (of rated)", "Head load cycles (of rated)"};
    static const char *cyc_txt[2] = {"start/stop cycles", "head load cycles"};
    for (int k = 0; k < 2; k++) {
        if (accs[k] < 0) continue;
        char a[32], b[32]; fmt_int(a, sizeof(a), (uint64_t)accs[k]);
        s = SEV_OK;
        if (specs[k] > 0) {
            fmt_int(b, sizeof(b), (uint64_t)specs[k]);
            int pct = (int)((double)accs[k] * 100.0 / (double)specs[k]);
            snprintf(v, sizeof(v), "%s / %s (%d %%)", a, b, pct);
            s = pct >= 100 ? SEV_CRIT : pct >= 90 ? SEV_WARN : SEV_OK;
            if (s != SEV_OK) issue_add(&is, s, "%d %% of the %s rated by the manufacturer are used up.", pct, cyc_txt[k]);
        } else snprintf(v, sizeof(v), "%s", a);
        sas_row(cyc[k], v, s);
    }
    if (si->endurance_used >= 0) {
        int life = si->endurance_used >= 100 ? 0 : 100 - si->endurance_used;
        snprintf(v, sizeof(v), "%d %% (remaining life ≈ %d %%)", si->endurance_used, life);
        s = life <= 10 ? SEV_CRIT : life <= 30 ? SEV_WARN : SEV_OK;
        if (s == SEV_CRIT) issue_add(&is, SEV_CRIT, "Endurance %d %% used: plan to replace the SSD.", si->endurance_used);
        else if (s == SEV_WARN) issue_add(&is, SEV_WARN, "Endurance %d %% used.", si->endurance_used);
        sas_row("Wear (endurance used)", v, s);
    }
    if (si->grown_defects < 0)
        printf("%sGrown defect list not available on this disk.%s\n", c_dim(), c_reset());
    print_issues_and_verdict(&is, si->ie_known);
    return is.worst;
}

/* ---------- Short identity cards for the TUI menu header ---------- */

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
/* NVMe card: Identify Controller + SMART/Health log. */
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
    card_add(c, "Model: %s", model);
    card_add(c, "Serial: %s | Firmware: %s", serial, fw);
    card_add(c, "Type: NVMe SSD · namespace %d", ioctl(fd, NVME_IOCTL_ID));
    if (nvme_admin(fd, 0x02, 0xffffffffU, (127U << 16) | 0x02, log, 512) == 0 ||
        nvme_admin(fd, 0x02, 0, (127U << 16) | 0x02, log, 512) == 0) {
        int temp = le16(log + 1) ? (int)le16(log + 1) - 273 : -1, used = log[5];
        char t[24] = "";
        if (temp >= 0) snprintf(t, sizeof(t), " · %d °C", temp);
        card_add(c, "Health: %s%s · remaining life ≈ %d %% (details: 1 = SMART)",
                 log[0] ? "CRITICAL WARNING" : "OK", t, used >= 100 ? 0 : 100 - used);
    }
    rc = 0;
out:
    free(ctrl); free(log); return rc;
}
/* SAS/SCSI card: INQUIRY + firmware assessment. */
static int sas_card(int fd, card_t *c) {
    sas_info_t si;
    memset(c, 0, sizeof(*c));
    if (sas_collect(fd, &si) < 0) return -1;
    card_add(c, "Model: %s %s", si.vendor, si.product);
    card_add(c, "Serial: %s | Revision: %s", si.serial[0] ? si.serial : "unknown", si.rev);
    if (si.rotation > 1) card_add(c, "Type: SAS/SCSI hard disk %d rpm", si.rotation);
    else card_add(c, "Type: %s", si.rotation == 1 ? "SAS/SCSI SSD" : "SAS/SCSI");
    if (si.ie_known || si.temp >= 0) {
        char t[24] = "";
        if (si.temp >= 0) snprintf(t, sizeof(t), " · %d °C", si.temp);
        card_add(c, "Health: %s%s (details: 1 = SMART)",
                 !si.ie_known ? "unknown" : si.ie_asc ? "FAILURE PREDICTED" : "OK", t);
    }
    return 0;
}

static int smart_report(int fd) {
    ui_status("SMART diagnostics", "Reading the identity and SMART attributes");
    unsigned long long bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) bytes = 0;
    if (ioctl(fd, NVME_IOCTL_ID) > 0) { int r = nvme_report(fd, (uint64_t)bytes); fflush(stdout); return r; }
    unsigned char id[512], data[512], thr[512];
    if (identify_quiet(fd, id) < 0) {
        /* No ATA response: SAS/SCSI disk (LOG SENSE pages)? */
        sas_info_t si;
        if (sas_collect(fd, &si) == 0) { int r = sas_print(&si, (uint64_t)bytes); fflush(stdout); return r; }
        fprintf(stderr, "No ATA IDENTIFY, SCSI INQUIRY or NVMe response: SMART is not available for this device.\n");
        return -1;
    }
    /* Words 82/85 bit 0: SMART supported / enabled. */
    if (!(word_le(id, 82) & 1)) { fprintf(stderr, "This disk does not report SMART support.\n"); return -1; }
    if (!(word_le(id, 85) & 1)) {
        fprintf(stderr, "SMART is disabled on this disk. Enable it with: sudo smartctl -s on %s\n", ui_device);
        return -1;
    }
    if (smart_read_data(fd, data) < 0) { fprintf(stderr, "SMART READ DATA failed (incompatible USB bridge?).\n"); return -1; }
    int have_thr = smart_read_thresholds(fd, thr) == 0;
    int health = smart_health(fd);
    int r = smart_print_ata(id, data, have_thr ? thr : NULL, health, (uint64_t)bytes);
    fflush(stdout);
    return r;
}
/*
 * Temporary ATA password, deliberately known and displayed: if the erase is
 * interrupted (power loss, crash), the disk can be unlocked with hdparm.
 */
#define ATA_TEMP_PASSWORD "diskmedic"
static int sec_cmd(int fd, uint8_t command, uint8_t count, const unsigned char *payload, unsigned len, unsigned timeout) {
    uint8_t protocol = payload ? 5 : 3;
    uint8_t flags = payload ? 0x06 : 0x00;
    return ata_pt(fd, protocol, flags, 0, count, 0, 0, command,
                  (void *)payload, len, payload ? SG_DXFER_TO_DEV : SG_DXFER_NONE, timeout);
}
static int secure_erase(int fd, const unsigned char id[512]) {
    ui_status("ATA Secure Erase", "Firmware command running; its duration depends on the disk");
    uint16_t s = word_le(id,128);
    if (!(s & 1)) { fprintf(stderr,"ATA Security not reported by IDENTIFY; aborting.\n"); return -1; }
    if (s & 8) { fprintf(stderr,"The disk is FROZEN; no change attempted. Unfreeze it (e.g. suspend/resume) before trying again.\n"); return -1; }
    if (s & 2) { fprintf(stderr,"ATA security is already enabled; aborting for safety.\n"); return -1; }
    if (s & 4) { fprintf(stderr,"The disk is LOCKED; aborting.\n"); return -1; }
    int enhanced = !!(s & 0x20);
    fprintf(stderr,"ATA %s erase requested (may take several hours)…\n", enhanced ? "enhanced" : "standard");
    unsigned char payload[512] = {0};
    /*
     * ATA security data block: word 0 = identifier (bit 0: 0 = user) and
     * level (bit 8: 0 = high), then a 32-byte password padded with zeros.
     */
    memcpy(payload + 2, ATA_TEMP_PASSWORD, strlen(ATA_TEMP_PASSWORD));
    fprintf(stderr,
            "Temporary ATA user password: \"%s\".\n"
            "If interrupted (power loss, crash), unlock the disk with:\n"
            "  hdparm --user-master u --security-unlock %s %s\n"
            "  hdparm --user-master u --security-disable %s %s\n",
            ATA_TEMP_PASSWORD, ATA_TEMP_PASSWORD, ui_device, ATA_TEMP_PASSWORD, ui_device);
    if (sec_cmd(fd, ATA_SET_PASSWORD, 1, payload, sizeof(payload), 30000) < 0) {
        fprintf(stderr,"SET PASSWORD failed.\n"); return -1;
    }
    int result = -1;
    if (sec_cmd(fd, ATA_ERASE_PREPARE, 0, NULL, 0, 30000) < 0) {
        fprintf(stderr,"ERASE PREPARE failed; trying to remove the password.\n");
        goto cleanup;
    }
    /* SECURITY ERASE UNIT: Enhanced mode is selected by bit 1 of data word 0. */
    payload[0] = enhanced ? 0x02 : 0x00;
    if (sec_cmd(fd, ATA_ERASE_UNIT, 1, payload, sizeof(payload), ATA_TIMEOUT_MS) < 0) {
        fprintf(stderr,"SECURITY ERASE UNIT failed; trying to remove the password.\n");
        goto cleanup;
    }
    result = 0; fprintf(stderr,"ATA Secure Erase complete (the disk removes the password itself).\n");
cleanup:
    if (result < 0) {
        /* SECURITY DISABLE PASSWORD with the same user password. */
        payload[0] = 0;
        if (sec_cmd(fd, ATA_DISABLE_PASSWORD, 1, payload, sizeof(payload), 30000) < 0)
            fprintf(stderr,"WARNING: removing the password failed. The disk may stay locked; "
                           "use the hdparm commands above with the password \"%s\".\n", ATA_TEMP_PASSWORD);
        else fprintf(stderr,"ATA password removed.\n");
    }
    memset(payload, 0, sizeof(payload));
    return result;
}

/* ---------- Media type and SSD-friendly operations (TRIM, Sanitize / Format NVM) ---------- */
static int read_sysfs_value(const char *path, char *out, size_t out_size);
enum { MEDIA_UNKNOWN, MEDIA_HDD, MEDIA_SSD };
typedef struct { int kind; char why[128]; } media_t;
/* Kernel queue/rotational flag (whole disk, or parent disk for a partition). */
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
 * SSD or hard disk? The firmware is authoritative: NVMe, IDENTIFY word 217 (ATA), VPD page B1h (SCSI).
 * The kernel's rotational flag is only a last resort: it is wrong for an NVMe device
 * backed by a loop file, and defaults to 1 for many virtual devices.
 */
static media_t media_detect(int fd) {
    media_t m;
    m.kind = MEDIA_UNKNOWN;
    snprintf(m.why, sizeof(m.why), "unknown media type");
    if (ioctl(fd, NVME_IOCTL_ID) > 0) { m.kind = MEDIA_SSD; snprintf(m.why, sizeof(m.why), "NVMe SSD"); return m; }
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
        snprintf(m.why, sizeof(m.why), "%s SSD (the firmware reports a non-rotating medium)", bus);
        return m;
    }
    if (r >= 0x0401 && r <= 0xfffe) {
        m.kind = MEDIA_HDD;
        snprintf(m.why, sizeof(m.why), "%s hard disk, %u rpm", bus, r);
        return m;
    }
    int rot = sysfs_rotational(fd);
    if (rot == 0) { m.kind = MEDIA_SSD; snprintf(m.why, sizeof(m.why), "non-rotational according to the kernel (probably an SSD)"); }
    else if (rot == 1) { m.kind = MEDIA_HDD; snprintf(m.why, sizeof(m.why), "rotational according to the kernel"); }
    return m;
}
static void ssd_explain_zero(const media_t *m, int cli) {
    fprintf(stderr,
        "Zero-fill not recommended on this disk (%s).\n"
        "On an SSD, filling the disk with zeros wears the flash memory without repairing anything. It is not a\n"
        "secure erase either: the spare blocks keep copies of the old data. And the controller, which then\n"
        "believes the disk is full, slows down until the next TRIM.\n", m->why);
    if (cli)
        fprintf(stderr,
            "Instead: --trim empties the SSD in seconds, without wear; --secure-erase erases it securely\n"
            "through the firmware. To write the zeros anyway: --zero --force.\n");
}
static void ssd_explain_repair(const media_t *m, int cli) {
    fprintf(stderr,
        "Slow-block repair not recommended on this disk (%s).\n"
        "Rewriting slow zones heals the weak sectors of a hard disk. An SSD already moves and refreshes\n"
        "its data by itself: a slow read there rather points to a struggling controller or flash memory,\n"
        "which a rewrite does not heal.\n", m->why);
    if (cli)
        fprintf(stderr,
            "Instead: --scan-slow for diagnostics (read-only) and --smart for the wear level.\n"
            "To rewrite anyway: --slow-repair --force.\n");
}

/* Reads back 1 MiB at the start, middle and end: 1 = all zero, 0 = non-zero data, -1 = error. */
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

/* TRIM of the whole disk in 1 GiB chunks (visible progress, can be interrupted). */
#define TRIM_CHUNK (1ULL << 30)
static int trim_disk(int fd, uint64_t size) {
    ui_status("TRIM", "The disk marks all its space as free");
    fprintf(stderr, "TRIM of %" PRIu64 " bytes: the disk marks all its space as free…\n", size);
    progress_t p; progress_begin(&p, "TRIM", size);
    for (uint64_t off = 0; off < size;) {
        uint64_t n = size - off > TRIM_CHUNK ? TRIM_CHUNK : size - off;
        uint64_t range[2] = {off, n};
        if (ioctl(fd, BLKDISCARD, range) < 0) {
            if (off == 0 && (errno == EOPNOTSUPP || errno == ENOTTY))
                fprintf(stderr, "This disk does not support TRIM/discard (%s): nothing was modified.\n", strerror(errno));
            else
                fprintf(stderr, "\nTRIM interrupted at 0x%" PRIx64 ": %s\n", off, strerror(errno));
            return -1;
        }
        off += n;
        progress_show(&p, "TRIM", off, size);
    }
    fprintf(stderr, "\n");
    int z = sample_is_zero(fd, size);
    if (z < 0) { fprintf(stderr, "Read-back after TRIM failed: %s\n", strerror(errno)); return -1; }
    if (z) fprintf(stderr, "Sample read-back (start, middle, end): everything is zero.\n");
    else fprintf(stderr, "Sample read-back: some data is still readable (this disk does not guarantee zeros after TRIM).\n");
    fprintf(stderr, "TRIM complete. This is not a secure erase: to make the data unrecoverable, use Secure Erase.\n");
    return 0;
}

static uint32_t le32_at(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
enum { NVE_NONE, NVE_SANITIZE_CRYPTO, NVE_SANITIZE_BLOCK, NVE_FORMAT_CRYPTO, NVE_FORMAT_ERASE };
typedef struct { int method; char why[480]; } nvme_plan_t;
static const char *nvme_method_name(int m) {
    return m == NVE_SANITIZE_CRYPTO ? "Sanitize, cryptographic erase"
         : m == NVE_SANITIZE_BLOCK ? "Sanitize, block erase"
         : m == NVE_FORMAT_CRYPTO ? "Format NVM, cryptographic erase"
         : m == NVE_FORMAT_ERASE ? "Format NVM, user data erase" : "none";
}
/*
 * Chooses the erase method from Identify Controller (OACS, SANICAP, FNA).
 * active_ns: number of active namespaces (< 0: unknown). Sanitize acts on the whole
 * subsystem, and so does Format when FNA says so: with several namespaces they would
 * erase the others too, hence a refusal. Sanitize overwrite (which wears the flash) is
 * never offered. Capabilities are checked here instead of relying on the disk to refuse:
 * QEMU, for example, accepts a cryptographic erase that it does not report.
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
        snprintf(p->why, sizeof(p->why), "The disk changes its internal encryption key: instant and without wear. "
                 "Sanitize erases the whole disk, spare blocks and caches included, and resumes by itself after a power loss.");
        return 0;
    case NVE_SANITIZE_BLOCK:
        snprintf(p->why, sizeof(p->why), "Every flash memory block is erased (a few seconds to a few minutes). "
                 "Sanitize erases the whole disk, spare blocks and caches included, and resumes by itself after a power loss.");
        return 0;
    case NVE_FORMAT_CRYPTO:
    case NVE_FORMAT_ERASE:
        snprintf(p->why, sizeof(p->why), "%s%s The current sector format is kept.",
                 (sanicap & 3) ? "Sanitize is not used because the disk has several namespaces (or an unknown number). "
                               : "The disk does not offer Sanitize. ",
                 p->method == NVE_FORMAT_CRYPTO ? "The disk changes its internal encryption key: instant and without wear."
                                                : "The disk does not report cryptographic erase: it erases the user data.");
        return 0;
    default: break;
    }
    if (!can_format && !(sanicap & 3))
        snprintf(p->why, sizeof(p->why), "This NVMe device offers neither Sanitize (cryptographic or block) nor Format NVM%s: "
                 "no firmware erase is possible, nothing was modified.",
                 (sanicap & 4) ? " (only Sanitize overwrite is offered: it wears the flash, DiskMedic does not use it)" : "");
    else
        snprintf(p->why, sizeof(p->why), "This NVMe device has several active namespaces (or an unknown number): Sanitize or Format "
                 "would erase them all. Refused for safety, nothing was modified.");
    return -1;
}
static int nvme_active_namespaces(int fd, const unsigned char *ctrl) {
    unsigned char *list = alloc_aligned(4096);
    int n = -1;
    if (list) {
        memset(list, 0, 4096);
        if (nvme_admin(fd, 0x06, 0, 2, list, 4096) == 0)   /* CNS 02h: active namespace list */
            for (n = 0; n < 1024 && le32_at(list + 4 * n); n++) {}
        free(list);
    }
    if (n < 0 && le32_at(ctrl + 516) == 1) n = 1;          /* NN: only one namespace possible */
    return n;
}
/* Prepares the erase plan: 0 if a method is possible; ctrl receives Identify Controller. */
static int nvme_erase_plan(int fd, unsigned char *ctrl, nvme_plan_t *p) {
    p->method = NVE_NONE;
    memset(ctrl, 0, 4096);
    if (nvme_admin(fd, 0x06, 0, 1, ctrl, 4096) < 0) {
        snprintf(p->why, sizeof(p->why), "NVMe Identify Controller failed: erase not possible.");
        return -1;
    }
    return nvme_erase_choose(ctrl, nvme_active_namespaces(fd, ctrl), p);
}
/* Announces the method before confirmation; -1 if none is possible. */
static int nvme_erase_preview(int fd) {
    unsigned char *ctrl = alloc_aligned(4096);
    nvme_plan_t plan;
    if (!ctrl) { perror("allocation"); return -1; }
    int r = nvme_erase_plan(fd, ctrl, &plan);
    if (r == 0) fprintf(stderr, "NVMe Secure Erase: %s.\n%s\n", nvme_method_name(plan.method), plan.why);
    else fprintf(stderr, "%s\n", plan.why);
    free(ctrl);
    return r;
}
static void nvme_status_error(const char *what, int st) {
    if (st > 0) {
        int sct = (st >> 8) & 7, sc = st & 0xff;
        fprintf(stderr, "%s refused by the disk (NVMe status 0x%03x%s).\n", what, st & 0x7ff,
                !sct && sc == 0x01 ? ": command not supported" :
                !sct && sc == 0x02 ? ": invalid field" :
                !sct && sc == 0x1d ? ": Sanitize prohibited while a firmware activation is pending" :
                sct == 1 && sc == 0x0a ? ": invalid format" : "");
    } else fprintf(stderr, "%s failed: %s\n", what, strerror(errno));
}
/* Sanitize Status log (81h): SPROG = progress out of 65536, SSTAT bits 2:0 = state. */
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
        fprintf(stderr, "NVMe Identify Namespace failed: erase not possible, nothing was modified.\n"); goto out;
    }
    fprintf(stderr, "Method: %s.\n", nvme_method_name(plan.method));
    if (plan.method == NVE_SANITIZE_CRYPTO || plan.method == NVE_SANITIZE_BLOCK) {
        unsigned sp = 0, ss = 0;
        if (nvme_sanitize_status(fd, &sp, &ss) == 0 && (ss & 7) == 2) {
            fprintf(stderr, "A Sanitize is already running on this disk: wait for it to finish before starting another one.\n"); goto out;
        }
        /* SANACT: 4 = cryptographic, 2 = block; AUSE (bit 3): unrestricted exit from failure mode. */
        uint32_t cdw10 = (plan.method == NVE_SANITIZE_CRYPTO ? 4U : 2U) | (1U << 3);
        ui_status("NVMe Sanitize", "Firmware command running; it continues even if DiskMedic stops");
        if (nvme_admin_full(fd, 0x84, 0, cdw10, NULL, 0, 30000, &st) < 0) { nvme_status_error("Sanitize", st); goto out; }
        progress_t p; progress_begin(&p, "Sanitize", size);
        for (int idle = 0;;) {
            struct timespec pause = {0, 500 * 1000 * 1000};
            nanosleep(&pause, NULL);
            if (nvme_sanitize_status(fd, &sp, &ss) < 0) {
                fprintf(stderr, "\nSanitize log unreadable: %s. The disk continues the erase on its own.\n", strerror(errno)); goto out;
            }
            unsigned state = ss & 7;
            if (state == 2) { progress_show(&p, "Sanitize", (uint64_t)((double)sp / 65536.0 * (double)size), size); continue; }
            if (state == 1 || state == 4) { progress_show(&p, "Sanitize", size, size); fprintf(stderr, "\nSanitize complete.\n"); break; }
            if (state == 3) {
                fprintf(stderr, "\nThe disk reports that the Sanitize failed: the data may not be erased. "
                                "Run Secure Erase again; if it keeps failing, the disk is probably faulty.\n");
                goto out;
            }
            if (++idle > 60) { fprintf(stderr, "\nThe disk reports no Sanitize in progress or completed: result unknown.\n"); goto out; }
        }
    } else {
        uint8_t flbas = ns[26], dps = ns[29];
        uint32_t ses = plan.method == NVE_FORMAT_CRYPTO ? 2U : 1U;
        /* Format kept: LBAF (FLBAS bits 3:0 and 6:5), metadata (bit 4), protection (DPS). */
        uint32_t cdw10 = (flbas & 0x0fU) | ((flbas & 0x10U) ? 1U << 4 : 0) | ((uint32_t)(dps & 7U) << 5) |
                         ((dps & 8U) ? 1U << 8 : 0) | (ses << 9) | ((uint32_t)((flbas >> 5) & 3U) << 12);
        ui_status("NVMe Format NVM", "Firmware command running; its duration depends on the disk");
        fprintf(stderr, "Format NVM of namespace %d in progress…\n", nsid);
        if (nvme_admin_full(fd, 0x80, (uint32_t)nsid, cdw10, NULL, 0, ATA_TIMEOUT_MS, &st) < 0) {
            nvme_status_error("Format NVM", st); goto out;
        }
        fprintf(stderr, "Format NVM complete.\n");
    }
    int z = sample_is_zero(fd, size);
    if (z == 1) fprintf(stderr, "Sample read-back (start, middle, end): everything is zero.\n");
    else if (z == 0 && (plan.method == NVE_SANITIZE_CRYPTO || plan.method == NVE_FORMAT_CRYPTO))
        fprintf(stderr, "Sample read-back: random data, as expected after a cryptographic erase (the old key is destroyed).\n");
    else if (z == 0) fprintf(stderr, "Sample read-back: non-zero data is still readable; this disk does not return zeros after an erase.\n");
    else fprintf(stderr, "Sample read-back failed (%s): the kernel may still be rescanning the disk.\n", strerror(errno));
    rc = 0;
out:
    free(ctrl); free(ns); return rc;
}
/* Secure Erase: NVMe (Sanitize or Format NVM), otherwise ATA Security Erase (SATA, SSD or hard disk). */
static int secure_erase_any(int fd, uint64_t size) {
    if (ioctl(fd, NVME_IOCTL_ID) > 0) return nvme_secure_erase(fd, size);
    unsigned char id[512];
    if (identify(fd, id) < 0) {
        fprintf(stderr, "IDENTIFY DEVICE failed: Secure Erase not possible (SAS/SCSI disk or USB bridge?).\n");
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
/* Sorts v in place and returns its median. */
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
 * Minimum gap to flag a zone on the relative criterion alone (time ≥ factor × reference):
 * on very fast media (a reference of a few hundredths of a ms), measurement noise would
 * otherwise exceed the threshold. A weak sector costs tens of ms: it is still detected.
 */
#define ZONE_MIN_EXCESS_MS 1.0
/*
 * Same principle for 1 MiB blocks: a block is slow only if it exceeds the median by at
 * least 5 ms. On an NVMe or a cached file (median < 1 ms), the system's pauses of a few ms
 * would otherwise exceed median × factor, sometimes on both passes. A re-read on a hard
 * disk costs at least one platter revolution (8 to 11 ms): it is still detected.
 */
#define BLOCK_MIN_EXCESS_MS 5.0

/* Slow zone located inside a 1 MiB block. */
typedef struct {
    size_t block;           /* index in the list of reported blocks */
    uint64_t off;
    size_t len;
    double t, base, t_after;
    int written, verified, still_slow;
} zone_t;

/*
 * Flushes the disk's internal cache as well as possible by reading 64 MiB from a distant
 * region: otherwise a re-read would be served from the disk's memory and look fast.
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
 * Times each zone of a block, from the last one to the first: disks do not read ahead
 * backwards, so each zone is read from the medium. A preliminary read just after the
 * block positions the head without prefetching the block itself.
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

/* Reference: median time of a zone over three healthy blocks, measured the same way. */
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
    if (ms < 0) snprintf(out, n, "n/a"); else snprintf(out, n, "%.3f ms", ms);
}

/*
 * Slow-block scan (and repair if repair is set):
 *   1. pass 1: time of each 1 MiB block, median and threshold = median × factor;
 *   2. pass 2: a block is confirmed only if it exceeds the threshold on both passes
 *      (the whole disk is read in between, so its cache cannot skew the measurement);
 *   3. location: each confirmed block is measured again in 64 KiB zones, compared to
 *      a reference taken on healthy blocks (3rd measurement: if the slowness does not
 *      reproduce, nothing is rewritten); when repairing, only slow zones are rewritten;
 *   4. deferred check: after flushing the disk's cache, each rewritten zone is timed
 *      again to find out whether it is still slow.
 * In an unreadable block, only the physical sectors that contain a lost sector are
 * rewritten (with zeros for the unreadable part).
 */
static int slow_scan(int fd, uint64_t size, double factor, int repair) {
    const uint64_t total = size / BUF_SIZE + (size % BUF_SIZE != 0);
    const size_t sample_cap = (size_t)1U << 20;
    const char *op = repair ? "slow-repair" : "scan-slow";
    const size_t zsz = phys_sector_size > ZONE_SIZE ? phys_sector_size : ZONE_SIZE;
    const double zone_factor = factor > 2.0 ? factor : 2.0;
    if (!total || total > (uint64_t)INT64_MAX / BUF_SIZE) {
        fprintf(stderr, "Invalid or too large device size.\n");
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
        fprintf(stderr, "Warning: not enough memory to keep the pass 1 times; "
                        "blocks will not be confirmed by two measurements.\n");
    size_t nsample = 0;
    uint64_t nvalid = 0, errors = 0, bad_sectors = 0;
    uint64_t rng = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)buf ^ size;
    if (!rng) rng = UINT64_C(0x9e3779b97f4a7c15);

    /* Pass 1: reference measurement. */
    fprintf(stderr, "Pass 1/2: timing %" PRIu64 " blocks of 1 MiB (%s)…\n",
            total, io.dfd >= 0 ? "O_DIRECT, no cache" : "buffered I/O, cache possible");
    progress_t progress; progress_begin(&progress, "Pass 1/2: timing", size);
    for (uint64_t i = 0; i < total; i++) {
        uint64_t off = i * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        double t;
        int r = timed_pread(&io, buf, n, off, &t);
        if (pass1) pass1[i] = r == 0 ? (float)t : -1.0f;
        if (r != 0) {
            /*
             * Read error on pass 1: locate and log the exact LBAs right away, so that an error
             * that does not come back on pass 2 (transient) is not lost.
             */
            errors++;
            fprintf(stderr, "\nRead error in block %" PRIu64 " (pass 1); checking sector by sector…\n", i);
            uint64_t bad = read_sectors(&io, buf, n, off, op, repair ? "PENDING" : "NONE", 1, NULL);
            if (bad == UINT64_MAX) goto out;
            bad_sectors += bad;
        }
        if (r == 0) {
            nvalid++;
            if (nsample < sample_cap) sample[nsample++] = t;
            else {
                uint64_t j = sample_random(&rng) % nvalid;
                if (j < sample_cap) sample[(size_t)j] = t;
            }
        }
        if ((i + 1) % 16 == 0 || i + 1 == total)
            progress_show(&progress, "Pass 1/2: timing", (i + 1 == total) ? size : (i + 1) * (uint64_t)BUF_SIZE, size);
    }
    fprintf(stderr, "\n");
    if (!nsample) { fprintf(stderr, "No successful read.\n"); goto out; }
    double median = median_of(sample, nsample);
    double threshold = median * factor;
    int floor_used = threshold < median + BLOCK_MIN_EXCESS_MS;
    if (floor_used) threshold = median + BLOCK_MIN_EXCESS_MS;
    free(sample); sample = NULL;
    if (floor_used)
        printf("Median reference: %.3f ms/block; threshold: %.3f ms (fast media: minimum gap of %.0f ms).\n",
               median, threshold, BLOCK_MIN_EXCESS_MS);
    else printf("Median reference: %.3f ms/block; threshold: %.3f ms (factor %.2f).\n", median, threshold, factor);

    /* Pass 2: confirmation. */
    fprintf(stderr, "Pass 2/2: confirming slow blocks…\n");
    progress_begin(&progress, "Pass 2/2: confirmation", size);
    for (uint64_t i = 0; i < total; i++) {
        uint64_t off = i * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        double ms;
        int failed1 = pass1 && pass1[i] < 0;
        if (timed_pread(&io, buf, n, off, &ms) < 0) {
            ms = -1.0;
            if (!failed1) {   /* new error: locate and log it (already done on pass 1 otherwise) */
                errors++;
                fprintf(stderr, "\nRead error in block %" PRIu64 " (pass 2); checking sector by sector…\n", i);
                uint64_t bad = read_sectors(&io, buf, n, off, op, repair ? "PENDING" : "NONE", 1, NULL);
                if (bad == UINT64_MAX) goto out;
                bad_sectors += bad;
            }
        }
        if (ms < 0 || ms > threshold || failed1) {
            if (nslow == cap) {
                size_t nc = cap ? cap * 2 : 16;
                if (nc < cap || nc > SIZE_MAX / sizeof(*slow)) { fprintf(stderr, "Slow block list too large.\n"); goto out; }
                slow_block_t *p = realloc(slow, nc * sizeof(*p));
                if (!p) { perror("realloc"); goto out; }
                slow = p; cap = nc;
            }
            double ms1 = pass1 ? (double)pass1[i] : -2.0;
            int status = ms < 0 ? SB_UNREADABLE : failed1 ? SB_TRANSIENT
                       : (!pass1 || ms1 < 0 || ms1 > threshold) ? SB_CONFIRMED : SB_INTERMITTENT;
            slow[nslow++] = (slow_block_t){.index = i, .ms = ms, .ms1 = ms1, .status = status};
        }
        if ((i + 1) % 16 == 0 || i + 1 == total)
            progress_show(&progress, "Pass 2/2: confirmation", (i + 1 == total) ? size : (i + 1) * (uint64_t)BUF_SIZE, size);
    }
    fprintf(stderr, "\n");

    /* Location (and targeted rewrite) inside confirmed blocks. */
    size_t nconf = 0;
    for (size_t k = 0; k < nslow; k++) nconf += slow[k].status == SB_CONFIRMED;
    double ref = -1.0;
    if (nconf) {
        ref = calibrate_zone_ref(&io, buf, tmp, size, total, pass1, median, zsz);
        fprintf(stderr, "Locating in %zu confirmed block(s), %zu KiB zones", nconf, zsz / 1024);
        if (ref > 0) fprintf(stderr, " (healthy reference zone: %.3f ms)", ref);
        fprintf(stderr, "…\n");
    }
    free(pass1); pass1 = NULL;
    size_t done_conf = 0;
    progress_begin(&progress, repair ? "Location and rewrite" : "Location", (uint64_t)nconf * BUF_SIZE);
    for (size_t k = 0; k < nslow; k++) {
        if (slow[k].status != SB_CONFIRMED) continue;
        uint64_t off = slow[k].index * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        double zt[ZONES_PER_BLOCK];
        size_t nz = 0;
        int mr = measure_zones(&io, buf, tmp, size, off, n, zsz, zt, &nz);
        done_conf++;
        progress_show(&progress, repair ? "Location and rewrite" : "Location",
                      (uint64_t)done_conf * BUF_SIZE, (uint64_t)nconf * BUF_SIZE);
        if (mr < 0) {
            /* Became unreadable since pass 2: handled as an unreadable block. */
            slow[k].status = SB_UNREADABLE; errors++;
            fprintf(stderr, "\nBlock %" PRIu64 " became unreadable; checking sector by sector…\n", slow[k].index);
            uint64_t bad = read_sectors(&io, buf, n, off, op, repair ? "PENDING" : "NONE", 1, NULL);
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
                if (zone_push(&zones, &nzones, &zcap, &zn) < 0) { perror("zone allocation"); goto out; }
            }
        }
        if (nzones == first) {
            /* No zone stands out: slowness spread over the whole block, or not reproduced. */
            if (sum - (double)nz * base >= 0.5 * excess) {
                for (size_t z = 0; z < nz; z++) {
                    size_t zo = z * zsz;
                    zone_t zn = {.block = k, .off = off + zo, .len = n - zo < zsz ? n - zo : zsz, .t = zt[z], .base = base, .t_after = -1};
                    if (zone_push(&zones, &nzones, &zcap, &zn) < 0) { perror("zone allocation"); goto out; }
                }
            } else { slow[k].status = SB_NOT_REPRODUCED; continue; }
        }
        if (!repair) continue;
        /* buf holds the block's data that was just read: only the slow zones are rewritten. */
        for (size_t j = first; j < nzones; j++) {
            if (dio_pwrite(&io, buf + (zones[j].off - off), zones[j].len, zones[j].off) == 0) zones[j].written = 1;
            else fprintf(stderr, "\nZone write failed at LBA %" PRIu64 ": %s\n", zones[j].off / log_sector_size, strerror(errno));
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

    /* Deferred check: the disk's cache is flushed before each new measurement. */
    size_t nverif = 0;
    for (size_t j = 0; j < nzones; j++) nverif += zones[j].verified != 0;
    if (repair && nverif) {
        fprintf(stderr, "Deferred check of %zu rewritten zone(s)…\n", nverif);
        size_t done = 0;
        progress_begin(&progress, "Deferred check", 0);
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
            progress_show(&progress, "Deferred check", (uint64_t)done * zones[j].len, (uint64_t)nverif * zones[j].len);
        }
        fprintf(stderr, "\n");
    }

    /* Report and log. */
    size_t n_unread = 0, n_conf = 0, n_inter = 0, n_trans = 0;
    size_t z_ok = 0, z_still = 0, z_fail = 0, u_fixed = 0, u_fail = 0;
    uint64_t zeroed = 0;
    for (size_t k = 0; k < nslow; k++) {
        n_unread += slow[k].status == SB_UNREADABLE;
        n_conf += slow[k].status == SB_CONFIRMED;
        n_inter += slow[k].status == SB_INTERMITTENT || slow[k].status == SB_NOT_REPRODUCED;
        n_trans += slow[k].status == SB_TRANSIENT;
    }
    printf("Reported blocks: %zu — confirmed slow: %zu, intermittent (not confirmed): %zu, unreadable: %zu "
           "(unreadable sectors: %" PRIu64 "), transient read errors: %zu.\n",
           nslow, n_conf, n_inter, n_unread, bad_sectors, n_trans);
    size_t zi = 0;
    for (size_t k = 0; k < nslow; k++) {
        uint64_t off = slow[k].index * (uint64_t)BUF_SIZE;
        size_t n = (size - off > BUF_SIZE) ? BUF_SIZE : (size_t)(size - off);
        char m1[32], m2[32], state[160], action[160];
        fmt_ms(m1, sizeof(m1), slow[k].ms1); fmt_ms(m2, sizeof(m2), slow[k].ms);
        printf("  block %" PRIu64 " (LBA %" PRIu64 "): ", slow[k].index, off / log_sector_size);
        if (slow[k].status == SB_TRANSIENT) {
            printf("read error on pass 1 only, readable on pass 2 (%s) — not rewritten\n", m2);
            snprintf(state, sizeof(state), "TRANSIENT READ ERROR: pass 1 failed, pass 2 read in %s", m2);
            if (log_event(op, off / log_sector_size, state, repair ? "NONE (readable again, not rewritten)" : "NONE") < 0) goto out;
            continue;
        }
        if (slow[k].status == SB_INTERMITTENT || slow[k].status == SB_NOT_REPRODUCED) {
            if (slow[k].status == SB_INTERMITTENT)
                snprintf(state, sizeof(state), "INTERMITTENT: pass 1 %s, pass 2 %s", m1, m2);
            else
                snprintf(state, sizeof(state), "INTERMITTENT: passes 1/2 %s / %s, not reproduced during location", m1, m2);
            printf("%s — not rewritten\n", state + strlen("INTERMITTENT: "));
            if (log_event(op, off / log_sector_size, state, repair ? "NONE (not confirmed, not rewritten)" : "NONE") < 0) goto out;
            continue;
        }
        if (slow[k].status == SB_UNREADABLE) {
            printf("unreadable");
            if (!repair) { printf("\n"); continue; }
            /* Rewrite only the physical sectors that contain a lost logical sector. */
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
            if (!bad) snprintf(action, sizeof(action), "NONE: sectors read back successfully");
            else if (wfail == 1) { snprintf(action, sizeof(action), "FAILED: write/sync"); u_fail++; }
            else if (wfail == 2) {
                snprintf(action, sizeof(action), "FAILED: verification (%zu physical sector(s) rewritten, still unreadable)", nphys);
                u_fail++;
            }
            else {
                snprintf(action, sizeof(action), "REWRITE VERIFIED; %" PRIu64 " unreadable sector(s) replaced with zeros "
                         "(%zu physical sector(s) rewritten)", bad, nphys);
                u_fixed++; zeroed += bad;
            }
            printf(" — %s\n", action);
            /* Final outcome logged for each lost LBA (the pass 1/2 entries say PENDING). */
            if (!bad) {
                if (log_event(op, off / log_sector_size, "DEFECTIVE: read failed earlier, readable now", action) < 0) goto out;
            } else {
                for (size_t idx = 0; idx < n / log_sector_size; idx++)
                    if ((badmap[idx / 8] & (1U << (idx % 8))) &&
                        log_event(op, off / log_sector_size + idx, "DEFECTIVE: read failed", action) < 0) goto out;
            }
            continue;
        }
        printf("slow (pass 1: %s, pass 2: %s)\n", m1, m2);
        for (; zi < nzones && zones[zi].block < k; zi++) {}
        for (; zi < nzones && zones[zi].block == k; zi++) {
            zone_t *z = &zones[zi];
            uint64_t lba = z->off / log_sector_size, nsec = z->len / log_sector_size;
            printf("    zone LBA %" PRIu64 "–%" PRIu64 " (%zu KiB): %.3f ms (healthy zone: %.3f ms)",
                   lba, lba + nsec - 1, z->len / 1024, z->t, z->base);
            if (!repair) snprintf(action, sizeof(action), "NONE");
            else if (!z->written) { snprintf(action, sizeof(action), "FAILED: write/sync"); z_fail++; }
            else if (!z->verified) { snprintf(action, sizeof(action), "FAILED: verification"); z_fail++; }
            else if (z->still_slow) { snprintf(action, sizeof(action), "REWRITE VERIFIED, STILL SLOW (%.3f ms)", z->t_after); z_ok++; z_still++; }
            else { snprintf(action, sizeof(action), "REWRITE VERIFIED, FAST (%.3f ms)", z->t_after); z_ok++; }
            if (repair) printf(" → %s", action);
            printf("\n");
            snprintf(state, sizeof(state), "SLOW: zone LBA %" PRIu64 "-%" PRIu64 " (%zu KiB) %.3f ms (healthy zone %.3f ms); block %s / %s",
                     lba, lba + nsec - 1, z->len / 1024, z->t, z->base, m1, m2);
            if (log_event(op, lba, state, action) < 0) goto out;
        }
    }
    if (repair)
        printf("Repair report: zones rewritten and verified=%zu (still slow=%zu), failures=%zu; "
               "unreadable blocks repaired=%zu, failures=%zu, sectors replaced with zeros=%" PRIu64 "; "
               "intermittent blocks not rewritten=%zu.\n",
               z_ok, z_still, z_fail, u_fixed, u_fail, zeroed, n_inter);
    rc = repair ? ((z_fail || u_fail || n_trans) ? -1 : 0) : (errors ? -1 : 0);
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
    char kind[8];                       /* "HDD", "SSD", "NVMe" or "?" (TUI only) */
    int in_use, system_disk;            /* mounted/held; holds the root filesystem (TUI only) */
} disk_info_t;

/* Metadata reused by the TUI; only the selected entries are kept. */
static disk_info_t tui_disks[MAX_DISKS];

/* Strips leading and trailing ASCII whitespace; returns the normalized string. */
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
    snprintf(disk->model, sizeof(disk->model), "Unknown");
    snprintf(disk->vendor, sizeof(disk->vendor), "Unknown");
    snprintf(disk->serial, sizeof(disk->serial), "Unknown");
    if (!strncmp(name, "nvme", 4)) snprintf(disk->type, sizeof(disk->type), "NVMe");
    else if (!strncmp(name, "sd", 2)) snprintf(disk->type, sizeof(disk->type), "SCSI/SATA (sd)");
    else if (!strncmp(name, "hd", 2)) snprintf(disk->type, sizeof(disk->type), "IDE (hd)");
    else snprintf(disk->type, sizeof(disk->type), "Virtual (vd)");

    snprintf(path, sizeof(path), "/sys/block/%.255s/device/model", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->model, sizeof(disk->model), "%s", value);
    snprintf(path, sizeof(path), "/sys/block/%.255s/device/vendor", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->vendor, sizeof(disk->vendor), "%s", value);
    snprintf(path, sizeof(path), "/sys/block/%.255s/device/serial", name);
    if (read_sysfs_value(path, value, sizeof(value)) == 0) {
        snprintf(disk->serial, sizeof(disk->serial), "%s", value);
    } else {
        /* Prefer the SCSI VPD page 0x80 serial number over the generic WWID. */
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
            if (!strcmp(disk->model, "Unknown")) {
                snprintf(path, sizeof(path), "/sys/class/nvme/%.31s/model", controller);
                if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->model, sizeof(disk->model), "%s", value);
            }
            if (!strcmp(disk->serial, "Unknown")) {
                snprintf(path, sizeof(path), "/sys/class/nvme/%.31s/serial", controller);
                if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->serial, sizeof(disk->serial), "%s", value);
            }
            if (!strcmp(disk->vendor, "Unknown")) {
                snprintf(path, sizeof(path), "/sys/class/nvme/%.31s/device/vendor", controller);
                if (read_sysfs_value(path, value, sizeof(value)) == 0) snprintf(disk->vendor, sizeof(disk->vendor), "%s", value);
            }
        }
    }
    if (!strcmp(disk->serial, "Unknown") && strncmp(name, "nvme", 4)) {
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

/* Natural sort: sdb < sdz < sdaa, nvme2n1 < nvme10n1. */
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

/* 1 if path has a component equal to name ("/sys/.../block/sda/sda2" contains "sda"). */
static int path_has_component(const char *path, const char *name) {
    size_t l = strlen(name);
    for (const char *p = strchr(path, '/'); p; p = strchr(p + 1, '/'))
        if (!strncmp(p + 1, name, l) && (p[1 + l] == '/' || p[1 + l] == '\0')) return 1;
    return 0;
}
/* 1 if block device ma:mi is the disk, one of its partitions, or built on it (LVM, RAID, dm-crypt). */
static int blockdev_on_disk(unsigned ma, unsigned mi, const char *name, int depth) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", ma, mi);
    char *real = realpath(path, NULL);
    if (!real) return 0;
    int hit = path_has_component(real, name);
    if (!hit && depth < 4) {
        char slaves[PATH_MAX + 16];
        snprintf(slaves, sizeof(slaves), "%s/slaves", real);
        DIR *d = opendir(slaves);
        struct dirent *e;
        while (d && !hit && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char devf[PATH_MAX + 300], v[32];
            unsigned a, b;
            snprintf(devf, sizeof(devf), "%s/%.255s/dev", slaves, e->d_name);
            if (read_sysfs_value(devf, v, sizeof(v)) == 0 && sscanf(v, "%u:%u", &a, &b) == 2)
                hit = blockdev_on_disk(a, b, name, depth + 1);
        }
        if (d) closedir(d);
    }
    free(real);
    return hit;
}
/* Device holding the root filesystem (from /proc/self/mounts, or the st_dev of "/"); 0 if unknown. */
static int root_device(unsigned *ma, unsigned *mi) {
    char line[4096], src[PATH_MAX] = "";
    FILE *f = fopen("/proc/self/mounts", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char s[PATH_MAX], m[PATH_MAX];
            if (sscanf(line, "%4095s %4095s", s, m) == 2 && !strcmp(m, "/")) snprintf(src, sizeof(src), "%s", s);
        }
        fclose(f);
    }
    struct stat st;
    if (!strncmp(src, "/dev/", 5) && stat(src, &st) == 0 && S_ISBLK(st.st_mode)) {
        *ma = major(st.st_rdev); *mi = minor(st.st_rdev); return 1;
    }
    if (stat("/", &st) == 0 && major(st.st_dev) != 0) { *ma = major(st.st_dev); *mi = minor(st.st_dev); return 1; }
    return 0;
}
/* Media type and use of each disk, for the selection screens (read-only probes). */
static void load_disk_status(disk_info_t *disks, size_t count) {
    unsigned rma = 0, rmi = 0;
    int have_root = root_device(&rma, &rmi);
    for (size_t i = 0; i < count; i++) {
        disk_info_t *d = &disks[i];
        char dev[PATH_MAX + 128];
        snprintf(dev, sizeof(dev), "/dev/%.255s", d->name);
        snprintf(d->kind, sizeof(d->kind), "?");
        d->system_disk = have_root && blockdev_on_disk(rma, rmi, d->name, 0);
        /* O_EXCL is refused (EBUSY) while a partition is mounted or the disk is held (swap, LVM, RAID…). */
        int fd = open(dev, O_RDONLY | O_NONBLOCK | O_EXCL | O_CLOEXEC);
        if (fd < 0) {
            d->in_use = errno == EBUSY;
            fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        if (d->system_disk) d->in_use = 1;
        if (fd < 0) continue;
        media_t m = media_detect(fd);
        close(fd);
        if (!strncmp(d->name, "nvme", 4)) snprintf(d->kind, sizeof(d->kind), "NVMe");
        else if (m.kind == MEDIA_SSD) snprintf(d->kind, sizeof(d->kind), "SSD");
        else if (m.kind == MEDIA_HDD) snprintf(d->kind, sizeof(d->kind), "HDD");
    }
}
static const char *disk_badge(const disk_info_t *d) { return d->system_disk ? "SYSTEM" : d->in_use ? "IN USE" : ""; }
/* "model · serial X · 1.0 TB (931.5 GiB) · SSD · IN USE": the model comes first. */
static void disk_summary(const disk_info_t *d, char *out, size_t n) {
    char cap[80];
    fmt_capacity(cap, sizeof(cap), d->bytes);
    const char *badge = disk_badge(d);
    snprintf(out, n, "%.255s · serial %.255s · %s · %s%s%s", d->model, d->serial, cap, d->kind, *badge ? " · " : "", badge);
}
/* Framed block of the selected disks, using at most max_lines lines. */
static void print_selected_block(const unsigned char *selected, size_t count, size_t nsel, unsigned max_lines) {
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) if (selected[i]) total += tui_disks[i].bytes;
    char cap[80], row[1400], sum[1200];
    fmt_capacity(cap, sizeof(cap), total);
    snprintf(row, sizeof(row), "Selected disks (%zu) · total %s", nsel, cap);
    print_tui_line_attr(row, ui_col(COL_BOLD));
    size_t avail = max_lines > 1 ? max_lines - 1 : 1, room = nsel <= avail ? nsel : avail - 1, shown = 0;
    for (size_t i = 0; i < count && shown < room; i++) {
        if (!selected[i]) continue;
        const disk_info_t *d = &tui_disks[i];
        disk_summary(d, sum, sizeof(sum));
        snprintf(row, sizeof(row), "  /dev/%-8.64s %.1300s", d->name, sum);
        print_tui_line_attr(row, d->system_disk ? ui_col(COL_RED) : d->in_use ? ui_col(COL_YELLOW) : NULL);
        shown++;
    }
    if (shown < nsel) {
        snprintf(row, sizeof(row), "  … and %zu more disk(s): enlarge the window to see them", nsel - shown);
        print_tui_line(row);
    }
}

static unsigned terminal_rows(void);
static void print_tui_line_lr(const char *left, const char *right, const char *color);
static const char *op_label(int op);

/* ---------- Operation menu (single disk and multi-disk) ---------- */
typedef struct { int key; const char *name, *desc, *unavailable; } op_item_t;
static const op_item_t op_items_base[7] = {
    {'1', "SMART", "Identity and health report", NULL},
    {'2', "Read verification", "Reads the whole disk, no writes", NULL},
    {'3', "Slow scan", "Finds slow blocks in 2 passes, no writes", NULL},
    {'4', "Slow-block repair", "Scan, then rewrites the confirmed slow zones", NULL},
    {'5', "Zero-fill", "Writes zeros over the whole disk", NULL},
    {'6', "Secure Erase", "Firmware erase (ATA Security Erase, NVMe Sanitize/Format)", NULL},
    {'7', "TRIM", "Empties an SSD in seconds, without wear", NULL},
};
#define OP_MENU_LINES 10            /* lines drawn by op_menu_draw */
static void op_menu_draw(const op_item_t *it, int cursor, const char *hint) {
    char row[256], attr[32];
    for (int i = 0; i < 7; i++) {
        if (i == 3) print_tui_line_attr("   -- Destructive: all data on the disk is lost --", ui_col(COL_RED));
        if (it[i].unavailable)
            snprintf(row, sizeof(row), "%c %c  %-18s unavailable: %s", i == cursor ? '>' : ' ', it[i].key, it[i].name, it[i].unavailable);
        else snprintf(row, sizeof(row), "%c %c  %-18s %s", i == cursor ? '>' : ' ', it[i].key, it[i].name, it[i].desc);
        snprintf(attr, sizeof(attr), "%s%s", i == cursor ? COL_REV : "",
                 it[i].unavailable ? ui_col(COL_DIM) : i >= 3 ? ui_col(COL_RED) : "");
        print_tui_line_attr(row, attr);
    }
    print_tui_rule('M', 'M');
    print_tui_line(hint);
}
/* Moves the cursor; returns the chosen key ('1'..'7'), 0 if the key was used, otherwise the key itself. */
static int op_menu_key(const op_item_t *it, int *cursor, int c) {
    if (c == KEY_UP || c == 'k') { do *cursor = (*cursor + 6) % 7; while (it[*cursor].unavailable); return 0; }
    if (c == KEY_DOWN || c == 'j') { do *cursor = (*cursor + 1) % 7; while (it[*cursor].unavailable); return 0; }
    if (c == KEY_HOME) { *cursor = 0; return 0; }
    if (c == KEY_END) { *cursor = 6; while (it[*cursor].unavailable) (*cursor)--; return 0; }
    if (c == '\r' || c == '\n') return it[*cursor].key;
    if (c >= '1' && c <= '7') {
        if (it[c - '1'].unavailable) return 0;
        *cursor = c - '1';
        return c;
    }
    return c;
}

/* ---------- Pager: scrollable text in a frame (SMART report, disk log) ---------- */
static unsigned pager_body_rows = 10;
/* Copies one line for display: tabs and control characters become spaces, only color sequences are kept. */
static void pager_clean_line(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap;) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 0x1b) {
            size_t a = ansi_seq_len((const unsigned char *)s + i);
            if (a && i + a <= n && s[i + a - 1] == 'm' && o + a < cap) { memcpy(out + o, s + i, a); o += a; }
            i += a ? a : 1;
            continue;
        }
        out[o++] = ch < 0x20 || ch == 0x7f ? ' ' : (char)ch;
        i++;
    }
    out[o] = '\0';
}
/*
 * Draws text from line *top. With *follow set, shows the end (live log) ; reaching the end
 * by scrolling sets it again. title_color colors the line counter on the right.
 */
static void pager_draw(const char *title, const char *title_color, const char *buf, size_t len,
                       size_t *top, int *follow, const char *footer) {
    size_t n = 0, cap = 256, *start = malloc(cap * sizeof(*start));
    if (!start) return;
    start[n++] = 0;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n' && i + 1 < len) {
            if (n == cap) {
                size_t *p = realloc(start, (cap *= 2) * sizeof(*start));
                if (!p) break;
                start = p;
            }
            start[n++] = i + 1;
        }
    if (!len) n = 0;
    unsigned rows = terminal_rows();
    unsigned body = rows > 7 ? rows - 6 : 1;
    pager_body_rows = body;
    size_t maxtop = n > body ? n - body : 0;
    if (*follow || *top >= maxtop) { *top = maxtop; *follow = 1; }
    char *frame = NULL;
    size_t flen = 0;
    FILE *mf = open_memstream(&frame, &flen);
    if (!mf) { free(start); return; }
    ui_out = mf;
    char pos[64], line[2048];
    if (n) snprintf(pos, sizeof(pos), "lines %zu-%zu / %zu", *top + 1, *top + body < n ? *top + body : n, n);
    else snprintf(pos, sizeof(pos), "empty");
    fputs("\033[H", mf);
    print_tui_rule('T', 'T');
    print_tui_line_lr(title, pos, title_color);
    print_tui_rule('M', 'M');
    for (unsigned r = 0; r < body; r++) {
        size_t k = *top + r;
        if (k < n) {
            size_t end = k + 1 < n ? start[k + 1] - 1 : len;
            if (end > start[k] && buf[end - 1] == '\n') end--;
            pager_clean_line(buf + start[k], end - start[k], line, sizeof(line));
            print_tui_line(line);
        } else print_tui_line("");
    }
    print_tui_rule('M', 'M');
    print_tui_line(footer);
    print_tui_rule('B', 'B');
    fputs("\033[J", mf);
    ui_out = NULL;
    if (fclose(mf) == 0) { fwrite(frame, 1, flen, stderr); fflush(stderr); }
    free(frame);
    free(start);
}
/* Scroll keys shared by the pagers; returns 1 if the key was used. */
static int pager_key(int c, size_t *top, int *follow) {
    size_t page = pager_body_rows > 1 ? pager_body_rows - 1 : 1;
    if (c == KEY_UP || c == 'k') { *follow = 0; if (*top) (*top)--; return 1; }
    if (c == KEY_DOWN || c == 'j') { (*top)++; return 1; }
    if (c == KEY_PGUP) { *follow = 0; *top = *top > page ? *top - page : 0; return 1; }
    if (c == KEY_PGDN || c == ' ') { *top += page; return 1; }
    if (c == KEY_HOME) { *follow = 0; *top = 0; return 1; }
    if (c == KEY_END) { *follow = 1; return 1; }
    return c == KEY_RESIZE;
}
/* Blocking pager, closed by q, Escape or Enter. */
static void pager_run(const char *title, const char *title_color, const char *buf, size_t len) {
    size_t top = 0;
    int follow = 0;
    for (;;) {
        pager_draw(title, title_color, buf, len, &top, &follow,
                   "↑/↓ PgUp/PgDn Home/End scroll · q, Esc or Enter: back");
        int c = read_key();
        if (c == EOF || c == 'q' || c == 'Q' || c == KEY_ESC || c == '\r' || c == '\n') return;
        pager_key(c, &top, &follow);
    }
}
/* End of a file (at most cap bytes, starting on a whole line), as an allocated buffer. */
static char *read_file_tail(const char *path, size_t cap, size_t *len) {
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    char *b = NULL;
    if (fstat(fd, &st) == 0) {
        off_t off = st.st_size > (off_t)cap ? st.st_size - (off_t)cap : 0;
        size_t n = (size_t)(st.st_size - off);
        b = malloc(n + 1);
        if (b && n && full_pread(fd, b, n, off) < 0) { free(b); b = NULL; }
        if (b) {
            b[n] = '\0';
            char *s = b;
            if (off) { char *nl = memchr(b, '\n', n); if (nl) s = nl + 1; }
            *len = n - (size_t)(s - b);
            memmove(b, s, *len + 1);
        }
    }
    close(fd);
    return b;
}
/* SMART report of the single-disk menu, captured then shown in the pager (it used to be erased by the menu). */
static void smart_in_pager(int fd, const char *dev) {
    ui_status("SMART diagnostics", "Reading the identity and SMART attributes");
    fflush(stdout); fflush(stderr);
    FILE *tmp = tmpfile();
    int s1 = dup(STDOUT_FILENO), s2 = dup(STDERR_FILENO);
    if (!tmp || s1 < 0 || s2 < 0) {
        if (tmp) fclose(tmp);
        if (s1 >= 0) close(s1);
        if (s2 >= 0) close(s2);
        smart_report(fd); fflush(stdout);
        fprintf(stderr, "\nPress any key…"); fflush(stderr); wait_any_key();
        return;
    }
    int saved_color = color_mode, saved_tui = ui_tui;
    color_mode = ui_color_on();              /* colors follow the screen (stderr), not the capture */
    ui_tui = 0;
    dup2(fileno(tmp), STDOUT_FILENO); dup2(fileno(tmp), STDERR_FILENO);
    int sev = smart_report(fd);
    fflush(stdout); fflush(stderr);
    dup2(s1, STDOUT_FILENO); dup2(s2, STDERR_FILENO); close(s1); close(s2);
    ui_tui = saved_tui; color_mode = saved_color;
    size_t len = 0;
    char *buf = NULL;
    long sz;
    if (fseek(tmp, 0, SEEK_END) == 0 && (sz = ftell(tmp)) >= 0 && fseek(tmp, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)sz + 1);
        if (buf) { len = fread(buf, 1, (size_t)sz, tmp); buf[len] = '\0'; }
    }
    fclose(tmp);
    int code = smart_exit_code(sev);
    const char *verdict = code == 0 ? "HEALTHY" : code == 3 ? "WARNING" : code == 4 ? "CRITICAL" : code == 5 ? "UNKNOWN" : "not available";
    char title[PATH_MAX + 64];
    snprintf(title, sizeof(title), "SMART · %s · verdict: %s", dev, verdict);
    pager_run(title, ui_col(code == 0 ? COL_GREEN : code == 4 || code == 1 ? COL_RED : COL_YELLOW), buf ? buf : "", len);
    free(buf);
}
/* Shown before "Type exactly YES": the disk that will be erased, as precisely as possible. */
static void print_target_disk(const char *dev, uint64_t size, const char *op) {
    const char *base = strrchr(dev, '/');
    base = base ? base + 1 : dev;
    char sum[1200], cap[80];
    fmt_capacity(cap, sizeof(cap), size);
    if (supported_disk_name(base)) {
        disk_info_t d;
        load_disk_info(base, &d);
        snprintf(sum, sizeof(sum), "%.255s · serial %.255s · %s", d.model, d.serial, cap);
    } else snprintf(sum, sizeof(sum), "%s", cap);
    fprintf(stderr, "\n%sDestructive operation: all data on this disk will be lost.%s\n", ui_col(COL_RED), ui_col(COL_RESET));
    fprintf(stderr, "  %s%s — %s → %s%s\n", ui_col(COL_RED), dev, sum, op, ui_col(COL_RESET));
}

static int make_log_dir(char *out, size_t n);
static void give_to_sudo_user(const char *path);
/* Single-disk menu: CSV log in a new diskmedic-YYYYMMDD-HHMMSS/ directory, like the multi-disk mode. */
static void tui_open_csv(const char *dev) {
    static char path[PATH_MAX + 300];
    char dir[PATH_MAX];
    log_entries = 0; log_failed = 0;
    if (make_log_dir(dir, sizeof(dir)) < 0) {
        fprintf(stderr, "Cannot create the log directory (%s): no CSV log for this operation.\n", strerror(errno));
        return;
    }
    const char *base = strrchr(dev, '/');
    snprintf(path, sizeof(path), "%s/%.255s.csv", dir, base ? base + 1 : dev);
    if (open_log(path) == 0) give_to_sudo_user(path);
}
static int tui_menu(int fd, uint64_t size, const char *dev) {
    struct termios saved, raw;
    int raw_enabled = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    if (!raw_enabled) { fprintf(stderr, "--tui mode requires an interactive terminal on stdin.\n"); return -1; }
    if (raw_enabled) { term_remember(&saved); raw = saved; raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0; tcsetattr(STDIN_FILENO,TCSANOW,&raw); }
    ui_tui = 1; ui_device = dev; ui_size = size; ui_fd = fd; ui_begin();
    /* Header card: IDENTIFY for ATA, otherwise an NVMe or SAS/SCSI card (silent probes). */
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
    int ret=0, menu_cursor=0;
    for (;;) {
        unsigned long sectors=0; if (ioctl(fd, BLKGETSIZE, &sectors) < 0) sectors=0;
        fputs("\033[H",stderr);
        print_tui_rule('T','T');
        char menu_title[256];
        snprintf(menu_title,sizeof(menu_title),"DiskMedic %s · MENU",DISKMEDIC_VERSION);
        print_tui_line(menu_title);
        print_tui_rule('M','M');
        char menu_row[PATH_MAX + 256];
        snprintf(menu_row,sizeof(menu_row),"Disk: %s",dev); print_tui_line(menu_row);
        if (have_id) {
            /* IDENTIFY: serial = words 10-19 (20 bytes), firmware = words 23-26 (8 bytes), model = words 27-46 (40 bytes). */
            char model[41], serial[DISK_INFO_LEN + 1], fw[9], vpd_serial[DISK_INFO_LEN];
            ata_ident_string(ident, 20, 20, serial, sizeof(serial));
            ata_ident_string(ident, 46, 8, fw, sizeof(fw));
            ata_ident_string(ident, 54, 40, model, sizeof(model));
            if (scsi_vpd_serial(fd, vpd_serial, sizeof(vpd_serial)) == 0)
                snprintf(serial, sizeof(serial), "%s", vpd_serial);
            snprintf(menu_row,sizeof(menu_row),"Model: %s",model); print_tui_line(menu_row);
            snprintf(menu_row,sizeof(menu_row),"Serial: %s",serial); print_tui_line(menu_row);
            snprintf(menu_row,sizeof(menu_row),"Firmware: %s",fw); print_tui_line(menu_row);
            uint64_t lba48=(uint64_t)word_le(ident,100)|((uint64_t)word_le(ident,101)<<16)|((uint64_t)word_le(ident,102)<<32)|((uint64_t)word_le(ident,103)<<48);
            snprintf(menu_row,sizeof(menu_row),"LBA48 sectors: %llu",(unsigned long long)lba48); print_tui_line(menu_row);
            uint16_t sec=word_le(ident,128);
            snprintf(menu_row,sizeof(menu_row),"ATA security: %s, enabled=%s, frozen=%s, locked=%s, enhanced erase=%s",
              sec&1?"supported":"unknown",sec&2?"yes":"no",sec&8?"yes":"no",sec&4?"yes":"no",sec&0x20?"yes":"no"); print_tui_line(menu_row);
        } else if (have_card) {
            for (int i = 0; i < card.n; i++) print_tui_line(card.line[i]);
        } else print_tui_line("Disk identity unavailable (USB bridge?): try 1 = SMART.");
        {
            const char *fmt = log_sector_size == 512 && phys_sector_size == 512 ? "512n"
                            : log_sector_size == 512 && phys_sector_size == 4096 ? "512e"
                            : log_sector_size == 4096 && phys_sector_size == 4096 ? "4Kn" : "other";
            char cap[80], nsec[32];
            fmt_capacity(cap, sizeof(cap), size);
            fmt_int(nsec, sizeof(nsec), (uint64_t)sectors);
            snprintf(menu_row,sizeof(menu_row),"Capacity: %s · %s sectors of 512 B · %s (%u/%u B)",
                     cap,nsec,fmt,log_sector_size,phys_sector_size);
            print_tui_line(menu_row);
            snprintf(menu_row,sizeof(menu_row),"Media: %s",media.why); print_tui_line(menu_row);
        }
        print_tui_rule('M','M');
        op_item_t items[7];
        memcpy(items, op_items_base, sizeof(items));
        if (!have_id && !is_nvme) items[5].unavailable = "SAS/SCSI disk or USB bridge";
        if (media.kind == MEDIA_SSD) {
            items[3].desc = "Not advised on an SSD: a read-only scan is offered instead";
            items[4].desc = "Not advised on an SSD: TRIM is offered instead";
        }
        if (items[menu_cursor].unavailable) menu_cursor = 0;
        op_menu_draw(items, menu_cursor, "↑/↓ move · Enter or 1-7 run · r refresh · q quit");
        print_tui_rule('B','B');
        fputs("\033[J",stderr); fflush(stderr);
        int c=read_key(); if(c=='q'||c=='Q'||c==EOF) break;
        if(c==KEY_RESIZE) continue;
        if(c=='r'||c=='R') { TUI_PROBE(); continue; }
        c=op_menu_key(items,&menu_cursor,c);
        if(c=='1') { smart_in_pager(fd, dev); continue; }
        int mutation=(c=='4'||c=='5'||c=='6'||c=='7');
        if(c!='2'&&c!='3'&&!mutation) continue;
        if((c=='4'||c=='5') && media.kind==MEDIA_SSD) {
            /* SSD: explain, then offer the suitable alternative or the operation anyway. */
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
            char answer[16]="";
            fputc('\n',stderr);
            if(c=='5') { ssd_explain_zero(&media,0); fputs("t = TRIM instead | z = write zeros anyway | Enter = cancel: ",stderr); }
            else { ssd_explain_repair(&media,0); fputs("s = slow scan only (read-only) | r = repair anyway | Enter = cancel: ",stderr); }
            fflush(stderr);
            if(!fgets(answer,sizeof(answer),stdin)) answer[0]=0;
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&raw);
            int a=tolower((unsigned char)answer[0]);
            if(c=='5' && a=='t') c='7';
            else if(c=='4' && a=='s') c='3';
            else if(!((c=='5' && a=='z') || (c=='4' && a=='r'))) {
                fprintf(stderr,"Cancelled: nothing was modified. Press any key…"); fflush(stderr); wait_any_key(); continue;
            }
            mutation=c!='3';
        }
        if(mutation) {
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
            int claim=-1;
            if(c=='6' && !have_id && !is_nvme) {
                /* Warn before any confirmation: nothing will be attempted. */
                fprintf(stderr,"Secure Erase is not available for this disk (SAS/SCSI or USB bridge): nothing was modified.\n");
                ret=-1;
            }
            else if(c=='6' && is_nvme && nvme_erase_preview(fd)<0) ret=-1;
            else if((claim=claim_device(dev))<0) ret=-1;
            else if((print_target_disk(dev,size,op_label(c-'0')),!confirm_mutation())) { fprintf(stderr,"Confirmation declined.\n"); }
            else if(c=='4') { tui_open_csv(dev); ret=slow_scan(fd,size,4.0,1); close_log(); if(log_failed) ret=-1; }
            else if(c=='5') ret=zero_disk(fd,size);
            else if(c=='6') ret=secure_erase_any(fd,size);
            else if(c=='7') ret=trim_disk(fd,size);
            if(claim>=0) close(claim);
            if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&raw);
        } else {
            tui_open_csv(dev);
            ret = c=='2' ? verify_disk(fd,size) : slow_scan(fd,size,4.0,0);
            close_log(); if(log_failed) ret=-1;
        }
        fflush(stdout);
        fprintf(stderr,"\n%sOperation finished: %s (code %d)%s. Press any key…",
                ui_col(ret==0?COL_GREEN:COL_RED), ret==0?"OK":"FAILED", ret, ui_col(COL_RESET));
        fflush(stderr); wait_any_key(); ret=0;
    }
    if(raw_enabled) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
    ui_end(); return ret;
#undef TUI_PROBE
}

/* ---------- Parallel multi-disk execution and full-screen dashboard ---------- */
enum { JOB_RUNNING, JOB_DONE };
enum { JR_OK, JR_FAIL, JR_KILLED, JR_NOT_STARTED, JR_WARN, JR_CRIT, JR_UNKNOWN };
/* Finished jobs whose operation completed but whose SMART verdict is not healthy. */
static int jr_alert(int status) { return status == JR_WARN || status == JR_CRIT || status == JR_UNKNOWN; }
typedef struct {
    size_t disk;                        /* index in tui_disks */
    char dev[PATH_MAX + 128];
    char log[PATH_MAX + 300], csv[PATH_MAX + 300];
    pid_t pid;
    int state, status;
    int op;                             /* operation for this disk (TRIM or scan only on SSD) */
    struct timespec start, end;
} job_t;
static const char *op_label(int op) {
    return op == 1 ? "SMART" : op == 2 ? "Verification" : op == 3 ? "Slow scan" : op == 4 ? "Slow-block repair" :
           op == 5 ? "Zero-fill" : op == 6 ? "Secure Erase" : op == 7 ? "TRIM" : "?";
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
    for (const unsigned char *s = (const unsigned char *)text; *s;) {
        size_t a = ansi_seq_len(s);
        if (a) { s += a; continue; }
        s += utf8_char_bytes(s);
        w++;
    }
    return w;
}
/* Framed line: text on the left, optionally colored text aligned on the right. */
static void print_tui_line_lr(const char *left, const char *right, const char *color) {
    unsigned cols = terminal_columns(), inner = cols > 2 ? cols - 2 : 1;
    unsigned rw = right && *right ? utf8_width(right) : 0;
    if (rw + 1 >= inner) rw = 0;
    const glyphs_t *g = ui_glyphs();
    int on = color && *color;
    fputs(g->v, UI_OUT);
    print_padded(left, rw ? inner - rw - 1 : inner);
    if (rw) {
        fputc(' ', UI_OUT);
        if (on) fputs(color, UI_OUT);
        print_padded(right, rw);
        if (on) fputs(COL_RESET, UI_OUT);
    }
    fprintf(UI_OUT, "%s\n", g->v);
}
/* Files created under sudo are handed back to the user who ran sudo, so they can delete them. */
static void give_to_sudo_user(const char *path) {
    const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
    if (!u || !g || geteuid() != 0) return;
    char *e1, *e2;
    long uid = strtol(u, &e1, 10), gid = strtol(g, &e2, 10);
    if (*e1 || *e2 || uid <= 0 || gid < 0) return;
    if (chown(path, (uid_t)uid, (gid_t)gid) < 0) { /* harmless */ }
}
/* Log directory: ./diskmedic-YYYYMMDD-HHMMSS, or in /tmp if the current directory is read-only. */
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
/* Last non-empty line of a log (shown under the progress bar). */
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
/* Prints a log, limited to its last max_lines lines. */
static void print_log_excerpt(const char *path, unsigned max_lines) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        printf("  (log unreadable: %s)\n", strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    const off_t cap = 256 * 1024;
    off_t off = st.st_size > cap ? st.st_size - cap : 0;
    size_t len = (size_t)(st.st_size - off);
    char *b = malloc(len + 1);
    if (!b || (len && full_pread(fd, b, len, off) < 0)) { free(b); close(fd); printf("  (log unreadable)\n"); return; }
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
    if (from > start) printf("  … beginning omitted — full log: %s\n", path);
    fwrite(from, 1, (size_t)(end - from), stdout);
    printf("\n");
    free(b);
}
/* Progress line: bar, then text (percentage, rate…). */
static void dash_bar_line(int has_prog, int running, double pct, unsigned tick, const char *tail, const char *color) {
    unsigned cols = terminal_columns(), inner = cols > 2 ? cols - 2 : 1;
    unsigned tw = utf8_width(tail);
    int bw = (int)inner - 2 - 1 - (int)tw - 1;
    if (bw > 60) bw = 60;
    const glyphs_t *g = ui_glyphs();
    size_t lf = strlen(g->full), le = strlen(g->empty);
    char *row = malloc((size_t)inner * (lf > le ? lf : le) + strlen(tail) + 16);
    if (!row) return;
    size_t u = 0;
    row[u++] = ' '; row[u++] = ' ';
    if (bw >= 10 && (has_prog || running)) {
        int filled = has_prog ? (int)(pct / 100.0 * bw + 0.5) : 0;
        /* Without a progress measure (SMART, Secure Erase): a cursor moves back and forth. */
        int pos = (int)(tick % (unsigned)(2 * (bw - 3)));
        if (pos > bw - 3) pos = 2 * (bw - 3) - pos;
        for (int i = 0; i < bw; i++) {
            int on = has_prog ? i < filled : (i >= pos && i < pos + 3);
            if (on) { memcpy(row + u, g->full, lf); u += lf; }
            else { memcpy(row + u, g->empty, le); u += le; }
        }
        row[u++] = ' ';
    }
    snprintf(row + u, strlen(tail) + 2, "%s", tail);
    print_tui_line_attr(row, color);
    free(row);
}
static const char *job_label(const job_t *j, const char **color) {
    if (j->status == JR_NOT_STARTED) { *color = ui_col(COL_RED); return "NOT STARTED"; }
    if (j->state == JOB_RUNNING) { *color = ui_col(COL_CYAN); return "RUNNING"; }
    if (j->status == JR_OK) { *color = ui_col(COL_GREEN); return "OK"; }
    if (j->status == JR_KILLED) { *color = ui_col(COL_YELLOW); return "INTERRUPTED"; }
    if (j->status == JR_WARN) { *color = ui_col(COL_YELLOW); return "WARNING"; }
    if (j->status == JR_CRIT) { *color = ui_col(COL_RED); return "CRITICAL"; }
    if (j->status == JR_UNKNOWN) { *color = ui_col(COL_YELLOW); return "UNKNOWN"; }
    *color = ui_col(COL_RED); return "FAILED";
}
static void dash_draw(const job_t *jobs, size_t njobs, const worker_slot_t *slots, const char *op_name,
                      const char *logdir, const struct timespec *t0, const char *footer, unsigned tick, size_t sel) {
    char *frame = NULL;
    size_t flen = 0;
    FILE *mf = open_memstream(&frame, &flen);
    if (!mf) return;
    ui_out = mf;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned rows = terminal_rows();
    char left[PATH_MAX + 512], right[160], el[32];
    size_t running = 0, ok = 0, bad = 0, alerts = 0;
    double eta_all = -1.0;                  /* time left for the slowest disk with a measure */
    for (size_t k = 0; k < njobs; k++) {
        if (jobs[k].state == JOB_RUNNING) {
            worker_slot_t w;
            worker_snapshot(&slots[k], &w);
            if (w.total && w.done && w.eta > eta_all) eta_all = w.eta;
        }
        if (jobs[k].state == JOB_RUNNING) running++;
        else if (jobs[k].status == JR_OK) ok++;
        else if (jr_alert(jobs[k].status)) alerts++;
        else bad++;
    }
    fputs("\033[H", mf);
    print_tui_rule('T', 'T');
    snprintf(left, sizeof(left), "DiskMedic %s · %s · %zu disk(s) in parallel", DISKMEDIC_VERSION, op_name, njobs);
    format_eta(elapsed_seconds(t0, &now), el, sizeof(el));
    print_tui_line_lr(left, el, NULL);
    print_tui_rule('M', 'M');
    /* Layout: 4 lines per disk if the screen allows it, otherwise 2; one line stays free at the bottom. */
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
        if (!run && (j->status == JR_OK || jr_alert(j->status))) { pct = 100.0; }
        char pcts[16], sz[32], inst[32], avg[32], eta[32], tail[256], badtxt[64];
        fmt_dec(pcts, sizeof(pcts), pct, 1);
        fmt_bytes(sz, sizeof(sz), (double)d->bytes);
        fmt_dec(inst, sizeof(inst), w.instant, 1);
        fmt_dec(avg, sizeof(avg), w.average, 1);
        format_eta(w.eta, eta, sizeof(eta));
        badtxt[0] = '\0';
        if (w.bad) snprintf(badtxt, sizeof(badtxt), "unreadable: %" PRIu64, w.bad);
        if (full && k) print_tui_rule('M', 'M');
        snprintf(left, sizeof(left), "%s%c %s · %.255s · %s%s", k == sel ? COL_REV : "", k == sel ? '>' : ' ',
                 j->dev, d->model, sz, k == sel ? COL_RESET : "");
        if (full || !has_prog) snprintf(right, sizeof(right), "%s · %s", label, el);
        else snprintf(right, sizeof(right), "%s %s %% · %s", label, pcts, el);
        print_tui_line_lr(left, right, color);
        if (run && has_prog) snprintf(tail, sizeof(tail), "%s %% · %s MiB/s · left %s", pcts, inst, eta);
        else if (has_prog) snprintf(tail, sizeof(tail), "%s %% · average %s MiB/s", pcts, avg);
        else if (run) snprintf(tail, sizeof(tail), "%s", w.phase[0] ? w.phase : "Starting");
        else snprintf(tail, sizeof(tail), "done");
        if (!full && w.bad) {
            size_t tl = strlen(tail);
            snprintf(tail + tl, sizeof(tail) - tl, " · %s", badtxt);
        }
        if (full) {
            snprintf(left, sizeof(left), "  Step: %s", w.phase[0] ? w.phase : "Starting");
            print_tui_line_lr(left, badtxt, ui_col(COL_RED));
        }
        dash_bar_line(has_prog, run, pct, tick, tail, color);
        if (full) {
            char last[512];
            last_log_line(j->log, last, sizeof(last));
            double stale = elapsed_seconds(&w.updated, &now);
            right[0] = '\0';
            if (run && has_prog && w.updated.tv_sec && stale >= 10.0)
                snprintf(right, sizeof(right), "updated %.0f s ago", stale);
            snprintf(left, sizeof(left), "  › %s", last);
            print_tui_line_lr(left, right, ui_col("\033[33m"));
        }
    }
    if (shown < njobs) {
        snprintf(left, sizeof(left), "… and %zu more disk(s): enlarge the window to see them", njobs - shown);
        print_tui_line(left);
    }
    print_tui_rule('M', 'M');
    if (alerts)
        snprintf(left, sizeof(left), "Running: %zu · Finished: %zu (OK: %zu, SMART alerts: %zu, failed: %zu)",
                 running, ok + alerts + bad, ok, alerts, bad);
    else snprintf(left, sizeof(left), "Running: %zu · Finished: %zu (OK: %zu, failed: %zu)", running, ok + bad, ok, bad);
    right[0] = '\0';
    if (running && eta_all >= 0) {
        format_eta(eta_all, el, sizeof(el));
        snprintf(right, sizeof(right), "all done in ≈ %s", el);
    }
    print_tui_line_lr(left, right, NULL);
    snprintf(left, sizeof(left), "Logs: %s", logdir);
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
/* Operation on one disk (run in a child process, output redirected to its log). */
static int run_disk_operation(int operation, const char *dev, int mutating) {
    int fd = open(dev, (mutating ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) { perror(dev); return -1; }
    struct stat st;
    uint64_t size = 0;
    int one = -1;
    if (fstat(fd, &st) < 0) perror("fstat");
    else if (!S_ISBLK(st.st_mode)) fprintf(stderr, "Refused: %s is not a block device.\n", dev);
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

/* Multi-disk selection, then parallel execution (one process per disk). */
static int tui_multi(void) {
    size_t count = scan_disks(tui_disks), cursor = 0, selected_count = 0;
    unsigned char selected[MAX_DISKS] = {0};
    struct termios saved, raw;
    int ret = 0;
    if (!count) {
        fprintf(stderr, "No supported disk found in /sys/block.\n");
        return -1;
    }
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) < 0) {
        fprintf(stderr, "TUI mode requires an interactive terminal on stdin.\n");
        return -1;
    }
    term_remember(&saved);
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) { perror("tcsetattr"); return -1; }
    load_disk_status(tui_disks, count);
    int any_badge = 0;
    for (size_t i = 0; i < count; i++) any_badge |= tui_disks[i].in_use;
    ui_tui = 1;
    ui_begin();
    size_t first = 0, vis = 8;
    int operation = 0, op_cursor = 0;
select_disks:
    for (;;) {
        char row[PATH_MAX + 256], cap[80], attr[32];
        /* The list uses the whole height: 10 fixed lines (+1 for the legend). */
        unsigned rows = terminal_rows(), fixed = 10U + (unsigned)any_badge;
        vis = rows > fixed + 1 ? rows - fixed : 1;
        if (cursor < first) first = cursor;
        if (cursor >= first + vis) first = cursor - vis + 1;
        if (first + vis > count) first = count > vis ? count - vis : 0;
        size_t last = first + vis < count ? first + vis : count;
        unsigned cols = terminal_columns(), inner = cols > 2 ? cols - 2 : 1;
        int mw = (int)inner - 55;
        if (mw < 10) mw = 10;
        fputs("\033[H", stderr);
        print_tui_rule('T', 'T');
        snprintf(row, sizeof(row), "DiskMedic %s · MULTIPLE SELECTION", DISKMEDIC_VERSION);
        print_tui_line(row);
        print_tui_rule('M', 'M');
        print_tui_line("↑/↓ move | Space select | a select all | Enter start | q quit");
        if (first) snprintf(row, sizeof(row), "  ↑ %zu more above", first); else row[0] = '\0';
        print_tui_line_attr(row, ui_col(COL_DIM));
        for (size_t i = first; i < last; i++) {
            const disk_info_t *d = &tui_disks[i];
            fmt_capacity(cap, sizeof(cap), d->bytes);
            snprintf(row, sizeof(row), "%c [%c] /dev/%-8s %-*.*s %-22s %-4s %s",
                     i == cursor ? '>' : ' ', selected[i] ? 'x' : ' ', d->name,
                     mw, mw, d->model, cap, d->kind, disk_badge(d));
            snprintf(attr, sizeof(attr), "%s%s", i == cursor ? COL_REV : "",
                     d->system_disk ? ui_col(COL_RED) : d->in_use ? ui_col(COL_YELLOW) : "");
            print_tui_line_attr(row, attr);
        }
        if (last < count) snprintf(row, sizeof(row), "  ↓ %zu more below", count - last); else row[0] = '\0';
        print_tui_line_attr(row, ui_col(COL_DIM));
        print_tui_rule('M', 'M');
        snprintf(row, sizeof(row), "Selected: %zu / %zu", selected_count, count);
        print_tui_line(row);
        if (any_badge)
            print_tui_line_attr("IN USE = mounted or held (swap, LVM, RAID…): read-only operations only · "
                                "SYSTEM = holds the root filesystem", ui_col(COL_DIM));
        print_tui_rule('B', 'B');
        fputs("\033[J", stderr);
        fflush(stderr);
        int c = read_key();
        if (c == 'q' || c == 'Q' || c == EOF) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved); ui_end(); return 0;
        }
        if (c == KEY_UP || c == 'k') { if (cursor > 0) cursor--; }
        else if (c == KEY_DOWN || c == 'j') { if (cursor + 1 < count) cursor++; }
        else if (c == KEY_HOME) cursor = 0;
        else if (c == KEY_END) cursor = count - 1;
        else if (c == KEY_PGUP) cursor = cursor > vis ? cursor - vis : 0;
        else if (c == KEY_PGDN) cursor = cursor + vis < count ? cursor + vis : count - 1;
        else if (c == ' ' || c == 'x' || c == 'X') {
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

    for (;;) {
        fputs("\033[H", stderr);
        print_tui_rule('T', 'T');
        print_tui_line("CHOOSE THE OPERATION");
        print_tui_rule('M', 'M');
        /* The selected disks stay in view while the operation is chosen. */
        unsigned rows = terminal_rows(), fixed = 3 + 1 + OP_MENU_LINES + 1 + 1;
        print_selected_block(selected, count, selected_count, rows > fixed + 2 ? rows - fixed - 1 : 1);
        print_tui_rule('M', 'M');
        op_menu_draw(op_items_base, op_cursor, "↑/↓ move · Enter or 1-7 run · q back to the selection");
        print_tui_rule('B', 'B'); fputs("\033[J", stderr); fflush(stderr);
        int c = read_key();
        if (c == EOF) { tcsetattr(STDIN_FILENO, TCSANOW, &saved); ui_end(); return 0; }
        if (c == 'q' || c == 'Q' || c == KEY_ESC) goto select_disks;
        c = op_menu_key(op_items_base, &op_cursor, c);
        if (c >= '1' && c <= '7') { operation = c - '0'; break; }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    ui_end();
    ui_tui = 0;

    /* Operation per disk: on SSDs, TRIM instead of zeros, scan only instead of repair. */
    int disk_op[MAX_DISKS];
    for (size_t i = 0; i < MAX_DISKS; i++) disk_op[i] = operation;
    /* Destructive choice: the selected disks are listed again on the normal screen. */
    if (op_mutates(operation)) {
        fprintf(stderr, "\nSelected disks (%zu):\n", selected_count);
        for (size_t i = 0; i < count; i++) {
            if (!selected[i]) continue;
            char sum[1200];
            disk_summary(&tui_disks[i], sum, sizeof(sum));
            fprintf(stderr, "  /dev/%s — %s\n", tui_disks[i].name, sum);
        }
    }
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
            if (!nssd) fprintf(stderr, "\nSSDs in the selection:\n");
            fprintf(stderr, "  /dev/%s — %s (%s)\n", tui_disks[i].name, tui_disks[i].model, m.why);
            is_ssd[i] = 1; nssd++;
        }
        if (nssd) {
            const media_t generic = {MEDIA_SSD, "SSD"};
            if (operation == 5) {
                ssd_explain_zero(&generic, 0);
                fputs("t = TRIM on these SSDs (zeros on the others) | z = zeros everywhere | Enter = cancel: ", stderr);
            } else {
                ssd_explain_repair(&generic, 0);
                fputs("s = slow scan only on these SSDs (repair on the others) | r = repair everywhere | Enter = cancel: ", stderr);
            }
            fflush(stderr);
            char answer[16] = "";
            if (!fgets(answer, sizeof(answer), stdin)) answer[0] = 0;
            int a = tolower((unsigned char)answer[0]);
            if ((operation == 5 && a == 't') || (operation == 4 && a == 's')) {
                for (size_t i = 0; i < count; i++) if (is_ssd[i]) disk_op[i] = operation == 5 ? 7 : 3;
            } else if (!((operation == 5 && a == 'z') || (operation == 4 && a == 'r'))) {
                fprintf(stderr, "No operation started.\n");
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
        /* Refuse the whole set if a single disk is mounted/in use, before any confirmation. */
        int busy = 0;
        for (size_t i = 0; i < count; i++) {
            if (!selected[i] || !op_mutates(disk_op[i])) continue;
            char dev[PATH_MAX + 128];
            snprintf(dev, sizeof(dev), "/dev/%.255s", tui_disks[i].name);
            claims[i] = claim_device(dev);
            if (claims[i] < 0) busy = 1;
        }
        if (!busy) {
            fprintf(stderr, "\n%sDestructive operation: all data on the disks below will be lost.%s\n",
                    ui_col(COL_RED), ui_col(COL_RESET));
            fprintf(stderr, "%s on %zu disk(s) at the same time:\n", op_name, selected_count);
            for (size_t i = 0; i < count; i++) {
                if (!selected[i]) continue;
                char sum[1200];
                disk_summary(&tui_disks[i], sum, sizeof(sum));
                fprintf(stderr, "  %s/dev/%s — %s → %s%s\n", ui_col(op_mutates(disk_op[i]) ? COL_RED : ""),
                        tui_disks[i].name, sum, op_label(disk_op[i]), ui_col(COL_RESET));
            }
        }
        if (busy || !confirm_mutation()) {
            if (!busy) fprintf(stderr, "Confirmation declined; no operation started.\n");
            else fprintf(stderr, "No operation started.\n");
            for (size_t i = 0; i < count; i++) if (claims[i] >= 0) close(claims[i]);
            return 1;
        }
    }

    /* One task per disk; progress is published in a shared memory area. */
    char logdir[PATH_MAX];
    job_t *jobs = calloc(selected_count, sizeof(*jobs));
    worker_slot_t *slots = mmap(NULL, selected_count * sizeof(*slots), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!jobs || slots == MAP_FAILED || make_log_dir(logdir, sizeof(logdir)) < 0) {
        fprintf(stderr, "Cannot prepare (memory or log directory): %s\n", strerror(errno));
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
            /* Child process: output to the disk's log, progress to the shared memory. */
            worker_slot = &slots[k];
            term_forget();
            ui_tui = 0; ui_out = NULL; color_mode = 0; ui_color_mode = 0;
            int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
            if (!freopen(j->log, "w", stdout)) _exit(1);
            setvbuf(stdout, NULL, _IOLBF, 0);
            dup2(STDOUT_FILENO, STDERR_FILENO);
            give_to_sudo_user(j->log);
            const disk_info_t *d = &tui_disks[j->disk];
            printf("diskmedic %s — %s — %s (%s, serial number %s)\n", DISKMEDIC_VERSION, op_label(j->op), j->dev, d->model, d->serial);
            worker_publish("Starting", 0, 0, 0, 0, 0);
            int one = 0;
            if (j->csv[0]) {
                if (open_log(j->csv) < 0) one = -1;
                else give_to_sudo_user(j->csv);
            }
            if (!one) one = run_disk_operation(j->op, j->dev, op_mutates(j->op));
            close_log();
            if (log_failed) one = -1;
            int code = j->op == 1 ? smart_exit_code(one) : one == 0 ? 0 : 1;
            printf("\nResult: %s\n", code == 0 ? "OK" : code == 3 ? "WARNING" : code == 4 ? "CRITICAL" :
                                     code == 5 ? "UNKNOWN" : "FAILED");
            fflush(stdout);
            _exit(code);
        }
        if (pid < 0) {
            fprintf(stderr, "fork: %s\n", strerror(errno));
            j->state = JOB_DONE; j->status = JR_NOT_STARTED; j->end = j->start;
        }
        j->pid = pid;
    }

    /*
     * Full-screen dashboard. The keyboard is read without signals (Ctrl+C arrives as a
     * key): an interruption is always confirmed before stopping the operations.
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
    int view = -1, vfollow = 1;             /* view >= 0: log of jobs[view] shown full screen */
    size_t dsel = 0, vtop = 0;
    for (;;) {
        int st;
        pid_t p;
        while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
            for (size_t k = 0; k < njobs; k++) {
                if (jobs[k].pid != p || jobs[k].state != JOB_RUNNING) continue;
                jobs[k].state = JOB_DONE;
                clock_gettime(CLOCK_MONOTONIC, &jobs[k].end);
                int ec = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
                jobs[k].status = ec < 0 ? JR_KILLED : ec == 0 ? JR_OK : ec == 3 ? JR_WARN :
                                 ec == 4 ? JR_CRIT : ec == 5 ? JR_UNKNOWN : JR_FAIL;
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
            if (operation == 6) notice = "Secure Erase cannot be interrupted: the disk runs the command itself.";
            else for (size_t k = 0; k < njobs; k++) if (jobs[k].state == JOB_RUNNING) kill(jobs[k].pid, SIGTERM);
        }
        /* The final screen and the interruption question are always shown on the dashboard. */
        if (!running || asking) view = -1;
        const char *footer = !running ? "Done — press any key to show the reports and the summary"
                           : asking ? "Interrupt all running operations? y = yes, any other key = no"
                           : notice ? notice : "↑/↓ choose a disk · Enter view its log · q interrupt all operations";
        if (draw && view >= 0) {
            /* Live log of one disk; the operations go on meanwhile. */
            const job_t *j = &jobs[view];
            const char *c;
            const char *label = job_label(j, &c);
            char title[PATH_MAX + 400];
            size_t len = 0;
            char *buf = read_file_tail(j->log, 256 * 1024, &len);
            snprintf(title, sizeof(title), "Log of %s · %s · %s", j->dev, op_label(j->op), label);
            pager_draw(title, c, buf ? buf : "", len, &vtop, &vfollow,
                       "↑/↓ PgUp/PgDn Home/End scroll · End follows the log · q, Esc or Enter: back to the dashboard");
            free(buf);
            tick++;
        } else if (draw) dash_draw(jobs, njobs, slots, op_name, logdir, &t0, footer, tick++, dsel);
        if (!running) break;
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        int pr = poll(&pfd, stdin_dead ? 0 : 1, 250);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int ch = read_key();
            if (ch == EOF) stdin_dead = 1;
            else if (ch == KEY_RESIZE) {}
            else if (asking) {
                asking = 0;
                if (ch == 'y' || ch == 'Y') {
                    if (operation == 6) notice = "Secure Erase cannot be interrupted: the disk runs the command itself.";
                    else {
                        for (size_t k = 0; k < njobs; k++) if (jobs[k].state == JOB_RUNNING) kill(jobs[k].pid, SIGTERM);
                        notice = "Interruption requested…";
                    }
                }
            } else if (ch == 3) asking = 1;
            else if (view >= 0) {
                if (ch == 'q' || ch == 'Q' || ch == KEY_ESC || ch == '\r' || ch == '\n') view = -1;
                else pager_key(ch, &vtop, &vfollow);
            } else if (ch == 'q' || ch == 'Q') asking = 1;
            else if (ch == KEY_UP || ch == 'k') { if (dsel) dsel--; }
            else if (ch == KEY_DOWN || ch == 'j') { if (dsel + 1 < njobs) dsel++; }
            else if ((ch == '\r' || ch == '\n') && jobs[dsel].status != JR_NOT_STARTED) {
                view = (int)dsel; vtop = 0; vfollow = 1;
            }
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

    /* Summary first, then the report of each disk (logs), then a one-line recap. */
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    char total_el[32];
    format_eta(elapsed_seconds(&t0, &t1), total_el, sizeof(total_el));
    fprintf(stderr, "\n=== Summary: %s · %zu disk(s) in parallel · %s ===\n", op_name, njobs, total_el);
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
            snprintf(extra, sizeof(extra), " · stopped at %s %% (%.60s)", pcts, w.phase);
        } else if (w.bad) snprintf(extra, sizeof(extra), " · %" PRIu64 " unreadable sector(s)", w.bad);
        fprintf(stderr, "  [%s]%*s %s — %s (%s)%s%s%s\n", label, (int)(12 - utf8_width(label)), "",
                jobs[k].dev, tui_disks[jobs[k].disk].model, el, extra,
                jobs[k].op != operation ? " · " : "", jobs[k].op != operation ? op_label(jobs[k].op) : "");
        if (jobs[k].status != JR_OK) ret = 1;
    }
    fprintf(stderr, "Logs: %s\n", logdir);
    fflush(stderr);
    for (size_t k = 0; k < njobs; k++) {
        const char *c;
        char el[32];
        format_eta(elapsed_seconds(&jobs[k].start, &jobs[k].end), el, sizeof(el));
        printf("\n━━ %s · %s · %s in %s ━━\n", jobs[k].dev, tui_disks[jobs[k].disk].model, job_label(&jobs[k], &c), el);
        if (jobs[k].status != JR_NOT_STARTED) print_log_excerpt(jobs[k].log, 150);
    }
    fflush(stdout);
    size_t n_ok = 0, n_alert = 0, n_bad = 0;
    for (size_t k = 0; k < njobs; k++) {
        if (jobs[k].status == JR_OK) n_ok++;
        else if (jr_alert(jobs[k].status)) n_alert++;
        else n_bad++;
    }
    fprintf(stderr, "\n%sDone: %zu OK, %zu SMART alert(s), %zu failed or interrupted · %s · logs: %s%s\n",
            ui_col(n_bad ? COL_RED : n_alert ? COL_YELLOW : COL_GREEN), n_ok, n_alert, n_bad, op_name, logdir, ui_col(COL_RESET));
    munmap(slots, selected_count * sizeof(*slots));
    free(jobs);
    return ret;
}

int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--version")) { printf("DiskMedic %s\n",DISKMEDIC_VERSION); return 0; }
    if (argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) { usage(stdout); return 0; }
    options_t o; if(parse_options(argc,argv,&o)<0) { usage(stderr); return 2; }
    opt_no_color = o.no_color;
    if (o.tui && !isatty(STDIN_FILENO)) {
        fprintf(stderr, "TUI mode requires an interactive terminal on stdin.\n\n");
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
    if(!S_ISBLK(st.st_mode)) { fprintf(stderr,"Refused: %s is not a block device.\n",o.device);close(fd);return 1; }
    uint64_t size; if(get_size(fd,&size)<0){close(fd);return 1;}
    load_sector_sizes(fd);
    ui_device=o.device; ui_size=size; ui_fd=fd; ui_tui=o.tui;
    if(o.tui) { int rc=tui_menu(fd,size,o.device); close(fd); return rc<0?1:rc; }
    /* SSD: no zero-fill and no rewrite of slow zones without --force. */
    if(o.zero || o.slow_scan) {
        media_t media=media_detect(fd);
        if(media.kind==MEDIA_SSD) {
            if(o.zero && !o.force) { ssd_explain_zero(&media,1); close(fd); return 1; }
            if(o.slow_repair && !o.force) { ssd_explain_repair(&media,1); close(fd); return 1; }
            if(o.force) fprintf(stderr,"--force: running the operation despite the media type (%s).\n",media.why);
            else fprintf(stderr,"Note (%s): on an SSD, a slow read points to the controller or the flash, "
                                "not to a damaged surface; check --smart as well.\n",media.why);
        }
    }
    if(o.secure_erase && ioctl(fd,NVME_IOCTL_ID)>0 && nvme_erase_preview(fd)<0) { close(fd); return 1; }
    /* Disk validated and claimed exclusively before asking for confirmation. */
    int claim=-1;
    if(mutating) {
        claim=claim_device(o.device);
        if(claim<0) { close(fd); return 1; }
        print_target_disk(o.device,size,o.secure_erase?op_label(6):o.zero?op_label(5):o.trim?op_label(7):op_label(4));
        if(!confirm_mutation()) {
            fprintf(stderr,"Confirmation missing or incorrect; no operation performed.\n");
            close(claim); close(fd); return 2;
        }
    }
    if(o.log_path && open_log(o.log_path)<0) { if(claim>=0) close(claim); close(fd); return 1; }
    int rc=0;
    if(o.smart_only) rc=smart_exit_code(smart_report(fd));
    if(!rc && o.secure_erase) rc=secure_erase_any(fd,size)<0?1:0;
    if(!rc && o.zero) rc=zero_disk(fd,size)<0?1:0;
    if(!rc && o.trim) rc=trim_disk(fd,size)<0?1:0;
    if(!rc && o.verify) rc=verify_disk(fd,size)<0?1:0;
    if(!rc && o.slow_scan) rc=slow_scan(fd,size,o.slow_factor,o.slow_repair)<0?1:0;
    if(claim>=0) close(claim);
    close(fd); close_log(); if(log_failed) rc=1; return rc;
}
