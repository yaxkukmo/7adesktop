/* 7aclock - analog X11 clock with optional date display inside the face
 * inspired by xclock and urxvclock
 *
 * Rendering uses a back-buffer Pixmap to avoid flicker:
 *   draw() → off-screen Pixmap via Xlib
 *   present() → single atomic XCopyArea to window
 *
 * Sleep timing: without the seconds hand we sync to the minute boundary
 * via clock_gettime(); with it we sync to the second boundary.  Either
 * way we sleep precisely until the next hand movement is visible, so CPU
 * usage stays near zero between frames.
 *
 * Cairo removed: all drawing uses pure Xlib (XDrawArc, XDrawLine,
 * XFillPolygon, XFillRectangle).  Transparency (-alpha) is not supported.
 */

/* clock_gettime/CLOCK_* (POSIX), M_PI (XSI/BSD) i strdup (POSIX) sa poza
 * ISO C99 - -std=c99 w Makefile ukrywa je w glibc bez tego makra, chyba ze
 * wlaczymy je jawnie; na OpenBSD nie ma to wplywu (tam sa widoczne
 * niezaleznie) - patrz ta sama uwaga w utils/7aweather.c. */
#define _DEFAULT_SOURCE

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>

#define PI       M_PI
#define PROG     "7aclock"
#define DEF_SIZE 150
#define DEF_PAD  4

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

/* ── types ──────────────────────────────────────────────────────────── */

typedef struct {
    int    w, h;
    int    show_date;
    char  *date_fmt;
    unsigned long bg, fg, hands, sec_hand, date_fg, date_bg;
    unsigned long ring, hour_tick, min_tick, box_bg;
    int    padding;
    int    noseconds;
    int    noring;
    int    update_ms;
    char        *font;
    XFontStruct *xfont;
    char  *title;
    char  *name;
    char  *wm_class;
    char  *geometry;
} Cfg;

/* Off-screen buffer for double-buffering. */
typedef struct {
    Display *dpy;
    int      depth;
    Pixmap   pix;
    GC       gc;
    int      w, h;
} Buf;

/* ── color ──────────────────────────────────────────────────────────── */

static int parse_color(Display *dpy, Colormap cmap, const char *spec,
                       unsigned long *px)
{
    XColor xc;
    if (!XParseColor(dpy, cmap, spec, &xc)) {
        fprintf(stderr, PROG ": bad color '%s'\n", spec);
        return 0;
    }
    if (!XAllocColor(dpy, cmap, &xc)) {
        fprintf(stderr, PROG ": cannot allocate color '%s'\n", spec);
        return 0;
    }
    *px = xc.pixel;
    return 1;
}

/* ── XResources ─────────────────────────────────────────────────────── */

static XrmDatabase xrm_open(Display *dpy)
{
    XrmInitialize();
    XrmDatabase db = NULL;
    char *rms = XResourceManagerString(dpy);
    if (rms)
        db = XrmGetStringDatabase(rms);
    char *env = getenv("XENVIRONMENT");
    char path[512];
    if (!env) {
        char *home = getenv("HOME");
        if (home) {
            snprintf(path, sizeof(path), "%s/.Xresources", home);
            env = path;
        }
    }
    if (env) {
        XrmDatabase fdb = XrmGetFileDatabase(env);
        if (fdb) XrmMergeDatabases(fdb, &db);
    }
    return db;
}

static const char *xrm_str(XrmDatabase db,
                            const char *name, const char *klass)
{
    if (!db) return NULL;
    char full_name[128], full_class[128];
    snprintf(full_name,  sizeof(full_name),  PROG ".%s", name);
    snprintf(full_class, sizeof(full_class), PROG ".%s", klass);
    char *type = NULL;
    XrmValue val;
    if (XrmGetResource(db, full_name, full_class, &type, &val) && val.addr)
        return val.addr;
    return NULL;
}

static int xrm_color(Display *dpy, Colormap cmap, XrmDatabase db,
                     const char *name, const char *klass, unsigned long *px)
{
    if (!db) return 0;
    char full_name[128], full_class[128];
    snprintf(full_name,  sizeof(full_name),  PROG ".%s", name);
    snprintf(full_class, sizeof(full_class), PROG ".%s", klass);
    char *type = NULL;
    XrmValue val;
    if (XrmGetResource(db, full_name, full_class, &type, &val) && val.addr)
        return parse_color(dpy, cmap, val.addr, px);
    return 0;
}

/* ── back-buffer ────────────────────────────────────────────────────── */

static void buf_free(Buf *b)
{
    if (b->pix) { XFreePixmap(b->dpy, b->pix); b->pix = 0; }
}

static void buf_create(Buf *b, Window win, int w, int h)
{
    buf_free(b);
    b->w   = w;
    b->h   = h;
    b->pix = XCreatePixmap(b->dpy, win,
                           (unsigned)w, (unsigned)h, (unsigned)b->depth);
}

static void buf_present(Buf *b, Window win)
{
    XCopyArea(b->dpy, b->pix, win, b->gc,
              0, 0, (unsigned)b->w, (unsigned)b->h, 0, 0);
    XFlush(b->dpy);
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

/* ── font helpers ───────────────────────────────────────────────────── */

/* UTF-8 → UCS-2 (same logic as ui.c / draw_string_utf8).
 * Returns number of XChar2b elements written (≤ out_max).
 * Characters > U+FFFF are replaced with '?'. */
static int utf8_to_ucs2(const char *str, int len, XChar2b *out, int out_max)
{
    int n = 0, i = 0;
    while (i < len && n < out_max) {
        unsigned char c = (unsigned char)str[i];
        unsigned long u;
        if (c < 0x80) {
            u = c; i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len &&
                   ((unsigned char)str[i+1] & 0xC0) == 0x80) {
            u = ((unsigned long)(c & 0x1F) << 6) |
                ((unsigned char)str[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len &&
                   ((unsigned char)str[i+1] & 0xC0) == 0x80 &&
                   ((unsigned char)str[i+2] & 0xC0) == 0x80) {
            u = ((unsigned long)(c & 0x0F) << 12) |
                ((unsigned long)((unsigned char)str[i+1] & 0x3F) << 6) |
                ((unsigned char)str[i+2] & 0x3F);
            i += 3;
        } else {
            u = 0xFFFD; i++;
        }
        if (u > 0xFFFF) u = '?';
        out[n].byte1 = (unsigned char)((u >> 8) & 0xFF);
        out[n].byte2 = (unsigned char)(u & 0xFF);
        n++;
    }
    return n;
}

/* ── drawing ────────────────────────────────────────────────────────── */

/* Filled lance-shaped hand: pointed at both ends, widest at wp*R from the
 * pivot toward the tip.  front/back/wp/hw are all fractions of R.         */
static void fill_hand(Display *dpy, Drawable d, GC gc,
                      double cx, double cy, double R,
                      double angle, double front, double back,
                      double wp, double hw)
{
    double ca = cos(angle), sa = sin(angle);
    XPoint pts[4];
    pts[0].x = (short)(cx - back*R*ca);
    pts[0].y = (short)(cy - back*R*sa);
    pts[1].x = (short)(cx + wp*R*ca - hw*R*sa);
    pts[1].y = (short)(cy + wp*R*sa + hw*R*ca);
    pts[2].x = (short)(cx + front*R*ca);
    pts[2].y = (short)(cy + front*R*sa);
    pts[3].x = (short)(cx + wp*R*ca + hw*R*sa);
    pts[3].y = (short)(cy + wp*R*sa - hw*R*ca);
    XFillPolygon(dpy, d, gc, pts, 4, Convex, CoordModeOrigin);
}

static void draw(Buf *b, const Cfg *cfg)
{
    Display   *dpy = b->dpy;
    Drawable   d   = b->pix;
    GC         gc  = b->gc;
    int        w   = b->w, h = b->h;
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);

    double cx = w * 0.5;
    double cy = h * 0.5;
    double R  = ((w < h) ? w : h) * 0.5 - cfg->padding;
    if (R < 1.0) R = 1.0;

    /* Background */
    XSetForeground(dpy, gc, cfg->bg);
    XFillRectangle(dpy, d, gc, 0, 0, (unsigned)w, (unsigned)h);

    /* outer ring */
    if (!cfg->noring) {
        int lw = (int)(R * 0.03);
        if (lw < 1) lw = 1;
        /* diam first, then x0/y0 derived from IT (not from R independently) -
         * two separate (int) truncations of (cx - R) and (2.0 * R) can each
         * round down by up to 1px, so the bounding box's actual center
         * (x0 + diam/2) could land up to ~1px off from (cx, cy), visibly
         * offset from the minute ticks below which are placed straight off
         * cx/cy without that compounding. */
        int diam = (int)lround(2.0 * R);
        int x0 = (int)lround(cx - diam / 2.0);
        int y0 = (int)lround(cy - diam / 2.0);
        XSetForeground(dpy, gc, cfg->box_bg);
        XFillArc(dpy, d, gc, x0, y0,
                 (unsigned)diam, (unsigned)diam, 0, 360 * 64);
        XSetForeground(dpy, gc, cfg->ring);
        XSetLineAttributes(dpy, gc, (unsigned)lw,
                           LineSolid, CapButt, JoinMiter);
        XDrawArc(dpy, d, gc, x0, y0,
                 (unsigned)diam, (unsigned)diam, 0, 360 * 64);
    }

    /* hour ticks */
    {
        int lw = (int)(R * 0.045);
        if (lw < 1) lw = 1;
        XSetForeground(dpy, gc, cfg->hour_tick);
        XSetLineAttributes(dpy, gc, (unsigned)lw,
                           LineSolid, CapButt, JoinMiter);
        for (int i = 0; i < 12; i++) {
            double a = i * (PI / 6.0) - PI * 0.5;
            double ca = cos(a), sa = sin(a);
            XDrawLine(dpy, d, gc,
                      (int)(cx + R * 0.80 * ca), (int)(cy + R * 0.80 * sa),
                      (int)(cx + R * 0.92 * ca), (int)(cy + R * 0.92 * sa));
        }
    }

    /* minute ticks */
    {
        int lw = (int)(R * 0.015);
        if (lw < 1) lw = 1;
        XSetForeground(dpy, gc, cfg->min_tick);
        XSetLineAttributes(dpy, gc, (unsigned)lw,
                           LineSolid, CapButt, JoinMiter);
        for (int i = 0; i < 60; i++) {
            if (i % 5 == 0) continue;
            double a = i * (PI / 30.0) - PI * 0.5;
            double ca = cos(a), sa = sin(a);
            XDrawLine(dpy, d, gc,
                      (int)(cx + R * 0.87 * ca), (int)(cy + R * 0.87 * sa),
                      (int)(cx + R * 0.92 * ca), (int)(cy + R * 0.92 * sa));
        }
    }

    /* date window near 6 o'clock — box via Xlib, text via core X11 font */
    if (cfg->show_date && cfg->xfont) {
        char tbuf[128];
        strftime(tbuf, sizeof(tbuf), cfg->date_fmt, tm);
        XChar2b xbuf[128];
        int nc  = utf8_to_ucs2(tbuf, (int)strlen(tbuf), xbuf, 128);
        int tw  = XTextWidth16(cfg->xfont, xbuf, nc);
        int th  = cfg->xfont->ascent + cfg->xfont->descent;
        int pad = (int)(R * 0.05);
        if (pad < 1) pad = 1;
        int bw  = tw + pad * 2;
        int bh  = th + pad * 2;
        int bx  = (int)(cx - bw * 0.5);
        int by  = (int)(cy + R * 0.42 - bh * 0.5);

        XSetForeground(dpy, gc, cfg->date_bg);
        XFillRectangle(dpy, d, gc, bx, by, (unsigned)bw, (unsigned)bh);

        int blw = (int)(R * 0.012);
        if (blw < 1) blw = 1;
        XSetForeground(dpy, gc, cfg->fg);
        XSetLineAttributes(dpy, gc, (unsigned)blw,
                           LineSolid, CapButt, JoinMiter);
        XDrawRectangle(dpy, d, gc, bx, by, (unsigned)bw, (unsigned)bh);

        XSetFont(dpy, gc, cfg->xfont->fid);
        XSetForeground(dpy, gc, cfg->date_fg);
        XDrawString16(dpy, d, gc,
                      bx + pad, by + pad + cfg->xfont->ascent, xbuf, nc);
    }

    /* line width 0 = thin (1 px); irrelevant for filled shapes below */
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);

    /* minute hand — lance: wide at 0.08R ahead of pivot, tapers to both tips */
    double min_a = ((tm->tm_min + tm->tm_sec / 60.0) / 60.0) * 2.0 * PI - PI * 0.5;
    XSetForeground(dpy, gc, cfg->hands);
    fill_hand(dpy, d, gc, cx, cy, R, min_a, 0.76, 0.12, 0.08, 0.028);

    /* hour hand — wider and stubbier lance */
    double hr_a = ((tm->tm_hour % 12 + tm->tm_min / 60.0) / 12.0) * 2.0 * PI - PI * 0.5;
    fill_hand(dpy, d, gc, cx, cy, R, hr_a, 0.55, 0.12, 0.06, 0.040);

    /* seconds hand — thin needle lance, symmetric (widest at pivot) */
    if (!cfg->noseconds) {
        double sec_a = (tm->tm_sec / 60.0) * 2.0 * PI - PI * 0.5;
        XSetForeground(dpy, gc, cfg->sec_hand);
        fill_hand(dpy, d, gc, cx, cy, R, sec_a, 0.87, 0.20, 0.0, 0.011);
    }

    /* center cap */
    XSetForeground(dpy, gc, cfg->hands);
    int cap_r = (int)(R * 0.045);
    if (cap_r < 1) cap_r = 1;
    XFillArc(dpy, d, gc,
             (int)(cx - cap_r), (int)(cy - cap_r),
             (unsigned)(cap_r * 2), (unsigned)(cap_r * 2), 0, 360 * 64);
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
        "  -bg COLOR          background color         (default: #1a1a2e)\n"
        "  -fg COLOR          face/ticks/border color  (default: #e0e0e0)\n"
        "  -hd COLOR          hour+minute hands color  (default: #e0e0e0)\n"
        "  -sd COLOR          seconds hand color       (default: #e05050)\n"
        "  -dc COLOR          date text color          (default: same as -fg)\n"
        "  -db COLOR          date box background      (default: same as -bg)\n"
        "  -font XLFD         date font XLFD or alias  (default: sans-serif)\n"
        "  -ring COLOR        outer ring/border color  (default: same as -fg)\n"
        "  -boxbg COLOR       outer ring fill color    (default: same as -bg)\n"
        "  -htick COLOR       hour tick marks color    (default: same as -fg)\n"
        "  -mtick COLOR       minute tick marks color  (default: same as -fg)\n"
        "  -padding N         padding pixels           (default: 4)\n"
        "  -title FMT         window title; strftime formats allowed\n"
        "                     e.g. \"%H:%M\" shows current time as title\n"
        "  -name NAME         WM_CLASS instance name (default: 7aclock)\n"
        "  -class CLASS       WM_CLASS class name    (default: 7aclock)\n"
        "  -geometry WxH+X+Y  window geometry\n"
        "  -update MS         fixed redraw interval ms (overrides auto-sync)\n"
        "  -help              this message\n"
        "  q / Escape         quit\n",
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
    int geom_flags = 0, geom_x = 0, geom_y = 0;

    Cfg cfg = {
        .w         = DEF_SIZE,
        .h         = DEF_SIZE,
        .show_date = 0,
        .date_fmt  = "%d %b",
        .padding   = DEF_PAD,
        .noseconds = 0,
        .update_ms = 0,
        .font      = "sans-serif",
        .title     = PROG,
        .name      = PROG,
        .wm_class  = PROG,
        .geometry  = NULL,
    };

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs(PROG ": cannot open display\n", stderr); return 1; }

    int      screen = DefaultScreen(dpy);
    Window   root   = RootWindow(dpy, screen);
    Colormap cmap   = DefaultColormap(dpy, screen);

    /* default colors */
    parse_color(dpy, cmap, "#1a1a2e", &cfg.bg);
    parse_color(dpy, cmap, "#e0e0e0", &cfg.fg);
    cfg.hands     = cfg.fg;
    parse_color(dpy, cmap, "#e05050", &cfg.sec_hand);
    cfg.date_fg   = cfg.fg;
    cfg.date_bg   = cfg.bg;
    cfg.ring      = cfg.fg;
    cfg.hour_tick = cfg.fg;
    cfg.min_tick  = cfg.fg;
    cfg.box_bg    = cfg.bg;

    /* XResources — applied before CLI args so command-line wins */
    {
        XrmDatabase db = xrm_open(dpy);
        xrm_color(dpy, cmap, db, "background", "Background", &cfg.bg);
        xrm_color(dpy, cmap, db, "foreground", "Foreground", &cfg.fg);
        xrm_color(dpy, cmap, db, "hands",      "Hands",      &cfg.hands);
        xrm_color(dpy, cmap, db, "secondHand", "SecondHand", &cfg.sec_hand);
        xrm_color(dpy, cmap, db, "dateFg",     "DateFg",     &cfg.date_fg);
        xrm_color(dpy, cmap, db, "dateBg",     "DateBg",     &cfg.date_bg);
        xrm_color(dpy, cmap, db, "ring",       "Ring",       &cfg.ring);
        xrm_color(dpy, cmap, db, "hourTick",   "HourTick",   &cfg.hour_tick);
        xrm_color(dpy, cmap, db, "minuteTick", "MinuteTick", &cfg.min_tick);
        xrm_color(dpy, cmap, db, "boxBackground", "BoxBackground", &cfg.box_bg);
        {
            char *type = NULL; XrmValue val;
            if (db && XrmGetResource(db, "uiFont", "UiFont", &type, &val)
                    && val.addr)
                cfg.font = strdup(val.addr);
        }
        const char *xfont = xrm_str(db, "font", "Font");
        if (xfont) cfg.font = strdup(xfont);
        if (db) XrmDestroyDatabase(db);
    }

    for (int i = 1; i < argc; i++) {
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
            cfg.geometry = argv[i];
            unsigned int uw = 0, uh = 0;
            geom_flags = XParseGeometry(argv[i], &geom_x, &geom_y, &uw, &uh);
            if ((geom_flags & WidthValue)  && uw > 0) cfg.w = (int)uw;
            if ((geom_flags & HeightValue) && uh > 0) cfg.h = (int)uh;
        }
        else if (!strcmp(argv[i], "-update")) {
            NEED("-update");
            cfg.update_ms = safe_atoi(argv[i], 100, 60000, "-update");
        }
        else if (!strcmp(argv[i], "-padding")) {
            NEED("-padding");
            cfg.padding = safe_atoi(argv[i], 0, 500, "-padding");
        }
        else if (!strcmp(argv[i], "-bg")) {
            NEED("-bg");
            if (parse_color(dpy, cmap, argv[i], &cfg.bg)) {
                cfg.date_bg = cfg.bg;
                cfg.date_fg = cfg.fg;
                cfg.box_bg  = cfg.bg;
            }
        }
        else if (!strcmp(argv[i], "-fg")) {
            NEED("-fg");
            if (parse_color(dpy, cmap, argv[i], &cfg.fg))
                cfg.date_fg = cfg.fg;
        }
        else if (!strcmp(argv[i], "-hd"))    { NEED("-hd");    parse_color(dpy, cmap, argv[i], &cfg.hands);     }
        else if (!strcmp(argv[i], "-sd"))    { NEED("-sd");    parse_color(dpy, cmap, argv[i], &cfg.sec_hand);  }
        else if (!strcmp(argv[i], "-dc"))    { NEED("-dc");    parse_color(dpy, cmap, argv[i], &cfg.date_fg);   }
        else if (!strcmp(argv[i], "-db"))    { NEED("-db");    parse_color(dpy, cmap, argv[i], &cfg.date_bg);   }
        else if (!strcmp(argv[i], "-ring"))  { NEED("-ring");  parse_color(dpy, cmap, argv[i], &cfg.ring);      }
        else if (!strcmp(argv[i], "-boxbg")) { NEED("-boxbg"); parse_color(dpy, cmap, argv[i], &cfg.box_bg);    }
        else if (!strcmp(argv[i], "-htick")) { NEED("-htick"); parse_color(dpy, cmap, argv[i], &cfg.hour_tick); }
        else if (!strcmp(argv[i], "-mtick")) { NEED("-mtick"); parse_color(dpy, cmap, argv[i], &cfg.min_tick);  }
        else if (!strcmp(argv[i], "-font"))  { NEED("-font");  cfg.font = argv[i];                              }
        else {
            fprintf(stderr, PROG ": unknown option '%s'\n", argv[i]);
            usage(); XCloseDisplay(dpy); return 1;
        }
#undef NEED
    }

    /* Resolve geometry position — negative offsets from right/bottom edge */
    int win_x = 0, win_y = 0;
    if (geom_flags & XValue)
        win_x = (geom_flags & XNegative)
            ? DisplayWidth(dpy, screen)  + geom_x - cfg.w
            : geom_x;
    if (geom_flags & YValue)
        win_y = (geom_flags & YNegative)
            ? DisplayHeight(dpy, screen) + geom_y - cfg.h
            : geom_y;

    /* ── window ────────────────────────────────────────────────────── */
    XSetWindowAttributes attr = {
        .background_pixel = cfg.bg,
        .event_mask       = ExposureMask | StructureNotifyMask
                          | KeyPressMask | PropertyChangeMask,
    };
    Window win = XCreateWindow(dpy, root,
        win_x, win_y, (unsigned)cfg.w, (unsigned)cfg.h, 0,
        DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
        CWBackPixel | CWEventMask, &attr);

    XClassHint ch = { .res_name = cfg.name, .res_class = cfg.wm_class };
    XSetClassHint(dpy, win, &ch);

    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    /* _NET_FRAME_EXTENTS: request frame extents before mapping so we can
     * correct geometry position.  Falls back to PropertyNotify post-map.  */
    Atom a_frame_extents = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    Atom a_req_frame     = XInternAtom(dpy, "_NET_REQUEST_FRAME_EXTENTS", True);
    int  frame_adj_done  = 0;

    if (a_req_frame != None && (geom_flags & (XValue | YValue))) {
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
                XNextEvent(dpy, &pev);
                if (pev.type != PropertyNotify
                    || pev.xproperty.atom != a_frame_extents) continue;
                Atom at; int fmt; unsigned long n, ba;
                unsigned char *data = NULL;
                if (XGetWindowProperty(dpy, win, a_frame_extents, 0, 4,
                        False, XA_CARDINAL, &at, &fmt, &n, &ba, &data)
                    == Success && data && n >= 4) {
                    long *ex = (long *)data;
                    if ((geom_flags & XValue) && (geom_flags & XNegative))
                        win_x = DisplayWidth(dpy, screen) + geom_x
                                - cfg.w - (int)ex[1];
                    else if (geom_flags & XValue)
                        win_x = geom_x + (int)ex[0];
                    if ((geom_flags & YValue) && (geom_flags & YNegative))
                        win_y = DisplayHeight(dpy, screen) + geom_y
                                - cfg.h - (int)ex[3];
                    else if (geom_flags & YValue)
                        win_y = geom_y + (int)ex[2];
                    frame_adj_done = 1;
                }
                if (data) XFree(data);
            }
        }
    }

    XSizeHints sh = {
        .flags     = PMinSize,
        .min_width = 50, .min_height = 50,
    };
    if (geom_flags & (XValue | YValue)) {
        sh.flags |= USPosition;
        sh.x = win_x;
        sh.y = win_y;
    }
    XSetNormalHints(dpy, win, &sh);

    XMapWindow(dpy, win);
    XFlush(dpy);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    Buf buf = { .dpy = dpy, .depth = DefaultDepth(dpy, screen), .gc = gc };
    buf_create(&buf, win, cfg.w, cfg.h);

    /* Load core X11 font (XLFD or alias); fall back to "fixed" if not found. */
    cfg.xfont = XLoadQueryFont(dpy, cfg.font);
    if (!cfg.xfont)
        cfg.xfont = XLoadQueryFont(dpy, "fixed");

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    update_title(dpy, win, cfg.title);
    draw(&buf, &cfg);
    buf_present(&buf, win);

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
                if (ev.xexpose.count == 0)
                    buf_present(&buf, win);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width  != buf.w ||
                    ev.xconfigure.height != buf.h) {
                    cfg.w = ev.xconfigure.width;
                    cfg.h = ev.xconfigure.height;
                    buf_create(&buf, win, cfg.w, cfg.h);
                    draw(&buf, &cfg);
                    buf_present(&buf, win);
                }
                break;
            case PropertyNotify:
                /* Fallback: correct position post-map via XMoveWindow. */
                if (!frame_adj_done
                    && ev.xproperty.atom == a_frame_extents
                    && (geom_flags & (XValue | YValue))) {
                    Atom at; int fmt; unsigned long n, ba;
                    unsigned char *data = NULL;
                    if (XGetWindowProperty(dpy, win, a_frame_extents,
                                           0, 4, False, XA_CARDINAL,
                                           &at, &fmt, &n, &ba, &data)
                        == Success && data && n >= 4) {
                        long *ex = (long *)data;
                        if ((geom_flags & XValue) && (geom_flags & XNegative))
                            win_x = DisplayWidth(dpy, screen) + geom_x
                                    - cfg.w - (int)ex[1];
                        else if (geom_flags & XValue)
                            win_x = geom_x + (int)ex[0];
                        if ((geom_flags & YValue) && (geom_flags & YNegative))
                            win_y = DisplayHeight(dpy, screen) + geom_y
                                    - cfg.h - (int)ex[3];
                        else if (geom_flags & YValue)
                            win_y = geom_y + (int)ex[2];
                        XMoveWindow(dpy, win, win_x, win_y);
                        XFlush(dpy);
                    }
                    if (data) XFree(data);
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
            draw(&buf, &cfg);
            buf_present(&buf, win);
        }
    }

    buf_free(&buf);
    if (cfg.xfont) XFreeFont(dpy, cfg.xfont);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
