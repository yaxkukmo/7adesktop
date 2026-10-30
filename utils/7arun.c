/*
 * 7arun.c - launcher z klawiatury, odpowiednik dmenu_run.
 *
 * Nowa apka (nie port Xt/Xaw). Uzupelnienie 7acenter (siatka ikon do
 * klikania): pole tekstowe + filtrowana lista, obslugiwane w calosci
 * klawiatura. Kandydaci:
 *   - pozycje z ~/.7a/center.conf ("Nazwa|komenda|ikona", ten sam plik i
 *     format co 7acenter, trzecie pole ignorowane) - na gorze listy,
 *     dopasowywane po nazwie ORAZ komendzie, w wierszu widac komende;
 *   - pliki wykonywalne z $PATH (jak dmenu_path), posortowane, bez
 *     duplikatow (wygrywa pierwszy katalog w $PATH - nazwa i tak trafia
 *     do sh, ktory szuka tak samo).
 * Dopasowanie: podciag bez wielkosci liter (Utf8LowerFold, kopia z
 * 7ashop.c - polskie znaki w nazwach z center.conf); w $PATH najpierw
 * trafienia od poczatku nazwy, potem reszta.
 *
 * Klawisze (przechwytywane w main() PRZED ui_feed_event, reszta idzie do
 * pola, ktore ma fokus przez caly czas):
 *   Enter          uruchom zaznaczony (gdy nic nie pasuje - wpisany tekst)
 *   Shift/Ctrl-Enter  uruchom wpisany tekst dokladnie tak, jak jest
 *                  (np. "xterm -e top" - z argumentami nic nie pasuje,
 *                  wiec wystarcza tez zwykly Enter)
 *   Tab            wpisz nazwe zaznaczonego do pola (potem argumenty)
 *   Up/Down, Ctrl-p/n, Ctrl-k/j, PgUp/PgDn  zaznaczenie
 *   Ctrl-u         wyczysc pole
 *   Escape         zamknij
 * Klik w wiersz uruchamia go, kolko przewija liste.
 *
 * Uruchomienie: fork + setsid + execl("/bin/sh", "sh", "-c", cmd) i
 * natychmiastowe wyjscie apki (jak dmenu) - powloka, bo wpisany tekst to
 * z definicji komenda uzytkownika (argumenty, ~, zmienne). Komendy z
 * center.conf tez ida przez sh (w 7acenter: execvp po spacjach) - dla
 * formatu bez cudzyslowow wynik ten sam.
 *
 * Pamiec: nazwy w jednej puli (g_pool, offsety zamiast wskaznikow, bo
 * realloc ja przesuwa), tablice wpisow/rang/trafien alokowane raz przy
 * starcie - filtrowanie na kazdy klawisz bez malloc. Kilka tysiecy
 * programow w $PATH to rzad kilkudziesieciu kB.
 *
 * Uzycie:
 *   7arun [-geometry GEOM] [-name NAME] [-title TEXT]
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/terminal_48.xbm"

#define QUERY_LEN       256
#define NAME_LEN        256
#define VISIBLE_ROWS    10
#define ROW_H           20
#define ROW_GAP         2
#define COL_GAP         6
#define V_MARGIN        6
#define BOX_PAD_V       4
#define WIN_W           420

/* Offsety w g_pool; dla wpisu z $PATH cmd == name. */
typedef struct {
    int name;
    int cmd;
    unsigned char conf;   /* 1 = z center.conf */
} Entry;

/* ------------------------------------------------------------------ */
/* Stan                                                                */
/* ------------------------------------------------------------------ */

static char  *g_pool;
static size_t g_pool_len, g_pool_cap;
static Entry *g_entries;
static int    g_count, g_cap;
static int    g_conf_count;   /* g_entries[0..g_conf_count) z center.conf */

static unsigned char *g_rank; /* per wpis, wynik Filter(), 0xff = brak */
static int   *g_matches;      /* indeksy w g_entries, w kolejnosci listy */
static int    g_nmatch;

static char g_query[QUERY_LEN] = "";
static int  g_cursor = 0;
static char g_filtered[QUERY_LEN] = "";  /* zapytanie ostatniego Filter() */
static int  g_sel = -1;       /* indeks w g_matches */
static int  g_scroll = 0;
static UiRect g_list_r;       /* wiersze z OSTATNIEJ klatki, jak w 7atube.c */
static int  g_quit = 0;

/* ------------------------------------------------------------------ */
/* Male litery + polskie znaki diakrytyczne - kopia z 7ashop.c         */
/* ------------------------------------------------------------------ */

static void
Utf8LowerFold(const char *in, char *out, size_t outsz)
{
    static const struct { unsigned char u0, u1, l0, l1; } tbl[] = {
        { 0xC4, 0x84, 0xC4, 0x85 }, /* Ą ą */
        { 0xC4, 0x86, 0xC4, 0x87 }, /* Ć ć */
        { 0xC4, 0x98, 0xC4, 0x99 }, /* Ę ę */
        { 0xC5, 0x81, 0xC5, 0x82 }, /* Ł ł */
        { 0xC5, 0x83, 0xC5, 0x84 }, /* Ń ń */
        { 0xC3, 0x93, 0xC3, 0xB3 }, /* Ó ó */
        { 0xC5, 0x9A, 0xC5, 0x9B }, /* Ś ś */
        { 0xC5, 0xB9, 0xC5, 0xBA }, /* Ź ź */
        { 0xC5, 0xBB, 0xC5, 0xBC }, /* Ż ż */
    };
    size_t inlen = strlen(in);
    size_t ii = 0, oi = 0;

    while (ii < inlen && oi + 1 < outsz) {
        unsigned char c0 = (unsigned char) in[ii];

        if (c0 < 0x80) {
            out[oi++] = (char) tolower(c0);
            ii++;
        } else if ((c0 & 0xE0) == 0xC0 && ii + 1 < inlen && oi + 2 < outsz) {
            unsigned char c1 = (unsigned char) in[ii + 1];
            size_t t;

            for (t = 0; t < sizeof(tbl) / sizeof(tbl[0]); t++) {
                if (tbl[t].u0 == c0 && tbl[t].u1 == c1) {
                    c0 = tbl[t].l0;
                    c1 = tbl[t].l1;
                    break;
                }
            }
            out[oi++] = (char) c0;
            out[oi++] = (char) c1;
            ii += 2;
        } else {
            out[oi++] = (char) c0;
            ii++;
        }
    }
    out[oi] = '\0';
}

/* ------------------------------------------------------------------ */
/* Kandydaci (bez ui_*)                                                */
/* ------------------------------------------------------------------ */

static const char *
EntryName(int i)
{
    return g_pool + g_entries[i].name;
}

static const char *
EntryCmd(int i)
{
    return g_pool + g_entries[i].cmd;
}

/* Dopisuje s do puli; zwraca offset albo -1 przy OOM. */
static int
PoolAdd(const char *s)
{
    size_t n = strlen(s) + 1;

    if (g_pool_len + n > g_pool_cap) {
        size_t cap = g_pool_cap ? g_pool_cap * 2 : 16384;
        char *tmp;

        while (cap < g_pool_len + n)
            cap *= 2;
        tmp = realloc(g_pool, cap);
        if (!tmp)
            return -1;
        g_pool = tmp;
        g_pool_cap = cap;
    }
    memcpy(g_pool + g_pool_len, s, n);
    g_pool_len += n;
    return (int)(g_pool_len - n);
}

/* cmd == NULL: komenda = nazwa (wpis z $PATH, bez drugiej kopii). */
static int
AddEntry(const char *name, const char *cmd, int conf)
{
    int noff, coff;

    if (g_count == g_cap) {
        int cap = g_cap ? g_cap * 2 : 1024;
        Entry *tmp = realloc(g_entries, (size_t)cap * sizeof(Entry));

        if (!tmp)
            return -1;
        g_entries = tmp;
        g_cap = cap;
    }
    noff = PoolAdd(name);
    if (noff < 0)
        return -1;
    coff = cmd ? PoolAdd(cmd) : noff;
    if (coff < 0)
        return -1;
    g_entries[g_count].name = noff;
    g_entries[g_count].cmd = coff;
    g_entries[g_count].conf = (unsigned char)conf;
    g_count++;
    return 0;
}

static char *
TrimInPlace(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0')
        return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end--;
    end[1] = '\0';
    return s;
}

/* ~/.7a/center.conf - brak pliku to nie blad (sam $PATH wystarczy). */
static void
LoadConf(void)
{
    const char *home = getenv("HOME");
    char path[1024], line[512];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/.7a/center.conf", home ? home : ".");
    fp = fopen(path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char *t = TrimInPlace(line);
        char *sep1, *sep2, *name, *cmd;

        if (t[0] == '\0' || t[0] == '#')
            continue;
        sep1 = strchr(t, '|');
        if (!sep1)
            continue;
        *sep1 = '\0';
        sep2 = strchr(sep1 + 1, '|');
        if (sep2)
            *sep2 = '\0';
        name = TrimInPlace(t);
        cmd = TrimInPlace(sep1 + 1);
        if (name[0] && cmd[0] && AddEntry(name, cmd, 1) < 0)
            break;
    }
    fclose(fp);
    g_conf_count = g_count;
}

static int
CmpName(const void *a, const void *b)
{
    const Entry *ea = a, *eb = b;
    int c = strcmp(g_pool + ea->name, g_pool + eb->name);

    /* przy rownych nazwach wczesniejszy wpis (wczesniejszy katalog w
     * $PATH) pierwszy - qsort nie jest stabilny, a dedup zostawia
     * pierwszy z grupy */
    return c ? c : (ea->name < eb->name ? -1 : ea->name > eb->name);
}

static void
ScanDir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char path[1024];
    struct stat st;

    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= NAME_LEN)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) == -1 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
            continue;
        if (AddEntry(de->d_name, NULL, 0) < 0)
            break;
    }
    closedir(d);
}

/* Pliki wykonywalne z $PATH za wpisami z center.conf, posortowane, bez
 * powtorzen nazw. Puste skladniki $PATH ("." w sh) pomijane. */
static void
ScanPath(void)
{
    const char *env = getenv("PATH");
    char buf[4096], *dir, *save = NULL;
    int i, out;

    snprintf(buf, sizeof(buf), "%s", env ? env : "/bin:/usr/bin");
    for (dir = strtok_r(buf, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
        ScanDir(dir);

    qsort(g_entries + g_conf_count, (size_t)(g_count - g_conf_count),
          sizeof(Entry), CmpName);
    out = g_conf_count;
    for (i = g_conf_count; i < g_count; i++) {
        if (out > g_conf_count && strcmp(EntryName(i), EntryName(out - 1)) == 0)
            continue;
        g_entries[out++] = g_entries[i];
    }
    g_count = out;
}

/* Rangi: 0 = center.conf (nazwa albo komenda), 1 = $PATH od poczatku
 * nazwy, 2 = $PATH w srodku nazwy. Lista = rangi po kolei, w kazdej
 * kolejnosc g_entries. */
static void
Filter(void)
{
    char raw[QUERY_LEN], q[QUERY_LEN], s[NAME_LEN * 2];
    int i, r;

    snprintf(g_filtered, sizeof(g_filtered), "%s", g_query);
    snprintf(raw, sizeof(raw), "%s", g_query);
    Utf8LowerFold(TrimInPlace(raw), q, sizeof(q));
    for (i = 0; i < g_count; i++) {
        const char *hit;

        g_rank[i] = 0xff;
        Utf8LowerFold(EntryName(i), s, sizeof(s));
        hit = strstr(s, q);
        if (g_entries[i].conf) {
            if (!hit) {
                Utf8LowerFold(EntryCmd(i), s, sizeof(s));
                hit = strstr(s, q);
            }
            if (hit)
                g_rank[i] = 0;
        } else if (hit) {
            g_rank[i] = hit == s ? 1 : 2;
        }
    }
    g_nmatch = 0;
    for (r = 0; r <= 2; r++)
        for (i = 0; i < g_count; i++)
            if (g_rank[i] == r)
                g_matches[g_nmatch++] = i;
    g_sel = g_nmatch ? 0 : -1;
    g_scroll = 0;
}

/* fork + setsid (zamkniecie terminala/sesji 7arun nie zabija programu,
 * jak w 7acenter.c) + sh -c. Sukces = apka konczy dzialanie. */
static void
Run(const char *cmd)
{
    pid_t pid;

    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (*cmd == '\0')
        return;
    pid = fork();
    if (pid == -1) {
        perror("7arun: fork");
        return;
    }
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    g_quit = 1;
}

static void
RunSelected(void)
{
    if (g_sel >= 0 && g_sel < g_nmatch)
        Run(EntryCmd(g_matches[g_sel]));
    else
        Run(g_query);
}

static void
ClampScroll(void)
{
    int max = g_nmatch - VISIBLE_ROWS;

    if (max < 0) max = 0;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
}

static void
MoveSel(int delta)
{
    if (g_nmatch == 0)
        return;
    g_sel += delta;
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= g_nmatch) g_sel = g_nmatch - 1;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + VISIBLE_ROWS) g_scroll = g_sel - VISIBLE_ROWS + 1;
}

/* ------------------------------------------------------------------ */
/* Rysowanie                                                           */
/* ------------------------------------------------------------------ */

static int
BoxHeightForRows(int n)
{
    return n * ROW_H + (n - 1) * ROW_GAP + 2 + 2 * BOX_PAD_V;
}

/* "Run:" + pole + licznik trafien; Filter() od razu po ui_textbox, wiec
 * lista ponizej w tej samej klatce juz pasuje do pola. */
static int
DrawQueryBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    static int label_w = 0, count_w = 0;
    UiBox *box;
    UiRect row, l, m, r;
    char count[32];

    if (label_w == 0) {
        label_w = ui_text_width(ctx, "Run:") + 4;
        count_w = ui_text_width(ctx, "00000/00000");
    }
    box = ui_box_begin(ctx, "query", 0, y, win_w, style);
    row = ui_box_next_rect(box, ROW_H);
    ui_rect_split3(row, label_w, count_w, COL_GAP, &l, &m, &r);

    ui_label(ctx, l, "Run:");
    ui_set_focus(ctx, g_query); /* pole ma fokus zawsze, nawet po kliku obok */
    ui_textbox(ctx, m, g_query, sizeof(g_query), &g_cursor);
    if (strcmp(g_query, g_filtered) != 0)
        Filter();
    snprintf(count, sizeof(count), "%d/%d", g_nmatch, g_count);
    ui_label_centered(ctx, r, count);

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "query") + style->margin_b;
}

static void
DrawListBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    UiBox *box;
    int i, mx, my;

    ClampScroll();
    /* klik = uruchomienie; geometria z poprzedniej klatki (stala) */
    for (i = 0; i < VISIBLE_ROWS && g_scroll + i < g_nmatch; i++) {
        UiRect r = { g_list_r.x, g_list_r.y + i * (ROW_H + ROW_GAP), g_list_r.w, ROW_H };

        if (ui_hit_test(ctx, r)) {
            g_sel = g_scroll + i;
            RunSelected();
        }
    }

    ui_mouse_state(ctx, &mx, &my, NULL);
    box = ui_box_begin(ctx, "list", 0, y, win_w, style);
    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = g_scroll + i, e;

        if (i == 0)
            g_list_r = (UiRect) { row.x, row.y, row.w,
                                  VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * ROW_GAP };
        if (idx >= g_nmatch)
            continue;
        e = g_matches[idx];
        if (idx == g_sel)
            ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
        else if (mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h)
            ui_draw_round_border(ctx, row, 1, ui_theme_accent(ctx));
        row.x += 4;
        row.w -= 8;
        if (g_entries[e].conf) {
            UiRect l, r;

            ui_rect_split3(row, row.w / 2, 0, COL_GAP, &l, &r, NULL);
            ui_label_ellipsis(ctx, l, EntryName(e));
            ui_label_ellipsis(ctx, r, EntryCmd(e));
        } else {
            ui_label_ellipsis(ctx, row, EntryName(e));
        }
    }
    ui_box_end(box);
}

static void
draw(UiCtx *ctx, int win_w)
{
    static UiBoxStyle style;
    static int style_ready = 0;
    int y;

    if (!style_ready) {
        style = (UiBoxStyle){0};
        style.margin_l     = ui_window_margin(ctx);
        style.margin_r     = ui_window_margin(ctx);
        style.margin_t     = V_MARGIN;
        style.margin_b     = V_MARGIN;
        style.padding_l    = ui_box_padding(ctx);
        style.padding_r    = ui_box_padding(ctx);
        style.padding_t    = BOX_PAD_V;
        style.padding_b    = BOX_PAD_V;
        style.gap          = ROW_GAP;
        style.bg_color     = *ui_theme_box_bg(ctx);
        style.border_w     = 1;
        style.border_color = *ui_theme_line_fg(ctx);
        style_ready = 1;
    }
    y = DrawQueryBox(ctx, &style, win_w, 0);
    DrawListBox(ctx, &style, win_w, y);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* Klawisze listy/apki - zwraca 1, jesli obsluzony (nie idzie do pola). */
static int
HandleKey(XKeyEvent *kev)
{
    int ctrl = (kev->state & ControlMask) != 0;
    int shift = (kev->state & ShiftMask) != 0;
    KeySym ks = XLookupKeysym(kev, 0);

    switch (ks) {
    case XK_Escape:
        g_quit = 1;
        return 1;
    case XK_Return: case XK_KP_Enter:
        if (ctrl || shift)
            Run(g_query);
        else
            RunSelected();
        return 1;
    case XK_Tab:
        if (g_sel >= 0 && g_sel < g_nmatch) {
            snprintf(g_query, sizeof(g_query), "%s", EntryName(g_matches[g_sel]));
            g_cursor = (int)strlen(g_query);
        }
        return 1;
    case XK_Down: MoveSel(1); return 1;
    case XK_Up:   MoveSel(-1); return 1;
    case XK_Next: MoveSel(VISIBLE_ROWS); return 1;
    case XK_Prior: MoveSel(-VISIBLE_ROWS); return 1;
    }
    if (!ctrl)
        return 0;
    switch (ks) {
    case XK_n: case XK_j: MoveSel(1); return 1;
    case XK_p: case XK_k: MoveSel(-1); return 1;
    case XK_u:
        g_query[0] = '\0';
        g_cursor = 0;
        return 1;
    }
    return 0;
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
    int win_w = WIN_W;
    int win_h = 4 * V_MARGIN + BoxHeightForRows(1) + BoxHeightForRows(VISIBLE_ROWS);
    int win_x, win_y;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int screen, redraw, i;
    char app_name[64] = "7aRun";
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
            fprintf(stderr, "Uzycie: 7arun [-geometry GEOM] [-name NAME] [-title TEXT]\n");
            return 1;
        }
    }

#ifdef __OpenBSD__
    /* rpath - center.conf i katalogi $PATH; proc exec - uruchomienie
     * wybranego programu. Bez unveil - wybrany program dziedziczylby
     * ograniczenia (jak w 7acenter.c). */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    LoadConf();
    ScanPath();
    g_rank = malloc(g_count > 0 ? (size_t)g_count : 1);
    g_matches = malloc((g_count > 0 ? (size_t)g_count : 1) * sizeof(int));
    if (!g_rank || !g_matches) {
        fprintf(stderr, "7arun: brak pamieci\n");
        return 1;
    }
    Filter();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7arun: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    /* domyslnie na srodku ekranu w poziomie, w gornej czesci w pionie */
    if (geom_mask & WidthValue) win_w = (int)geom_w;
    win_x = (DisplayWidth(dpy, screen) - win_w) / 2;
    win_y = DisplayHeight(dpy, screen) / 5;
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
        ch->res_class = "7aRun";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }
    {
        /* stala wysokosc (VISIBLE_ROWS wierszy), szerokosc dowolna;
         * PPosition, zeby WM uszanowal wyliczone polozenie */
        XSizeHints *sh = XAllocSizeHints();

        sh->flags      = PPosition | PMinSize | PMaxSize;
        sh->x          = win_x;
        sh->y          = win_y;
        sh->min_width  = 200;
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
        fprintf(stderr, "7arun: ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, terminal_48_width, terminal_48_height, terminal_48_bits);

    /* Dwie klatki przed petla - cache wysokosci boxow, patrz 7aping.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);

    redraw = 1;
    while (!g_quit) {
        XNextEvent(dpy, &ev);

        if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
            (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
            /* kolko przed ui_feed_event, wzorem 7askm.c */
            if (ev.type == ButtonPress) {
                g_scroll += ev.xbutton.button == Button4 ? -3 : 3;
                ClampScroll();
                redraw = 1;
            }
        } else if (ev.type == KeyPress && HandleKey(&ev.xkey)) {
            redraw = 1;
        } else {
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
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_del)
                    g_quit = 1;
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

        if (redraw && !g_quit && !XPending(dpy)) {
            ui_begin_frame(ctx);
            draw(ctx, win_w);
            ui_end_frame(ctx);
            redraw = 0;
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    free(g_matches);
    free(g_rank);
    free(g_entries);
    free(g_pool);
    return 0;
}
