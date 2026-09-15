#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
    int moves, frames, ed3, top, bot, marked, maxrow;
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
            if (first > s.maxrow) s.maxrow = first;
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

static double ink_per_row(void) {
    int rows = scan_output().moves;
    long ink = 0;
    for (size_t i = 0; i + 2 < cap_len; i++) {
        unsigned char a = (unsigned char)cap[i];
        unsigned char b = (unsigned char)cap[i + 1];
        unsigned char c = (unsigned char)cap[i + 2];
        if (a == 0xe2 && b >= 0xa0 && b <= 0xa3 && c >= 0x80 && c <= 0xbf)
            ink += __builtin_popcount((unsigned)((b & 0x03) << 6 | (c & 0x3f)));
    }
    return rows ? (double)ink / rows : 0.0;
}

static int bend_spread(void) {
    int lo[MAXROWS], hi[MAXROWS];
    for (int i = 0; i < MAXROWS; i++) { lo[i] = 1 << 20; hi[i] = -1; }
    const unsigned char *p = (const unsigned char *)cap, *e = p + cap_len;
    while (p < e) {
        if (*p != 0x1b || p + 1 >= e || p[1] != '[') { p++; continue; }
        const unsigned char *q = p + 2;
        int first = -1, cur = -1;
        while (q < e && ((*q >= '0' && *q <= '9') || *q == ';')) {
            if (*q == ';') { first = cur; cur = -1; }
            else cur = (cur < 0 ? 0 : cur) * 10 + (*q - '0');
            q++;
        }
        if (q >= e) break;
        if (*q == 'H' && first > 0 && first < MAXROWS && cur == 1) {
            const unsigned char *t = q + 1;
            int idx = 0, found = -1;
            while (t + 2 < e && !(t[0] == 0x1b && t[1] == '[' && t[2] == 'K')) {
                if (t[0] == 0xe2 && t[1] >= 0xa0 && t[1] <= 0xa3) {
                    if (((t[1] & 3) << 6 | (t[2] & 0x3f)) && found < 0) found = idx;
                    idx++;
                    t += 3;
                    continue;
                }
                t++;
            }
            if (found >= 0) {
                if (found < lo[first]) lo[first] = found;
                if (found > hi[first]) hi[first] = found;
            }
        }
        p = q + 1;
    }
    int spread = 0;
    for (int i = 0; i < MAXROWS; i++)
        if (hi[i] >= 0 && hi[i] - lo[i] > spread) spread = hi[i] - lo[i];
    return spread;
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
    if (conf_dir[0]) {
        char p[700];
        snprintf(p, sizeof p, "%s/home/.zshrc", conf_dir); unlink(p);
        snprintf(p, sizeof p, "%s/home/.bashrc", conf_dir); unlink(p);
        snprintf(p, sizeof p, "%s/home/.bashrc.ffanim.bak", conf_dir); unlink(p);
        snprintf(p, sizeof p, "%s/home/.config/ffanim/off", conf_dir); unlink(p);
        snprintf(p, sizeof p, "%s/home/.config/ffanim/anim", conf_dir); unlink(p);
        snprintf(p, sizeof p, "%s/home/.config/ffanim/color", conf_dir); unlink(p);
        snprintf(p, sizeof p, "%s/home/.config/ffanim", conf_dir); rmdir(p);
        snprintf(p, sizeof p, "%s/home/.config", conf_dir); rmdir(p);
        snprintf(p, sizeof p, "%s/home", conf_dir); rmdir(p);
        rmdir(conf_dir);
    }
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

static char home[400];

static void make_home(void) {
    char p[600];
    snprintf(home, sizeof home, "%s/home", conf_dir);
    mkdir(home, 0755);
    snprintf(p, sizeof p, "%s/.config", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.config/ffanim", home);
    mkdir(p, 0755);
}

static void write_pref(const char *name, const char *value) {
    char p[700];
    snprintf(p, sizeof p, "%s/.config/ffanim/%s", home, name);
    FILE *f = fopen(p, "w");
    if (!f) die("test_pin: cannot write %s", p);
    fprintf(f, "%s\n", value);
    fclose(f);
}

static void cli_checks(void) {
    char logo[512], cmd[4096], out[8192] = {0};
    snprintf(logo, sizeof logo, "%s/logo_braille", dir);
    setenv("FFANIM_LOGO", logo, 1);
    make_home();

    snprintf(cmd, sizeof cmd,
             "HOME=%s %s/ffanim --color 10,20,30 >/dev/null && "
             "printf 'CPU: x\\n' | HOME=%s %s/ffanim --once --stdin", home, dir, home, dir);
    FILE *f = popen(cmd, "r");
    size_t n = f ? fread(out, 1, sizeof out - 1, f) : 0;
    if (f) pclose(f);
    out[n] = '\0';
    check("color", n > 0 && strstr(out, "\x1b[38;2;10;20;30m") != NULL,
          "a saved --color should paint the logo in that colour at full brightness");

    snprintf(cmd, sizeof cmd, "HOME=%s %s/ffanim --anim nope 2>/dev/null", home, dir);
    check("anim-name", system(cmd) != 0, "--anim should reject a name it does not know");

    snprintf(cmd, sizeof cmd, "HOME=%s %s/ffanim --anim wave >/dev/null"
             " && grep -qx wave %s/.config/ffanim/anim", home, dir, home);
    check("anim-saved", system(cmd) == 0,
          "--anim should save the choice so the next terminal picks it up");

    snprintf(cmd, sizeof cmd, "echo nonsense > %s/.config/ffanim/anim"
             " && printf 'CPU: x\\n' | HOME=%s %s/ffanim --once --stdin >/dev/null",
             home, home, dir);
    check("pref-damaged", system(cmd) == 0, "a damaged preference file must not stop ffanim");

    snprintf(cmd, sizeof cmd, "HOME=%s %s/ffanim --on", home, dir);
    FILE *g = popen(cmd, "r");
    n = g ? fread(out, 1, sizeof out - 1, g) : 0;
    if (g) pclose(g);
    out[n] = '\0';
    int nudged = n > 0 && strstr(out, "shell config") != NULL;
    snprintf(cmd, sizeof cmd, "%s/.zshrc", home);
    FILE *rc = fopen(cmd, "w");
    if (rc) { fputs("# ffanim starts here\n", rc); fclose(rc); }
    snprintf(cmd, sizeof cmd, "HOME=%s %s/ffanim --on", home, dir);
    g = popen(cmd, "r");
    n = g ? fread(out, 1, sizeof out - 1, g) : 0;
    if (g) pclose(g);
    out[n] = '\0';
    snprintf(cmd, sizeof cmd,
             "printf 'export X=1\\n' > %s/.bashrc"
             " && HOME=%s SHELL=/bin/bash %s/ffanim --setup >/dev/null"
             " && HOME=%s SHELL=/bin/bash %s/ffanim --setup >/dev/null"
             " && [ \"$(grep -c '>>> ffanim >>>' %s/.bashrc)\" = 1 ]"
             " && grep -q 'export X=1' %s/.bashrc",
             home, home, dir, home, dir, home, home);
    check("setup", system(cmd) == 0,
          "--setup should add one marked block and keep what the file already had");

    snprintf(cmd, sizeof cmd,
             "HOME=%s SHELL=/bin/bash %s/ffanim --unsetup >/dev/null"
             " && ! grep -q ffanim %s/.bashrc"
             " && grep -q 'export X=1' %s/.bashrc",
             home, dir, home, home);
    check("unsetup", system(cmd) == 0,
          "--unsetup should take the block out and leave the rest of the file alone");

    check("off-hint", nudged && !(n > 0 && strstr(out, "shell config")),
          "--on should say so when no shell config starts ffanim, and stay quiet when one does");
    unsetenv("FFANIM_LOGO");
}

static void start_wrap(const char *home) {
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) die("test_pin: no pty");
    if (ptsname_r(master, slave_name, sizeof slave_name)) die("test_pin: no pty name");
    struct winsize ws = { 30, 100, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);

    shell = fork();
    if (shell < 0) die("test_pin: fork failed");
    if (shell == 0) {
        setsid();
        int s = open(slave_name, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(master);
        char logo[512], cmd[1024];
        snprintf(logo, sizeof logo, "%s/logo_braille", dir);
        setenv("FFANIM_LOGO", logo, 1);
        setenv("SHELL", "/bin/bash", 1);
        setenv("HISTFILE", "", 1);
        if (home) setenv("HOME", home, 1);
        snprintf(cmd, sizeof cmd,
                 "fastfetch -c %s --logo none --pipe false | %s/ffanim --wrap --stdin",
                 conf_path, dir);
        execlp("sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void clear_checks(void) {
    start_wrap(NULL);
    pump(2.0);
    send("printf '\\033[2J\\033[H'");
    pump(1.5);
    cap_reset();
    pump(2.0);
    scan s = scan_output();
    check("wrap-clear-quiet", s.moves == 0,
          "a full screen clear wipes the block, so --wrap must stop painting "
          "instead of landing on whatever took its place (%d moves)", s.moves);
    kill(shell, SIGKILL);
    waitpid(shell, NULL, 0);
    shell = 0;
    close(master);
    master = -1;
}

static void off_checks(void) {
    char path[700];
    make_home();
    snprintf(path, sizeof path, "%s/.config/ffanim/off", home);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) die("test_pin: cannot write %s", path);
    close(fd);

    start_wrap(home);
    size_t printed = pump(2.0);
    check("off-prints", printed > 0, "--off must still print the block once");
    cap_reset();
    pump(2.0);
    scan s = scan_output();
    check("off-quiet", s.moves == 0, "--off must not animate, wrote %d moves", s.moves);

    send("echo MARK''ER");
    pump(1.5);
    check("off-shell", memmem(cap, cap_len, "MARKER", 6) != NULL,
          "--off must still hand the terminal to a working shell");

    kill(shell, SIGKILL);
    waitpid(shell, NULL, 0);
    shell = 0;
    close(master);
    master = -1;
    snprintf(path, sizeof path, "%s/.config/ffanim/off", home);
    unlink(path);
}

static void wrap_checks(void) {
    start_wrap(NULL);

    pump(2.0);
    cap_reset();
    pump(2.0);
    scan a = scan_output();
    check("wrap-animates", a.moves > 0, "--wrap should animate the block it printed");

    send("echo MARK''ER");
    pump(1.5);
    check("wrap-shell", memmem(cap, cap_len, "MARKER", 6) != NULL,
          "--wrap must run a working shell inside itself");

    send("printf '\\033[?1049h'; sleep 3; printf '\\033[?1049l'");
    pump(1.0);
    cap_reset();
    pump(1.5);
    scan alt = scan_output();
    check("wrap-alt-quiet", alt.moves == 0,
          "--wrap must not paint over a full-screen app, wrote %d moves", alt.moves);
    pump(2.0);
    cap_reset();
    pump(1.5);
    scan back = scan_output();
    check("wrap-alt-resumes", back.moves > 0,
          "--wrap must animate again once the full-screen app leaves");

    for (int i = 0; i < 4; i++) {
        send("seq 1 4");
        pump(1.2);
    }
    cap_reset();
    pump(2.0);
    scan f = scan_output();
    check("wrap-freezes", f.moves == 0 || f.maxrow == a.maxrow,
          "--wrap must only ever paint the block where it printed it, so what "
          "lands in the scrollback is what was printed; painted up to row %d, "
          "printed at %d", f.maxrow, a.maxrow);

    send("exit");
    int gone = 0;
    for (int i = 0; i < 40 && !gone; i++) {
        pump(0.1);
        gone = waitpid(shell, NULL, WNOHANG) > 0;
    }
    check("wrap-exits", gone, "--wrap must exit when the shell inside it exits");

    if (!gone) kill(shell, SIGKILL);
    shell = 0;
    close(master);
    master = -1;
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
    cli_checks();
    clear_checks();
    off_checks();
    wrap_checks();
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

    write_pref("anim", "sweep");
    send("fastfetch -c %s --logo none --pipe false | HOME=%s %s/ffanim --pin --stdin",
         conf_path, home, dir);
    pump(1.5);
    double sweep = rate(2.0);
    write_pref("anim", "pulse");
    send("fastfetch -c %s --logo none --pipe false | HOME=%s %s/ffanim --pin --stdin",
         conf_path, home, dir);
    pump(1.5);
    double pulse = rate(2.0);
    write_pref("anim", "sweep");
    send("fastfetch -c %s --logo none --pipe false | HOME=%s %s/ffanim --pin --stdin",
         conf_path, home, dir);
    pump(1.5);
    cap_reset();
    pump(2.0);
    double solid = ink_per_row();
    int straight = bend_spread();
    write_pref("anim", "glitch");
    send("fastfetch -c %s --logo none --pipe false | HOME=%s %s/ffanim --pin --stdin",
         conf_path, home, dir);
    pump(1.5);
    cap_reset();
    pump(2.0);
    double eaten = ink_per_row();
    int painted = scan_output().moves;
    check("anim-glitch", painted > 20 && eaten < solid * 0.85,
          "glitch should keep painting and eat dots as its band travels, so its "
          "rows must carry fewer than sweep's (%d rows painted, %.0f dots vs %.0f)",
          painted, eaten, solid);

    write_pref("anim", "ripple");
    send("fastfetch -c %s --logo none --pipe false | HOME=%s %s/ffanim --pin --stdin",
         conf_path, home, dir);
    pump(1.5);
    cap_reset();
    pump(2.0);
    int bend = bend_spread();
    check("anim-ripple", bend >= 2 && bend > straight,
          "ripple should slide the logo sideways as the bend travels, sweep should "
          "not (%d cells against %d)", bend, straight);

    check("anim-pulse", pulse > sweep * 2.0,
          "pulse relights every row each frame and sweep only a few, so pulse should "
          "cost far more (%.1f vs %.1f KiB/s)", pulse, sweep);

    if (failures) {
        printf("%d/%d failed\n", failures, total);
        return 1;
    }
    puts("ok");
    return 0;
}
