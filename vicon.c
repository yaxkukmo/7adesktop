/* vicon.c - parser podzbioru SVG ikon IRIX i ich rysowanie, patrz vicon.h.
 *
 * Parser jest jednoprzebiegowy po calym pliku w pamieci: kazdy tag <g>
 * odklada na stos stan (macierz transformacji + styl), </g> go zdejmuje,
 * <path> przelicza swoje punkty przez biezaca macierz i viewBox od razu
 * do docelowych jednostek (VICON_UNIT na bok viewBoxa), wiec rysowanie to
 * juz tylko mnozenie przez rozmiar ikony. Nieznane tagi sa pomijane. */
#include "vicon.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VICON_MAX_FILE    (1024 * 1024)
#define VICON_MAX_DEPTH   16
#define VICON_MAX_COLORS  32
#define VICON_NO_COLOR    (-1L)   /* fill/stroke: none */

typedef struct {
    double m[6];        /* a b c d e f: x' = a x + c y + e, y' = b x + d y + f */
    long fill, stroke;  /* 0xRRGGBB albo VICON_NO_COLOR */
    double fill_op;     /* 0..1 */
} State;

typedef struct {
    Display *dpy;
    const XColor *bg;
    double vx, vy, vscale;      /* viewBox -> jednostki VICON_UNIT */
    State st[VICON_MAX_DEPTH];
    int depth;
    /* rosnace tablice wyniku (malloc/realloc tylko przy ladowaniu) */
    short *pts;
    int npts, cap_pts;
    ViconPath *paths;
    int npaths, cap_paths;
    long rgb[VICON_MAX_COLORS];
    XColor col[VICON_MAX_COLORS];
    int ncolors;
    int failed;
} Parser;

/* ---- wczytanie pliku ------------------------------------------------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n;

    if (!f) return NULL;
    buf = malloc(VICON_MAX_FILE + 1);
    if (!buf) { fclose(f); return NULL; }
    n = fread(buf, 1, VICON_MAX_FILE + 1, f);
    fclose(f);
    if (n > VICON_MAX_FILE) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

/* ---- atrybuty i liczby ----------------------------------------------- */

/* wartosc atrybutu name w tagu [s, e): wskaznik na poczatek wartosci
 * (po cudzyslowie) i jej dlugosc, NULL gdy brak */
static const char *attr(const char *s, const char *e, const char *name, size_t *len) {
    size_t nl = strlen(name);

    for (const char *p = s; p + nl + 2 < e; p++) {
        if (!isspace((unsigned char) p[0])) continue;
        if (strncmp(p + 1, name, nl) != 0) continue;
        const char *q = p + 1 + nl;
        while (q < e && isspace((unsigned char) *q)) q++;
        if (q >= e || *q != '=') continue;
        q++;
        while (q < e && isspace((unsigned char) *q)) q++;
        if (q >= e || (*q != '"' && *q != '\'')) continue;
        char quote = *q++;
        const char *v = q;
        while (q < e && *q != quote) q++;
        if (q >= e) return NULL;
        *len = (size_t) (q - v);
        return v;
    }
    return NULL;
}

static const char *skip_sep(const char *p, const char *e) {
    while (p < e && (isspace((unsigned char) *p) || *p == ',')) p++;
    return p;
}

/* liczba z [p, e) (strtod nie zna konca bufora - wartosci atrybutow
 * zawsze koncza sie cudzyslowem, ktory strtod zatrzymuje, wiec nie
 * wyjdzie poza e) */
static int number(const char **pp, const char *e, double *out) {
    const char *p = skip_sep(*pp, e);
    char *end;

    if (p >= e) return 0;
    *out = strtod(p, &end);
    if (end == p || end > e) return 0;
    *pp = end;
    return 1;
}

/* ---- styl ------------------------------------------------------------ */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "#rgb" / "#rrggbb" / "none"; 0 = nieobslugiwana wartosc (np. url()) */
static int parse_paint(const char *v, size_t n, long *out) {
    int d[6];

    while (n && isspace((unsigned char) *v)) { v++; n--; }
    while (n && isspace((unsigned char) v[n - 1])) n--;
    if (n == 4 && strncmp(v, "none", 4) == 0) { *out = VICON_NO_COLOR; return 1; }
    if (n != 4 && n != 7) return 0;
    if (v[0] != '#') return 0;
    for (size_t i = 1; i < n; i++)
        if ((d[i - 1] = hexval((unsigned char) v[i])) < 0) return 0;
    if (n == 4)
        *out = (long) (d[0] * 17) << 16 | (long) (d[1] * 17) << 8 | (long) (d[2] * 17);
    else
        *out = (long) (d[0] * 16 + d[1]) << 16 | (long) (d[2] * 16 + d[3]) << 8 |
               (long) (d[4] * 16 + d[5]);
    return 1;
}

/* "0.5" albo "50%" */
static double parse_opacity(const char *v, size_t n) {
    const char *p = v, *e = v + n;
    double x;

    if (!number(&p, e, &x)) return 1.0;
    p = skip_sep(p, e);
    if (p < e && *p == '%') x /= 100.0;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return x;
}

/* jedna wlasciwosc stylu key=value; 0 = nieobslugiwany kolor */
static int apply_prop(State *st, const char *k, size_t kn, const char *v, size_t vn) {
    if (kn == 4 && strncmp(k, "fill", 4) == 0)
        return parse_paint(v, vn, &st->fill);
    if (kn == 6 && strncmp(k, "stroke", 6) == 0)
        return parse_paint(v, vn, &st->stroke);
    if (kn == 12 && strncmp(k, "fill-opacity", 12) == 0)
        st->fill_op = parse_opacity(v, vn);
    return 1;
}

static int apply_style(State *st, const char *v, size_t n) {
    const char *p = v, *e = v + n;

    while (p < e) {
        const char *semi = memchr(p, ';', (size_t) (e - p));
        const char *end = semi ? semi : e;
        const char *colon = memchr(p, ':', (size_t) (end - p));
        if (colon) {
            const char *k = p, *ke = colon;
            while (k < ke && isspace((unsigned char) *k)) k++;
            while (ke > k && isspace((unsigned char) ke[-1])) ke--;
            if (!apply_prop(st, k, (size_t) (ke - k), colon + 1,
                            (size_t) (end - colon - 1)))
                return 0;
        }
        p = end + 1;
    }
    return 1;
}

static int apply_paint_attrs(State *st, const char *s, const char *e) {
    static const char *const names[] = { "fill", "stroke", "fill-opacity" };
    const char *v;
    size_t n;

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if ((v = attr(s, e, names[i], &n)) &&
            !apply_prop(st, names[i], strlen(names[i]), v, n))
            return 0;
    if ((v = attr(s, e, "style", &n)) && !apply_style(st, v, n)) return 0;
    return 1;
}

/* ---- transformacje --------------------------------------------------- */

/* m = m * t (t stosowane do punktow jako pierwsze) */
static void mat_mul(double m[6], const double t[6]) {
    double r[6];

    r[0] = m[0] * t[0] + m[2] * t[1];
    r[1] = m[1] * t[0] + m[3] * t[1];
    r[2] = m[0] * t[2] + m[2] * t[3];
    r[3] = m[1] * t[2] + m[3] * t[3];
    r[4] = m[0] * t[4] + m[2] * t[5] + m[4];
    r[5] = m[1] * t[4] + m[3] * t[5] + m[5];
    memcpy(m, r, sizeof r);
}

/* argumenty "(a b ...)" do max 6 liczb, zwraca ich liczbe, -1 = blad */
static int func_args(const char **pp, const char *e, double *a) {
    const char *p = skip_sep(*pp, e);
    int n = 0;

    if (p >= e || *p != '(') return -1;
    p++;
    for (;;) {
        p = skip_sep(p, e);
        if (p < e && *p == ')') break;
        if (n == 6 || !number(&p, e, &a[n])) return -1;
        n++;
    }
    *pp = p + 1;
    return n;
}

static int apply_transform(State *st, const char *v, size_t len) {
    const char *p = v, *e = v + len;

    for (;;) {
        double a[6], t[6] = { 1, 0, 0, 1, 0, 0 };
        int n;

        p = skip_sep(p, e);
        if (p >= e) return 1;
        if (strncmp(p, "matrix", 6) == 0) {
            p += 6;
            if (func_args(&p, e, a) != 6) return 0;
            memcpy(t, a, sizeof t);
        } else if (strncmp(p, "scale", 5) == 0) {
            p += 5;
            n = func_args(&p, e, a);
            if (n < 1 || n > 2) return 0;
            t[0] = a[0];
            t[3] = n == 2 ? a[1] : a[0];
        } else if (strncmp(p, "translate", 9) == 0) {
            p += 9;
            n = func_args(&p, e, a);
            if (n < 1 || n > 2) return 0;
            t[4] = a[0];
            t[5] = n == 2 ? a[1] : 0;
        } else {
            return 0;   /* rotate/skew - nie wystepuja w ikonach IRIX */
        }
        mat_mul(st->m, t);
    }
}

/* ---- kolory ---------------------------------------------------------- */

static long blend(long rgb, double op, const XColor *bg) {
    long out = 0;

    for (int sh = 16; sh >= 0; sh -= 8) {
        int c = (int) (rgb >> sh & 0xff);
        int b = (sh == 16 ? bg->red : sh == 8 ? bg->green : bg->blue) >> 8;
        out |= (long) (c * op + b * (1.0 - op) + 0.5) << sh;
    }
    return out;
}

static long rgb_dist(long a, long b) {
    long d = 0;

    for (int sh = 16; sh >= 0; sh -= 8) {
        long x = (a >> sh & 0xff) - (b >> sh & 0xff);
        d += x * x;
    }
    return d;
}

/* indeks koloru w tablicy parsera (alokowany przy pierwszym uzyciu);
 * przy pelnej tablicy albo bledzie alokacji - najblizszy juz zajety */
static unsigned char color_index(Parser *ps, long rgb) {
    int best = -1;

    if (rgb == VICON_NO_COLOR) return VICON_NONE;
    for (int i = 0; i < ps->ncolors; i++)
        if (ps->rgb[i] == rgb) return (unsigned char) i;
    if (ps->ncolors < VICON_MAX_COLORS) {
        XColor *c = &ps->col[ps->ncolors];
        c->red = (unsigned short) ((rgb >> 16 & 0xff) * 257);
        c->green = (unsigned short) ((rgb >> 8 & 0xff) * 257);
        c->blue = (unsigned short) ((rgb & 0xff) * 257);
        c->flags = DoRed | DoGreen | DoBlue;
        if (XAllocColor(ps->dpy, DefaultColormap(ps->dpy, DefaultScreen(ps->dpy)), c)) {
            ps->rgb[ps->ncolors] = rgb;
            return (unsigned char) ps->ncolors++;
        }
    }
    for (int i = 0; i < ps->ncolors; i++)
        if (best < 0 || rgb_dist(ps->rgb[i], rgb) < rgb_dist(ps->rgb[best], rgb))
            best = i;
    return best < 0 ? VICON_NONE : (unsigned char) best;
}

/* ---- sciezki --------------------------------------------------------- */

static short to_unit(double v) {
    v = v < -32767 ? -32767 : v > 32767 ? 32767 : v;
    return (short) (v < 0 ? v - 0.5 : v + 0.5);
}

static int add_point(Parser *ps, ViconPath *cur, double x, double y) {
    const double *m = ps->st[ps->depth].m;
    double tx = m[0] * x + m[2] * y + m[4];
    double ty = m[1] * x + m[3] * y + m[5];

    if (cur->n >= VICON_MAX_PATH_PTS) return 1;     /* obciecie */
    if (ps->npts == ps->cap_pts) {
        int cap = ps->cap_pts ? ps->cap_pts * 2 : 256;
        short *p = realloc(ps->pts, (size_t) cap * 2 * sizeof *p);
        if (!p) return 0;
        ps->pts = p;
        ps->cap_pts = cap;
    }
    ps->pts[2 * ps->npts] = to_unit((tx - ps->vx) * ps->vscale);
    ps->pts[2 * ps->npts + 1] = to_unit((ty - ps->vy) * ps->vscale);
    ps->npts++;
    cur->n++;
    return 1;
}

static int finish_subpath(Parser *ps, ViconPath *cur) {
    if (cur->n < 2) {           /* sam punkt - nic do rysowania */
        ps->npts -= cur->n;
        cur->n = 0;
        return 1;
    }
    if (ps->npaths == ps->cap_paths) {
        int cap = ps->cap_paths ? ps->cap_paths * 2 : 32;
        ViconPath *p = realloc(ps->paths, (size_t) cap * sizeof *p);
        if (!p) return 0;
        ps->paths = p;
        ps->cap_paths = cap;
    }
    ps->paths[ps->npaths++] = *cur;
    cur->n = 0;
    return 1;
}

/* dane d: M/L/H/V/Z (i male = wzgledne); inne polecenie = 0 */
static int parse_d(Parser *ps, ViconPath tmpl, const char *p, const char *e) {
    ViconPath cur = tmpl;
    double x = 0, y = 0, sx = 0, sy = 0;
    char cmd = 0;

    cur.first = ps->npts;
    for (;;) {
        double a, b;

        p = skip_sep(p, e);
        if (p >= e) break;
        if (isalpha((unsigned char) *p)) {
            cmd = *p++;
            if (cmd == 'Z' || cmd == 'z') {
                cur.closed = 1;
                if (!finish_subpath(ps, &cur)) return 0;
                cur = tmpl;
                cur.first = ps->npts;
                x = sx; y = sy;
                cmd = 0;
                continue;
            }
            if (!strchr("MmLlHhVv", cmd)) return 0;
            continue;
        }
        if (!cmd) return 0;
        int rel = islower((unsigned char) cmd);
        switch (toupper((unsigned char) cmd)) {
        case 'M':
            if (!number(&p, e, &a) || !number(&p, e, &b)) return 0;
            if (!finish_subpath(ps, &cur)) return 0;
            cur = tmpl;
            cur.first = ps->npts;
            x = rel ? x + a : a;
            y = rel ? y + b : b;
            sx = x; sy = y;
            cmd = rel ? 'l' : 'L';  /* kolejne pary po M = L */
            break;
        case 'L':
            if (!number(&p, e, &a) || !number(&p, e, &b)) return 0;
            if (cur.n == 0 && !add_point(ps, &cur, x, y)) return 0;
            x = rel ? x + a : a;
            y = rel ? y + b : b;
            break;
        case 'H':
            if (!number(&p, e, &a)) return 0;
            if (cur.n == 0 && !add_point(ps, &cur, x, y)) return 0;
            x = rel ? x + a : a;
            break;
        default: /* V */
            if (!number(&p, e, &a)) return 0;
            if (cur.n == 0 && !add_point(ps, &cur, x, y)) return 0;
            y = rel ? y + a : a;
            break;
        }
        if (!add_point(ps, &cur, x, y)) return 0;
    }
    return finish_subpath(ps, &cur);
}

/* ---- tagi ------------------------------------------------------------ */

static int tag_is(const char *p, const char *name) {
    size_t n = strlen(name);
    return strncmp(p, name, n) == 0 &&
           (isspace((unsigned char) p[n]) || p[n] == '>' || p[n] == '/');
}

static int tag_svg(Parser *ps, const char *s, const char *e) {
    size_t n;
    const char *v = attr(s, e, "viewBox", &n);
    double vb[4];

    if (!v) return 1;   /* bez viewBox: jednostki uzytkownika 1:1 */
    const char *p = v, *ve = v + n;
    for (int i = 0; i < 4; i++)
        if (!number(&p, ve, &vb[i])) return 0;
    if (vb[2] <= 0 || vb[3] <= 0) return 0;
    ps->vx = vb[0];
    ps->vy = vb[1];
    ps->vscale = VICON_UNIT / (vb[2] > vb[3] ? vb[2] : vb[3]);
    return 1;
}

static int apply_common(State *st, const char *s, const char *e) {
    size_t n;
    const char *v;

    if (!apply_paint_attrs(st, s, e)) return 0;
    if ((v = attr(s, e, "transform", &n)) && !apply_transform(st, v, n)) return 0;
    return 1;
}

static int tag_g(Parser *ps, const char *s, const char *e) {
    if (ps->depth + 1 >= VICON_MAX_DEPTH) return 0;
    ps->st[ps->depth + 1] = ps->st[ps->depth];
    ps->depth++;
    return apply_common(&ps->st[ps->depth], s, e);
}

static int tag_path(Parser *ps, const char *s, const char *e) {
    size_t n;
    const char *d = attr(s, e, "d", &n);
    ViconPath tmpl;
    State *st;
    int ok;

    if (!d || ps->depth + 1 >= VICON_MAX_DEPTH) return d != NULL ? 0 : 1;
    /* styl i transform sciezki dzialaja tylko na nia - tymczasowy poziom */
    ps->st[ps->depth + 1] = ps->st[ps->depth];
    ps->depth++;
    st = &ps->st[ps->depth];
    ok = apply_common(st, s, e);
    if (ok) {
        long fill = st->fill;
        if (fill != VICON_NO_COLOR && st->fill_op < 1.0)
            fill = blend(fill, st->fill_op, ps->bg);
        memset(&tmpl, 0, sizeof tmpl);
        tmpl.fill = st->fill_op > 0 ? color_index(ps, fill) : VICON_NONE;
        tmpl.stroke = color_index(ps, st->stroke);
        if (tmpl.fill != VICON_NONE || tmpl.stroke != VICON_NONE)
            ok = parse_d(ps, tmpl, d, d + n);
    }
    ps->depth--;
    return ok;
}

/* koniec tagu zaczynajacego sie w p ('<'), z pominieciem cudzyslowow */
static const char *tag_end(const char *p) {
    char q = 0;

    for (; *p; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

static int parse_doc(Parser *ps, const char *p) {
    while ((p = strchr(p, '<')) != NULL) {
        const char *e;

        if (strncmp(p, "<!--", 4) == 0) {
            e = strstr(p + 4, "-->");
            if (!e) return 0;
            p = e + 3;
            continue;
        }
        if (!(e = tag_end(p))) return 0;
        int self_close = e > p && e[-1] == '/';
        if (tag_is(p + 1, "svg")) {
            if (!tag_svg(ps, p, e)) return 0;
        } else if (tag_is(p + 1, "g")) {
            if (!tag_g(ps, p, e)) return 0;
            if (self_close) ps->depth--;
        } else if (tag_is(p + 1, "/g")) {
            if (ps->depth > 0) ps->depth--;
        } else if (tag_is(p + 1, "path")) {
            if (!tag_path(ps, p, e)) return 0;
        } else if (tag_is(p + 1, "linearGradient") || tag_is(p + 1, "radialGradient") ||
                   tag_is(p + 1, "text") || tag_is(p + 1, "image")) {
            return 0;
        }
        p = e + 1;
    }
    return 1;
}

/* ---- API ------------------------------------------------------------- */

static void free_parser_colors(Display *dpy, XColor *col, int n) {
    unsigned long pixels[VICON_MAX_COLORS];

    if (n <= 0) return;
    for (int i = 0; i < n; i++) pixels[i] = col[i].pixel;
    XFreeColors(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), pixels, n, 0);
}

int vicon_load(Display *dpy, const char *path, const XColor *bg, Vicon *out) {
    static Parser ps;   /* duza (stos stanow) - static zamiast na stosie */
    char *doc;
    int ok;

    memset(out, 0, sizeof *out);
    if (!(doc = read_file(path))) return 0;
    memset(&ps, 0, sizeof ps);
    ps.dpy = dpy;
    ps.bg = bg;
    ps.vscale = 1.0;
    ps.st[0].m[0] = ps.st[0].m[3] = 1.0;
    ps.st[0].fill = 0;                      /* domyslny fill SVG: czarny */
    ps.st[0].stroke = VICON_NO_COLOR;
    ps.st[0].fill_op = 1.0;
    ok = parse_doc(&ps, doc) && ps.npaths > 0;
    free(doc);
    if (ok && !(out->colors = malloc((size_t) ps.ncolors * sizeof *out->colors + 1)))
        ok = 0;
    if (!ok) {
        free(ps.pts);
        free(ps.paths);
        free_parser_colors(dpy, ps.col, ps.ncolors);
        return 0;
    }
    memcpy(out->colors, ps.col, (size_t) ps.ncolors * sizeof *out->colors);
    out->pts = ps.pts;
    out->paths = ps.paths;
    out->npts = ps.npts;
    out->npaths = ps.npaths;
    out->ncolors = ps.ncolors;
    return 1;
}

void vicon_draw(UiCtx *ctx, const Vicon *v, UiRect r) {
    XPoint buf[VICON_MAX_PATH_PTS];
    int size = r.w < r.h ? r.w : r.h;
    int ox, oy;

    if (!v->npaths || size <= 0) return;
    ox = r.x + (r.w - size) / 2;
    oy = r.y + (r.h - size) / 2;
    for (int i = 0; i < v->npaths; i++) {
        const ViconPath *pa = &v->paths[i];
        const short *s = v->pts + 2 * pa->first;
        int n = pa->n;

        for (int k = 0; k < n; k++) {
            buf[k].x = (short) (ox + (s[2 * k] * size + VICON_UNIT / 2) / VICON_UNIT);
            buf[k].y = (short) (oy + (s[2 * k + 1] * size + VICON_UNIT / 2) / VICON_UNIT);
        }
        if (pa->fill != VICON_NONE)
            ui_fill_polygon(ctx, buf, n, &v->colors[pa->fill]);
        if (pa->stroke != VICON_NONE)
            ui_draw_polyline(ctx, buf, n, pa->closed, &v->colors[pa->stroke]);
    }
}

void vicon_free(Display *dpy, Vicon *v) {
    free_parser_colors(dpy, v->colors, v->ncolors);
    free(v->pts);
    free(v->paths);
    free(v->colors);
    memset(v, 0, sizeof *v);
}
