/* 7acnc - prosty symulator sciezki narzedzia CNC (G-code) w rzucie
 * izometrycznym, na ui.c/ui.h.
 *
 * Nowa apka (nie port Xt/Xaw). Zakres celowo waski - podstawowe slowa
 * DIN 66025: G0/G1/G2/G3, G4 (postoj), G17/G18/G19 (plaszczyzna luku),
 * G28, G33 (gwint, skok K), G70/G71 (= G20/G21), G90/G91, G94/G95
 * (posuw mm/min / mm/obr), G96/G97 (stala predkosc skrawania / obroty),
 * F, S, M0-M9 (bez efektu), M2/M30 (koniec programu); inne slowa G/M sa
 * ignorowane i liczone w pasku stanu,
 * - petle jako podprogramy w stylu Fanuc: M98 P<O> L<ile> (albo skrot
 *   P<ile><OOOO>), podprogram "O<n>" ... M99 za M30, do CALL_DEPTH
 *   poziomow; wykonywane przy wczytywaniu (powtorzenia rozwijane w
 *   odcinki, limit MAX_EXEC linii na wypadek petli bez konca),
 * - zakladka Help ma dwie strony (widok/klawisze i lista slow G-code z
 *   opisem petli) i przelacznik English/Polski (tylko w pamieci),
 * - dwa tryby: frezowanie (rzut izometryczny albo z gory, X w prawo-dol,
 *   Y w prawo-gore, Z w gore) i toczenie (widok z boku: Z w prawo, X w
 *   gore, domyslnie G18; X jako srednica albo promien - Config); oba
 *   automatycznie dopasowane do okna,
 * - przy toczeniu material to walec (srednica i dlugosc w Config, czolo
 *   na Z0, w strone -Z); zebrany material liczony co klatke jako
 *   najmniejszy promien osiagniety przez ostrze w kazdej kolumnie ekranu
 *   (stala tablica, bez alokacji); przy frezowaniu sama sciezka,
 *   bez symulacji usuwania materialu,
 * - bez trybu podanego pliku: frezowanie edytuje ~/.7a/cnc.nc, toczenie
 *   ~/.7a/lathe.nc (kazdy tworzony z wlasnym przykladem),
 * - edycja w ZEWNETRZNYM edytorze w osobnym oknie terminala
 *   (zasoby 7aCnc.terminal, domyslnie "urxvt", i 7aCnc.editor, domyslnie
 *   $EDITOR albo "vi"); apka sama przeladowuje plik, gdy zmieni sie jego
 *   mtime (polling stat() co 500 ms - bez inotify/kqueue, przenosnie).
 * - "Run" animuje narzedzie wzdluz sciezki; czas symulacji liczony z
 *   rzeczywistego uplywu czasu (CLOCK_MONOTONIC) razy mnoznik predkosci,
 *   a czas kazdego odcinka z posuwu F (G1-G3) albo stalego RAPID_FEED (G0).
 * - "Step" (klawisz s) wykonuje program linia po linii: pauza i skok na
 *   koniec biezacej (albo, gdy skonczona, nastepnej) linii z ruchem; linie
 *   bez ruchu (np. G21 G90) sa przeskakiwane, ich efekt modalny i tak jest
 *   juz w sciezce.
 * - po prawej od widoku box informacyjny (boxInfoBackground) z kodem
 *   programu i podswietlona linia aktualnie wykonywana; podaza za nia
 *   (linia na srodku), kolko myszy przewija recznie do nastepnej zmiany
 *   linii. Tekst pliku trzymany w stalym buforze (SRC_CAP/SRC_LINES), przy
 *   wiekszym pliku reszta linii nie jest pokazywana.
 *
 * Sciezka to stala tablica odcinkow (MAX_SEGS) - luki dzielone sa na
 * krotkie odcinki przy wczytywaniu, wiec petla rysowania nie alokuje.
 *
 * Uzycie: 7acnc [plik.nc]   (domyslnie ~/.7a/cnc.nc albo ~/.7a/lathe.nc)
 */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../ui.h"
#include "icons/precision_manufacturing_48.xbm"

#define PROG        "7acnc"
#define DEF_FONT    "DejaVu Sans-10"
#define MAX_SEGS    32768
#define LINE_MAX_   512
#define RAPID_FEED  5000.0      /* mm/min dla G0 */
#define DEF_FEED    100.0       /* mm/min gdy F jeszcze nie podano */
#define ARC_STEP    (M_PI / 36) /* 5 stopni na odcinek luku */
#define FRAME_MS    16
#define POLL_MS     500
#define MARGIN      6
#define TAB_INDENT  8
#define MAX_GRID_LINES 200
#define MAX_COLS    4096        /* kolumny ekranu profilu materialu (toczenie) */
#define CSS_MAX_RPM 4000.0      /* limit obrotow przy G96 */

typedef struct {
    float a[3], b[3];   /* poczatek / koniec, mm */
    double t_end;       /* skumulowany czas symulacji na koncu odcinka, s */
    int   line;         /* numer linii w pliku */
    unsigned char rapid;
    unsigned short lead_um; /* skok gwintu G33 w um (0 = nie gwint); miesci
                             * sie w wyrownaniu, Seg nie rosnie */
} Seg;

typedef struct {
    Seg    seg[MAX_SEGS];
    int    n;
    int    truncated;   /* przekroczono MAX_SEGS */
    int    ignored;     /* pominiete slowa G/M spoza G0-G3 */
    int    errors;      /* linie z bledem (np. luk bez I/J/R) */
    double total_t;
    float  lo[3], hi[3];
    int    err_line;    /* linia pierwszego bledu (0 = brak) */
    char   err[384];    /* opis pierwszego bledu, wiersze rozdzielone '\n' */
} Path;

static Path g_path;     /* ~1.3 MB w BSS, nie na stosie */

/* Tekst programu do boxa z kodem: linie NUL-terminowane jedna za druga,
 * off[i] = poczatek linii i+1. Po przepelnieniu (full) kolejne linie
 * odrzucane, zeby numer linii nadal = indeks + 1. */
#define SRC_CAP   (256 * 1024)
#define SRC_LINES 16384
typedef struct {
    char   text[SRC_CAP];
    int    off[SRC_LINES];
    unsigned char too_long[SRC_LINES];   /* linia przycieta - nie wykonywana */
    int    n, full;
    size_t used;
} Source;

static Source g_src;    /* ~320 KB w BSS */
static volatile sig_atomic_t g_running = 1;
static double g_rapid_feed = RAPID_FEED;   /* zmieniany w zakladce Config */
static int g_lathe;      /* toczenie: domyslnie G18 (kopia Config.lathe) */
static int g_diameter;   /* toczenie: X w programie = srednica (Config.diameter) */

static const double speeds[] = { 1, 2, 5, 10, 20, 50, 100, 200 };
#define N_SPEEDS ((int)(sizeof speeds / sizeof speeds[0]))
#define SPEED_DEFAULT 3   /* x10 - Slower/Faster swieca, gdy speed od tego odbiega */

typedef struct {
    double sim_t;       /* aktualny czas symulacji, s */
    int    playing;
    int    speed;       /* indeks w speeds[] */
    int    cur;         /* indeks biezacego odcinka (cache wyszukiwania) */
    int    stepped;     /* byl Step - start, nawet gdy pierwsza linia trwa 0 s */
} Anim;

enum { ACT_NONE, ACT_EDIT, ACT_RUN, ACT_STEP, ACT_RESET, ACT_SLOWER, ACT_FASTER,
       ACT_RELOAD, ACT_SAVE, ACT_ERR_CLOSE, ACT_VIEW, ACT_LATHE, ACT_MODE };

typedef struct {
    XColor bg, label, rapid, cut, air, mat, done, tool, ax[3], grid, shadow, drop, led,
           stock;   /* stock = material walca (toczenie) */
    unsigned long pixels[16];
    int npix;
} Colors;

/* ── parser G-code ─────────────────────────────────────────────────── */

enum { MOT_RAPID, MOT_LINE, MOT_CW, MOT_CCW, MOT_THREAD };

typedef struct {
    double pos[3];      /* X zawsze jako promien */
    double feed;        /* F w mm: mm/min (G94) albo mm/obr (G95) */
    double unit;        /* 1.0 albo 25.4 */
    double spindle;     /* S: obr/min (G97) albo m/min (G96) */
    double lead;        /* skok gwintu G33 (K), mm/obr */
    int    absolute;
    int    motion;      /* MOT_* */
    int    plane;       /* 0 = G17 XY, 1 = G18 ZX, 2 = G19 YZ */
    int    per_rev;     /* G95 */
    int    css;         /* G96 */
    int    diam;        /* X jako srednica */
    int    ended;       /* M2/M30 - reszta pliku pomijana */
} Machine;

/* Osie plaszczyzny luku: u, v (kierunek przeciwny do zegara = G3 patrzac
 * od strony dodatniej trzeciej osi) i os liniowa w (helisa). */
static const int plane_ax[3][3] = { { 0, 1, 2 }, { 2, 0, 1 }, { 1, 2, 0 } };

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* Liczy blad; tresc zapamietuje tylko dla pierwszego (pokazywany w popupie). */
static void path_err(Path *p, int line, const char *fmt, ...)
{
    if (p->errors++ > 0) return;
    p->err_line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
}

static void path_add(Path *p, const double a[3], const double b[3],
                     int rapid, double feed, int line)
{
    if (p->n >= MAX_SEGS) { p->truncated = 1; return; }
    Seg *s = &p->seg[p->n++];
    double d2 = 0;
    for (int i = 0; i < 3; i++) {
        s->a[i] = (float)a[i];
        s->b[i] = (float)b[i];
        d2 += (b[i] - a[i]) * (b[i] - a[i]);
        if (b[i] < p->lo[i]) p->lo[i] = (float)b[i];
        if (b[i] > p->hi[i]) p->hi[i] = (float)b[i];
    }
    double f = rapid ? g_rapid_feed : feed;
    p->total_t += sqrt(d2) / f * 60.0;
    s->t_end = p->total_t;
    s->rapid = (unsigned char)rapid;
    s->lead_um = 0;
    s->line  = line;
}

/* G4: postoj w miejscu - odcinek zerowej dlugosci z samym czasem. */
static void path_dwell(Path *p, const double pos[3], double secs, int line)
{
    if (p->n >= MAX_SEGS) { p->truncated = 1; return; }
    Seg *s = &p->seg[p->n++];
    for (int i = 0; i < 3; i++) s->a[i] = s->b[i] = (float)pos[i];
    p->total_t += secs > 0 ? secs : 0;
    s->t_end = p->total_t;
    s->rapid = 0;
    s->lead_um = 0;
    s->line  = line;
}

/* Luk w plaszczyznie pl (patrz plane_ax), os w liniowo (helisa).
 * cu/cv = srodek wzgledem startu; has_r -> format R. Zwraca 0 przy
 * niepoprawnej geometrii. */
static int path_arc(Path *p, const double st[3], const double en[3],
                    int cw, int pl, int has_c, double cu, double cv,
                    int has_r, double r, double feed, int line)
{
    const int u = plane_ax[pl][0], v = plane_ax[pl][1], w = plane_ax[pl][2];
    const char au = "XYZ"[u], av = "XYZ"[v];
    double cx, cy;
    if (has_c) {
        cx = st[u] + cu;
        cy = st[v] + cv;
    } else if (has_r) {
        double dx = en[u] - st[u], dy = en[v] - st[v];
        double d = sqrt(dx * dx + dy * dy);
        if (d < 1e-9) {
            path_err(p, line, "G%d with R cannot make a full circle\n"
                     "(start point = end point).\n"
                     "Use I/J (center offset) instead of R.", cw ? 2 : 3);
            return 0;
        }
        if (fabs(r) < d / 2 - 0.01) {
            path_err(p, line, "R%.3f is too small: the distance from start\n"
                     "to end is %.3f mm, so R must be at least %.3f.\n"
                     "Check the end point (G91: X/Y are relative).",
                     r, d, d / 2);
            return 0;
        }
        double h2 = r * r - d * d / 4;
        double h = h2 > 0 ? sqrt(h2) : 0;
        /* G3 i R>0: srodek po lewej cieciwy; G2 albo R<0 odwraca strone */
        double side = (cw ? -1 : 1) * (r < 0 ? -1 : 1);
        cx = (st[u] + en[u]) / 2 - side * h * dy / d;
        cy = (st[v] + en[v]) / 2 + side * h * dx / d;
    } else {
        path_err(p, line, "G%d without center offset or R: the arc center\n"
                 "is unknown. Add %c/%c (center offset from start)\n"
                 "or R (radius).", cw ? 2 : 3, "IJK"[u], "IJK"[v]);
        return 0;
    }

    double rad = hypot(st[u] - cx, st[v] - cy);
    /* jak sterownik: promien do konca musi zgadzac sie z promieniem do
     * startu (tolerancja 0.01 mm albo 0.1% promienia, co wieksze) */
    double rad_end = hypot(en[u] - cx, en[v] - cy);
    if (fabs(rad_end - rad) > fmax(0.01, 0.001 * rad)) {
        path_err(p, line, "Arc end point does not lie on the circle.\n"
                 "Center %c%.3f %c%.3f: radius to start %.3f mm,\n"
                 "radius to end %.3f mm (start %c%.3f %c%.3f, end %c%.3f %c%.3f).\n"
                 "In G91 coordinates are relative: a full circle is 0 0.",
                 au, cx, av, cy, rad, rad_end, au, st[u], av, st[v],
                 au, en[u], av, en[v]);
        return 0;
    }
    double a0 = atan2(st[v] - cy, st[u] - cx);
    double a1 = atan2(en[v] - cy, en[u] - cx);
    double sweep = a1 - a0;
    if (cw)  { if (sweep >= -1e-9) sweep -= 2 * M_PI; }
    else     { if (sweep <=  1e-9) sweep += 2 * M_PI; }

    int n = (int)ceil(fabs(sweep) / ARC_STEP);
    if (n < 1) n = 1;
    double prev[3] = { st[0], st[1], st[2] };
    for (int k = 1; k <= n; k++) {
        double t = (double)k / n, pt[3];
        if (k == n) {
            pt[u] = en[u]; pt[v] = en[v];
        } else {
            pt[u] = cx + rad * cos(a0 + sweep * t);
            pt[v] = cy + rad * sin(a0 + sweep * t);
        }
        pt[w] = st[w] + (en[w] - st[w]) * t;
        path_add(p, prev, pt, 0, feed, line);
        memcpy(prev, pt, sizeof prev);
    }
    return 1;
}

/* Usuwa komentarze "( ... )" i "; ..." w miejscu. */
static void strip_comments(char *s)
{
    char *w = s;
    int depth = 0;
    for (char *r = s; *r; r++) {
        if (*r == ';' && depth == 0) break;
        if (*r == '(') { depth++; continue; }
        if (*r == ')') { if (depth > 0) depth--; continue; }
        if (depth == 0) *w++ = *r;
    }
    *w = '\0';
}

/* Obroty wrzeciona przy promieniu r (mm): S przy G97, z predkosci
 * skrawania S (m/min) przy G96, ograniczone CSS_MAX_RPM. */
static double spindle_rpm(const Machine *m, double r)
{
    if (!m->css) return m->spindle;
    r = fabs(r);
    if (r < 1e-6) return CSS_MAX_RPM;
    double rpm = m->spindle * 1000.0 / (2 * M_PI * r);
    return rpm < CSS_MAX_RPM ? rpm : CSS_MAX_RPM;
}

/* Posuw w mm/min: F wprost (G94) albo F * obroty (G95, G33 ze skokiem). */
static double feed_mm_min(Path *p, const Machine *m, double per_rev, double f,
                          int line)
{
    if (!per_rev) return f;
    double rpm = spindle_rpm(m, m->pos[0]);
    if (rpm <= 0) {
        path_err(p, line, "Feed per revolution (G95/G33) needs a spindle\n"
                 "speed: add S (rpm with G97, m/min with G96).\n"
                 "Using %g mm/min instead.", DEF_FEED);
        return DEF_FEED;
    }
    return f * rpm;
}

/* Slowo G (wartosc*10, G17.1 -> 171). Zwraca 0 dla nieobslugiwanych. */
static int parse_g(Machine *m, int g, int *motion, int *home, int *dwell)
{
    switch (g) {
    case 0: case 10: case 20: case 30: *motion = g / 10; break;
    case 40:  *dwell = 1; break;
    case 170: m->plane = 0; break;
    case 180: m->plane = 1; break;
    case 190: m->plane = 2; break;
    case 280: *home = 1; break;
    case 330: *motion = MOT_THREAD; break;
    case 200: case 700: m->unit = 25.4; break;
    case 210: case 710: m->unit = 1.0;  break;
    case 900: m->absolute = 1; break;
    case 910: m->absolute = 0; break;
    case 940: m->per_rev = 0; break;
    case 950: m->per_rev = 1; break;
    case 960: m->css = 1; break;
    case 970: m->css = 0; break;
    default:  return 0;
    }
    return 1;
}

/* Sterowanie przebiegiem z linii: M98 (wywolanie podprogramu) / M99 (powrot). */
enum { CTL_NONE, CTL_CALL, CTL_RET };

typedef struct {
    int kind;    /* CTL_* */
    long prog;   /* M98: numer podprogramu O */
    long count;  /* M98: liczba powtorzen L */
} Ctl;

/* M98 P.. L..: bez L i z P powyzej 4 cyfr (Fanuc) P51000 = 5 x O1000. */
static void parse_call(Path *p, Ctl *ctl, const int has[26], const double v[26], int line)
{
    if (!has['P' - 'A']) {
        path_err(p, line, "M98 needs the subprogram number P,\n"
                 "e.g. M98 P100 L3 (call O100 three times).");
        ctl->kind = CTL_NONE;
        return;
    }
    long pn = lround(v['P' - 'A']);
    ctl->prog = pn;
    ctl->count = 1;
    if (has['L' - 'A']) ctl->count = lround(v['L' - 'A']);
    else if (pn > 9999) { ctl->prog = pn % 10000; ctl->count = pn / 10000; }
}

static void parse_line(Path *p, Machine *m, char *s, int line, Ctl *ctl)
{
    double v[26] = {0};
    int has[26] = {0};
    int motion = -1, home = 0, dwell = 0;

    ctl->kind = CTL_NONE;
    if (m->ended) return;
    strip_comments(s);
    for (char *c = s; *c; ) {
        if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') { c++; continue; }
        if (*c == '%' && c == s) return;   /* znacznik poczatku programu */
        int L = (*c >= 'a' && *c <= 'z') ? *c - 'a'
              : (*c >= 'A' && *c <= 'Z') ? *c - 'A' : -1;
        if (L < 0) {
            path_err(p, line, "Unexpected character '%c'.\n"
                     "A G-code word is a letter followed by a number,\n"
                     "e.g. G1 X10.5 F300.", *c);
            return;
        }
        char *end;
        double val = strtod(c + 1, &end);
        if (end == c + 1) {
            path_err(p, line, "Letter '%c' has no number after it.\n"
                     "Every word needs a value, e.g. %c0.", *c, toupper((unsigned char)*c));
            return;
        }
        c = end;
        if (L == 'G' - 'A') {
            if (!parse_g(m, (int)lround(val * 10), &motion, &home, &dwell))
                p->ignored++;
        } else if (L == 'M' - 'A') {
            int mc = (int)lround(val);
            if (mc == 2 || mc == 30) m->ended = 1;
            else if (mc == 98) ctl->kind = CTL_CALL;
            else if (mc == 99) ctl->kind = CTL_RET;
            else if (mc < 0 || mc > 9) p->ignored++;   /* M0-M9: bez efektu */
        } else {
            v[L] = val;
            has[L] = 1;
        }
    }
    if (ctl->kind == CTL_CALL) parse_call(p, ctl, has, v, line);
    if (ctl->kind != CTL_NONE) has['P' - 'A'] = has['L' - 'A'] = 0;   /* nie G4 P */

    if (dwell) {   /* G4 X/F = sekundy, P = milisekundy; bez ruchu */
        double secs = has['X' - 'A'] ? v['X' - 'A']
                    : has['F' - 'A'] ? v['F' - 'A']
                    : has['P' - 'A'] ? v['P' - 'A'] / 1000.0 : 0;
        path_dwell(p, m->pos, secs, line);
        return;
    }
    if (has['F' - 'A'] && v['F' - 'A'] > 0)
        m->feed = v['F' - 'A'] * m->unit;
    if (has['S' - 'A'] && v['S' - 'A'] >= 0)
        m->spindle = v['S' - 'A'];
    if (has['K' - 'A'] && motion == MOT_THREAD)
        m->lead = fabs(v['K' - 'A']) * m->unit;
    if (motion >= 0) m->motion = motion;

    static const int axis[3] = { 'X' - 'A', 'Y' - 'A', 'Z' - 'A' };
    int moved = 0;
    double end[3];
    for (int i = 0; i < 3; i++) {
        end[i] = m->pos[i];
        if (has[axis[i]]) {
            double d = v[axis[i]] * m->unit;
            if (i == 0 && m->diam) d /= 2;   /* X jako srednica */
            end[i] = m->absolute ? d : m->pos[i] + d;
            moved = 1;
        }
    }
    if (home) {
        /* G28: rapid do punktu posredniego (osie z linii, G90/G91 jak zwykle),
         * potem rapid do home = zero programu; bez osi - wszystkie do home.
         * Tryb modalny ruchu bez zmian. */
        double h[3];
        for (int i = 0; i < 3; i++)
            h[i] = (moved && !has[axis[i]]) ? end[i] : 0;
        if (memcmp(m->pos, end, sizeof end) != 0)
            path_add(p, m->pos, end, 1, m->feed, line);
        if (memcmp(end, h, sizeof h) != 0)
            path_add(p, end, h, 1, m->feed, line);
        memcpy(m->pos, h, sizeof h);
        return;
    }

    const int pu = plane_ax[m->plane][0], pv = plane_ax[m->plane][1];
    const int ou = 'I' - 'A' + pu, ov = 'I' - 'A' + pv;   /* I/J/K = X/Y/Z */
    int has_c = has[ou] || has[ov];
    if (!moved && !(m->motion >= MOT_CW && m->motion <= MOT_CCW && has_c)) return;

    if (m->motion == MOT_THREAD) {
        if (m->lead <= 0) {
            path_err(p, line, "G33 needs the thread lead K (mm per revolution),\n"
                     "e.g. G33 Z-20 K1.5.");
            return;
        }
        int n = p->n;
        path_add(p, m->pos, end, 0, feed_mm_min(p, m, 1, m->lead, line), line);
        if (p->n > n) {
            double um = m->lead * 1000.0;
            p->seg[n].lead_um = (unsigned short)(um > 65535 ? 65535 : um < 1 ? 1 : um);
        }
    } else if (m->motion <= MOT_LINE) {
        path_add(p, m->pos, end, m->motion == MOT_RAPID,
                 feed_mm_min(p, m, m->motion == MOT_LINE && m->per_rev, m->feed, line),
                 line);
    } else {
        double cu = has[ou] ? v[ou] * m->unit : 0;
        double cv = has[ov] ? v[ov] * m->unit : 0;
        double r = has['R' - 'A'] ? v['R' - 'A'] * m->unit : 0;
        if (!path_arc(p, m->pos, end, m->motion == MOT_CW, m->plane, has_c, cu, cv,
                      has['R' - 'A'], r, feed_mm_min(p, m, m->per_rev, m->feed, line),
                      line))
            return;   /* path_arc juz zapisal blad */
    }
    memcpy(m->pos, end, sizeof end);
}

static void src_add(Source *s, const char *line)
{
    size_t len = strcspn(line, "\r\n");
    if (s->full || s->n >= SRC_LINES || s->used + len + 1 > sizeof s->text) {
        s->full = 1;
        return;
    }
    char *d = s->text + s->used;
    for (size_t i = 0; i < len; i++) d[i] = line[i] == '\t' ? ' ' : line[i];
    d[len] = '\0';
    s->too_long[s->n] = 0;
    s->off[s->n++] = (int)s->used;
    s->used += len + 1;
}

/* Linia ln (od 1) albo "" poza zapamietanym zakresem. */
static const char *src_line(const Source *s, int ln)
{
    return ln >= 1 && ln <= s->n ? s->text + s->off[ln - 1] : "";
}

/* Linia z etykieta podprogramu "O<numer>" (spacje i N.. przed nia dozwolone);
 * zwraca numer albo -1. */
static long src_label(const char *t)
{
    while (*t == ' ') t++;
    if (*t == 'N' || *t == 'n') {
        t++;
        while (isdigit((unsigned char)*t)) t++;
        while (*t == ' ') t++;
    }
    if ((*t != 'O' && *t != 'o') || !isdigit((unsigned char)t[1])) return -1;
    return strtol(t + 1, NULL, 10);
}

static int src_find_label(const Source *s, long prog)
{
    for (int ln = 1; ln <= s->n; ln++)
        if (src_label(src_line(s, ln)) == prog) return ln;
    return 0;
}

/* Wczytanie pliku do g_src (tekst do boxa z kodem i do wykonania). */
static int src_load(Path *p, const char *file)
{
    FILE *f = fopen(file, "r");
    g_src.n = g_src.full = 0;
    g_src.used = 0;
    if (!f) return 0;
    char buf[LINE_MAX_];
    int line = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        size_t len = strlen(buf);
        src_add(&g_src, buf);
        if (len == sizeof buf - 1 && buf[len - 1] != '\n') {
            int ch;  /* za dluga linia - odrzuc reszte */
            while ((ch = fgetc(f)) != EOF && ch != '\n') ;
            if (line <= g_src.n) g_src.too_long[line - 1] = 1;
            path_err(p, line, "Line is longer than %d characters\n"
                     "and was skipped.", LINE_MAX_ - 2);
        }
    }
    fclose(f);
    if (g_src.full)
        path_err(p, g_src.n + 1, "The file is too large: only the first %d lines\n"
                 "(max %d lines, %d KB of text) are simulated.",
                 g_src.n, SRC_LINES, SRC_CAP / 1024);
    return 1;
}

#define CALL_DEPTH 8          /* zagniezdzenie M98 */
#define MAX_EXEC   1000000    /* limit wykonanych linii (petla bez konca) */

typedef struct {
    int  ret;     /* linia po M98 */
    int  body;    /* pierwsza linia podprogramu (po O..) */
    long left;    /* pozostale powtorzenia */
} Frame;

/* M98: skok do podprogramu; zwraca linie, od ktorej wykonywac dalej. */
static int exec_call(Path *p, Frame *st, int *depth, const Ctl *c, int ln)
{
    int lbl = src_find_label(&g_src, c->prog);
    if (!lbl) {
        path_err(p, ln, "M98: subprogram O%ld not found.\n"
                 "Put it after M30 as a line \"O%ld\" ... M99.", c->prog, c->prog);
        return ln + 1;
    }
    if (*depth >= CALL_DEPTH) {
        path_err(p, ln, "M98: subprograms nested deeper than %d levels\n"
                 "(does O%ld call itself?).", CALL_DEPTH, c->prog);
        return ln + 1;
    }
    if (c->count < 1) return ln + 1;   /* L0: bez wywolania */
    st[*depth] = (Frame){ ln + 1, lbl + 1, c->count };
    (*depth)++;
    return lbl + 1;
}

/* M99: kolejne powtorzenie albo powrot za M98; 0 = koniec programu
 * (M99 w programie glownym - na maszynie petla bez konca). */
static int exec_ret(Frame *st, int *depth)
{
    if (*depth == 0) return 0;
    Frame *f = &st[*depth - 1];
    if (--f->left > 0) return f->body;
    (*depth)--;
    return f->ret;
}

/* Wykonuje program z g_src: od linii 1 do M2/M30, M98/M99 skacza po
 * liniach, wiec odcinki petli maja numery linii podprogramu. */
static void path_exec(Path *p)
{
    Machine m = { .feed = DEF_FEED, .unit = 1.0, .absolute = 1, .motion = MOT_RAPID,
                  .plane = g_lathe ? 1 : 0, .diam = g_lathe && g_diameter };
    Frame st[CALL_DEPTH];
    int depth = 0;
    long steps = 0;
    char buf[LINE_MAX_];
    for (int ln = 1; ln >= 1 && ln <= g_src.n && !m.ended && !p->truncated; ) {
        if (++steps > MAX_EXEC) {
            path_err(p, ln, "Stopped after %d executed lines:\n"
                     "endless loop? Check M98 L counts and M99.", MAX_EXEC);
            return;
        }
        if (g_src.too_long[ln - 1]) { ln++; continue; }
        Ctl c;
        snprintf(buf, sizeof buf, "%s", src_line(&g_src, ln));
        if (src_label(buf) >= 0) buf[0] = '\0';   /* sama etykieta O.. */
        parse_line(p, &m, buf, ln, &c);
        if (c.kind == CTL_CALL)     ln = exec_call(p, st, &depth, &c, ln);
        else if (c.kind == CTL_RET) ln = exec_ret(st, &depth);
        else                        ln++;
    }
}

static int path_load(Path *p, const char *file)
{
    memset(p->lo, 0, sizeof p->lo);  /* zakres zawsze obejmuje zero maszyny */
    memset(p->hi, 0, sizeof p->hi);
    p->n = p->truncated = p->ignored = p->errors = p->err_line = 0;
    p->err[0] = '\0';
    p->total_t = 0;
    if (!src_load(p, file)) return 0;
    path_exec(p);
    return 1;
}

/* ── animacja ──────────────────────────────────────────────────────── */

/* Indeks odcinka zawierajacego czas t (wyszukiwanie binarne po t_end). */
static int seg_at(const Path *p, double t)
{
    int lo = 0, hi = p->n - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (p->seg[mid].t_end < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void tool_pos(const Path *p, const Anim *an, double out[3])
{
    if (p->n == 0) { out[0] = out[1] = out[2] = 0; return; }
    const Seg *s = &p->seg[an->cur];
    double t0 = an->cur > 0 ? p->seg[an->cur - 1].t_end : 0;
    double span = s->t_end - t0;
    double f = span > 0 ? (an->sim_t - t0) / span : 1;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    for (int i = 0; i < 3; i++)
        out[i] = s->a[i] + (s->b[i] - s->a[i]) * f;
}

static void anim_advance(Anim *an, const Path *p, double dt)
{
    an->sim_t += dt * speeds[an->speed];
    if (an->sim_t >= p->total_t) {
        an->sim_t = p->total_t;
        an->playing = 0;
    }
    an->cur = p->n > 0 ? seg_at(p, an->sim_t) : 0;
}

static int anim_started(const Anim *an)
{
    return an->sim_t > 0 || an->playing || an->stepped;
}

/* Step: pauza i narzedzie na koniec biezacej linii programu albo - gdy jest
 * juz skonczona - nastepnej (wszystkie odcinki luku maja ten sam numer). */
static void anim_step(Anim *an, const Path *p)
{
    if (p->n == 0) return;
    int k = an->cur;
    an->playing = 0;
    if (anim_started(an) && an->sim_t >= p->seg[k].t_end) {
        int done = p->seg[k].line;
        while (k < p->n && p->seg[k].line == done) k++;
        if (k >= p->n) return;   /* koniec programu */
    }
    int line = p->seg[k].line;
    while (k + 1 < p->n && p->seg[k + 1].line == line) k++;
    an->stepped = 1;
    an->cur = k;
    an->sim_t = p->seg[k].t_end;
}

/* ── rzuty: izometria / z gory / z boku (toczenie) ─────────────────── */

enum { VIEW_ISO, VIEW_TOP, VIEW_SIDE };

typedef struct {
    double scale, ox, oy;   /* ekran = o + (u, -v) * scale */
    int    mode;            /* VIEW_*: TOP u = X, v = Y; SIDE u = Z, v = X */
} View;

#define ISO_C 0.8660254037844386   /* cos 30 */
#define ISO_S 0.5                  /* sin 30 */

static void iso(const View *vw, const double p[3], double *u, double *v)
{
    switch (vw->mode) {
    case VIEW_TOP:  *u = p[0]; *v = p[1]; return;
    case VIEW_SIDE: *u = p[2]; *v = p[0]; return;
    }
    *u = (p[0] + p[1]) * ISO_C;
    *v = (p[1] - p[0]) * ISO_S + p[2];
}

/* Dopasowuje rzut bryly [lo, hi] (+ osie o dlugosci axis_len) do r;
 * vw->mode musi byc ustawione wczesniej. */
static void view_fit(View *vw, const double lo[3], const double hi[3],
                     UiRect r, double axis_len)
{
    double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30;
    for (int c = 0; c < 8; c++) {   /* 8 naroznikow bryly ograniczajacej */
        double q[3], u, v;
        for (int i = 0; i < 3; i++) {
            double h = hi[i] < axis_len ? axis_len : hi[i];
            q[i] = (c >> i) & 1 ? h : lo[i];
        }
        iso(vw, q, &u, &v);
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    double du = umax - umin, dv = vmax - vmin;
    if (du < 1e-6) du = 1;
    if (dv < 1e-6) dv = 1;
    double sx = r.w * 0.9 / du, sy = r.h * 0.9 / dv;
    vw->scale = sx < sy ? sx : sy;
    vw->ox = r.x + r.w / 2.0 - (umin + umax) / 2 * vw->scale;
    vw->oy = r.y + r.h / 2.0 + (vmin + vmax) / 2 * vw->scale;
}

static void project(const View *vw, const double p[3], int *x, int *y)
{
    double u, v;
    iso(vw, p, &u, &v);
    *x = (int)lround(vw->ox + u * vw->scale);
    *y = (int)lround(vw->oy - v * vw->scale);
}

static void projectf(const View *vw, const float pf[3], int *x, int *y)
{
    double p[3] = { pf[0], pf[1], pf[2] };
    project(vw, p, x, y);
}

/* ── siatka ────────────────────────────────────────────────────────── */

typedef struct {
    double x0, x1, y0, y1, step;
    int    ia, ib;      /* osie siatki: XY (0, 1) albo przy toczeniu ZX (2, 0) */
} Grid;

/* Siatka w plaszczyznie osi ia/ib (trzecia os = 0) obejmujaca [lo, hi],
 * krawedzie na wielokrotnosciach kroku. Gdy linii byloby za duzo, krok jest
 * podwajany (tylko na potrzeby rysowania). Zwraca 0 przy zlym kroku. */
static int grid_fit(Grid *g, const double lo[3], const double hi[3], double step)
{
    if (!(step > 0) || !isfinite(step)) return 0;
    for (int tries = 0; tries < 40; tries++, step *= 2) {
        g->x0 = floor(lo[g->ia] / step) * step;
        g->x1 = ceil(hi[g->ia] / step) * step;
        g->y0 = floor(lo[g->ib] / step) * step;
        g->y1 = ceil(hi[g->ib] / step) * step;
        if (g->x1 - g->x0 < step) g->x1 = g->x0 + step;
        if (g->y1 - g->y0 < step) g->y1 = g->y0 + step;
        if ((g->x1 - g->x0) / step <= MAX_GRID_LINES
            && (g->y1 - g->y0) / step <= MAX_GRID_LINES) {
            g->step = step;
            return 1;
        }
    }
    return 0;
}

/* Podpis wartosci (mm) przy punkcie p rzutu; dx < 0 -> tekst konczy sie
 * przed punktem, dx > 0 -> zaczyna za nim. */
static void grid_label(UiCtx *ui, const View *vw, const double p[3], double val,
                       int dx, const XColor *c)
{
    char s[32];
    int x, y;
    snprintf(s, sizeof s, "%g", val);
    project(vw, p, &x, &y);
    int tw = ui_text_width(ui, s) + 2;
    UiRect r = { dx < 0 ? x + dx - tw : x + dx, y, tw, ui_line_height(ui) };
    ui_label_fg(ui, r, s, c);
}

/* Odcinek siatki: os ia = a, os ib od b0 do b1 (albo odwrotnie przy swap). */
static void grid_pts(const Grid *g, int swap, double a, double b0, double b1,
                     double p0[3], double p1[3])
{
    memset(p0, 0, 3 * sizeof *p0);
    memset(p1, 0, 3 * sizeof *p1);
    p0[swap ? g->ib : g->ia] = p1[swap ? g->ib : g->ia] = a;
    p0[swap ? g->ia : g->ib] = b0;
    p1[swap ? g->ia : g->ib] = b1;
}

/* Siatka + podzialka w mm: wartosci osi ia wzdluz krawedzi y0, wartosci
 * osi ib wzdluz krawedzi x1. Podpisywana co n-ta linia, tak by napisy na
 * siebie nie wchodzily (odstep linii na ekranie = step * scale). */
static void draw_grid(UiCtx *ui, const View *vw, const Grid *g,
                      const XColor *c, const XColor *lc)
{
    long nx = lround((g->x1 - g->x0) / g->step);
    long ny = lround((g->y1 - g->y0) / g->step);
    char wide[32];
    snprintf(wide, sizeof wide, "%g", -fmax(fmax(fabs(g->x0), fabs(g->x1)),
                                            fmax(fabs(g->y0), fabs(g->y1))));
    double px = g->step * vw->scale;
    long every = px > 0 ? (long)ceil((ui_text_width(ui, wide) + 12) / px) : 1;
    if (every < 1) every = 1;

    for (int swap = 0; swap < 2; swap++) {
        long n = swap ? ny : nx;
        for (long k = 0; k <= n; k++) {
            double val = (swap ? g->y0 : g->x0) + k * g->step, a[3], b[3];
            if (swap) grid_pts(g, 1, val, g->x0, g->x1, a, b);
            else      grid_pts(g, 0, val, g->y0, g->y1, a, b);
            int x0, y0, x1, y1;
            project(vw, a, &x0, &y0);
            project(vw, b, &x1, &y1);
            ui_draw_line(ui, x0, y0, x1, y1, 1, c);
            if (k % every == 0) grid_label(ui, vw, swap ? b : a, val, swap ? 4 : -4, lc);
        }
    }
}

/* ── rysowanie ─────────────────────────────────────────────────────── */

typedef struct {
    int  show_grid, show_rapid, show_axes;   /* stan ui_checkbox */
    int  show_shadow;  /* cien sciezki na Z=0 */
    int  show_drops;   /* pionowe kreski od punktow do Z=0 + podpis Z */
    int  show_air;     /* kolor ciecia wg Z: nad 0 = powietrze, pod 0 = material */
    int  top_view;     /* 0 = izometria, 1 = widok z gory (XY) */
    int  lathe;        /* toczenie: widok z boku (ZX), material = walec */
    int  diameter;     /* toczenie: X w programie to srednica */
    char step[16], rapid[16], term[256], editor[256];
    char stock_d[16], stock_l[16];   /* walec: srednica i dlugosc (mm) */
    int  step_cur, rapid_cur, term_cur, editor_cur, stock_d_cur, stock_l_cur;
    char path[1024];   /* ~/.7a/cnc.conf */
    char msg[1100];    /* wynik ostatniego Save */
} Config;

#define FILE_CAP 1024

typedef struct {
    UiCtx      *ui;
    Colors      col;
    Path       *p;
    Anim        an;
    Config      cfg;
    char       *file;       /* bufor main, FILE_CAP bajtow */
    int         own_file;   /* bez argumentu: plik zalezy od trybu */
    time_t      mtime;      /* wersja pliku z ostatniego wczytania */
    int         loaded, tab, w, h;
    int         err_open;   /* popup z pierwszym bledem parsera */
    UiRect      code_r;     /* box z kodem z ostatniej klatki (w = 0: ukryty) */
    int         code_top;   /* pierwsza widoczna linia (od 1) */
    int         code_follow; /* linia, do ktorej ostatnio przewinieto */
    int         help_page;  /* Help: 0 = widok i klawisze, 1 = G-code */
    int         help_pl;    /* Help po polsku */
} App;

static void draw_axes(UiCtx *ui, const View *vw, const Colors *col, double len)
{
    static const char *names[3] = { "X", "Y", "Z" };
    double o[3] = { 0, 0, 0 };
    int x0, y0;
    project(vw, o, &x0, &y0);
    /* z gory os Z, z boku os Y zapada sie w punkt */
    int skip = vw->mode == VIEW_TOP ? 2 : vw->mode == VIEW_SIDE ? 1 : -1;
    for (int i = 0; i < 3; i++) {
        if (i == skip) continue;
        double e[3] = { 0, 0, 0 };
        int x1, y1;
        e[i] = len;
        project(vw, e, &x1, &y1);
        ui_draw_line(ui, x0, y0, x1, y1, 1, &col->ax[i]);
        UiRect lr = { x1 + 3, y1 - ui_line_height(ui) / 2,
                      ui_text_width(ui, names[i]) + 2, ui_line_height(ui) };
        ui_label_fg(ui, lr, names[i], &col->ax[i]);
    }
}

#define Z_EPS 1e-6

/* Kolor nieprzebytego odcinka: G0; przy show_air ciecie w materiale (choc
 * jeden koniec pod Z=0, wiec wjazd w material tez) albo w powietrzu (oba
 * konce nad Z=0); w pozostalych przypadkach (po Z=0) zwykle ciecie.
 * Przy toczeniu (sr = promien walca > 0) material to wnetrze walca:
 * |X| < sr i Z < 0. */
static int in_stock(const float q[3], double sr)
{
    return fabs(q[0]) < sr - Z_EPS && q[2] < -Z_EPS;
}

static const XColor *seg_color(const Colors *col, const Seg *s, int show_air, double sr)
{
    if (s->rapid) return &col->rapid;
    if (show_air && sr > 0)
        return in_stock(s->a, sr) || in_stock(s->b, sr) ? &col->mat : &col->air;
    if (show_air) {
        if (s->a[2] < -Z_EPS || s->b[2] < -Z_EPS) return &col->mat;
        if (s->a[2] > Z_EPS && s->b[2] > Z_EPS) return &col->air;
    }
    return &col->cut;
}

/* Rzut sciezki na plaszczyzne Z=0 - pokazuje, gdzie figura lezy w XY. */
static void draw_shadow(UiCtx *ui, const View *vw, const Colors *col,
                        const Path *p, int show_rapid)
{
    for (int k = 0; k < p->n; k++) {
        const Seg *s = &p->seg[k];
        if (s->rapid && !show_rapid) continue;
        double a[3] = { s->a[0], s->a[1], 0 }, b[3] = { s->b[0], s->b[1], 0 };
        int x0, y0, x1, y1;
        project(vw, a, &x0, &y0);
        project(vw, b, &x1, &y1);
        ui_draw_line(ui, x0, y0, x1, y1, 1, &col->shadow);
    }
}

/* Pionowa kreska z punktu do Z=0 + podpis "Z..." przy punkcie. */
static void draw_drop(UiCtx *ui, const View *vw, const Colors *col, const float pf[3])
{
    double top[3] = { pf[0], pf[1], pf[2] }, base[3] = { pf[0], pf[1], 0 };
    int x0, y0, x1, y1;
    char lbl[32];
    project(vw, top, &x0, &y0);
    project(vw, base, &x1, &y1);
    ui_draw_line(ui, x0, y0, x1, y1, 1, &col->drop);
    snprintf(lbl, sizeof lbl, "Z%g", (double)pf[2]);
    UiRect lr = { x0 + 3, y0 - ui_line_height(ui), ui_text_width(ui, lbl) + 2,
                  ui_line_height(ui) };
    ui_label_fg(ui, lr, lbl, &col->drop);
}

/* Kreski do Z=0 w punktach poczatkowych linii programu (luki sa dzielone na
 * wiele odcinkow z tym samym numerem linii - dostaja jedna kreske) i w
 * punkcie koncowym. Pomija punkty na Z=0 i powtorzenia tego samego punktu. */
static void draw_drops(UiCtx *ui, const View *vw, const Colors *col, const Path *p)
{
    const float *prev = NULL;
    for (int k = 0; k <= p->n && p->n > 0; k++) {
        const float *pt;
        if (k == p->n) pt = p->seg[k - 1].b;
        else if (k == 0 || p->seg[k].line != p->seg[k - 1].line) pt = p->seg[k].a;
        else continue;
        if (fabs(pt[2]) <= Z_EPS) continue;
        if (prev && !memcmp(prev, pt, 3 * sizeof *pt)) continue;
        draw_drop(ui, vw, col, pt);
        prev = pt;
    }
}

/* Narzedzie: frez (kropka + trzonek w gore) albo przy toczeniu nozyk
 * (trojkatna plytka z ostrzem w punkcie narzedzia, oprawka w strone +X). */
static void draw_tool(UiCtx *ui, const Colors *col, int tx, int ty, int lathe)
{
    if (lathe) {
        ui_fill_triangle(ui, tx, ty, tx + 6, ty - 16, tx + 16, ty - 8, &col->tool);
        ui_draw_line(ui, tx + 11, ty - 12, tx + 11, ty - 40, 6, &col->tool);
        return;
    }
    ui_draw_line(ui, tx, ty, tx, ty - 30, 4, &col->tool);  /* trzonek */
    ui_fill_circle(ui, tx, ty, 4, &col->tool);
}

/* Punkt odcinka s w odleglosci d (mm, wzdluz Z od startu) odsuniety o h
 * promieniowo na zewnatrz (w strone rosnacego |X|), rzutowany na ekran. */
static void thread_pt(const View *vw, const Seg *s, double d, double h,
                      int *x, int *y)
{
    double dz = fabs(s->b[2] - s->a[2]), t = dz > 0 ? d / dz : 0, q[3];
    for (int i = 0; i < 3; i++) q[i] = s->a[i] + (s->b[i] - s->a[i]) * t;
    q[0] += q[0] < 0 ? -h : h;
    project(vw, q, x, y);
}

/* Gwint G33: ten sam profil V co w stock_cut - dno na linii ostrza co
 * skok, grzbiet 0.866 * skok wyzej, przy toczeniu (sr > 0) sciety do
 * promienia walca. Rysuje profil, ktory ostrze zostawia w materiale.
 * Przy skoku ponizej 3 px albo bez ruchu w Z zwykla gruba linia. */
static void draw_thread(UiCtx *ui, const View *vw, const Seg *s, double sr,
                        const XColor *c)
{
    double lead = s->lead_um / 1000.0, dz = fabs(s->b[2] - s->a[2]);
    double H = 0.866 * lead, z0[3] = { 0, 0, 0 }, z1[3] = { 0, 0, lead };
    int x0, y0, x1, y1;
    project(vw, z0, &x0, &y0);
    project(vw, z1, &x1, &y1);
    if (dz < lead || abs(x1 - x0) + abs(y1 - y0) < 3) {
        projectf(vw, s->a, &x0, &y0);
        projectf(vw, s->b, &x1, &y1);
        ui_draw_line(ui, x0, y0, x1, y1, 2, c);
        return;
    }
    int n = (int)(dz / lead), lx, ly, x, y;
    thread_pt(vw, s, 0, 0, &lx, &ly);
    for (int k = 0; k < n; k++) {
        double d = k * lead, h = H;
        if (sr > 0) {   /* grzbiet nie wyzej niz powierzchnia walca */
            double r = fabs(s->a[0] + (s->b[0] - s->a[0]) * (d + lead / 2) / dz);
            double cap = sr - r;
            if (cap < h) h = cap > 0 ? cap : 0;
        }
        double rise = h / H * lead / 2;   /* zbocze 60 st. do wysokosci h */
        double pts[3][2] = { { d + rise, h }, { d + lead - rise, h }, { d + lead, 0 } };
        for (int i = 0; i < 3; i++) {
            thread_pt(vw, s, pts[i][0], pts[i][1], &x, &y);
            ui_draw_line(ui, lx, ly, x, y, 1, c);
            lx = x; ly = y;
        }
    }
    projectf(vw, s->b, &x, &y);   /* reszta krotsza niz skok */
    ui_draw_line(ui, lx, ly, x, y, 1, c);
}

static void draw_path(UiCtx *ui, const View *vw, const Colors *col, const Path *p,
                      const Anim *an, int started, int show_rapid, int show_air,
                      double sr)
{
    double tp[3];
    tool_pos(p, an, tp);
    for (int k = 0; k < p->n; k++) {
        const Seg *s = &p->seg[k];
        if (s->rapid && !show_rapid) continue;
        int x0, y0, x1, y1;
        projectf(vw, s->a, &x0, &y0);
        projectf(vw, s->b, &x1, &y1);
        if (started && k == an->cur) {
            /* biezacy odcinek: przebyta czesc wyrozniona, reszta zwykla */
            int tx, ty;
            project(vw, tp, &tx, &ty);
            if (s->lead_um) draw_thread(ui, vw, s, sr, seg_color(col, s, show_air, sr));
            else ui_draw_line(ui, tx, ty, x1, y1, 1, seg_color(col, s, show_air, sr));
            ui_draw_line(ui, x0, y0, tx, ty, 2, &col->done);
        } else if (started && k < an->cur) {
            if (s->lead_um) draw_thread(ui, vw, s, sr, &col->done);
            else ui_draw_line(ui, x0, y0, x1, y1, s->rapid ? 1 : 2, &col->done);
        } else if (s->lead_um) {
            draw_thread(ui, vw, s, sr, seg_color(col, s, show_air, sr));
        } else {
            ui_draw_line(ui, x0, y0, x1, y1, 1, seg_color(col, s, show_air, sr));
        }
    }
    if (started) {
        int tx, ty;
        project(vw, tp, &tx, &ty);
        draw_tool(ui, col, tx, ty, sr > 0);
    }
}

/* ── toczenie: walec i zebrany material ────────────────────────────── */

static float g_rem[MAX_COLS];   /* pozostaly promien w kolumnie ekranu */

/* Odcinek ostrza od a do b (X = promien, Z wzdluz osi) zbiera material:
 * w kazdej kolumnie ekranu miedzy koncami promien walca maleje do |X|
 * ostrza (interpolowanego liniowo po kolumnach). c0 = kolumna czola
 * walca po lewej (x ekranu), nc = liczba kolumn walca. */
static void stock_cut(const View *vw, const double a[3], const double b[3],
                      double lead, int c0, int nc)
{
    int xa, xb, y;
    project(vw, a, &xa, &y);
    project(vw, b, &xb, &y);
    int lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
    if (hi < c0 || lo >= c0 + nc) return;
    for (int x = lo < c0 ? c0 : lo; x <= hi && x < c0 + nc; x++) {
        double t = xb != xa ? (double)(x - xa) / (xb - xa) : 0;
        float r = (float)fabs(a[0] + (b[0] - a[0]) * t);
        if (lead > 0) {
            /* gwint (G33): nozyk 60 st. zostawia profil V - dno bruzdy co
             * skok od Z startu odcinka, miedzy nimi grzbiet wyzej o
             * 0.866 * skok (ostry trojkat; min z g_rem przycina grzbiety) */
            double z = a[2] + (b[2] - a[2]) * t;
            double ph = fmod(fabs(z - a[2]) / lead, 1.0);
            r += (float)(0.866 * lead * (1 - fabs(2 * ph - 1)));
        }
        if (xb == xa) {   /* odcinek w jednej kolumnie: blizszy koniec osi */
            float rb = (float)fabs(b[0]);
            if (rb < r) r = rb;
        }
        if (r < g_rem[x - c0]) g_rem[x - c0] = r;
    }
}

/* Walec Z od -len do 0, promien sr; po starcie symulacji pokazuje tylko
 * material, ktorego ostrze jeszcze nie zebralo (przebyte odcinki + czesc
 * biezacego). Obrys walca przed obrobka zostaje cienka linia. */
static void draw_stock(UiCtx *ui, const View *vw, const Colors *col, const Path *p,
                       const Anim *an, int started, double sr, double len)
{
    double f0[3] = { sr, 0, -len }, f1[3] = { -sr, 0, 0 };
    int x0, yt, x1, yb;
    project(vw, f0, &x0, &yt);
    project(vw, f1, &x1, &yb);
    int nc = x1 - x0 + 1;
    if (nc <= 0 || yb <= yt) return;
    if (nc > MAX_COLS) nc = MAX_COLS;
    for (int i = 0; i < nc; i++) g_rem[i] = (float)sr;
    if (started && p->n > 0) {
        for (int k = 0; k < an->cur && k < p->n; k++) {
            double a[3], b[3];
            for (int i = 0; i < 3; i++) { a[i] = p->seg[k].a[i]; b[i] = p->seg[k].b[i]; }
            stock_cut(vw, a, b, p->seg[k].lead_um / 1000.0, x0, nc);
        }
        double a[3], tp[3];
        for (int i = 0; i < 3; i++) a[i] = p->seg[an->cur].a[i];
        tool_pos(p, an, tp);
        stock_cut(vw, a, tp, p->seg[an->cur].lead_um / 1000.0, x0, nc);
    }
    /* kolumny o tej samej wysokosci laczone w jeden prostokat */
    int run = 0, run_top = 0, run_bot = 0;
    for (int i = 0; i <= nc; i++) {
        int top = 0, bot = 0;
        if (i < nc) {
            double q[3] = { g_rem[i], 0, 0 }, w[3] = { -g_rem[i], 0, 0 };
            int xx;
            project(vw, q, &xx, &top);
            project(vw, w, &xx, &bot);
        }
        if (i == nc || top != run_top || bot != run_bot) {
            if (i > run && run_bot > run_top) {
                UiRect r = { x0 + run, run_top, i - run, run_bot - run_top };
                ui_fill_rect(ui, r, &col->stock);
            }
            run = i;
            run_top = top;
            run_bot = bot;
        }
    }
    UiRect outline = { x0, yt, nc, yb - yt };
    ui_draw_border(ui, outline, 1, &col->shadow);
}

static void fmt_time(char *buf, size_t sz, double s)
{
    long t = (long)(s + 0.5);
    snprintf(buf, sz, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}

/* Promien i dlugosc walca z Config; 0 gdy nie toczenie albo zle wartosci. */
static double stock_radius(const Config *c)
{
    double d = atof(c->stock_d);
    return c->lathe && d > 0 && atof(c->stock_l) > 0 ? d / 2 : 0;
}

static void draw_view(App *a, UiRect vr)
{
    const Path *p = a->p;
    const Config *c = &a->cfg;
    double sr = stock_radius(c), slen = sr > 0 ? atof(c->stock_l) : 0;
    double lo[3], hi[3], ext = 0;
    for (int i = 0; i < 3; i++) {
        lo[i] = p->lo[i];
        hi[i] = p->hi[i];
    }
    if (sr > 0) {   /* walec w calosci w kadrze */
        if (-sr < lo[0]) lo[0] = -sr;
        if (sr > hi[0])  hi[0] = sr;
        if (-slen < lo[2]) lo[2] = -slen;
        if (hi[2] < 0) hi[2] = 0;
    }
    for (int i = 0; i < 3; i++)
        if (hi[i] - lo[i] > ext) ext = hi[i] - lo[i];
    Grid g = { .ia = c->lathe ? 2 : 0, .ib = c->lathe ? 0 : 1 };
    int grid = c->show_grid && grid_fit(&g, lo, hi, atof(c->step));
    if (grid) {
        if (g.x0 < lo[g.ia]) lo[g.ia] = g.x0;
        if (g.x1 > hi[g.ia]) hi[g.ia] = g.x1;
        if (g.y0 < lo[g.ib]) lo[g.ib] = g.y0;
        if (g.y1 > hi[g.ib]) hi[g.ib] = g.y1;
    }
    double axis_len = c->show_axes ? (ext > 0 ? ext * 0.2 : 10) : 0;
    View vw = { 0, 0, 0, c->lathe ? VIEW_SIDE : c->top_view ? VIEW_TOP : VIEW_ISO };
    UiRect fr = vr;
    if (grid) {   /* miejsce na podpisy podzialki po bokach i u dolu */
        int in = ui_text_width(a->ui, "-0000.0") + 8;
        fr.x += in;
        fr.w -= 2 * in;
        fr.h -= ui_line_height(a->ui);
        if (fr.w < 10 || fr.h < 10) fr = vr;
    }
    view_fit(&vw, lo, hi, fr, axis_len);
    int started = anim_started(&a->an);
    ui_fill_rect(a->ui, vr, &a->col.bg);
    ui_set_clip(a->ui, vr);
    if (grid) draw_grid(a->ui, &vw, &g, &a->col.grid, &a->col.label);
    if (sr > 0) draw_stock(a->ui, &vw, &a->col, p, &a->an, started, sr, slen);
    if (c->show_axes) draw_axes(a->ui, &vw, &a->col, axis_len);
    /* cien i kreski Z maja sens tylko w izometrii - z gory pokrywaja sciezke */
    if (vw.mode == VIEW_ISO && c->show_shadow)
        draw_shadow(a->ui, &vw, &a->col, p, c->show_rapid);
    if (vw.mode == VIEW_ISO && c->show_drops) draw_drops(a->ui, &vw, &a->col, p);
    draw_path(a->ui, &vw, &a->col, p, &a->an, started, c->show_rapid, c->show_air, sr);
    ui_clear_clip(a->ui);
    ui_draw_border(a->ui, vr, 1, ui_theme_line_fg(a->ui));
}

static void draw_status(App *a, UiRect sr)
{
    const Path *p = a->p;
    char st[512];
    if (!a->loaded) {
        snprintf(st, sizeof st, "%s: cannot open", a->file);
    } else {
        double tp[3];
        int line = p->n > 0 ? p->seg[a->an.cur].line : 0, n;
        tool_pos(p, &a->an, tp);
        if (a->cfg.lathe)   /* X wyswietlany tak, jak pisze go program */
            n = snprintf(st, sizeof st,
                         "%s  |  lathe, stock D%s x %s mm  |  X%.3f Z%.3f  line %d",
                         a->file, a->cfg.stock_d, a->cfg.stock_l,
                         a->cfg.diameter ? 2 * tp[0] : tp[0], tp[2], line);
        else
            n = snprintf(st, sizeof st,
                         "%s  |  size %gx%gx%g mm  |  X%.3f Y%.3f Z%.3f  line %d",
                         a->file, p->hi[0] - p->lo[0], p->hi[1] - p->lo[1],
                         p->hi[2] - p->lo[2], tp[0], tp[1], tp[2], line);
        if (n > 0 && (size_t)n < sizeof st && (p->ignored || p->errors || p->truncated))
            snprintf(st + n, sizeof st - (size_t)n, "  |  ignored %d, errors %d%s",
                     p->ignored, p->errors, p->truncated ? ", TRUNCATED" : "");
    }
    ui_label_ellipsis(a->ui, sr, st);
}

/* Box z kodem programu: numery linii, tekst, podswietlona linia biezacego
 * odcinka (tylko po starcie). Przewija sie do niej, gdy sie zmieni. */
static void draw_code(App *a, UiRect r)
{
    UiCtx *ui = a->ui;
    const Source *s = &g_src;
    int rh = ui_line_height(ui) + 2;
    int rows = (r.h - 2 * MARGIN) / rh;
    int cur = anim_started(&a->an) && a->p->n > 0 ? a->p->seg[a->an.cur].line : 0;

    a->code_r = r;
    ui_fill_rect(ui, r, ui_theme_box_info_bg(ui));
    ui_draw_border(ui, r, 1, ui_theme_line_fg(ui));
    if (rows < 1) return;
    if (cur != a->code_follow) {
        a->code_follow = cur;
        a->code_top = cur - rows / 2;
    }
    int max_top = s->n - rows + 1;
    if (a->code_top > max_top) a->code_top = max_top;
    if (a->code_top < 1) a->code_top = 1;

    char num[16];
    snprintf(num, sizeof num, "%d", s->n > 0 ? s->n : 1);
    int gw = ui_text_width(ui, num) + 2 * MARGIN;
    int y0 = r.y + MARGIN;
    ui_draw_line(ui, r.x + gw, y0, r.x + gw, y0 + rows * rh, 1, ui_theme_line_fg(ui));
    for (int i = 0; i < rows && a->code_top + i <= s->n; i++) {
        int ln = a->code_top + i;
        UiRect row = { r.x + 2, y0 + i * rh, r.w - 4, rh };
        if (ln == cur) ui_fill_round_rect(ui, row, ui_theme_accent(ui));
        snprintf(num, sizeof num, "%d", ln);
        int nw = ui_text_width(ui, num);
        UiRect nr = { r.x + gw - MARGIN - nw, row.y, nw + 2, rh };
        UiRect tr = { r.x + gw + MARGIN, row.y, r.w - gw - 2 * MARGIN, rh };
        ui_label(ui, nr, num);
        if (tr.w > 0) ui_label_ellipsis(ui, tr, src_line(s, ln));
    }
}

/* Zakladka Viewer: pasek przyciskow, widok + box z kodem, pasek stanu - w r. */
static int draw_viewer(App *a, UiRect r)
{
    enum { NBTN = 8, LED_R = 3 };
    static const char *labels[NBTN] = { "Edit", NULL, "Step", "Reset", "Slower",
                                        "Faster", "Top", "Lathe" };
    static const int acts[NBTN] = { ACT_EDIT, ACT_RUN, ACT_STEP, ACT_RESET, ACT_SLOWER,
                                    ACT_FASTER, ACT_VIEW, ACT_LATHE };
    /* -1 = przycisk bez stanu, 0/1 = wylaczony/wlaczony (zolta kropka);
     * Top bez znaczenia przy toczeniu (zawsze widok z boku) */
    int on[NBTN] = { -1, a->an.playing, -1, -1, a->an.speed < SPEED_DEFAULT,
                     a->an.speed > SPEED_DEFAULT,
                     a->cfg.lathe ? -1 : a->cfg.top_view, a->cfg.lathe };
    UiCtx *ui = a->ui;
    int act = ACT_NONE;
    int lh = ui_line_height(ui), bh = lh + 8, sbh = lh + 6;

    int x = r.x;
    for (int i = 0; i < NBTN; i++) {
        const char *l = labels[i] ? labels[i] : (a->an.playing ? "Pause" : "Run");
        int bw = ui_button_width(ui, "Pause");
        if (labels[i]) {
            int lw = ui_button_width(ui, l);
            if (lw > bw) bw = lw;
        }
        if (on[i] >= 0) bw += 4 * LED_R + 4;   /* miejsce na kropke po prawej */
        UiRect br = { x, r.y, bw, bh };
        if (ui_button(ui, br, l)) act = acts[i];
        if (on[i] > 0)
            ui_fill_circle(ui, br.x + br.w - 3 * LED_R - 2, br.y + bh / 2, LED_R,
                           &a->col.led);
        x += bw + MARGIN;
    }
    char info[256], tnow[32], ttot[32];
    fmt_time(tnow, sizeof tnow, a->an.sim_t);
    fmt_time(ttot, sizeof ttot, a->p->total_t);
    snprintf(info, sizeof info, "x%g   %s / %s", speeds[a->an.speed], tnow, ttot);
    UiRect ir = { x, r.y, r.x + r.w - x, bh };
    if (ir.w > 0) ui_label(ui, ir, info);

    UiRect vr = { r.x, r.y + bh + MARGIN, r.w, r.h - bh - sbh - 2 * MARGIN };
    int cw = r.w / 3 > 420 ? 420 : r.w / 3;
    a->code_r.w = 0;
    if (cw >= 160 && vr.h > 10) {   /* waskie okno: sam widok */
        vr.w -= cw + MARGIN;
        draw_code(a, (UiRect){ vr.x + vr.w + MARGIN, vr.y, cw, vr.h });
    }
    if (vr.w > 10 && vr.h > 10) draw_view(a, vr);

    UiRect sr = { r.x, r.y + r.h - sbh, r.w, sbh };
    draw_status(a, sr);
    return act;
}

/* Wiersz "etykieta: pole tekstowe"; lw = szerokosc kolumny etykiet. */
static int config_field(UiCtx *ui, UiRect r, int lw, const char *label,
                        char *buf, int cap, int *cur, int digits)
{
    UiRect lr = { r.x, r.y, lw, r.h };
    int fw = r.w - lw > 320 ? 320 : r.w - lw;
    UiRect fr = { r.x + lw, r.y, fw, r.h };
    ui_label(ui, lr, label);
    if (fr.w < 20) return 0;
    return digits ? ui_textbox_digits(ui, fr, buf, cap, cur)
                  : ui_textbox(ui, fr, buf, cap, cur);
}

/* Zakladka Config. Zmiana posuwu G0 zmienia czasy odcinkow -> ACT_RELOAD. */
static int draw_config(App *a, UiBox *box, int avail)
{
    UiCtx *ui = a->ui;
    Config *c = &a->cfg;
    int rh = ui_line_height(ui) + 10, act = ACT_NONE;
    int lw = ui_text_width(ui, "Rapid feed G0 (mm/min)") + 16;

    UiRect first = ui_box_next_rect(box, rh);   /* gora tresci - do dopelnienia */
    ui_checkbox(ui, first, "Show grid (XY plane, Z=0)", &c->show_grid);
    config_field(ui, ui_box_next_rect(box, rh), lw, "Grid step (mm)",
                 c->step, sizeof c->step, &c->step_cur, 0);
    ui_checkbox(ui, ui_box_next_rect(box, rh), "Show rapid moves (G0)", &c->show_rapid);
    ui_checkbox(ui, ui_box_next_rect(box, rh), "Show axes", &c->show_axes);
    ui_checkbox(ui, ui_box_next_rect(box, rh), "Show path shadow on Z=0", &c->show_shadow);
    ui_checkbox(ui, ui_box_next_rect(box, rh), "Show Z drop lines with labels",
                &c->show_drops);
    ui_checkbox(ui, ui_box_next_rect(box, rh),
                "Color cuts by Z (air above / material below Z=0)", &c->show_air);
    ui_checkbox(ui, ui_box_next_rect(box, rh), "Top view (XY) instead of isometric",
                &c->top_view);
    if (config_field(ui, ui_box_next_rect(box, rh), lw, "Rapid feed G0 (mm/min)",
                     c->rapid, sizeof c->rapid, &c->rapid_cur, 1)) {
        double v = atof(c->rapid);
        if (v > 0 && v != g_rapid_feed) {
            g_rapid_feed = v;
            act = ACT_RELOAD;
        }
    }
    if (ui_checkbox(ui, ui_box_next_rect(box, rh),
                    "Lathe (side view ZX, G18 default, stock = cylinder)", &c->lathe))
        act = ACT_MODE;   /* checkbox juz przelaczyl c->lathe */
    if (ui_checkbox(ui, ui_box_next_rect(box, rh),
                    "Lathe: X in program is the diameter (else radius)", &c->diameter)) {
        g_diameter = c->diameter;
        act = ACT_RELOAD;
    }
    config_field(ui, ui_box_next_rect(box, rh), lw, "Stock diameter (mm)",
                 c->stock_d, sizeof c->stock_d, &c->stock_d_cur, 0);
    config_field(ui, ui_box_next_rect(box, rh), lw, "Stock length (mm)",
                 c->stock_l, sizeof c->stock_l, &c->stock_l_cur, 0);
    config_field(ui, ui_box_next_rect(box, rh), lw, "Terminal",
                 c->term, sizeof c->term, &c->term_cur, 0);
    config_field(ui, ui_box_next_rect(box, rh), lw, "Editor",
                 c->editor, sizeof c->editor, &c->editor_cur, 0);

    UiRect row = ui_box_next_rect(box, rh);
    UiRect br = { row.x, row.y, ui_button_width(ui, "Save"), row.h };
    if (ui_button(ui, br, "Save")) act = ACT_SAVE;
    UiRect mr = { br.x + br.w + MARGIN, row.y, row.w - br.w - MARGIN, row.h };
    if (c->msg[0] && mr.w > 0) ui_label_ellipsis(ui, mr, c->msg);
    /* pusty wiersz do dolu okna - box na pelna wysokosc jak w Viewer/Help */
    int rest = avail - (row.y + row.h - first.y) - MARGIN;
    if (rest > 0) ui_box_next_rect(box, rest);
    return act;
}

/* Popup bledu rysowany na wierzchu (bez osobnego okna/grab, jak dropdown
 * w 7atodo). Wiersze z p->err rozdzielone '\n'. Zwraca ACT_ERR_CLOSE na OK. */
static int draw_error(App *a)
{
    UiCtx *ui = a->ui;
    const Path *p = a->p;
    int lh = ui_line_height(ui), bh = lh + 8;
    char head[96];
    if (p->errors > 1)
        snprintf(head, sizeof head, "Error in line %d  (%d errors total, first shown)",
                 p->err_line, p->errors);
    else
        snprintf(head, sizeof head, "Error in line %d", p->err_line);

    int nl = 1;
    for (const char *s = p->err; *s; s++) nl += *s == '\n';
    int w = a->w - 8 * MARGIN > 620 ? 620 : a->w - 8 * MARGIN;
    int h = (nl + 1) * lh + bh + 5 * MARGIN;
    if (w < 100) return ACT_NONE;
    UiRect r = { (a->w - w) / 2, (a->h - h) / 2, w, h };
    ui_fill_rect(ui, r, ui_theme_box_bg(ui));
    ui_draw_border(ui, r, 2, &a->col.done);

    int x = r.x + 2 * MARGIN, tw = r.w - 4 * MARGIN, y = r.y + 2 * MARGIN;
    ui_label_fg(ui, (UiRect){ x, y, tw, lh }, head, &a->col.done);
    y += lh + MARGIN;
    const char *s = p->err;
    for (int k = 0; k < nl; k++, y += lh) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char buf[sizeof p->err];
        snprintf(buf, sizeof buf, "%.*s", (int)n, s);
        ui_label_ellipsis(ui, (UiRect){ x, y, tw, lh }, buf);
        s = e ? e + 1 : s + n;
    }
    int bw = ui_button_width(ui, "OK");
    UiRect br = { r.x + r.w - bw - 2 * MARGIN, r.y + r.h - bh - 2 * MARGIN, bw, bh };
    return ui_button(ui, br, "OK") ? ACT_ERR_CLOSE : ACT_NONE;
}

/* Probka linii na tle widoku (kolory dobrane pod czarne tlo) + opis. */
static void help_swatch(App *a, UiRect r, const XColor *c, int w, const char *text)
{
    UiRect sr = { r.x, r.y + 2, 48, r.h - 4 };
    UiRect lr = { r.x + sr.w + MARGIN, r.y, r.w - sr.w - MARGIN, r.h };
    ui_fill_rect(a->ui, sr, &a->col.bg);
    ui_draw_line(a->ui, sr.x + 6, sr.y + sr.h / 2, sr.x + sr.w - 6, sr.y + sr.h / 2, w, c);
    ui_label_ellipsis(a->ui, lr, text);
}

/* Wiersz Help: klucz (kpl = NULL -> ten sam po polsku) + opis EN/PL. */
typedef struct { const char *k, *kpl, *en, *pl; } HelpRow;

static void help_rows(UiCtx *ui, UiRect r, const HelpRow *t, size_t n, int pl)
{
    int kw = 0;
    for (size_t i = 0; i < n; i++) {
        int w = ui_text_width(ui, pl && t[i].kpl ? t[i].kpl : t[i].k);
        if (w > kw) kw = w;
    }
    kw += 2 * MARGIN;
    if (kw > r.w / 2) kw = r.w / 2;
    for (size_t i = 0; i < n; i++, r.y += r.h) {
        UiRect kr = { r.x, r.y, kw, r.h }, dr = { r.x + kw, r.y, r.w - kw, r.h };
        ui_label_ellipsis(ui, kr, pl && t[i].kpl ? t[i].kpl : t[i].k);
        ui_label_ellipsis(ui, dr, pl ? t[i].pl : t[i].en);
    }
}

#define HELP_N(t) (sizeof (t) / sizeof (t)[0])

/* Strona 1: legenda linii (lewa kolumna), przyciski i klawisze (prawa). */
static void help_view(App *a, UiRect L, UiRect R, int pl)
{
    static const HelpRow keys[] = {
        { "Edit  (e)", NULL, "open the file in the external editor",
          "otwórz plik w zewnętrznym edytorze" },
        { "Run / Pause (space)", "Run / Pause (spacja)", "start / pause the simulation",
          "start / pauza symulacji" },
        { "Step  (s)", NULL, "pause, run to the end of the next line",
          "pauza, wykonaj do końca następnej linii" },
        { "", NULL, "  current line is highlighted in the code box",
          "  bieżąca linia podświetlona w boxie z kodem" },
        { "Reset  (r)", NULL, "move the tool back to the program start",
          "narzędzie z powrotem na początek programu" },
        { "Slower / Faster (- +)", NULL, "simulation speed (shown as x...)",
          "prędkość symulacji (pokazana jako x...)" },
        { "Top  (t)", NULL, "top view (XY) on/off, else isometric",
          "widok z góry (XY) wł./wył., inaczej izometria" },
        { "Lathe  (l)", NULL, "turning: side view Z right, X up, stock",
          "toczenie: widok z boku, Z w prawo, X w górę," },
        { "", NULL, "  cylinder from Config (D x length)",
          "  walec z Config (średnica x długość)" },
        { "yellow dot", "żółta kropka", "button is on: Run playing, Slower/Faster",
          "przycisk włączony: Run gra, Slower/Faster" },
        { "", NULL, "  away from x10, Top view, Lathe active",
          "  poza x10, widok z góry, toczenie" },
        { "code box wheel", "kółko nad kodem", "scroll the program text",
          "przewija tekst programu" },
        { "q, Esc", NULL, "quit (Esc/Enter also close the error popup)",
          "wyjście (Esc/Enter zamykają też okno błędu)" },
        { "Config: Save", NULL, "write settings to ~/.7a/cnc.conf",
          "zapisz ustawienia w ~/.7a/cnc.conf" },
        { "File reload", "Przeładowanie", "automatic when the edited file is saved",
          "samo, gdy edytowany plik zostanie zapisany" },
    };
    UiCtx *ui = a->ui;
    const Colors *c = &a->col;
    struct { const XColor *c; int w; const char *en, *pl; } sw[] = {
        { &c->cut,    1, "cut G1/G2/G3 on Z=0", "skrawanie G1/G2/G3 na Z=0" },
        { &c->mat,    1, "cut below Z=0 - material is machined",
                         "skrawanie pod Z=0 - obróbka materiału" },
        { &c->air,    1, "cut above Z=0 - cutting air", "skrawanie nad Z=0 - w powietrzu" },
        { &c->rapid,  1, "rapid move G0 / G28", "szybki przejazd G0 / G28" },
        { &c->done,   2, "already travelled part of the path", "przebyta część ścieżki" },
        { &c->tool,   4, "tool (blue dot + shank)", "narzędzie (niebieska kropka + trzonek)" },
        { &c->shadow, 1, "path shadow on Z=0 (XY position)", "cień ścieżki na Z=0 (położenie XY)" },
        { &c->drop,   1, "drop line to Z=0 with Z value", "kreska do Z=0 z wartością Z" },
        { &c->grid,   1, "grid on Z=0, step in mm (Config)", "siatka na Z=0, krok w mm (Config)" },
        { &c->stock,  8, "lathe: stock left after the cut", "toczenie: materiał pozostały po skrawaniu" },
        { &c->ax[0],  1, "axis X", "oś X" },
        { &c->ax[1],  1, "axis Y", "oś Y" },
        { &c->ax[2],  1, "axis Z (hidden in top view)", "oś Z (ukryta w widoku z góry)" },
    };
    ui_label_fg(ui, L, pl ? "Linie w widoku" : "Lines in the view", ui_theme_accent(ui));
    L.y += L.h;
    for (size_t i = 0; i < HELP_N(sw); i++, L.y += L.h)
        help_swatch(a, L, sw[i].c, sw[i].w, pl ? sw[i].pl : sw[i].en);
    ui_label_ellipsis(ui, L, pl ? "Kolory wg Z oraz cień/kreski: zakładka Config."
                                : "Line colors by Z and shadow/drops: Config tab.");

    ui_label_fg(ui, R, pl ? "Przyciski i klawisze" : "Buttons and keys", ui_theme_accent(ui));
    R.y += R.h;
    help_rows(ui, R, keys, HELP_N(keys), pl);
}

/* Strona 2: slowa obslugiwane przez parser (lewa), petle M98/M99 (prawa). */
static void help_gcode(App *a, UiRect L, UiRect R, int pl)
{
    static const HelpRow words[] = {
        { "G0", NULL, "rapid move (G0 feed from Config)", "szybki przejazd (posuw G0 z Config)" },
        { "G1", NULL, "linear move with feed F", "ruch liniowy z posuwem F" },
        { "G2 / G3", NULL, "arc CW / CCW: end + centre I J K or R",
          "łuk w prawo / w lewo: koniec + środek I J K albo R" },
        { "G4", NULL, "dwell: X or F in seconds, P in ms", "postój: X albo F w sekundach, P w ms" },
        { "G17 G18 G19", NULL, "arc plane XY / ZX / YZ (lathe: G18)",
          "płaszczyzna łuku XY / ZX / YZ (toczenie: G18)" },
        { "G20 G21", NULL, "inch / mm (also G70 / G71)", "cale / mm (też G70 / G71)" },
        { "G28", NULL, "rapid to program zero (given axes or all)",
          "szybko do zera programu (podane osie albo wszystkie)" },
        { "G33", NULL, "thread, lead K", "gwint, skok K" },
        { "G90 G91", NULL, "absolute / incremental", "absolutnie / przyrostowo" },
        { "G94 G95", NULL, "feed mm/min / mm per revolution", "posuw mm/min / mm na obrót" },
        { "G96 G97", NULL, "constant cutting speed S m/min / S rpm",
          "stała prędkość skrawania S m/min / S obr/min" },
        { "F  S", NULL, "feed, spindle speed", "posuw, obroty wrzeciona" },
        { "M0-M9", NULL, "accepted, no effect in the simulation",
          "akceptowane, bez efektu w symulacji" },
        { "M2  M30", NULL, "end of the main program", "koniec programu głównego" },
        { "M98  M99", NULL, "subprogram call / return - see right",
          "wywołanie / powrót z podprogramu - obok" },
        { "N..  %", NULL, "block number / program start, skipped",
          "numer bloku / początek programu, pomijane" },
        { "( )  ;", NULL, "comments", "komentarze" },
        { "other G/M", "inne G/M", "ignored, counted in the status bar",
          "pomijane, liczone w pasku stanu" },
    };
    static const HelpRow loop[] = {
        { "M98 P100 L3", NULL, "run subprogram O100 three times",
          "wykonaj podprogram O100 trzy razy" },
        { "M98 P31000", NULL, "the same, Fanuc short form: 3 x O1000",
          "to samo, skrót Fanuc: 3 x O1000" },
        { "O100", NULL, "start of subprogram 100, put it after M30",
          "początek podprogramu 100, umieść go za M30" },
        { "M99", NULL, "end of subprogram: next pass or return",
          "koniec podprogramu: kolejny przebieg albo powrót" },
        { "", NULL, "  subprograms may call others, up to 8 levels",
          "  podprogram może wołać inne, do 8 poziomów" },
    };
    static const char *example[] = {
        "G21 G90 G0 X0 Y0 Z0",
        "M98 P100 L3",
        "G90 G0 Z5",
        "M30",
        "O100",
        "G91 G1 Z-1 F200",
        "G90 G1 X40 F600",
        "G1 Y30",
        "G1 X0",
        "G1 Y0",
        "M99",
    };
    UiCtx *ui = a->ui;
    ui_label_fg(ui, L, pl ? "Słowa obsługiwane przez parser (DIN 66025)"
                          : "Words understood by the parser (DIN 66025)", ui_theme_accent(ui));
    L.y += L.h;
    help_rows(ui, L, words, HELP_N(words), pl);

    ui_label_fg(ui, R, pl ? "Pętle: podprogramy (styl Fanuc)"
                          : "Loops: subprograms (Fanuc style)", ui_theme_accent(ui));
    R.y += R.h;
    help_rows(ui, R, loop, HELP_N(loop), pl);
    R.y += (int)HELP_N(loop) * R.h + R.h / 2;
    ui_label_fg(ui, R, pl ? "Przykład: obrys 40x30 w 3 przejściach po 1 mm"
                          : "Example: 40x30 outline in 3 passes, 1 mm each",
                ui_theme_accent(ui));
    R.y += R.h;
    int lh = ui_line_height(ui) + 2;
    UiRect box = { R.x, R.y, R.w, (int)HELP_N(example) * lh + 2 * MARGIN };
    ui_fill_rect(ui, box, ui_theme_box_info_bg(ui));
    ui_draw_border(ui, box, 1, ui_theme_line_fg(ui));
    UiRect er = { R.x + 2 * MARGIN, R.y + MARGIN, R.w - 4 * MARGIN, lh };
    for (size_t i = 0; i < HELP_N(example); i++, er.y += lh)
        ui_label_ellipsis(ui, er, example[i]);
    R.y = box.y + box.h + R.h / 2;
    ui_label_ellipsis(ui, R, pl ? "G91 w podprogramie: każdy przebieg 1 mm głębiej."
                                : "G91 inside: every pass goes 1 mm deeper.");
    R.y += R.h;
    ui_label_ellipsis(ui, R, pl ? "Box z kodem podświetla linie podprogramu w każdym przebiegu."
                                : "The code box highlights subprogram lines on every pass.");
}

/* Zakladka Help: przelaczniki stron i jezyka u gory, pod nimi strona. */
static void draw_help(App *a, UiRect r)
{
    enum { LED_R = 3 };
    UiCtx *ui = a->ui;
    int pl = a->help_pl, rh = ui_line_height(ui) + 6, bh = ui_line_height(ui) + 8;
    const char *pages[2] = { pl ? "Widok i klawisze" : "View and keys", "G-code" };
    int x = r.x;
    for (int i = 0; i < 2; i++) {
        int bw = ui_button_width(ui, pages[i]) + 4 * LED_R + 4;
        UiRect br = { x, r.y, bw, bh };
        if (ui_button(ui, br, pages[i])) a->help_page = i;
        if (a->help_page == i)
            ui_fill_circle(ui, br.x + br.w - 3 * LED_R - 2, br.y + bh / 2, LED_R, &a->col.led);
        x += bw + MARGIN;
    }
    const char *lang = pl ? "English" : "Polski";
    int lw = ui_button_width(ui, lang);
    if (ui_button(ui, (UiRect){ r.x + r.w - lw, r.y, lw, bh }, lang)) a->help_pl = !pl;
    pl = a->help_pl;   /* najpierw stan, potem rysowanie strony */

    UiRect row = { r.x, r.y + bh + 2 * MARGIN, r.w, rh };
    UiRect L = ui_rect_col(row, 0, 2, 2 * MARGIN), R = ui_rect_col(row, 1, 2, 2 * MARGIN);
    if (a->help_page == 0) help_view(a, L, R, pl);
    else                   help_gcode(a, L, R, pl);
}

static int draw(App *a)
{
    static const char *tabs[3] = { "Viewer", "Config", "Help" };
    UiCtx *ui = a->ui;
    int tab_h = ui_line_height(ui) + 8, act = ACT_NONE;
    UiBoxStyle st = {0};
    st.margin_l = st.margin_r = st.margin_t = st.margin_b = MARGIN;
    st.padding_l = st.padding_r = st.padding_t = st.padding_b = MARGIN;
    st.border_w = 1;
    st.gap = MARGIN;
    st.border_color = *ui_theme_line_fg(ui);
    st.bg_color = *ui_theme_box_bg(ui);
    st.transparent = 1;

    ui_begin_frame(ui);
    /* box pod zakladkami; ui_tabs PO ui_box_begin (patrz ui.h) */
    UiBox *box = ui_box_begin(ui, "main", 0, tab_h, a->w, &st);
    UiRect tr = { MARGIN + TAB_INDENT, MARGIN, a->w - 2 * (MARGIN + TAB_INDENT), tab_h };
    if (ui_tabs(ui, tr, tabs, 3, &a->tab, &st))
        ui_set_focus(ui, NULL);
    int avail = a->h - tab_h - 4 * MARGIN - 2 * st.border_w;
    if (a->tab == 0) {
        if (avail > 0) act = draw_viewer(a, ui_box_next_rect(box, avail));
    } else if (a->tab == 1) {
        act = draw_config(a, box, avail);
    } else if (avail > 0) {
        draw_help(a, ui_box_next_rect(box, avail));
    }
    ui_box_end(box);
    /* popup modalny: akcje spod spodu (klikniete "przez" popup) odrzucane */
    if (a->tab == 0 && a->err_open) act = draw_error(a);
    ui_end_frame(ui);
    return act;
}

/* ── plik / edytor ─────────────────────────────────────────────────── */

static const char sample[] =
    "( 7acnc sample: square pocket outline + circle )\n"
    "G21 G90\n"
    "G0 Z5\n"
    "G0 X0 Y0\n"
    "G1 Z-1 F200\n"
    "G1 X40 F600\n"
    "G1 Y30\n"
    "G1 X0\n"
    "G1 Y0\n"
    "G0 Z5\n"
    "G0 X20 Y5\n"
    "G1 Z-2 F200\n"
    "G2 X20 Y5 I0 J10 F400  ; full circle r=10\n"
    "G0 Z5\n"
    "G0 X0 Y0\n";

/* X jako srednica (domyslne ustawienie Config), stock D40 x 60 */
static const char lathe_sample[] =
    "( 7acnc lathe sample: face, rough, profile, thread - DIN 66025 )\n"
    "N10 G90 G71 G18\n"
    "N20 G96 S180 M4        ; constant cutting speed 180 m/min\n"
    "N30 G95 F0.2           ; feed in mm per revolution\n"
    "N40 G0 X42 Z2\n"
    "N50 G1 Z0 F0.15        ; face\n"
    "N60 G1 X0\n"
    "N70 G0 Z2\n"
    "N80 G0 X36\n"
    "N90 G1 Z-40 F0.25      ; rough D36\n"
    "N100 G0 X40 Z2\n"
    "N110 G0 X32\n"
    "N120 G1 Z-25\n"
    "N130 G0 X40 Z2\n"
    "N140 G0 X20\n"
    "N150 G1 Z0 F0.1        ; finish profile\n"
    "N160 G1 X24 Z-2\n"
    "N170 G1 Z-15\n"
    "N180 G2 X32 Z-19 R4\n"
    "N190 G1 Z-25\n"
    "N200 G1 X36 Z-27\n"
    "N210 G1 Z-40\n"
    "N220 G0 X45\n"
    "N230 G0 Z2\n"
    "N240 G97 S600\n"
    "N250 G0 X23.4          ; thread M24x1.5, 4 passes to D22.16\n"
    "N252 G33 Z-14 K1.5\n"
    "N254 G0 X30\n"
    "N256 G0 Z2 X22.9\n"
    "N258 G33 Z-14\n"
    "N260 G0 X30\n"
    "N262 G0 Z2 X22.5\n"
    "N264 G33 Z-14\n"
    "N266 G0 X30\n"
    "N268 G0 Z2 X22.16\n"
    "N270 G33 Z-14\n"
    "N272 G0 X30\n"
    "N280 G0 Z2\n"
    "N290 G4 F1             ; dwell 1 s\n"
    "N300 G0 X50 Z20\n"
    "N310 M30\n";

static void ensure_file(const char *file, int lathe)
{
    int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;   /* istnieje albo brak katalogu - obie sytuacje OK */
    ssize_t wr = lathe ? write(fd, lathe_sample, sizeof lathe_sample - 1)
                       : write(fd, sample, sizeof sample - 1);
    (void)wr;
    close(fd);
}

/* Bez argumentu CLI: plik w ~/.7a zalezny od trybu (tworzony z przykladem). */
static void own_file_path(char *file, int lathe)
{
    const char *home = getenv("HOME");
    char dir[FILE_CAP - 16];
    snprintf(dir, sizeof dir, "%s/.7a", home ? home : ".");
    mkdir(dir, 0755);
    snprintf(file, FILE_CAP, "%s/%s", dir, lathe ? "lathe.nc" : "cnc.nc");
    ensure_file(file, lathe);
}

static time_t file_mtime(const char *file)
{
    struct stat sb;
    return stat(file, &sb) == 0 ? sb.st_mtime : 0;
}

static void spawn_editor(const char *term, const char *editor, const char *file)
{
    if (!*term || !*editor) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp(term, term, "-e", editor, file, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) perror(PROG ": fork");
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void alloc_colors(UiCtx *ui, Colors *c)
{
    /* widok ma stale czarne tlo, wiec kolory widoku sa stale, nie z motywu */
    struct { XColor *dst; const char *spec; const XColor *fb; } t[] = {
        { &c->bg,    "#000000", ui_theme_bg(ui) },
        { &c->label, "#a0a0a0", ui_theme_fg(ui) },
        { &c->rapid, "#707070", ui_theme_line_fg(ui) },
        { &c->cut,   "#e8e8e8", ui_theme_fg(ui) },
        { &c->air,   "#3f9f9f", ui_theme_line_fg(ui) },  /* ciecie nad Z=0 */
        { &c->mat,   "#f0d040", ui_theme_accent(ui) },   /* ciecie pod Z=0 */
        { &c->led,   "#f0d040", ui_theme_accent(ui) },   /* kropka "wlaczony" */
        { &c->done,  "#ff7a1a", ui_theme_accent(ui) },
        { &c->tool,  "#2f8fff", ui_theme_fg(ui) },
        { &c->ax[0], "#e04848", ui_theme_fg(ui) },
        { &c->ax[1], "#3cb043", ui_theme_fg(ui) },
        { &c->ax[2], "#4a7bff", ui_theme_fg(ui) },
        { &c->grid,  "#2a2a2a", ui_theme_line_fg(ui) },
        { &c->shadow,"#4a4a4a", ui_theme_line_fg(ui) },
        { &c->drop,  "#8a8a50", ui_theme_line_fg(ui) },
        { &c->stock, "#5a6470", ui_theme_line_fg(ui) },  /* walec (toczenie) */
    };
    c->npix = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        /* wlasnosc: piksele z ui_color zwalnia main przez XFreeColors */
        if (t[i].spec && ui_color(ui, t[i].spec, t[i].dst))
            c->pixels[c->npix++] = t[i].dst->pixel;
        else
            *t[i].dst = *t[i].fb;
    }
}

/* Plik konfiguracji: linie "klucz=wartosc"; nieznane klucze pomijane. */
static int config_save(const Config *c)
{
    FILE *f = fopen(c->path, "w");
    if (!f) return 0;
    fprintf(f, "grid=%d\ngrid_step=%s\nrapid=%d\naxes=%d\n"
               "shadow=%d\ndrops=%d\nair_color=%d\ntop_view=%d\n"
               "rapid_feed=%s\nterminal=%s\neditor=%s\n"
               "lathe=%d\ndiameter=%d\nstock_diameter=%s\nstock_length=%s\n",
            c->show_grid, c->step, c->show_rapid, c->show_axes,
            c->show_shadow, c->show_drops, c->show_air, c->top_view,
            c->rapid, c->term, c->editor,
            c->lathe, c->diameter, c->stock_d, c->stock_l);
    return fclose(f) == 0;
}

static void config_load(Config *c)
{
    FILE *f = fopen(c->path, "r");
    if (!f) return;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        char *v = strchr(buf, '=');
        if (!v) continue;
        *v++ = '\0';
        if      (!strcmp(buf, "grid"))       c->show_grid  = atoi(v) != 0;
        else if (!strcmp(buf, "rapid"))      c->show_rapid = atoi(v) != 0;
        else if (!strcmp(buf, "axes"))       c->show_axes  = atoi(v) != 0;
        else if (!strcmp(buf, "shadow"))     c->show_shadow = atoi(v) != 0;
        else if (!strcmp(buf, "drops"))      c->show_drops = atoi(v) != 0;
        else if (!strcmp(buf, "air_color"))  c->show_air   = atoi(v) != 0;
        else if (!strcmp(buf, "top_view"))   c->top_view   = atoi(v) != 0;
        else if (!strcmp(buf, "grid_step"))  snprintf(c->step, sizeof c->step, "%s", v);
        else if (!strcmp(buf, "rapid_feed")) snprintf(c->rapid, sizeof c->rapid, "%s", v);
        else if (!strcmp(buf, "terminal"))   snprintf(c->term, sizeof c->term, "%s", v);
        else if (!strcmp(buf, "editor"))     snprintf(c->editor, sizeof c->editor, "%s", v);
        else if (!strcmp(buf, "lathe"))      c->lathe      = atoi(v) != 0;
        else if (!strcmp(buf, "diameter"))   c->diameter   = atoi(v) != 0;
        else if (!strcmp(buf, "stock_diameter"))
            snprintf(c->stock_d, sizeof c->stock_d, "%s", v);
        else if (!strcmp(buf, "stock_length"))
            snprintf(c->stock_l, sizeof c->stock_l, "%s", v);
    }
    fclose(f);
    double r = atof(c->rapid);
    if (r > 0) g_rapid_feed = r;
    g_lathe = c->lathe;
    g_diameter = c->diameter;
}

/* Kolejnosc: wartosci wbudowane < zasoby X / $EDITOR < ~/.7a/cnc.conf */
static void config_init(Config *c, Display *dpy)
{
    const char *env_ed = getenv("EDITOR");
    const char *home = getenv("HOME");
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s/.7a/cnc.conf", home ? home : ".");
    c->show_grid = c->show_rapid = c->show_axes = 1;
    c->show_shadow = c->show_drops = c->show_air = 1;
    snprintf(c->step, sizeof c->step, "10");
    snprintf(c->rapid, sizeof c->rapid, "%g", g_rapid_feed);
    c->diameter = 1;   /* typowo w toczeniu X programuje sie srednica */
    snprintf(c->stock_d, sizeof c->stock_d, "40");
    snprintf(c->stock_l, sizeof c->stock_l, "60");
    ui_resource_string(dpy, "7aCnc.terminal", "7aCnc.Terminal",
                       c->term, sizeof c->term, "urxvt");
    ui_resource_string(dpy, "7aCnc.editor", "7aCnc.Editor",
                       c->editor, sizeof c->editor,
                       env_ed && *env_ed ? env_ed : "vi");
    config_load(c);
    c->step_cur   = (int)strlen(c->step);
    c->rapid_cur  = (int)strlen(c->rapid);
    c->term_cur   = (int)strlen(c->term);
    c->editor_cur = (int)strlen(c->editor);
    c->stock_d_cur = (int)strlen(c->stock_d);
    c->stock_l_cur = (int)strlen(c->stock_l);
}

static void anim_reset(Anim *an)
{
    an->playing = 0;
    an->sim_t = 0;
    an->cur = 0;
    an->stepped = 0;
}

static void apply(App *a, int act)
{
    Anim *an = &a->an;
    switch (act) {
    case ACT_EDIT:   spawn_editor(a->cfg.term, a->cfg.editor, a->file); break;
    case ACT_RUN:
        if (!an->playing && an->sim_t >= a->p->total_t) an->sim_t = 0;
        an->playing = !an->playing && a->p->n > 0;
        break;
    case ACT_STEP:   anim_step(an, a->p); break;
    case ACT_RESET:  anim_reset(an); break;
    case ACT_SLOWER: if (an->speed > 0) an->speed--; break;
    case ACT_FASTER: if (an->speed < N_SPEEDS - 1) an->speed++; break;
    case ACT_LATHE:
        a->cfg.lathe = !a->cfg.lathe;
        /* fall through */
    case ACT_MODE:   /* plaszczyzna domyslna i (bez argumentu) plik zaleza od trybu */
        g_lathe = a->cfg.lathe;
        if (a->own_file) own_file_path(a->file, g_lathe);
        /* fall through */
    case ACT_RELOAD:
        a->loaded = path_load(a->p, a->file);
        a->err_open = a->p->errors > 0;
        a->mtime = file_mtime(a->file);
        anim_reset(an);
        break;
    case ACT_ERR_CLOSE: a->err_open = 0; break;
    case ACT_VIEW:   if (!a->cfg.lathe) a->cfg.top_view = !a->cfg.top_view; break;
    case ACT_SAVE:
        snprintf(a->cfg.msg, sizeof a->cfg.msg, "%s %s",
                 config_save(&a->cfg) ? "Saved to" : "Cannot write",
                 a->cfg.path);
        break;
    }
}

static int viewer_key(KeySym ks)
{
    switch (ks) {
    case XK_space: return ACT_RUN;
    case XK_s:     return ACT_STEP;
    case XK_r:     return ACT_RESET;
    case XK_e:     return ACT_EDIT;
    case XK_t:     return ACT_VIEW;
    case XK_l:     return ACT_LATHE;
    case XK_minus: return ACT_SLOWER;
    case XK_plus: case XK_equal: return ACT_FASTER;
    }
    return ACT_NONE;
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    char file[FILE_CAP];
    App app = { .w = 800, .h = 600, .p = &g_path, .file = file };

    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fputs("usage: " PROG " [file.nc]\n", stderr);
        return 1;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs(PROG ": cannot open display\n", stderr); return 1; }
    int screen = DefaultScreen(dpy);
    config_init(&app.cfg, dpy);   /* tryb (lathe) z configu wybiera plik */
    app.own_file = argc != 2;
    if (app.own_file) {
        own_file_path(file, app.cfg.lathe);
    } else {
        snprintf(file, sizeof file, "%s", argv[1]);
        ensure_file(file, app.cfg.lathe);
    }

    XSetWindowAttributes attr = {
        .background_pixmap = None,
        .event_mask = ExposureMask | StructureNotifyMask | KeyPressMask
                    | ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
    };
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
        (unsigned)app.w, (unsigned)app.h, 0, DefaultDepth(dpy, screen),
        InputOutput, DefaultVisual(dpy, screen), CWBackPixmap | CWEventMask, &attr);

    /* GC: wlasnosc main, XFreeGC na koncu (ui.c go tylko uzywa) */
    GC gc = XCreateGC(dpy, win, 0, NULL);
    UiCtx *ui = ui_init(dpy, win, gc, DEF_FONT, app.w, app.h);
    if (!ui) {
        fputs(PROG ": ui_init failed (no font?)\n", stderr);
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ui, precision_manufacturing_48_width, precision_manufacturing_48_height, precision_manufacturing_48_bits);
    app.ui = ui;
    XSetWindowBackground(dpy, win, ui_theme_bg(ui)->pixel);
    alloc_colors(ui, &app.col);

    XStoreName(dpy, win, PROG);
    XClassHint ch = { .res_name = PROG, .res_class = "7aCnc" };
    XSetClassHint(dpy, win, &ch);
    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);
    XSizeHints sh = { .flags = PMinSize, .min_width = 400, .min_height = 300 };
    XSetNormalHints(dpy, win, &sh);
    XMapWindow(dpy, win);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGCHLD, SIG_IGN);   /* terminal z edytorem - bez zombie */

    app.an.speed = SPEED_DEFAULT;
    app.loaded = path_load(&g_path, file);
    app.err_open = g_path.errors > 0;
    app.mtime = file_mtime(file);
    double last = now_s(), last_poll = last;
    int xfd = ConnectionNumber(dpy);

    draw(&app);

    while (g_running) {
        long ms = app.an.playing ? FRAME_MS : POLL_MS;
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        if (!XPending(dpy) && select(xfd + 1, &fds, NULL, NULL, &tv) < 0
            && errno != EINTR)
            break;

        int redraw = 0;
        while (XPending(dpy)) {
            XEvent ev;
            int act = ACT_NONE;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) ui_end_frame(ui);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != app.w || ev.xconfigure.height != app.h) {
                    app.w = ev.xconfigure.width;
                    app.h = ev.xconfigure.height;
                    ui_resize(ui, app.w, app.h);
                    redraw = 1;
                }
                break;
            case ButtonPress: case ButtonRelease:
                /* kolko nad boxem z kodem przewija go (przed ui_feed_event) */
                if (app.tab == 0 && !app.err_open && app.code_r.w > 0
                    && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
                    && ev.xbutton.x >= app.code_r.x
                    && ev.xbutton.x < app.code_r.x + app.code_r.w
                    && ev.xbutton.y >= app.code_r.y
                    && ev.xbutton.y < app.code_r.y + app.code_r.h) {
                    if (ev.type == ButtonPress) {
                        app.code_top += ev.xbutton.button == Button4 ? -3 : 3;
                        redraw = 1;
                    }
                    break;
                }
                /* fall through */
            case MotionNotify:
                ui_feed_event(ui, &ev);
                act = draw(&app);
                break;
            case KeyPress:
                if (app.tab == 1) {   /* Config: klawisze dla pol tekstowych */
                    ui_feed_event(ui, &ev);
                    act = draw(&app);
                } else if (app.tab == 2) {   /* Help: tylko wyjscie */
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_q || ks == XK_Escape) g_running = 0;
                } else {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (app.err_open) {   /* popup lapie klawisze */
                        if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter
                            || ks == XK_space)
                            act = ACT_ERR_CLOSE;
                    } else if (ks == XK_q || ks == XK_Escape) g_running = 0;
                    else act = viewer_key(ks);
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_del) g_running = 0;
                break;
            }
            if (act != ACT_NONE) {
                apply(&app, act);
                redraw = 1;
            }
        }

        double t = now_s();
        if (app.an.playing) {
            anim_advance(&app.an, &g_path, t - last);
            redraw = 1;
        }
        last = t;

        if (t - last_poll >= POLL_MS / 1000.0) {
            last_poll = t;
            if (file_mtime(file) != app.mtime) {
                apply(&app, ACT_RELOAD);
                redraw = 1;
            }
        }
        if (redraw) draw(&app);
    }

    if (app.col.npix)
        XFreeColors(dpy, DefaultColormap(dpy, screen), app.col.pixels, app.col.npix, 0);
    ui_destroy(ui);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
