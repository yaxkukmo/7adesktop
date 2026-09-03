/*
 * 7asys.c - panel informacji systemowych na bibliotece ui.c/ui.h z tego
 * katalogu.
 *
 * Nowa apka (nie port Xt/Xaw). Pokazuje:
 *   - uptime + srednia obciazenia (popen("uptime"))
 *   - top-10 procesow wg %CPU (popen("ps aux | sort ...") na Linuxie,
 *     popen("ps aux -r") na OpenBSD)
 *   - punkty montowania (popen("df -h"))
 *
 * Auto-odswiezanie co 5 sekund przez select() na fd polaczenia X (ten sam
 * wzorzec co 7aweather.c). Brak suwaka - dane sa przewijane kolkiem myszy
 * (ten sam wzorzec co 7amessage.c, lecz jeszcze prostszy bo linie sa
 * stalej dlugosci).
 *
 * Na OpenBSD: pledge("stdio rpath unix prot_exec proc exec", NULL).
 * popen() wymaga proc+exec.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ICON_SIZE      32
#define ROW_H          18
#define REFRESH_MS     5000
#define LINE_W         256  /* max dlugosc jednej linii wyjscia */
#define UPTIME_MAX     4
#define PROC_MAX       14
#define DISK_MAX       22

static char g_uptime[UPTIME_MAX][LINE_W];
static int  g_uptime_n = 0;
static char g_proc[PROC_MAX][LINE_W];
static int  g_proc_n = 0;
static char g_disk[DISK_MAX][LINE_W];
static int  g_disk_n = 0;

/* ------------------------------------------------------------------ */
/* Pobieranie danych systemowych                                        */
/* ------------------------------------------------------------------ */

/* Czyta do buf[size] pierwsze n linii z komendy cmd przez popen().
 * Zwraca liczbe zapisanych linii (>=0) lub -1 przy bledzie popen. */
static int
ReadLines(const char *cmd, char out[][LINE_W], int max_lines)
{
    FILE *fp;
    int n = 0;

    fp = popen(cmd, "r");
    if (!fp) return -1;

    while (n < max_lines) {
        if (!fgets(out[n], LINE_W, fp)) break;
        /* odetnij trailing newline */
        {
            int len = (int)strlen(out[n]);
            while (len > 0 && (out[n][len-1] == '\n' || out[n][len-1] == '\r'))
                out[n][--len] = '\0';
        }
        n++;
    }
    pclose(fp);
    return n;
}

static void
RefreshData(void)
{
    char tmp[DISK_MAX][LINE_W];
    int n, i;

    /* Uptime */
    n = ReadLines("uptime", tmp, UPTIME_MAX);
    g_uptime_n = (n > 0) ? n : 0;
    for (i = 0; i < g_uptime_n; i++)
        snprintf(g_uptime[i], LINE_W, "%s", tmp[i]);

    /* Procesy */
#ifdef __OpenBSD__
    n = ReadLines("ps -ax -r -o pid,pcpu,pmem,comm", tmp, PROC_MAX);
#else
    n = ReadLines("ps aux --sort=-%cpu", tmp, PROC_MAX);
#endif
    g_proc_n = (n > 0) ? n : 0;
    for (i = 0; i < g_proc_n; i++)
        snprintf(g_proc[i], LINE_W, "%s", tmp[i]);

    /* Dyski */
    n = ReadLines("df -h", tmp, DISK_MAX);
    g_disk_n = (n > 0) ? n : 0;
    for (i = 0; i < g_disk_n; i++)
        snprintf(g_disk[i], LINE_W, "%s", tmp[i]);
}

/* ------------------------------------------------------------------ */
/* Ikonka: monitor - ten sam wzorzec co inne apki                      */
/* ------------------------------------------------------------------ */

static Pixmap
MakeSysIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    /* monitor: prostokat + podstawka */
    XDrawRectangle(idpy, icon, gc, 2, 2, 27, 20);
    XFillRectangle(idpy, icon, gc, 10, 22, 12, 3);
    XFillRectangle(idpy, icon, gc, 7,  25, 18, 2);
    /* linie wewnatrz ekranu (wykres) */
    XDrawLine(idpy, icon, gc, 5, 18, 9,  12);
    XDrawLine(idpy, icon, gc, 9, 12, 14, 16);
    XDrawLine(idpy, icon, gc, 14, 16, 19, 8);
    XDrawLine(idpy, icon, gc, 19, 8,  26, 14);
    XFreeGC(idpy, gc);
    return icon;
}

/* ------------------------------------------------------------------ */
/* Rysowanie                                                           */
/* ------------------------------------------------------------------ */

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y = 0;
    int i;

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 0;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = 4;
        style.border_w = 1;
        style.gap = 2;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    /* --- Uptime / Load --- */
    y += 6;
    {
        UiRect hdr = { style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        UiBox *box;

        ui_label(ctx, hdr, "Uptime / Load");
        y += ROW_H + 4;
        box = ui_box_begin(ctx, "uptime", 0, y, win_w, &style);
        for (i = 0; i < g_uptime_n; i++)
            ui_label(ctx, ui_box_next_rect(box, ROW_H), g_uptime[i]);
        if (g_uptime_n == 0)
            ui_label(ctx, ui_box_next_rect(box, ROW_H), "...");
        ui_box_end(box);
        y += ui_box_height(ctx, "uptime");
    }

    /* --- Top procesow (CPU) --- */
    y += 10;
    {
        UiRect hdr = { style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        UiBox *box;

        ui_label(ctx, hdr, "Top procesow (CPU)");
        y += ROW_H + 4;
        box = ui_box_begin(ctx, "proc", 0, y, win_w, &style);
        for (i = 0; i < g_proc_n; i++)
            ui_label(ctx, ui_box_next_rect(box, ROW_H), g_proc[i]);
        if (g_proc_n == 0)
            ui_label(ctx, ui_box_next_rect(box, ROW_H), "...");
        ui_box_end(box);
        y += ui_box_height(ctx, "proc");
    }

    /* --- Punkty montowania (df -h) --- */
    y += 10;
    {
        UiRect hdr = { style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        UiBox *box;

        ui_label(ctx, hdr, "Punkty montowania (df -h)");
        y += ROW_H + 4;
        box = ui_box_begin(ctx, "disk", 0, y, win_w, &style);
        for (i = 0; i < g_disk_n; i++)
            ui_label(ctx, ui_box_next_rect(box, ROW_H), g_disk[i]);
        if (g_disk_n == 0)
            ui_label(ctx, ui_box_next_rect(box, ROW_H), "...");
        ui_box_end(box);
        y += ui_box_height(ctx, "disk");
    }

    /* --- Refresh --- */
    y += 10;
    {
        int btn_w = ui_text_width(ctx, "Refresh") + 20;
        UiRect refresh_r = { style.margin_l, y, btn_w, ROW_H };

        if (ui_button(ctx, refresh_r, "Refresh"))
            RefreshData();
    }

    (void)win_h;
}

/* ------------------------------------------------------------------ */

static long
now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 560, win_h = 440;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw;
    long next_refresh;
    XEvent ev;

    (void)argc;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y,
                                       &geom_w, &geom_h);
            i++;
        }
    }

#ifdef __OpenBSD__
    {
        const char *home = getenv("HOME");
        char home_fontcfg[1024];
        char xauth_buf[1024];
        const char *xauth_path;

        snprintf(home_fontcfg, sizeof(home_fontcfg), "%s/.fontconfig",
                 home ? home : ".");
        /* "rx" zamiast "r": popen() robi fork+exec /bin/sh, a ps/df/uptime
         * sa w /bin lub /usr/bin - bez x exec zawodzi cicho */
        if (unveil("/usr", "rx")         == -1) { perror("unveil"); return 1; }
        if (unveil("/etc", "r")          == -1) { perror("unveil"); return 1; }
        if (unveil("/var", "r")          == -1) { perror("unveil"); return 1; }
        if (unveil("/bin", "rx")         == -1) { perror("unveil"); return 1; }
        if (unveil("/sbin", "rx")        == -1) { perror("unveil"); return 1; }
        if (unveil("/proc", "r")         == -1) { perror("unveil"); return 1; }
        if (unveil("/tmp/.X11-unix", "rw") == -1) { perror("unveil"); return 1; }
        if (unveil(home_fontcfg, "r")    == -1) { perror("unveil"); return 1; }
        xauth_path = getenv("XAUTHORITY");
        if (!xauth_path) {
            snprintf(xauth_buf, sizeof(xauth_buf), "%s/.Xauthority",
                     home ? home : ".");
            xauth_path = xauth_buf;
        }
        if (unveil(xauth_path, "r")      == -1) { perror("unveil"); return 1; }
        if (unveil(NULL, NULL)           == -1) { perror("unveil"); return 1; }
        if (pledge("stdio rpath unix prot_exec proc exec", NULL) == -1) {
            perror("pledge");
            return 1;
        }
    }
#endif

    RefreshData();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7asys: brak polaczenia z X11\n");
        return 1;
    }

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);

    if (geom_mask & WidthValue)  win_w = (int)geom_w;
    if (geom_mask & HeightValue) win_h = (int)geom_h;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative)
              ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative)
              ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;

    win = XCreateSimpleWindow(dpy, root, win_x, win_y, win_w, win_h, 0,
                               BlackPixel(dpy, screen),
                               WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, "7aSys");
    XSetIconName(dpy, win, "7aSys");

    icon    = MakeSysIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags        = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap  = icon;
    wmhints->icon_mask    = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags      = PMinSize;
    sizehints->min_width  = 300;
    sizehints->min_height = 200;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc  = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7asys: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    next_refresh = now_ms() + REFRESH_MS;
    running = 1;
    redraw  = 1;

    while (running) {
        int xfd = ConnectionNumber(dpy);
        fd_set fds;
        struct timeval tv;
        long ms_left;

        if (!XPending(dpy)) {
            long now = now_ms();
            long wait_refresh = next_refresh - now;

            if (wait_refresh <= 0) {
                RefreshData();
                next_refresh = now + REFRESH_MS;
                redraw = 1;
                wait_refresh = REFRESH_MS;
            }

            ms_left = wait_refresh;
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            tv.tv_sec  = ms_left / 1000;
            tv.tv_usec = (ms_left % 1000) * 1000;
            select(xfd + 1, &fds, NULL, NULL, &tv);
        }

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
                break;
            case ButtonPress:
            case ButtonRelease:
            case MotionNotify:
                redraw = 1;
                break;
            case KeyPress: {
                char keybuf[16];
                KeySym ks;
                XLookupString(&ev.xkey, keybuf, sizeof(keybuf), &ks, NULL);
                if (ks == XK_Escape || ks == XK_q)
                    running = 0;
                if (ks == XK_r) {
                    RefreshData();
                    next_refresh = now_ms() + REFRESH_MS;
                    redraw = 1;
                }
                break;
            }
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
            ui_begin_frame(ctx);
            draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            redraw = 0;
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
