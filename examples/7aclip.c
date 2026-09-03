/*
 * 7aclip.c - historia schowka dla X11 na bibliotece ui.c/ui.h z tego
 * katalogu.
 *
 * Nowa apka (nie port Xt/Xaw). Sledzi zawartosc PRIMARY i CLIPBOARD:
 * co sekunde pyta wlasciciela PRIMARY o UTF-8 przez XConvertSelection
 * (protokol ICCCM - ten sam wzorzec co examples/7afm.c), dodaje nowe
 * wpisy do listy historii (max HIST_MAX, bez duplikatow). Klikniecie
 * pozycji na liscie: apka staje sie wlascicielem obu selekcji i serwuje
 * zawartosc kazdemu klientowi, ktory zada (SelectionRequest).
 *
 * Mechanizm odpytywania selekcji przez XConvertSelection:
 *   1. apka wywoluje XConvertSelection(dpy, PRIMARY, UTF8_STRING, ..., win)
 *   2. czeka na SelectionNotify w petli eventow
 *   3. jesli xselection.property != None, czyta przez XGetWindowProperty
 *   4. usuwa property przez XDeleteProperty (protokol INCR nie obslugiwany -
 *      bardzo dlugie selekcje beda pominiete, rzadki przypadek dla schowka)
 *
 * Bez XFixes - zero nowych zaleznosci poza X11.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include "../ui.h"

#define ICON_SIZE      32
#define ROW_H          20
#define HIST_MAX       40
#define HIST_ITEM_W    2048   /* max dlugosc jednego wpisu historii */
#define ITEMS_PER_PAGE 10     /* wierszy na strone (paginacja) */
#define POLL_MS        1000   /* jak czesto pytac o selekcje PRIMARY */
#define PROP_NAME      "_7ACLIP_PASTE"

/* ------------------------------------------------------------------ */
/* Historia                                                            */
/* ------------------------------------------------------------------ */

static char g_hist[HIST_MAX][HIST_ITEM_W];
static int  g_hist_n        = 0;
static int  g_selected      = -1;  /* zaznaczony wpis (serwowany jako selekcja) */
static int  g_page          = 0;   /* aktualna strona (paginacja) */
static int  g_claim_pending = 0;   /* flaga: draw() wykryl klikniecie, main() realizuje XSetSelectionOwner */

/* Dodaje wpis na poczatek historii (najnowszy na gorze), usuwa duplikaty. */
static void
HistPush(const char *text)
{
    int i, len;

    if (!text || !text[0]) return;

    /* pomijaj biale znaki */
    len = (int)strlen(text);
    while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r'
                       || text[len-1] == ' '))
        len--;
    if (len <= 0) return;

    /* usun istniejacy duplikat */
    for (i = 0; i < g_hist_n; i++) {
        if (strncmp(g_hist[i], text, (size_t)len) == 0
            && g_hist[i][len] == '\0') {
            /* przesuń wpis na poczatek */
            char tmp[HIST_ITEM_W];
            snprintf(tmp, sizeof(tmp), "%s", g_hist[i]);
            memmove(g_hist[1], g_hist[0],
                    (size_t)i * sizeof(g_hist[0]));
            snprintf(g_hist[0], HIST_ITEM_W, "%s", tmp);
            g_selected = -1;
            return;
        }
    }

    /* przesuń w dół, dodaj na górze */
    if (g_hist_n < HIST_MAX) g_hist_n++;
    memmove(g_hist[1], g_hist[0],
            (size_t)(g_hist_n - 1) * sizeof(g_hist[0]));
    snprintf(g_hist[0], HIST_ITEM_W, "%.*s", len, text);
    g_selected = -1;
}

/* ------------------------------------------------------------------ */
/* Atomy X11                                                           */
/* ------------------------------------------------------------------ */

static Atom a_utf8;        /* UTF8_STRING                     */
static Atom a_primary;     /* XA_PRIMARY                      */
static Atom a_clipboard;   /* CLIPBOARD                        */
static Atom a_prop;        /* _7ACLIP_PASTE (wlasna property) */
static Atom a_targets;     /* TARGETS                          */
static Atom a_incr;        /* INCR (do detekcji, nie obslugujemy) */

static void
InitAtoms(Display *dpy)
{
    a_utf8      = XInternAtom(dpy, "UTF8_STRING", False);
    a_primary   = XA_PRIMARY;
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_prop      = XInternAtom(dpy, PROP_NAME, False);
    a_targets   = XInternAtom(dpy, "TARGETS", False);
    a_incr      = XInternAtom(dpy, "INCR", False);
}

/* ------------------------------------------------------------------ */
/* Odpytywanie PRIMARY o zawartosc (krok 1: wyslij zadanie)           */
/* ------------------------------------------------------------------ */

static void
RequestPrimary(Display *dpy, Window win)
{
    XConvertSelection(dpy, a_primary, a_utf8, a_prop, win, CurrentTime);
}

/* Obsługa SelectionNotify (krok 2: odbierz wynik zapytania). */
static void
HandleSelectionNotify(Display *dpy, Window win, XSelectionEvent *ev)
{
    Atom actual_type;
    int actual_fmt;
    unsigned long nitems, bytes_left;
    unsigned char *data = NULL;

    if (ev->property == None) return;      /* wlasciciel odrzucil */
    if (ev->selection != a_primary) return; /* nie to co pytalismy */

    if (XGetWindowProperty(dpy, win, ev->property, 0, (HIST_ITEM_W / 4),
                           False, AnyPropertyType,
                           &actual_type, &actual_fmt,
                           &nitems, &bytes_left, &data) != Success) {
        XDeleteProperty(dpy, win, ev->property);
        return;
    }

    if (actual_type == a_incr) {
        /* INCR - zbyt duze, pomijamy (jak dokumentacja: rzadki przypadek) */
        if (data) XFree(data);
        XDeleteProperty(dpy, win, ev->property);
        return;
    }

    if (data && nitems > 0)
        HistPush((char *)data);

    if (data) XFree(data);
    XDeleteProperty(dpy, win, ev->property);
}

/* ------------------------------------------------------------------ */
/* Serwowanie selekcji (SelectionRequest od innych klientow)          */
/* ------------------------------------------------------------------ */

static void
HandleSelectionRequest(Display *dpy, XSelectionRequestEvent *req)
{
    XSelectionEvent reply;
    const char *content = (g_selected >= 0 && g_selected < g_hist_n)
                          ? g_hist[g_selected] : NULL;

    reply.type      = SelectionNotify;
    reply.display   = req->display;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target    = req->target;
    reply.property  = None;
    reply.time      = req->time;

    if (!content) {
        XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&reply);
        return;
    }

    if (req->target == a_targets) {
        /* odpowiedz lista obslogiwanych formatow */
        Atom supported[2] = { a_utf8, XA_STRING };
        XChangeProperty(dpy, req->requestor, req->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported, 2);
        reply.property = req->property;
    } else if (req->target == a_utf8 || req->target == XA_STRING) {
        XChangeProperty(dpy, req->requestor, req->property,
                        req->target, 8, PropModeReplace,
                        (unsigned char *)content,
                        (int)strlen(content));
        reply.property = req->property;
    }

    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&reply);
}

/* ------------------------------------------------------------------ */
/* Przejecie obu selekcji                                              */
/* ------------------------------------------------------------------ */

static void
ClaimSelections(Display *dpy, Window win)
{
    XSetSelectionOwner(dpy, a_primary,   win, CurrentTime);
    XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
}

/* ------------------------------------------------------------------ */
/* Ikonka: schowek                                                     */
/* ------------------------------------------------------------------ */

static Pixmap
MakeClipIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    /* deska schowka */
    XDrawRectangle(idpy, icon, gc, 6, 6, 20, 24);
    /* zacinaczek na gorze */
    XFillRectangle(idpy, icon, gc, 11, 3, 10, 5);
    /* linie tekstu */
    XDrawLine(idpy, icon, gc, 10, 13, 22, 13);
    XDrawLine(idpy, icon, gc, 10, 18, 22, 18);
    XDrawLine(idpy, icon, gc, 10, 23, 18, 23);
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
    int i, n_pages, page_start, page_end;

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

    n_pages = (g_hist_n + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (n_pages < 1) n_pages = 1;
    if (g_page >= n_pages) g_page = n_pages - 1;
    if (g_page < 0) g_page = 0;

    page_start = g_page * ITEMS_PER_PAGE;
    page_end   = page_start + ITEMS_PER_PAGE;
    if (page_end > g_hist_n) page_end = g_hist_n;

    /* --- naglowek: przyciski stron + licznik --- */
    y += 6;
    {
        UiRect hdr = { style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        UiRect prev_r, center_r, next_r;
        char page_label[32];

        snprintf(page_label, sizeof(page_label), "Historia (%d-%d z %d)",
                 g_hist_n > 0 ? page_start + 1 : 0, page_end, g_hist_n);
        ui_rect_split3(hdr, ROW_H, ROW_H, 4, &prev_r, &center_r, &next_r);
        if (ui_button(ctx, prev_r, "<") && g_page > 0) g_page--;
        ui_label_centered(ctx, center_r, page_label);
        if (ui_button(ctx, next_r, ">") && g_page < n_pages - 1) g_page++;
        y += ROW_H + 4;
    }

    /* --- lista wpisow --- */
    {
        UiBox *box = ui_box_begin(ctx, "hist", 0, y, win_w, &style);

        for (i = page_start; i < page_end; i++) {
            UiRect row  = ui_box_next_rect(box, ROW_H);
            UiRect mark_r, label_r;

            /* zaznaczenie klikniecia PRZED rysowaniem (wzorzec "stan przed rysowaniem").
             * UiCtx jest opaque - nie mozna tu wywolac ClaimSelections(ctx->dpy,...).
             * Ustawiamy g_selected i g_claim_pending; main() realizuje XSetSelectionOwner
             * po zakonczeniu klatki. */
            if (ui_hit_test(ctx, row)) {
                g_selected      = i;
                g_claim_pending = 1;
            }
            ui_rect_split3(row, ROW_H, 0, 4, &mark_r, &label_r, NULL);
            ui_selection_mark(ctx, mark_r, i == g_selected);
            ui_label_ellipsis(ctx, label_r, g_hist[i]);
        }

        if (g_hist_n == 0)
            ui_label_fg(ctx, ui_box_next_rect(box, ROW_H),
                        "(historia pusta - skopiuj cos do schowka)",
                        ui_theme_line_fg(ctx));

        ui_box_end(box);
        y += ui_box_height(ctx, "hist");
    }

    /* --- Clear --- */
    y += 10;
    {
        int btn_w = ui_text_width(ctx, "Clear") + 20;
        UiRect clear_r = { style.margin_l, y, btn_w, ROW_H };

        if (ui_button(ctx, clear_r, "Clear")) {
            g_hist_n   = 0;
            g_selected = -1;
            g_page     = 0;
        }
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

static Display *g_dpy;
static Window   g_win;

int
main(int argc, char **argv)
{
    int screen;
    Window root;
    GC gc;
    UiCtx *ctx;
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 420, win_h = 340;
    int win_x = 200, win_y = 200;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw;
    long next_poll;
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
        if (unveil("/usr", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/etc", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/var", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/tmp/.X11-unix", "rw") == -1) { perror("unveil"); return 1; }
        if (unveil(home_fontcfg, "r")      == -1) { perror("unveil"); return 1; }
        xauth_path = getenv("XAUTHORITY");
        if (!xauth_path) {
            snprintf(xauth_buf, sizeof(xauth_buf), "%s/.Xauthority",
                     home ? home : ".");
            xauth_path = xauth_buf;
        }
        if (unveil(xauth_path, "r")        == -1) { perror("unveil"); return 1; }
        if (unveil(NULL, NULL)             == -1) { perror("unveil"); return 1; }
        if (pledge("stdio rpath unix prot_exec", NULL) == -1) {
            perror("pledge");
            return 1;
        }
    }
#endif

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "7aclip: brak polaczenia z X11\n");
        return 1;
    }

    screen = DefaultScreen(g_dpy);
    root   = RootWindow(g_dpy, screen);

    InitAtoms(g_dpy);

    if (geom_mask & WidthValue)  win_w = (int)geom_w;
    if (geom_mask & HeightValue) win_h = (int)geom_h;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative)
              ? DisplayWidth(g_dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative)
              ? DisplayHeight(g_dpy, screen) - win_h + geom_y : geom_y;

    g_win = XCreateSimpleWindow(g_dpy, root, win_x, win_y, win_w, win_h, 0,
                                 BlackPixel(g_dpy, screen),
                                 WhitePixel(g_dpy, screen));
    XSelectInput(g_dpy, g_win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(g_dpy, g_win, "7aClip");
    XSetIconName(g_dpy, g_win, "7aClip");

    icon    = MakeClipIconPixmap(g_dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags       = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask   = icon;
    XSetWMHints(g_dpy, g_win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags     = PMinSize;
    sizehints->min_width  = 240;
    sizehints->min_height = 150;
    XSetWMNormalHints(g_dpy, g_win, sizehints);
    XFree(sizehints);

    XMapWindow(g_dpy, g_win);

    gc  = XCreateGC(g_dpy, g_win, 0, NULL);
    ctx = ui_init(g_dpy, g_win, gc, "DejaVu Sans-9", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7aclip: ui_init nie powiodlo sie\n");
        XFreeGC(g_dpy, gc);
        XFreePixmap(g_dpy, icon);
        XCloseDisplay(g_dpy);
        return 1;
    }

    next_poll = now_ms();  /* odpytaj od razu na starcie */
    running   = 1;
    redraw    = 1;

    while (running) {
        int xfd = ConnectionNumber(g_dpy);
        fd_set fds;
        struct timeval tv;
        long now, ms_left;

        if (!XPending(g_dpy)) {
            now      = now_ms();
            ms_left  = next_poll - now;

            if (ms_left <= 0) {
                RequestPrimary(g_dpy, g_win);
                next_poll = now + POLL_MS;
                ms_left   = POLL_MS;
            }

            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            tv.tv_sec  = ms_left / 1000;
            tv.tv_usec = (ms_left % 1000) * 1000;
            select(xfd + 1, &fds, NULL, NULL, &tv);
        }

        while (XPending(g_dpy)) {
            XNextEvent(g_dpy, &ev);

            /* Selekcja: wyslana odpowiedz na nasze XConvertSelection */
            if (ev.type == SelectionNotify) {
                HandleSelectionNotify(g_dpy, g_win, &ev.xselection);
                redraw = 1;
                continue;
            }

            /* Ktos pyta nas o zawartosc selekcji */
            if (ev.type == SelectionRequest) {
                HandleSelectionRequest(g_dpy, &ev.xselectionrequest);
                continue;
            }

            /* Stracilismy wlasnosc selekcji (ktos inny skopiował cos) */
            if (ev.type == SelectionClear) {
                /* nie resetujemy g_selected - tylko przygotuj sie do odpytania */
                next_poll = now_ms(); /* odpytaj zaraz */
                continue;
            }

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
                if (ks == XK_Delete && g_selected >= 0 && g_selected < g_hist_n) {
                    /* usuń zaznaczony wpis */
                    memmove(g_hist[g_selected], g_hist[g_selected + 1],
                            (size_t)(g_hist_n - g_selected - 1) * sizeof(g_hist[0]));
                    g_hist_n--;
                    if (g_selected >= g_hist_n) g_selected = g_hist_n - 1;
                    redraw = 1;
                }
                break;
            }
            case MapNotify:
                XSetInputFocus(g_dpy, g_win, RevertToParent, CurrentTime);
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

        /* Obsluga klikniecia - wykryta przez ui_hit_test w draw().
         * draw() ustawia g_selected i potrzebuje dpy+win do ClaimSelections.
         * Rozwiazanie: draw() nie wywoluje X bezposrednio - zamiast tego
         * zostawia to glownej petli przez flage g_claim_pending. */
        if (g_claim_pending) {
            ClaimSelections(g_dpy, g_win);
            g_claim_pending = 0;
        }

        if (redraw) {
            int old_selected = g_selected;
            ui_begin_frame(ctx);
            draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            if (g_selected != old_selected)
                g_claim_pending = 1;
            redraw = 0;
        }
    }

    ui_destroy(ctx);
    XFreeGC(g_dpy, gc);
    XFreePixmap(g_dpy, icon);
    XCloseDisplay(g_dpy);
    return 0;
}
