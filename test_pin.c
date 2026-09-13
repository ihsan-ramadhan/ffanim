#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
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

#define MAXROWS 256
#define MARK "\342\200\226"

static int master = -1;
static pid_t shell;
static char slave_name[256];
static char conf_dir[256];
static char conf_path[320];
static char dir[256];

static char *cap;
static size_t cap_len, cap_cap;

static int failures, total;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void cap_reset(void) { cap_len = 0; }

static void cap_add(const char *s, size_t n) {
    if (cap_len + n + 1 > cap_cap) {
        while (cap_cap < cap_len + n + 1) cap_cap = cap_cap ? cap_cap * 2 : 1 << 16;
        cap = realloc(cap, cap_cap);
        if (!cap) die("test_pin: out of memory");
    }
    memcpy(cap + cap_len, s, n);
    cap_len += n;
    cap[cap_len] = '\0';
}

static size_t pump(double secs) {
    double end = now() + secs;
    size_t got = 0;
    for (;;) {
        double left = end - now();
        if (left <= 0) break;
        struct pollfd p = { master, POLLIN, 0 };
        if (poll(&p, 1, (int)(left * 1000)) <= 0) continue;
        char b[1 << 16];
        ssize_t n = read(master, b, sizeof b);
        if (n <= 0) break;
        cap_add(b, (size_t)n);
        got += (size_t)n;
    }
    return got;
}

static double rate(double secs) { return pump(secs) / secs / 1024.0; }

static void send(const char *fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    line[n++] = '\n';
    if (write(master, line, n) != (ssize_t)n) die("test_pin: write to pty failed");
}

typedef struct {
    int moves, frames, ed3, top, bot, marked;
} scan;

static scan scan_output(void) {
    scan s = {0};
    unsigned char marked[MAXROWS];
    memset(marked, 0, sizeof marked);

    const unsigned char *p = (const unsigned char *)cap;
    const unsigned char *e = p + cap_len;
    while (p < e) {
        if (*p != 0x1b) { p++; continue; }
        if (p + 1 < e && p[1] == '7') { s.frames++; p += 2; continue; }
        if (p + 1 >= e || p[1] != '[') { p++; continue; }

        const unsigned char *q = p + 2;
        int first = -1, cur = -1;
        while (q < e && ((*q >= '0' && *q <= '9') || *q == ';')) {
            if (*q == ';') { first = cur; cur = -1; }
            else cur = (cur < 0 ? 0 : cur) * 10 + (*q - '0');
            q++;
        }
        if (q >= e) break;

        if (*q == 'H' && first > 0 && cur == 1) {
            s.moves++;
            if (first < MAXROWS) {
                const unsigned char *body = q + 1;
                const unsigned char *stop = body;
                size_t room = (size_t)(e - body);
                if (room > 4096) room = 4096;
                while (stop + 2 < body + room &&
                       !(stop[0] == 0x1b && stop[1] == '[' && stop[2] == 'K')) stop++;
                marked[first] = memmem(body, (size_t)(stop - body), MARK, 3) ? 1 : 0;
            }
        } else if (*q == 'r' && first > 0 && cur > 0) {
            s.top = first;
            s.bot = cur;
        } else if (*q == 'J' && cur == 3) {
            s.ed3++;
        }
        p = q + 1;
    }
    for (int i = 0; i < MAXROWS; i++) s.marked += marked[i];
    return s;
}

static void check(const char *name, int ok, const char *fmt, ...) {
    total++;
    if (ok) return;
    failures++;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL %s: ", name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void cleanup(void) {
    if (slave_name[0]) {
        const char *base = getenv("XDG_RUNTIME_DIR");
        if (!base || !*base) base = "/tmp";
        char key[sizeof slave_name];
        snprintf(key, sizeof key, "%s", slave_name);
        for (char *c = key; *c; c++)
            if (*c == '/') *c = '_';
        char path[512];
        snprintf(path, sizeof path, "%s/ffanim-%s.pid", base, key);
        FILE *f = fopen(path, "r");
        if (f) {
            long pid = 0;
            if (fscanf(f, "%ld", &pid) == 1 && pid > 0) kill((pid_t)pid, SIGTERM);
            fclose(f);
            unlink(path);
        }
    }
    if (shell > 0) {
        kill(shell, SIGKILL);
        waitpid(shell, NULL, 0);
    }
    if (master >= 0) close(master);
    if (conf_path[0]) unlink(conf_path);
    if (conf_dir[0]) rmdir(conf_dir);
}

static void start_shell(void) {
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) die("test_pin: no pty");
    if (ptsname_r(master, slave_name, sizeof slave_name)) die("test_pin: no pty name");

    struct winsize ws = { 40, 120, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);

    shell = fork();
    if (shell < 0) die("test_pin: fork failed");
    if (shell == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        char logo[512];
        snprintf(logo, sizeof logo, "%s/logo_braille", dir);
        setenv("FFANIM_LOGO", logo, 1);
        setenv("FFANIM_CONFIG", conf_path, 1);
        execlp("bash", "bash", "--norc", "--noprofile", "-i", (char *)NULL);
        _exit(127);
    }
}

int main(void) {
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n <= 0) die("test_pin: cannot locate myself");
    dir[n] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    snprintf(conf_dir, sizeof conf_dir, "/tmp/ffanim-test-XXXXXX");
    if (!mkdtemp(conf_dir)) die("test_pin: mkdtemp failed");
    snprintf(conf_path, sizeof conf_path, "%s/conf.jsonc", conf_dir);
    FILE *cf = fopen(conf_path, "w");
    if (!cf) die("test_pin: cannot write %s", conf_path);
    fputs("{\"modules\":[\"os\",\"kernel\",\"uptime\",\"memory\"]}\n", cf);
    fclose(cf);

    atexit(cleanup);
    start_shell();

    pump(1.0);
    send("fastfetch -c %s --logo none --pipe false"
         " | while IFS= read -r l; do printf '\\342\\200\\226%%s\\n' \"$l\"; done"
         " | %s/ffanim --pin --stdin --refresh 5", conf_path, dir);
    pump(1.5);

    check("prompt", rate(2.0) > 10.0, "the painter should animate at the prompt");

    send("stty raw; sleep 6; stty sane");
    pump(1.0);
    check("tui", rate(3.0) < 1.0, "the painter must write nothing while a TUI owns the screen");
    pump(2.5);
    check("resume", rate(2.0) > 10.0, "the painter should animate again once the TUI exits");

    send("sleep 5");
    pump(0.7);
    check("command", rate(2.0) > 10.0, "the painter should animate through an ordinary command");

    int block = 0;
    int heights[2] = { 34, 40 };
    for (int i = 0; i < 2; i++) {
        cap_reset();
        struct winsize ws = { (unsigned short)heights[i], 120, 0, 0 };
        ioctl(master, TIOCSWINSZ, &ws);
        pump(1.2);
        scan s = scan_output();
        check("resize-scrollback", s.ed3 == 0, "a resize must not drop the scrollback");
        check("resize-region", s.bot == heights[i],
              "a resize must re-arm the scroll region to the new height, got %d", s.bot);
        if (s.top > 0) block = s.top - 2;
    }

    cap_reset();
    pump(3.0);
    scan s = scan_output();
    double per_frame = s.frames ? (double)s.moves / s.frames : 999.0;
    check("diff-paint", block > 0 && per_frame < block,
          "a frame rewrote %.1f of %d rows; it should rewrite fewer", per_frame, block);

    cap_reset();
    pump(2.5);
    int before = scan_output().marked;
    cap_reset();
    pump(12.0);
    int after = scan_output().marked;
    check("refresh-identity", before > 0 && after > 0,
          "a refresh overwrote every line the shell produced (%d marked rows -> %d)",
          before, after);

    if (failures) {
        printf("%d/%d failed\n", failures, total);
        return 1;
    }
    puts("ok");
    return 0;
}
