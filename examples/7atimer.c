/*
 * 7atimer.c - port oryginalnej apki z ../7atimer (Xt/Xaw, zwykle Form/
 * Label/Command/Toggle/AsciiText) na biblioteke ui.c/ui.h z tego
 * katalogu - ten sam wzorzec portowania co examples/7aweather.c,
 * examples/7asensors.c, examples/7acal.c i examples/7atodo.c.
 *
 * Apka miala pierwotnie DWA niezalezne liczniki (stoper + minutnik z
 * alarmem) - minutnik (Countdown) zostal przeniesiony do nowej apki
 * examples/7afilm.c (narzedzie fotograficzne, na razie zawiera tylko ten
 * jeden przeniesiony widget), zeby 7atimer pozostal prostym stoperem.
 * 7atimer nie fork+exec'uje juz niczego (to robil tylko alarm minutnika),
 * wiec nie ma tez SIGCHLD/pledge "proc exec" - patrz main().
 *
 * Najwieksza roznica wzgledem oryginalu Xt/Xaw: w Xt/Xaw Form NIE
 * przelicza swojej naturalnej szerokosci synchronicznie, wiec oryginal
 * musial recznie mierzyc szerokosc dzieci i przekazywac wyliczone box_w W
 * ARGUMENTACH TWORZENIA (ponad 100 linii komentarzy w
 * ../7atimer/7atimer.c o tym, dlaczego kazda inna kolejnosc dziala
 * zawodnie) - tutaj to zwykly box rysowany co klatke, rozciagniety do
 * win_w jak wszystko inne w tej bibliotece (patrz examples/7acal.c/
 * 7atodo.c), wiec cala ta gimnastyka po prostu nie ma czego dotyczyc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20
#define TICK_MS 1000

static char g_sw_buf[16] = "00:00:00";
static int g_sw_elapsed = 0;
static int g_sw_running = 0;
static long g_next_sw_tick_ms = 0;

/* -------------------------------------------------------------------- */
/* Pomoce                                                               */
/* -------------------------------------------------------------------- */

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
FormatHMS(int total_seconds, char *buf, size_t bufsize)
{
    int h = total_seconds / 3600;
    int m = (total_seconds % 3600) / 60;
    int s = total_seconds % 60;

    snprintf(buf, bufsize, "%02d:%02d:%02d", h, m, s);
}

/* -------------------------------------------------------------------- */
/* Stoper                                                               */
/* -------------------------------------------------------------------- */

static void
StopwatchDoStart(void)
{
    if (g_sw_running)
        return;
    g_sw_running = 1;
    g_next_sw_tick_ms = now_ms() + TICK_MS;
}

static void
StopwatchDoStop(void)
{
    g_sw_running = 0;
}

static void
StopwatchDoReset(void)
{
    StopwatchDoStop();
    g_sw_elapsed = 0;
    snprintf(g_sw_buf, sizeof(g_sw_buf), "00:00:00");
}

/* -------------------------------------------------------------------- */
/* Ikona okna (zegar) - surowa 1-bitowa Pixmapa, jak w pozostalych       */
/* portach.                                                              */
/* -------------------------------------------------------------------- */

static void
DrawClockIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XDrawArc(idpy, p, gc, 2, 2, 27, 27, 0, 360 * 64);
    XDrawLine(idpy, p, gc, 16, 16, 16, 6);
    XDrawLine(idpy, p, gc, 16, 16, 23, 16);
}

static Pixmap
MakeClockIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawClockIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y = 10;  /* odstep od gornej krawedzi okna */
    UiRect row;
    UiBox *box;
    UiRect brow, start_r, stop_r, reset_r;

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 6;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = 4;
        style.border_w = 1;
        style.gap = 2;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    box = ui_box_begin(ctx, "swbox", 0, y, win_w, &style);
    row = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, row, g_sw_buf);
    ui_box_end(box);
    y += style.margin_t + ui_box_height(ctx, "swbox") + style.margin_b;

    y += 10;
    brow = (UiRect){ style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
    start_r = ui_rect_col(brow, 0, 3, 6);
    stop_r = ui_rect_col(brow, 1, 3, 6);
    reset_r = ui_rect_col(brow, 2, 3, 6);

    if (ui_button(ctx, start_r, "Start")) StopwatchDoStart();
    if (ui_button(ctx, stop_r, "Stop")) StopwatchDoStop();
    if (ui_button(ctx, reset_r, "Reset")) StopwatchDoReset();

    (void) win_h;
    return 0;
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
    int win_w = 210, win_h = 110;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    char app_name[64] = "7aTimer";
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

#ifdef __OpenBSD__
    /* Bez proc/exec - apka niczego juz nie fork+exec'uje (to robil tylko
     * alarm minutnika, przeniesiony do examples/7afilm.c), wiec wystarczy
     * ten sam pledge co examples/demo.c. Bez wpath/cpath - apka nic nie
     * zapisuje na dysk. */
    if (pledge("stdio rpath unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

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
        ch->res_name  = app_name;
        ch->res_class = "7aTimer";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    icon = MakeClockIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 100;
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    running = 1;
    redraw = 1;

    while (running) {
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

        /* stoper - jeden timer, obslugiwany select()-owym pollingiem w
         * petli glownej zamiast osobnego XtIntervalId, ten sam mechanizm
         * co w examples/7aweather.c. Sam sobie liczy nastepny "due" czas
         * po odpaleniu. */
        {
            long now = now_ms();

            if (g_sw_running && now >= g_next_sw_tick_ms) {
                g_sw_elapsed++;
                FormatHMS(g_sw_elapsed, g_sw_buf, sizeof(g_sw_buf));
                g_next_sw_tick_ms = now + TICK_MS;
                redraw = 1;
            }
        }

        if (redraw) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h)) running = 0;
            ui_end_frame(ctx);
            redraw = 0;
        }
        if (!running)
            break;

        {
            long now = now_ms();

            if (!g_sw_running) {
                fd_set rfds;
                int xfd = ConnectionNumber(dpy);

                FD_ZERO(&rfds);
                FD_SET(xfd, &rfds);
                select(xfd + 1, &rfds, NULL, NULL, NULL);
            } else {
                long remaining = g_next_sw_tick_ms - now;

                if (remaining > 0) {
                    fd_set rfds;
                    int xfd = ConnectionNumber(dpy);
                    struct timeval tv;

                    FD_ZERO(&rfds);
                    FD_SET(xfd, &rfds);
                    tv.tv_sec = remaining / 1000;
                    tv.tv_usec = (remaining % 1000) * 1000;
                    select(xfd + 1, &rfds, NULL, NULL, &tv);
                }
            }
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
