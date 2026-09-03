/*
 * 7anotify.c - minimalistyczny popup powiadomien dla X11 na bibliotece
 * ui.c/ui.h z tego katalogu.
 *
 * Nowa apka (nie port Xt/Xaw). Pokazuje krotki tekst (z argumentow CLI
 * lub z stdin) w rogu ekranu i automatycznie zamyka sie po N sekundach
 * (domyslnie 5). Uzytkownik moze tez zamknac przez klikniecie lub Escape.
 *
 * Roznice wzgledem 7amessage.c:
 * - Brak przyciskow, brak suwaka (powiadomienia maja byc krotkie).
 * - override_redirect = True: brak dekoracji WM, nie trafia do taskbara.
 * - Pozycjonowanie w rogu ekranu (-p tr/tl/br/bl, domyslnie br).
 * - Auto-zamkniecie po timeout przez select() na fd polaczenia X (ten sam
 *   wzorzec co 7aweather.c), zamiast XtAppAddTimeOut.
 * - Tekst z argumentow LUB z stdin (gdy brak argumentow).
 *
 * Uzycie:
 *   7anotify [-t SEKUNDY] [-p POZYCJA] [tresc powiadomienia...]
 *   echo "tresc" | 7anotify [-t SEKUNDY] [-p POZYCJA]
 *
 * POZYCJA: tl (lewy-gorny), tr (prawy-gorny), bl (lewy-dolny), br
 * (prawy-dolny, domyslny).
 */

#define _DEFAULT_SOURCE  /* POSIX - ta sama uwaga co w 7aweather.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include "../ui.h"

#define WIN_W          300
#define WIN_H          80
#define WIN_MARGIN     10   /* odleglosc od krawedzi ekranu */
#define PADDING        10
#define DEFAULT_SECS   5
#define MAX_MSG        4096
#define MAX_LINES      64
#define MEASURE_BUF    512

typedef struct { const char *ptr; int len; } LineSpan;

static char      g_message[MAX_MSG];
static LineSpan  g_lines[MAX_LINES];
static int       g_line_count = 0;

/* ------------------------------------------------------------------ */
/* Zawijanie tekstu (uproszczone - bez suwaka, jak w 7amessage.c)     */
/* ------------------------------------------------------------------ */

static int
MeasureWidth(UiCtx *ctx, const char *s, int len)
{
    char buf[MEASURE_BUF];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;

    if (n < 0) n = 0;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    return ui_text_width(ctx, buf);
}

static void
WrapMessage(UiCtx *ctx, int wrap_w)
{
    const char *p = g_message;

    g_line_count = 0;
    if (wrap_w < 10) wrap_w = 10;

    while (*p && g_line_count < MAX_LINES) {
        const char *para_end = strchr(p, '\n');
        const char *seg_start = p;
        const char *last_break = NULL;
        const char *cur = p;

        if (!para_end) para_end = p + strlen(p);

        if (para_end == p) {
            g_lines[g_line_count].ptr = p;
            g_lines[g_line_count].len = 0;
            g_line_count++;
            p = (*para_end == '\n') ? para_end + 1 : para_end;
            continue;
        }

        while (g_line_count < MAX_LINES) {
            int at_end = (cur == para_end);

            if (at_end || *cur == ' ') {
                int seg_len = (int)(cur - seg_start);

                if (MeasureWidth(ctx, seg_start, seg_len) > wrap_w && last_break) {
                    g_lines[g_line_count].ptr = seg_start;
                    g_lines[g_line_count].len = (int)(last_break - seg_start);
                    g_line_count++;
                    seg_start = last_break + 1;
                    last_break = NULL;
                    continue;
                }
                if (at_end) {
                    g_lines[g_line_count].ptr = seg_start;
                    g_lines[g_line_count].len = (int)(cur - seg_start);
                    g_line_count++;
                    break;
                }
                last_break = cur;
            }
            cur++;
        }
        p = (*para_end == '\n') ? para_end + 1 : para_end;
    }
}

/* ------------------------------------------------------------------ */
/* Rysowanie                                                           */
/* ------------------------------------------------------------------ */

static void
draw(UiCtx *ctx, int win_w, int win_h, long ms_left, long total_ms)
{
    static UiBoxStyle style;
    static int ready = 0;
    static int cache_w = -1;
    int lh = ui_line_height(ctx);
    int wrap_w;
    int i;
    UiBox *box;
    UiRect bar_r;

    if (lh < 1) lh = 14;

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 4;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = 4;
        style.border_w = 0;
        style.gap = 2;
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    wrap_w = win_w - 2 * (style.margin_l + style.padding_l);
    if (wrap_w < 10) wrap_w = 10;

    if (cache_w != wrap_w) {
        WrapMessage(ctx, wrap_w);
        cache_w = wrap_w;
    }

    box = ui_box_begin(ctx, "msg", 0, 0, win_w, &style);
    for (i = 0; i < g_line_count; i++) {
        char buf[MEASURE_BUF];
        int len = g_lines[i].len;
        UiRect row = ui_box_next_rect(box, lh);

        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        memcpy(buf, g_lines[i].ptr, (size_t)len);
        buf[len] = '\0';
        ui_label(ctx, row, buf);
    }
    ui_box_end(box);

    /* pasek postępu timeoutu na dole okna */
    if (total_ms > 0) {
        double frac = (double)ms_left / (double)total_ms;
        bar_r = (UiRect){ 0, win_h - 4, win_w, 4 };
        ui_fill_rect(ctx, bar_r, ui_theme_bg(ctx));
        bar_r.w = (int)(win_w * frac);
        ui_fill_rect(ctx, bar_r, ui_theme_accent(ctx));
    }
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
    XSetWindowAttributes wa;
    int win_w = WIN_W, win_h = WIN_H;
    int win_x = 0, win_y = 0;
    int secs = DEFAULT_SECS;
    int pos_x_neg = 1, pos_y_neg = 1; /* domyslnie br = prawy-dolny */
    int i, running, redraw;
    long deadline_ms, total_ms;
    XEvent ev;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            secs = atoi(argv[++i]);
            if (secs <= 0) secs = DEFAULT_SECS;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            const char *pos = argv[++i];

            pos_x_neg = (pos[1] == 'r') ? 1 : 0;
            pos_y_neg = (pos[0] == 'b') ? 1 : 0;
        } else {
            /* argumenty nie-opcyjne: tresc powiadomienia */
            size_t off = strlen(g_message);

            if (off > 0 && off < MAX_MSG - 1)
                g_message[off++] = ' ';
            snprintf(g_message + off, MAX_MSG - off, "%s", argv[i]);
        }
    }

    /* brak argumentow = czytaj ze stdin */
    if (g_message[0] == '\0') {
        ssize_t n = read(STDIN_FILENO, g_message, MAX_MSG - 1);

        if (n > 0) {
            g_message[n] = '\0';
            /* odetnij trailing newline */
            while (n > 0 && (g_message[n-1] == '\n' || g_message[n-1] == '\r'))
                g_message[--n] = '\0';
        }
    }

    if (g_message[0] == '\0') {
        fprintf(stderr, "Uzycie: %s [-t SEKUNDY] [-p tl|tr|bl|br] [tresc...]\n", argv[0]);
        return 1;
    }

#ifdef __OpenBSD__
    {
        const char *home = getenv("HOME");
        char home_fontcfg[1024];
        char xauth_buf[1024];
        const char *xauth_path;

        snprintf(home_fontcfg, sizeof(home_fontcfg), "%s/.fontconfig", home ? home : ".");
        if (unveil("/usr", "r") == -1) { perror("unveil"); return 1; }
        if (unveil("/etc", "r") == -1) { perror("unveil"); return 1; }
        if (unveil("/var", "r") == -1) { perror("unveil"); return 1; }
        if (unveil("/tmp/.X11-unix", "rw") == -1) { perror("unveil"); return 1; }
        if (unveil(home_fontcfg, "r") == -1) { perror("unveil"); return 1; }
        xauth_path = getenv("XAUTHORITY");
        if (!xauth_path) {
            snprintf(xauth_buf, sizeof(xauth_buf), "%s/.Xauthority", home ? home : ".");
            xauth_path = xauth_buf;
        }
        if (unveil(xauth_path, "r") == -1) { perror("unveil"); return 1; }
        if (unveil(NULL, NULL) == -1) { perror("unveil"); return 1; }
        if (pledge("stdio rpath unix prot_exec", NULL) == -1) {
            perror("pledge");
            return 1;
        }
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7anotify: brak polaczenia z X11\n");
        return 1;
    }

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);

    /* oblicz pozycje w rogu po poznaniu rozmiaru ekranu */
    win_x = pos_x_neg
        ? DisplayWidth(dpy, screen)  - win_w - WIN_MARGIN
        : WIN_MARGIN;
    win_y = pos_y_neg
        ? DisplayHeight(dpy, screen) - win_h - WIN_MARGIN
        : WIN_MARGIN;

    wa.override_redirect = True;
    win = XCreateWindow(dpy, root, win_x, win_y, win_w, win_h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect, &wa);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           KeyPressMask | StructureNotifyMask);
    XStoreName(dpy, win, "7aNotify");
    XMapRaised(dpy, win);

    gc  = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "DejaVu Sans-9", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7anotify: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }

    total_ms   = (long)secs * 1000L;
    deadline_ms = now_ms() + total_ms;
    running    = 1;
    redraw     = 1;

    while (running) {
        long now   = now_ms();
        long ms_left = deadline_ms - now;
        int xfd    = ConnectionNumber(dpy);
        fd_set fds;
        struct timeval tv;

        if (ms_left <= 0)
            break;

        /* odswiezaj co 100ms (pasek timeoutu) gdy brak eventow */
        if (!XPending(dpy)) {
            long wait = ms_left < 100 ? ms_left : 100;

            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            tv.tv_sec  = wait / 1000;
            tv.tv_usec = (wait % 1000) * 1000;
            select(xfd + 1, &fds, NULL, NULL, &tv);
            redraw = 1;
        }

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
                break;
            case ButtonPress:
                running = 0;
                break;
            case KeyPress: {
                char keybuf[16];
                KeySym ks;

                XLookupString(&ev.xkey, keybuf, sizeof(keybuf), &ks, NULL);
                if (ks == XK_Escape || ks == XK_Return || ks == XK_space)
                    running = 0;
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

        if (redraw && running) {
            long ms_rem = deadline_ms - now_ms();

            if (ms_rem < 0) ms_rem = 0;
            ui_begin_frame(ctx);
            draw(ctx, win_w, win_h, ms_rem, total_ms);
            ui_end_frame(ctx);
            redraw = 0;
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
