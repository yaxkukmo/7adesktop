/*
 * 7aping.c - monitor dostepnosci hostow, odpowiednik xbiff dla sieci.
 *
 * Nowa apka (nie port Xt/Xaw). Sprawdza czy hosty podane w argv sa
 * osiagalne przez ping(8). Kazdy host dostaje wiersz z kolorowym
 * wskaznikiem statusu:
 *   zielony  = odpowiada na ping  (STATUS_UP)
 *   czerwony = brak odpowiedzi    (STATUS_DOWN)
 *   szary    = jeszcze nie badany (STATUS_UNKNOWN)
 * i godziną ostatniego sprawdzenia.
 *
 * Dlaczego ping, nie TCP connect: ping dziala dla dowolnego hosta bez
 * znajomosci konkretnego portu/uslugi. TCP connect bylby szybszy i
 * przechodzilby przez firewalle blokujace ICMP, ale wymagalby -port
 * jako dodatkowego parametru. Dla monitora ogolnego przeznaczenia (jak
 * xbiff dla maila) ping jest wlasciwszym wyborem.
 *
 * Sprawdzanie jest synchroniczne (sequential system() per host) - apka
 * blokuje na czas pingowania. Przy timeout=1s i np. 8 hostach offline
 * to ~8s blokady UI - swiadomy kompromis v1, analogiczny do blokujacego
 * UpdateWeather() w 7aweather.c.
 *
 * Uzycie:
 *   7aping host1 [host2 ...]
 *
 * Opcje:
 *   -title TEXT      tytul okna (domyslnie "7aping")
 *   -name NAME       WM_CLASS resource name (domyslnie "7aping")
 *   -interval SEC    interwał sprawdzania w sekundach (domyslnie 30)
 */

/* popen, gettimeofday w glibc wymagaja _DEFAULT_SOURCE; na OpenBSD nie ma
 * to znaczenia (sa dostepne niezaleznie od _DEFAULT_SOURCE). */
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define MAX_HOSTS           16
#define DEFAULT_INTERVAL_MS 30000   /* 30 sekund */
#define ROW_H               26      /* wysokosc wiersza hosta */
#define HEADER_H            24      /* wysokosc paska tytulowego */
#define DOT_R                6      /* promien kolka statusu */
#define WIN_W              280

/* Na OpenBSD timeout pinga to -w (lowercase), na Linuksie -W (uppercase).
 * %s podstawiane tylko po walidacji przez valid_host() ponizej - brak
 * ryzyka wstrzykniecia komendy. */
#ifdef __OpenBSD__
# define PING_FMT "ping -c 1 -w 1 %s >/dev/null 2>&1"
#else
# define PING_FMT "ping -c 1 -W 1 %s >/dev/null 2>&1"
#endif

/* ------------------------------------------------------------------ */
/* Stan                                                                  */
/* ------------------------------------------------------------------ */

typedef enum { STATUS_UNKNOWN = 0, STATUS_UP, STATUS_DOWN } HostStatus;

static struct {
    char       name[128];
    HostStatus status;
    char       checked_at[16]; /* "HH:MM:SS" lub "" jesli nie sprawdzano */
} g_hosts[MAX_HOSTS];

static int  g_nhost        = 0;
static long g_interval_ms  = DEFAULT_INTERVAL_MS;

/* Kolory statusu; wlascicielem jest colormap displaya - zwolnienie przez
 * XCloseDisplay(), nie wymaga jawnego XFreeColors na TrueColor (pixel
 * obliczany bezposrednio z RGB bez wpisu w colormap). */
static XColor g_col_up;
static XColor g_col_down;
static XColor g_col_unknown;

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                            */
/* ------------------------------------------------------------------ */

static long
now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + (long)tv.tv_usec / 1000;
}

/* Przepuszcza tylko znaki bezpieczne jako argument powloki - zapobiega
 * przypadkowemu wstrzyknieciu metacharaktera przez zlosliwa nazwe hosta.
 * Dopuszcza: alfanumeryczne, kropka, myslnik, dwukropek (IPv6),
 * podkreslenie (czeste w sieciach wewnetrznych). */
static int
valid_host(const char *h)
{
    if (!h || !*h || strlen(h) >= 128)
        return 0;
    for (; *h; h++) {
        unsigned char c = (unsigned char)*h;
        if (!isalnum(c) && c != '.' && c != '-' && c != ':' && c != '_')
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Sprawdzanie                                                           */
/* ------------------------------------------------------------------ */

static void
check_host(int i)
{
    char cmd[256];
    time_t t;
    struct tm *tm_info;
    int ret;

    snprintf(cmd, sizeof(cmd), PING_FMT, g_hosts[i].name);
    ret = system(cmd);
    /* system() zwraca -1 przy bledzie fork lub status wait(); WIFEXITED
     * rozroznia normalne wyjscie od sygnalu/bledu. */
    g_hosts[i].status = (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
                        ? STATUS_UP : STATUS_DOWN;

    t       = time(NULL);
    tm_info = localtime(&t);
    strftime(g_hosts[i].checked_at, sizeof(g_hosts[i].checked_at),
             "%H:%M:%S", tm_info);
}

static void
check_all(void)
{
    int i;
    for (i = 0; i < g_nhost; i++)
        check_host(i);
}

/* ------------------------------------------------------------------ */
/* Rysowanie                                                             */
/* ------------------------------------------------------------------ */

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int style_ready = 0;
    UiBox *box;
    UiRect row, left, mid, right;
    const XColor *dot_col;
    int m = ui_window_margin(ctx);
    int i, dot_cx, dot_cy;

    (void)win_h;

    if (!style_ready) {
        style = (UiBoxStyle){0};
        style.margin_l    = m;
        style.margin_r    = m;
        style.margin_t    = 4;
        style.margin_b    = 4;
        style.gap         = 2;
        style.bg_color    = *ui_theme_box_bg(ctx);
        style.border_w    = 1;
        style.border_color = *ui_theme_line_fg(ctx);
        style_ready = 1;
    }

    box = ui_box_begin(ctx, "hosts", m, 4, win_w - 2 * m, &style);

    for (i = 0; i < g_nhost; i++) {
        row = ui_box_next_rect(box, ROW_H);

        /* left: kolko statusu | mid: nazwa hosta | right: czas */
        ui_rect_split3(row, DOT_R * 2 + 8, 60, 4, &left, &mid, &right);

        switch (g_hosts[i].status) {
        case STATUS_UP:   dot_col = &g_col_up;      break;
        case STATUS_DOWN: dot_col = &g_col_down;    break;
        default:          dot_col = &g_col_unknown; break;
        }
        dot_cx = left.x + left.w / 2;
        dot_cy = left.y + left.h / 2;
        ui_fill_circle(ctx, dot_cx, dot_cy, DOT_R, dot_col);
        ui_draw_circle(ctx, dot_cx, dot_cy, DOT_R, 1, ui_theme_line_fg(ctx));

        ui_label_ellipsis(ctx, mid, g_hosts[i].name);
        ui_label(ctx, right,
                 g_hosts[i].checked_at[0] ? g_hosts[i].checked_at : "---");
    }

    ui_box_end(box);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    Display    *dpy;
    Window      win, root;
    GC          gc;
    UiCtx      *ctx;
    XEvent      ev;
    int         screen, win_w, win_h, running, redraw, i;
    long        next_check_ms;
    char        title[128]   = "7aping";
    char        app_name[64] = "7aping";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(title, sizeof(title), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-interval") == 0 && i + 1 < argc) {
            long sec = strtol(argv[++i], NULL, 10);
            if (sec > 0)
                g_interval_ms = sec * 1000;
        } else if (argv[i][0] != '-') {
            if (g_nhost >= MAX_HOSTS) {
                fprintf(stderr, "7aping: za duzo hostow (max %d)\n", MAX_HOSTS);
                return 1;
            }
            if (!valid_host(argv[i])) {
                fprintf(stderr, "7aping: nieprawidlowa nazwa hosta: %s\n",
                        argv[i]);
                return 1;
            }
            snprintf(g_hosts[g_nhost].name, sizeof(g_hosts[0].name),
                     "%s", argv[i]);
            g_hosts[g_nhost].status        = STATUS_UNKNOWN;
            g_hosts[g_nhost].checked_at[0] = '\0';
            g_nhost++;
        } else {
            fprintf(stderr, "7aping: nieznana opcja: %s\n", argv[i]);
            return 1;
        }
    }

    if (g_nhost == 0) {
        fprintf(stderr,
                "Uzycie: 7aping [-title TEXT] [-name NAME] "
                "[-interval SEC] host [host ...]\n");
        return 1;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7aping: XOpenDisplay failed\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);

    win_w = WIN_W;
    win_h = 16 + g_nhost * (ROW_H + 2);

    win = XCreateSimpleWindow(dpy, root, 0, 0,
                               (unsigned)win_w, (unsigned)win_h,
                               0,
                               BlackPixel(dpy, screen),
                               WhitePixel(dpy, screen));
    XSelectInput(dpy, win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, title);
    XSetIconName(dpy, win, title);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7aping";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }
    {
        XSizeHints *sh = XAllocSizeHints();
        sh->flags      = PMinSize | PMaxSize;
        sh->min_width  = 160;
        sh->min_height = 60;
        sh->max_width  = 32000;
        sh->max_height = 32000;
        XSetWMNormalHints(dpy, win, sh);
        XFree(sh);
    }

    XMapWindow(dpy, win);

    gc  = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc,
                  "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1",
                  win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7aping: ui_init failed\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }

    ui_color(ctx, "#44bb44", &g_col_up);
    ui_color(ctx, "#cc4444", &g_col_down);
    ui_color(ctx, "#888888", &g_col_unknown);

    /* Dwie klatki przed check_all(): pierwsza zapelnia cache wysokosci
     * boxa (immediate-mode nie zna wysokosci przed rysowaniem - patrz
     * CLAUDE.md), druga uzywa juz poprawnej wysokosci i rysuje tlo/border.
     * Bez tego box pojawialby sie bez tla az do pierwszego odswiezenia. */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    check_all();
    next_check_ms = now_ms() + g_interval_ms;
    redraw  = 1;
    running = 1;

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
                if (ev.xconfigure.width  != win_w ||
                    ev.xconfigure.height != win_h) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;
                    ui_resize(ctx, win_w, win_h);
                }
                redraw = 1;
                break;
            }
        }

        if (redraw) {
            ui_begin_frame(ctx);
            draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            redraw = 0;
        }

        {
            long remaining = next_check_ms - now_ms();
            if (remaining <= 0) {
                check_all();
                next_check_ms = now_ms() + g_interval_ms;
                redraw = 1;
                continue;
            }
            {
                fd_set rfds;
                int xfd = ConnectionNumber(dpy);
                struct timeval tv;
                FD_ZERO(&rfds);
                FD_SET(xfd, &rfds);
                tv.tv_sec  = remaining / 1000;
                tv.tv_usec = (remaining % 1000) * 1000;
                select(xfd + 1, &rfds, NULL, NULL, &tv);
            }
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
