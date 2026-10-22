/*
 * 7avol.c - regulacja glosnosci (suwak + mute), odpowiednik xmixer.
 *
 * Nowa apka (nie port Xt/Xaw). Jeden box: pasek glosnosci (ui_meter,
 * klik/przeciaganie ustawia poziom) i rzad przyciskow "-"/"Mute"/"+".
 * Kolko myszy w dowolnym miejscu okna i klawisze Up/Down/Left/Right/+/-
 * zmieniaja glosnosc o STEP_PCT, "m" przelacza mute, Escape/q zamyka.
 * Suwak narysowany w samej apce (ui_meter + ui_mouse_state, wzorem kciuka
 * scrollbara w 7acenter.c) - bez nowego widgetu w ui.h.
 *
 * Apka sama nie rozmawia z dzwiekiem (bez libsndio/libasound - zero
 * nowych zaleznosci linkowania), tylko woła mikser z systemu bazowego:
 *   OpenBSD: sndioctl(1) - "output.level" (0..1) i "output.mute" (0/1)
 *   Linux:   amixer(1)   - kontrolka "Master" ("[54%]" i "[on]/[off]");
 *            z PulseAudio/PipeWire domyslne urzadzenie ALSA to i tak
 *            ich wtyczka, wiec amixer dziala tez tam
 * Nazwa kontrolki z zasobu X 7aVol.control (domyslnie "output" na
 * OpenBSD, "Master" na Linuksie). Komendy odpalane fork+execvp BEZ
 * powloki (RunCapture), wiec nazwa kontrolki nie trafia do sh -c.
 *
 * Zmiany z zewnatrz (klawisze multimedialne, inny mikser) - polling
 * co POLL_MS (jeden fork miksera na 2 s, pomijalny koszt), wzorem
 * timera przez select() na fd polaczenia X z 7aweather.c. W trakcie
 * przeciagania suwaka polling wstrzymany, a ustawianie poziomu
 * ograniczone do raz na DRAG_APPLY_MS (plus ostatnia wartosc przy
 * puszczeniu przycisku) - inaczej kazdy MotionNotify to osobny fork.
 *
 * Uzycie:
 *   7avol [-geometry GEOM] [-name NAME] [-title TEXT]
 */

#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"

#define STEP_PCT        5
#define POLL_MS         2000
#define DRAG_APPLY_MS   80
#define ROW_H           20
#define V_MARGIN        6
#define BOX_PAD_V       4
#define BOX_GAP         6
#define WIN_W           220

#ifdef __OpenBSD__
# define DEFAULT_CONTROL "output"
#else
# define DEFAULT_CONTROL "Master"
#endif

/* ------------------------------------------------------------------ */
/* Stan                                                                */
/* ------------------------------------------------------------------ */

static char g_control[64];
static int  g_ok;          /* 0 = ostatni odczyt miksera sie nie udal */
static int  g_level;       /* 0..100, to co pokazuje pasek */
static int  g_muted;
static int  g_applied;     /* ostatni poziom faktycznie wyslany do miksera */
static long g_last_apply_ms;

/* Sesja przeciagania suwaka - zaczyna ja ButtonPress (Button1) nad
 * g_meter_r w main(), konczy draw(), gdy LPM juz nie jest wcisniety.
 * g_meter_r to pasek z OSTATNIEJ narysowanej klatki (jak g_*_list_r w
 * 7askm.c). */
static int    g_drag;
static UiRect g_meter_r;

/* Kolor paska przy wyciszeniu; wlascicielem jest colormap displaya -
 * zwolnienie przez XCloseDisplay(), jak kolory statusu w 7aping.c. */
static XColor g_col_muted;

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                          */
/* ------------------------------------------------------------------ */

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + (long)tv.tv_usec / 1000;
}

static int
clamp_pct(int v)
{
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

static int
in_rect(UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* fork+execvp argv bez powloki; stdout dziecka do out (pojemnosc outsz z
 * NUL, nadmiar czytany i odrzucany, zeby dziecko nie zawislo na pelnym
 * potoku), stderr do /dev/null. out moze byc NULL (tylko kod wyjscia).
 * Zwraca 0 przy kodzie wyjscia 0, -1 w kazdym innym przypadku (w tym
 * brak programu w PATH - dziecko konczy sie wtedy 127). */
static int
RunCapture(char *const argv[], char *out, size_t outsz)
{
    int fds[2], status, devnull;
    size_t len = 0;
    ssize_t n;
    pid_t pid;
    char drain[256];

    if (out && outsz > 0)
        out[0] = '\0';
    if (pipe(fds) == -1)
        return -1;
    pid = fork();
    if (pid == -1) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1)
            dup2(devnull, STDERR_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    for (;;) {
        char *dst = drain;
        size_t room = sizeof(drain);

        if (out && len + 1 < outsz) {
            dst = out + len;
            room = outsz - 1 - len;
        }
        n = read(fds[0], dst, room);
        if (n <= 0)
            break;
        if (dst != drain)
            len += (size_t)n;
    }
    if (out && outsz > 0)
        out[len] = '\0';
    close(fds[0]);
    if (waitpid(pid, &status, 0) == -1)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Mikser (bez ui_*)                                                   */
/* ------------------------------------------------------------------ */

#ifdef __OpenBSD__

/* "sndioctl output.level output.mute" wypisuje "output.level=0.502\n
 * output.mute=0\n" - jeden fork na oba odczyty. */
static int
ReadMixer(int *level, int *muted)
{
    char lvl_arg[80], mute_arg[80], buf[256], *p;
    char *argv[] = { "sndioctl", lvl_arg, mute_arg, NULL };
    int have_level = 0;

    snprintf(lvl_arg, sizeof(lvl_arg), "%s.level", g_control);
    snprintf(mute_arg, sizeof(mute_arg), "%s.mute", g_control);
    if (RunCapture(argv, buf, sizeof(buf)) == -1)
        return -1;
    *muted = 0;
    for (p = strtok(buf, "\n"); p; p = strtok(NULL, "\n")) {
        char *eq = strchr(p, '=');

        if (!eq)
            continue;
        *eq = '\0';
        if (strcmp(p, lvl_arg) == 0) {
            *level = clamp_pct((int)(strtod(eq + 1, NULL) * 100.0 + 0.5));
            have_level = 1;
        } else if (strcmp(p, mute_arg) == 0) {
            *muted = atoi(eq + 1) != 0;
        }
    }
    return have_level ? 0 : -1;
}

static int
SetLevel(int pct)
{
    char arg[96];
    char *argv[] = { "sndioctl", "-q", arg, NULL };

    snprintf(arg, sizeof(arg), "%s.level=%.3f", g_control, pct / 100.0);
    return RunCapture(argv, NULL, 0);
}

static int
ToggleMute(void)
{
    char arg[96];
    char *argv[] = { "sndioctl", "-q", arg, NULL };

    snprintf(arg, sizeof(arg), "%s.mute=!", g_control);
    return RunCapture(argv, NULL, 0);
}

#else

/* "amixer get Master" - pierwszy "[NN%]" w wyjsciu to poziom (kanaly
 * stereo i tak sa zwykle rowne), "[off]" gdziekolwiek = wyciszone.
 * Kontrolka bez przelacznika (brak [on]/[off]) jest traktowana jako
 * niewyciszona. */
static int
ReadMixer(int *level, int *muted)
{
    char buf[2048], *p;
    char *argv[] = { "amixer", "get", g_control, NULL };

    if (RunCapture(argv, buf, sizeof(buf)) == -1)
        return -1;
    for (p = strchr(buf, '['); p; p = strchr(p + 1, '[')) {
        char *end;
        long v = strtol(p + 1, &end, 10);

        if (end != p + 1 && end[0] == '%' && end[1] == ']') {
            *level = clamp_pct((int)v);
            *muted = strstr(buf, "[off]") != NULL;
            return 0;
        }
    }
    return -1;
}

static int
SetLevel(int pct)
{
    char arg[16];
    char *argv[] = { "amixer", "-q", "set", g_control, arg, NULL };

    snprintf(arg, sizeof(arg), "%d%%", pct);
    return RunCapture(argv, NULL, 0);
}

static int
ToggleMute(void)
{
    char *argv[] = { "amixer", "-q", "set", g_control, "toggle", NULL };

    return RunCapture(argv, NULL, 0);
}

#endif

/* Odczyt do stanu globalnego; zwraca 1, jesli cokolwiek sie zmienilo
 * (wtedy trzeba przerysowac). */
static int
Refresh(void)
{
    int level = g_level, muted = g_muted;
    int ok = ReadMixer(&level, &muted) == 0;
    int changed = ok != g_ok || level != g_level || muted != g_muted;

    g_ok = ok;
    if (ok) {
        g_level = level;
        g_muted = muted;
        g_applied = level;
    }
    return changed;
}

static void
Apply(int pct)
{
    g_level = clamp_pct(pct);
    if (g_level == g_applied)
        return;
    if (SetLevel(g_level) == 0)
        g_applied = g_level;
    g_last_apply_ms = now_ms();
}

/* ------------------------------------------------------------------ */
/* Rysowanie                                                           */
/* ------------------------------------------------------------------ */

/* Stan przeciagania aktualizowany PRZED ui_meter (wzorzec "najpierw
 * stan, potem rysowanie"), zeby pasek nadazal za kursorem w tej samej
 * klatce. */
static void
UpdateDrag(UiCtx *ctx)
{
    int mx, down;

    if (!g_drag || g_meter_r.w <= 0)
        return;
    ui_mouse_state(ctx, &mx, NULL, &down);
    g_level = clamp_pct((mx - g_meter_r.x) * 100 / g_meter_r.w);
    if (!down) {
        g_drag = 0;
        Apply(g_level);
    } else if (now_ms() - g_last_apply_ms >= DRAG_APPLY_MS) {
        Apply(g_level);
    }
}

static void
draw(UiCtx *ctx, int win_w)
{
    static UiBoxStyle style;
    static int style_ready = 0;
    UiBox *box;
    UiRect row;
    char label[80];
    int m = ui_window_margin(ctx);

    if (!style_ready) {
        style = (UiBoxStyle){0};
        style.margin_l     = m;
        style.margin_r     = m;
        style.margin_t     = V_MARGIN;
        style.margin_b     = V_MARGIN;
        style.padding_l    = ui_box_padding(ctx);
        style.padding_r    = ui_box_padding(ctx);
        style.padding_t    = BOX_PAD_V;
        style.padding_b    = BOX_PAD_V;
        style.gap          = BOX_GAP;
        style.bg_color     = *ui_theme_box_bg(ctx);
        style.border_w     = 1;
        style.border_color = *ui_theme_line_fg(ctx);
        style_ready = 1;
    }

    UpdateDrag(ctx);

    box = ui_box_begin(ctx, "vol", 0, 0, win_w, &style);

    g_meter_r = ui_box_next_rect(box, ROW_H);
    if (!g_ok) {
        snprintf(label, sizeof(label), "No mixer: %s", g_control);
        ui_meter(ctx, g_meter_r, 0.0, label);
    } else if (g_muted) {
        snprintf(label, sizeof(label), "%d%% (muted)", g_level);
        ui_meter_color(ctx, g_meter_r, g_level / 100.0, label, &g_col_muted);
    } else {
        snprintf(label, sizeof(label), "%d%%", g_level);
        ui_meter(ctx, g_meter_r, g_level / 100.0, label);
    }

    row = ui_box_next_rect(box, ROW_H);
    if (ui_button(ctx, ui_rect_col(row, 0, 3, BOX_GAP), "-"))
        Apply(g_level - STEP_PCT);
    if (ui_button(ctx, ui_rect_col(row, 1, 3, BOX_GAP), g_muted ? "Unmute" : "Mute")) {
        ToggleMute();
        Refresh();
    }
    if (ui_button(ctx, ui_rect_col(row, 2, 3, BOX_GAP), "+"))
        Apply(g_level + STEP_PCT);

    ui_box_end(box);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* Klawisze apki (bez pol tekstowych, wiec nic nie koliduje z
 * ui_feed_event). Zwraca 0, gdy apka ma sie zamknac. */
static int
HandleKey(XKeyEvent *kev)
{
    char keybuf[16];
    KeySym ks;

    XLookupString(kev, keybuf, sizeof(keybuf), &ks, NULL);
    switch (ks) {
    case XK_Escape:
    case XK_q:
        return 0;
    case XK_Up: case XK_Right: case XK_plus: case XK_KP_Add:
        Apply(g_level + STEP_PCT);
        break;
    case XK_Down: case XK_Left: case XK_minus: case XK_KP_Subtract:
        Apply(g_level - STEP_PCT);
        break;
    case XK_m:
        ToggleMute();
        Refresh();
        break;
    }
    return 1;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    Window win, root;
    GC gc;
    UiCtx *ctx;
    XEvent ev;
    Atom wm_del;
    /* V_MARGIN + border + padding + dwa wiersze z odstepem + padding +
     * border + V_MARGIN = 6+1+4+20+6+20+4+1+6 = 68 */
    int win_w = WIN_W;
    int win_h = 2 * V_MARGIN + 2 + 2 * BOX_PAD_V + 2 * ROW_H + BOX_GAP;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int screen, running, redraw, i;
    long next_poll_ms;
    char app_name[64] = "7aVol";
    char app_title[64] = "";

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[++i], &geom_x, &geom_y, &geom_w, &geom_h);
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[++i]);
        } else {
            fprintf(stderr, "Uzycie: 7avol [-geometry GEOM] [-name NAME] [-title TEXT]\n");
            return 1;
        }
    }

#ifdef __OpenBSD__
    /* proc exec - fork+execvp sndioctl; bez wpath/cpath, apka nic nie
     * zapisuje. Bez unveil - dziecko dziedziczyloby ograniczenia (ten
     * sam powod co w 7aweather.c). */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7avol: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    ui_resource_string(dpy, "7aVol.control", "7aVol.Control",
                       g_control, sizeof(g_control), DEFAULT_CONTROL);
    /* bez powloki, ale nazwa zaczynajaca sie od '-' bylaby opcja miksera */
    if (g_control[0] == '\0' || g_control[0] == '-')
        snprintf(g_control, sizeof(g_control), "%s", DEFAULT_CONTROL);

    if (geom_mask & WidthValue) win_w = (int)geom_w;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative) ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative) ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;

    win = XCreateSimpleWindow(dpy, root, win_x, win_y, (unsigned)win_w, (unsigned)win_h, 0,
                              BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();

        ch->res_name  = app_name;
        ch->res_class = "7aVol";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }
    {
        /* wysokosc stala (dwa wiersze), szerokosc dowolna - pasek i
         * przyciski skaluja sie z win_w */
        XSizeHints *sh = XAllocSizeHints();

        sh->flags      = PMinSize | PMaxSize;
        sh->min_width  = 120;
        sh->min_height = win_h;
        sh->max_width  = 32000;
        sh->max_height = win_h;
        XSetWMNormalHints(dpy, win, sh);
        XFree(sh);
    }
    wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1",
                  win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7avol: ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_color(ctx, "#888888", &g_col_muted);

    Refresh();

    /* Dwie klatki przed petla - cache wysokosci boxa, patrz 7aping.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);

    next_poll_ms = now_ms() + POLL_MS;
    running = 1;
    redraw = 1;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy przechwycone PRZED ui_feed_event (ui.c nie
             * rozroznia numeru przycisku, wzorem 7askm.c) - w calym oknie,
             * nie tylko nad paskiem, bo okno jest male. */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress && !g_drag) {
                    Apply(g_level + (ev.xbutton.button == Button4 ? STEP_PCT : -STEP_PCT));
                    redraw = 1;
                }
                continue;
            }

            if (ev.type == ButtonPress && ev.xbutton.button == Button1 &&
                in_rect(g_meter_r, ev.xbutton.x, ev.xbutton.y))
                g_drag = 1;

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
                if (!HandleKey(&ev.xkey))
                    running = 0;
                redraw = 1;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_del)
                    running = 0;
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
        if (!running)
            break;

        if (!g_drag && now_ms() >= next_poll_ms) {
            if (Refresh())
                redraw = 1;
            next_poll_ms = now_ms() + POLL_MS;
        }

        if (redraw) {
            ui_begin_frame(ctx);
            draw(ctx, win_w);
            ui_end_frame(ctx);
            redraw = 0;
        }

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy);
            long remaining = next_poll_ms - now_ms();
            struct timeval tv;

            if (remaining < 0)
                remaining = 0;
            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            tv.tv_sec  = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
            /* w trakcie przeciagania polling stoi (patrz wyzej), wiec
             * czekamy tylko na zdarzenia X - inaczej przeterminowany
             * next_poll_ms dawalby zerowy timeout i petle na pusto */
            select(xfd + 1, &rfds, NULL, NULL, g_drag ? NULL : &tv);
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
