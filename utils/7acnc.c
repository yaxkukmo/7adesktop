/* 7acnc - prosty symulator sciezki narzedzia CNC (G-code) w rzucie
 * izometrycznym, na ui.c/ui.h.
 *
 * Nowa apka (nie port Xt/Xaw). Zakres celowo waski:
 * - tylko G0/G1/G2/G3 (+ modalne G17 zakladane domyslnie, G20/G21,
 *   G90/G91, F); inne slowa G/M sa ignorowane i liczone w pasku stanu,
 * - sama sciezka narzedzia, bez symulacji usuwania materialu,
 * - staly rzut izometryczny (X w prawo-dol, Y w prawo-gore, Z w gore),
 *   automatycznie dopasowany do okna,
 * - edycja w ZEWNETRZNYM edytorze w osobnym oknie terminala
 *   (zasoby 7aCnc.terminal, domyslnie "urxvt", i 7aCnc.editor, domyslnie
 *   $EDITOR albo "vi"); apka sama przeladowuje plik, gdy zmieni sie jego
 *   mtime (polling stat() co 500 ms - bez inotify/kqueue, przenosnie).
 * - "Run" animuje narzedzie wzdluz sciezki; czas symulacji liczony z
 *   rzeczywistego uplywu czasu (CLOCK_MONOTONIC) razy mnoznik predkosci,
 *   a czas kazdego odcinka z posuwu F (G1-G3) albo stalego RAPID_FEED (G0).
 *
 * Sciezka to stala tablica odcinkow (MAX_SEGS) - luki dzielone sa na
 * krotkie odcinki przy wczytywaniu, wiec petla rysowania nie alokuje.
 *
 * Uzycie: 7acnc [plik.nc]   (domyslnie ~/.7a/cnc.nc, tworzony z przykladem)
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

typedef struct {
    float a[3], b[3];   /* poczatek / koniec, mm */
    double t_end;       /* skumulowany czas symulacji na koncu odcinka, s */
    int   line;         /* numer linii w pliku */
    unsigned char rapid;
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
static volatile sig_atomic_t g_running = 1;
static double g_rapid_feed = RAPID_FEED;   /* zmieniany w zakladce Config */

static const double speeds[] = { 1, 2, 5, 10, 20, 50, 100, 200 };
#define N_SPEEDS ((int)(sizeof speeds / sizeof speeds[0]))
#define SPEED_DEFAULT 3   /* x10 - Slower/Faster swieca, gdy speed od tego odbiega */

typedef struct {
    double sim_t;       /* aktualny czas symulacji, s */
    int    playing;
    int    speed;       /* indeks w speeds[] */
    int    cur;         /* indeks biezacego odcinka (cache wyszukiwania) */
} Anim;

enum { ACT_NONE, ACT_EDIT, ACT_RUN, ACT_RESET, ACT_SLOWER, ACT_FASTER, ACT_RELOAD,
       ACT_SAVE, ACT_ERR_CLOSE, ACT_VIEW };

typedef struct {
    XColor bg, label, rapid, cut, air, mat, done, tool, ax[3], grid, shadow, drop, led;
    unsigned long pixels[15];
    int npix;
} Colors;

/* ── parser G-code ─────────────────────────────────────────────────── */

typedef struct {
    double pos[3];
    double feed;        /* mm/min */
    double unit;        /* 1.0 albo 25.4 */
    int    absolute;
    int    motion;      /* 0..3 */
} Machine;

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
    s->line  = line;
}

/* Luk w plaszczyznie XY (G17), Z liniowo (helisa). ijk = srodek wzgledem
 * startu; has_r -> format R. Zwraca 0 przy niepoprawnej geometrii. */
static int path_arc(Path *p, const double st[3], const double en[3],
                    int cw, int has_ij, double i, double j,
                    int has_r, double r, double feed, int line)
{
    double cx, cy;
    if (has_ij) {
        cx = st[0] + i;
        cy = st[1] + j;
    } else if (has_r) {
        double dx = en[0] - st[0], dy = en[1] - st[1];
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
        cx = (st[0] + en[0]) / 2 - side * h * dy / d;
        cy = (st[1] + en[1]) / 2 + side * h * dx / d;
    } else {
        path_err(p, line, "G%d without I/J or R: the arc center is unknown.\n"
                 "Add I/J (center offset from start) or R (radius).",
                 cw ? 2 : 3);
        return 0;
    }

    double rad = hypot(st[0] - cx, st[1] - cy);
    /* jak sterownik: promien do konca musi zgadzac sie z promieniem do
     * startu (tolerancja 0.01 mm albo 0.1% promienia, co wieksze) */
    double rad_end = hypot(en[0] - cx, en[1] - cy);
    if (fabs(rad_end - rad) > fmax(0.01, 0.001 * rad)) {
        path_err(p, line, "Arc end point does not lie on the circle.\n"
                 "Center X%.3f Y%.3f: radius to start %.3f mm,\n"
                 "radius to end %.3f mm (start X%.3f Y%.3f, end X%.3f Y%.3f).\n"
                 "In G91 X/Y are relative: a full circle is X0 Y0.",
                 cx, cy, rad, rad_end, st[0], st[1], en[0], en[1]);
        return 0;
    }
    double a0 = atan2(st[1] - cy, st[0] - cx);
    double a1 = atan2(en[1] - cy, en[0] - cx);
    double sweep = a1 - a0;
    if (cw)  { if (sweep >= -1e-9) sweep -= 2 * M_PI; }
    else     { if (sweep <=  1e-9) sweep += 2 * M_PI; }

    int n = (int)ceil(fabs(sweep) / ARC_STEP);
    if (n < 1) n = 1;
    double prev[3] = { st[0], st[1], st[2] };
    for (int k = 1; k <= n; k++) {
        double t = (double)k / n, pt[3];
        if (k == n) {
            pt[0] = en[0]; pt[1] = en[1];
        } else {
            pt[0] = cx + rad * cos(a0 + sweep * t);
            pt[1] = cy + rad * sin(a0 + sweep * t);
        }
        pt[2] = st[2] + (en[2] - st[2]) * t;
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

static void parse_line(Path *p, Machine *m, char *s, int line)
{
    double v[26] = {0};
    int has[26] = {0};
    int motion = -1, home = 0;

    strip_comments(s);
    for (char *c = s; *c; ) {
        if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') { c++; continue; }
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
            int g = (int)lround(val * 10);   /* G17.1 itp. -> 171 */
            switch (g) {
            case 0: case 10: case 20: case 30: motion = g / 10; break;
            case 170: break;
            case 280: home = 1; break;
            case 200: m->unit = 25.4; break;
            case 210: m->unit = 1.0;  break;
            case 900: m->absolute = 1; break;
            case 910: m->absolute = 0; break;
            default:  p->ignored++; break;
            }
        } else if (L == 'M' - 'A') {
            p->ignored++;
        } else {
            v[L] = val;
            has[L] = 1;
        }
    }

    if (has['F' - 'A'] && v['F' - 'A'] > 0)
        m->feed = v['F' - 'A'] * m->unit;
    if (motion >= 0) m->motion = motion;

    static const int axis[3] = { 'X' - 'A', 'Y' - 'A', 'Z' - 'A' };
    int moved = 0;
    double end[3];
    for (int i = 0; i < 3; i++) {
        end[i] = m->pos[i];
        if (has[axis[i]]) {
            double d = v[axis[i]] * m->unit;
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

    int has_ij = has['I' - 'A'] || has['J' - 'A'];
    if (!moved && !(m->motion >= 2 && has_ij)) return;

    if (m->motion <= 1) {
        path_add(p, m->pos, end, m->motion == 0, m->feed, line);
    } else {
        double i = has['I' - 'A'] ? v['I' - 'A'] * m->unit : 0;
        double j = has['J' - 'A'] ? v['J' - 'A'] * m->unit : 0;
        double r = has['R' - 'A'] ? v['R' - 'A'] * m->unit : 0;
        if (!path_arc(p, m->pos, end, m->motion == 2, has_ij, i, j,
                      has['R' - 'A'], r, m->feed, line))
            return;   /* path_arc juz zapisal blad */
    }
    memcpy(m->pos, end, sizeof end);
}

static int path_load(Path *p, const char *file)
{
    FILE *f = fopen(file, "r");
    memset(p->lo, 0, sizeof p->lo);  /* zakres zawsze obejmuje zero maszyny */
    memset(p->hi, 0, sizeof p->hi);
    p->n = p->truncated = p->ignored = p->errors = p->err_line = 0;
    p->err[0] = '\0';
    p->total_t = 0;
    if (!f) return 0;

    Machine m = { .feed = DEF_FEED, .unit = 1.0, .absolute = 1, .motion = 0 };
    char buf[LINE_MAX_];
    int line = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        size_t len = strlen(buf);
        if (len == sizeof buf - 1 && buf[len - 1] != '\n') {
            int ch;  /* za dluga linia - odrzuc reszte */
            while ((ch = fgetc(f)) != EOF && ch != '\n') ;
            path_err(p, line, "Line is longer than %d characters\n"
                     "and was skipped.", LINE_MAX_ - 2);
            continue;
        }
        parse_line(p, &m, buf, line);
    }
    fclose(f);
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

/* ── rzut izometryczny ─────────────────────────────────────────────── */

typedef struct {
    double scale, ox, oy;   /* ekran = o + (u, -v) * scale */
    int    top;             /* 1 = widok z gory: u = X, v = Y, Z pomijane */
} View;

#define ISO_C 0.8660254037844386   /* cos 30 */
#define ISO_S 0.5                  /* sin 30 */

static void iso(const View *vw, const double p[3], double *u, double *v)
{
    if (vw->top) {
        *u = p[0];
        *v = p[1];
        return;
    }
    *u = (p[0] + p[1]) * ISO_C;
    *v = (p[1] - p[0]) * ISO_S + p[2];
}

/* Dopasowuje rzut bryly [lo, hi] (+ osie o dlugosci axis_len) do r;
 * vw->top musi byc ustawione wczesniej. */
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
} Grid;

/* Siatka w plaszczyznie XY (Z=0) obejmujaca sciezke, krawedzie na
 * wielokrotnosciach kroku. Gdy linii byloby za duzo, krok jest podwajany
 * (tylko na potrzeby rysowania). Zwraca 0, gdy krok jest niepoprawny. */
static int grid_fit(Grid *g, const Path *p, double step)
{
    if (!(step > 0) || !isfinite(step)) return 0;
    for (int tries = 0; tries < 40; tries++, step *= 2) {
        g->x0 = floor(p->lo[0] / step) * step;
        g->x1 = ceil(p->hi[0] / step) * step;
        g->y0 = floor(p->lo[1] / step) * step;
        g->y1 = ceil(p->hi[1] / step) * step;
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

/* Siatka + podzialka w mm: wartosci X wzdluz krawedzi y0, wartosci Y
 * wzdluz krawedzi x1. Podpisywana co n-ta linia, tak by napisy na siebie
 * nie wchodzily (odstep linii na ekranie = step * scale). */
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

    for (long k = 0; k <= nx; k++) {
        double x = g->x0 + k * g->step;
        double a[3] = { x, g->y0, 0 }, b[3] = { x, g->y1, 0 };
        int x0, y0, x1, y1;
        project(vw, a, &x0, &y0);
        project(vw, b, &x1, &y1);
        ui_draw_line(ui, x0, y0, x1, y1, 1, c);
        if (k % every == 0) grid_label(ui, vw, a, x, -4, lc);
    }
    for (long k = 0; k <= ny; k++) {
        double y = g->y0 + k * g->step;
        double a[3] = { g->x0, y, 0 }, b[3] = { g->x1, y, 0 };
        int x0, y0, x1, y1;
        project(vw, a, &x0, &y0);
        project(vw, b, &x1, &y1);
        ui_draw_line(ui, x0, y0, x1, y1, 1, c);
        if (k % every == 0) grid_label(ui, vw, b, y, 4, lc);
    }
}

/* ── rysowanie ─────────────────────────────────────────────────────── */

typedef struct {
    int  show_grid, show_rapid, show_axes;   /* stan ui_checkbox */
    int  show_shadow;  /* cien sciezki na Z=0 */
    int  show_drops;   /* pionowe kreski od punktow do Z=0 + podpis Z */
    int  show_air;     /* kolor ciecia wg Z: nad 0 = powietrze, pod 0 = material */
    int  top_view;     /* 0 = izometria, 1 = widok z gory (XY) */
    char step[16], rapid[16], term[256], editor[256];
    int  step_cur, rapid_cur, term_cur, editor_cur;
    char path[1024];   /* ~/.7a/cnc.conf */
    char msg[1100];    /* wynik ostatniego Save */
} Config;

typedef struct {
    UiCtx      *ui;
    Colors      col;
    Path       *p;
    Anim        an;
    Config      cfg;
    const char *file;
    int         loaded, tab, w, h;
    int         err_open;   /* popup z pierwszym bledem parsera */
} App;

static void draw_axes(UiCtx *ui, const View *vw, const Colors *col, double len)
{
    static const char *names[3] = { "X", "Y", "Z" };
    double o[3] = { 0, 0, 0 };
    int x0, y0;
    project(vw, o, &x0, &y0);
    int naxes = vw->top ? 2 : 3;   /* z gory os Z zapada sie w punkt */
    for (int i = 0; i < naxes; i++) {
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
 * konce nad Z=0); w pozostalych przypadkach (po Z=0) zwykle ciecie. */
static const XColor *seg_color(const Colors *col, const Seg *s, int show_air)
{
    if (s->rapid) return &col->rapid;
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

static void draw_path(UiCtx *ui, const View *vw, const Colors *col, const Path *p,
                      const Anim *an, int started, int show_rapid, int show_air)
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
            ui_draw_line(ui, tx, ty, x1, y1, 1, seg_color(col, s, show_air));
            ui_draw_line(ui, x0, y0, tx, ty, 2, &col->done);
        } else if (started && k < an->cur) {
            ui_draw_line(ui, x0, y0, x1, y1, s->rapid ? 1 : 2, &col->done);
        } else {
            ui_draw_line(ui, x0, y0, x1, y1, 1, seg_color(col, s, show_air));
        }
    }
    if (started) {
        int tx, ty;
        project(vw, tp, &tx, &ty);
        ui_draw_line(ui, tx, ty, tx, ty - 30, 4, &col->tool);  /* trzonek */
        ui_fill_circle(ui, tx, ty, 4, &col->tool);
    }
}

static void fmt_time(char *buf, size_t sz, double s)
{
    long t = (long)(s + 0.5);
    snprintf(buf, sz, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}

static void draw_view(App *a, UiRect vr)
{
    const Path *p = a->p;
    const Config *c = &a->cfg;
    double lo[3], hi[3], ext = 0;
    for (int i = 0; i < 3; i++) {
        lo[i] = p->lo[i];
        hi[i] = p->hi[i];
        if (hi[i] - lo[i] > ext) ext = hi[i] - lo[i];
    }
    Grid g;
    int grid = c->show_grid && grid_fit(&g, p, atof(c->step));
    if (grid) {
        if (g.x0 < lo[0]) lo[0] = g.x0;
        if (g.x1 > hi[0]) hi[0] = g.x1;
        if (g.y0 < lo[1]) lo[1] = g.y0;
        if (g.y1 > hi[1]) hi[1] = g.y1;
    }
    double axis_len = c->show_axes ? (ext > 0 ? ext * 0.2 : 10) : 0;
    View vw = { 0, 0, 0, c->top_view };
    UiRect fr = vr;
    if (grid) {   /* miejsce na podpisy podzialki po bokach i u dolu */
        int in = ui_text_width(a->ui, "-0000.0") + 8;
        fr.x += in;
        fr.w -= 2 * in;
        fr.h -= ui_line_height(a->ui);
        if (fr.w < 10 || fr.h < 10) fr = vr;
    }
    view_fit(&vw, lo, hi, fr, axis_len);
    ui_fill_rect(a->ui, vr, &a->col.bg);
    ui_set_clip(a->ui, vr);
    if (grid) draw_grid(a->ui, &vw, &g, &a->col.grid, &a->col.label);
    if (c->show_axes) draw_axes(a->ui, &vw, &a->col, axis_len);
    /* cien i kreski Z maja sens tylko w izometrii - z gory pokrywaja sciezke */
    if (!vw.top && c->show_shadow) draw_shadow(a->ui, &vw, &a->col, p, c->show_rapid);
    if (!vw.top && c->show_drops) draw_drops(a->ui, &vw, &a->col, p);
    draw_path(a->ui, &vw, &a->col, p, &a->an,
              a->an.sim_t > 0 || a->an.playing, c->show_rapid, c->show_air);
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
        tool_pos(p, &a->an, tp);
        int n = snprintf(st, sizeof st,
                         "%s  |  size %gx%gx%g mm  |  X%.3f Y%.3f Z%.3f  line %d",
                         a->file, p->hi[0] - p->lo[0], p->hi[1] - p->lo[1],
                         p->hi[2] - p->lo[2], tp[0], tp[1], tp[2],
                         p->n > 0 ? p->seg[a->an.cur].line : 0);
        if (n > 0 && (size_t)n < sizeof st && (p->ignored || p->errors || p->truncated))
            snprintf(st + n, sizeof st - (size_t)n, "  |  ignored %d, errors %d%s",
                     p->ignored, p->errors, p->truncated ? ", TRUNCATED" : "");
    }
    ui_label_ellipsis(a->ui, sr, st);
}

/* Zakladka Viewer: pasek przyciskow, widok, pasek stanu - wszystko w r. */
static int draw_viewer(App *a, UiRect r)
{
    enum { NBTN = 6, LED_R = 3 };
    static const char *labels[NBTN] = { "Edit", NULL, "Reset", "Slower", "Faster", "Top" };
    static const int acts[NBTN] = { ACT_EDIT, ACT_RUN, ACT_RESET, ACT_SLOWER,
                                    ACT_FASTER, ACT_VIEW };
    /* -1 = przycisk bez stanu, 0/1 = wylaczony/wlaczony (zolta kropka) */
    int on[NBTN] = { -1, a->an.playing, -1, a->an.speed < SPEED_DEFAULT,
                     a->an.speed > SPEED_DEFAULT, a->cfg.top_view };
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

/* Zakladka Help: legenda linii (lewa kolumna), przyciski i klawisze (prawa). */
static void draw_help(App *a, UiRect r)
{
    static const char *keys[][2] = {
        { "Edit  (e)",          "open the file in the external editor" },
        { "Run / Pause (space)", "start / pause the simulation" },
        { "Reset  (r)",         "move the tool back to the program start" },
        { "Slower / Faster (- +)", "simulation speed (shown as x...)" },
        { "Top  (t)",           "top view (XY) on/off, else isometric" },
        { "yellow dot",         "button is on: Run playing, Slower/Faster" },
        { "",                   "  away from x10, Top view active" },
        { "q, Esc",             "quit (Esc/Enter also close the error popup)" },
        { "Config: Save",       "write settings to ~/.7a/cnc.conf" },
        { "File reload",        "automatic when the edited file is saved" },
    };
    UiCtx *ui = a->ui;
    const Colors *c = &a->col;
    int rh = ui_line_height(ui) + 6, kw = 0;
    UiRect row = { r.x, r.y, r.w, rh };
    UiRect L = ui_rect_col(row, 0, 2, 2 * MARGIN), R = ui_rect_col(row, 1, 2, 2 * MARGIN);

    ui_label_fg(ui, L, "Lines in the view", ui_theme_accent(ui));
    L.y += rh;
    help_swatch(a, L, &c->cut,    1, "cut G1/G2/G3 on Z=0"); L.y += rh;
    help_swatch(a, L, &c->mat,    1, "cut below Z=0 - material is machined"); L.y += rh;
    help_swatch(a, L, &c->air,    1, "cut above Z=0 - cutting air"); L.y += rh;
    help_swatch(a, L, &c->rapid,  1, "rapid move G0 / G28"); L.y += rh;
    help_swatch(a, L, &c->done,   2, "already travelled part of the path"); L.y += rh;
    help_swatch(a, L, &c->tool,   4, "tool (blue dot + shank)"); L.y += rh;
    help_swatch(a, L, &c->shadow, 1, "path shadow on Z=0 (XY position)"); L.y += rh;
    help_swatch(a, L, &c->drop,   1, "drop line to Z=0 with Z value"); L.y += rh;
    help_swatch(a, L, &c->grid,   1, "grid on Z=0, step in mm (Config)"); L.y += rh;
    help_swatch(a, L, &c->ax[0],  1, "axis X"); L.y += rh;
    help_swatch(a, L, &c->ax[1],  1, "axis Y"); L.y += rh;
    help_swatch(a, L, &c->ax[2],  1, "axis Z (hidden in top view)"); L.y += rh;
    ui_label_ellipsis(ui, L, "Line colors by Z and shadow/drops: Config tab.");

    ui_label_fg(ui, R, "Buttons and keys", ui_theme_accent(ui));
    R.y += rh;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        int w = ui_text_width(ui, keys[i][0]);
        if (w > kw) kw = w;
    }
    kw += 2 * MARGIN;
    if (kw > R.w / 2) kw = R.w / 2;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        UiRect kr = { R.x, R.y, kw, rh }, dr = { R.x + kw, R.y, R.w - kw, rh };
        ui_label_ellipsis(ui, kr, keys[i][0]);
        ui_label_ellipsis(ui, dr, keys[i][1]);
        R.y += rh;
    }
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

static void ensure_file(const char *file)
{
    int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;   /* istnieje albo brak katalogu - obie sytuacje OK */
    ssize_t wr = write(fd, sample, sizeof sample - 1);
    (void)wr;
    close(fd);
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
               "rapid_feed=%s\nterminal=%s\neditor=%s\n",
            c->show_grid, c->step, c->show_rapid, c->show_axes,
            c->show_shadow, c->show_drops, c->show_air, c->top_view,
            c->rapid, c->term, c->editor);
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
    }
    fclose(f);
    double r = atof(c->rapid);
    if (r > 0) g_rapid_feed = r;
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
}

static void anim_reset(Anim *an)
{
    an->playing = 0;
    an->sim_t = 0;
    an->cur = 0;
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
    case ACT_RESET:  anim_reset(an); break;
    case ACT_SLOWER: if (an->speed > 0) an->speed--; break;
    case ACT_FASTER: if (an->speed < N_SPEEDS - 1) an->speed++; break;
    case ACT_RELOAD:
        a->loaded = path_load(a->p, a->file);
        a->err_open = a->p->errors > 0;
        anim_reset(an);
        break;
    case ACT_ERR_CLOSE: a->err_open = 0; break;
    case ACT_VIEW:   a->cfg.top_view = !a->cfg.top_view; break;
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
    case XK_r:     return ACT_RESET;
    case XK_e:     return ACT_EDIT;
    case XK_t:     return ACT_VIEW;
    case XK_minus: return ACT_SLOWER;
    case XK_plus: case XK_equal: return ACT_FASTER;
    }
    return ACT_NONE;
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    char file[1024];
    App app = { .w = 800, .h = 600, .p = &g_path, .file = file };

    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fputs("usage: " PROG " [file.nc]\n", stderr);
        return 1;
    }
    if (argc == 2) {
        snprintf(file, sizeof file, "%s", argv[1]);
    } else {
        const char *home = getenv("HOME");
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/.7a", home ? home : ".");
        mkdir(dir, 0755);
        snprintf(file, sizeof file, "%s/cnc.nc", dir);
    }
    ensure_file(file);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs(PROG ": cannot open display\n", stderr); return 1; }
    int screen = DefaultScreen(dpy);
    config_init(&app.cfg, dpy);

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
    time_t mtime = file_mtime(file);
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
            case ButtonPress: case ButtonRelease: case MotionNotify:
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
            time_t mt = file_mtime(file);
            if (mt != mtime) {
                mtime = mt;
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
