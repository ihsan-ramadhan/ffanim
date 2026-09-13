#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef DATADIR
#define DATADIR "/usr/local/share"
#endif

#define GAP 3
#define SPACER 1
#define SPAN 4.0
#define DIM 0.42
#define INK 0.42

static int prompt_lead = 2;
static char cfg_path[4096];
static char logo_path[4096];

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("ffanim: out of memory");
    return p;
}

typedef struct {
    char *s;
    size_t len, cap;
} buf;

static void buf_need(buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    while (b->cap < b->len + extra + 1) b->cap = b->cap ? b->cap * 2 : 4096;
    b->s = realloc(b->s, b->cap);
    if (!b->s) die("ffanim: out of memory");
}

static void buf_add(buf *b, const char *s, size_t n) {
    buf_need(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void buf_str(buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_fmt(buf *b, const char *fmt, ...) {
    va_list ap;
    char tmp[256];
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n);
}

static void buf_pad(buf *b, int n) {
    while (n-- > 0) buf_add(b, " ", 1);
}

static int vis_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p == 0x1b) {
            p++;
            if (*p == '[') {
                p++;
                while (*p && (*p < 0x40 || *p > 0x7e)) p++;
                if (*p) p++;
            } else if (*p) {
                p++;
            }
            continue;
        }
        if ((*p & 0xc0) != 0x80) w++;
        p++;
    }
    return w;
}

static char **lines;
static int nlines;
static char **info;
static int ninfo;
static int width, rows, need_w;

static char **split(char *text, int *count) {
    int n = 1;
    for (char *p = text; *p; p++)
        if (*p == '\n') n++;
    char **out = xmalloc((size_t)n * sizeof *out);
    int i = 0;
    out[i++] = text;
    for (char *p = text; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            if (*(p + 1)) out[i++] = p + 1;
        }
    }
    *count = i;
    return out;
}

static char *read_fd(int fd) {
    buf b = {0};
    char chunk[8192];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) buf_add(&b, chunk, (size_t)n);
    if (!b.s) buf_str(&b, "");
    while (b.len && b.s[b.len - 1] == '\n') b.s[--b.len] = '\0';
    return b.s;
}

static char *read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("ffanim: cannot read %s: %s", path, strerror(errno));
    char *s = read_fd(fd);
    close(fd);
    return s;
}

static char *run_fastfetch(void) {
    int pipefd[2];
    if (pipe(pipefd) < 0) die("ffanim: pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) die("ffanim: fork: %s", strerror(errno));
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (cfg_path[0])
            execlp("fastfetch", "fastfetch", "--logo", "none", "--pipe", "false",
                   "-c", cfg_path, (char *)NULL);
        execlp("fastfetch", "fastfetch", "--logo", "none", "--pipe", "false",
               (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char *out = read_fd(pipefd[0]);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        die("ffanim: fastfetch not found in PATH");
    return out;
}

static const int DOT_DX[8] = {0, 0, 0, 1, 1, 1, 0, 1};
static const int DOT_DY[8] = {0, 1, 2, 0, 1, 2, 3, 3};

static unsigned utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    unsigned cp;
    if (*s < 0x80) {
        cp = *s;
        *p = s + 1;
    } else if ((*s & 0xe0) == 0xc0 && s[1]) {
        cp = ((unsigned)(*s & 0x1f) << 6) | (s[1] & 0x3fu);
        *p = s + 2;
    } else if ((*s & 0xf0) == 0xe0 && s[1] && s[2]) {
        cp = ((unsigned)(*s & 0x0f) << 12) | ((s[1] & 0x3fu) << 6) | (s[2] & 0x3fu);
        *p = s + 3;
    } else if ((*s & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) {
        cp = ((unsigned)(*s & 0x07) << 18) | ((s[1] & 0x3fu) << 12) |
             ((s[2] & 0x3fu) << 6) | (s[3] & 0x3fu);
        *p = s + 4;
    } else {
        cp = *s;
        *p = s + 1;
    }
    return cp;
}

static int line_blank(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p == 0x1b) {
            p++;
            if (*p == '[') {
                p++;
                while (*p && (*p < 0x40 || *p > 0x7e)) p++;
                if (*p) p++;
            } else if (*p) {
                p++;
            }
            continue;
        }
        if (*p != ' ' && *p != '\t') return 0;
        p++;
    }
    return 1;
}

static void recompute(void) {
    width = 0;
    for (int i = 0; i < nlines; i++) {
        int w = vis_width(lines[i]);
        if (w > width) width = w;
    }
    rows = nlines + 1 > ninfo ? nlines + 1 : ninfo;
    int widest = 0;
    for (int i = 0; i < ninfo; i++) {
        int w = vis_width(info[i]);
        if (w > widest) widest = w;
    }
    need_w = width + GAP + widest;
}

static char **orig_lines;
static int orig_nlines;
static char **orig_info;
static int orig_ninfo;

static void reset_to_original(void) {
    if (lines != orig_lines) {
        for (int i = 0; i < nlines; i++) free(lines[i]);
        free(lines);
    }
    lines = orig_lines;
    nlines = orig_nlines;
    info = orig_info;
    ninfo = orig_ninfo;
    recompute();
}

static int logo_is_braille(void) {
    for (int i = 0; i < nlines; i++) {
        const unsigned char *p = (const unsigned char *)lines[i];
        while (*p) {
            unsigned cp = utf8_next(&p);
            if (cp != ' ' && (cp < 0x2800 || cp > 0x28ff)) return 0;
        }
    }
    return 1;
}

static void scale_logo(int cx, int cy) {
    int sw = width * 2, sh = nlines * 4;
    int dw = cx * 2, dh = cy * 4;
    unsigned char *src = calloc((size_t)sw * (size_t)sh, 1);
    if (!src) return;

    for (int r = 0; r < nlines; r++) {
        const unsigned char *p = (const unsigned char *)lines[r];
        int c = 0;
        while (*p && c < width) {
            unsigned cp = utf8_next(&p);
            if (cp >= 0x2800 && cp <= 0x28ff) {
                unsigned v = cp - 0x2800;
                for (int b = 0; b < 8; b++)
                    if (v >> b & 1)
                        src[(size_t)(r * 4 + DOT_DY[b]) * sw + c * 2 + DOT_DX[b]] = 1;
            }
            c++;
        }
    }

    char **out = xmalloc((size_t)cy * sizeof *out);
    for (int y = 0; y < cy; y++) {
        buf b = {0};
        for (int x = 0; x < cx; x++) {
            unsigned v = 0;
            for (int d = 0; d < 8; d++) {
                int dx = x * 2 + DOT_DX[d], dy = y * 4 + DOT_DY[d];
                int x0 = dx * sw / dw, x1 = (dx + 1) * sw / dw;
                int y0 = dy * sh / dh, y1 = (dy + 1) * sh / dh;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 > sw) x1 = sw;
                if (y1 > sh) y1 = sh;
                int on = 0, tot = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        on += src[(size_t)yy * sw + xx];
                        tot++;
                    }
                if (tot && (double)on / tot >= INK) v |= 1u << d;
            }
            unsigned cp = 0x2800u + v;
            char u[3];
            u[0] = (char)0xe2;
            u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
            u[2] = (char)(0x80 | (cp & 0x3f));
            buf_add(&b, u, 3);
        }
        if (!b.s) buf_str(&b, "");
        out[y] = b.s;
    }

    free(src);
    lines = out;
    nlines = cy;
    width = cx;
}

static void fit_block(int max_rows, int max_cols) {
    if (rows <= max_rows && need_w <= max_cols) return;

    int a = 0, z = ninfo;
    while (a < z && line_blank(info[a])) a++;
    while (z > a && line_blank(info[z - 1])) z--;
    if (a > 0 || z < ninfo) {
        info += a;
        ninfo = z - a;
        recompute();
    }
    if (rows <= max_rows && need_w <= max_cols) return;
    if (!logo_is_braille()) return;

    int info_w = need_w - width - GAP;
    int max_logo_cols = max_cols - GAP - info_w;
    int max_logo_rows = max_rows - 1;
    if (max_logo_cols < 8 || max_logo_rows < 3) return;

    double sx = (double)max_logo_cols / width;
    double sy = (double)max_logo_rows / nlines;
    double s = sx < sy ? sx : sy;
    if (s >= 1.0) return;

    int cx = (int)(width * s), cy = (int)(nlines * s);
    if (cx < 8 || cy < 3) return;
    scale_logo(cx, cy);
    recompute();
}

static void load(int from_stdin) {
    char *itext = from_stdin ? read_fd(STDIN_FILENO) : run_fastfetch();
    info = split(itext, &ninfo);
    if (access(logo_path, R_OK) < 0)
        die("ffanim: no logo at %s\n"
            "put a text file of braille art there, or set FFANIM_LOGO to one",
            logo_path);
    lines = split(read_file(logo_path), &nlines);

    orig_lines = lines;
    orig_nlines = nlines;
    orig_info = info;
    orig_ninfo = ninfo;

    recompute();
}

static void row_text(buf *b, int r, double pos, int flat) {
    int drawn = 0;
    if (r >= 1 && r - 1 < nlines) {
        double d = fabs((double)(r - 1) - pos) / SPAN;
        double lit = 1.0 - d;
        if (lit < 0.0) lit = 0.0;
        double bright = flat ? 1.0 : DIM + (1.0 - DIM) * lit;
        int v = (int)(70.0 + 185.0 * bright + 0.5);
        buf_fmt(b, "\x1b[38;2;%d;%d;%dm", v, v, v);
        buf_str(b, lines[r - 1]);
        buf_str(b, "\x1b[m");
        drawn = vis_width(lines[r - 1]);
    }
    buf_pad(b, width - drawn);
    if (r < ninfo) {
        buf_pad(b, GAP);
        buf_str(b, info[r]);
    }
}

static void buf_add_clamped(buf *b, const char *s, int max) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p == 0x1b) {
            const unsigned char *st = p;
            p++;
            if (*p == '[') {
                p++;
                while (*p && (*p < 0x40 || *p > 0x7e)) p++;
                if (*p) p++;
            } else if (*p) {
                p++;
            }
            buf_add(b, (const char *)st, (size_t)(p - st));
            continue;
        }
        if (w >= max) break;
        const unsigned char *st = p;
        p++;
        while ((*p & 0xc0) == 0x80) p++;
        buf_add(b, (const char *)st, (size_t)(p - st));
        w++;
    }
    buf_str(b, "\x1b[m");
}

static char **rowcache;
static int rowcache_n;

static void paint_reset(void) {
    for (int i = 0; i < rowcache_n; i++) free(rowcache[i]);
    free(rowcache);
    rowcache = NULL;
    rowcache_n = 0;
}

static int paint(int fd, double pos, int flat, int max_cols, int max_rows) {
    if (rowcache_n != rows) {
        paint_reset();
        rowcache = xmalloc((size_t)rows * sizeof *rowcache);
        memset(rowcache, 0, (size_t)rows * sizeof *rowcache);
        rowcache_n = rows;
    }
    buf b = {0};
    buf_str(&b, "\x1b\x37");
    for (int r = 0; r < rows; r++) {
        if (max_rows > 0 && r >= max_rows) break;
        buf line = {0};
        row_text(&line, r, pos, flat);
        buf out = {0};
        buf_add(&out, "", 0);
        if (max_cols > 0)
            buf_add_clamped(&out, line.s ? line.s : "", max_cols);
        else
            buf_add(&out, line.s ? line.s : "", line.len);
        free(line.s);
        if (rowcache[r] && !strcmp(rowcache[r], out.s)) {
            free(out.s);
            continue;
        }
        buf_fmt(&b, "\x1b[%d;1H", r + 1);
        buf_add(&b, out.s, out.len);
        buf_str(&b, "\x1b[K");
        free(rowcache[r]);
        rowcache[r] = out.s;
    }
    buf_str(&b, "\x1b\x38");
    ssize_t n = write(fd, b.s, b.len);
    free(b.s);
    return n == (ssize_t)b.len ? 0 : -1;
}

static void print_static(void) {
    buf b = {0};
    for (int r = 0; r < rows; r++) {
        row_text(&b, r, 0.0, 1);
        buf_str(&b, "\n");
    }
    fwrite(b.s, 1, b.len, stdout);
    free(b.s);
}

static char pidfile[4096];

static const char *pidfile_path(void) {
    if (pidfile[0]) return pidfile;
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || !*base) base = "/tmp";
    char tty[256] = "tty";
    ssize_t n = readlink("/proc/self/fd/1", tty, sizeof tty - 1);
    if (n > 0) {
        tty[n] = '\0';
        for (char *p = tty; *p; p++)
            if (*p == '/') *p = '_';
    }
    snprintf(pidfile, sizeof pidfile, "%s/ffanim-%s.pid", base, tty);
    return pidfile;
}

static void kill_existing(void) {
    FILE *f = fopen(pidfile_path(), "r");
    if (!f) return;
    long pid = 0;
    int got = fscanf(f, "%ld", &pid);
    fclose(f);
    if (got != 1 || pid <= 0) return;

    char path[64], cmd[4096];
    snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, cmd, sizeof cmd - 1);
    close(fd);
    if (n <= 0) return;
    cmd[n] = '\0';
    for (ssize_t i = 0; i < n; i++)
        if (cmd[i] == '\0') cmd[i] = ' ';
    if (!strstr(cmd, "ffanim")) return;
    kill((pid_t)pid, SIGTERM);
}

static volatile sig_atomic_t stop_flag = 0;

static void on_stop(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void on_stop_child(int sig) {
    (void)sig;
    unlink(pidfile);
    _exit(0);
}

static int term_size(int fd, struct winsize *ws) {
    return ioctl(fd, TIOCGWINSZ, ws) == 0 ? 0 : -1;
}

static void nap(double seconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static void wipe_rows(buf *b, int n) {
    for (int r = 0; r < n; r++) buf_fmt(b, "\x1b[%d;1H\x1b[K", r + 1);
}


static void nudge_shell(int fd) {
    struct winsize ws, blip;
    if (ioctl(fd, TIOCGWINSZ, &ws) < 0) return;
    blip = ws;
    blip.ws_row = ws.ws_row > 1 ? ws.ws_row - 1 : ws.ws_row + 1;
    ioctl(fd, TIOCSWINSZ, &blip);
    ioctl(fd, TIOCSWINSZ, &ws);
}

static pid_t shell_sid;

static int fg_pgid(void) {
    char path[64], st[512];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)shell_sid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, st, sizeof st - 1);
    close(fd);
    if (n <= 0) return -1;
    st[n] = '\0';
    char *p = strrchr(st, ')');
    long sid = 0, tpgid = 0;
    if (!p || sscanf(p + 1, " %*c %*d %*d %ld %*d %ld", &sid, &tpgid) != 2)
        return -1;
    if (sid != (long)shell_sid) return -1;
    return (int)tpgid;
}

static int screen_taken(int fd) {
    struct termios t;
    if (!shell_sid || tcgetattr(fd, &t) < 0) return 0;
    if (t.c_lflag & ICANON) return 0;
    int fg = fg_pgid();
    return fg > 0 && fg != (int)shell_sid;
}

static char *probe_text;
static char **probe_lines;
static int probe_n;
static unsigned char *volatile_line;

static int refresh_info(void) {
    char *txt = run_fastfetch();
    int n = 0;
    char **cur = split(txt, &n);
    int changed = 0;

    if (probe_lines && n == probe_n && n == orig_ninfo) {
        if (!volatile_line) {
            volatile_line = xmalloc((size_t)n);
            memset(volatile_line, 0, (size_t)n);
        }
        for (int i = 0; i < n; i++)
            if (strcmp(cur[i], probe_lines[i])) volatile_line[i] = 1;
        for (int i = 0; i < n; i++) {
            if (!volatile_line[i]) continue;
            if (strcmp(orig_info[i], cur[i])) changed = 1;
            orig_info[i] = cur[i];
        }
    }
    free(probe_lines);
    free(probe_text);
    probe_lines = cur;
    probe_text = txt;
    probe_n = n;
    return changed;
}

static void animate_pinned(int fd, double fps, double step, struct winsize last,
                           double refresh) {
    double pos = -SPAN;
    int paused = 0, held = 0;
    int period = refresh > 0 ? (int)(refresh * fps) : 0, tick = 0;
    for (;;) {
        struct winsize ws;
        if (term_size(fd, &ws) < 0) break;
        if (screen_taken(fd)) {
            held = 1;
            nap(1.0 / fps);
            continue;
        }
        if (held) {
            held = 0;
            paint_reset();
            if (!paused && ws.ws_row == last.ws_row && ws.ws_col == last.ws_col) {
                buf b = {0};
                buf_fmt(&b, "\x1b\x37\x1b[%d;%dr\x1b\x38",
                        rows + SPACER + 1, ws.ws_row);
                ssize_t ignored = write(fd, b.s, b.len);
                (void)ignored;
                free(b.s);
            }
        }
        if (ws.ws_row != last.ws_row || ws.ws_col != last.ws_col) {
            int old_rows = rows;
            reset_to_original();
            fit_block(ws.ws_row - SPACER - 3, ws.ws_col);
            int fits = ws.ws_col >= need_w && ws.ws_row >= rows + SPACER + 3;
            buf b = {0};
            if (fits) {
                int top = rows + SPACER + 1;
                int park = top + prompt_lead;
                if (park > ws.ws_row) park = ws.ws_row;
                buf_fmt(&b, "\x1b[2J\x1b[%d;%dr\x1b[%d;1H", top, ws.ws_row, park);
            } else if (!paused) {
                buf_str(&b, "\x1b\x37");
                wipe_rows(&b, old_rows > rows ? old_rows : rows);
                buf_str(&b, "\x1b[r\x1b\x38");
            }
            if (b.len) {
                ssize_t ignored = write(fd, b.s, b.len);
                (void)ignored;
                free(b.s);
                nudge_shell(fd);
            }
            paint_reset();
            paused = !fits;
            last = ws;
        }
        if (period && ++tick >= period) {
            tick = 0;
            if (refresh_info()) {
                reset_to_original();
                fit_block(ws.ws_row - SPACER - 3, ws.ws_col);
                paint_reset();
            }
        }
        if (!paused && paint(fd, pos, 0, ws.ws_col, ws.ws_row) < 0) break;
        nap(1.0 / fps);
        if (!paused) pos = pos > (double)nlines + SPAN ? -SPAN : pos + step;
    }
    unlink(pidfile_path());
}

static void animate_inline(double fps, double step) {
    double pos = -SPAN;
    fputs("\x1b[?25l", stdout);
    while (!stop_flag) {
        buf b = {0};
        for (int r = 0; r < rows; r++) {
            if (r) buf_str(&b, "\n");
            row_text(&b, r, pos, 0);
        }
        fwrite(b.s, 1, b.len, stdout);
        free(b.s);
        fflush(stdout);
        nap(1.0 / fps);
        printf("\x1b[%dA\r", rows - 1);
        pos = pos > (double)nlines + SPAN ? -SPAN : pos + step;
    }
    print_static();
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
}

static void usage(void) {
    puts("ffanim - animated fastfetch, pinned above a working shell\n"
         "\n"
         "  ffanim --pin [--stdin]   pin above the shell and keep animating\n"
         "  ffanim --unpin           stop the pinned animation\n"
         "  ffanim --once            print one static frame and exit\n"
         "  ffanim                   animate in place, Ctrl-C to quit\n"
         "\n"
         "  --stdin                  read the info pane from stdin instead of\n"
         "                           running fastfetch. Prefer this: fastfetch\n"
         "                           names your shell from the parent process\n"
         "                           chain, and from inside ffanim that is us\n"
         "  --fps <n>                frames per second (default 20)\n"
         "  --step <n>               rows the band moves per frame (default 0.35)\n"
         "  --refresh <n>            re-read the info pane every n seconds (default\n"
         "                           off). Only the lines that actually change over\n"
         "                           time are replaced, so the rest stays as your\n"
         "                           shell produced it\n"
         "\n"
         "Env: FFANIM_CONFIG, FFANIM_LOGO override the fastfetch config and\n"
         "the logo file. FFANIM_PROMPT_LEAD (default 2) is how many rows your\n"
         "prompt sits below where the shell puts the cursor when it repaints,\n"
         "used to land it after a resize exactly where it starts in a fresh\n"
         "terminal. 2 suits a prompt with one leading blank line.\n"
         "\n"
         "In ~/.config/fish/config.fish:\n"
         "  function fish_greeting\n"
         "      type -q ffanim; or return\n"
         "      fastfetch --logo none --pipe false | ffanim --pin --stdin\n"
         "  end");
}

static void set_paths(void) {
    const char *home = getenv("HOME");
    const char *lead = getenv("FFANIM_PROMPT_LEAD");
    if (lead && *lead) {
        int v = atoi(lead);
        if (v >= 0 && v <= 16) prompt_lead = v;
    }
    const char *c = getenv("FFANIM_CONFIG");
    const char *l = getenv("FFANIM_LOGO");
    if (!home) home = "";
    if (c && *c)
        snprintf(cfg_path, sizeof cfg_path, "%s", c);
    if (l && *l) {
        snprintf(logo_path, sizeof logo_path, "%s", l);
        return;
    }
    const char *mine[] = { "%s/.config/ffanim/logo",
                           "%s/.config/fastfetch/logo_braille" };
    for (size_t i = 0; i < sizeof mine / sizeof *mine; i++) {
        char p[sizeof logo_path];
        snprintf(p, sizeof p, mine[i], home);
        if (access(p, R_OK) == 0) {
            snprintf(logo_path, sizeof logo_path, "%s", p);
            return;
        }
    }
    snprintf(logo_path, sizeof logo_path, "%s/ffanim/logo_braille", DATADIR);
}

int main(int argc, char **argv) {
    int pin = 0, unpin = 0, once = 0, from_stdin = 0;
    double fps = 20.0, step = 0.35, refresh = 0.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--pin")) pin = 1;
        else if (!strcmp(a, "--unpin")) unpin = 1;
        else if (!strcmp(a, "--once")) once = 1;
        else if (!strcmp(a, "--stdin")) from_stdin = 1;
        else if (!strcmp(a, "--fps") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(a, "--step") && i + 1 < argc) step = atof(argv[++i]);
        else if (!strcmp(a, "--refresh") && i + 1 < argc) refresh = atof(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else die("ffanim: unknown option %s (try --help)", a);
    }
    if (pin + unpin + once > 1) die("ffanim: --pin, --unpin and --once are exclusive");
    if (fps < 1.0 || fps > 120.0) die("ffanim: --fps must be between 1 and 120");
    if (refresh != 0.0 && (refresh < 5.0 || refresh > 3600.0))
        die("ffanim: --refresh must be 0, or between 5 and 3600 seconds");

    set_paths();

    if (unpin) {
        int fd = open("/dev/tty", O_WRONLY);
        kill_existing();
        if (fd >= 0) {
            ssize_t ignored = write(fd, "\x1b[r", 3);
            (void)ignored;
            close(fd);
        }
        unlink(pidfile_path());
        return 0;
    }

    load(from_stdin);

    if (once) {
        print_static();
        return 0;
    }

    int fd = open("/dev/tty", O_WRONLY);
    if (fd < 0) {
        print_static();
        return 0;
    }
    shell_sid = getsid(0);

    struct winsize ws;
    if (term_size(fd, &ws) < 0) {
        print_static();
        return 0;
    }
    fit_block(pin ? ws.ws_row - SPACER - 3 : ws.ws_row - 1, ws.ws_col);
    int min_rows = pin ? rows + SPACER + 3 : rows + 1;
    int too_small = ws.ws_col < need_w || ws.ws_row < min_rows;

    if (pin) {
        if (too_small) {
            if (cfg_path[0])
                execlp("fastfetch", "fastfetch", "-c", cfg_path, (char *)NULL);
            execlp("fastfetch", "fastfetch", (char *)NULL);
            print_static();
            return 0;
        }
        kill_existing();
        int top = rows + SPACER + 1;
        buf b = {0};
        buf_fmt(&b, "\x1b[2J\x1b[H\x1b[%d;%dr", top, ws.ws_row);
        ssize_t ignored = write(fd, b.s, b.len);
        (void)ignored;
        free(b.s);
        paint(fd, 0.0, 1, ws.ws_col, ws.ws_row);
        dprintf(fd, "\x1b[%d;1H", top);

        pid_t child = fork();
        if (child < 0) die("ffanim: fork: %s", strerror(errno));
        if (child > 0) return 0;

        setsid();
        pidfile_path();
        FILE *f = fopen(pidfile, "w");
        if (f) {
            fprintf(f, "%ld", (long)getpid());
            fclose(f);
        }
        signal(SIGTERM, on_stop_child);
        signal(SIGHUP, on_stop_child);
        animate_pinned(fd, fps, step, ws, refresh);
        return 0;
    }

    if (too_small)
        die("ffanim: needs %dx%d, terminal is %dx%d\n"
            "try a shorter logo, or FFANIM_LOGO=<file> with one that fits",
            need_w, min_rows, ws.ws_col, ws.ws_row);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    animate_inline(fps, step);
    return 0;
}
