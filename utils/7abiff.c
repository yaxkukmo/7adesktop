/*
 * 7abiff.c - odpowiednik xbiff: ikonka skrzynki mbox z licznikiem
 *   nieprzeczytanych wiadomosci i podpinanymi komendami.
 *
 * Nowa apka (nie port Xt/Xaw). Roznice wzgledem xbiff:
 *  - zamiast XBell i opuszczania flagi klikiem - dwie komendy z zasobow X
 *    (fork + execl("/bin/sh","sh","-c",...), fire-and-forget jak w
 *    utils/7aexit.c):
 *      7aBiff.clickCommand    - przycisk "Open" (np. "xterm -e mutt"),
 *      7aBiff.newMailCommand  - gdy flaga nowej poczty sie podnosi albo
 *                               przy podniesionej przybywa wiadomosci;
 *                               liczba trafia do skryptu jako $1, wiec
 *                               dzwiek + powiadomienie graficzne robi
 *                               skrypt uzytkownika, nie apka.
 *  - uklad: ikona na zwyklym tle okna (nie przycisk), po prawej male
 *    pole z liczba nieprzeczytanych i pod nim przycisk "Open"; liczba
 *    takze w tytule okna.
 *
 * Zrodlo: plik mbox (7aBiff.file, domyslnie $MAIL, potem /var/mail/$USER).
 * Co 7aBiff.update sekund (domyslnie 30) stat() pliku. Ikona "pelna" =
 * plik niepusty i mtime > atime (konwencja biff/powloki/mutt: czytnik
 * podbija atime przy otwarciu skrzynki), wiec po przeczytaniu poczty
 * flaga opada, nawet gdy zostaja wiadomosci bez 'R'. Pole z liczba
 * pokazuje nieprzeczytane: wiadomosc = linia "From " na poczatku pliku
 * albo po pustej linii, nieprzeczytana = brak 'R' w "Status:";
 * parsowanie tylko gdy zmienil sie rozmiar albo mtime, po nim atime
 * przywracany (inaczej wlasny odczyt zbilby flage). Brak pliku = 0.
 *
 * Ikony: pliki XBM z zasobow 7aBiff.fullPixmap/emptyPixmap (+ ...Mask),
 * a gdy brak - te same zasoby oryginalnego xbiff (xbiff*fullPixmap...),
 * wczytywane XReadBitmapFileData. Maska wypelniana kolorem
 * 7aBiff.fillColor (domyslnie boxBackground), bity w
 * 7aBiff.iconForeground (domyslnie globalny iconForeground; ikonka WM
 * zawsze w globalnym). Bez zasobow albo przy bledzie
 * pliku: wbudowane Material Symbols (Outlined, Apache 2.0) mail i
 * mark_email_unread, 48px SVG -> rsvg-convert, prog alfa 50%. Ta sama
 * bitmapa sluzy za ikonke WMHints (o ile <= UI_ICON_MAX).
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
#include <fcntl.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/mail_48.xbm"
#include "icons/mark_email_unread_48.xbm"

#define V_MARGIN     4
#define UPDATE_DFLT  30
#define UPDATE_MAX   3600
#define ICON_MAX     128   /* bitmapy z zasobow; wieksze odrzucane */
#define COL_GAP      16    /* ikona <-> kolumna licznik/przycisk */
#define ROW_GAP      4     /* licznik <-> przycisk */

/* ------------------------------------------------------------------
 * Konfiguracja (zasoby X, czytane raz przy starcie)
 * ------------------------------------------------------------------ */

typedef struct {
    char path[1024];
    char click_cmd[512];
    char newmail_cmd[512];
    int  update;               /* sekundy */
    char full_pix[1024], full_mask[1024];
    char empty_pix[1024], empty_mask[1024];
    char fill_color[64];       /* "" = boxBackground */
    char icon_color[64];       /* "" = iconForeground */
} Config;

/* Sciezka bitmapy: 7aBiff.KEY, a gdy brak - zasob oryginalnego xbiff
 * (xbiff*fullPixmap itd.), zeby istniejace ~/.Xresources dzialalo bez zmian. */
static void
pixmap_resource(Display *dpy, const char *key, const char *Key, char *out, size_t cap)
{
    char name[64], cls[64], xbiff[1024];

    snprintf(name, sizeof(name), "xbiff.%s", key);
    snprintf(cls, sizeof(cls), "XBiff.%s", Key);
    ui_resource_string(dpy, name, cls, xbiff, sizeof(xbiff), "");
    snprintf(name, sizeof(name), "7aBiff.%s", key);
    snprintf(cls, sizeof(cls), "7aBiff.%s", Key);
    ui_resource_string(dpy, name, cls, out, cap, xbiff);
}

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
    pixmap_resource(dpy, "fullPixmap", "FullPixmap",
                    cfg->full_pix, sizeof(cfg->full_pix));
    pixmap_resource(dpy, "fullPixmapMask", "FullPixmapMask",
                    cfg->full_mask, sizeof(cfg->full_mask));
    pixmap_resource(dpy, "emptyPixmap", "EmptyPixmap",
                    cfg->empty_pix, sizeof(cfg->empty_pix));
    pixmap_resource(dpy, "emptyPixmapMask", "EmptyPixmapMask",
                    cfg->empty_mask, sizeof(cfg->empty_mask));
    ui_resource_string(dpy, "7aBiff.fillColor", "7aBiff.FillColor",
                       cfg->fill_color, sizeof(cfg->fill_color), "");
    ui_resource_string(dpy, "7aBiff.iconForeground", "7aBiff.IconForeground",
                       cfg->icon_color, sizeof(cfg->icon_color), "");
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

/* Stan skrzynki. fresh = "nowa poczta" w sensie biff/xbiff/powloki:
 * plik niepusty i zmodyfikowany po ostatnim odczycie (mtime > atime).
 * Czytnik (mutt, mail) podbija atime przy otwarciu skrzynki, wiec
 * flaga opada po przeczytaniu poczty - niezaleznie od tego, ile
 * wiadomosci zostalo bez 'R' w Status:. unread to tylko liczba w polu. */
typedef struct {
    int unread;
    int fresh;
} MailState;

/* Nasz wlasny odczyt pliku podbija atime (na Linuksie z relatime tez -
 * wlasnie gdy atime <= mtime), co zbiloby flage fresh. Przywracamy
 * atime sprzed odczytu, mtime bez zmian (UTIME_OMIT). */
static void
restore_atime(const char *path, const struct stat *st)
{
    struct timespec ts[2];

    ts[0].tv_sec  = st->st_atime;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec  = 0;
    ts[1].tv_nsec = UTIME_OMIT;
    utimensat(AT_FDCWD, path, ts, 0);
}

/* stat() co wywolanie (fresh zalezy tez od atime, ktory zmienia
 * czytnik poczty); parsowanie tylko gdy zmienil sie rozmiar albo mtime.
 * Zwraca 1, gdy stan *ms sie zmienil. */
static int
mbox_poll(const char *path, MailState *ms)
{
    static off_t  last_size = -1;
    static time_t last_mtime = -1;
    static int    last_unread = 0;
    struct stat   st;
    MailState     n;

    if (stat(path, &st) != 0) {
        last_size = -1;
        last_mtime = -1;
        last_unread = 0;
        n.unread = 0;
        n.fresh = 0;
    } else {
        if (st.st_size != last_size || st.st_mtime != last_mtime) {
            last_size = st.st_size;
            last_mtime = st.st_mtime;
            last_unread = 0;
            if (st.st_size > 0) {
                last_unread = mbox_count_unread(path);
                restore_atime(path, &st);
            }
        }
        n.unread = last_unread;
        n.fresh = st.st_size > 0 && st.st_mtime > st.st_atime;
    }
    if (n.unread == ms->unread && n.fresh == ms->fresh)
        return 0;
    *ms = n;
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
static XColor      g_fill;     /* wypelnienie maski ikony, patrz main */
static XColor      g_icon_fg;  /* bity ikony, patrz main */

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

/* Ikona skrzynki: bitmapa + opcjonalna maska ksztaltu (format XBM, jak
 * ui_draw_bitmap). bits/mask NULL = brak (mask NULL = bez wypelnienia). */
typedef struct {
    int                  w, h;
    const unsigned char *bits;
    const unsigned char *mask;
} Icon;

static Icon g_icon_empty = { mail_48_width, mail_48_height, mail_48_bits, NULL };
static Icon g_icon_full  = { mark_email_unread_48_width, mark_email_unread_48_height,
                             mark_email_unread_48_bits, NULL };

/* Wczytuje plik XBM z dysku. Wynik (XReadBitmapFileData = malloc Xlib)
 * zyje do konca programu - zwalniany XFree w free_icon. */
static unsigned char *
read_xbm(const char *path, int *w, int *h)
{
    unsigned int   bw, bh;
    unsigned char *data;
    int            hx, hy;

    if (!path[0])
        return NULL;
    if (XReadBitmapFileData(path, &bw, &bh, &data, &hx, &hy) != BitmapSuccess) {
        fprintf(stderr, "7abiff: nie mozna wczytac bitmapy %s\n", path);
        return NULL;
    }
    if (bw == 0 || bh == 0 || bw > ICON_MAX || bh > ICON_MAX) {
        fprintf(stderr, "7abiff: bitmapa %s ma rozmiar poza 1..%d\n", path, ICON_MAX);
        XFree(data);
        return NULL;
    }
    *w = (int)bw;
    *h = (int)bh;
    return data;
}

/* Zastepuje wbudowana ikone plikiem z zasobow (pixmap + maska). Maska
 * innego rozmiaru niz pixmapa jest odrzucana. Przy bledzie pixmapy
 * zostaje ikona wbudowana. */
static void
load_icon(Icon *ic, const char *path, const char *mask_path)
{
    unsigned char *bits, *mask;
    int            w, h, mw, mh;

    bits = read_xbm(path, &w, &h);
    if (!bits)
        return;
    mask = read_xbm(mask_path, &mw, &mh);
    if (mask && (mw != w || mh != h)) {
        fprintf(stderr, "7abiff: maska %s ma inny rozmiar niz %s\n", mask_path, path);
        XFree(mask);
        mask = NULL;
    }
    ic->w = w;
    ic->h = h;
    ic->bits = bits;
    ic->mask = mask;
}

/* tylko ikony z load_icon - wbudowane (mask == NULL i bits ze .xbm w
 * binarce) rozpoznawane po porownaniu z tablicami statycznymi */
static void
free_icon(Icon *ic)
{
    if (ic->bits != mail_48_bits && ic->bits != mark_email_unread_48_bits)
        XFree((void *)ic->bits);
    if (ic->mask)
        XFree((void *)ic->mask);
}

/* Ikona okna (WM) tylko przy przejsciu 0 <-> >0 - ui_set_window_icon
 * przepisuje _NET_WM_ICON i tworzy nowa bitmape serwera. */
static void
update_wm_icon(UiCtx *ctx, int fresh)
{
    static int  shown = -1; /* -1: jeszcze nic, 0: pusta, 1: poczta */
    int         want = fresh != 0;
    const Icon *ic = want ? &g_icon_full : &g_icon_empty;

    if (want == shown)
        return;
    shown = want;
    ui_set_window_icon(ctx, ic->w, ic->h, ic->bits);
}

/* Ikona na zwyklym tle okna: maska w g_fill (wnetrze ksztaltu, jak tlo
 * xbiff), bity w g_icon_fg. */
static void
draw_icon(UiCtx *ctx, const Icon *ic, int x, int y)
{
    if (ic->mask)
        ui_draw_bitmap(ctx, x, y, ic->w, ic->h, ic->mask, &g_fill);
    ui_draw_bitmap(ctx, x, y, ic->w, ic->h, ic->bits, &g_icon_fg);
}

/* Pole z liczba nieprzeczytanych - nieedytowalny input: wyglad
 * niezfokusowanego ui_textbox (tlo inputBg, wklesly bevel), ale bez
 * ui_textbox, zeby klik nie dawal fokusu ani kursora. */
static void
draw_count(UiCtx *ctx, UiRect r, int unread)
{
    char txt[16];

    snprintf(txt, sizeof(txt), "%d", unread);
    ui_fill_round_rect(ctx, r, ui_theme_input_bg(ctx));
    ui_draw_bevel(ctx, r, 1, ui_theme_line_fg(ctx), 1);
    ui_label_centered(ctx, r, txt);
}

/* Zwraca 1 przy kliknieciu przycisku "Open" */
static int
draw(UiCtx *ctx, int win_w, int win_h, const MailState *ms)
{
    int         margin = ui_window_margin(ctx);
    int         row_h;
    const Icon *ic;
    UiRect      r, col;

    r.x = margin;
    r.y = V_MARGIN;
    r.w = win_w - 2 * margin;
    r.h = win_h - 2 * V_MARGIN;
    if (r.w < 4) r.w = 4;
    if (r.h < 4) r.h = 4;

    update_title(ms->unread);
    update_wm_icon(ctx, ms->fresh);

    /* lewo: ikona na tle okna, wysrodkowana w pionie */
    ic = ms->fresh ? &g_icon_full : &g_icon_empty;
    ui_set_clip(ctx, r);
    draw_icon(ctx, ic, r.x, r.y + (r.h - ic->h) / 2);
    ui_clear_clip(ctx);

    /* prawo: pole z liczba nad przyciskiem, oba wysrodkowane w pionie */
    row_h = ui_line_height(ctx) + 6;
    col.x = r.x + ic->w + COL_GAP;
    col.w = r.x + r.w - col.x;
    if (col.w < 8) col.w = 8;
    col.h = row_h;
    col.y = r.y + (r.h - (2 * row_h + ROW_GAP)) / 2;
    draw_count(ctx, col, ms->unread);
    col.y += row_h + ROW_GAP;
    return ui_button(ctx, col, "Open");
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

/* Kolor z zasobu albo dflt (pusty zasob / zla nazwa). XAllocColor w
 * domyslnej colormapie - zwalniane przez XCloseDisplay razem z reszta
 * zasobow polaczenia (jak kolory ui_color w pozostalych apkach). */
static void
load_color(UiCtx *ctx, const char *name, const char *res, const XColor *dflt,
           XColor *out)
{
    *out = *dflt;
    if (name[0] && !ui_color(ctx, name, out)) {
        fprintf(stderr, "7abiff: nieznany kolor %s %s\n", res, name);
        *out = *dflt;
    }
}

static double
now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Sprawdza skrzynke; odpala newMailCommand, gdy flaga nowej poczty
 * wlasnie sie podniosla albo, przy podniesionej, przybylo wiadomosci.
 * Zwraca 1, gdy trzeba przerysowac. */
static int
check_mail(const Config *cfg, MailState *ms)
{
    MailState prev = *ms;
    char      arg[16];

    if (!mbox_poll(cfg->path, ms))
        return 0;
    if (ms->fresh && (!prev.fresh || ms->unread > prev.unread)) {
        snprintf(arg, sizeof(arg), "%d", ms->unread);
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
    int         running, redraw;
    int         i;
    double      next_check;
    char        app_name[64] = "7aBiff";
    char        app_title[64] = "";
    XEvent      ev;
    MailState   ms = { 0, 0 };

    win_w = 138; /* ikona 48px + kolumna licznik/"Open" */
    win_h = 60;  /* 4 + ikona 48px + 4 + zapas */
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
    load_icon(&g_icon_full, cfg.full_pix, cfg.full_mask);
    load_icon(&g_icon_empty, cfg.empty_pix, cfg.empty_mask);

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
    load_color(ctx, cfg.fill_color, "fillColor", ui_theme_box_bg(ctx), &g_fill);
    load_color(ctx, cfg.icon_color, "iconForeground", ui_theme_icon_fg(ctx), &g_icon_fg);
    xfd        = ConnectionNumber(dpy);
    check_mail(&cfg, &ms);
    update_wm_icon(ctx, ms.fresh); /* przed pierwszym Expose */
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
                clicked = draw(ctx, win_w, win_h, &ms);
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
        if (check_mail(&cfg, &ms)) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h, &ms))
                run_cmd(cfg.click_cmd, NULL);
            ui_end_frame(ctx);
        }
    }

    ui_destroy(ctx);
    free_icon(&g_icon_full);
    free_icon(&g_icon_empty);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
