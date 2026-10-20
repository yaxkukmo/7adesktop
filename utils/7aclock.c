/* 7aclock - analog X11 clock with optional date display inside the face
 * inspired by xclock and urxvclock
 *
 * Sleep timing: without the seconds hand we sync to the minute boundary
 * via clock_gettime(); with it we sync to the second boundary.  Either
 * way we sleep precisely until the next hand movement is visible, so CPU
 * usage stays near zero between frames.
 *
 * Cairo removed: all drawing uses pure Xlib.  Transparency (-alpha) is
 * not supported.
 *
 * Port na ui.c/ui.h (wczesniej jedyna apka 7a* bez libui.a, z wlasnymi
 * kopiami UTF-8->UCS-2, zaokraglonych rogow, backbuffera i odczytu
 * motywu): tlo okna, pole tarczy (boxBackground), obramowanie ramki daty
 * (lineForeground), font daty (uiFont) i cornerRadius pochodza z motywu
 * ui_init - dokladnie jak w pozostalych apkach, takze domyslne kolory przy
 * pustej bazie zasobow. Usuniete flagi -bg/-fg/-boxbg/-font (zastapione
 * zasobami globalnymi); zostaly tylko kolory specyficzne dla zegara
 * (color_defs ponizej), kazdy z zasobem 7aclock.<nazwa> (klasa
 * 7aClock.<Nazwa>) i flaga CLI, domyslnie dziedziczony z motywu.
 * Wskazowka = czworokat wypukly rysowany jako dwa ui_fill_triangle.
 * Zasoby czyta tylko RESOURCE_MANAGER (xrdb), jak ui.c - plik
 * ~/.Xresources i $XENVIRONMENT nie sa juz czytane bezposrednio.
 */

/* clock_gettime/CLOCK_* (POSIX) i M_PI (XSI/BSD) sa poza ISO C99 -
 * -std=c99 w Makefile ukrywa je w glibc bez tego makra, chyba ze
 * wlaczymy je jawnie; na OpenBSD nie ma to wplywu (tam sa widoczne
 * niezaleznie) - patrz ta sama uwaga w utils/7aweather.c. */
#define _DEFAULT_SOURCE

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>

#include "../ui.h"

#define PI       M_PI
#define PROG     "7aclock"
#define DEF_SIZE 150
#define DEF_PAD  4
#define DEF_FONT "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1"

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

/* ── colors ─────────────────────────────────────────────────────────── */

enum { C_HANDS, C_SEC_HAND, C_DATE_FG, C_DATE_BG, C_RING, C_HTICK, C_MTICK,
       C_COUNT };

typedef struct {
    const char *name;   /* zasob "7aclock.<name>" */
    const char *klass;  /* klasa "7aClock.<klass>" */
    const char *opt;    /* flaga CLI */
    const char *dflt;   /* "" = kolor z motywu (inherit) */
    const XColor *(*inherit)(UiCtx *);
} ColorDef;

static const ColorDef color_defs[C_COUNT] = {
    [C_HANDS]    = { "hands",      "Hands",      "-hd",    "",        ui_theme_fg },
    [C_SEC_HAND] = { "secondHand", "SecondHand", "-sd",    "#e05050", ui_theme_fg },
    [C_DATE_FG]  = { "dateFg",     "DateFg",     "-dc",    "",        ui_theme_fg },
    [C_DATE_BG]  = { "dateBg",     "DateBg",     "-db",    "",        ui_theme_box_bg },
    [C_RING]     = { "ring",       "Ring",       "-ring",  "",        ui_theme_fg },
    [C_HTICK]    = { "hourTick",   "HourTick",   "-htick", "",        ui_theme_fg },
    [C_MTICK]    = { "minuteTick", "MinuteTick", "-mtick", "",        ui_theme_fg },
};

/* ── types ──────────────────────────────────────────────────────────── */

typedef struct {
    int    show_date;
    const char *date_fmt;
    int    padding;
    int    noseconds;
    int    noring;
    int    update_ms;
    const char *title;
    char  *name;
    char  *wm_class;
    char   spec[C_COUNT][64];  /* zasob/flaga, "" = z motywu */
    XColor color[C_COUNT];     /* rozwiazane w resolve_colors */
    /* piksele zaalokowane przez resolve_colors (kolory z motywu naleza do
     * ui.c) - zwalniane XFreeColors na koncu main */
    unsigned long owned[C_COUNT];
    int    n_owned;
} Cfg;

/* -geometry: flagi XParseGeometry + offsety */
typedef struct {
    int flags, x, y;
} Geom;

/* zasoby czytane PRZED flagami CLI, zeby flagi wygrywaly */
static void read_color_specs(Display *dpy, Cfg *cfg)
{
    char name[64], klass[64];

    for (int i = 0; i < C_COUNT; i++) {
        snprintf(name,  sizeof(name),  "7aclock.%s", color_defs[i].name);
        snprintf(klass, sizeof(klass), "7aClock.%s", color_defs[i].klass);
        ui_resource_string(dpy, name, klass, cfg->spec[i],
                           sizeof(cfg->spec[i]), color_defs[i].dflt);
    }
}

static int color_opt(const char *arg)
{
    for (int i = 0; i < C_COUNT; i++)
        if (!strcmp(arg, color_defs[i].opt))
            return i;
    return -1;
}

static void resolve_colors(UiCtx *ui, Cfg *cfg)
{
    for (int i = 0; i < C_COUNT; i++) {
        if (cfg->spec[i][0] && ui_color(ui, cfg->spec[i], &cfg->color[i])) {
            cfg->owned[cfg->n_owned++] = cfg->color[i].pixel;
            continue;
        }
        if (cfg->spec[i][0])
            fprintf(stderr, PROG ": bad color '%s'\n", cfg->spec[i]);
        cfg->color[i] = *color_defs[i].inherit(ui);
    }
}

/* ── sleep timing ───────────────────────────────────────────────────── */

static struct timeval next_tick(int noseconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    long usec     = ts.tv_nsec / 1000;
    long sleep_us;

    if (noseconds) {
        time_t next_min = (ts.tv_sec / 60 + 1) * 60;
        sleep_us = (long)(next_min - ts.tv_sec) * 1000000L - usec;
    } else {
        sleep_us = 1000000L - usec;
    }

    if (sleep_us <= 0)
        sleep_us += noseconds ? 60000000L : 1000000L;

    struct timeval tv = {
        .tv_sec  = (time_t)(sleep_us / 1000000),
        .tv_usec = (suseconds_t)(sleep_us % 1000000),
    };
    return tv;
}

/* ── title ──────────────────────────────────────────────────────────── */

static void update_title(Display *dpy, Window win, const char *fmt)
{
    time_t now = time(NULL);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, localtime(&now));

    XStoreName(dpy, win, buf);
    XSetIconName(dpy, win, buf);

    static Atom net_wm_name      = None;
    static Atom net_wm_icon_name = None;
    static Atom utf8_string      = None;
    if (net_wm_name == None) {
        net_wm_name      = XInternAtom(dpy, "_NET_WM_NAME",      False);
        net_wm_icon_name = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
        utf8_string      = XInternAtom(dpy, "UTF8_STRING",       False);
    }
    XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
        (const unsigned char *)buf, (int)strlen(buf));
    XChangeProperty(dpy, win, net_wm_icon_name, utf8_string, 8, PropModeReplace,
        (const unsigned char *)buf, (int)strlen(buf));
}

/* ── geometry ───────────────────────────────────────────────────────── */

/* pozycja okna z -geometry; ex = _NET_FRAME_EXTENTS (left, right, top,
 * bottom) do korekty o ramke WM albo NULL */
static void geom_position(Display *dpy, const Geom *g, int w, int h,
                          const long *ex, int *x, int *y)
{
    int  scr = DefaultScreen(dpy);
    long l = ex ? ex[0] : 0, r = ex ? ex[1] : 0;
    long t = ex ? ex[2] : 0, b = ex ? ex[3] : 0;

    if (g->flags & XValue)
        *x = (g->flags & XNegative)
            ? DisplayWidth(dpy, scr) + g->x - w - (int)r
            : g->x + (int)l;
    if (g->flags & YValue)
        *y = (g->flags & YNegative)
            ? DisplayHeight(dpy, scr) + g->y - h - (int)b
            : g->y + (int)t;
}

/* 1 = ex[0..3] wypelnione z _NET_FRAME_EXTENTS okna */
static int read_frame_extents(Display *dpy, Window win, Atom prop, long ex[4])
{
    Atom at; int fmt; unsigned long n, ba;
    unsigned char *data = NULL;
    int ok = 0;

    if (XGetWindowProperty(dpy, win, prop, 0, 4, False, XA_CARDINAL,
                           &at, &fmt, &n, &ba, &data) == Success
        && data && n >= 4) {
        memcpy(ex, data, 4 * sizeof(long));
        ok = 1;
    }
    if (data) XFree(data);
    return ok;
}

/* ── drawing ────────────────────────────────────────────────────────── */

/* Filled lance-shaped hand: pointed at both ends, widest at wp*R from the
 * pivot toward the tip.  front/back/wp/hw are all fractions of R.
 * Czworokat (wypukly) jako dwa trojkaty ze wspolna przekatna tyl-czubek. */
static void fill_hand(UiCtx *ui, const XColor *c,
                      double cx, double cy, double R,
                      double angle, double front, double back,
                      double wp, double hw)
{
    double ca = cos(angle), sa = sin(angle);
    int x0 = (int)(cx - back*R*ca),          y0 = (int)(cy - back*R*sa);
    int x1 = (int)(cx + wp*R*ca - hw*R*sa),  y1 = (int)(cy + wp*R*sa + hw*R*ca);
    int x2 = (int)(cx + front*R*ca),         y2 = (int)(cy + front*R*sa);
    int x3 = (int)(cx + wp*R*ca + hw*R*sa),  y3 = (int)(cy + wp*R*sa - hw*R*ca);

    ui_fill_triangle(ui, x0, y0, x1, y1, x2, y2, c);
    ui_fill_triangle(ui, x0, y0, x2, y2, x3, y3, c);
}

/* pole tarczy w kolorze boxow + obwodka; srodek/promien zaokraglone do
 * calych pikseli (ui_fill_circle) */
static void draw_ring(UiCtx *ui, const Cfg *cfg, double cx, double cy, double R)
{
    int lw = (int)(R * 0.03);
    if (lw < 1) lw = 1;
    int icx = (int)lround(cx), icy = (int)lround(cy), ir = (int)lround(R);

    ui_fill_circle(ui, icx, icy, ir, ui_theme_box_bg(ui));
    ui_draw_circle(ui, icx, icy, ir, lw, &cfg->color[C_RING]);
}

/* n kresek co 2*PI/n (z pominieciem co skip-tej, 0 = bez pomijania) od
 * r0*R do r1*R */
static void draw_ticks(UiCtx *ui, const XColor *c, double cx, double cy,
                       double R, int n, int skip, double r0, double r1,
                       double width)
{
    int lw = (int)(R * width);
    if (lw < 1) lw = 1;

    for (int i = 0; i < n; i++) {
        if (skip && i % skip == 0) continue;
        double a = i * (2.0 * PI / n) - PI * 0.5;
        double ca = cos(a), sa = sin(a);
        ui_draw_line(ui,
                     (int)(cx + R * r0 * ca), (int)(cy + R * r0 * sa),
                     (int)(cx + R * r1 * ca), (int)(cy + R * r1 * sa),
                     lw, c);
    }
}

/* ramka z data w okolicy szostej - jak box w apkach ui.c: tlo, obwodka
 * lineForeground, tekst foreground (o ile nie nadpisane dateBg/dateFg) */
static void draw_date(UiCtx *ui, const Cfg *cfg, const struct tm *tm,
                      double cx, double cy, double R)
{
    char text[128];
    if (strftime(text, sizeof(text), cfg->date_fmt, tm) == 0) return;

    int pad = (int)(R * 0.05);
    if (pad < 1) pad = 1;
    int blw = (int)(R * 0.012);
    if (blw < 1) blw = 1;

    UiRect r;
    r.w = ui_text_width(ui, text) + pad * 2;
    r.h = ui_line_height(ui) + pad * 2;
    r.x = (int)(cx - r.w * 0.5);
    r.y = (int)(cy + R * 0.42 - r.h * 0.5);

    ui_fill_round_rect(ui, r, &cfg->color[C_DATE_BG]);
    ui_draw_round_border(ui, r, blw, ui_theme_line_fg(ui));
    ui_label_centered_fg(ui, r, text, &cfg->color[C_DATE_FG]);
}

static void draw_hands(UiCtx *ui, const Cfg *cfg, const struct tm *tm,
                       double cx, double cy, double R)
{
    const XColor *hands = &cfg->color[C_HANDS];

    /* minute hand — lance: wide at 0.08R ahead of pivot, tapers to both tips */
    double min_a = ((tm->tm_min + tm->tm_sec / 60.0) / 60.0) * 2.0 * PI - PI * 0.5;
    fill_hand(ui, hands, cx, cy, R, min_a, 0.76, 0.12, 0.08, 0.028);

    /* hour hand — wider and stubbier lance */
    double hr_a = ((tm->tm_hour % 12 + tm->tm_min / 60.0) / 12.0) * 2.0 * PI - PI * 0.5;
    fill_hand(ui, hands, cx, cy, R, hr_a, 0.55, 0.12, 0.06, 0.040);

    /* seconds hand — thin needle lance, symmetric (widest at pivot) */
    if (!cfg->noseconds) {
        double sec_a = (tm->tm_sec / 60.0) * 2.0 * PI - PI * 0.5;
        fill_hand(ui, &cfg->color[C_SEC_HAND], cx, cy, R, sec_a,
                  0.87, 0.20, 0.0, 0.011);
    }

    /* center cap */
    int cap_r = (int)(R * 0.045);
    if (cap_r < 1) cap_r = 1;
    ui_fill_circle(ui, (int)cx, (int)cy, cap_r, hands);
}

static void draw(UiCtx *ui, const Cfg *cfg, int w, int h)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);

    double cx = w * 0.5;
    double cy = h * 0.5;
    double R  = ((w < h) ? w : h) * 0.5 - cfg->padding;
    if (R < 1.0) R = 1.0;

    ui_begin_frame(ui);
    if (!cfg->noring)
        draw_ring(ui, cfg, cx, cy, R);
    draw_ticks(ui, &cfg->color[C_HTICK], cx, cy, R, 12, 0, 0.80, 0.92, 0.045);
    draw_ticks(ui, &cfg->color[C_MTICK], cx, cy, R, 60, 5, 0.87, 0.92, 0.015);
    if (cfg->show_date)
        draw_date(ui, cfg, tm, cx, cy, R);
    draw_hands(ui, cfg, tm, cx, cy, R);
    ui_end_frame(ui);
}

/* ── argument helpers ───────────────────────────────────────────────── */

static void usage(void)
{
    fputs(
        "Usage: " PROG " [options]\n"
        "  -date              show date inside clock face\n"
        "  -dateformat FMT    strftime format (default: \"%d %b\")\n"
        "  -noseconds         hide seconds hand (wakes once/minute)\n"
        "  -noring            hide outer ring of the clock face\n"
        "  -hd COLOR          hour+minute hands color  (default: foreground)\n"
        "  -sd COLOR          seconds hand color       (default: #e05050)\n"
        "  -dc COLOR          date text color          (default: foreground)\n"
        "  -db COLOR          date box background      (default: boxBackground)\n"
        "  -ring COLOR        outer ring color         (default: foreground)\n"
        "  -htick COLOR       hour tick marks color    (default: foreground)\n"
        "  -mtick COLOR       minute tick marks color  (default: foreground)\n"
        "  -padding N         padding pixels           (default: 4)\n"
        "  -title FMT         window title; strftime formats allowed\n"
        "                     e.g. \"%H:%M\" shows current time as title\n"
        "  -name NAME         WM_CLASS instance name (default: 7aclock)\n"
        "  -class CLASS       WM_CLASS class name    (default: 7aclock)\n"
        "  -geometry WxH+X+Y  window geometry\n"
        "  -update MS         fixed redraw interval ms (overrides auto-sync)\n"
        "  -help              this message\n"
        "  q / Escape         quit\n"
        "Background, face fill, date box border, font and corner radius come\n"
        "from the shared X resources (background, boxBackground,\n"
        "lineForeground, uiFont, cornerRadius) - see Xresources.sample.\n",
        stderr);
}

static int safe_atoi(const char *s, int lo, int hi, const char *name)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi) {
        fprintf(stderr, PROG ": invalid %s '%s' (range %d..%d)\n",
                name, s, lo, hi);
        exit(1);
    }
    return (int)v;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    Geom geom = {0};
    int  win_w = DEF_SIZE, win_h = DEF_SIZE;

    Cfg cfg = {
        .show_date = 0,
        .date_fmt  = "%d %b",
        .padding   = DEF_PAD,
        .noseconds = 0,
        .update_ms = 0,
        .title     = PROG,
        .name      = PROG,
        .wm_class  = PROG,
    };

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs(PROG ": cannot open display\n", stderr); return 1; }

    int    screen = DefaultScreen(dpy);
    Window root   = RootWindow(dpy, screen);

    read_color_specs(dpy, &cfg);

    for (int i = 1; i < argc; i++) {
        int ci;
#define NEED(opt) \
        if (i + 1 >= argc) { \
            fprintf(stderr, PROG ": %s requires an argument\n", opt); \
            XCloseDisplay(dpy); return 1; \
        } ++i
        if      (!strcmp(argv[i], "-date"))      cfg.show_date = 1;
        else if (!strcmp(argv[i], "-noseconds")) cfg.noseconds = 1;
        else if (!strcmp(argv[i], "-noring"))    cfg.noring    = 1;
        else if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help")) {
            usage(); XCloseDisplay(dpy); return 0;
        }
        else if (!strcmp(argv[i], "-dateformat")) {
            NEED("-dateformat"); cfg.date_fmt = argv[i];
        }
        else if (!strcmp(argv[i], "-title"))  { NEED("-title");  cfg.title    = argv[i]; }
        else if (!strcmp(argv[i], "-name"))   { NEED("-name");   cfg.name     = argv[i]; }
        else if (!strcmp(argv[i], "-class"))  { NEED("-class");  cfg.wm_class = argv[i]; }
        else if (!strcmp(argv[i], "-geometry")) {
            NEED("-geometry");
            unsigned int uw = 0, uh = 0;
            geom.flags = XParseGeometry(argv[i], &geom.x, &geom.y, &uw, &uh);
            if ((geom.flags & WidthValue)  && uw > 0) win_w = (int)uw;
            if ((geom.flags & HeightValue) && uh > 0) win_h = (int)uh;
        }
        else if (!strcmp(argv[i], "-update")) {
            NEED("-update");
            cfg.update_ms = safe_atoi(argv[i], 100, 60000, "-update");
        }
        else if (!strcmp(argv[i], "-padding")) {
            NEED("-padding");
            cfg.padding = safe_atoi(argv[i], 0, 500, "-padding");
        }
        else if ((ci = color_opt(argv[i])) >= 0) {
            NEED(color_defs[ci].opt);
            snprintf(cfg.spec[ci], sizeof(cfg.spec[ci]), "%s", argv[i]);
        }
        else {
            fprintf(stderr, PROG ": unknown option '%s'\n", argv[i]);
            usage(); XCloseDisplay(dpy); return 1;
        }
#undef NEED
    }

    int win_x = 0, win_y = 0;
    geom_position(dpy, &geom, win_w, win_h, NULL, &win_x, &win_y);

    /* ── window ────────────────────────────────────────────────────── */
    /* tlo None - kolor tla znany dopiero po ui_init, ustawiany nizej */
    XSetWindowAttributes attr = {
        .background_pixmap = None,
        .event_mask        = ExposureMask | StructureNotifyMask
                           | KeyPressMask | PropertyChangeMask,
    };
    Window win = XCreateWindow(dpy, root,
        win_x, win_y, (unsigned)win_w, (unsigned)win_h, 0,
        DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
        CWBackPixmap | CWEventMask, &attr);

    /* GC: wlasnosc main, XFreeGC na koncu (ui.c go tylko uzywa) */
    GC gc = XCreateGC(dpy, win, 0, NULL);
    UiCtx *ui = ui_init(dpy, win, gc, DEF_FONT, win_w, win_h);
    if (!ui) {
        fputs(PROG ": ui_init failed (no font?)\n", stderr);
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }
    XSetWindowBackground(dpy, win, ui_theme_bg(ui)->pixel);
    resolve_colors(ui, &cfg);

    XClassHint ch = { .res_name = cfg.name, .res_class = cfg.wm_class };
    XSetClassHint(dpy, win, &ch);

    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    /* _NET_FRAME_EXTENTS: request frame extents before mapping so we can
     * correct geometry position.  Falls back to PropertyNotify post-map.  */
    Atom a_frame_extents = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    Atom a_req_frame     = XInternAtom(dpy, "_NET_REQUEST_FRAME_EXTENTS", True);
    int  frame_adj_done  = 0;
    int  want_pos        = geom.flags & (XValue | YValue);

    if (a_req_frame != None && want_pos) {
        XEvent fev = {0};
        fev.xclient.type         = ClientMessage;
        fev.xclient.window       = win;
        fev.xclient.message_type = a_req_frame;
        fev.xclient.format       = 32;
        XSendEvent(dpy, root, False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &fev);
        XFlush(dpy);

        int pre_fd = ConnectionNumber(dpy);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!frame_adj_done) {
            struct timespec tn;
            clock_gettime(CLOCK_MONOTONIC, &tn);
            long ms = (tn.tv_sec - t0.tv_sec) * 1000L
                    + (tn.tv_nsec - t0.tv_nsec) / 1000000L;
            if (ms >= 200) break;
            fd_set fds; FD_ZERO(&fds); FD_SET(pre_fd, &fds);
            long rem = 200 - ms;
            struct timeval tv = { rem / 1000, (rem % 1000) * 1000 };
            if (select(pre_fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
            while (XPending(dpy)) {
                XEvent pev;
                long   ex[4];
                XNextEvent(dpy, &pev);
                if (pev.type != PropertyNotify
                    || pev.xproperty.atom != a_frame_extents) continue;
                if (read_frame_extents(dpy, win, a_frame_extents, ex)) {
                    geom_position(dpy, &geom, win_w, win_h, ex, &win_x, &win_y);
                    frame_adj_done = 1;
                }
            }
        }
    }

    XSizeHints sh = {
        .flags     = PMinSize,
        .min_width = 50, .min_height = 50,
    };
    if (want_pos) {
        sh.flags |= USPosition;
        sh.x = win_x;
        sh.y = win_y;
    }
    XSetNormalHints(dpy, win, &sh);

    XMapWindow(dpy, win);
    XFlush(dpy);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    update_title(dpy, win, cfg.title);
    draw(ui, &cfg, win_w, win_h);

    int xfd = ConnectionNumber(dpy);

    while (g_running) {
        struct timeval tv;
        if (cfg.update_ms > 0) {
            tv.tv_sec  = cfg.update_ms / 1000;
            tv.tv_usec = (cfg.update_ms % 1000) * 1000;
        } else {
            tv = next_tick(cfg.noseconds);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int sel = select(xfd + 1, &fds, NULL, NULL, &tv);
        if (sel < 0 && errno == EINTR) continue;

        /* Redraw only when the timer fires, not on every X event.
         * Expose and ConfigureNotify set this flag explicitly below.
         * Without this guard, update_title() → XChangeProperty() generates
         * PropertyNotify back to us (PropertyChangeMask), causing select()
         * to return immediately on every iteration — a 15 % CPU busy-loop. */
        int do_redraw = (sel == 0);

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                /* backbuffer nadal aktualny - samo skopiowanie na okno */
                if (ev.xexpose.count == 0)
                    ui_end_frame(ui);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width  != win_w ||
                    ev.xconfigure.height != win_h) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;
                    ui_resize(ui, win_w, win_h);
                    draw(ui, &cfg, win_w, win_h);
                }
                break;
            case PropertyNotify:
                /* Fallback: correct position post-map via XMoveWindow. */
                if (!frame_adj_done && want_pos
                    && ev.xproperty.atom == a_frame_extents) {
                    long ex[4];
                    if (read_frame_extents(dpy, win, a_frame_extents, ex)) {
                        geom_position(dpy, &geom, win_w, win_h, ex,
                                      &win_x, &win_y);
                        XMoveWindow(dpy, win, win_x, win_y);
                        XFlush(dpy);
                    }
                    frame_adj_done = 1;
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_del)
                    g_running = 0;
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_q || ks == XK_Escape)
                    g_running = 0;
                break;
            }
            }
        }

        if (do_redraw) {
            update_title(dpy, win, cfg.title);
            draw(ui, &cfg, win_w, win_h);
        }
    }

    if (cfg.n_owned)
        XFreeColors(dpy, DefaultColormap(dpy, screen),
                    cfg.owned, cfg.n_owned, 0);
    ui_destroy(ui);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
