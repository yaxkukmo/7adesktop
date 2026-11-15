/*
 * 7aroot.c - ustawianie tla pulpitu, nakladka na xsetroot(1).
 *
 * Nowa apka (nie port Xt/Xaw). Dwie zakladki ui_tabs:
 *   Bitmap - trzy listy obok siebie: bitmapa z katalogu bitmap X11
 *            (domyslnie /usr/X11R6/include/X11/bitmaps na OpenBSD,
 *            /usr/include/X11/bitmaps gdzie indziej, zasob X
 *            7aRoot.bitmapDir, zmienialny w polu "Directory:" nad boxem list -
 *            Enter/Load, "~/" = $HOME), kolor pierwszego planu i kolor tla
 *            -> "xsetroot -bitmap PLIK -fg KOLOR -bg KOLOR"
 *   Solid  - jedna lista kolorow -> "xsetroot -solid KOLOR"
 * Kolory to nazwy z showrgb(1) (baza rgb serwera X) - pomijane nazwy ze
 * spacjami ("ghost white" ma i tak wariant "GhostWhite"), wiec zapisana
 * komenda nie wymaga cytowania kolorow. Przy kazdej nazwie probka koloru
 * (XAllocColor dopiero przy pierwszym narysowaniu wiersza, nie dla
 * wszystkich ~700 nazw na starcie).
 *
 * Pod listami box informacyjny z podgladem: bitmapa kafelkowana od
 * lewego gornego rogu tak jak xsetroot kafelkuje roota (Pixmap budowana
 * tylko przy zmianie wyboru/rozmiaru - XReadBitmapFileData +
 * XCreatePixmapFromBitmapData + FillTiled - i kopiowana co klatke przez
 * ui_draw_pixmap). Pod boxem pole "Command:" (ui_textbox tylko do
 * odczytu) z wygenerowana komenda. "Approve" odpala xsetroot
 * (fork+execvp bez powloki), "Save" zapisuje komende do ~/.7a/background
 * (skrypt sh z #!/bin/sh, mkstemp + rename, tryb 0755 - do wywolania z
 * ~/.xsession). Na starcie ten plik jest czytany, zeby zaznaczyc
 * zapisane ustawienia.
 *
 * Listy przewijane kolkiem i scrollbarem ze strzalkami + kropkami
 * (przeniesione z utils/7arss.c). Liczba widocznych wierszy z wysokosci
 * okna. Klawiatura: Up/Down/j/k = wybor w ostatnio kliknietej liscie,
 * Tab = zakladka, Enter = Approve, Esc/q = wyjscie.
 *
 * Uzycie:
 *   7aroot [-geometry GEOM] [-name NAME] [-title TEXT]
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/palette_48.xbm"

#define ROW_H            20
#define SCROLLBAR_W      ROW_H
#define SCROLL_DOT_PITCH 14
#define SCROLL_DOT_R     4
#define COL_GAP          6
#define TAB_H            22
#define TAB_INDENT       8
#define V_MARGIN         6
#define BOX_PAD_V        4
#define BOX_GAP          4
#define PREVIEW_H        80
#define DIR_ROW_H        (V_MARGIN + ROW_H)   /* odstep + wiersz Directory */
/* naglowek + odstep boxa + wiersze */
#define LIST_BOX_H       (ROW_H + BOX_GAP + g_rows * ROW_H)
#define SWATCH_PAD       3
#define MIN_ROWS         3
#define MAX_ROWS         64
#define WIN_W            640
#define DEFAULT_ROWS     10

#define MAX_COLORS       1024
#define COLOR_NAME_LEN   32
#define MAX_BITMAPS      256
#define BITMAP_NAME_LEN  64

#ifdef __OpenBSD__
# define DEFAULT_BITMAP_DIR "/usr/X11R6/include/X11/bitmaps"
#else
# define DEFAULT_BITMAP_DIR "/usr/include/X11/bitmaps"
#endif

enum { TAB_BITMAP, TAB_SOLID, TAB_COUNT };
enum { L_BITMAP, L_FG, L_BG, L_SOLID, L_COUNT };

/* ------------------------------------------------------------------ */
/* Stan                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char   name[COLOR_NAME_LEN];
    XColor xc;
    int    alloc;   /* 0 = jeszcze nie probowano, 1 = ok, -1 = nieudane */
} Color;

/* Lista przewijana: sel = indeks pozycji (-1 brak), scroll = pierwszy
 * widoczny wiersz, r = wiersze z OSTATNIEJ klatki (bez scrollbara) do
 * kolka myszy i zaznaczania (wzorem g_list_r w 7arss.c). */
typedef struct {
    const char *title;
    int    sel, scroll;
    UiRect r;
} List;

static Color g_colors[MAX_COLORS];
static int   g_ncolors;
static char  g_bitmaps[MAX_BITMAPS][BITMAP_NAME_LEN];
static int   g_nbitmaps;
static char  g_bitmap_dir[512];
static char  g_dir_edit[512];   /* tresc pola "Directory:" */
static int   g_dir_cursor;

static List g_lists[L_COUNT] = {
    { "Bitmap", -1, 0, {0, 0, 0, 0} },
    { "Foreground", -1, 0, {0, 0, 0, 0} },
    { "Background", -1, 0, {0, 0, 0, 0} },
    { "Color", -1, 0, {0, 0, 0, 0} },
};
static int g_tab = TAB_BITMAP;
static int g_focus = L_BITMAP;   /* lista sterowana klawiatura */
static int g_rows = DEFAULT_ROWS;

static const char *g_tab_labels[TAB_COUNT] = { "Bitmap", "Solid" };
static char g_status[256];
static char g_save_path[512];

/* Podglad bitmapy: pixmapa o rozmiarze prostokata podgladu, wlasnosc
 * apki - XFreePixmap przy przebudowie i w FreePreview na koncu. g_pgc
 * (XCreateGC w main, XFreeGC na koncu) sluzy tylko do jej wypelniania,
 * bo ui.c nie udostepnia swojego GC. Klucz cache = wybor + rozmiar. */
static Display *g_dpy;
static Window   g_win;
static GC       g_pgc;
static Pixmap   g_preview = None;
static int      g_prev_key[5] = { -2, -2, -2, -1, -1 };
static int      g_prev_ok;

/* stderr dzieci na /dev/null - otwarte przed pledge, jak w 7avol.c */
static int g_devnull = -1;

/* ------------------------------------------------------------------ */
/* Dane (bez ui_*)                                                     */
/* ------------------------------------------------------------------ */

static int
FindColor(const char *name)
{
    int i;

    for (i = 0; i < g_ncolors; i++)
        if (strcasecmp(g_colors[i].name, name) == 0)
            return i;
    return -1;
}

static int
FindBitmap(const char *name)
{
    int i;

    for (i = 0; i < g_nbitmaps; i++)
        if (strcmp(g_bitmaps[i], name) == 0)
            return i;
    return -1;
}

/* Linie showrgb: "R G B<tab><tab>nazwa". Nazwy ze spacjami pomijane
 * (patrz naglowek), dokladne duplikaty tez. */
static void
LoadColors(void)
{
    char line[256];
    FILE *fp = popen("showrgb", "r");

    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp) && g_ncolors < MAX_COLORS) {
        int r, g, b, off = 0;
        char *name;
        size_t len;
        Color *c;

        if (sscanf(line, "%d %d %d %n", &r, &g, &b, &off) != 3 || off == 0)
            continue;
        name = line + off;
        len = strcspn(name, "\r\n");
        name[len] = '\0';
        if (len == 0 || len >= COLOR_NAME_LEN || strchr(name, ' ') ||
            name[0] == '-' || FindColor(name) >= 0)
            continue;
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
            continue;
        c = &g_colors[g_ncolors++];
        snprintf(c->name, sizeof(c->name), "%s", name);
        c->xc.red   = (unsigned short)(r * 257);
        c->xc.green = (unsigned short)(g * 257);
        c->xc.blue  = (unsigned short)(b * 257);
        c->xc.flags = DoRed | DoGreen | DoBlue;
        c->alloc = 0;
    }
    pclose(fp);
}

static int
CmpName(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Wczytuje (od nowa) liste bitmap z g_bitmap_dir; -1 gdy katalogu nie
 * da sie otworzyc (lista zostaje wtedy pusta). */
static int
LoadBitmaps(void)
{
    DIR *d = opendir(g_bitmap_dir);
    struct dirent *de;

    g_nbitmaps = 0;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL && g_nbitmaps < MAX_BITMAPS) {
        char path[1024];
        struct stat st;

        if (de->d_name[0] == '.' || strlen(de->d_name) >= BITMAP_NAME_LEN)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", g_bitmap_dir, de->d_name)
            >= (int)sizeof(path))
            continue;
        if (stat(path, &st) == -1 || !S_ISREG(st.st_mode))
            continue;
        snprintf(g_bitmaps[g_nbitmaps++], BITMAP_NAME_LEN, "%s", de->d_name);
    }
    closedir(d);
    qsort(g_bitmaps, (size_t)g_nbitmaps, BITMAP_NAME_LEN, CmpName);
    return 0;
}

/* Pole "Directory:" -> g_bitmap_dir: "~/" rozwijane do $HOME, koncowe
 * '/' obcinane. Wybor bitmapy wraca na "gray" (albo pierwsza), a klucz
 * podgladu jest uniewazniany, bo ten sam indeks to teraz inny plik. */
static void
ReloadBitmaps(void)
{
    const char *home = getenv("HOME");
    size_t len;
    int n;

    if (strncmp(g_dir_edit, "~/", 2) == 0 && home)
        n = snprintf(g_bitmap_dir, sizeof(g_bitmap_dir), "%s/%s", home, g_dir_edit + 2);
    else
        n = snprintf(g_bitmap_dir, sizeof(g_bitmap_dir), "%s", g_dir_edit);
    if (n < 0 || (size_t)n >= sizeof(g_bitmap_dir)) {
        g_bitmap_dir[0] = '\0';
        snprintf(g_status, sizeof(g_status), "Directory path too long");
    }
    len = strlen(g_bitmap_dir);
    while (len > 1 && g_bitmap_dir[len - 1] == '/')
        g_bitmap_dir[--len] = '\0';

    if (g_bitmap_dir[0] && LoadBitmaps() == -1)
        snprintf(g_status, sizeof(g_status), "Cannot open %s: %s",
                 g_bitmap_dir, strerror(errno));
    else if (g_bitmap_dir[0])
        snprintf(g_status, sizeof(g_status), "%d bitmaps in %s",
                 g_nbitmaps, g_bitmap_dir);
    else
        g_nbitmaps = 0;
    g_lists[L_BITMAP].sel = FindBitmap("gray") >= 0 ? FindBitmap("gray") :
                            (g_nbitmaps > 0 ? 0 : -1);
    g_lists[L_BITMAP].scroll = g_lists[L_BITMAP].sel >= g_rows ?
                               g_lists[L_BITMAP].sel - g_rows + 1 : 0;
    g_prev_key[0] = -2;
}

static int
BitmapPath(int i, char *out, size_t outsz)
{
    if (i < 0 || i >= g_nbitmaps)
        return -1;
    return snprintf(out, outsz, "%s/%s", g_bitmap_dir, g_bitmaps[i])
           >= (int)outsz ? -1 : 0;
}

static const char *
ColorName(int list)
{
    int i = g_lists[list].sel;

    return (i >= 0 && i < g_ncolors) ? g_colors[i].name : NULL;
}

/* argv dla xsetroot z biezacego wyboru; path to bufor na sciezke
 * bitmapy (musi zyc tak dlugo jak argv). Zwraca liczbe argumentow albo
 * 0, gdy wybor jest niekompletny. */
static int
BuildArgv(char *argv[8], char *path, size_t pathsz)
{
    if (g_tab == TAB_SOLID) {
        if (!ColorName(L_SOLID))
            return 0;
        argv[0] = "xsetroot";
        argv[1] = "-solid";
        argv[2] = (char *)ColorName(L_SOLID);
        argv[3] = NULL;
        return 3;
    }
    if (BitmapPath(g_lists[L_BITMAP].sel, path, pathsz) == -1 ||
        !ColorName(L_FG) || !ColorName(L_BG))
        return 0;
    argv[0] = "xsetroot";
    argv[1] = "-bitmap";
    argv[2] = path;
    argv[3] = "-fg";
    argv[4] = (char *)ColorName(L_FG);
    argv[5] = "-bg";
    argv[6] = (char *)ColorName(L_BG);
    argv[7] = NULL;
    return 7;
}

/* Argument do linii powloki: bez cytowania, jesli same bezpieczne znaki,
 * inaczej w '...' z ' zamienionym na '\'' . Zwraca -1 przy braku
 * miejsca. */
static int
AppendQuoted(char *out, size_t outsz, size_t *len, const char *arg)
{
    const char *p;
    int safe = arg[0] != '\0';

    for (p = arg; *p && safe; p++)
        safe = isalnum((unsigned char)*p) || strchr("/._-+", *p);
    if (*len > 0 && *len + 1 < outsz)
        out[(*len)++] = ' ';
    if (safe) {
        int n = snprintf(out + *len, outsz - *len, "%s", arg);

        if (n < 0 || (size_t)n >= outsz - *len)
            return -1;
        *len += (size_t)n;
        return 0;
    }
    if (*len + 1 >= outsz)
        return -1;
    out[(*len)++] = '\'';
    for (p = arg; *p; p++) {
        const char *piece = *p == '\'' ? "'\\''" : NULL;
        size_t plen = piece ? 4 : 1;

        if (*len + plen + 1 >= outsz)
            return -1;
        if (piece)
            memcpy(out + *len, piece, plen);
        else
            out[*len] = *p;
        *len += plen;
    }
    if (*len + 1 >= outsz)
        return -1;
    out[(*len)++] = '\'';
    out[*len] = '\0';
    return 0;
}

static int
CommandLine(char *out, size_t outsz)
{
    char *argv[8], path[1024];
    size_t len = 0;
    int i, n = BuildArgv(argv, path, sizeof(path));

    out[0] = '\0';
    if (n == 0)
        return -1;
    for (i = 0; i < n; i++)
        if (AppendQuoted(out, outsz, &len, argv[i]) == -1)
            return -1;
    return 0;
}

static void
Approve(void)
{
    char *argv[8], path[1024];
    pid_t pid;
    int status;

    if (BuildArgv(argv, path, sizeof(path)) == 0) {
        snprintf(g_status, sizeof(g_status), "Nothing selected");
        return;
    }
    pid = fork();
    if (pid == -1) {
        snprintf(g_status, sizeof(g_status), "fork: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        if (g_devnull != -1)
            dup2(g_devnull, STDERR_FILENO);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        snprintf(g_status, sizeof(g_status), "xsetroot failed");
    else
        snprintf(g_status, sizeof(g_status), "Background set");
}

/* ~/.7a/background jako skrypt sh: mkstemp w tym samym katalogu +
 * rename, zeby przerwany zapis nie zostawil polowy pliku. */
static void
Save(void)
{
    char cmd[1536], tmp[600], dir[512];
    char *slash;
    FILE *fp;
    int fd;

    if (CommandLine(cmd, sizeof(cmd)) == -1) {
        snprintf(g_status, sizeof(g_status), "Nothing selected");
        return;
    }
    snprintf(dir, sizeof(dir), "%s", g_save_path);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0755) == -1 && errno != EEXIST) {
            snprintf(g_status, sizeof(g_status), "mkdir %s: %s", dir, strerror(errno));
            return;
        }
    }
    if (snprintf(tmp, sizeof(tmp), "%s.XXXXXX", g_save_path) >= (int)sizeof(tmp) ||
        (fd = mkstemp(tmp)) == -1) {
        snprintf(g_status, sizeof(g_status), "Cannot write %s", g_save_path);
        return;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        snprintf(g_status, sizeof(g_status), "Cannot write %s", g_save_path);
        return;
    }
    fchmod(fd, 0755);
    fprintf(fp, "#!/bin/sh\n%s\n", cmd);
    if (fclose(fp) != 0 || rename(tmp, g_save_path) == -1) {
        unlink(tmp);
        snprintf(g_status, sizeof(g_status), "Cannot write %s", g_save_path);
        return;
    }
    snprintf(g_status, sizeof(g_status), "Saved to %s", g_save_path);
}

/* Odczyt zapisanej komendy - tylko proste tokeny (to, co pisze Save dla
 * nazw z showrgb i bitmap bez spacji); cudzyslowy obcinane. */
static void
LoadSaved(void)
{
    char line[1536], *tok, *prev = NULL;
    FILE *fp = fopen(g_save_path, "r");

    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, "xsetroot", 8) == 0)
            break;
    fclose(fp);
    if (strncmp(line, "xsetroot", 8) != 0)
        return;
    for (tok = strtok(line, " \t\r\n'"); tok; tok = strtok(NULL, " \t\r\n'")) {
        if (prev && strcmp(prev, "-solid") == 0) {
            g_lists[L_SOLID].sel = FindColor(tok);
            g_tab = TAB_SOLID;
        } else if (prev && strcmp(prev, "-bitmap") == 0) {
            char *base = strrchr(tok, '/');

            /* bitmapa z innego katalogu niz biezacy - wczytaj tamten */
            if (base && base != tok) {
                *base = '\0';
                if (strcmp(tok, g_bitmap_dir) != 0 &&
                    strlen(tok) < sizeof(g_bitmap_dir)) {
                    snprintf(g_bitmap_dir, sizeof(g_bitmap_dir), "%s", tok);
                    LoadBitmaps();
                }
            }
            g_lists[L_BITMAP].sel = FindBitmap(base ? base + 1 : tok);
            g_tab = TAB_BITMAP;
        } else if (prev && strcmp(prev, "-fg") == 0) {
            g_lists[L_FG].sel = FindColor(tok);
        } else if (prev && strcmp(prev, "-bg") == 0) {
            g_lists[L_BG].sel = FindColor(tok);
        }
        prev = tok;
    }
}

static void
DefaultSelection(void)
{
    if (g_lists[L_BITMAP].sel < 0)
        g_lists[L_BITMAP].sel = FindBitmap("gray") >= 0 ? FindBitmap("gray") :
                                (g_nbitmaps > 0 ? 0 : -1);
    if (g_lists[L_FG].sel < 0)
        g_lists[L_FG].sel = FindColor("black");
    if (g_lists[L_BG].sel < 0)
        g_lists[L_BG].sel = FindColor("white");
    if (g_lists[L_SOLID].sel < 0)
        g_lists[L_SOLID].sel = FindColor("SteelBlue");
}

static int
ListCount(int list)
{
    return list == L_BITMAP ? g_nbitmaps : g_ncolors;
}

static void
ClampTo(int *v, int max)
{
    if (*v > max) *v = max;
    if (*v < 0) *v = 0;
}

static int
MaxScroll(int list)
{
    int m = ListCount(list) - g_rows;

    return m < 0 ? 0 : m;
}

/* przewija tak, zeby zaznaczona pozycja byla widoczna */
static void
ScrollToSel(List *l)
{
    if (l->sel < 0)
        return;
    if (l->sel < l->scroll)
        l->scroll = l->sel;
    else if (l->sel >= l->scroll + g_rows)
        l->scroll = l->sel - g_rows + 1;
}

static void
MoveSel(int list, int delta)
{
    List *l = &g_lists[list];
    int n = ListCount(list);

    if (n == 0)
        return;
    l->sel = l->sel < 0 ? 0 : l->sel + delta;
    ClampTo(&l->sel, n - 1);
    ScrollToSel(l);
}

/* ------------------------------------------------------------------ */
/* Podglad                                                             */
/* ------------------------------------------------------------------ */

static const XColor *
ColorPixel(int i)
{
    Color *c;

    if (i < 0 || i >= g_ncolors)
        return NULL;
    c = &g_colors[i];
    /* wlasciciel: domyslna colormapa displaya, zwalniana przez
     * XCloseDisplay (jak kolory w 7aping.c) */
    if (c->alloc == 0)
        c->alloc = XAllocColor(g_dpy, DefaultColormap(g_dpy, DefaultScreen(g_dpy)),
                               &c->xc) ? 1 : -1;
    return c->alloc == 1 ? &c->xc : NULL;
}

static void
FreePreview(void)
{
    if (g_preview != None)
        XFreePixmap(g_dpy, g_preview);
    g_preview = None;
}

static int
BuildPreview(int w, int h)
{
    const XColor *fg = ColorPixel(g_lists[L_FG].sel);
    const XColor *bg = ColorPixel(g_lists[L_BG].sel);
    char path[1024];
    unsigned int bw, bh;
    unsigned char *data;
    int xh, yh, depth = DefaultDepth(g_dpy, DefaultScreen(g_dpy));
    Pixmap tile;

    FreePreview();
    if (!fg || !bg || BitmapPath(g_lists[L_BITMAP].sel, path, sizeof(path)) == -1)
        return 0;
    if (XReadBitmapFileData(path, &bw, &bh, &data, &xh, &yh) != BitmapSuccess)
        return 0;
    tile = XCreatePixmapFromBitmapData(g_dpy, g_win, (char *)data, bw, bh,
                                       fg->pixel, bg->pixel, (unsigned)depth);
    XFree(data);
    if (tile == None)
        return 0;
    g_preview = XCreatePixmap(g_dpy, g_win, (unsigned)w, (unsigned)h, (unsigned)depth);
    XSetTile(g_dpy, g_pgc, tile);
    XSetTSOrigin(g_dpy, g_pgc, 0, 0);
    XSetFillStyle(g_dpy, g_pgc, FillTiled);
    XFillRectangle(g_dpy, g_preview, g_pgc, 0, 0, (unsigned)w, (unsigned)h);
    XSetFillStyle(g_dpy, g_pgc, FillSolid);
    XFreePixmap(g_dpy, tile);
    return 1;
}

static void
DrawPreview(UiCtx *ctx, UiRect r)
{
    int key[5] = { g_lists[L_BITMAP].sel, g_lists[L_FG].sel,
                   g_lists[L_BG].sel, r.w, r.h };

    if (r.w <= 0 || r.h <= 0)
        return;
    if (g_tab == TAB_SOLID) {
        const XColor *c = ColorPixel(g_lists[L_SOLID].sel);

        if (c)
            ui_fill_rect(ctx, r, c);
        else
            ui_label_centered(ctx, r, "No color");
        return;
    }
    if (memcmp(key, g_prev_key, sizeof(key)) != 0) {
        g_prev_ok = BuildPreview(r.w, r.h);
        memcpy(g_prev_key, key, sizeof(key));
    }
    if (g_prev_ok)
        ui_draw_pixmap(ctx, r, g_preview);
    else
        ui_label_centered(ctx, r, "Cannot read bitmap");
}

/* ------------------------------------------------------------------ */
/* Rysowanie list                                                      */
/* ------------------------------------------------------------------ */

static int
InRect(UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Kropki stron scrollbara - z DrawScrollDots w utils/7arss.c. */
static int
DotsCurrent(int scroll, int max, int page, int pages)
{
    if (scroll >= max) return pages - 1;
    return (scroll + page / 2) / page;
}

static int
DotsStart(int visible, int pages, int current)
{
    int start = current - visible / 2;

    if (start > pages - visible) start = pages - visible;
    if (start < 0) start = 0;
    return start;
}

static void
DrawScrollDots(UiCtx *ctx, UiRect track, int *scroll, int max, int page)
{
    int pages = 1 + (max + page - 1) / page;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = DotsStart(visible, pages, DotsCurrent(*scroll, max, page, pages));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            *scroll = (start + i) * page;
            ClampTo(scroll, max);
            break;
        }
    }

    current = DotsCurrent(*scroll, max, page, pages);
    start = DotsStart(visible, pages, current);
    for (i = 0; i < visible; i++) {
        int cx = track.x + track.w / 2;
        int cy = y0 + i * SCROLL_DOT_PITCH + SCROLL_DOT_PITCH / 2;
        int more = (i == 0 && start > 0) ||
                   (i == visible - 1 && start + visible < pages);
        int rad = more ? SCROLL_DOT_R / 2 : SCROLL_DOT_R;

        if (start + i == current)
            ui_fill_circle(ctx, cx, cy, rad, ui_theme_fg(ctx));
        else
            ui_draw_circle(ctx, cx, cy, rad, 1, ui_theme_fg(ctx));
    }
}

/* Strzalki rysowane PO ui_button (inaczej tlo przycisku je zakryje). */
static void
DrawScrollbar(UiCtx *ctx, UiRect sb, int *scroll, int max, int page)
{
    UiRect up_r = { sb.x, sb.y, sb.w, ROW_H };
    UiRect down_r = { sb.x, sb.y + sb.h - ROW_H, sb.w, ROW_H };
    UiRect track_r = { sb.x, sb.y + ROW_H, sb.w, sb.h - 2 * ROW_H };
    int up = ui_button(ctx, up_r, "");
    int down = ui_button(ctx, down_r, "");

    ui_draw_arrow(ctx, up_r, UI_ARROW_UP);
    ui_draw_arrow(ctx, down_r, UI_ARROW_DOWN);
    if (up) (*scroll)--;
    if (down) (*scroll)++;
    ClampTo(scroll, max);
    if (track_r.h > 0)
        DrawScrollDots(ctx, track_r, scroll, max, page);
}

/* Klik w wiersz zaznacza pozycje - PRZED rysowaniem (wzorzec "najpierw
 * stan"), geometria z l->r poprzedniej klatki. */
static void
UpdateListSel(UiCtx *ctx, int list)
{
    List *l = &g_lists[list];
    int i, n = ListCount(list);

    for (i = 0; i < g_rows && l->scroll + i < n; i++) {
        UiRect row = { l->r.x, l->r.y + i * ROW_H, l->r.w, ROW_H };

        if (ui_hit_test(ctx, row)) {
            l->sel = l->scroll + i;
            g_focus = list;
        }
    }
}

/* Pasek naglowkow kolumn (wzorem utils/7atube.c): pierwszy wiersz boxa
 * rozciagniety do ramki, n kolumn jak ui_rect_col list ponizej - granica
 * komorek w polowie odstepu miedzy kolumnami, sasiednie komorki
 * zachodza na siebie o 1px, zeby ramki zlaly sie w jedna linie.
 * Naglowek obejmuje tez scrollbar kolumny. */
static void
DrawHeader(UiCtx *ctx, UiBox *box, UiRect hdr, int first, int n)
{
    UiRect strip = ui_box_header_strip(box, hdr);
    int i;

    for (i = 0; i < n; i++) {
        UiRect col = ui_rect_col(hdr, i, n, 2 * COL_GAP);
        int x0 = i == 0 ? strip.x : col.x - COL_GAP - 1;
        int x1 = i == n - 1 ? strip.x + strip.w
                            : ui_rect_col(hdr, i + 1, n, 2 * COL_GAP).x - COL_GAP;

        ui_header_cell(ctx, (UiRect){ x0, strip.y, x1 - x0, strip.h },
                       col.x + UI_CELL_PAD, g_lists[first + i].title);
    }
}

/* Kolumna: g_rows wierszy + scrollbar po prawej (naglowek osobno,
 * DrawHeader). Zaznaczony/pod myszka wiersz = accent. */
static void
DrawList(UiCtx *ctx, UiRect col, int list)
{
    List *l = &g_lists[list];
    UiRect sb = { col.x + col.w - SCROLLBAR_W, col.y,
                  SCROLLBAR_W, g_rows * ROW_H };
    int n = ListCount(list), mx, my, i;

    UpdateListSel(ctx, list);

    ClampTo(&l->scroll, MaxScroll(list));
    DrawScrollbar(ctx, sb, &l->scroll, MaxScroll(list), g_rows);
    l->r = (UiRect){ col.x, col.y, col.w - SCROLLBAR_W - COL_GAP, g_rows * ROW_H };

    if (n == 0) {
        ui_label(ctx, (UiRect){ l->r.x + UI_CELL_PAD, l->r.y, l->r.w, ROW_H },
                 list == L_BITMAP ? "No bitmaps" : "No colors (showrgb?)");
        return;
    }
    ui_mouse_state(ctx, &mx, &my, NULL);
    for (i = 0; i < g_rows && l->scroll + i < n; i++) {
        int idx = l->scroll + i;
        UiRect row = { l->r.x, l->r.y + i * ROW_H, l->r.w, ROW_H };
        UiRect text = { row.x + UI_CELL_PAD, row.y, row.w - 2 * UI_CELL_PAD, row.h };

        if (idx == l->sel || InRect(row, mx, my))
            ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
        if (list != L_BITMAP) {
            UiRect sw = { text.x, row.y + SWATCH_PAD, ROW_H - 2 * SWATCH_PAD,
                          ROW_H - 2 * SWATCH_PAD };
            const XColor *c = ColorPixel(idx);

            if (c)
                ui_fill_rect(ctx, sw, c);
            ui_draw_border(ctx, sw, 1, ui_theme_line_fg(ctx));
            text.x += sw.w + COL_GAP;
            text.w -= sw.w + COL_GAP;
            ui_label_ellipsis(ctx, text, g_colors[idx].name);
        } else {
            ui_label_ellipsis(ctx, text, g_bitmaps[idx]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Klatka                                                              */
/* ------------------------------------------------------------------ */

/* Sama gorna krawedz rowka pod zakladkami - DrawTopLine z 7arss.c. */
static void
DrawTopLine(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    int bw = style->border_w;
    int x0 = style->margin_l, w = win_w - style->margin_l - style->margin_r;
    UiRect outer = { x0 - 4 * ROW_H, y, w + 8 * ROW_H, 4 * ROW_H };
    UiRect in = { outer.x + bw, outer.y + bw, outer.w - 2 * bw, outer.h - 2 * bw };

    if (bw <= 0) return;
    ui_set_clip(ctx, (UiRect){ x0, y, w, 2 * bw + 1 });
    ui_draw_bevel(ctx, outer, bw, &style->border_color, 1);
    ui_draw_bevel(ctx, in, bw, &style->border_color, 0);
    ui_draw_inner_bevel(ctx, in, bw, &style->border_color, ui_theme_bg(ctx), 0);
    ui_clear_clip(ctx);
}

/* Pionowe rozmiary sa stale (nie zaleza od motywu), wiec main() moze z
 * nich policzyc wysokosc okna przed ui_init. */
static int
BoxOuter(int content_h)
{
    return 2 * V_MARGIN + 2 + 2 * BOX_PAD_V + content_h;
}

static int
FixedHeight(void)
{
    return V_MARGIN + TAB_H + 3 +
           DIR_ROW_H +                                      /* katalog nad boxem */
           BoxOuter(LIST_BOX_H - g_rows * ROW_H) +          /* lista bez wierszy */
           BoxOuter(PREVIEW_H) +                            /* podglad */
           ROW_H + V_MARGIN +                               /* komenda */
           ROW_H + V_MARGIN;                                /* przyciski */
}

static int
RowsForHeight(int win_h)
{
    int rows = (win_h - FixedHeight()) / ROW_H;

    return rows < MIN_ROWS ? MIN_ROWS : (rows > MAX_ROWS ? MAX_ROWS : rows);
}

/* "Directory:" + pole + "Load" miedzy zakladkami a boxem list (poza
 * boxem), tylko w zakladce Bitmap; Enter w polu = Load. W zakladce Solid
 * miejsce zostaje puste, zeby boxy nie skakaly przy zmianie zakladki. */
static void
DrawDirRow(UiCtx *ctx, UiRect row)
{
    static int label_w = 0, load_w = 0;
    UiRect l, m, r;
    int load;

    if (g_tab != TAB_BITMAP)
        return;
    if (label_w == 0) {
        label_w = ui_text_width(ctx, "Directory:") + 4;
        load_w = ui_button_width(ctx, "Load");
    }
    ui_rect_split3(row, label_w, load_w, COL_GAP, &l, &m, &r);
    ui_label(ctx, l, "Directory:");
    ui_textbox(ctx, m, g_dir_edit, sizeof(g_dir_edit), &g_dir_cursor);
    load = ui_textbox_key(ctx, g_dir_edit, XK_Return) |
           ui_textbox_key(ctx, g_dir_edit, XK_KP_Enter);
    if (ui_button(ctx, r, "Load") || load)
        ReloadBitmaps();
}

/* "Command:" + pole z wygenerowana komenda pod boxem podgladu. Pole
 * tylko do odczytu: bufor przepisywany co klatke z CommandLine, a fokus
 * zdejmowany od razu po kliknieciu, wiec klawisze nie trafiaja do pola. */
static void
DrawCmdRow(UiCtx *ctx, UiRect row)
{
    static int label_w = 0, cursor = 0;
    static char cmd[1536];
    UiRect l, m;

    if (label_w == 0)
        label_w = ui_text_width(ctx, "Command:") + 4;
    if (CommandLine(cmd, sizeof(cmd)) == -1)
        snprintf(cmd, sizeof(cmd), "(select a bitmap and both colors)");
    cursor = 0;
    l = (UiRect){ row.x, row.y, label_w, row.h };
    m = (UiRect){ row.x + label_w + COL_GAP, row.y, row.w - label_w - COL_GAP, row.h };
    ui_label(ctx, l, "Command:");
    ui_textbox(ctx, m, cmd, sizeof(cmd), &cursor);
    if (ui_has_focus(ctx, cmd))
        ui_set_focus(ctx, NULL);
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle line_style, box_style, info_style;
    static int ready = 0;
    UiRect tabs_r, hdr, area, btn;
    UiBox *box;
    int i, y, m = ui_window_margin(ctx), bw_a, bw_s, quit = 0;

    if (!ready) {
        line_style = (UiBoxStyle){0};
        line_style.margin_l = line_style.margin_r = m;
        line_style.margin_t = line_style.margin_b = V_MARGIN;
        line_style.padding_l = line_style.padding_r = ui_box_padding(ctx);
        line_style.padding_t = line_style.padding_b = BOX_PAD_V;
        line_style.border_w = 1;
        line_style.gap = BOX_GAP;
        line_style.border_color = *ui_theme_line_fg(ctx);
        line_style.bg_color = *ui_theme_box_bg(ctx);
        line_style.transparent = 1;
        box_style = line_style;
        box_style.transparent = 0;
        info_style = box_style;
        info_style.bg_color = *ui_theme_box_info_bg(ctx);
        ready = 1;
    }
    g_rows = RowsForHeight(win_h);

    y = TAB_H + V_MARGIN;
    DrawTopLine(ctx, &line_style, win_w, y);
    tabs_r = (UiRect){ m + TAB_INDENT, y - TAB_H, win_w - 2 * m - 2 * TAB_INDENT, TAB_H };
    if (ui_tabs(ctx, tabs_r, g_tab_labels, TAB_COUNT, &g_tab, &line_style))
        g_focus = g_tab == TAB_SOLID ? L_SOLID : L_BITMAP;
    y += 3;

    DrawDirRow(ctx, (UiRect){ m, y + V_MARGIN, win_w - 2 * m, ROW_H });
    y += DIR_ROW_H;

    box = ui_box_begin(ctx, "lists", 0, y, win_w, &box_style);
    hdr = ui_box_next_rect(box, ROW_H);
    area = ui_box_next_rect(box, g_rows * ROW_H);
    if (g_tab == TAB_SOLID) {
        DrawHeader(ctx, box, hdr, L_SOLID, 1);
        DrawList(ctx, area, L_SOLID);
    } else {
        DrawHeader(ctx, box, hdr, L_BITMAP, 3);
        for (i = 0; i < 3; i++)
            DrawList(ctx, ui_rect_col(area, i, 3, 2 * COL_GAP), L_BITMAP + i);
    }
    ui_box_end(box);
    y += BoxOuter(LIST_BOX_H);

    box = ui_box_begin(ctx, "preview", 0, y, win_w, &info_style);
    DrawPreview(ctx, ui_box_next_rect(box, PREVIEW_H));
    ui_box_end(box);
    y += BoxOuter(PREVIEW_H);

    DrawCmdRow(ctx, (UiRect){ m, y, win_w - 2 * m, ROW_H });
    y += ROW_H + V_MARGIN;

    /* przyciski pod boxami, z prawej; status po lewej */
    bw_a = ui_button_width(ctx, "Approve");
    bw_s = ui_button_width(ctx, "Save");
    btn = (UiRect){ win_w - m - bw_s, y, bw_s, ROW_H };
    if (ui_button(ctx, btn, "Save"))
        Save();
    btn = (UiRect){ btn.x - COL_GAP - bw_a, y, bw_a, ROW_H };
    if (ui_button(ctx, btn, "Approve"))
        Approve();
    ui_label_ellipsis(ctx, (UiRect){ m, y, btn.x - COL_GAP - m, ROW_H }, g_status);
    return quit;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* Zwraca 0, gdy apka ma sie zamknac. */
static int
HandleKey(XKeyEvent *kev)
{
    char keybuf[16];
    KeySym ks;

    XLookupString(kev, keybuf, sizeof(keybuf), &ks, NULL);
    switch (ks) {
    case XK_Escape: case XK_q:
        return 0;
    case XK_Up: case XK_k:
        MoveSel(g_focus, -1);
        break;
    case XK_Down: case XK_j:
        MoveSel(g_focus, 1);
        break;
    case XK_Prior:
        MoveSel(g_focus, -g_rows);
        break;
    case XK_Next:
        MoveSel(g_focus, g_rows);
        break;
    case XK_Tab: case XK_ISO_Left_Tab:
        g_tab = g_tab == TAB_SOLID ? TAB_BITMAP : TAB_SOLID;
        g_focus = g_tab == TAB_SOLID ? L_SOLID : L_BITMAP;
        break;
    case XK_Left: case XK_h:
        if (g_tab == TAB_BITMAP && g_focus > L_BITMAP)
            g_focus--;
        break;
    case XK_Right: case XK_l:
        if (g_tab == TAB_BITMAP && g_focus < L_BG)
            g_focus++;
        break;
    case XK_Return: case XK_KP_Enter:
        Approve();
        break;
    }
    return 1;
}

/* kolko nad lista widoczna w aktywnej zakladce przewija ja o 3 wiersze */
static int
HandleWheel(XButtonEvent *bev)
{
    int i;

    for (i = 0; i < L_COUNT; i++) {
        List *l = &g_lists[i];
        UiRect area = { l->r.x, l->r.y, l->r.w + COL_GAP + SCROLLBAR_W, l->r.h };

        if ((i == L_SOLID) != (g_tab == TAB_SOLID) || !InRect(area, bev->x, bev->y))
            continue;
        l->scroll += bev->button == Button4 ? -3 : 3;
        ClampTo(&l->scroll, MaxScroll(i));
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
    int win_w = WIN_W, win_h = FixedHeight() + DEFAULT_ROWS * ROW_H;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int screen, running, redraw, i;
    char app_name[64] = "7aRoot";
    char app_title[64] = "";
    const char *home = getenv("HOME");

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[++i], &geom_x, &geom_y, &geom_w, &geom_h);
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[++i]);
        } else {
            fprintf(stderr, "Uzycie: 7aroot [-geometry GEOM] [-name NAME] [-title TEXT]\n");
            return 1;
        }
    }
    if (!home || !*home) {
        fprintf(stderr, "7aroot: brak $HOME\n");
        return 1;
    }
    if (snprintf(g_save_path, sizeof(g_save_path), "%s/.7a/background", home)
        >= (int)sizeof(g_save_path)) {
        fprintf(stderr, "7aroot: za dluga sciezka $HOME\n");
        return 1;
    }

    g_devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);

#ifdef __OpenBSD__
    /* proc exec - showrgb/xsetroot; wpath cpath fattr - zapis
     * ~/.7a/background (mkstemp, rename, fchmod). Bez unveil - dzieci
     * dziedziczylyby ograniczenia (jak w 7aweather.c). */
    if (pledge("stdio rpath wpath cpath fattr proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7aroot: brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    ui_resource_string(dpy, "7aRoot.bitmapDir", "7aRoot.BitmapDir",
                       g_bitmap_dir, sizeof(g_bitmap_dir), DEFAULT_BITMAP_DIR);
    LoadColors();
    LoadBitmaps();
    LoadSaved();
    DefaultSelection();
    snprintf(g_dir_edit, sizeof(g_dir_edit), "%s", g_bitmap_dir);
    g_dir_cursor = (int)strlen(g_dir_edit);
    for (i = 0; i < L_COUNT; i++)
        ScrollToSel(&g_lists[i]);
    g_focus = g_tab == TAB_SOLID ? L_SOLID : L_BITMAP;

    if (geom_mask & WidthValue) win_w = (int)geom_w;
    if (geom_mask & HeightValue) win_h = (int)geom_h;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative) ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative) ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;
    g_rows = RowsForHeight(win_h);

    win = XCreateSimpleWindow(dpy, root, win_x, win_y, (unsigned)win_w, (unsigned)win_h, 0,
                              BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();

        ch->res_name  = app_name;
        ch->res_class = "7aRoot";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }
    {
        XSizeHints *sh = XAllocSizeHints();

        sh->flags      = PMinSize;
        sh->min_width  = 420;
        sh->min_height = FixedHeight() + MIN_ROWS * ROW_H;
        XSetWMNormalHints(dpy, win, sh);
        XFree(sh);
    }
    wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    g_pgc = XCreateGC(dpy, win, 0, NULL);
    g_dpy = dpy;
    g_win = win;
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1",
                  win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7aroot: ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, g_pgc);
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, palette_48_width, palette_48_height, palette_48_bits);

    /* Dwie klatki przed petla - cache wysokosci boxa, patrz 7aping.c. */
    for (i = 0; i < 2; i++) {
        ui_begin_frame(ctx);
        draw(ctx, win_w, win_h);
        ui_end_frame(ctx);
    }

    running = 1;
    redraw = 1;
    while (running) {
        XNextEvent(dpy, &ev);

        /* kolko przechwycone PRZED ui_feed_event (ui.c nie rozroznia
         * numeru przycisku, wzorem 7arss.c) */
        if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
            (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
            if (ev.type == ButtonPress && HandleWheel(&ev.xbutton))
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
                redraw = 1;
                break;
            case KeyPress:
                /* klawisze nalezace do pola "Directory:" nie sa skrotami;
                 * Esc tylko zdejmuje z niego fokus */
                if (ui_has_focus(ctx, g_dir_edit)) {
                    if (XLookupKeysym(&ev.xkey, 0) == XK_Escape)
                        ui_set_focus(ctx, NULL);
                } else if (!HandleKey(&ev.xkey)) {
                    running = 0;
                }
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

        if (running && redraw) {
            ui_begin_frame(ctx);
            draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            redraw = 0;
        }
    }

    FreePreview();
    ui_destroy(ctx);
    XFreeGC(dpy, g_pgc);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
