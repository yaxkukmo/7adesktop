/*
 * 7aexit.c - systemowy launcher sesji X: 4 przyciski-ikonki uruchamiajace
 *   Reboot, Halt, xrdb -merge ~/.Xresources, fvwm Quit.
 *
 * Nowa apka (nie port Xt/Xaw). Jeden poziomy rzad prostokatow wypelniajacych
 * okno - komorki przyciskow skaluja sie z rozmiarem okna (przeliczane co
 * klatke z win_w/win_h), ikonki maja staly rozmiar i sa wysrodkowane w
 * komorce (przyciete do niej, gdy okno jest bardzo male).
 *
 * Ikony to Material Symbols Google (Outlined, wght 400, licencja Apache
 * 2.0): restart_alt, power_settings_new, refresh, logout - 1-bitowe XBM w
 * utils/icons/, ten sam przepis konwersji co w naglowku utils/7atube.c
 * (24px SVG -> rsvg-convert, prog alfa 50%, przyciecie do obrysu; odbicie
 * lewej polowy tylko dla symetrycznej power_settings_new), rysowane
 * ui_draw_bitmap. power_settings_new sluzy tez za ikonke WMHints.
 * Klikniecie odpala komende przez fork + execl("/bin/sh","sh","-c",...) -
 * fire-and-forget; apka pozostaje otwarta. Zamkniecie: Escape lub WM close.
 *
 * Reboot/Halt: zwykle "shutdown -r now" i "shutdown -h now" z PATH, bez sudo/doas -
 * uprawnienia daje przynaleznosc uzytkownika do odpowiednich grup systemowych
 * (np. operator na OpenBSD), nie apka.
 * fvwm Quit (FVWM_QUIT_CMD): zabija proces WM sygnalem (nie przez FvwmCommand).
 * OpenBSD ma "pkill" w bazowym systemie (pkill fvwm3); Linux (psmisc) uzywa
 * "killall -q fvwm fvwm3" - obie nazwy naraz, zeby dzialalo i z fvwm 2.x,
 * i z fvwm3; -q wycisza blad dla nazwy, ktora akurat nie dziala.
 * #ifdef __OpenBSD__ wybiera wlasciwa komende w czasie kompilacji.
 *
 * Brak pledge/unveil na OpenBSD - patrz komentarz przy run_cmd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/power_settings_new_48.xbm"
#include "icons/restart_alt.xbm"
#include "icons/power_settings_new.xbm"
#include "icons/refresh.xbm"
#include "icons/logout.xbm"

/* ------------------------------------------------------------------
 * Stale
 * ------------------------------------------------------------------ */

#define N_BTNS    4
#define BTN_GAP   3      /* staly odstep poziomy miedzy przyciskami (px) */
#define BORDER_W  1
#define ROW_H     20     /* jednolita wysokosc przycisku - patrz 7amessage.c/7atimer.c/7afm.c */
#define HDR_PAD   4      /* padding pionowy naglowka - jak header w 7acal.c (30px razem) */
#define V_MARGIN  6      /* odstep pionowy boxow od krawedzi okna i miedzy soba - jak 7acal.c */

/* ------------------------------------------------------------------
 * Dane przyciskow
 * ------------------------------------------------------------------ */

static const char *g_labels[N_BTNS] = {
    "Reboot", "Halt", "Reload Xresources", "Quit WM"
};

#ifdef __OpenBSD__
#define FVWM_QUIT_CMD "7amessage -confirm 'Quit window manager?' && pkill fvwm3"
#else
#define FVWM_QUIT_CMD "7amessage -confirm 'Quit window manager?' && killall -q fvwm fvwm3"
#endif

static const char *g_cmds[N_BTNS] = {
    "7amessage -confirm 'Reboot system?' && shutdown -r now",
    "7amessage -confirm 'Halt system?' && shutdown -h now",
    "xrdb -merge ~/.Xresources",
    FVWM_QUIT_CMD
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
 * Ikonki (XBM, kolejnosc jak g_labels/g_cmds)
 * ------------------------------------------------------------------ */

typedef struct {
    int                  w, h;
    const unsigned char *bits;
} Icon;

#define ICON(name) { name##_width, name##_height, name##_bits }

static const Icon g_icons[N_BTNS] = {
    ICON(restart_alt), ICON(power_settings_new), ICON(refresh), ICON(logout)
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
    static UiBoxStyle style, hdr_style;
    static int        style_init = 0;
    int margin, bw;
    int lbl_w, lbl_h;
    int ico_y, ico_w, ico_row_h;
    int cell_gap;
    int mx, my, hovered_idx, i;
    UiBox *box;
    UiRect cells[N_BTNS];

    margin = ui_window_margin(ctx);
    bw     = 1;

    if (!style_init) {
        style              = (UiBoxStyle){0};
        style.border_w     = bw;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color     = *ui_theme_box_bg(ctx);
        style.padding_l    = style.padding_r = ui_box_padding(ctx);
        /* naglowek: ten sam padding pionowy co header w 7acal.c */
        hdr_style           = style;
        hdr_style.padding_t = hdr_style.padding_b = HDR_PAD;
        style_init = 1;
    }

    /* Poziomo: ui_window_margin jak w pozostalych apkach; pionowo: V_MARGIN
     * nad naglowkiem, miedzy boxami i pod boxem ikonek (7acal.c: margin_t/b=6). */
    lbl_w     = win_w - 2 * margin;
    lbl_h     = 2 * bw + 2 * HDR_PAD + ROW_H;
    ico_y     = V_MARGIN + lbl_h + V_MARGIN;
    ico_w     = lbl_w;
    ico_row_h = win_h - ico_y - V_MARGIN - 2 * bw;
    if (ico_row_h < 4) ico_row_h = 4;

    /* --- Pre-obliczenie pol ikonek (do hover detection przed rysowaniem) --- */
    cell_gap = 2;
    {
        int row_x = margin + bw + style.padding_l;
        int row_w = ico_w - 2 * bw - style.padding_l - style.padding_r;
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
    box = ui_box_begin(ctx, "lbl", margin, V_MARGIN, lbl_w, &hdr_style);
    {
        UiRect lr = ui_box_next_rect(box, ROW_H);
        ui_label_centered(ctx, lr,
                          hovered_idx >= 0 ? g_labels[hovered_idx] : "Leave 7adesktop");
    }
    ui_box_end(box);

    /* --- Icon box: 4 ikonki poziomo w jednym wierszu --- */
    box = ui_box_begin(ctx, "icons", margin, ico_y, ico_w, &style);
    {
        UiRect row = ui_box_next_rect(box, ico_row_h);

        for (i = 0; i < N_BTNS; i++) {
            UiRect ic = ui_rect_col(row, i, N_BTNS, cell_gap);
            const Icon *ico = &g_icons[i];

            if (i == hovered_idx)
                ui_fill_round_rect(ctx, ic, ui_theme_accent(ctx));

            if (i > 0)
                ui_draw_line(ctx,
                    ic.x, ic.y,
                    ic.x, ic.y + ico_row_h,
                    1, ui_theme_line_fg(ctx));

            ui_set_clip(ctx, ic);
            ui_draw_bitmap(ctx, ic.x + (ic.w - ico->w) / 2, ic.y + (ic.h - ico->h) / 2,
                           ico->w, ico->h, ico->bits, ui_theme_icon_fg(ctx));
            ui_clear_clip(ctx);

            if (ui_hit_test(ctx, ic))
                run_cmd(i);
        }
    }
    ui_box_end(box);
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
    XSizeHints *szh;
    Atom        wm_del;
    int         win_w, win_h, win_x, win_y;
    int         geo_mask;
    int         running, redraw;
    int         i;
    char        app_name[64] = "7aExit";
    char        app_title[64] = "";
    XEvent      ev;

    win_w = 240;
    win_h = 120; /* 6 + naglowek 30px + 6 + ikony ~72px + 6 */
    win_x = 0;
    win_y = 0;
    geo_mask = 0;

    /* opcjonalne argumenty: -geometry WxH+X+Y (standardowa flaga X11), -title TYTUL */
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
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7aExit";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

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
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, power_settings_new_48_width, power_settings_new_48_height, power_settings_new_48_bits);

    /* Dwie klatki PRZED petla glowna: pierwsza zapelnia cache wysokosci
     * boxa (immediate-mode nie zna wysokosci przed narysowaniem zawartosci
     * - patrz architektura w CLAUDE.md), druga uzywa juz poprawnej
     * wysokosci i rysuje tlo/border. Bez tego okno pokazywaloby box bez
     * tla/bordera az do pierwszego X eventu, bo petla nizej blokuje sie na
     * XNextEvent i nie rysuje kolejnej klatki sama z siebie. Ten sam
     * wzorzec co w utils/7aping.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

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
    XCloseDisplay(dpy);
    return 0;
}
