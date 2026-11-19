/*
 * 7abubbles.c - lista katalogow ~/projects z akcjami dla wybranego projektu.
 *
 * Nowa apka (nie port Xt/Xaw). Wczesniej wizualizacja katalogow jako
 * kolorowych kol z przelacznikiem Bubbles/List - zastapiona sama tabela
 * (wyrazne zyczenie uzytkownika). Uklad okna:
 *   "list" - box z tabela: naglowki kolumn "Project" i "Date" (data
 *            ostatniej modyfikacji katalogu, st_mtime), wszystkie
 *            podkatalogi ~/projects od najnowszego; STALA liczba
 *            widocznych wierszy (VISIBLE_ROWS), nadmiar przewijany kolkiem
 *            i scrollbarem ze strzalkami + kropkami (wzorem utils/7agroff.c).
 *   pasek akcji pod boxem - przyciski "Open Terminal" i "Run LLM",
 *            widoczne dopiero po wybraniu wiersza (klik), plus
 *            "Description", gdy projekt ma README/README.md/README.txt
 *            (sprawdzane raz przy skanowaniu, sciezka w $README).
 * Na poczatku kazdego wiersza ikonka katalogu (utils/icons/mini-kfm.xpm,
 * wkompilowana, libXpm). ui_draw_pixmap nie zna maski, wiec
 * przezroczystosc jest wypiekana RAZ przy starcie w trzech wariantach tla
 * wiersza (zwykly/hover/zaznaczony) - patrz LoadRowIcons.
 *
 * Akcje definiowane w zasobach X (patrz Xresources.sample):
 *   7aBubbles.terminalCommand, 7aBubbles.llmCommand, 7aBubbles.readmeCommand
 * Komenda idzie przez "/bin/sh -c" z cwd = katalog projektu i sciezka w
 * zmiennej $PROJECT - nazwa katalogu nigdy nie jest wklejana do tekstu
 * komendy, wiec nie da sie nia wstrzyknac skladni powloki.
 *
 * Lista skanowana raz przy starcie (bez timera) - 'r' skanuje ponownie.
 * Wybor pamietany po nazwie, wiec przezywa reskan. Daty formatowane raz
 * przy skanowaniu, nie co klatke.
 * Zamkniecie: Escape/q lub WM close.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include "../ui.h"
#include "icons/bubble_chart_48.xbm"
#include "icons/mini-kfm.xpm"

/* ------------------------------------------------------------------ */

#define ROW_H            20
#define SCROLLBAR_W      ROW_H /* przyciski strzalek ROW_H x ROW_H, jak w 7agroff */
#define SB_GAP           6
#define SCROLL_DOT_PITCH 14
#define SCROLL_DOT_R     4
#define VISIBLE_ROWS     10
#define BTN_GAP          6
#define MAX_DIRS         256
#define MAX_NAME         256   /* dlugosc nazwy katalogu (wlacznie z NUL) */
#define DATE_LEN         20    /* "YYYY-MM-DD HH:MM" + NUL, z zapasem */
#define CMD_LEN          512
#define PATH_LEN         800   /* base (512) + '/' + nazwa (256) + NUL, z zapasem */

/* Te same wartosci co w stylu boxa w draw() - wspolne z ContentHeight(),
 * zeby nie rozjechaly sie (wzorzec z utils/7agroff.c). */
#define BOX_BORDER_W  1
#define BOX_PAD_TB    6
#define BOX_MARGIN_TB 6
#define BOX_GAP       2

typedef struct {
    char   name[MAX_NAME];
    char   date[DATE_LEN];
    time_t mtime;
    int    readme;  /* indeks w k_readme_names + 1, 0 = brak README */
} DirEntry;

/* kolejnosc = priorytet, gdy w katalogu jest kilka wariantow */
static const char *const k_readme_names[] = { "README", "README.md", "README.txt" };
#define N_README ((int)(sizeof(k_readme_names) / sizeof(k_readme_names[0])))

static DirEntry g_dirs[MAX_DIRS];
static int      g_ndir   = 0;
static int      g_sel    = -1;  /* indeks w g_dirs albo -1 */
static int      g_scroll = 0;
static UiRect   g_list_r;       /* widoczne wiersze z ostatniej klatki - kolko myszy */

static char g_base[512];        /* $HOME/projects */
static char g_term_cmd[CMD_LEN];
static char g_llm_cmd[CMD_LEN];
static char g_readme_cmd[CMD_LEN];

/* ikonka wiersza w wariantach tla: zwykly / hover / zaznaczony.
 * Wlasciciel: LoadRowIcons tworzy, FreeRowIcons zwalnia przed ui_destroy. */
enum { ICON_NORMAL, ICON_HOVER, ICON_SELECTED, ICON_N };
#define ICON_GAP 4
static Pixmap g_icon[ICON_N] = { None, None, None };
static int    g_icon_w, g_icon_h;

/* ------------------------------------------------------------------ */

/* kfm[] -> trzy Pixmapy z przezroczystoscia wypieczona kolorem tla
 * danego stanu wiersza (maska libXpm jako clip GC przy kopiowaniu).
 * Przy bledzie g_icon zostaje None i wiersze rysuja sie bez ikonki. */
static void
LoadRowIcons(UiCtx *ctx, Display *dpy, Window win, unsigned depth)
{
    const XColor *bg[ICON_N];
    Pixmap        src = None, mask = None;
    XpmAttributes attrs;
    GC            gc;
    int           i;

    bg[ICON_NORMAL]   = ui_theme_box_bg(ctx);
    bg[ICON_HOVER]    = ui_theme_button_bg(ctx);
    bg[ICON_SELECTED] = ui_theme_accent(ctx);

    memset(&attrs, 0, sizeof(attrs));
    if (XpmCreatePixmapFromData(dpy, win, kfm, &src, &mask, &attrs) != XpmSuccess)
        return;
    g_icon_w = (int)attrs.width;
    g_icon_h = (int)attrs.height;

    if (g_icon_w > 0 && g_icon_h > 0 && g_icon_h <= ROW_H) {
        gc = XCreateGC(dpy, win, 0, NULL);
        for (i = 0; i < ICON_N; i++) {
            g_icon[i] = XCreatePixmap(dpy, win, (unsigned)g_icon_w, (unsigned)g_icon_h, depth);
            XSetForeground(dpy, gc, bg[i]->pixel);
            XFillRectangle(dpy, g_icon[i], gc, 0, 0, (unsigned)g_icon_w, (unsigned)g_icon_h);
            XSetClipMask(dpy, gc, mask);
            XCopyArea(dpy, src, g_icon[i], gc, 0, 0, (unsigned)g_icon_w, (unsigned)g_icon_h, 0, 0);
            XSetClipMask(dpy, gc, None);
        }
        XFreeGC(dpy, gc);
    }

    XFreePixmap(dpy, src);
    if (mask != None) XFreePixmap(dpy, mask);
    XpmFreeAttributes(&attrs);
}

static void
FreeRowIcons(Display *dpy)
{
    int i;

    for (i = 0; i < ICON_N; i++)
        if (g_icon[i] != None) {
            XFreePixmap(dpy, g_icon[i]);
            g_icon[i] = None;
        }
}

/* ------------------------------------------------------------------ */

/* fire-and-forget: "/bin/sh -c cmd" w katalogu projektu, sciezka w $PROJECT,
 * a gdy projekt ma README - pelna sciezka do niego w $README.
 * Zombie sprzatane przez SIG_IGN na SIGCHLD ustawiony w main. */
static void
run_action(const char *cmd, int idx)
{
    char  path[PATH_LEN];
    char  readme[PATH_LEN + 16];
    int   ri;
    pid_t pid;

    if (idx < 0 || idx >= g_ndir || cmd[0] == '\0') return;
    snprintf(path, sizeof(path), "%s/%s", g_base, g_dirs[idx].name);
    ri = g_dirs[idx].readme - 1;
    readme[0] = '\0';
    if (ri >= 0 && ri < N_README)
        snprintf(readme, sizeof(readme), "%s/%s", path, k_readme_names[ri]);

    pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        if (chdir(path) != 0) _exit(1);
        if (setenv("PROJECT", path, 1) != 0) _exit(1);
        if (readme[0] && setenv("README", readme, 1) != 0) _exit(1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    }
}

/* ------------------------------------------------------------------ */

static int
cmp_mtime_desc(const void *a, const void *b)
{
    const DirEntry *da = (const DirEntry *)a;
    const DirEntry *db = (const DirEntry *)b;
    if (da->mtime > db->mtime) return -1;
    if (da->mtime < db->mtime) return  1;
    return strcmp(da->name, db->name);
}

/* zwraca indeks pierwszego istniejacego README* (zwykly plik) + 1, 0 = brak */
static int
find_readme(const char *dir)
{
    char        path[PATH_LEN + 16];
    struct stat st;
    int         i;

    for (i = 0; i < N_README; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, k_readme_names[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            return i + 1;
    }
    return 0;
}

/* skanuje g_base i wypelnia g_dirs[] od najnowszego; wybor zachowany po nazwie */
static void
scan_projects(void)
{
    char           sel_name[MAX_NAME] = "";
    DIR           *d;
    struct dirent *ent;
    struct stat    st;
    int            i;

    if (g_sel >= 0 && g_sel < g_ndir)
        snprintf(sel_name, sizeof(sel_name), "%s", g_dirs[g_sel].name);
    g_ndir = 0;
    g_sel  = -1;

    d = opendir(g_base);
    if (!d) return;

    while ((ent = readdir(d)) != NULL && g_ndir < MAX_DIRS) {
        char       full[PATH_LEN];
        struct tm *tm;
        DirEntry  *e = &g_dirs[g_ndir];

        if (ent->d_name[0] == '.') continue;
        snprintf(full, sizeof(full), "%s/%s", g_base, ent->d_name);
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(e->name, sizeof(e->name), "%s", ent->d_name);
        e->mtime = st.st_mtime;
        tm = localtime(&st.st_mtime);
        if (!tm || strftime(e->date, sizeof(e->date), "%Y-%m-%d %H:%M", tm) == 0)
            e->date[0] = '\0';
        e->readme = find_readme(full);
        g_ndir++;
    }
    closedir(d);

    qsort(g_dirs, (size_t)g_ndir, sizeof(DirEntry), cmp_mtime_desc);

    for (i = 0; i < g_ndir && sel_name[0]; i++)
        if (strcmp(g_dirs[i].name, sel_name) == 0) {
            g_sel = i;
            break;
        }
}

/* ------------------------------------------------------------------ */

static int
BoxHeightForRows(int n_rows)
{
    return n_rows * ROW_H + (n_rows - 1) * BOX_GAP + 2 * BOX_BORDER_W + 2 * BOX_PAD_TB;
}

/* box z naglowkiem + VISIBLE_ROWS wierszy, pod nim pasek przyciskow */
static int
ContentHeight(void)
{
    return BOX_MARGIN_TB + BoxHeightForRows(1 + VISIBLE_ROWS) + BOX_MARGIN_TB
           + ROW_H + BOX_MARGIN_TB;
}

/* Kropki stron na torze scrollbara - wzorem DrawScrollDots w utils/7agroff.c.
 * Strona = VISIBLE_ROWS wierszy; ostatnia kropka = koniec listy. */
static int
ScrollDotsCurrent(int pages, int max_scroll)
{
    if (g_scroll >= max_scroll) return pages - 1;
    return (g_scroll + VISIBLE_ROWS / 2) / VISIBLE_ROWS;
}

static int
ScrollDotsStart(int visible, int pages, int current)
{
    int start = current - visible / 2;

    if (start > pages - visible) start = pages - visible;
    if (start < 0) start = 0;
    return start;
}

static void
DrawScrollDots(UiCtx *ctx, UiRect track, int max_scroll)
{
    int pages = 1 + (max_scroll + VISIBLE_ROWS - 1) / VISIBLE_ROWS;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = ScrollDotsStart(visible, pages, ScrollDotsCurrent(pages, max_scroll));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            g_scroll = (start + i) * VISIBLE_ROWS;
            if (g_scroll > max_scroll) g_scroll = max_scroll;
            break;
        }
    }

    current = ScrollDotsCurrent(pages, max_scroll);
    start = ScrollDotsStart(visible, pages, current);
    for (i = 0; i < visible; i++) {
        int cx = track.x + track.w / 2;
        int cy = y0 + i * SCROLL_DOT_PITCH + SCROLL_DOT_PITCH / 2;
        int more = (i == 0 && start > 0) ||
                   (i == visible - 1 && start + visible < pages);
        int r = more ? SCROLL_DOT_R / 2 : SCROLL_DOT_R;

        if (start + i == current)
            ui_fill_circle(ctx, cx, cy, r, ui_theme_fg(ctx));
        else
            ui_draw_circle(ctx, cx, cy, r, 1, ui_theme_fg(ctx));
    }
}

/* strzalki: ui_arrow_button;
 * g_scroll zmieniany PRZED rysowaniem wierszy - ta sama klatka */
static void
DrawScrollbar(UiCtx *ctx, UiRect sb, int max_scroll)
{
    UiRect up_r    = { sb.x, sb.y, sb.w, ROW_H };
    UiRect down_r  = { sb.x, sb.y + sb.h - ROW_H, sb.w, ROW_H };
    UiRect track_r = { sb.x, sb.y + ROW_H, sb.w, sb.h - 2 * ROW_H };
    int up_clicked   = ui_arrow_button(ctx, up_r, UI_ARROW_UP);
    int down_clicked = ui_arrow_button(ctx, down_r, UI_ARROW_DOWN);

    if (up_clicked) g_scroll--;
    if (down_clicked) g_scroll++;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    if (track_r.h > 0)
        DrawScrollDots(ctx, track_r, max_scroll);
}

/* Tabela Project | Date. Klik ustawia g_sel PRZED narysowaniem wiersza
 * (ui_hit_test), wiec podswietlenie i przyciski akcji widza nowy wybor w
 * tej samej klatce. Zwraca y pod boxem. */
static int
DrawListBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    UiBox *box;
    int    date_w, max_scroll, i, mx, my;
    int    grid_x, grid_y0, grid_y1;

    ui_mouse_state(ctx, &mx, &my, NULL);
    date_w = ui_text_width(ctx, "0000-00-00 00:00") + 2 * UI_CELL_PAD;

    max_scroll = g_ndir - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    box = ui_box_begin(ctx, "list", 0, y, win_w, style);

    {
        UiRect hdr    = ui_box_next_rect(box, ROW_H);
        int    list_h = VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap;
        UiRect sb_r   = { hdr.x + hdr.w - SCROLLBAR_W, hdr.y + ROW_H + style->gap,
                          SCROLLBAR_W, list_h };
        UiRect strip  = ui_box_header_strip(box, hdr);
        int    date_x;

        hdr.w -= SCROLLBAR_W + SB_GAP;
        date_x = hdr.x + hdr.w - date_w;
        ui_header_cell(ctx, (UiRect) { strip.x, strip.y, date_x - strip.x, strip.h },
                       hdr.x + UI_CELL_PAD, "Project");
        ui_header_cell(ctx, (UiRect) { date_x - 1, strip.y, strip.x + strip.w - date_x + 1, strip.h },
                       date_x + UI_CELL_PAD, "Date");
        DrawScrollbar(ctx, sb_r, max_scroll);
        grid_x  = date_x - 1;
        grid_y0 = sb_r.y;
        grid_y1 = sb_r.y + sb_r.h - 1;
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int    idx = g_scroll + i;
        int    name_w;
        UiRect name_r, date_r;

        row.w -= SCROLLBAR_W + SB_GAP;
        name_w = row.w - date_w;
        if (name_w < 2 * UI_CELL_PAD) name_w = 2 * UI_CELL_PAD;
        name_r = (UiRect) { row.x + UI_CELL_PAD, row.y, name_w - 2 * UI_CELL_PAD, row.h };
        date_r = (UiRect) { row.x + name_w + UI_CELL_PAD, row.y, date_w - 2 * UI_CELL_PAD, row.h };

        if (i == 0)
            g_list_r = (UiRect) { row.x, row.y, row.w,
                                  VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap };

        if (g_ndir == 0) {
            if (i == 0) ui_label(ctx, name_r, "No directories in ~/projects");
        } else if (idx < g_ndir) {
            int hover = mx >= row.x && mx < row.x + row.w &&
                        my >= row.y && my < row.y + row.h;

            if (ui_hit_test(ctx, row))
                g_sel = idx;
            if (idx == g_sel)
                ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
            else if (hover)
                ui_fill_round_rect(ctx, row, ui_theme_button_bg(ctx));
            if (g_icon[ICON_NORMAL] != None) {
                int    st = idx == g_sel ? ICON_SELECTED : hover ? ICON_HOVER : ICON_NORMAL;
                UiRect ir = { name_r.x, row.y + (row.h - g_icon_h) / 2, g_icon_w, g_icon_h };

                ui_draw_pixmap(ctx, ir, g_icon[st]);
                name_r.x += g_icon_w + ICON_GAP;
                name_r.w -= g_icon_w + ICON_GAP;
                if (name_r.w < 0) name_r.w = 0;
            }
            ui_label_ellipsis(ctx, name_r, g_dirs[idx].name);
            ui_label(ctx, date_r, g_dirs[idx].date);
        }
    }

    /* linia siatki PO wierszach - inaczej tlo wiersza by ja przerwalo */
    if (g_ndir > 0)
        ui_draw_line(ctx, grid_x, grid_y0, grid_x, grid_y1, 1, ui_theme_line_fg(ctx));

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "list") + style->margin_b;
}

static int
BarButton(UiCtx *ctx, int *x, int y, const char *label)
{
    UiRect r = { *x, y, ui_button_width(ctx, label), ROW_H };

    *x += r.w + BTN_GAP;
    return ui_button(ctx, r, label);
}

/* przyciski akcji tylko przy wybranym projekcie */
static void
DrawActionBar(UiCtx *ctx, int y)
{
    int x = ui_window_margin(ctx);

    if (g_sel < 0 || g_sel >= g_ndir) return;
    if (BarButton(ctx, &x, y, "Open Terminal")) run_action(g_term_cmd, g_sel);
    if (BarButton(ctx, &x, y, "Run LLM"))       run_action(g_llm_cmd, g_sel);
    if (g_dirs[g_sel].readme &&
        BarButton(ctx, &x, y, "Description"))   run_action(g_readme_cmd, g_sel);
}

static void
draw(UiCtx *ctx, int win_w)
{
    static UiBoxStyle style;
    static int        ready = 0;
    int               y;

    if (!ready) {
        style.margin_t     = style.margin_b = BOX_MARGIN_TB;
        style.margin_l     = style.margin_r = ui_window_margin(ctx);
        style.padding_l    = style.padding_r = ui_box_padding(ctx);
        style.padding_t    = style.padding_b = BOX_PAD_TB;
        style.border_w     = BOX_BORDER_W;
        style.gap          = BOX_GAP;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color     = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    y = DrawListBox(ctx, &style, win_w, 0);
    DrawActionBar(ctx, y);
}

static void
redraw_frame(UiCtx *ctx, int win_w)
{
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    Display       *dpy;
    int            screen;
    Window         root, win;
    GC             gc;
    UiCtx         *ctx;
    XSizeHints    *szh;
    Atom           wm_del;
    struct passwd *pw;
    int            win_w, win_h, win_x, win_y;
    int            geo_mask;
    int            running, redraw;
    int            i;
    char           app_name[64] = "7aBubbles";
    char           app_title[64] = "";
    XEvent         ev;

    win_w    = 420;
    win_h    = ContentHeight();
    win_x    = 0;
    win_y    = 0;
    geo_mask = 0;

    /* opcjonalne argumenty: -geometry WxH+X+Y, -name NAZWA, -title TYTUL */
    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            unsigned int gw = (unsigned)win_w, gh = (unsigned)win_h;
            int          gx = 0, gy = 0;
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
    if (win_h < ContentHeight()) win_h = ContentHeight();

    signal(SIGCHLD, SIG_IGN);

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w utils/7afm.c: komendy akcji (terminal,
     * LLM) dziedzicza ograniczenia unveil po exec, a potrzebuja szerokiego
     * dostepu do wlasnej konfiguracji. "getpw" dla getpwuid() nizej. */
    if (pledge("stdio rpath proc exec unix prot_exec getpw", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    pw = getpwuid(getuid());
    if (!pw) {
        fprintf(stderr, "7abubbles: nie znaleziono katalogu domowego\n");
        return 1;
    }
    snprintf(g_base, sizeof(g_base), "%s/projects", pw->pw_dir);
    scan_projects();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7abubbles: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    ui_resource_string(dpy, "7aBubbles.terminalCommand", "7aBubbles.TerminalCommand",
                       g_term_cmd, sizeof(g_term_cmd), "urxvtc -cd \"$PROJECT\"");
    ui_resource_string(dpy, "7aBubbles.llmCommand", "7aBubbles.LlmCommand",
                       g_llm_cmd, sizeof(g_llm_cmd), "urxvtc -cd \"$PROJECT\" -e claude");
    ui_resource_string(dpy, "7aBubbles.readmeCommand", "7aBubbles.ReadmeCommand",
                       g_readme_cmd, sizeof(g_readme_cmd), "urxvtc -e less \"$README\"");

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);

    if (!(geo_mask & XValue))
        win_x = (DisplayWidth(dpy, screen)  - win_w) / 2;
    if (!(geo_mask & YValue))
        win_y = (DisplayHeight(dpy, screen) - win_h) / 2;
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
        if (ch) {
            ch->res_name  = app_name;
            ch->res_class = "7aBubbles";
            XSetClassHint(dpy, win, ch);
            XFree(ch);
        }
    }

    wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    /* stala liczba wierszy - okno nie nizsze niz zawartosc */
    szh = XAllocSizeHints();
    if (szh) {
        szh->flags      = PMinSize;
        szh->min_width  = 240;
        szh->min_height = ContentHeight();
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
        fprintf(stderr, "7abubbles: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, bubble_chart_48_width, bubble_chart_48_height, bubble_chart_48_bits);
    LoadRowIcons(ctx, dpy, win, (unsigned)DefaultDepth(dpy, screen));

    /* Dwie klatki PRZED petla glowna - cache wysokosci boxa, patrz
     * utils/7aping.c i architektura w CLAUDE.md. */
    redraw_frame(ctx, win_w);
    redraw_frame(ctx, win_w);

    running = 1;
    redraw  = 1;

    while (running) {
        XNextEvent(dpy, &ev);

        /* Kolko myszy (Button4/5) przechwycone PRZED ui_feed_event - ui.c
         * nie rozroznia numeru przycisku, wiec kolko liczyloby sie jak klik
         * (patrz utils/7agroff.c). Bez "continue", zeby nie ominac redraw. */
        if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
            (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
            if (ev.type == ButtonPress) {
                int px = ev.xbutton.x, py = ev.xbutton.y;

                if (px >= g_list_r.x && px < g_list_r.x + g_list_r.w &&
                    py >= g_list_r.y && py < g_list_r.y + g_list_r.h) {
                    g_scroll += (ev.xbutton.button == Button4) ? -1 : 1;
                    redraw = 1;
                }
            }
        } else {
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
                if (ks == XK_Escape || ks == XK_q)
                    running = 0;
                else if (ks == XK_r)
                    scan_projects();
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
        }

        if (redraw) {
            redraw_frame(ctx, win_w);
            redraw = 0;
        }
    }

    FreeRowIcons(dpy);
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
