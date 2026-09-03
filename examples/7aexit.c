/*
 * 7aexit.c - systemowy launcher sesji X: 5 przyciskow-ikonek uruchamiajacych
 *   Reboot, Halt, xrdb -merge ~/.Xresources, fvwm Quit, fvwm Restart.
 *
 * Nowa apka (nie port Xt/Xaw). Jeden poziomy rzad prostokatow wypelniajacych
 * okno - przyciski i ikonki skaluja sie z rozmiarem okna: btn_w/btn_h
 * i icon_sz sa przeliczane co klatke z win_w/win_h, a wszelkie wspolrzedne
 * wewnatrz ikonek przez makro SC() (proporcja wzgledem ICON_BASE=42).
 * Klikniecie odpala komende przez fork + execl("/bin/sh","sh","-c",...) -
 * fire-and-forget; apka pozostaje otwarta. Zamkniecie: Escape lub WM close.
 *
 * Reboot/Halt: na OpenBSD czlonkowie grupy operator moga wolac /sbin/reboot
 * i /sbin/halt bezposrednio. Na Linuksie dopisz "doas "/"sudo " do g_cmds[0]/
 * g_cmds[1] jesli potrzeba.
 * fvwm Quit/Restart: wymaga zaladowanego "Module FvwmCommandS" w ~/.fvwm/config.
 *
 * Brak pledge/unveil na OpenBSD - patrz komentarz przy run_cmd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"

/* ------------------------------------------------------------------
 * Stale
 * ------------------------------------------------------------------ */

#define N_BTNS    4
#define BTN_GAP   3      /* staly odstep poziomy miedzy przyciskami (px) */
#define BORDER_W  1
#define ROW_H     20     /* jednolita wysokosc przycisku - patrz 7amessage.c/7atimer.c/7afm.c */
#define WM_ICON_SZ 32    /* rozmiar 1-bit pixmapy dla WMHints */

/* ICON_BASE: bazowy rozmiar ikonki do makra skalujacego SC().
 * Wszystkie wspolrzedne w funkcjach icon_* zapisane sa dla ICON_BASE px
 * i skalowane do aktualnego icon_sz przez SC(n, sz). */
#define ICON_BASE 42

/* SC(n, sz): liniowe skalowanie wartosci n z podstawy ICON_BASE do sz */
#define SC(n, sz) ((n) * (sz) / ICON_BASE)

/* ------------------------------------------------------------------
 * Dane przyciskow
 * ------------------------------------------------------------------ */

static const char *g_labels[N_BTNS] = {
    "Reboot", "Halt", "xrdb -merge ~/.Xresources", "fvwm Quit"
};

static const char *g_cmds[N_BTNS] = {
    "7amessage -confirm 'Reboot system?' && /sbin/reboot",
    "7amessage -confirm 'Halt system?' && /sbin/halt",
    "xrdb -merge ~/.Xresources",
    "7amessage -confirm 'Quit fvwm3?' && pkill fvwm3"
};

/* ------------------------------------------------------------------
 * Wykonanie komendy
 *
 * Brak pledge/unveil: exec'd procesy dziedzicza unveil po forku, co
 * zablokowaloby /bin/sh i wszystkie komendy systemowe. Apka i tak
 * jest przyciskiem "root-level", wiec szerokie uprawnienia sa oczekiwane.
 * ------------------------------------------------------------------ */

static void
run_cmd(int idx)
{
    pid_t pid;

    if (idx < 0 || idx >= N_BTNS)
        return;
    pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", g_cmds[idx], (char *)NULL);
        _exit(1);
    }
    /* rodzic nie czeka - zombie sprz,tane przez SIG_IGN ustawiony w main */
}

/* ------------------------------------------------------------------
 * Ikonki - wspolny styl: kazda ikonka = zewnetrzny pierscien (plan kola)
 * + odrozniajacy symbol wewnatrz. Skalowanie przez SC() wzgledem
 * sz = min(r.w, r.h). Promien pierscienia = SC(17, sz) dla wszystkich.
 * ------------------------------------------------------------------ */

/* pomocnik: rysuje wspolny pierscien i zwraca jego promien i grubosc */
static void
ring(UiCtx *ctx, int cx, int cy, int sz, const XColor *fg,
     int *rad_out, int *thk_out)
{
    int rad = SC(17, sz);
    int thk = 1;

    if (rad < 2) { *rad_out = 0; *thk_out = thk; return; }
    ui_draw_circle(ctx, cx, cy, rad, thk, fg);
    *rad_out = rad;
    *thk_out = thk;
}

/* Ikonka 0: Reboot - pierscien + trojkat wskazujacy w gore (restart systemu) */
static void
icon_reboot(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int sz = r.w < r.h ? r.w : r.h;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int rad, thk, aw, ah;

    ring(ctx, cx, cy, sz, fg, &rad, &thk);
    if (!rad) return;
    aw = SC(8, sz);
    ah = SC(10, sz);
    if (aw >= 1 && ah >= 1)
        ui_fill_triangle(ctx,
            cx,       cy - ah / 2,
            cx - aw,  cy + ah / 2,
            cx + aw,  cy + ah / 2,
            fg);
}

/* Ikonka 1: Halt - pierscien + wypelniony kwadrat w srodku (stop) */
static void
icon_halt(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int sz = r.w < r.h ? r.w : r.h;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int rad, thk, sq;

    ring(ctx, cx, cy, sz, fg, &rad, &thk);
    if (!rad) return;
    sq = SC(7, sz);
    if (sq >= 1)
        ui_fill_rect(ctx, (UiRect){cx - sq, cy - sq, 2 * sq, 2 * sq}, fg);
}

/* Ikonka 2: xrdb - pierscien + trzy poziome paski (zasoby/konfiguracja) */
static void
icon_xrdb(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int sz = r.w < r.h ? r.w : r.h;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int rad, thk;
    int bw, bh, gap;

    ring(ctx, cx, cy, sz, fg, &rad, &thk);
    if (!rad) return;
    bw  = SC(9, sz);   /* polowa szerokosci paska */
    bh  = thk;         /* ta sama grubosc co pierscien */
    gap = SC(4, sz);
    if (bw < 1) return;
    ui_fill_rect(ctx, (UiRect){cx - bw, cy - gap - bh, 2 * bw, bh}, fg);
    ui_fill_rect(ctx, (UiRect){cx - bw, cy - bh / 2,   2 * bw, bh}, fg);
    ui_fill_rect(ctx, (UiRect){cx - bw, cy + gap,       2 * bw, bh}, fg);
}

/* Ikonka 3: fvwm Quit - pierscien + znak X w srodku (zamkniecie) */
static void
icon_quit(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int sz = r.w < r.h ? r.w : r.h;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int rad, thk, off;

    ring(ctx, cx, cy, sz, fg, &rad, &thk);
    if (!rad) return;
    off = SC(8, sz);
    if (off < 1) return;
    ui_draw_line(ctx, cx - off, cy - off, cx + off, cy + off, thk, fg);
    ui_draw_line(ctx, cx + off, cy - off, cx - off, cy + off, thk, fg);
}

typedef void (*IconFn)(UiCtx *, UiRect, const XColor *);

static IconFn g_icons[N_BTNS] = {
    icon_reboot, icon_halt, icon_xrdb, icon_quit
};

/* ------------------------------------------------------------------
 * Rysowanie klatki.
 *
 * Jeden UiBox wypelniajacy dostepna wysokosc okna; N_BTNS wierszy przez
 * ui_box_next_rect, kazdy wiersz = ikona (kwadrat) po lewej + etykieta
 * po prawej. Hover podswietla caly wiersz (accent). Pionowy separator
 * oddziela kolumne ikonki od etykiety; poziomy separator miedzy wierszami
 * rysowany recznie (gap=0 w stylu, bo potrzebujemy separatora, nie przerwy).
 * Ostatni wiersz dostaje resztke pikseli z dzielenia calkowitego, zeby box
 * dokladnie wypelnil okno.
 * ------------------------------------------------------------------ */

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int        style_init = 0;
    int margin, lh, bw;
    int lbl_w, lbl_row_h;
    int ico_y, ico_w, ico_row_h;
    int pad, icon_sz, cell_gap;
    int mx, my, hovered_idx, i;
    UiBox *box;
    UiRect cells[N_BTNS];

    margin = ui_window_margin(ctx);
    lh     = ui_line_height(ctx);
    bw     = 1;

    if (!style_init) {
        style              = (UiBoxStyle){0};
        style.border_w     = bw;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color     = *ui_theme_box_bg(ctx);
        style_init = 1;
    }

    lbl_w     = win_w - 2 * margin;
    lbl_row_h = lh + 4;
    /* ico_y: margin + border + lbl_row_h + border + 4px przerwy */
    ico_y     = margin + bw + lbl_row_h + bw + 4;
    ico_w     = lbl_w;
    ico_row_h = win_h - ico_y - margin;
    if (ico_row_h < 4) ico_row_h = 4;

    /* --- Pre-obliczenie pol ikonek (do hover detection przed rysowaniem) --- */
    cell_gap = 2;
    {
        int row_x = margin + bw;
        int row_w = ico_w - 2 * bw;
        int cw    = (row_w - (N_BTNS - 1) * cell_gap) / N_BTNS;
        if (cw < 1) cw = 1;
        for (i = 0; i < N_BTNS; i++) {
            cells[i].x = row_x + i * (cw + cell_gap);
            cells[i].y = ico_y + bw;
            cells[i].w = (i == N_BTNS - 1)
                         ? row_w - i * (cw + cell_gap)
                         : cw;
            cells[i].h = ico_row_h;
        }
    }

    /* --- Hover detection --- */
    ui_mouse_state(ctx, &mx, &my, NULL);
    hovered_idx = -1;
    for (i = 0; i < N_BTNS; i++) {
        if (mx >= cells[i].x && mx < cells[i].x + cells[i].w &&
            my >= cells[i].y && my < cells[i].y + cells[i].h) {
            hovered_idx = i;
            break;
        }
    }

    /* --- Label box: pokazuje etykiete najechanej ikonki --- */
    box = ui_box_begin(ctx, "lbl", margin, margin, lbl_w, &style);
    {
        UiRect lr = ui_box_next_rect(box, lbl_row_h);
        lr.y += (lbl_row_h - lh) / 2;
        lr.h  = lh;
        ui_label_centered(ctx, lr,
                          hovered_idx >= 0 ? g_labels[hovered_idx] : "Leave 7adesktop");
    }
    ui_box_end(box);

    /* --- Icon box: 4 ikonki poziomo w jednym wierszu --- */
    pad     = 3;
    icon_sz = cells[0].w - 2 * pad;
    {
        int max_h = ico_row_h - 2 * pad;
        if (max_h < icon_sz) icon_sz = max_h;
    }
    if (icon_sz < 2) icon_sz = 2;

    box = ui_box_begin(ctx, "icons", margin, ico_y, ico_w, &style);
    {
        UiRect row = ui_box_next_rect(box, ico_row_h);

        for (i = 0; i < N_BTNS; i++) {
            UiRect ic = ui_rect_col(row, i, N_BTNS, cell_gap);
            UiRect ir = {
                ic.x + (ic.w - icon_sz) / 2,
                ic.y + (ic.h - icon_sz) / 2,
                icon_sz, icon_sz
            };

            if (i == hovered_idx)
                ui_fill_rect(ctx, ic, ui_theme_accent(ctx));

            if (i > 0)
                ui_draw_line(ctx,
                    ic.x, ic.y,
                    ic.x, ic.y + ico_row_h,
                    1, ui_theme_line_fg(ctx));

            g_icons[i](ctx, ir, ui_theme_icon_fg(ctx));

            if (ui_hit_test(ctx, ic))
                run_cmd(i);
        }
    }
    ui_box_end(box);
}

/* ------------------------------------------------------------------
 * Ikonka WMHints (1-bit pixmapa, symbol przycisku zasilania)
 * ------------------------------------------------------------------ */

static Pixmap
make_wm_icon(Display *dpy, Window root)
{
    Pixmap p  = XCreatePixmap(dpy, root, WM_ICON_SZ, WM_ICON_SZ, 1);
    GC     gc = XCreateGC(dpy, p, 0, NULL);

    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, p, gc, 0, 0, WM_ICON_SZ, WM_ICON_SZ);
    XSetForeground(dpy, gc, 1);
    XDrawArc(dpy, p, gc, 5, 8, 22, 22, 40 * 64, 280 * 64);
    XDrawLine(dpy, p, gc, 16, 2, 16, 15);
    XFreeGC(dpy, gc);
    return p;
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    Display    *dpy;
    int         screen;
    Window      root, win;
    GC          gc;
    UiCtx      *ctx;
    Pixmap      icon;
    XWMHints   *wmhints;
    XSizeHints *szh;
    Atom        wm_del;
    int         win_w, win_h, win_x, win_y;
    int         geo_mask;
    int         running, redraw;
    XEvent      ev;

    win_w = 240;
    win_h = 120; /* label box ~24px + 4px przerwy + ikony ~80px + marginesy */
    win_x = 0;
    win_y = 0;
    geo_mask = 0;

    /* opcjonalny argument: -geometry WxH+X+Y (standardowa flaga X11) */
    if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'g') {
        int gx = 0, gy = 0;
        unsigned int gw = (unsigned)win_w, gh = (unsigned)win_h;
        geo_mask = XParseGeometry(argv[2], &gx, &gy, &gw, &gh);
        if (geo_mask & WidthValue)  win_w = (int)gw;
        if (geo_mask & HeightValue) win_h = (int)gh;
        if (geo_mask & XValue)      win_x = gx;
        if (geo_mask & YValue)      win_y = gy;
    }

    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7aexit: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);

    /* jesli geometria nie podana: wycentruj na ekranie */
    if (!(geo_mask & XValue))
        win_x = (DisplayWidth(dpy, screen)  - win_w) / 2;
    if (!(geo_mask & YValue))
        win_y = (DisplayHeight(dpy, screen) - win_h) / 2;
    /* XNegative/YNegative: offset od prawej/dolnej krawedzi ekranu */
    if (geo_mask & XNegative)
        win_x = DisplayWidth(dpy, screen)  - win_w - win_x;
    if (geo_mask & YNegative)
        win_y = DisplayHeight(dpy, screen) - win_h - win_y;

    win = XCreateSimpleWindow(dpy, root,
          win_x, win_y,
          (unsigned)win_w, (unsigned)win_h, 0,
          BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XSelectInput(dpy, win,
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, "7aExit");
    XSetIconName(dpy, win, "7aExit");

    wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    icon    = make_wm_icon(dpy, root);
    wmhints = XAllocWMHints();
    if (wmhints) {
        wmhints->flags        = IconPixmapHint | IconMaskHint;
        wmhints->icon_pixmap  = icon;
        wmhints->icon_mask    = icon;
        XSetWMHints(dpy, win, wmhints);
        XFree(wmhints);
    }

    szh = XAllocSizeHints();
    if (szh) {
        szh->flags      = PMinSize;
        szh->min_width  = 60;
        szh->min_height = N_BTNS * 10;
        if (geo_mask & (XValue | YValue)) {
            szh->flags |= USPosition;
            szh->x = win_x;
            szh->y = win_y;
        }
        if (geo_mask & (WidthValue | HeightValue))
            szh->flags |= USSize;
        XSetWMNormalHints(dpy, win, szh);
        XFree(szh);
    }

    XMapWindow(dpy, win);

    gc  = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7aexit: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    running = 1;
    redraw  = 1;

    while (running) {
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
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape)
                running = 0;
            redraw = 1;
            break;
        }
        case MapNotify:
            XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
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
