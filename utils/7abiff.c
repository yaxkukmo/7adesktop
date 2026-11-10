/*
 * 7abiff.c - odpowiednik xbiff: ikonka skrzynki mbox z licznikiem
 *   nieprzeczytanych wiadomosci i podpinanymi komendami.
 *
 * Nowa apka (nie port Xt/Xaw). Roznice wzgledem xbiff:
 *  - zamiast XBell i opuszczania flagi klikiem - dwie komendy z zasobow X
 *    (fork + execl("/bin/sh","sh","-c",...), fire-and-forget jak w
 *    utils/7aexit.c):
 *      7aBiff.clickCommand    - klik w ikonke (np. "xterm -e mutt"),
 *      7aBiff.newMailCommand  - przy WZROSCIE liczby nieprzeczytanych
 *                               (takze przy starcie, gdy cos juz czeka);
 *                               liczba trafia do skryptu jako $1, wiec
 *                               dzwiek + powiadomienie graficzne robi
 *                               skrypt uzytkownika, nie apka.
 *  - licznik nieprzeczytanych w kolku na ikonce i w tytule okna.
 *
 * Zrodlo: plik mbox (7aBiff.file, domyslnie $MAIL, potem /var/mail/$USER).
 * Co 7aBiff.update sekund (domyslnie 30) stat() pliku; parsowanie tylko
 * gdy zmienil sie rozmiar albo mtime. Wiadomosc = linia "From " na
 * poczatku pliku albo po pustej linii; nieprzeczytana = brak 'R' w
 * naglowku "Status:" (konwencja mutt/mail/pine). Brak pliku = 0 wiadomosci.
 *
 * Ikony: Material Symbols (Outlined, Apache 2.0) mail i mark_email_unread,
 * 48px SVG -> rsvg-convert, prog alfa 50% (mail: lewa polowa odbita na
 * prawa), bez przyciecia - te same bitmapy sluza za ikonke WMHints.
 * Timer: select() z timeoutem na deskryptorze polaczenia X (jak 7aclock.c).
 * Zamkniecie: Escape lub WM close.
 */

/* clock_gettime/strncasecmp sa poza ISO C99 - patrz utils/7aclock.c */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/mail_48.xbm"
#include "icons/mark_email_unread_48.xbm"

#define V_MARGIN     4
#define UPDATE_DFLT  30
#define UPDATE_MAX   3600

/* ------------------------------------------------------------------
 * Konfiguracja (zasoby X, czytane raz przy starcie)
 * ------------------------------------------------------------------ */

typedef struct {
    char path[1024];
    char click_cmd[512];
    char newmail_cmd[512];
    int  update;               /* sekundy */
} Config;

static void
load_config(Display *dpy, Config *cfg)
{
    char        dflt[1024], buf[32];
    const char *mail = getenv("MAIL");
    const char *user = getenv("USER");
    long        v;

    if (mail && *mail)
        snprintf(dflt, sizeof(dflt), "%s", mail);
    else
        snprintf(dflt, sizeof(dflt), "/var/mail/%s", user ? user : "");

    ui_resource_string(dpy, "7aBiff.file", "7aBiff.File",
                       cfg->path, sizeof(cfg->path), dflt);
    ui_resource_string(dpy, "7aBiff.clickCommand", "7aBiff.ClickCommand",
                       cfg->click_cmd, sizeof(cfg->click_cmd), "");
    ui_resource_string(dpy, "7aBiff.newMailCommand", "7aBiff.NewMailCommand",
                       cfg->newmail_cmd, sizeof(cfg->newmail_cmd), "");
    ui_resource_string(dpy, "7aBiff.update", "7aBiff.Update",
                       buf, sizeof(buf), "");
    v = strtol(buf, NULL, 10);
    cfg->update = (v >= 1 && v <= UPDATE_MAX) ? (int)v : UPDATE_DFLT;
}

/* ------------------------------------------------------------------
 * mbox - logika niezalezna od ui
 * ------------------------------------------------------------------ */

/* Liczba nieprzeczytanych wiadomosci w mbox. Linie dluzsze niz bufor
 * sa czytane w kawalkach - analizowany jest tylko pierwszy kawalek
 * (at_bol), reszta pomijana. */
static int
mbox_count_unread(const char *path)
{
    FILE *f;
    char  line[1024];
    int   at_bol = 1, prev_blank = 1, in_hdr = 0, seen = 0, msgs = 0;
    int   unread = 0;

    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        int    bol = at_bol;

        at_bol = (len > 0 && line[len - 1] == '\n');
        if (!bol)
            continue;
        if (prev_blank && strncmp(line, "From ", 5) == 0) {
            if (msgs > 0 && !seen)
                unread++;
            msgs++;
            in_hdr = 1;
            seen = 0;
        } else if (in_hdr) {
            if (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n'))
                in_hdr = 0;
            else if (strncasecmp(line, "Status:", 7) == 0 && strchr(line + 7, 'R'))
                seen = 1;
        }
        prev_blank = (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n'));
    }
    if (msgs > 0 && !seen)
        unread++;
    fclose(f);
    return unread;
}

/* Przelicza licznik tylko gdy plik sie zmienil (rozmiar/mtime).
 * Zwraca 1, gdy *unread sie zmienil. */
static int
mbox_poll(const char *path, int *unread)
{
    static off_t  last_size = -1;
    static time_t last_mtime = -1;
    struct stat   st;
    int           n;

    if (stat(path, &st) != 0) {
        last_size = -1;
        last_mtime = -1;
        n = 0;
    } else {
        if (st.st_size == last_size && st.st_mtime == last_mtime)
            return 0;
        last_size = st.st_size;
        last_mtime = st.st_mtime;
        n = st.st_size > 0 ? mbox_count_unread(path) : 0;
    }
    if (n == *unread)
        return 0;
    *unread = n;
    return 1;
}

/* ------------------------------------------------------------------
 * Wykonanie komendy: sh -c CMD 7abiff [ARG] - ARG widoczny jako $1.
 * ------------------------------------------------------------------ */

static void
run_cmd(const char *cmd, const char *arg)
{
    pid_t pid;

    if (!cmd[0])
        return;
    pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, "7abiff", arg, (char *)NULL);
        _exit(1);
    }
    /* rodzic nie czeka - zombie sprzatane przez SIG_IGN ustawiony w main */
}

/* ------------------------------------------------------------------
 * Rysowanie
 * ------------------------------------------------------------------ */

static Display    *g_dpy;
static Window      g_win;
static const char *g_base_title;

/* XStoreName tylko przy zmianie licznika (round-trip do WM) */
static void
update_title(int unread)
{
    static int shown = -1;
    char       title[128];

    if (unread == shown)
        return;
    shown = unread;
    if (unread > 0) {
        snprintf(title, sizeof(title), "%s (%d)", g_base_title, unread);
        XStoreName(g_dpy, g_win, title);
    } else {
        XStoreName(g_dpy, g_win, g_base_title);
    }
}

/* Kolko z liczba w prawym gornym rogu ikonki */
static void
draw_badge(UiCtx *ctx, int icon_x, int icon_y, int icon_w, int unread)
{
    char   txt[8];
    int    r, cx, cy;
    UiRect tr;

    if (unread > 99)
        snprintf(txt, sizeof(txt), "99+");
    else
        snprintf(txt, sizeof(txt), "%d", unread);
    r = (ui_line_height(ctx) + 4) / 2;
    if (2 * r < ui_text_width(ctx, txt) + 4)
        r = (ui_text_width(ctx, txt) + 5) / 2;
    cx = icon_x + icon_w - r;
    cy = icon_y + r;
    ui_fill_circle(ctx, cx, cy, r, ui_theme_accent(ctx));
    tr.x = cx - r;
    tr.y = cy - r;
    tr.w = 2 * r;
    tr.h = 2 * r;
    ui_label_centered_fg(ctx, tr, txt, ui_theme_fg(ctx));
}

/* Zwraca 1 przy kliknieciu ikonki */
static int
draw(UiCtx *ctx, int win_w, int win_h, int unread)
{
    int                  margin = ui_window_margin(ctx);
    int                  w, h, x, y, clicked;
    const unsigned char *bits;
    UiRect               r;

    r.x = margin;
    r.y = V_MARGIN;
    r.w = win_w - 2 * margin;
    r.h = win_h - 2 * V_MARGIN;
    if (r.w < 4) r.w = 4;
    if (r.h < 4) r.h = 4;

    update_title(unread);
    clicked = ui_button(ctx, r, "");

    if (unread > 0) {
        w = mark_email_unread_48_width;
        h = mark_email_unread_48_height;
        bits = mark_email_unread_48_bits;
    } else {
        w = mail_48_width;
        h = mail_48_height;
        bits = mail_48_bits;
    }
    x = r.x + (r.w - w) / 2;
    y = r.y + (r.h - h) / 2;
    ui_set_clip(ctx, r);
    ui_draw_bitmap(ctx, x, y, w, h, bits, ui_theme_icon_fg(ctx));
    if (unread > 0)
        draw_badge(ctx, x, y, w, unread);
    ui_clear_clip(ctx);
    return clicked;
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

static double
now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Sprawdza skrzynke; przy wzroscie licznika odpala newMailCommand.
 * Zwraca 1, gdy trzeba przerysowac. */
static int
check_mail(const Config *cfg, int *unread)
{
    int  prev = *unread;
    char arg[16];

    if (!mbox_poll(cfg->path, unread))
        return 0;
    if (*unread > prev) {
        snprintf(arg, sizeof(arg), "%d", *unread);
        run_cmd(cfg->newmail_cmd, arg);
    }
    return 1;
}

int
main(int argc, char **argv)
{
    Display    *dpy;
    int         screen, xfd;
    Window      root, win;
    GC          gc;
    UiCtx      *ctx;
    XSizeHints *szh;
    Atom        wm_del;
    Config      cfg;
    int         win_w, win_h, win_x, win_y;
    int         geo_mask;
    int         running, redraw, unread;
    int         i;
    double      next_check;
    char        app_name[64] = "7aBiff";
    char        app_title[64] = "";
    XEvent      ev;

    win_w = 64;
    win_h = 60; /* 4 + ikona 48px + 4 + zapas */
    win_x = 0;
    win_y = 0;
    geo_mask = 0;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            int gx = 0, gy = 0;
            unsigned int gw = (unsigned)win_w, gh = (unsigned)win_h;
            geo_mask = XParseGeometry(argv[i + 1], &gx, &gy, &gw, &gh);
            if (geo_mask & WidthValue)  win_w = (int)gw;
            if (geo_mask & HeightValue) win_h = (int)gh;
            if (geo_mask & XValue)      win_x = gx;
            if (geo_mask & YValue)      win_y = gy;
            i++;
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[i + 1]);
            i++;
        }
    }

    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7abiff: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    load_config(dpy, &cfg);

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);

    /* XParseGeometry zwraca ujemny offset dla "-X", stad "+" */
    if (geo_mask & XNegative)
        win_x = DisplayWidth(dpy, screen)  - win_w + win_x;
    if (geo_mask & YNegative)
        win_y = DisplayHeight(dpy, screen) - win_h + win_y;

    win = XCreateSimpleWindow(dpy, root,
          win_x, win_y,
          (unsigned)win_w, (unsigned)win_h, 0,
          BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XSelectInput(dpy, win,
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | StructureNotifyMask | KeyPressMask);
    g_dpy = dpy;
    g_win = win;
    g_base_title = app_title[0] ? app_title : app_name;
    XStoreName(dpy, win, g_base_title);
    XSetIconName(dpy, win, g_base_title);
    {
        XClassHint *ch = XAllocClassHint();
        if (ch) {
            ch->res_name  = app_name;
            ch->res_class = "7aBiff";
            XSetClassHint(dpy, win, ch);
            XFree(ch);
        }
    }

    wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    szh = XAllocSizeHints();
    if (szh) {
        szh->flags      = PMinSize;
        szh->min_width  = 24;
        szh->min_height = 24;
        if (geo_mask & (XValue | YValue)) {
            /* win_gravity - patrz komentarz w utils/7aexit.c */
            szh->flags |= USPosition | PWinGravity;
            szh->x = win_x;
            szh->y = win_y;
            if (geo_mask & XNegative)
                szh->win_gravity = (geo_mask & YNegative) ? SouthEastGravity
                                                          : NorthEastGravity;
            else
                szh->win_gravity = (geo_mask & YNegative) ? SouthWestGravity
                                                          : NorthWestGravity;
        }
        if (geo_mask & (WidthValue | HeightValue))
            szh->flags |= USSize;
        XSetWMNormalHints(dpy, win, szh);
        XFree(szh);
    }

    XMapWindow(dpy, win);

    /* gc: zwalniany XFreeGC na koncu main (i przy bledzie ui_init) */
    gc  = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7abiff: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, mail_48_width, mail_48_height, mail_48_bits);

    xfd        = ConnectionNumber(dpy);
    unread     = 0;
    check_mail(&cfg, &unread);
    next_check = now_mono() + cfg.update;
    running    = 1;
    redraw     = 1;

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
                redraw = 1;
                break;
            case KeyPress:
                if (XLookupKeysym(&ev.xkey, 0) == XK_Escape)
                    running = 0;
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width  != win_w ||
                    ev.xconfigure.height != win_h) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;
                    ui_resize(ctx, win_w, win_h);
                }
                redraw = 1;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_del)
                    running = 0;
                break;
            }

            if (redraw && running) {
                int clicked;

                ui_begin_frame(ctx);
                clicked = draw(ctx, win_w, win_h, unread);
                ui_end_frame(ctx);
                redraw = 0;
                if (clicked)
                    run_cmd(cfg.click_cmd, NULL);
            }
        }
        if (!running)
            break;

        {
            double         left = next_check - now_mono();
            struct timeval tv;
            fd_set         fds;

            if (left > 0) {
                tv.tv_sec  = (time_t)left;
                tv.tv_usec = (suseconds_t)((left - (double)tv.tv_sec) * 1e6);
                FD_ZERO(&fds);
                FD_SET(xfd, &fds);
                if (select(xfd + 1, &fds, NULL, NULL, &tv) != 0)
                    continue; /* event X (albo EINTR) - obsluz i licz dalej */
            }
        }
        next_check = now_mono() + cfg.update;
        if (check_mail(&cfg, &unread)) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h, unread))
                run_cmd(cfg.click_cmd, NULL);
            ui_end_frame(ctx);
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
