/*
 * 7abubbles.c - wizualizacja katalogow ~/projects jako kolorowych kol.
 *
 * Nowa apka (nie port Xt/Xaw). Trzy boxy:
 *   "lbl"    - gorny pasek z opisem; przy najechaniu na kolo pokazuje
 *              nazwe katalogu w kolorze tego kola; inaczej tytul "7aBubbles".
 *   "canvas" - obszar z kolami (tryb bubbles) lub lista katalogow (tryb list).
 *   "nav"    - dolny pasek z przyciskiem przelaczajacym tryb wyswietlania.
 *
 * Tryb listy: katalogi od najnowszego (gora) do najstarszego; kazdy wiersz
 * ma kolorowy kwadrat (ten sam kolor co kolo) i nazwe katalogu; klik otwiera
 * terminal tak samo jak w trybie bubbles.
 * Mapowanie: scan_projects() skanuje ~/projects, sortuje podkatalogi
 * rosnaco po mtime. Bierzemy min(N, N_CIRCLES) najnowszych (N=liczba
 * znalezionych). Kolo 0 = najmniejsze = najstarszy wybrany katalog,
 * kolo g_ndir-1 = najwieksze = najswiezszy. Kolor i rozmiar stale
 * przez caly czas dzialania; pozycje losowane raz przy starcie i przy
 * resize (arc4random_uniform, OpenBSD).
 *
 * Kolejnosc rysowania: 6..0 (duze pierwsze, male na wierzchu) - kazde
 * kolo choc czesciowo widoczne. Hover detection: ta sama odwrotna
 * kolejnosc (wykrywa wizualnie najwyzsze kolo).
 *
 * Kolory: stala paleta N_CIRCLES barw alokowana przez ui_color po
 * ui_init; wlasciciel zwolnienia: free_colors() przed XCloseDisplay.
 * Zamkniecie: Escape lub WM close.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include "../ui.h"

/* ------------------------------------------------------------------ */

#define N_CIRCLES 7
#define BORDER_W  1
#define MAX_NAME  256  /* max dlugosc nazwy katalogu (wlacznie z NUL) */

/* stala paleta - indeks odpowiada numerowi kola (0=najmniejsze/najstarsze) */
static const char *g_palette[N_CIRCLES] = {
    "#e74c3c",  /* czerwony     */
    "#e67e22",  /* pomaranczowy */
    "#f1c40f",  /* zolty        */
    "#2ecc71",  /* zielony      */
    "#1abc9c",  /* morski       */
    "#3498db",  /* niebieski    */
    "#9b59b6",  /* fioletowy    */
};

typedef struct {
    int cx, cy, r;
} Circle;

typedef struct {
    char   name[MAX_NAME];
    time_t mtime;
} DirEntry;

static Circle   g_circles[N_CIRCLES];
static XftColor g_colors[N_CIRCLES];
static int      g_colors_ok = 0;
static int      g_last_cw   = 0;
static int      g_last_ch   = 0;

static DirEntry g_dirs[N_CIRCLES];  /* katalogi przypisane do kol */
static int      g_ndir = 0;         /* ile kol/katalogow aktywnych (0..N_CIRCLES) */
static int      g_mode = 0;         /* 0=bubbles, 1=lista */

static XftColor g_label_bg;         /* czarne tlo labelek; wlasciciel: free_colors() */

/* potrzebne do XftColorFree w free_colors() */
static Display  *g_dpy;
static Visual   *g_visual;
static Colormap  g_cmap;

/* ------------------------------------------------------------------ */

static void
init_colors(UiCtx *ctx)
{
    int i;
    for (i = 0; i < N_CIRCLES; i++)
        ui_color(ctx, g_palette[i], &g_colors[i]);
    ui_color(ctx, "#000000", &g_label_bg);
    g_colors_ok = 1;
}

/* wlasciciel zwolnienia g_colors[] - wolac przed XCloseDisplay */
static void
free_colors(void)
{
    int i;
    if (!g_colors_ok) return;
    for (i = 0; i < N_CIRCLES; i++)
        XftColorFree(g_dpy, g_visual, g_cmap, &g_colors[i]);
    XftColorFree(g_dpy, g_visual, g_cmap, &g_label_bg);
    g_colors_ok = 0;
}

/* ------------------------------------------------------------------ */

/* uruchamia urxvtc z katalogiem ~/projects/<name> jako cwd.
 * fire-and-forget; zombie sprzatane przez SIG_IGN ustawiony w main. */
static void
run_dir(int idx)
{
    struct passwd *pw;
    char           path[512];
    pid_t          pid;

    if (idx < 0 || idx >= g_ndir) return;
    pw = getpwuid(getuid());
    if (!pw) return;
    snprintf(path, sizeof(path), "%s/projects/%s", pw->pw_dir, g_dirs[idx].name);

    pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        execlp("urxvtc", "urxvtc", "-cd", path, (char *)NULL);
        _exit(1);
    }
}

/* ------------------------------------------------------------------ */

static int
cmp_mtime_asc(const void *a, const void *b)
{
    const DirEntry *da = (const DirEntry *)a;
    const DirEntry *db = (const DirEntry *)b;
    if (da->mtime < db->mtime) return -1;
    if (da->mtime > db->mtime) return  1;
    return 0;
}

/* skanuje ~/projects i wypelnia g_dirs[]/g_ndir.
 * bierze min(N, N_CIRCLES) najnowszych katalogow, posortowanych
 * od najstarszego (g_dirs[0]) do najnowszego (g_dirs[g_ndir-1]). */
static void
scan_projects(void)
{
    struct passwd *pw;
    char           base[512];
    DIR           *d;
    struct dirent *ent;
    struct stat    st;
    DirEntry       all[128];
    int            n = 0, start, i;

    g_ndir = 0;

    pw = getpwuid(getuid());
    if (!pw) return;
    snprintf(base, sizeof(base), "%s/projects", pw->pw_dir);

    d = opendir(base);
    if (!d) return;

    while ((ent = readdir(d)) != NULL && n < 128) {
        char full[512];
        if (ent->d_name[0] == '.') continue;
        snprintf(full, sizeof(full), "%s/%s", base, ent->d_name);
        if (stat(full, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        snprintf(all[n].name, MAX_NAME, "%s", ent->d_name);
        all[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);

    qsort(all, (size_t)n, sizeof(DirEntry), cmp_mtime_asc);

    /* bierz ostatnie min(n, N_CIRCLES): najnowsze, od najstarszego do najnowszego */
    g_ndir = n < N_CIRCLES ? n : N_CIRCLES;
    start  = n - g_ndir;
    for (i = 0; i < g_ndir; i++)
        g_dirs[i] = all[start + i];
}

/* ------------------------------------------------------------------ */

/* przelicza pozycje i promienie kol dla obszaru (cx0,cy0,cw,ch).
 * Canvas dzielony na siatke cols x rows (cols*rows >= g_ndir); komórki
 * losowane bez powtorzen (Fisher-Yates), kazde kolo trafia do innej
 * komorki i jest losowo rozmieszczone w jej obrebie. Gwarantuje
 * rownomlerne pokrycie bez skupisk.
 * Promienie: liniowo od min_r (kolo 0) do max_r (kolo g_ndir-1). */
static void
place_circles(int cx0, int cy0, int cw, int ch)
{
    /* max komórek siatki: ceil(sqrt(N_CIRCLES))^2 = 3^2 = 9 dla N_CIRCLES=7 */
    int cells[16];
    int i, min_side, min_r, max_r, span;
    int cols, rows, total, cell_w, cell_h;

    if (cw < 10 || ch < 10 || g_ndir == 0) return;

    min_side = cw < ch ? cw : ch;
    min_r    = min_side / 14;
    if (min_r < 3) min_r = 3;
    max_r    = min_side / 4;
    if (max_r <= min_r) max_r = min_r + 1;
    span = max_r - min_r;

    /* oblicz wymiary siatki */
    cols = 1;
    while (cols * cols < g_ndir) cols++;
    rows  = (g_ndir + cols - 1) / cols;
    total = cols * rows;
    if (total > 16) total = 16;  /* nie przekraczaj bufora */

    cell_w = cw / cols;
    cell_h = ch / rows;
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;

    /* Fisher-Yates: przetasuj indeksy komorek, bierz pierwsze g_ndir */
    for (i = 0; i < total; i++) cells[i] = i;
    for (i = 0; i < g_ndir; i++) {
        int j   = i + (int)arc4random_uniform((unsigned)(total - i));
        int tmp = cells[i]; cells[i] = cells[j]; cells[j] = tmp;
    }

    for (i = 0; i < g_ndir; i++) {
        int r     = (g_ndir > 1)
                    ? min_r + (i * span) / (g_ndir - 1)
                    : min_r + span / 2;
        int col   = cells[i] % cols;
        int row   = cells[i] / cols;
        int x0    = cx0 + col * cell_w;
        int y0    = cy0 + row * cell_h;
        int marg  = r + 2;
        int rng_x = cell_w - 2 * marg;
        int rng_y = cell_h - 2 * marg;

        if (rng_x < 1) rng_x = 1;
        if (rng_y < 1) rng_y = 1;

        g_circles[i].cx = x0 + marg + (int)arc4random_uniform((unsigned)rng_x);
        g_circles[i].cy = y0 + marg + (int)arc4random_uniform((unsigned)rng_y);
        g_circles[i].r  = r;
    }

    g_last_cw = cw;
    g_last_ch = ch;
}

/* ------------------------------------------------------------------ */

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int        style_init = 0;
    int               margin, lh, bw;
    int               lbl_w, lbl_row_h;
    int               cx0, cy0, cw, ch;
    int               btn_row_h, nav_gap, nav_y;
    int               mx, my, hov, i;
    UiBox            *box;

    margin = ui_window_margin(ctx);
    lh     = ui_line_height(ctx);
    bw     = BORDER_W;

    if (!style_init) {
        style              = (UiBoxStyle){0};
        style.border_w     = bw;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color     = *ui_theme_box_bg(ctx);
        style_init         = 1;
    }

    lbl_w     = win_w - 2 * margin;
    lbl_row_h = lh + 4;
    btn_row_h = lh + 4;
    nav_gap   = 6;

    /* geometria canvasu - pomniejszona o nav box na dole */
    cx0   = margin;
    cy0   = margin + bw + lbl_row_h + bw + 4;
    cw    = lbl_w;
    ch    = win_h - cy0 - nav_gap - 2 * bw - btn_row_h - margin;
    if (ch < 4) ch = 4;

    nav_y = cy0 + ch + nav_gap;

    /* przelicz kola przy zmianie rozmiaru - wewnatrz bordera canvasu.
     * porownanie z inner_cw/ch (nie cw/ch), bo place_circles zapisuje
     * w g_last_cw/ch wymiary wewnetrzne (bez bordera). */
    {
        int inner_cw = cw - 2 * bw;
        int inner_ch = ch - 2 * bw;
        if (inner_cw != g_last_cw || inner_ch != g_last_ch)
            place_circles(cx0 + bw, cy0 + bw, inner_cw, inner_ch);
    }

    /* hover detection: tylko w trybie bubbles, od najmniejszego (0) do
     * najwiekszego - male kola sa rysowane na wierzchu, wiec sprawdzamy
     * je pierwsze (pierwsza trafiona = wizualnie najwyzsze kolo). */
    ui_mouse_state(ctx, &mx, &my, NULL);
    hov = -1;
    if (g_mode == 0) {
        for (i = 0; i < g_ndir; i++) {
            int dx = mx - g_circles[i].cx;
            int dy = my - g_circles[i].cy;
            if (dx * dx + dy * dy <= g_circles[i].r * g_circles[i].r) {
                hov = i;
                break;
            }
        }
    }

    /* --- label box --- */
    box = ui_box_begin(ctx, "lbl", margin, margin, lbl_w, &style);
    {
        UiRect lr = ui_box_next_rect(box, lbl_row_h);
        lr.y += (lbl_row_h - lh) / 2;
        lr.h  = lh;
        if (hov >= 0)
            ui_label_centered_fg(ctx, lr, g_dirs[hov].name, &g_colors[hov]);
        else
            ui_label_centered(ctx, lr, "Active projects");
    }
    ui_box_end(box);

    /* --- canvas/lista box --- */
    box = ui_box_begin(ctx, "canvas", cx0, cy0, cw, &style);
    if (g_mode == 0) {
        /* tryb bubbles */
        int clicked = 0;
        UiRect clip;
        ui_box_next_rect(box, ch);  /* rezerwuje wysokosc w cache boxa */
        /* przytnij do wnetrza bordera - kola nie wychodzą poza box */
        clip.x = cx0 + bw;
        clip.y = cy0 + bw;
        clip.w = cw - 2 * bw;
        clip.h = ch - 2 * bw;
        ui_set_clip(ctx, clip);
        /* duze kola pierwsze, male na wierzchu - kazde kolo choc czesciowo widoczne */
        for (i = g_ndir - 1; i >= 0; i--)
            ui_fill_circle(ctx,
                           g_circles[i].cx, g_circles[i].cy,
                           g_circles[i].r, &g_colors[i]);
        /* labelka tylko dla najechanego kola - domyslnie pod, jesli nie miesci
         * sie w clipie - nad kolem. tlo: zaokraglony prostokat. */
        for (i = 0; i < g_ndir; i++) {
            if (i != hov) continue;
            int pad   = 2;
            int cr    = 3;  /* promien zaokraglenia rogow */
            int tw    = ui_text_width(ctx, g_dirs[i].name);
            int lw    = tw + 2 * pad;   /* szerokosc tla labelki */
            int lh_bg = lh + 2 * pad;  /* wysokosc tla labelki */
            int cx_c  = g_circles[i].cx;
            int cy_c  = g_circles[i].cy;
            int r_c   = g_circles[i].r;
            int bx    = cx_c - tw / 2 - pad;
            int by, text_y;
            UiRect lr;

            /* wybor strony: pod kolem jesli miesci sie w clipie, inaczej nad */
            if (cy_c + r_c + 3 + lh_bg <= clip.y + clip.h) {
                by     = cy_c + r_c + 3 - pad;
                text_y = cy_c + r_c + 3;
            } else {
                by     = cy_c - r_c - 3 - lh_bg + pad;
                text_y = cy_c - r_c - 3 - lh;
            }

            /* tlo: krzyz z prostokotow + 4 wypelnione kola w rogach */
            ui_fill_rect(ctx, (UiRect){bx + cr, by,      lw - 2*cr, lh_bg},        &g_label_bg);
            ui_fill_rect(ctx, (UiRect){bx,      by + cr, lw,        lh_bg - 2*cr}, &g_label_bg);
            ui_fill_circle(ctx, bx + cr,      by + cr,         cr, &g_label_bg);
            ui_fill_circle(ctx, bx + lw - cr, by + cr,         cr, &g_label_bg);
            ui_fill_circle(ctx, bx + cr,      by + lh_bg - cr, cr, &g_label_bg);
            ui_fill_circle(ctx, bx + lw - cr, by + lh_bg - cr, cr, &g_label_bg);

            lr.x = cx_c - tw / 2;
            lr.y = text_y;
            lr.w = tw;
            lr.h = lh;
            ui_label_centered_fg(ctx, lr, g_dirs[i].name, &g_colors[i]);
        }
        ui_clear_clip(ctx);
        /* klik: od najmniejszego (na wierzchu) do najwiekszego; pierwsze trafione wygrywa */
        for (i = 0; i < g_ndir && !clicked; i++) {
            UiRect bbox;
            bbox.x = g_circles[i].cx - g_circles[i].r;
            bbox.y = g_circles[i].cy - g_circles[i].r;
            bbox.w = 2 * g_circles[i].r;
            bbox.h = 2 * g_circles[i].r;
            if (ui_hit_test(ctx, bbox)) {
                int dx = mx - g_circles[i].cx;
                int dy = my - g_circles[i].cy;
                if (dx * dx + dy * dy <= g_circles[i].r * g_circles[i].r) {
                    run_dir(i);
                    clicked = 1;
                }
            }
        }
    } else {
        /* tryb listy - jedno ui_box_next_rect(ch) jak w trybie bubbles,
         * zeby cached height boxa "canvas" nie zmienial sie przy przelaczaniu
         * trybow; recty wierszy liczone recznie w obrebie zarezerwowanego obszaru */
        UiRect area  = ui_box_next_rect(box, ch);
        int    row_h = lh + 4;
        int    sq_sz = lh;
        for (i = g_ndir - 1; i >= 0; i--) {
            int    from_top = g_ndir - 1 - i;
            UiRect row, sq_r, name_r;
            row.x    = area.x;
            row.y    = area.y + from_top * row_h;
            row.w    = area.w;
            row.h    = row_h;
            sq_r.x   = row.x + 6;
            sq_r.y   = row.y + (row_h - sq_sz) / 2;
            sq_r.w   = sq_sz;
            sq_r.h   = sq_sz;
            name_r.x = row.x + 6 + sq_sz + 6;
            name_r.y = row.y;
            name_r.w = row.w - 6 - sq_sz - 6;
            name_r.h = row_h;
            if (mx >= row.x && mx < row.x + row.w &&
                my >= row.y && my < row.y + row.h)
                ui_fill_rect(ctx, row, ui_theme_accent(ctx));
            ui_fill_rect(ctx, sq_r, &g_colors[i]);
            ui_label(ctx, name_r, g_dirs[i].name);
            if (ui_hit_test(ctx, row))
                run_dir(i);
        }
    }
    ui_box_end(box);

    /* --- przelacznik trybu (bez boxa - wzorzec 7atodo footer) --- */
    {
        int    btn_w = 80;
        UiRect btn_r;
        btn_r.x = margin;
        btn_r.y = nav_y + (btn_row_h - btn_row_h) / 2;
        btn_r.w = btn_w;
        btn_r.h = btn_row_h;
        if (ui_button(ctx, btn_r, g_mode == 0 ? "List" : "Bubbles"))
            g_mode ^= 1;
    }
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    int         screen;
    Window      root, win;
    GC          gc;
    UiCtx      *ctx;
    XSizeHints *szh;
    Atom        wm_del;
    int         win_w, win_h, win_x, win_y;
    int         geo_mask;
    int         running, redraw;
    XEvent      ev;

    win_w    = 420;
    win_h    = 320;
    win_x    = 0;
    win_y    = 0;
    geo_mask = 0;

    /* opcjonalny argument: -geometry WxH+X+Y */
    if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'g') {
        unsigned int gw = (unsigned)win_w, gh = (unsigned)win_h;
        int          gx = 0, gy = 0;
        geo_mask = XParseGeometry(argv[2], &gx, &gy, &gw, &gh);
        if (geo_mask & WidthValue)  win_w = (int)gw;
        if (geo_mask & HeightValue) win_h = (int)gh;
        if (geo_mask & XValue)      win_x = gx;
        if (geo_mask & YValue)      win_y = gy;
    }

    signal(SIGCHLD, SIG_IGN);
    scan_projects();

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "7abubbles: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    screen   = DefaultScreen(g_dpy);
    root     = RootWindow(g_dpy, screen);
    g_visual = DefaultVisual(g_dpy, screen);
    g_cmap   = DefaultColormap(g_dpy, screen);

    if (!(geo_mask & XValue))
        win_x = (DisplayWidth(g_dpy, screen)  - win_w) / 2;
    if (!(geo_mask & YValue))
        win_y = (DisplayHeight(g_dpy, screen) - win_h) / 2;
    if (geo_mask & XNegative)
        win_x = DisplayWidth(g_dpy, screen)  - win_w - win_x;
    if (geo_mask & YNegative)
        win_y = DisplayHeight(g_dpy, screen) - win_h - win_y;

    win = XCreateSimpleWindow(g_dpy, root,
          win_x, win_y,
          (unsigned)win_w, (unsigned)win_h, 0,
          BlackPixel(g_dpy, screen), WhitePixel(g_dpy, screen));

    XSelectInput(g_dpy, win,
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(g_dpy, win, "7aBubbles");
    XSetIconName(g_dpy, win, "7aBubbles");

    wm_del = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, win, &wm_del, 1);

    szh = XAllocSizeHints();
    if (szh) {
        szh->flags      = PMinSize;
        szh->min_width  = 120;
        szh->min_height = 90;
        if (geo_mask & (XValue | YValue)) {
            szh->flags |= USPosition;
            szh->x = win_x;
            szh->y = win_y;
        }
        if (geo_mask & (WidthValue | HeightValue))
            szh->flags |= USSize;
        XSetWMNormalHints(g_dpy, win, szh);
        XFree(szh);
    }

    XMapWindow(g_dpy, win);

    gc  = XCreateGC(g_dpy, win, 0, NULL);
    ctx = ui_init(g_dpy, win, gc, "DejaVu Sans-9", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7abubbles: ui_init nie powiodlo sie\n");
        XFreeGC(g_dpy, gc);
        XCloseDisplay(g_dpy);
        return 1;
    }

    init_colors(ctx);

    running = 1;
    redraw  = 1;

    while (running) {
        XNextEvent(g_dpy, &ev);
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
            XSetInputFocus(g_dpy, win, RevertToParent, CurrentTime);
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
            int prev_mode = g_mode;
            ui_begin_frame(ctx);
            draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            /* jesli draw() zmienil tryb (g_mode != prev_mode), klatka zostala
             * narysowana w starym trybie - wymusz natychmiastowe przerysowanie */
            redraw = (g_mode != prev_mode);
        }
    }

    free_colors();
    ui_destroy(ctx);
    XFreeGC(g_dpy, gc);
    XCloseDisplay(g_dpy);
    return 0;
}
