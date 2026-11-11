/*
 * 7aexit.c - systemowy launcher sesji X: 3 przyciski-ikonki uruchamiajace
 *   Reboot, Halt, fvwm Quit.
 *
 * Nowa apka (nie port Xt/Xaw). Jeden poziomy rzad osobnych ui_button (bez
 * wspolnego boxa) wypelniajacych cale okno - bez naglowka z etykieta:
 * nazwa najechanej ikonki (g_labels) trafia do TYTULU okna (XStoreName,
 * patrz update_title), poza ikonkami tytul wraca do -title/nazwy apki.
 * Przyciski skaluja
 * sie z rozmiarem okna (przeliczane co klatke z win_w/win_h), ikonki maja
 * staly rozmiar i sa wysrodkowane w przycisku (przyciete do niego, gdy
 * okno jest bardzo male).
 *
 * Ikony to Material Symbols Google (Outlined, wght 400, licencja Apache
 * 2.0): restart_alt, power_settings_new, logout - 1-bitowe XBM w
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
 * OpenBSD ma "pkill" w bazowym systemie: "pkill -x 'fvwm3?'" - wzorzec to
 * regex dopasowany do CALEJ nazwy procesu (-x), wiec trafia i w fvwm (2.x
 * z bazowego X11), i w fvwm3, ale nie w np. FvwmPager; Linux (psmisc) uzywa
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
#include "icons/logout.xbm"

/* ------------------------------------------------------------------
 * Stale
 * ------------------------------------------------------------------ */

#define N_BTNS    3
#define BTN_GAP   3      /* staly odstep poziomy miedzy przyciskami (px) */
#define BORDER_W  1
#define V_MARGIN  6      /* odstep pionowy przyciskow od krawedzi okna - jak 7acal.c */

/* ------------------------------------------------------------------
 * Dane przyciskow
 * ------------------------------------------------------------------ */

static const char *g_labels[N_BTNS] = {
    "Reboot", "Halt", "Quit WM"
};

#ifdef __OpenBSD__
#define FVWM_QUIT_CMD "7amessage -confirm 'Quit window manager?' && pkill -x 'fvwm3?'"
#else
#define FVWM_QUIT_CMD "7amessage -confirm 'Quit window manager?' && killall -q fvwm fvwm3"
#endif

static const char *g_cmds[N_BTNS] = {
    "7amessage -confirm 'Reboot system?' && shutdown -r now",
    "7amessage -confirm 'Halt system?' && shutdown -h now",
    FVWM_QUIT_CMD
};

/* ------------------------------------------------------------------
 * Wykonanie komendy
 *
 * Brak pledge/unveil: exec'd procesy dziedzicza unveil po forku, co
 * zablokowaloby /bin/sh i wszystkie komendy systemowe. Apka i tak
 * jest przyciskiem "root-level", wiec szerokie uprawnienia sa oczekiwane.
 * ------------------------------------------------------------------ */

/* Okno i tytul bazowy (-title albo nazwa apki) dla update_title - ustawiane
 * w main() przed pierwsza klatka. g_pointer_inside: ui.c nie obsluguje
 * LeaveNotify, wiec bez tego ostatnia pozycja myszy "zostawalaby" nad
 * ikonka po wyjechaniu kursora z okna (ten sam wzorzec co w 7acal.c). */
static Display    *g_dpy;
static Window      g_win;
static const char *g_base_title;
static int         g_pointer_inside = 0;

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
    ICON(restart_alt), ICON(power_settings_new), ICON(logout)
};

/* ------------------------------------------------------------------
 * Rysowanie klatki.
 *
 * Rzad N_BTNS przyciskow ui_button (bez boxa) z ikonka rysowana na
 * wierzchu, wypelniajacy cale okno - jak launcher w utils/7aclock.c.
 * Etykieta najechanego przycisku idzie do tytulu okna (update_title).
 * ------------------------------------------------------------------ */

/* Tytul okna = etykieta najechanej ikonki (idx >= 0) albo tytul bazowy.
 * XStoreName tylko przy ZMIANIE idx - draw() jest wolane na kazdy
 * MotionNotify, a kazda zmiana WM_NAME to round-trip do WM. */
static void
update_title(int idx)
{
    static int shown = -2; /* -2: jeszcze nic nie ustawione */

    if (idx == shown)
        return;
    shown = idx;
    XStoreName(g_dpy, g_win, idx >= 0 ? g_labels[idx] : g_base_title);
}

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    int margin;
    int mx, my, hovered_idx, i;
    UiRect row;

    margin = ui_window_margin(ctx);

    /* Poziomo: ui_window_margin jak w pozostalych apkach; pionowo: V_MARGIN
     * nad i pod przyciskami (7acal.c: margin_t/b=6). */
    row.x = margin;
    row.y = V_MARGIN;
    row.w = win_w - 2 * margin;
    row.h = win_h - 2 * V_MARGIN;
    if (row.h < 4) row.h = 4;

    ui_mouse_state(ctx, &mx, &my, NULL);
    hovered_idx = -1;
    for (i = 0; g_pointer_inside && i < N_BTNS; i++) {
        UiRect c = ui_rect_col(row, i, N_BTNS, BTN_GAP);
        if (mx >= c.x && mx < c.x + c.w && my >= c.y && my < c.y + c.h) {
            hovered_idx = i;
            break;
        }
    }
    update_title(hovered_idx);

    /* --- Przyciski: 3 ikonki poziomo, bez boxa --- */
    for (i = 0; i < N_BTNS; i++) {
        UiRect ic = ui_rect_col(row, i, N_BTNS, BTN_GAP);
        const Icon *ico = &g_icons[i];
        int clicked = ui_button(ctx, ic, "");

        ui_set_clip(ctx, ic);
        ui_draw_bitmap(ctx, ic.x + (ic.w - ico->w) / 2, ic.y + (ic.h - ico->h) / 2,
                       ico->w, ico->h, ico->bits, ui_theme_icon_fg(ctx));
        ui_clear_clip(ctx);

        if (clicked)
            run_cmd(i);
    }
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

    win_w = 180;
    win_h = 84; /* 6 + ikony ~72px + 6 */
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
    /* XNegative/YNegative: offset od prawej/dolnej krawedzi ekranu -
     * XParseGeometry zwraca go juz ujemny ("-10" -> -10), stad "+" */
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
        PointerMotionMask | StructureNotifyMask | KeyPressMask |
        EnterWindowMask | LeaveWindowMask);
    g_dpy = dpy;
    g_win = win;
    g_base_title = app_title[0] ? app_title : app_name;
    XStoreName(dpy, win, g_base_title);
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
            /* ICCCM win_gravity: bez tego WM przyjmuje NorthWest i dokleja
             * ramke na lewo/w gore od pozycji klienta, wiec przy -0-0
             * ramka wystaje poza ekran; z SouthEast WM sam kladzie rog
             * RAMKI w rogu ekranu */
            szh->flags |= PWinGravity;
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

    gc  = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7aexit: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, power_settings_new_48_width, power_settings_new_48_height, power_settings_new_48_bits);

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
        case EnterNotify:
        case LeaveNotify:
            g_pointer_inside = (ev.type == EnterNotify);
            redraw = 1;
            break;
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
