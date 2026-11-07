/* 7ajumps - pasek ikon uruchamiajacych 7atodo/7acal/7avol/7asensors/7aexit
 *
 * Nowa apka (nie port Xt/Xaw): wydzielony pasek launchera, ktory wczesniej
 * siedzial pod tarcza w utils/7aclock.c. Jeden poziomy rzad osobnych
 * ui_button (bez wspolnego boxa) wypelniajacy okno (margines windowMargin
 * po bokach, BOX_MARGIN_TB z gory/dolu); kazdy przycisk ma ikone 48 px
 * rysowana na wierzchu, zmniejszana najblizszym sasiadem, gdy przycisk
 * jest mniejszy (ui_draw_bitmap nie skaluje).
 *
 * Klik = fork+execvp komendy z zasobu 7ajumps.<nazwa>Command (klasa
 * 7aJumps.<Nazwa>Command) - bez powloki, tokeny rozdzielone spacjami (jak
 * LaunchEntry w utils/7acenter.c); pusta wartosc ukrywa ikone. Apka
 * zostaje otwarta. Zamkniecie: q/Escape lub WM close.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "../ui.h"
#include "icons/apps_48.xbm"
#include "icons/checklist_48.xbm"
#include "icons/calendar_month_48.xbm"
#include "icons/volume_up_48.xbm"
#include "icons/monitor_heart_48.xbm"
#include "icons/power_settings_new_48.xbm"

#define PROG          "7ajumps"
#define DEF_FONT      "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1"
#define BOX_MARGIN_TB 6  /* odstep od gory/dolu okna, jak w innych apkach */
#define LAUNCH_PAD    3  /* odstep ikony od krawedzi przycisku (z bevelem 1px) */
#define LAUNCH_GAP    4  /* odstep miedzy przyciskami */
#define LAUNCH_ICON   48 /* rozmiar zrodlowych ikon */
#define LAUNCH_ARGS   16 /* maks. tokenow komendy (z NULL) */
#define CMD_MAX       256
/* domyslne okno: 5 ikon w pelnym rozmiarze przy windowMargin 8 */
#define DEF_W (2 * 8 + 5 * (LAUNCH_ICON + 2 * LAUNCH_PAD) + 4 * LAUNCH_GAP)
#define DEF_H (2 * BOX_MARGIN_TB + LAUNCH_ICON + 2 * LAUNCH_PAD)

/* ── launchers ──────────────────────────────────────────────────────── */

enum { L_TODO, L_CAL, L_VOL, L_SENSORS, L_EXIT, L_COUNT };

typedef struct {
    const char *name;   /* zasob "7ajumps.<name>" */
    const char *klass;  /* klasa "7aJumps.<klass>" */
    const char *dflt;   /* domyslna komenda */
    const unsigned char *bits;  /* LAUNCH_ICON x LAUNCH_ICON, XBM */
} LaunchDef;

static const LaunchDef launch_defs[L_COUNT] = {
    [L_TODO]    = { "todoCommand",    "TodoCommand",    "7atodo",    checklist_48_bits },
    [L_CAL]     = { "calCommand",     "CalCommand",     "7acal",     calendar_month_48_bits },
    [L_VOL]     = { "volCommand",     "VolCommand",     "7avol",     volume_up_48_bits },
    [L_SENSORS] = { "sensorsCommand", "SensorsCommand", "7asensors", monitor_heart_48_bits },
    [L_EXIT]    = { "exitCommand",    "ExitCommand",    "7aexit",    power_settings_new_48_bits },
};

/* komendy z zasobow, "" = ikona ukryta */
static char g_cmd[L_COUNT][CMD_MAX];

static void read_launch_cmds(Display *dpy)
{
    char name[64], klass[64];

    for (int i = 0; i < L_COUNT; i++) {
        snprintf(name,  sizeof(name),  PROG ".%s", launch_defs[i].name);
        snprintf(klass, sizeof(klass), "7aJumps.%s", launch_defs[i].klass);
        ui_resource_string(dpy, name, klass, g_cmd[i], sizeof(g_cmd[i]),
                           launch_defs[i].dflt);
    }
}

/* fork+execvp bez powloki; SIGCHLD = SIG_IGN w main, wiec bez zombie */
static void launch(const char *cmd)
{
    char  buf[CMD_MAX];
    char *argv[LAUNCH_ARGS];
    int   argc = 0;

    snprintf(buf, sizeof(buf), "%s", cmd);
    for (char *tok = strtok(buf, " \t"); tok && argc < LAUNCH_ARGS - 1;
         tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    if (argc == 0)
        return;
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        /* nowa sesja - zamkniecie 7ajumps nie zabija odpalonego programu */
        setsid();
        /* SIG_IGN przechodzi przez exec - bez tego waitpid/pclose w
         * odpalonym programie (7avol, 7asensors) dostalyby ECHILD */
        signal(SIGCHLD, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0)
        fprintf(stderr, PROG ": fork failed for '%s'\n", cmd);
}

/* ── drawing ────────────────────────────────────────────────────────── */

/* indeksy launch_defs z niepusta komenda do out; zwraca ich liczbe */
static int visible_launchers(int out[L_COUNT])
{
    int n = 0;
    for (int i = 0; i < L_COUNT; i++)
        if (g_cmd[i][0]) out[n++] = i;
    return n;
}

/* rozmiar ikony dla n przyciskow w pasku r */
static int launch_icon_size(UiRect r, int n)
{
    int s = (r.w - (n - 1) * LAUNCH_GAP) / n - 2 * LAUNCH_PAD;
    if (s > r.h - 2 * LAUNCH_PAD) s = r.h - 2 * LAUNCH_PAD;
    if (s > LAUNCH_ICON) s = LAUNCH_ICON;
    if (s < 8) s = 8;
    return s;
}

/* ikona LAUNCH_ICON px zmniejszona najblizszym sasiadem do size px,
 * wysrodkowana w cell */
static void draw_launch_icon(UiCtx *ui, UiRect cell, const unsigned char *src,
                             int size, const XColor *c)
{
    enum { STRIDE = (LAUNCH_ICON + 7) / 8 };
    unsigned char dst[LAUNCH_ICON * STRIDE];
    int dstride = (size + 7) / 8;

    memset(dst, 0, sizeof(dst));
    for (int y = 0; y < size; y++) {
        int sy = (2 * y + 1) * LAUNCH_ICON / (2 * size);
        for (int x = 0; x < size; x++) {
            int sx = (2 * x + 1) * LAUNCH_ICON / (2 * size);
            if (src[sy * STRIDE + sx / 8] & (1 << (sx % 8)))
                dst[y * dstride + x / 8] |= (unsigned char)(1 << (x % 8));
        }
    }
    ui_draw_bitmap(ui, cell.x + (cell.w - size) / 2,
                   cell.y + (cell.h - size) / 2, size, size, dst, c);
}

static void draw(UiCtx *ui, int w, int h)
{
    int    m = ui_window_margin(ui);
    int    idx[L_COUNT];
    int    n = visible_launchers(idx);
    UiRect r = { m, BOX_MARGIN_TB, w - 2 * m, h - 2 * BOX_MARGIN_TB };

    ui_begin_frame(ui);
    if (n > 0 && r.w > 0 && r.h > 0) {
        int size = launch_icon_size(r, n);

        for (int i = 0; i < n; i++) {
            UiRect cell = ui_rect_col(r, i, n, LAUNCH_GAP);
            int clicked = ui_button(ui, cell, "");

            ui_set_clip(ui, cell);
            draw_launch_icon(ui, cell, launch_defs[idx[i]].bits, size,
                             ui_theme_icon_fg(ui));
            ui_clear_clip(ui);
            if (clicked)
                launch(g_cmd[idx[i]]);
        }
    }
    ui_end_frame(ui);
}

/* ── main ───────────────────────────────────────────────────────────── */

static void usage(void)
{
    fputs(
        "Usage: " PROG " [-geometry WxH+X+Y] [-name NAME] [-title TITLE]\n"
        "  q / Escape         quit\n"
        "Commands: 7ajumps.todoCommand, calCommand, volCommand,\n"
        "sensorsCommand, exitCommand (empty value hides the icon) -\n"
        "see Xresources.sample.\n",
        stderr);
}

int main(int argc, char **argv)
{
    int   win_w = DEF_W, win_h = DEF_H, win_x = 0, win_y = 0;
    int   geo_mask = 0;
    char *name  = PROG;
    char *title = PROG;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-geometry") && i + 1 < argc) {
            unsigned int gw = (unsigned)win_w, gh = (unsigned)win_h;
            geo_mask = XParseGeometry(argv[++i], &win_x, &win_y, &gw, &gh);
            if ((geo_mask & WidthValue)  && gw > 0) win_w = (int)gw;
            if ((geo_mask & HeightValue) && gh > 0) win_h = (int)gh;
        } else if (!strcmp(argv[i], "-name") && i + 1 < argc) {
            name = argv[++i];
        } else if (!strcmp(argv[i], "-title") && i + 1 < argc) {
            title = argv[++i];
        } else {
            usage();
            return strcmp(argv[i], "-help") && strcmp(argv[i], "--help");
        }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs(PROG ": cannot open display\n", stderr); return 1; }

    int    screen = DefaultScreen(dpy);
    Window root   = RootWindow(dpy, screen);

    read_launch_cmds(dpy);

    /* XNegative/YNegative: offset od prawej/dolnej krawedzi ekranu */
    if (geo_mask & XNegative)
        win_x = DisplayWidth(dpy, screen) + win_x - win_w;
    if (geo_mask & YNegative)
        win_y = DisplayHeight(dpy, screen) + win_y - win_h;

    /* tlo None - kolor tla znany dopiero po ui_init, ustawiany nizej */
    XSetWindowAttributes attr = {
        .background_pixmap = None,
        .event_mask        = ExposureMask | StructureNotifyMask
                           | KeyPressMask | ButtonPressMask
                           | ButtonReleaseMask | PointerMotionMask
                           | LeaveWindowMask,
    };
    Window win = XCreateWindow(dpy, root,
        win_x, win_y, (unsigned)win_w, (unsigned)win_h, 0,
        DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
        CWBackPixmap | CWEventMask, &attr);

    /* GC: wlasnosc main, XFreeGC na koncu (ui.c go tylko uzywa) */
    GC gc = XCreateGC(dpy, win, 0, NULL);
    UiCtx *ui = ui_init(dpy, win, gc, DEF_FONT, win_w, win_h);
    if (!ui) {
        fputs(PROG ": ui_init failed (no font?)\n", stderr);
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ui, apps_48_width, apps_48_height, apps_48_bits);
    XSetWindowBackground(dpy, win, ui_theme_bg(ui)->pixel);

    XStoreName(dpy, win, title);
    XSetIconName(dpy, win, title);
    XClassHint ch = { .res_name = name, .res_class = "7aJumps" };
    XSetClassHint(dpy, win, &ch);

    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    XSizeHints sh = { .flags = PMinSize, .min_width = 30, .min_height = 30 };
    if (geo_mask & (XValue | YValue)) {
        sh.flags |= USPosition;
        sh.x = win_x;
        sh.y = win_y;
    }
    if (geo_mask & (WidthValue | HeightValue))
        sh.flags |= USSize;
    XSetWMNormalHints(dpy, win, &sh);

    signal(SIGCHLD, SIG_IGN);   /* launch(): dzieci sprzata jadro */

    XMapWindow(dpy, win);
    draw(ui, win_w, win_h);

    for (int running = 1; running; ) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case LeaveNotify:
            /* ui.c nie zna LeaveNotify - kursor poza oknem gasi hover */
            ev.type = MotionNotify;
            ev.xmotion.x = ev.xmotion.y = -1;
            /* fallthrough */
        case MotionNotify:
        case ButtonPress:
        case ButtonRelease:
            /* rysowanie od razu - kolejny MotionNotify z tej samej paczki
             * przesunalby kursor przed ui_hit_test klikniecia */
            ui_feed_event(ui, &ev);
            draw(ui, win_w, win_h);
            break;
        case Expose:
            /* backbuffer nadal aktualny - samo skopiowanie na okno */
            if (ev.xexpose.count == 0)
                ui_end_frame(ui);
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width  != win_w ||
                ev.xconfigure.height != win_h) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                ui_resize(ui, win_w, win_h);
                draw(ui, win_w, win_h);
            }
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_del)
                running = 0;
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_q || ks == XK_Escape)
                running = 0;
            break;
        }
        }
    }

    ui_destroy(ui);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
