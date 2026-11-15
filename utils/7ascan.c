/*
 * 7ascan.c - frontend skanowania klatek filmu (Plustek OpticFilm 8100)
 * na bibliotece ui.c/ui.h z tego katalogu.
 *
 * Nowa apka (nie port Xt/Xaw). Cala wiedza o skanerze i obrobce obrazu
 * zostaje w dwoch zewnetrznych skryptach uzytkownika: ~/bin/skanuj (reset
 * portu USB + scanimage) i ~/bin/negatyw (ImageMagick: negatyw -> pozytyw).
 * Apka tylko zbiera parametry, odpala skrypt i pokazuje jego wyjscie -
 * zmiany w procedurze skanowania robi sie w skrypcie, nie tutaj.
 *
 * Dwie zakladki (ui_tabs nad pierwszym boxem, wzorem 7atube.c):
 *  - Scan: stan skanera (podlaczony? jest /dev/ugenN.00? mamy prawo
 *    zapisu?), katalog/prefiks/licznik nazwy pliku, rozdzielczosc,
 *    "Preview after scan" i przyciski Scan/Cancel.
 *  - Convert: parametry negatyw (-g -k -m -c -s jako suwaki z zakresem i
 *    krokiem z tablicy g_params - przeciaganie LPM wzorem 7avol.c, kolko
 *    nad suwakiem = jeden krok; obrot 0/90 CW/90 CCW/180 jako -R,
 *    odbicia poziome/pionowe jako -H/-V), plik wejsciowy
 *    (wypelniany ostatnim skanem), wyjsciowy, wybor przegladarki
 *    (feh/display) i Convert. Wynik otwiera sie w przegladarce. Na prawo
 *    od suwakow histogram (luminancja + krzywe R/G/B) ostatniego skanu
 *    albo wyniku konwersji - liczony z surowego RGB od ImageMagick
 *    (7aScan.imageMagick), patrz StartHistogram.
 * Box pod zakladkami bez ramki, tylko z gorna linia rowka (wzorem
 * 7afilm.c/7atube.c).
 * Pod obiema zakladkami wspolny box z postepem i ostatnimi liniami logu.
 *
 * Stan skanera: "usbdevs -v -d <usbBus>" co STATUS_MS (ta sama logika co
 * find_dev w skanuj: blok z VID:PID, z niego "driver: ugenN"). Uruchamiany
 * przez fork+execvp z argv, nie popen - usbBus/vidpid z zasobow nie
 * przechodza przez powloke. NIE odpytywany w trakcie skanu (skanuj sam
 * resetuje port i urzadzenie znika na chwile). Brak /dev/ugenN.00 albo
 * brak prawa zapisu -> podpowiedz i przycisk Fix: komenda doas w
 * terminalu (7aScan.terminal), bo doas musi zapytac o haslo, a skanuj
 * uruchomiony bez tty by na tym utknal. ugenN i nazwa uzytkownika sa
 * walidowane przed wstawieniem do "sh -c".
 *
 * Proces roboczy (skanuj albo negatyw, zawsze co najwyzej jeden):
 * fork+execvp z argv (bez powloki), stdout+stderr w jednym potoku
 * O_NONBLOCK czytanym przez select() razem z fd polaczenia X - ten sam
 * mechanizm co wyszukiwanie w 7atube.c. scanimage --progress pisze
 * "Progress: 12.3%" oddzielane '\r', wiec '\r' i '\n' koncza linie.
 * Linie z postepem ida do ui_meter, reszta do pierscienia LOG_ROWS linii
 * (stale bufory, bez malloc w petli zdarzen). Dziecko dostaje wlasna
 * grupe procesow (setpgid), zeby Cancel (kill(-pid)) zabil tez scanimage/
 * convert uruchomione przez skrypt. Po EOF potoku waitpid i kod wyjscia.
 * SIGCHLD NIE jest ignorowany (wtedy waitpid nie dalby kodu wyjscia), wiec
 * przegladarka i terminal Fix sa odpalane podwojnym forkiem - wnuk
 * dziedziczy init, a posredni proces jest od razu zbierany.
 *
 * skanuj odmawia nadpisania pliku, wiec licznik sam wybiera pierwszy
 * wolny PREFIXNN.tiff w katalogu (tworzonym przed skanem, jeden poziom).
 * negatyw przy sukcesie wypisuje sciezke wyniku w ostatniej linii -
 * ta linia idzie do przegladarki.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/camera_roll_48.xbm"

#define ROW_H 20
#define TAB_H 22
#define TAB_INDENT 8
#define BOX_BORDER_W 1
#define BOX_PAD_TB 6
#define BOX_MARGIN_TB 6
#define BOX_GAP 2
#define COL_GAP 6
#define STATUS_ROWS 3
#define SETTINGS_ROWS 5
#define CONVERT_ROWS 11
#define LOG_ROWS 5
#define METER_W 200               /* szerokosc paska postepu w linii statusu */
#define LOG_LEN 200
#define LINE_LEN 512
#define PATH_LEN 768
#define FIELD_LEN 256
#define NUM_LEN 16
#define KNOB_R 6                  /* promien galki suwaka (wzorem 7avol.c) */
#define TRACK_H 4
#define HIST_W 256                /* szerokosc wnetrza histogramu = liczba binow */
#define CMD_LEN 256
#define UGEN_LEN 16
#define CAPTURE_LEN 16384
#define STATUS_MS 3000
#define MAX_COUNTER 9999
#define MAX_ARGS 32

enum { TAB_SCAN, TAB_CONVERT, TAB_COUNT };
enum { JOB_NONE, JOB_SCAN, JOB_CONVERT };

static const char *res_labels[] = { "600", "1200", "2400", "3600" };
static const char *viewer_labels[] = { "feh", "display" };
#define N_RES ((int) (sizeof(res_labels) / sizeof(res_labels[0])))
#define N_VIEWERS ((int) (sizeof(viewer_labels) / sizeof(viewer_labels[0])))

/* --- konfiguracja z zasobow X ---------------------------------------- */

static struct {
    char scan_cmd[CMD_LEN];
    char negative_cmd[CMD_LEN];
    char usb_bus[64];
    char vidpid[16];
    char terminal[CMD_LEN];
    char magick[CMD_LEN];    /* ImageMagick do histogramu */
} app_data;

/* --- stan skanera ----------------------------------------------------- */

static struct {
    int checked;     /* 0 dopoki pierwsze usbdevs sie nie wykonalo */
    int connected;
    int dev_exists;
    int writable;
    char ugen[UGEN_LEN];
} g_scanner;

static long g_next_status_ms = 0;

/* --- pola formularzy (wlasnosc apki, edytowane przez ui_textbox) ----- */

static char g_dir[PATH_LEN], g_prefix[FIELD_LEN];
static int g_dir_cur, g_prefix_cur;
static int g_res = N_RES - 1;
static int g_preview = 1;
static char g_next_file[PATH_LEN];
static char g_cached_dir[PATH_LEN] = "\001", g_cached_prefix[FIELD_LEN];

static char g_in[PATH_LEN], g_out[PATH_LEN];
static int g_in_cur, g_out_cur;
/* Parametry negatyw jako suwaki: zakres i krok tutaj (domyslne = domyslne
 * skryptu), wartosc zawsze min + k*step, wiec fmt daje dokladny zapis.
 * r = tor suwaka z ostatniej klatki (w == 0: niewidoczny - inna zakladka),
 * do trafiania ButtonPress/kolkiem w petli zdarzen. */
typedef struct {
    const char *flag, *label, *fmt;
    double min, max, step, val;
    UiRect r;
} Param;

enum { P_GAMMA, P_CONTRAST, P_MID, P_CLIP, P_SHAVE, P_COUNT };

static Param g_params[P_COUNT] = {
    { "-g", "Gamma:",    "%.2f",   0.2,   3.0, 0.05, 1.0,  { 0, 0, 0, 0 } },
    { "-k", "Contrast:", "%.1f", -10.0,  10.0, 0.5,  0.0,  { 0, 0, 0, 0 } },
    { "-m", "Mid %:",    "%.0f",   0.0, 100.0, 1.0,  50.0, { 0, 0, 0, 0 } },
    { "-c", "Clip %:",   "%.2f",   0.0,   5.0, 0.01, 0.05, { 0, 0, 0, 0 } },
    { "-s", "Shave %:",  "%.1f",   0.0,  15.0, 0.5,  4.0,  { 0, 0, 0, 0 } },
};
static int g_drag = -1;              /* przeciagany suwak albo -1 */
static const char *rot_labels[] = { "0", "90 CW", "90 CCW", "180" };
static const char *rot_args[] = { NULL, "90", "270", "180" };  /* negatyw -R */
#define N_ROT ((int) (sizeof(rot_labels) / sizeof(rot_labels[0])))
static int g_rotate = 0;             /* indeks w rot_labels */
static int g_flop = 0, g_flip = 0;   /* odbicie poziome / pionowe */
static int g_viewer = 0;

static int g_tab = TAB_SCAN;

/* --- proces roboczy --------------------------------------------------- */

static int g_job = JOB_NONE;
static pid_t g_pid = -1;
static int g_fd = -1;
static char g_line[LINE_LEN];
static int g_line_len = 0;
static char g_last_line[LINE_LEN];
static char g_job_file[PATH_LEN];    /* plik skanu albo wejscie konwersji */
static double g_progress = -1.0;     /* < 0 = brak postepu do pokazania */
static char g_log[LOG_ROWS][LOG_LEN];
static int g_log_head = 0, g_log_count = 0;
static char g_status[LINE_LEN] = "Ready";

/* -------------------------------------------------------------------- */
/* Narzedzia                                                            */
/* -------------------------------------------------------------------- */

static void
ExpandHome(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, outsz, "%s%s", home, in + 1);
    else
        snprintf(out, outsz, "%s", in);
}

static long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Tylko [A-Za-z0-9._-] (i niepuste) - bezpieczne do wstawienia w "sh -c". */
static int
SafeWord(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-'))
            return 0;
    }
    return 1;
}

/* "ugen" + co najmniej jedna cyfra, nic wiecej */
static int
ValidUgen(const char *s)
{
    if (strncmp(s, "ugen", 4) != 0 || !s[4])
        return 0;
    for (s += 4; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

/* Dzieli komende z zasobu po spacjach na argv (bez cudzyslowow - komendy
 * to nazwy/sciezki programow). Modyfikuje buf. Zwraca liczbe tokenow. */
static int
SplitCommand(char *buf, char **argv, int max)
{
    int n = 0;
    char *p = buf;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return n;
}

/* Podwojny fork + execvp: proces odlaczony od apki (przegladarka, terminal
 * Fix), bez zombie i bez kolidowania z waitpid procesu roboczego. */
static void
SpawnDetached(char **argv)
{
    pid_t pid = fork();

    if (pid < 0) {
        snprintf(g_status, sizeof(g_status), "fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

/* -------------------------------------------------------------------- */
/* Stan skanera (usbdevs)                                               */
/* -------------------------------------------------------------------- */

/* /dev/null otwarty raz w main() PRZED pledge - open() do zapisu po
 * pledge wymagalby "wpath" (dziecko przed exec dziedziczy ograniczenia). */
static int g_devnull = -1;

/* Uruchamia argv i czyta cale stdout do buf (blokujaco - tylko do
 * krotkich komend jak usbdevs). Zwraca liczbe bajtow albo -1. */
static int
RunCapture(char **argv, char *buf, int cap)
{
    int fds[2], len = 0;
    ssize_t n;
    pid_t pid;

    if (pipe(fds) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        if (g_devnull >= 0)
            dup2(g_devnull, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    while (len < cap - 1 && (n = read(fds[0], buf + len, (size_t) (cap - 1 - len))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        len += (int) n;
    }
    buf[len] = '\0';
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return len;
}

/* Logika find_dev ze skanuj: linia "addr NN:" zapamietuje adres, linia z
 * VID:PID oznacza nasz blok, pierwsze "driver: X" w tym bloku to ugenN. */
static void
ParseUsbdevs(char *buf, char *ugen, size_t ugensz, int *found)
{
    char *line, *save = NULL, *d;
    int in_block = 0;

    *found = 0;
    ugen[0] = '\0';
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "addr ", 5) == 0)
            in_block = 0;
        if (strstr(line, app_data.vidpid)) {
            in_block = 1;
            *found = 1;
        }
        if (in_block && (d = strstr(line, "driver:")) != NULL) {
            d += 7;
            while (*d == ' ' || *d == '\t')
                d++;
            snprintf(ugen, ugensz, "%.*s", (int) strcspn(d, " \t"), d);
            return;
        }
    }
}

static void
RefreshScanner(void)
{
    static char buf[CAPTURE_LEN];
    char *argv[] = { "usbdevs", "-v", "-d", app_data.usb_bus, NULL };
    char node[64];

    g_scanner.checked = 1;
    g_scanner.connected = g_scanner.dev_exists = g_scanner.writable = 0;
    g_scanner.ugen[0] = '\0';
    if (RunCapture(argv, buf, sizeof(buf)) < 0)
        return;
    ParseUsbdevs(buf, g_scanner.ugen, sizeof(g_scanner.ugen), &g_scanner.connected);
    if (!g_scanner.connected || !ValidUgen(g_scanner.ugen)) {
        g_scanner.ugen[0] = '\0';
        return;
    }
    snprintf(node, sizeof(node), "/dev/%s.00", g_scanner.ugen);
    g_scanner.dev_exists = access(node, F_OK) == 0;
    g_scanner.writable = g_scanner.dev_exists && access(node, W_OK) == 0;
}

static int
ScannerReady(void)
{
    return g_scanner.connected && g_scanner.dev_exists && g_scanner.writable;
}

/* Komenda naprawy dla aktualnego stanu albo 0, gdy nie ma czego naprawiac. */
static int
FixCommand(char *out, size_t outsz)
{
    const char *user = getenv("USER");

    if (!g_scanner.connected || !g_scanner.ugen[0])
        return 0;
    if (!g_scanner.dev_exists) {
        snprintf(out, outsz, "cd /dev && doas sh MAKEDEV %s", g_scanner.ugen);
        return 1;
    }
    if (!g_scanner.writable) {
        if (!user || !SafeWord(user))
            user = getenv("LOGNAME");
        if (!user || !SafeWord(user))
            return 0;
        snprintf(out, outsz, "doas chown %s /dev/%s.*", user, g_scanner.ugen);
        return 1;
    }
    return 0;
}

static void
RunFix(void)
{
    char fix[CMD_LEN], script[CMD_LEN + 64], term[CMD_LEN];
    char *argv[MAX_ARGS];
    int n;

    if (!FixCommand(fix, sizeof(fix)))
        return;
    snprintf(script, sizeof(script), "%s; echo; echo 'press Enter'; read x", fix);
    snprintf(term, sizeof(term), "%s", app_data.terminal);
    n = SplitCommand(term, argv, MAX_ARGS - 5);
    if (n == 0)
        return;
    argv[n++] = "-e";
    argv[n++] = "sh";
    argv[n++] = "-c";
    argv[n++] = script;
    argv[n] = NULL;
    SpawnDetached(argv);
    g_next_status_ms = 0; /* stan od nowa przy nastepnym obrocie petli */
}

/* -------------------------------------------------------------------- */
/* Nazwa pliku skanu                                                    */
/* -------------------------------------------------------------------- */

/* Pierwszy wolny katalog/PREFIXNN.tiff. Przeliczane tylko po zmianie pol
 * (albo force po skanie), nie na kazdy MotionNotify. */
static void
UpdateNextFile(int force)
{
    char dir[PATH_LEN];
    int n, len;

    if (!force && strcmp(g_dir, g_cached_dir) == 0 && strcmp(g_prefix, g_cached_prefix) == 0)
        return;
    snprintf(g_cached_dir, sizeof(g_cached_dir), "%s", g_dir);
    snprintf(g_cached_prefix, sizeof(g_cached_prefix), "%s", g_prefix);
    g_next_file[0] = '\0';
    if (!g_prefix[0] || strchr(g_prefix, '/') || !g_dir[0])
        return;
    ExpandHome(g_dir, dir, sizeof(dir));
    for (n = 1; n <= MAX_COUNTER; n++) {
        len = snprintf(g_next_file, sizeof(g_next_file), "%s/%s%02d.tiff", dir, g_prefix, n);
        if (len < 0 || len >= (int) sizeof(g_next_file)) {
            g_next_file[0] = '\0';
            return;
        }
        if (access(g_next_file, F_OK) != 0 && errno == ENOENT)
            return;
    }
    g_next_file[0] = '\0';
}

/* -------------------------------------------------------------------- */
/* Proces roboczy                                                       */
/* -------------------------------------------------------------------- */

static void
LogPush(const char *s)
{
    int idx = (g_log_head + g_log_count) % LOG_ROWS;

    snprintf(g_log[idx], LOG_LEN, "%s", s);
    if (g_log_count < LOG_ROWS)
        g_log_count++;
    else
        g_log_head = (g_log_head + 1) % LOG_ROWS;
}

static void
LogClear(void)
{
    g_log_head = g_log_count = 0;
}

static void
HandleLine(const char *line)
{
    double pct;

    if (!line[0])
        return;
    if (sscanf(line, "Progress: %lf%%", &pct) == 1) {
        g_progress = pct / 100.0;
        return;
    }
    snprintf(g_last_line, sizeof(g_last_line), "%s", line);
    LogPush(line);
}

static int
StartJob(int job, char **argv)
{
    int fds[2];
    pid_t pid;

    if (g_job != JOB_NONE)
        return 0;
    if (pipe(fds) < 0) {
        snprintf(g_status, sizeof(g_status), "pipe failed: %s", strerror(errno));
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(g_status, sizeof(g_status), "fork failed: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid); /* tez w rodzicu - bez wyscigu z kill(-pid) */
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);  /* nie dziedziczy go histogram */
    g_job = job;
    g_pid = pid;
    g_fd = fds[0];
    g_line_len = 0;
    g_last_line[0] = '\0';
    g_progress = -1.0;
    LogClear();
    return 1;
}

static void
CancelJob(void)
{
    if (g_job == JOB_NONE)
        return;
    kill(-g_pid, SIGTERM);
    snprintf(g_status, sizeof(g_status), "Cancelling...");
}

static void
LaunchViewer(const char *path)
{
    char file[PATH_LEN];
    char *argv[] = { (char *) viewer_labels[g_viewer], file, NULL };

    snprintf(file, sizeof(file), "%s", path);
    SpawnDetached(argv);
}

/* --- histogram ---------------------------------------------------------
 * ImageMagick zmniejsza obraz i wypisuje surowe 8-bitowe RGB na stdout
 * ("magick PLIK[0] -resize 400x400> -depth 8 rgb:-"); apka liczy biny w
 * locie z kawalkow potoku (bez bufora na obraz), wiec pamiec = same biny.
 * Osobny proces obok g_job - startuje po udanym skanie (wejscie) albo
 * konwersji (wynik); nowy histogram zabija poprzedni, jeszcze liczony. */

enum { H_R, H_G, H_B, H_L, H_COUNT };

static unsigned long g_hist[H_COUNT][256];
static int g_hist_valid = 0;
static char g_hist_name[FIELD_LEN];  /* "input: plik" / "result: plik" */
static pid_t g_hpid = -1;
static int g_hfd = -1;
static unsigned char g_hpx[3];       /* niepelny piksel z konca kawalka */
static int g_hpos = 0;

static void
StopHistogram(void)
{
    if (g_hpid < 0)
        return;
    kill(g_hpid, SIGTERM);
    close(g_hfd);
    while (waitpid(g_hpid, NULL, 0) < 0 && errno == EINTR)
        ;
    g_hpid = -1;
    g_hfd = -1;
}

static void
StartHistogram(const char *path, const char *what)
{
    static char file[PATH_LEN + 4];
    const char *base = strrchr(path, '/');
    char *argv[] = { app_data.magick, file, "-resize", "400x400>",
                     "-depth", "8", "rgb:-", NULL };
    int fds[2];
    pid_t pid;

    StopHistogram();
    g_hist_valid = 0;
    snprintf(g_hist_name, sizeof(g_hist_name), "%s: %s", what, base ? base + 1 : path);
    snprintf(file, sizeof(file), "%s[0]", path);  /* [0] = pierwsza strona TIFF */
    if (pipe(fds) < 0)
        return;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        if (g_devnull >= 0)
            dup2(g_devnull, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    memset(g_hist, 0, sizeof(g_hist));
    g_hpos = 0;
    g_hpid = pid;
    g_hfd = fds[0];
}

static void
CountPixels(const unsigned char *buf, ssize_t n)
{
    ssize_t i;

    for (i = 0; i < n; i++) {
        g_hpx[g_hpos++] = buf[i];
        if (g_hpos < 3)
            continue;
        g_hpos = 0;
        g_hist[H_R][g_hpx[0]]++;
        g_hist[H_G][g_hpx[1]]++;
        g_hist[H_B][g_hpx[2]]++;
        /* luminancja Rec. 601 w liczbach calkowitych */
        g_hist[H_L][(299 * g_hpx[0] + 587 * g_hpx[1] + 114 * g_hpx[2]) / 1000]++;
    }
}

/* Wolane, gdy select() zglosi g_hfd jako gotowy. */
static void
PollHistogram(void)
{
    unsigned char buf[4096];
    unsigned long total = 0;
    ssize_t n;
    int st = 0, i;

    for (;;) {
        n = read(g_hfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            break;
        }
        if (n == 0)
            break;
        CountPixels(buf, n);
    }
    close(g_hfd);
    g_hfd = -1;
    while (waitpid(g_hpid, &st, 0) < 0 && errno == EINTR)
        ;
    g_hpid = -1;
    for (i = 0; i < 256; i++)
        total += g_hist[H_L][i];
    g_hist_valid = WIFEXITED(st) && WEXITSTATUS(st) == 0 && total > 0;
    if (!g_hist_valid)
        LogPush("histogram failed (is ImageMagick installed? see 7aScan.imageMagick)");
}

static void
FinishJob(void)
{
    int st = 0, ok;
    int job = g_job;

    if (g_line_len > 0) {
        g_line[g_line_len] = '\0';
        HandleLine(g_line);
        g_line_len = 0;
    }
    close(g_fd);
    g_fd = -1;
    while (waitpid(g_pid, &st, 0) < 0 && errno == EINTR)
        ;
    g_pid = -1;
    g_job = JOB_NONE;
    g_progress = -1.0;
    ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;

    if (!ok) {
        if (WIFSIGNALED(st))
            snprintf(g_status, sizeof(g_status), "%s killed by signal %d",
                     job == JOB_SCAN ? "Scan" : "Convert", WTERMSIG(st));
        else
            snprintf(g_status, sizeof(g_status), "%s failed (exit code %d)",
                     job == JOB_SCAN ? "Scan" : "Convert", WEXITSTATUS(st));
        g_next_status_ms = 0;
        return;
    }
    if (job == JOB_SCAN) {
        snprintf(g_status, sizeof(g_status), "Scan OK: %s", g_job_file);
        snprintf(g_in, sizeof(g_in), "%s", g_job_file);
        g_in_cur = (int) strlen(g_in);
        UpdateNextFile(1);
        if (g_preview)
            LaunchViewer(g_job_file);
        StartHistogram(g_job_file, "input");
        g_next_status_ms = 0;
    } else {
        snprintf(g_status, sizeof(g_status), "Convert OK: %s", g_last_line);
        if (g_last_line[0]) {
            LaunchViewer(g_last_line);
            StartHistogram(g_last_line, "result");
        }
    }
}

/* Wolane, gdy select() zglosi g_fd jako gotowy. '\r' i '\n' koncza linie;
 * linia dluzsza niz LINE_LEN jest przycinana. */
static void
PollJob(void)
{
    char buf[1024];
    ssize_t n;
    int i;

    for (;;) {
        n = read(g_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            break;
        }
        if (n == 0)
            break;
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                g_line[g_line_len] = '\0';
                HandleLine(g_line);
                g_line_len = 0;
            } else if (g_line_len < LINE_LEN - 1) {
                g_line[g_line_len++] = buf[i];
            }
        }
    }
    FinishJob();
}

static void
StartScan(void)
{
    char cmd[CMD_LEN], dir[PATH_LEN];
    char *argv[MAX_ARGS];
    int n;

    if (g_job != JOB_NONE)
        return;
    if (!ScannerReady()) {
        snprintf(g_status, sizeof(g_status), "Scanner not ready - see status above");
        return;
    }
    UpdateNextFile(1);
    if (!g_next_file[0]) {
        snprintf(g_status, sizeof(g_status), "Bad directory or prefix");
        return;
    }
    ExpandHome(g_dir, dir, sizeof(dir));
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        snprintf(g_status, sizeof(g_status), "Cannot create %s: %s", dir, strerror(errno));
        return;
    }
    snprintf(g_job_file, sizeof(g_job_file), "%s", g_next_file);
    snprintf(cmd, sizeof(cmd), "%s", app_data.scan_cmd);
    n = SplitCommand(cmd, argv, MAX_ARGS - 4);
    if (n == 0)
        return;
    argv[n++] = g_job_file;
    argv[n++] = "--resolution";
    argv[n++] = (char *) res_labels[g_res];
    argv[n] = NULL;
    if (StartJob(JOB_SCAN, argv))
        snprintf(g_status, sizeof(g_status), "Scanning %s at %s dpi...", g_job_file, res_labels[g_res]);
}

static void
StartConvert(void)
{
    static char in[PATH_LEN], out[PATH_LEN], vals[P_COUNT][NUM_LEN];
    char cmd[CMD_LEN];
    char *argv[MAX_ARGS];
    int n, i;

    if (g_job != JOB_NONE)
        return;
    ExpandHome(g_in, in, sizeof(in));
    ExpandHome(g_out, out, sizeof(out));
    if (!in[0] || access(in, R_OK) != 0) {
        snprintf(g_status, sizeof(g_status), "Cannot read input file");
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s", app_data.negative_cmd);
    n = SplitCommand(cmd, argv, MAX_ARGS - 18);
    if (n == 0)
        return;
    for (i = 0; i < P_COUNT; i++) {
        snprintf(vals[i], sizeof(vals[i]), g_params[i].fmt, g_params[i].val);
        argv[n++] = (char *) g_params[i].flag;
        argv[n++] = vals[i];
    }
    if (rot_args[g_rotate]) {
        argv[n++] = "-R";
        argv[n++] = (char *) rot_args[g_rotate];
    }
    if (g_flop)
        argv[n++] = "-H";
    if (g_flip)
        argv[n++] = "-V";
    argv[n++] = in;
    if (out[0])
        argv[n++] = out;
    argv[n] = NULL;
    snprintf(g_job_file, sizeof(g_job_file), "%s", in);
    if (StartJob(JOB_CONVERT, argv))
        snprintf(g_status, sizeof(g_status), "Converting %s...", in);
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                           */
/* -------------------------------------------------------------------- */

static int
BoxHeightForRows(int n_rows)
{
    return n_rows * ROW_H + (n_rows - 1) * BOX_GAP + 2 * BOX_BORDER_W + 2 * BOX_PAD_TB;
}

static int
BoxOuter(int n_rows)
{
    return BOX_MARGIN_TB + BoxHeightForRows(n_rows) + BOX_MARGIN_TB;
}

static int
LabelWidth(UiCtx *ctx)
{
    static int w = 0;

    if (w == 0)
        w = ui_text_width(ctx, "Resolution:") + 8;
    return w;
}

/* Wiersz "Etykieta: [reszta]" - zwraca prostokat na reszte */
static UiRect
LabeledRow(UiCtx *ctx, UiRect row, const char *label)
{
    int lw = LabelWidth(ctx);
    UiRect l = { row.x, row.y, lw, row.h };
    UiRect rest = { row.x + lw, row.y, row.w - lw, row.h };

    ui_label(ctx, l, label);
    return rest;
}

/* Rzad wylacznych opcji (ui_selection_mark + etykieta), stan przed
 * rysowaniem (patrz ui_selection_mark w ui.h). */
static void
RadioRow(UiCtx *ctx, UiRect r, const char **labels, int n, int *sel)
{
    int i;
    UiRect col, mark, text;

    for (i = 0; i < n; i++) {
        col = ui_rect_col(r, i, n, COL_GAP);
        if (ui_hit_test(ctx, col))
            *sel = i;
    }
    for (i = 0; i < n; i++) {
        col = ui_rect_col(r, i, n, COL_GAP);
        mark = (UiRect) { col.x, col.y, ROW_H, ROW_H };
        text = (UiRect) { col.x + ROW_H + 4, col.y, col.w - ROW_H - 4, col.h };
        ui_selection_mark(ctx, mark, *sel == i);
        ui_label(ctx, text, labels[i]);
    }
}

/* --- suwaki parametrow negatyw --------------------------------------- */

static void
SetParam(Param *p, double v)
{
    long k;

    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    k = (long) ((v - p->min) / p->step + 0.5);
    p->val = p->min + k * p->step;
}

/* Indeks suwaka pod (x, y) z ostatniej klatki albo -1 */
static int
SliderAt(int x, int y)
{
    int i;
    UiRect r;

    for (i = 0; i < P_COUNT; i++) {
        r = g_params[i].r;
        if (r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

/* Wartosc pod kursorem: srodek galki chodzi od r.x+KNOB_R do r.x+r.w-KNOB_R */
static void
SetParamAt(Param *p, int mx)
{
    int span = p->r.w - 2 * KNOB_R;

    if (span > 0)
        SetParam(p, p->min + (p->max - p->min) * (mx - p->r.x - KNOB_R) / span);
}

/* Stan przeciagania PRZED rysowaniem (wzorzec 7avol.c UpdateDrag): pasek
 * nadaza za kursorem w tej samej klatce, puszczenie LPM konczy sesje. */
static void
UpdateDrag(UiCtx *ctx)
{
    int mx, down;

    if (g_drag < 0)
        return;
    ui_mouse_state(ctx, &mx, NULL, &down);
    SetParamAt(&g_params[g_drag], mx);
    if (!down)
        g_drag = -1;
}

/* Wiersz "Etykieta: [tor suwaka] wartosc". Tor jak w 7avol.c: czesc do
 * galki kolorem aktywnym, reszta nieaktywnym, galka w kolorze tekstu. */
static void
SliderRow(UiCtx *ctx, UiRect row, Param *p)
{
    static int vw = 0;
    UiRect rest = LabeledRow(ctx, row, p->label);
    UiRect track, fill, val;
    char buf[NUM_LEN];
    int span, kx, cy;

    if (vw == 0)
        vw = ui_text_width(ctx, "-10.00") + 8;
    p->r = (UiRect) { rest.x, rest.y, rest.w - vw - COL_GAP, rest.h };
    val = (UiRect) { rest.x + rest.w - vw, rest.y, vw, rest.h };
    span = p->r.w - 2 * KNOB_R;
    if (span <= 0)
        return;
    kx = p->r.x + KNOB_R + (int) (span * (p->val - p->min) / (p->max - p->min) + 0.5);
    cy = p->r.y + p->r.h / 2;
    track = (UiRect) { p->r.x, cy - TRACK_H / 2, p->r.w, TRACK_H };
    fill = track;
    fill.w = kx - p->r.x;
    ui_fill_round_rect(ctx, track, ui_theme_bar_inactive_bg(ctx));
    if (fill.w > 0)
        ui_fill_round_rect(ctx, fill, ui_theme_bar_active_bg(ctx));
    ui_fill_circle(ctx, kx, cy, KNOB_R, ui_theme_fg(ctx));
    snprintf(buf, sizeof(buf), p->fmt, p->val);
    ui_label(ctx, val, buf);
}

/* Kolory krzywych R/G/B histogramu; wlasciciel: main (AllocHistColors /
 * XFreeColors na koncu). Nieprzydzielony kolor = krzywa nie rysowana. */
static XColor g_hcol[3];
static int g_hcol_ok[3];

static void
AllocHistColors(Display *dpy, Colormap cmap)
{
    static const char *names[3] = { "#e04040", "#40c040", "#4080ff" };
    XColor exact;
    int i;

    for (i = 0; i < 3; i++)
        g_hcol_ok[i] = XAllocNamedColor(dpy, cmap, names[i], &g_hcol[i], &exact) != 0;
}

static void
FreeHistColors(Display *dpy, Colormap cmap)
{
    int i;

    for (i = 0; i < 3; i++)
        if (g_hcol_ok[i])
            XFreeColors(dpy, cmap, &g_hcol[i].pixel, 1, 0);
}

/* Najwiekszy bin kanalu ch w kolumnach [b0, b1) */
static unsigned long
BinMax(int ch, int b0, int b1)
{
    unsigned long m = 0;
    int b;

    for (b = b0; b < b1; b++)
        if (g_hist[ch][b] > m)
            m = g_hist[ch][b];
    return m;
}

/* Wysokosc slupka w pikselach; skala = najwiekszy bin poza 0 i 255, wiec
 * przyciete cienie/swiatla (piki na koncach) siegaja sufitu zamiast
 * splaszczac reszte. */
static int
BarH(unsigned long v, unsigned long scale, int h)
{
    unsigned long long y = (unsigned long long) v * (unsigned long long) h / scale;

    return y > (unsigned long long) h ? h : (int) y;
}

/* Luminancja jako slupki (kolor linii), R/G/B jako krzywe na wierzchu. */
static void
DrawHistogram(UiCtx *ctx, UiRect r)
{
    UiRect in = { r.x + 1, r.y + 1, r.w - 2, r.h - 2 };
    unsigned long scale = 1, m;
    int x, ch, b0, b1, y, py[3] = { 0, 0, 0 };

    ui_fill_rect(ctx, r, ui_theme_input_bg(ctx));
    ui_draw_border(ctx, r, 1, ui_theme_line_fg(ctx));
    if (!g_hist_valid || in.w <= 0 || in.h <= 0)
        return;
    for (ch = 0; ch < H_COUNT; ch++)
        if ((m = BinMax(ch, 1, 255)) > scale)
            scale = m;
    for (x = 0; x < in.w; x++) {
        b0 = x * 256 / in.w;
        b1 = (x + 1) * 256 / in.w;
        if (b1 <= b0)
            b1 = b0 + 1;
        y = BarH(BinMax(H_L, b0, b1), scale, in.h);
        if (y > 0)
            ui_fill_rect(ctx, (UiRect) { in.x + x, in.y + in.h - y, 1, y },
                         ui_theme_bar_inactive_bg(ctx));
        for (ch = 0; ch < 3; ch++) {
            y = in.y + in.h - 1 - BarH(BinMax(ch, b0, b1), scale, in.h - 1);
            if (x > 0 && g_hcol_ok[ch])
                ui_draw_line(ctx, in.x + x - 1, py[ch], in.x + x, y, 1, &g_hcol[ch]);
            py[ch] = y;
        }
    }
}

/* Sama GORNA krawedz rowka boxa bez ramki (bez bokow i dolu), wzorem
 * DrawTopLine w utils/7afilm.c: ramki na prostokacie wiekszym od paska
 * linii, przyciete do niego. Rysowac PRZED zakladkami (aktywna wymazuje
 * linie pod soba). */
static void
DrawTopLine(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    int bw = style->border_w;
    int x0 = style->margin_l, w = win_w - style->margin_l - style->margin_r;
    UiRect outer = { x0 - 4 * ROW_H, y, w + 8 * ROW_H, 4 * ROW_H };
    UiRect in = { outer.x + bw, outer.y + bw, outer.w - 2 * bw, outer.h - 2 * bw };

    if (bw <= 0) return;
    ui_set_clip(ctx, (UiRect) { x0, y, w, 2 * bw + 1 });
    ui_draw_bevel(ctx, outer, bw, &style->border_color, 1);
    ui_draw_bevel(ctx, in, bw, &style->border_color, 0);
    ui_draw_inner_bevel(ctx, in, bw, &style->border_color, ui_theme_bg(ctx), 0);
    ui_clear_clip(ctx);
}

static void
DrawTabs(UiCtx *ctx, const UiBoxStyle *style, int win_w)
{
    static const char *labels[TAB_COUNT] = { "Scan", "Convert" };
    UiRect r = { style->margin_l + TAB_INDENT, style->margin_t,
                 win_w - style->margin_l - style->margin_r - 2 * TAB_INDENT, TAB_H };

    ui_tabs(ctx, r, labels, TAB_COUNT, &g_tab, style);
}

static void
ScannerText(char *conn, size_t cs, char *dev, size_t ds, char *hint, size_t hs)
{
    char fix[CMD_LEN];

    hint[0] = '\0';
    if (!g_scanner.checked) {
        snprintf(conn, cs, "checking...");
        snprintf(dev, ds, "-");
        return;
    }
    if (g_job == JOB_SCAN) {
        snprintf(conn, cs, "busy (scanning)");
        snprintf(dev, ds, "-");
        return;
    }
    if (!g_scanner.connected) {
        snprintf(conn, cs, "NOT FOUND (%s on %s)", app_data.vidpid, app_data.usb_bus);
        snprintf(dev, ds, "-");
        snprintf(hint, hs, "Plug in the USB cable and power on the scanner");
        return;
    }
    snprintf(conn, cs, "connected (%s)", g_scanner.ugen[0] ? g_scanner.ugen : "no driver");
    if (!g_scanner.ugen[0])
        snprintf(dev, ds, "-");
    else if (!g_scanner.dev_exists)
        snprintf(dev, ds, "/dev/%s.00 MISSING", g_scanner.ugen);
    else if (!g_scanner.writable)
        snprintf(dev, ds, "/dev/%s.00 NOT WRITABLE", g_scanner.ugen);
    else
        snprintf(dev, ds, "/dev/%s.00 OK", g_scanner.ugen);
    if (FixCommand(fix, sizeof(fix)))
        snprintf(hint, hs, "Fix: %s", fix);
    else if (ScannerReady())
        snprintf(hint, hs, "Ready to scan");
}

static int
DrawStatusBox(UiCtx *ctx, const UiBoxStyle *style, const UiBoxStyle *open, int win_w)
{
    char conn[128], dev[128], hint[CMD_LEN + 8], fix[CMD_LEN];
    UiBox *box;
    UiRect row, rest, b;
    int bw = ui_button_width(ctx, "Fix");

    ScannerText(conn, sizeof(conn), dev, sizeof(dev), hint, sizeof(hint));
    box = ui_box_begin(ctx, "status", 0, TAB_H, win_w, open);
    DrawTopLine(ctx, style, win_w, TAB_H + style->margin_t);
    DrawTabs(ctx, style, win_w);

    row = ui_box_next_rect(box, ROW_H);
    ui_label_ellipsis(ctx, LabeledRow(ctx, row, "Scanner:"), conn);
    row = ui_box_next_rect(box, ROW_H);
    ui_label_ellipsis(ctx, LabeledRow(ctx, row, "Device:"), dev);
    row = ui_box_next_rect(box, ROW_H);
    rest = LabeledRow(ctx, row, "Status:");
    if (g_job != JOB_SCAN && FixCommand(fix, sizeof(fix))) {
        b = (UiRect) { rest.x + rest.w - bw, rest.y, bw, rest.h };
        rest.w -= bw + COL_GAP;
        if (ui_button(ctx, b, "Fix"))
            RunFix();
    }
    ui_label_ellipsis(ctx, rest, hint);
    ui_box_end(box);
    return TAB_H + BoxOuter(STATUS_ROWS);
}

static int
DrawSettingsBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    UiBox *box;
    UiRect row, rest, l, r;
    int bw = ui_button_width(ctx, "Cancel");

    box = ui_box_begin(ctx, "settings", 0, y, win_w, style);
    row = ui_box_next_rect(box, ROW_H);
    ui_textbox(ctx, LabeledRow(ctx, row, "Directory:"), g_dir, sizeof(g_dir), &g_dir_cur);
    row = ui_box_next_rect(box, ROW_H);
    ui_textbox(ctx, LabeledRow(ctx, row, "Prefix:"), g_prefix, sizeof(g_prefix), &g_prefix_cur);

    UpdateNextFile(0);
    row = ui_box_next_rect(box, ROW_H);
    ui_label_ellipsis(ctx, LabeledRow(ctx, row, "Next file:"),
                      g_next_file[0] ? g_next_file : "(invalid directory or prefix)");
    row = ui_box_next_rect(box, ROW_H);
    RadioRow(ctx, LabeledRow(ctx, row, "Resolution:"), res_labels, N_RES, &g_res);

    row = ui_box_next_rect(box, ROW_H);
    rest = LabeledRow(ctx, row, "");
    l = (UiRect) { rest.x, rest.y, rest.w - 2 * bw - 2 * COL_GAP, rest.h };
    ui_checkbox(ctx, l, "Preview after scan", &g_preview);
    r = (UiRect) { rest.x + rest.w - 2 * bw - COL_GAP, rest.y, bw, rest.h };
    if (ui_button(ctx, r, "Scan"))
        StartScan();
    r.x += bw + COL_GAP;
    if (g_job == JOB_SCAN && ui_button(ctx, r, "Cancel"))
        CancelJob();
    ui_box_end(box);
    return y + BoxOuter(SETTINGS_ROWS);
}

static int
DrawConvertBox(UiCtx *ctx, const UiBoxStyle *style, const UiBoxStyle *open, int win_w)
{
    UiBox *box;
    UiRect row, rest, r;
    char hint[PATH_LEN + 16];
    int bw = ui_button_width(ctx, "Convert"), dot, i, hw;

    box = ui_box_begin(ctx, "convert", 0, TAB_H, win_w, open);
    DrawTopLine(ctx, style, win_w, TAB_H + style->margin_t);
    DrawTabs(ctx, style, win_w);

    row = ui_box_next_rect(box, ROW_H);
    ui_textbox(ctx, LabeledRow(ctx, row, "Input:"), g_in, sizeof(g_in), &g_in_cur);
    row = ui_box_next_rect(box, ROW_H);
    ui_textbox(ctx, LabeledRow(ctx, row, "Output:"), g_out, sizeof(g_out), &g_out_cur);
    row = ui_box_next_rect(box, ROW_H);
    rest = LabeledRow(ctx, row, "");
    if (g_out[0]) {
        hint[0] = '\0';
    } else {
        /* domyslna nazwa skryptu: ${in%.*}-poz.tif */
        const char *slash = strrchr(g_in, '/');
        const char *ext = strrchr(g_in, '.');

        dot = (ext && (!slash || ext > slash)) ? (int) (ext - g_in) : (int) strlen(g_in);
        if (g_in[0])
            snprintf(hint, sizeof(hint), "empty = %.*s-poz.tif", dot, g_in);
        else
            snprintf(hint, sizeof(hint), "empty = <input>-poz.tif");
    }
    /* histogram po prawej od suwakow (ich P_COUNT wierszy), podpis nad nim
     * w prawej czesci wiersza z podpowiedzia; 256 px = 1 bin na kolumne */
    hw = rest.w / 2 < HIST_W + 2 ? rest.w / 2 : HIST_W + 2;
    r = (UiRect) { rest.x + rest.w - hw, rest.y, hw, rest.h };
    rest.w -= hw + COL_GAP;
    ui_label_ellipsis(ctx, rest, hint);
    ui_label_ellipsis(ctx, r, g_hpid >= 0 ? "histogram..." :
                      g_hist_valid ? g_hist_name : "histogram");

    for (i = 0; i < P_COUNT; i++) {
        row = ui_box_next_rect(box, ROW_H);
        if (i == 0)
            DrawHistogram(ctx, (UiRect) { row.x + row.w - hw, row.y, hw,
                                          P_COUNT * ROW_H + (P_COUNT - 1) * BOX_GAP });
        row.w -= hw + COL_GAP;
        SliderRow(ctx, row, &g_params[i]);
    }

    row = ui_box_next_rect(box, ROW_H);
    rest = LabeledRow(ctx, row, "Viewer:");
    rest.w /= 2;
    RadioRow(ctx, rest, viewer_labels, N_VIEWERS, &g_viewer);

    row = ui_box_next_rect(box, ROW_H);
    RadioRow(ctx, LabeledRow(ctx, row, "Rotate:"), rot_labels, N_ROT, &g_rotate);

    row = ui_box_next_rect(box, ROW_H);
    rest = LabeledRow(ctx, row, "");
    r = (UiRect) { rest.x, rest.y, (rest.w - 2 * bw - 2 * COL_GAP) / 2, rest.h };
    ui_checkbox(ctx, r, "Flip horizontal", &g_flop);
    r.x += r.w;
    ui_checkbox(ctx, r, "Flip vertical", &g_flip);
    r = (UiRect) { row.x + row.w - 2 * bw - COL_GAP, row.y, bw, row.h };
    if (ui_button(ctx, r, "Convert"))
        StartConvert();
    r.x += bw + COL_GAP;
    if (g_job == JOB_CONVERT && ui_button(ctx, r, "Cancel"))
        CancelJob();
    ui_box_end(box);
    return TAB_H + BoxOuter(CONVERT_ROWS);
}

/* Wysokosc linii pasek postepu + status (poza boxami, nad boxem logu). */
static int
RunLineOuter(void)
{
    return BOX_MARGIN_TB + ROW_H;
}

/* Pasek postepu po lewej, status po prawej - jedna linia bez boxa. */
static void
DrawRunLine(UiCtx *ctx, int win_w, int y)
{
    int m = ui_window_margin(ctx);
    UiRect line = { m, y + BOX_MARGIN_TB, win_w - 2 * m, ROW_H };
    UiRect meter = line, status = line;
    char pct[16];

    meter.w = METER_W < line.w ? METER_W : line.w;
    status.x = meter.x + meter.w + BOX_GAP;
    status.w = line.w - meter.w - BOX_GAP;
    if (g_progress >= 0.0) {
        snprintf(pct, sizeof(pct), "%.1f%%", g_progress * 100.0);
        ui_meter(ctx, meter, g_progress, pct);
    } else {
        ui_meter(ctx, meter, 0.0, g_job != JOB_NONE ? "working..." : "");
    }
    if (status.w > 0)
        ui_label_ellipsis(ctx, status, g_status);
}

/* Box informacyjny (tlo boxInfoBackground) z ostatnimi liniami logu. */
static void
DrawLogBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    UiBox *box;
    UiRect row;
    int i;

    box = ui_box_begin(ctx, "log", 0, y, win_w, style);
    for (i = 0; i < LOG_ROWS; i++) {
        row = ui_box_next_rect(box, ROW_H);
        if (i < g_log_count)
            ui_label_ellipsis(ctx, row, g_log[(g_log_head + i) % LOG_ROWS]);
    }
    ui_box_end(box);
}

static int
ContentHeight(void)
{
    int scan = BoxOuter(STATUS_ROWS) + BoxOuter(SETTINGS_ROWS);
    int conv = BoxOuter(CONVERT_ROWS);

    return TAB_H + (scan > conv ? scan : conv) + RunLineOuter() + BoxOuter(LOG_ROWS);
}

static void
draw(UiCtx *ctx, int win_w)
{
    static UiBoxStyle style, tab_style, open_style, info_style;
    static int ready = 0;
    int y, i;

    if (!ready) {
        style = (UiBoxStyle) { 0 };
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = BOX_MARGIN_TB;
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = BOX_PAD_TB;
        style.border_w = BOX_BORDER_W;
        style.gap = BOX_GAP;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        tab_style = style;
        tab_style.transparent = 1;
        /* boxy pod zakladkami bez ramki - padding zamiast niej (ta sama
         * wysokosc), z ramki zostaje gorna krawedz (DrawTopLine) */
        open_style = tab_style;
        open_style.border_w = 0;
        open_style.padding_l += BOX_BORDER_W;
        open_style.padding_r += BOX_BORDER_W;
        open_style.padding_t += BOX_BORDER_W;
        open_style.padding_b += BOX_BORDER_W;
        info_style = style;
        info_style.bg_color = *ui_theme_box_info_bg(ctx);
        ready = 1;
    }

    /* UpdateDrag na torach z poprzedniej klatki; potem tory zerowane, bo
     * ustawia je tylko DrawConvertBox - na zakladce Scan zadne klikniecie
     * nie moze trafic w niewidoczny suwak */
    UpdateDrag(ctx);
    for (i = 0; i < P_COUNT; i++)
        g_params[i].r.w = 0;

    if (g_tab == TAB_CONVERT) {
        DrawConvertBox(ctx, &tab_style, &open_style, win_w);
    } else {
        y = DrawStatusBox(ctx, &tab_style, &open_style, win_w);
        DrawSettingsBox(ctx, &tab_style, win_w, y);
    }
    /* postep + log zawsze w tym samym miejscu na obu zakladkach */
    y = ContentHeight() - BoxOuter(LOG_ROWS);
    DrawRunLine(ctx, win_w, y - RunLineOuter());
    DrawLogBox(ctx, &info_style, win_w, y);
}

static int
AnyFocus(UiCtx *ctx)
{
    return ui_has_focus(ctx, g_dir) || ui_has_focus(ctx, g_prefix) ||
           ui_has_focus(ctx, g_in) || ui_has_focus(ctx, g_out);
}

static void
LoadResources(Display *dpy)
{
    char raw[PATH_LEN], res[16];
    int i;

    ui_resource_string(dpy, "7aScan.scanCommand", "7aScan.ScanCommand", raw, sizeof(raw), "skanuj");
    ExpandHome(raw, app_data.scan_cmd, sizeof(app_data.scan_cmd));
    ui_resource_string(dpy, "7aScan.negativeCommand", "7aScan.NegativeCommand", raw, sizeof(raw), "negatyw");
    ExpandHome(raw, app_data.negative_cmd, sizeof(app_data.negative_cmd));
    ui_resource_string(dpy, "7aScan.usbBus", "7aScan.UsbBus", app_data.usb_bus, sizeof(app_data.usb_bus), "/dev/usb0");
    ui_resource_string(dpy, "7aScan.vidpid", "7aScan.Vidpid", app_data.vidpid, sizeof(app_data.vidpid), "07b3:130c");
    ui_resource_string(dpy, "7aScan.terminal", "7aScan.Terminal", app_data.terminal, sizeof(app_data.terminal), "xterm");
    ui_resource_string(dpy, "7aScan.imageMagick", "7aScan.ImageMagick", app_data.magick, sizeof(app_data.magick), "magick");
    ui_resource_string(dpy, "7aScan.directory", "7aScan.Directory", g_dir, sizeof(g_dir), "~/skany");
    ui_resource_string(dpy, "7aScan.prefix", "7aScan.Prefix", g_prefix, sizeof(g_prefix), "klatka");
    ui_resource_string(dpy, "7aScan.resolution", "7aScan.Resolution", res, sizeof(res), "3600");
    for (i = 0; i < N_RES; i++)
        if (strcmp(res, res_labels[i]) == 0)
            g_res = i;
    ui_resource_string(dpy, "7aScan.viewer", "7aScan.Viewer", res, sizeof(res), "feh");
    for (i = 0; i < N_VIEWERS; i++)
        if (strcmp(res, viewer_labels[i]) == 0)
            g_viewer = i;
    g_dir_cur = (int) strlen(g_dir);
    g_prefix_cur = (int) strlen(g_prefix);
}

/* Timeout select(): do nastepnego odswiezenia stanu skanera (poza skanem) */
static struct timeval *
NextTimeout(struct timeval *tv)
{
    long left;

    if (g_job == JOB_SCAN)
        return NULL;
    left = g_next_status_ms - now_ms();
    if (left < 0)
        left = 0;
    tv->tv_sec = left / 1000;
    tv->tv_usec = (left % 1000) * 1000;
    return tv;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    XSizeHints *sizehints;
    int win_w = 620, win_h;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw, clicked = 0;
    char app_name[64] = "7aScan";
    char app_title[64] = "";
    XEvent ev;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[i + 1]);
            i++;
        }
    }

    win_h = ContentHeight();

    g_devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);

#ifdef __OpenBSD__
    /* rpath - zasoby X/fonty, access() na /dev/ugenN; cpath - mkdir
     * katalogu skanow; proc exec - usbdevs/skanuj/negatyw/przegladarka/
     * terminal (dzieci po exec nie sa ograniczane). Bez unveil, jak 7atube. */
    if (pledge("stdio rpath cpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7ascan: brak polaczenia z X11\n");
        return 1;
    }
    LoadResources(dpy);

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    if (geom_mask & WidthValue) win_w = (int) geom_w;
    if (geom_mask & HeightValue) win_h = (int) geom_h;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative) ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative) ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;

    win = XCreateSimpleWindow(dpy, root, win_x, win_y, win_w, win_h, 0,
                               BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();

        if (ch) {
            ch->res_name  = app_name;
            ch->res_class = "7aScan";
            XSetClassHint(dpy, win, ch);
            XFree(ch);
        }
    }

    /* szerokosc swobodna, min_height = wysokosc zawartosci (wzorem 7atube) */
    sizehints = XAllocSizeHints();
    if (sizehints) {
        sizehints->flags = PMinSize | PMaxSize;
        sizehints->min_width = 1;
        sizehints->min_height = ContentHeight();
        sizehints->max_width = 32000;
        sizehints->max_height = 32000;
        if (geom_mask & (WidthValue | HeightValue)) {
            sizehints->flags |= USSize;
            sizehints->width = win_w;
            sizehints->height = win_h;
        }
        if (geom_mask & (XValue | YValue)) {
            sizehints->flags |= USPosition;
            sizehints->x = win_x;
            sizehints->y = win_y;
        }
        XSetWMNormalHints(dpy, win, sizehints);
        XFree(sizehints);
    }

    XMapWindow(dpy, win);

    /* wlasciciel: main, XFreeGC na koncu */
    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7ascan: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, camera_roll_48_width, camera_roll_48_height, camera_roll_48_bits);
    AllocHistColors(dpy, DefaultColormap(dpy, screen));

    /* dwie klatki przed petla - cache wysokosci boxow (wzorem 7atube) */
    for (i = 0; i < 2; i++) {
        ui_begin_frame(ctx);
        draw(ctx, win_w);
        ui_end_frame(ctx);
    }

    running = 1;
    redraw = 1;

    while (running) {
        if (g_job != JOB_SCAN && now_ms() >= g_next_status_ms) {
            RefreshScanner();
            g_next_status_ms = now_ms() + STATUS_MS;
            redraw = 1;
        }

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* kolko myszy nie jest klikiem (ui.c nie rozroznia przyciskow);
             * nad suwakiem zmienia wartosc o jeden krok */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                int s = SliderAt(ev.xbutton.x, ev.xbutton.y);

                if (ev.type == ButtonPress && s >= 0 && g_drag < 0) {
                    Param *p = &g_params[s];

                    SetParam(p, p->val + (ev.xbutton.button == Button4 ? p->step : -p->step));
                    redraw = 1;
                }
                continue;
            }

            /* LPM na torze zaczyna przeciaganie (konczy je UpdateDrag) */
            if (ev.type == ButtonPress && ev.xbutton.button == Button1)
                g_drag = SliderAt(ev.xbutton.x, ev.xbutton.y);

            /* Escape w polu zdejmuje fokus; poza polem Escape/q zamyka */
            if (ev.type == KeyPress) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);

                if (AnyFocus(ctx)) {
                    if (ks == XK_Escape) {
                        ui_set_focus(ctx, NULL);
                        redraw = 1;
                        continue;
                    }
                } else if (ks == XK_Escape || ks == XK_q) {
                    running = 0;
                    continue;
                }
            }

            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
                break;
            case ButtonRelease:
                clicked = 1;
                redraw = 1;
                break;
            case ButtonPress:
            case MotionNotify:
            case KeyPress:
                redraw = 1;
                break;
            case MapNotify:
                XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;
                    ui_resize(ctx, win_w, win_h);
                }
                redraw = 1;
                break;
            }
        }

        if (redraw) {
            /* po kliku 3 klatki: zmiana stanu (zakladka, Scan) widoczna od
             * razu, a cache wysokosci nowych boxow sie zapelnia */
            for (i = clicked ? 3 : 1; i > 0; i--) {
                ui_begin_frame(ctx);
                draw(ctx, win_w);
                ui_end_frame(ctx);
            }
            redraw = clicked = 0;
        }

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy);
            int maxfd = xfd;
            struct timeval tv, *tvp;

            if (XPending(dpy))
                continue;
            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            if (g_fd >= 0) {
                FD_SET(g_fd, &rfds);
                if (g_fd > maxfd) maxfd = g_fd;
            }
            if (g_hfd >= 0) {
                FD_SET(g_hfd, &rfds);
                if (g_hfd > maxfd) maxfd = g_hfd;
            }
            tvp = NextTimeout(&tv);
            if (select(maxfd + 1, &rfds, NULL, NULL, tvp) > 0) {
                if (g_fd >= 0 && FD_ISSET(g_fd, &rfds)) {
                    PollJob();
                    redraw = 1;
                }
                if (g_hfd >= 0 && FD_ISSET(g_hfd, &rfds)) {
                    PollHistogram();
                    redraw = 1;
                }
            }
        }
    }

    if (g_job != JOB_NONE) {
        kill(-g_pid, SIGTERM);
        close(g_fd);
        waitpid(g_pid, NULL, 0);
    }
    StopHistogram();
    FreeHistColors(dpy, DefaultColormap(dpy, screen));
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
