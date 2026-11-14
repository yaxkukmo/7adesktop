/*
 * 7acenter.c - launcher programow, NOWA apka (nie port Xt/Xaw - w
 * przeciwienstwie do wiekszosci utils/7a*.c, patrz np. naglowek
 * utils/7afm.c). Architektura siatki ikon + scrollbara jest jednak
 * wprost oparta o utils/7afm.c: ta sama geometria kolumn/wierszy,
 * strzalki scrollbara (zamiast kciuka - kropki stron jak w
 * utils/7atodo.c, patrz DrawScrollDots) i to samo
 * rozroznienie klik=zaznaczenie / dwuklik=aktywacja (DOUBLE_CLICK_MS).
 *
 * Swiadomie POMINIETE wzgledem 7afm.c (bo tu nie ma zastosowania):
 *  - klasyfikacja plikow (KIND_*) - kazda pozycja to jeden zdefiniowany
 *    w configu program, bez rozroznienia typu;
 *  - XDND i schowek Copy/Cut/Paste - nie ma czego przeciagac/wklejac
 *    miedzy oknami launchera;
 *  - rename/delete plikow i pasek menu File/Edit/View - launcher tylko
 *    uruchamia programy, nie zarzadza plikami.
 *
 * Zrodlo listy programow: ~/.7a/center.conf, podzielony na sekcje-
 * kategorie: linia "[Nazwa]" zaczyna zakladke (max MAX_TABS), kolejne
 * linie "Nazwa|komenda argumenty[|ikona]" naleza do niej (# = komentarz,
 * puste linie ignorowane). Pozycje przed pierwsza sekcja (stary format
 * bez sekcji) trafiaja do zakladki "Main". Patrz center.conf.sample.
 *
 * Uklad to pionowy stos UiBoxow pod zakladkami (ui_tabs, jedna na
 * kategorie, "+" na poczatku ich rzedu) stojacymi na samej linii (gorna
 * krawedz rowka boxa, bez bokow i dolu - nie pusty box), w trybie Edit (olowek w kolumnie przyciskow na lewo od siatki) box z
 * formularzem Name/Command/Icon pozycji aktywnej zakladki, po "+" box
 * zakladek (nazwa + Add/Rename/Delete tab), na koncu box
 * informacyjny z siatka ikon i na dole box "dock" z ulubionymi ikonami
 * (jeden rzad, sekcja [@Dock] w center.conf - nie jest zakladka; wzor:
 * Icon Catalog z IRIX Indigo Magic). Ikone ze strony przeciaga sie do
 * docka (kopia; duplikat albo pelny rzad - odmowa z komunikatem), w
 * docku przeciaganie zmienia kolejnosc, a upuszczenie poza nim usuwa
 * ikone. Pojedynczy klik ikony docka uruchamia program. Wstawianie w
 * DockInsert - do ponownego uzycia przy przyszlym XDND z 7afm.
 * Konfigurator: dodanie
 * ikony/zakladki, edycja/usuniecie zaznaczonej ikony, zmiana nazwy/
 * usuniecie aktywnej zakladki. Kazda zmiana od razu przepisuje caly
 * center.conf (mkstemp w ~/.7a + rename, wiec przerwany zapis nie
 * zostawia polowy pliku) - komentarze z recznie pisanego pliku przy tym
 * NIE przetrwaja. Plik NIE jest tworzony, dopoki uzytkownik czegos nie
 * doda - apka nie pisze na dysk bez wyraznej akcji uzytkownika.
 *
 * Ikony: proba wyciagniecia PRAWDZIWEJ ikony uruchamianego programu przez
 * libXpm (jedyny format obrazkowy, jaki tu wczytujemy - bez PNG/SVG, wiec
 * bez ciezszej zaleznosci typu Imlib2/librsvg, patrz CLAUDE.md o minimalnym
 * zuzyciu pamieci/KISS). Domyslnie nazwa ikony to basename pierwszego
 * tokenu z pola "komenda" configu (patrz FirstCommandToken) - szukana jako
 * "<nazwa>.xpm" w kilku typowych katalogach ikon (kIconDirs, w tym
 * /usr/X11R6/include/X11/pixmaps - klasyczny katalog ikon Xaw/Xt na
 * OpenBSD). Wspolczesne apki GTK/Qt czesto NIE instaluja juz .xpm (tylko
 * PNG/SVG w motywie ikon), a apki z TEGO projektu (7afm i in.) w ogole nie
 * maja zadnej ikony na dysku - stad DWA sposoby na reczne dostrojenie,
 * zeby uzytkownik nie byl zdany wylacznie na automatyczne zgadywanie:
 *  - trzecie, OPCJONALNE pole w linii configu, "Nazwa|komenda|ikona"
 *    (patrz LoadEntries/center.conf.sample) - "ikona" to albo bezwzgledna
 *    sciezka do pliku .xpm (zaczyna sie od "/"), albo wlasna nazwa bazowa
 *    do przeszukania tych samych katalogow zamiast zgadywania z komendy;
 *  - zasob X "7aCenter.iconPath" (lista katalogow rozdzielona ":", jak
 *    $PATH) - dopisuje WLASNE katalogi z ikonami PRZED wbudowana lista
 *    kIconDirs, patrz Xresources.sample.
 * Gdy nic nie znaleziono (albo znaleziony plik jest podejrzanie duzy/
 * uszkodzony), fallback na PROCEDURALNA ikone (kolorowy kwadrat +
 * inicjal, kolor = hash nazwy nad mala stala paleta - deterministyczny,
 * ten sam program zawsze dostaje ten sam kolor). Znalezione .xpm sa
 * doskalowywane najblizszym sasiadem (ComposeIconPixmap) do ICON_SIZE x
 * ICON_SIZE, zeby siatka miala rowne komorki niezaleznie od rozmiaru
 * zrodlowego pliku - bez biblioteki skalujacej, czystym Xlib (XGetImage/
 * XPutImage). "Przezroczyste" piksele (maska ksztaltu z pliku .xpm, jesli
 * istnieje) dostaja kolor tla siatki (ui_theme_box_info_bg) zamiast koloru z
 * pliku - wypiekane RAZ przy ladowaniu, bo ui_draw_pixmap w ui.h/ui.c
 * nadal nie zna pojecia prawdziwej maski/przezroczystosci (patrz komentarz
 * przy ComposeIconPixmap po pelne uzasadnienie i znany kompromis przy
 * zaznaczonej komorce).
 */

#define _DEFAULT_SOURCE  /* execvp/fork - patrz ta sama uwaga w utils/7aweather.c */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include "../ui.h"
#include "icons/apps_48.xbm"

#define ICON_SIZE 32
#define CELL_W 80
#define CELL_H 64
#define CELL_GAP 6
#define CELL_VPAD 4
#define LIST_ICON 16      /* widok listy: ikona w wierszu (rozmiar mini-*.xpm) */
#define LIST_ROW_H 24
#define LIST_GAP 2
#define LIST_NAME_W 160   /* kolumna nazwy w widoku listy, reszta = komenda */
#define VIEW_ICON 12      /* rysunek na przyciskach Grid/List */
#define SCROLLBAR_W ROW_H  /* przyciski strzalek jak </> w 7atodo (ROW_H x ROW_H) */
#define SB_PAD 4  /* odstep scrollbara od obrysu viewportu i od siatki */
#define SCROLL_DOT_PITCH 14  /* wysokosc komorki jednej kropki na torze scrollbara */
#define SCROLL_DOT_R 4
#define ROW_H 20
#define MARGIN_Y 8
#define DOUBLE_CLICK_MS 400      /* brak Xt -> brak XtGetMultiClickTime, patrz ta sama
                                    uwaga przy tej stalej w utils/7afm.c */
#define WHEEL_STEP g_row_pitch  /* jeden wiersz siatki albo listy */
#define MAX_CMD_TOKENS 32
#define TAB_H 22
#define TAB_INDENT 8      /* odsuniecie zakladek od zaokraglonego rogu viewportu */
#define MAX_TABS 10
#define TAB_NAME_SZ 32
#define FORM_LABEL_W 72
#define FORM_GAP 6
#define SEARCH_W 160         /* pole wyszukiwarki w pasku statusu, po prawej */
#define FORM_ROWS 4       /* Name, Command, Icon + rzad przyciskow */
#define TAB_FORM_ROWS 2   /* nazwa zakladki + rzad przyciskow */
#define BOX_MARGIN_TB 6   /* pionowy odstep boxow (margin_t/margin_b) */
#define BOX_PAD_TB 4
#define DOCK_MAX 16       /* gorny limit ikon w docku (faktyczny - ile sie miesci w wierszu) */
#define DOCK_SECTION "@Dock"  /* sekcja docka w center.conf - '@' niedozwolone w nazwach zakladek */
#define DOCK_TAB (-3)     /* "zakladka" pozycji docka w trakcie LoadEntries */
#define DRAG_THRESHOLD 4  /* px ruchu z wcisnietym LPM, zanim klik stanie sie przeciaganiem */
#define PALETTE_SIZE 8
#define ICON_MAX_SRC 512  /* sanity cap na szerokosc/wysokosc zrodlowego .xpm -
                              obrona przed patologicznie duzym/uszkodzonym
                              plikiem, ten sam duch co ostroznosc przy polach
                              XEvent z CLAUDE.md, tylko dla danych z dysku */
#define MAX_EXTRA_ICON_DIRS 8  /* limit tokenow z zasobu X "7aCenter.iconPath" */

typedef struct {
    char name[64];
    char command[256];
    char icon_override[256];  /* "" = brak, wg pola "ikona" z configu - patrz naglowek */
    int tab;                   /* indeks w g_tabs */
    Pixmap icon;               /* None, jesli has_icon == 0 */
    int has_icon;
    Pixmap mini;               /* LIST_ICON x LIST_ICON z mini-*.xpm obok large-*.xpm,
                                  None = lista skaluje icon */
} LauncherEntry;

static LauncherEntry *entries = NULL;
static int entry_count = 0;
static int entry_cap = 0;

/* zakladki-kategorie: stala tablica, bez malloc (max MAX_TABS) */
static char g_tabs[MAX_TABS][TAB_NAME_SZ];
static int g_tab_count = 0;
static int g_active_tab = 0;
static unsigned char g_tab_list[MAX_TABS];  /* 1 = zakladka w widoku listy, 0 = siatka (nie zapisywane) */

/* stan formularza konfiguratora (tryb "Edit") - bufory ui_textbox */
typedef struct {
    char name[64];
    char command[256];
    char icon[256];
    int name_cur, command_cur, icon_cur;
    int index;  /* edytowana pozycja w entries albo -1 = nowa */
} EditForm;

static EditForm g_form;
static int g_edit_mode = 0;

/* formularz zakladek (przycisk "+" przy zakladkach) - osobny od pozycji */
static char g_tab_name[TAB_NAME_SZ];
static int g_tab_name_cur = 0;
static int g_tab_mode = 0;
static int g_need_redraw = 0;  /* akcja w draw() zmienila stan - narysuj jeszcze raz */

/* Potrzebne do zaladowania ikon XPM (XpmReadFileToPixmap) z LoadEntries()
 * i z akcji konfiguratora wewnatrz draw() (bez dostepu do surowego
 * Display/Window, ui.h celowo nie eksponuje ich z UiCtx) - ten sam
 * wzorzec globali co g_dpy/g_win w utils/7afm.c. */
static Display *g_dpy;
static Window g_win;

/* Dodatkowe katalogi ikon z zasobu X "7aCenter.iconPath" (patrz naglowek
 * pliku) - wskazniki w g_extra_icon_dirs wskazuja W GLAB g_extra_icon_dirs_buf
 * (podzielonego strtok_r na miejscu), wiec ten bufor musi zyc tak dlugo jak
 * same wskazniki (caly czas trwania programu - stad globalny, nie lokalny
 * w main()). */
static char g_extra_icon_dirs_buf[512];
static const char *g_extra_icon_dirs[MAX_EXTRA_ICON_DIRS];
static int g_extra_icon_dirs_count = 0;

static int g_selected_index = -1;
/* wyszukiwarka: filtr aktywnej zakladki po nazwie/komendzie (bez
 * rozrozniania wielkosci liter ASCII); pusty = wszystkie pozycje */
static char g_query[64] = "";
static int g_query_cur = 0;
static char g_status[1280] = "";  /* musi pomiescic g_conf_path (do 1200B) w komunikatach o braku/pustym configu */
static char g_conf_path[1200] = "";

static int g_scroll_y = 0;
static int g_row_pitch = CELL_H + CELL_GAP;  /* skok wiersza aktywnego widoku - z ostatniej klatki, do kolka */
static UiRect g_viewport_r = { 0, 0, 0, 0 };  /* z ostatniej klatki - do kolka myszy w main() */
static UiRect g_dock_r = { 0, 0, 0, 0 };      /* rzad komorek docka z ostatniej klatki - do upuszczenia */

static int g_last_click_index = -1;
static long g_last_click_ms = 0;

/* Dock - jeden wiersz ulubionych ikon w ostatnim boxie (sekcja [@Dock]
 * w center.conf). Stala tablica, bez malloc; pozycje to KOPIE (z wlasnym
 * Pixmapem ikony), niezalezne od pozycji na stronach. g_dock_cap = ile
 * komorek miesci sie w szerokosci okna (liczone w draw, <= DOCK_MAX). */
static LauncherEntry g_dock[DOCK_MAX];
static int g_dock_count = 0;
static int g_dock_cap = DOCK_MAX;
static int g_dock_hidden = 0;  /* przycisk w boxie "top" chowa box docka (nie zapisywane) */

/* Przeciaganie ikony (strona -> dock, dock -> dock/poza dock). Wciskanie
 * wykrywane przez zbocze surowego stanu LPM (ui_mouse_state), bo
 * ui_hit_test widzi tylko caly klik (press+release). */
typedef struct {
    int was_down;     /* LPM w poprzedniej klatce - do wykrycia zbocza */
    int press_edge;   /* LPM wcisniety W TEJ klatce */
    int pressed;      /* LPM wcisniety nad komorka (strony albo docka) */
    int active;       /* ruch przekroczyl DRAG_THRESHOLD - trwa przeciaganie */
    int from_dock;
    int index;        /* indeks w entries albo w g_dock */
    int press_x, press_y;
} DragState;

static DragState g_drag;

/* kolory komorek - alokowane raz w pierwszym draw() */
static XColor g_palette[PALETTE_SIZE];
static XColor g_outline_c, g_select_c, g_letter_c;

/* -------------------------------------------------------------------- */
/* Config - wlasny plik tej apki (~/.7a/center.conf, patrz naglowek);    */
/* jedyny zasob X, jaki 7acenter czyta, to dodatkowe katalogi ikon       */
/* ("7aCenter.iconPath") - patrz ui_resource_string w main() nizej.     */
/* -------------------------------------------------------------------- */

static int
EnsureCap(int needed)
{
    LauncherEntry *tmp;
    int new_cap;

    if (needed <= entry_cap)
        return 1;
    new_cap = entry_cap ? entry_cap * 2 : 32;
    if (new_cap < needed)
        new_cap = needed;
    tmp = realloc(entries, (size_t) new_cap * sizeof(LauncherEntry));
    if (!tmp)
        return 0;
    entries = tmp;
    entry_cap = new_cap;
    return 1;
}

/* Przycina biale znaki z obu koncow bufora W MIEJSCU. */
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

/* -------------------------------------------------------------------- */
/* Ikony XPM - wyciagniete z faktycznie uruchamianego programu (patrz    */
/* naglowek pliku po pelny opis podejscia i ograniczen).                 */
/* -------------------------------------------------------------------- */

/* Wbudowana lista, przeszukiwana PO katalogach z zasobu X "7aCenter.iconPath"
 * (g_extra_icon_dirs, patrz LoadIconForEntry) - user-owe katalogi wygrywaja,
 * jesli ten sam plik istnieje w obu miejscach. /usr/X11R6/include/X11/pixmaps
 * to klasyczny katalog ikon Xaw/Xt na OpenBSD (nie ma odpowiednika w
 * standardowym freedesktop icon theme spec, wiec musi byc wymieniony wprost). */
static const char *kIconDirs[] = {
    "/usr/share/pixmaps",
    "/usr/local/share/pixmaps",
    "/usr/X11R6/include/X11/pixmaps",
    "/usr/share/icons/hicolor/32x32/apps",
    "/usr/share/icons/hicolor/48x48/apps",
};

/* Basename pierwszego tokenu polecenia (np. "/usr/bin/firefox -p" ->
 * "firefox") - pod taka nazwa szuka sie pliku ikony. */
static void
FirstCommandToken(const char *command, char *out, size_t outsz)
{
    const char *start = command;
    const char *end;
    size_t len;
    char *slash;

    while (*start == ' ' || *start == '\t')
        start++;
    end = start;
    while (*end != '\0' && *end != ' ' && *end != '\t')
        end++;

    len = (size_t) (end - start);
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, start, len);
    out[len] = '\0';

    slash = strrchr(out, '/');
    if (slash)
        memmove(out, slash + 1, strlen(slash + 1) + 1);
}

/* Kolor, z ktorym "wypiekana" jest przezroczystosc ikony: tlo boxa, w
 * ktorym pozycja bedzie rysowana - dock ma box_bg (form_style), siatka i
 * lista box_info_bg. e->tab musi byc ustawione przed LoadIconForEntry. */
static const XColor *
IconBg(UiCtx *ctx, const LauncherEntry *e)
{
    return e->tab == DOCK_TAB ? ui_theme_box_bg(ctx) : ui_theme_box_info_bg(ctx);
}

/* Doskalowuje Pixmap src (sw x sh, ta sama glebia co win) do size x size
 * najblizszym sasiadem, czystym Xlib (XGetImage/XPutImage) -
 * bez zaleznosci od biblioteki skalujacej. Jesli mask != None (maska
 * ksztaltu z XpmReadFileToPixmap, TEJ SAMEJ wielkosci co src - tak
 * zawsze generuje ja libXpm), piksele spoza maski dostaja kolor tla
 * komorki siatki (ui_theme_box_info_bg) zamiast koloru z pliku .xpm - w ten
 * sposob "przezroczyste" fragmenty ikony (np. zaokraglone rogi) zlewaja
 * sie z tlem zamiast rysowac sie jako czarny prostokat. ui_draw_pixmap w
 * ui.h/ui.c NADAL nie zna pojecia maski/przezroczystosci (patrz komentarz
 * tam) - to podejscie polega na tym, ze tlo siatki jest jednolitym
 * kolorem znanym W MOMENCIE LADOWANIA ikony, wiec przezroczystosc mozna
 * "wypiec" raz, tutaj, zamiast prawdziwie maskowac przy KAZDYM rysowaniu
 * (co wymagaloby laczenia maski ksztaltu z prostokatnym przycieciem
 * viewportu w jednym GC clipie - ui.c go dla prostoty NIE udostepnia).
 * Znany kompromis: gdy komorka jest ZAZNACZONA (inne tlo, select_c
 * zamiast box_info_bg - patrz DrawCell), "przezroczyste" fragmenty ikony nadal
 * pokazuja box_info_bg, nie select_c - drobna niespojnosc kolorystyczna tylko
 * w tym jednym, przejsciowym stanie. Zwraca None przy bledzie (wywolujacy
 * spada wtedy na ikone proceduralna). */
static Pixmap
ComposeIconPixmap(const XColor *bg, Pixmap src, Pixmap mask, int sw, int sh, int size)
{
    XWindowAttributes wattr;
    XImage *src_img, *mask_img = NULL, *dst_img;
    Pixmap dst;
    GC gc;
    unsigned long bg_pixel = bg->pixel;
    int x, y;

    if (!XGetWindowAttributes(g_dpy, g_win, &wattr))
        return None;

    src_img = XGetImage(g_dpy, src, 0, 0, (unsigned) sw, (unsigned) sh, AllPlanes, ZPixmap);
    if (!src_img)
        return None;

    if (mask != None) {
        mask_img = XGetImage(g_dpy, mask, 0, 0, (unsigned) sw, (unsigned) sh, 1, XYPixmap);
        if (!mask_img) {
            XDestroyImage(src_img);
            return None;
        }
    }

    dst_img = XCreateImage(g_dpy, wattr.visual, (unsigned) wattr.depth, ZPixmap, 0, NULL,
                            (unsigned) size, (unsigned) size, 32, 0);
    if (!dst_img) {
        XDestroyImage(src_img);
        if (mask_img) XDestroyImage(mask_img);
        return None;
    }
    dst_img->data = malloc((size_t) dst_img->bytes_per_line * (size_t) size);
    if (!dst_img->data) {
        XDestroyImage(src_img);
        if (mask_img) XDestroyImage(mask_img);
        XDestroyImage(dst_img);  /* data == NULL, XFree(NULL) w srodku jest bezpieczne */
        return None;
    }

    for (y = 0; y < size; y++) {
        int sy = y * sh / size;

        if (sy >= sh) sy = sh - 1;
        for (x = 0; x < size; x++) {
            int sx = x * sw / size;
            unsigned long pixel;

            if (sx >= sw) sx = sw - 1;
            pixel = (mask_img && !XGetPixel(mask_img, sx, sy))
                    ? bg_pixel : XGetPixel(src_img, sx, sy);
            XPutPixel(dst_img, x, y, pixel);
        }
    }
    XDestroyImage(src_img);
    if (mask_img) XDestroyImage(mask_img);

    dst = XCreatePixmap(g_dpy, g_win, (unsigned) size, (unsigned) size, (unsigned) wattr.depth);
    gc = XCreateGC(g_dpy, dst, 0, NULL);
    XPutImage(g_dpy, dst, gc, dst_img, 0, 0, 0, 0, (unsigned) size, (unsigned) size);
    XFreeGC(g_dpy, gc);
    XDestroyImage(dst_img);  /* zwalnia tez dst_img->data (malloc powyzej) */

    return dst;
}

/* Plik .xpm -> Pixmap size x size (doskalowany, przezroczystosc wypieczona
 * kolorem bg). None, gdy plik nie istnieje/nie jest poprawnym .xpm/ma
 * podejrzany rozmiar. Wlasciciel wyniku: wywolujacy (FreeEntryIcon). */
static Pixmap
LoadXpmPixmap(const XColor *bg, const char *path, int size)
{
    Pixmap loaded = None, mask = None, out;
    XpmAttributes attrs;

    memset(&attrs, 0, sizeof(attrs));
    if (XpmReadFileToPixmap(g_dpy, g_win, path, &loaded, &mask, &attrs) != XpmSuccess)
        return None;

    if (attrs.width == 0 || attrs.height == 0 ||
        attrs.width > ICON_MAX_SRC || attrs.height > ICON_MAX_SRC) {
        out = None;
        XFreePixmap(g_dpy, loaded);
    } else if (mask == None && attrs.width == (unsigned) size && attrs.height == (unsigned) size) {
        out = loaded;  /* szybka sciezka: bez maski (pelna nieprzezroczystosc)
                          i juz wlasciwego rozmiaru - nic do przetworzenia */
    } else {
        out = ComposeIconPixmap(bg, loaded, mask, (int) attrs.width, (int) attrs.height, size);
        XFreePixmap(g_dpy, loaded);
    }
    if (mask != None) XFreePixmap(g_dpy, mask);
    XpmFreeAttributes(&attrs);
    return out;
}

/* Proba zaladowania JEDNEGO konkretnego pliku .xpm do e->icon. Zwraca 1
 * przy sukcesie (e->has_icon tez ustawione) - wspolny rdzen dla obu
 * sciezek w LoadIconForEntry nizej (bezwzglednej sciezki z configu i
 * przeszukiwania katalogow). */
static int
TryLoadXpmPath(UiCtx *ctx, LauncherEntry *e, const char *path)
{
    e->icon = LoadXpmPixmap(IconBg(ctx, e), path, ICON_SIZE);
    e->has_icon = (e->icon != None);
    return e->has_icon;
}

/* Zestawy ikon w stylu KDE 1 maja obok large-X.xpm (48x48) reczne
 * mini-X.xpm (16x16) - ostrzejsze w widoku listy niz zmniejszona duza
 * ikona. Szukane tylko dla bezwzglednej sciezki z configu; brak pliku =
 * e->mini zostaje None. */
static void
LoadMiniIcon(UiCtx *ctx, LauncherEntry *e, const char *path)
{
    const char *slash = strrchr(path, '/');
    char mini[sizeof(e->icon_override)];
    int dir_len;

    if (!slash || strncmp(slash + 1, "large-", 6) != 0)
        return;
    dir_len = (int) (slash + 1 - path);
    if (snprintf(mini, sizeof(mini), "%.*smini-%s", dir_len, path, slash + 7) >= (int) sizeof(mini))
        return;
    e->mini = LoadXpmPixmap(IconBg(ctx, e), mini, LIST_ICON);
}

/* Jednobitowa ikona .xbm (np. Material Design z utils/icons/) -> pelnoglebiowy
 * Pixmap ICON_SIZE x ICON_SIZE: ustawione bity w kolorze motywu
 * iconForeground (ui_theme_icon_fg), reszta w box_info_bg - ten sam "wypiekany"
 * kompromis co przy masce .xpm w ComposeIconPixmap (bez maskowania przy
 * kazdym rysowaniu; zmiana motywu wymaga restartu, jak dla .xpm). Skalowanie
 * najblizszym sasiadem jak wyzej. Zwraca None przy bledzie. */
static Pixmap
ComposeBitmapPixmap(UiCtx *ctx, const XColor *bg, Pixmap bits, int sw, int sh)
{
    XWindowAttributes wattr;
    XImage *src_img, *dst_img;
    Pixmap dst;
    GC gc;
    unsigned long fg_pixel = ui_theme_icon_fg(ctx)->pixel;
    unsigned long bg_pixel = bg->pixel;
    int x, y;

    if (!XGetWindowAttributes(g_dpy, g_win, &wattr))
        return None;

    src_img = XGetImage(g_dpy, bits, 0, 0, (unsigned) sw, (unsigned) sh, 1, XYPixmap);
    if (!src_img)
        return None;

    dst_img = XCreateImage(g_dpy, wattr.visual, (unsigned) wattr.depth, ZPixmap, 0, NULL,
                            ICON_SIZE, ICON_SIZE, 32, 0);
    if (!dst_img) {
        XDestroyImage(src_img);
        return None;
    }
    dst_img->data = malloc((size_t) dst_img->bytes_per_line * ICON_SIZE);
    if (!dst_img->data) {
        XDestroyImage(src_img);
        XDestroyImage(dst_img);
        return None;
    }

    for (y = 0; y < ICON_SIZE; y++) {
        int sy = y * sh / ICON_SIZE;

        for (x = 0; x < ICON_SIZE; x++) {
            int sx = x * sw / ICON_SIZE;

            XPutPixel(dst_img, x, y, XGetPixel(src_img, sx, sy) ? fg_pixel : bg_pixel);
        }
    }
    XDestroyImage(src_img);

    dst = XCreatePixmap(g_dpy, g_win, ICON_SIZE, ICON_SIZE, wattr.depth);
    gc = XCreateGC(g_dpy, dst, 0, NULL);
    XPutImage(g_dpy, dst, gc, dst_img, 0, 0, 0, 0, ICON_SIZE, ICON_SIZE);
    XFreeGC(g_dpy, gc);
    XDestroyImage(dst_img);  /* zwalnia tez dst_img->data */

    return dst;  /* wlasciciel: LauncherEntry.icon, zwalniane w FreeEntryIcon */
}

/* Bufor na bajty jednej .xbm (max ICON_MAX_SRC x ICON_MAX_SRC), uzywany tylko
 * przy ladowaniu ikon - stala tablica zamiast malloc. */
static unsigned char g_xbm_data[(ICON_MAX_SRC + 7) / 8 * ICON_MAX_SRC];

/* Wlasny minimalny parser .xbm zamiast XReadBitmapFile: Xlib rozpoznaje
 * tylko "static char"/"static unsigned char", a ikony z utils/icons/ maja
 * "static const unsigned char" (bo sa #include'owane w apkach) - Xlib je
 * odrzuca. Czyta "#define *_width N"/"#define *_height N", potem bajty
 * "0xNN" do g_xbm_data. Zwraca 1, gdy wczytano komplet (w+7)/8*h bajtow. */
static int
ReadXbmFile(const char *path, unsigned int *w, unsigned int *h)
{
    FILE *f = fopen(path, "r");
    char line[256];
    unsigned int v;
    size_t n = 0, need = 0;
    int in_data = 0;

    if (!f)
        return 0;
    *w = *h = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *s = line;
        char *end;

        if (!in_data) {
            if (sscanf(line, "#define %*s %u", &v) == 1) {
                if (strstr(line, "_width"))
                    *w = v;
                else if (strstr(line, "_height"))
                    *h = v;
                continue;
            }
            if (!(s = strchr(line, '{')))
                continue;
            if (*w == 0 || *h == 0 || *w > ICON_MAX_SRC || *h > ICON_MAX_SRC)
                break;
            need = (size_t) (*w + 7) / 8 * *h;
            in_data = 1;
        }
        while (n < need && (s = strstr(s, "0x")) != NULL) {
            g_xbm_data[n++] = (unsigned char) strtoul(s, &end, 16);
            s = (end > s) ? end : s + 2;
        }
        if (n == need)
            break;
    }
    fclose(f);
    return in_data && n == need;
}

/* Odpowiednik TryLoadXpmPath dla .xbm. Tymczasowy 1-bitowy Pixmap jest
 * zwalniany tutaj; wynik ComposeBitmapPixmap trafia do e->icon. */
static int
TryLoadXbmPath(UiCtx *ctx, LauncherEntry *e, const char *path)
{
    Pixmap bits;
    unsigned int w, h;

    if (!ReadXbmFile(path, &w, &h))
        return 0;
    bits = XCreateBitmapFromData(g_dpy, g_win, (const char *) g_xbm_data, w, h);
    if (bits == None)
        return 0;
    e->icon = ComposeBitmapPixmap(ctx, IconBg(ctx, e), bits, (int) w, (int) h);
    XFreePixmap(g_dpy, bits);

    e->has_icon = (e->icon != None);
    return e->has_icon;
}

/* Sciezka konczaca sie na ".xbm" -> TryLoadXbmPath, kazda inna -> .xpm. */
static int
TryLoadIconPath(UiCtx *ctx, LauncherEntry *e, const char *path)
{
    size_t len = strlen(path);

    if (len > 4 && strcmp(path + len - 4, ".xbm") == 0)
        return TryLoadXbmPath(ctx, e, path);
    return TryLoadXpmPath(ctx, e, path);
}

/* Szuka <dir>/<base>.xpm, potem <dir>/<base>.xbm. */
static int
TryLoadIconInDir(UiCtx *ctx, LauncherEntry *e, const char *dir, const char *base)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s.xpm", dir, base);
    if (TryLoadXpmPath(ctx, e, path))
        return 1;
    snprintf(path, sizeof(path), "%s/%s.xbm", dir, base);
    return TryLoadXbmPath(ctx, e, path);
}

/* Zrodlo nazwy/sciezki do wyszukania (patrz naglowek pliku po pelny opis):
 *  - e->icon_override zaczynajace sie od "/" -> bezwzgledna sciezka do
 *    KONKRETNEGO pliku .xpm z configu, bez przeszukiwania katalogow;
 *  - e->icon_override niepuste, bez "/" -> wlasna nazwa bazowa (zamiast
 *    zgadywania z komendy), nadal szukana w katalogach jak nizej;
 *  - e->icon_override puste -> nazwa bazowa zgadywana z pierwszego tokenu
 *    komendy (FirstCommandToken), jak dotychczas.
 * Katalogi: najpierw g_extra_icon_dirs (zasob X "7aCenter.iconPath" -
 * wlasne katalogi uzytkownika WYGRYWAJA), potem wbudowana kIconDirs.
 * e->has_icon zostaje 0 (fallback na proceduralna ikone w DrawCell),
 * jesli nic nie pasuje. */
static void
LoadIconForEntry(UiCtx *ctx, LauncherEntry *e)
{
    char base[sizeof(e->icon_override)];
    size_t i;

    e->icon = None;
    e->has_icon = 0;
    e->mini = None;

    if (!g_dpy)
        return;

    if (e->icon_override[0] == '/') {
        if (TryLoadIconPath(ctx, e, e->icon_override))
            LoadMiniIcon(ctx, e, e->icon_override);
        return;
    }

    if (e->icon_override[0] != '\0')
        snprintf(base, sizeof(base), "%s", e->icon_override);
    else
        FirstCommandToken(e->command, base, sizeof(base));
    if (base[0] == '\0')
        return;

    for (i = 0; i < (size_t) g_extra_icon_dirs_count; i++)
        if (TryLoadIconInDir(ctx, e, g_extra_icon_dirs[i], base))
            return;
    for (i = 0; i < sizeof(kIconDirs) / sizeof(kIconDirs[0]); i++)
        if (TryLoadIconInDir(ctx, e, kIconDirs[i], base))
            return;
}

static void
FreeEntryIcon(LauncherEntry *e)
{
    if (e->has_icon)
        XFreePixmap(g_dpy, e->icon);
    if (e->mini != None)
        XFreePixmap(g_dpy, e->mini);
    e->icon = None;
    e->has_icon = 0;
    e->mini = None;
}

/* -------------------------------------------------------------------- */
/* Dock - logika niezalezna od rysowania. DockInsert to jedyne wejscie   */
/* nowych pozycji (config, upuszczenie ze strony, w przyszlosci XDND z    */
/* 7afm), wiec limity i duplikaty sprawdzane sa w jednym miejscu.        */

static int
DockFind(const char *name, const char *command)
{
    int i;

    for (i = 0; i < g_dock_count; i++)
        if (strcmp(g_dock[i].name, name) == 0 && strcmp(g_dock[i].command, command) == 0)
            return i;
    return -1;
}

/* wstawia kopie pozycji na miejsce pos (przycinane do 0..g_dock_count);
 * 0 + komunikat w g_status, gdy dock pelny albo pozycja juz w nim jest
 * (limit DOCK_MAX; ile miesci sie w oknie - g_dock_cap - sprawdza
 * upuszczenie, bo config czytany jest przed pierwszym pomiarem okna) */
static int
DockInsert(UiCtx *ctx, const char *name, const char *command, const char *icon, int pos)
{
    LauncherEntry *e;

    if (g_dock_count >= DOCK_MAX) {
        snprintf(g_status, sizeof(g_status), "Dock is full");
        return 0;
    }
    if (DockFind(name, command) >= 0) {
        snprintf(g_status, sizeof(g_status), "%s is already in the dock", name);
        return 0;
    }
    if (pos < 0) pos = 0;
    if (pos > g_dock_count) pos = g_dock_count;
    memmove(&g_dock[pos + 1], &g_dock[pos], (size_t) (g_dock_count - pos) * sizeof(g_dock[0]));
    g_dock_count++;

    e = &g_dock[pos];
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->command, sizeof(e->command), "%s", command);
    snprintf(e->icon_override, sizeof(e->icon_override), "%s", icon);
    e->tab = DOCK_TAB;
    LoadIconForEntry(ctx, e);
    return 1;
}

static void
DockRemove(int index)
{
    FreeEntryIcon(&g_dock[index]);
    memmove(&g_dock[index], &g_dock[index + 1],
            (size_t) (g_dock_count - index - 1) * sizeof(g_dock[0]));
    g_dock_count--;
}

/* przenosi pozycje from na miejsce pos liczone PRZED jej wyjeciem */
static void
DockMove(int from, int pos)
{
    LauncherEntry tmp = g_dock[from];

    if (pos > from) pos--;
    if (pos < 0) pos = 0;
    if (pos > g_dock_count - 1) pos = g_dock_count - 1;
    memmove(&g_dock[from], &g_dock[from + 1], (size_t) (g_dock_count - from - 1) * sizeof(g_dock[0]));
    memmove(&g_dock[pos + 1], &g_dock[pos], (size_t) (g_dock_count - 1 - pos) * sizeof(g_dock[0]));
    g_dock[pos] = tmp;
}

/* needle w hay bez rozrozniania wielkosci liter (ASCII) - strcasestr
 * nie jest w C99 */
static int
ContainsNoCase(const char *hay, const char *needle)
{
    size_t i, n = strlen(needle);

    for (; *hay; hay++) {
        for (i = 0; i < n && hay[i]; i++)
            if (tolower((unsigned char) hay[i]) != tolower((unsigned char) needle[i]))
                break;
        if (i == n)
            return 1;
    }
    return n == 0;
}

/* pozycja widoczna w siatce: aktywna zakladka + filtr wyszukiwarki */
static int
EntryShown(int i)
{
    return entries[i].tab == g_active_tab &&
           (ContainsNoCase(entries[i].name, g_query) || ContainsNoCase(entries[i].command, g_query));
}

static int
ShownCount(void)
{
    int i, n = 0;

    for (i = 0; i < entry_count; i++)
        if (EntryShown(i))
            n++;
    return n;
}

static int
TabEntryCount(int tab)
{
    int i, n = 0;

    for (i = 0; i < entry_count; i++)
        if (entries[i].tab == tab)
            n++;
    return n;
}

static int
FindTab(const char *name)
{
    int i;

    for (i = 0; i < g_tab_count; i++)
        if (strcmp(g_tabs[i], name) == 0)
            return i;
    return -1;
}

/* indeks istniejacej zakladki o tej nazwie albo nowo dodanej; -1 gdy
 * nazwa pusta albo juz jest MAX_TABS zakladek */
static int
FindOrAddTab(const char *name)
{
    int t = FindTab(name);

    if (t >= 0 || name[0] == '\0')
        return t;
    if (g_tab_count >= MAX_TABS)
        return -1;
    snprintf(g_tabs[g_tab_count], sizeof(g_tabs[0]), "%s", name);
    g_tab_list[g_tab_count] = 0;
    return g_tab_count++;
}

/* Linia "[Nazwa]" -> zakladka. Zwraca 1, jesli to byla linia sekcji
 * (*cur_tab ustawione; -1 = przepelnienie MAX_TABS, jej pozycje sa
 * wtedy pomijane). */
static int
ParseSectionLine(char *line, int *cur_tab)
{
    size_t len = strlen(line);

    if (line[0] != '[' || len < 2 || line[len - 1] != ']')
        return 0;
    line[len - 1] = '\0';
    line = TrimInPlace(line + 1);
    *cur_tab = strcmp(line, DOCK_SECTION) == 0 ? DOCK_TAB : FindOrAddTab(line);
    return 1;
}

/* Linia "Nazwa|komenda[|ikona]" -> pola e (bez ikony). Zwraca 0, gdy
 * linia nie ma '|' albo nazwa/komenda sa puste. */
static int
ParseEntryFields(char *line, LauncherEntry *e)
{
    char *sep1, *sep2;

    sep1 = strchr(line, '|');
    if (!sep1)
        return 0;
    *sep1 = '\0';
    /* trzecie, OPCJONALNE pole "ikona" (patrz naglowek pliku) - jesli
     * nie ma drugiego '|', cala reszta linii to samo pole "komenda". */
    sep2 = strchr(sep1 + 1, '|');
    if (sep2)
        *sep2 = '\0';

    snprintf(e->name, sizeof(e->name), "%s", TrimInPlace(line));
    snprintf(e->command, sizeof(e->command), "%s", TrimInPlace(sep1 + 1));
    snprintf(e->icon_override, sizeof(e->icon_override), "%s",
             sep2 ? TrimInPlace(sep2 + 1) : "");
    e->icon = None;
    e->has_icon = 0;
    e->mini = None;
    return e->name[0] != '\0' && e->command[0] != '\0';
}

/* Linia pozycji -> nowa pozycja w zakladce tab albo w docku (DOCK_TAB). */
static void
ParseEntryLine(UiCtx *ctx, char *line, int tab)
{
    LauncherEntry e;

    if (!ParseEntryFields(line, &e))
        return;
    if (tab == DOCK_TAB) {
        DockInsert(ctx, e.name, e.command, e.icon_override, g_dock_count);
        return;
    }
    if (!EnsureCap(entry_count + 1))
        return; /* OOM - zostaje to, co juz wczytane, zamiast crashowac */
    e.tab = tab;
    LoadIconForEntry(ctx, &e);
    entries[entry_count++] = e;
}

static void
LoadEntries(UiCtx *ctx)
{
    const char *home = getenv("HOME");
    FILE *fp;
    char line[512];
    int cur_tab = -2;  /* -2 = przed pierwsza sekcja (stary format) */

    snprintf(g_conf_path, sizeof(g_conf_path), "%s/.7a/center.conf", home ? home : ".");
    entry_count = 0;
    while (g_dock_count > 0)
        DockRemove(g_dock_count - 1);
    g_tab_count = 0;

    fp = fopen(g_conf_path, "r");
    if (!fp) {
        snprintf(g_status, sizeof(g_status), "No config yet - click Edit to add programs");
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = TrimInPlace(line);

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;
        if (ParseSectionLine(trimmed, &cur_tab))
            continue;
        if (cur_tab == -2)
            cur_tab = FindOrAddTab("Main");
        if (cur_tab >= 0 || cur_tab == DOCK_TAB)
            ParseEntryLine(ctx, trimmed, cur_tab);
    }
    fclose(fp);

    g_active_tab = 0;
    g_selected_index = -1;
    g_scroll_y = 0;
    if (g_tab_count == 0)
        snprintf(g_status, sizeof(g_status), "%s is empty - click Edit to add programs", g_conf_path);
    else
        snprintf(g_status, sizeof(g_status), "%d programs", TabEntryCount(0));
}

static void
WriteEntryLine(FILE *fp, const LauncherEntry *e)
{
    if (e->icon_override[0] != '\0')
        fprintf(fp, "%s|%s|%s\n", e->name, e->command, e->icon_override);
    else
        fprintf(fp, "%s|%s\n", e->name, e->command);
}

static int
WriteConfig(FILE *fp)
{
    int t, i;

    fprintf(fp, "# 7acenter config - rewritten by 7acenter's Edit mode\n");
    for (t = 0; t < g_tab_count; t++) {
        fprintf(fp, "\n[%s]\n", g_tabs[t]);
        for (i = 0; i < entry_count; i++)
            if (entries[i].tab == t)
                WriteEntryLine(fp, &entries[i]);
    }
    if (g_dock_count > 0) {
        fprintf(fp, "\n[%s]\n", DOCK_SECTION);
        for (i = 0; i < g_dock_count; i++)
            WriteEntryLine(fp, &g_dock[i]);
    }
    return fflush(fp) == 0 && !ferror(fp);
}

/* Przepisuje caly center.conf: plik tymczasowy w tym samym katalogu
 * (mkstemp) + rename, wiec przerwany zapis nigdy nie zostawia polowy
 * configu. Zwraca 1 przy sukcesie, przy bledzie komunikat w g_status. */
static int
SaveConfig(void)
{
    const char *home = getenv("HOME");
    char dir[1024], tmp[1100];
    FILE *fp;
    int fd, ok;

    snprintf(dir, sizeof(dir), "%s/.7a", home ? home : ".");
    if (mkdir(dir, 0700) == -1 && errno != EEXIST) {
        snprintf(g_status, sizeof(g_status), "Cannot create %s: %s", dir, strerror(errno));
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s/.center.conf.XXXXXX", dir);
    fd = mkstemp(tmp);
    if (fd == -1) {
        snprintf(g_status, sizeof(g_status), "Cannot save: %s", strerror(errno));
        return 0;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        snprintf(g_status, sizeof(g_status), "Cannot save: %s", strerror(errno));
        return 0;
    }
    ok = WriteConfig(fp);
    ok = (fclose(fp) == 0) && ok;
    if (!ok || rename(tmp, g_conf_path) == -1) {
        unlink(tmp);
        snprintf(g_status, sizeof(g_status), "Cannot save %s", g_conf_path);
        return 0;
    }
    return 1;
}

static void
RemoveEntry(int index)
{
    FreeEntryIcon(&entries[index]);
    memmove(&entries[index], &entries[index + 1],
            (size_t) (entry_count - index - 1) * sizeof(LauncherEntry));
    entry_count--;
}

/* usuwa zakladke t razem ze wszystkimi jej pozycjami */
static void
RemoveTab(int t)
{
    int i;

    for (i = entry_count - 1; i >= 0; i--) {
        if (entries[i].tab == t)
            RemoveEntry(i);
        else if (entries[i].tab > t)
            entries[i].tab--;
    }
    memmove(g_tabs[t], g_tabs[t + 1], (size_t) (g_tab_count - t - 1) * sizeof(g_tabs[0]));
    memmove(&g_tab_list[t], &g_tab_list[t + 1], (size_t) (g_tab_count - t - 1));
    g_tab_count--;
    if (g_active_tab >= g_tab_count)
        g_active_tab = g_tab_count > 0 ? g_tab_count - 1 : 0;
}

/* Dzieli buf W MIEJSCU na tokeny rozdzielone spacjami/tabami; "..." grupuje
 * token zawierajacy spacje (cudzyslowy sa usuwane, bez escape'ow - KISS,
 * wystarcza na komendy z configu typu 7askm -stop "Gdynia Glowna").
 * Niezamkniety cudzyslow ciagnie token do konca bufora. Zwraca liczbe
 * tokenow; argv[argc] = NULL. max musi byc >= 1. */
static int
SplitCommand(char *buf, char **argv, int max)
{
    char *r = buf, *w;
    int argc = 0;

    while (argc < max - 1) {
        int in_quote = 0;

        while (*r == ' ' || *r == '\t')
            r++;
        if (*r == '\0')
            break;
        argv[argc++] = w = r;
        while (*r != '\0' && (in_quote || (*r != ' ' && *r != '\t'))) {
            if (*r == '"')
                in_quote = !in_quote;
            else
                *w++ = *r;
            r++;
        }
        if (*r != '\0')
            r++;    /* separator: w < r, wiec '\0' ponizej nie nadpisze nieprzeczytanego bajtu */
        *w = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void
LaunchEntry(const LauncherEntry *e)
{
    char buf[sizeof(e->command)];
    char *argv[MAX_CMD_TOKENS];
    pid_t pid;

    snprintf(buf, sizeof(buf), "%s", e->command);
    if (SplitCommand(buf, argv, MAX_CMD_TOKENS) == 0)
        return;

    pid = fork();
    if (pid == 0) {
        /* Nowa sesja, zeby zamkniecie 7acenter (SIGHUP/SIGTERM do grupy
         * procesow terminala, ktory je uruchomil) nie zabijalo tez
         * odpalonego programu - patrz setsid() w dwm/dmenu_run. */
        setsid();
        execvp(argv[0], argv);
        _exit(127);
    }
    snprintf(g_status, sizeof(g_status), "Launched: %s", e->name);
}

static void
SelectEntry(int index)
{
    g_selected_index = index;
    if (index < 0 || index >= entry_count)
        snprintf(g_status, sizeof(g_status), "%d programs", TabEntryCount(g_active_tab));
    else
        snprintf(g_status, sizeof(g_status), "%s: %s",
                 entries[index].name, entries[index].command);
}

/* -------------------------------------------------------------------- */
/* Konfigurator (tryb "Edit") - logika niezalezna od rysowania; kazda    */
/* akcja od razu przepisuje center.conf (SaveConfig).                    */

static void
FormSetCursors(void)
{
    g_form.name_cur = (int) strlen(g_form.name);
    g_form.command_cur = (int) strlen(g_form.command);
    g_form.icon_cur = (int) strlen(g_form.icon);
}

/* pusty formularz nowej pozycji w aktywnej zakladce */
static void
FormReset(void)
{
    g_form.name[0] = g_form.command[0] = g_form.icon[0] = '\0';
    g_form.index = -1;
    FormSetCursors();
}

static void
FormLoad(int index)
{
    const LauncherEntry *e = &entries[index];

    snprintf(g_form.name, sizeof(g_form.name), "%s", e->name);
    snprintf(g_form.command, sizeof(g_form.command), "%s", e->command);
    snprintf(g_form.icon, sizeof(g_form.icon), "%s", e->icon_override);
    g_form.index = index;
    FormSetCursors();
}

/* '|' rozdziela pola linii configu, '[' na poczatku robi z linii sekcje,
 * '#' na poczatku - komentarz; takie wartosci nie przetrwalyby zapisu */
static int
FieldOk(const char *s)
{
    return strchr(s, '|') == NULL && s[0] != '[' && s[0] != '#';
}

static int
TabNameOk(const char *s)
{
    if (s[0] == '\0') {
        snprintf(g_status, sizeof(g_status), "Tab name is empty");
        return 0;
    }
    if (strchr(s, ']') || !FieldOk(s)) {
        snprintf(g_status, sizeof(g_status), "Tab name must not contain ] or |");
        return 0;
    }
    if (s[0] == '@') {
        snprintf(g_status, sizeof(g_status), "Tab name must not start with @");
        return 0;
    }
    return 1;
}

static void
SwitchTab(int t)
{
    g_active_tab = t;
    g_scroll_y = 0;
    SelectEntry(-1);
    FormReset();
}

/* Add/Save: nowa pozycja (index -1) w aktywnej zakladce albo zmiana
 * istniejacej; bez zadnej zakladki powstaje "Main" (jak plik bez sekcji) */
static void
FormSubmit(UiCtx *ctx)
{
    const char *name = TrimInPlace(g_form.name);
    const char *command = TrimInPlace(g_form.command);
    const char *icon = TrimInPlace(g_form.icon);
    LauncherEntry *e;
    int tab, is_new = (g_form.index < 0);

    FormSetCursors();
    if (name[0] == '\0' || command[0] == '\0') {
        snprintf(g_status, sizeof(g_status), "Name and Command are required");
        return;
    }
    if (!FieldOk(name) || !FieldOk(command) || strchr(icon, '|')) {
        snprintf(g_status, sizeof(g_status), "Fields must not contain | (nor start with [ or #)");
        return;
    }
    tab = is_new ? (g_tab_count > 0 ? g_active_tab : FindOrAddTab("Main"))
                 : entries[g_form.index].tab;
    if (tab < 0)
        return;
    if (is_new) {
        if (!EnsureCap(entry_count + 1)) {
            snprintf(g_status, sizeof(g_status), "Out of memory");
            return;
        }
        e = &entries[entry_count++];
        e->has_icon = 0;
        e->icon = None;
        e->mini = None;
    } else {
        e = &entries[g_form.index];
        FreeEntryIcon(e);
    }
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->command, sizeof(e->command), "%s", command);
    snprintf(e->icon_override, sizeof(e->icon_override), "%s", icon);
    e->tab = tab;
    LoadIconForEntry(ctx, e);

    SwitchTab(tab);
    if (SaveConfig())
        snprintf(g_status, sizeof(g_status), "%s: %s", is_new ? "Added" : "Saved", e->name);
}

static void
FormDeleteEntry(void)
{
    char name[sizeof(entries[0].name)];

    if (g_form.index < 0) {
        snprintf(g_status, sizeof(g_status), "Click an icon first to pick it for deletion");
        return;
    }
    snprintf(name, sizeof(name), "%s", entries[g_form.index].name);
    RemoveEntry(g_form.index);
    SwitchTab(g_active_tab);
    if (SaveConfig())
        snprintf(g_status, sizeof(g_status), "Deleted: %s", name);
}

static void
FormAddTab(void)
{
    const char *tab_name = TrimInPlace(g_tab_name);
    int t;

    g_tab_name_cur = (int) strlen(g_tab_name);
    if (!TabNameOk(tab_name))
        return;
    if (FindTab(tab_name) >= 0) {
        snprintf(g_status, sizeof(g_status), "Tab \"%s\" already exists", tab_name);
        return;
    }
    t = FindOrAddTab(tab_name);
    if (t < 0) {
        snprintf(g_status, sizeof(g_status), "Too many tabs (max %d)", MAX_TABS);
        return;
    }
    SwitchTab(t);
    if (SaveConfig())
        snprintf(g_status, sizeof(g_status), "Added tab: %s", g_tabs[t]);
}

/* aktywna zakladka dostaje nazwe z pola formularza zakladek */
static void
FormRenameTab(void)
{
    const char *tab_name = TrimInPlace(g_tab_name);
    int other;

    g_tab_name_cur = (int) strlen(g_tab_name);
    if (g_tab_count == 0 || !TabNameOk(tab_name))
        return;
    other = FindTab(tab_name);
    if (other >= 0 && other != g_active_tab) {
        snprintf(g_status, sizeof(g_status), "Tab \"%s\" already exists", tab_name);
        return;
    }
    snprintf(g_tabs[g_active_tab], sizeof(g_tabs[0]), "%s", tab_name);
    if (SaveConfig())
        snprintf(g_status, sizeof(g_status), "Renamed tab to: %s", g_tabs[g_active_tab]);
}

/* usuwa aktywna zakladke z jej pozycjami - wymaga drugiego klikniecia
 * (armed), bo to jedyna akcja kasujaca wiele pozycji naraz */
static void
FormDeleteTab(int *armed)
{
    char name[TAB_NAME_SZ];

    if (g_tab_count == 0)
        return;
    if (*armed != g_active_tab + 1) {
        *armed = g_active_tab + 1;
        snprintf(g_status, sizeof(g_status), "Delete tab \"%s\" and its %d programs? Click again",
                 g_tabs[g_active_tab], TabEntryCount(g_active_tab));
        return;
    }
    *armed = 0;
    snprintf(name, sizeof(name), "%s", g_tabs[g_active_tab]);
    RemoveTab(g_active_tab);
    SwitchTab(g_active_tab);
    if (SaveConfig())
        snprintf(g_status, sizeof(g_status), "Deleted tab: %s", name);
}

/* -------------------------------------------------------------------- */
/* Ikonki komorek - proceduralne, patrz naglowek pliku. Ten sam duch     */
/* "rysowane na zywo przez prymitywy ui.c" co DrawDirIcon/DrawFileIcon   */
/* w utils/7afm.c, tylko jeden ksztalt zamiast kilku wariantow.       */
/* -------------------------------------------------------------------- */

static unsigned
HashName(const char *name)
{
    unsigned h = 0;
    const unsigned char *p;

    for (p = (const unsigned char *) name; *p; p++)
        h = h * 31u + *p;
    return h;
}

static void
DrawAppIcon(UiCtx *ctx, int x, int y, int size, const char *name,
            const XColor *fill, const XColor *outline, const XColor *letter_c)
{
    UiRect body = { x, y, size, size };
    char initial[2];

    ui_fill_round_rect(ctx, body, fill);
    ui_draw_round_border(ctx, body, 1, outline);

    initial[0] = (char) toupper((unsigned char) name[0]);
    initial[1] = '\0';
    ui_label_centered_fg(ctx, body, initial, letter_c);
}

/* ikona pozycji (prawdziwa albo proceduralna) w kwadracie size x size */
static void
DrawEntryIcon(UiCtx *ctx, const LauncherEntry *e, int x, int y, int size)
{
    if (e->has_icon)
        ui_draw_pixmap(ctx, (UiRect){ x, y, size, size }, e->icon);
    else
        DrawAppIcon(ctx, x, y, size, e->name,
                    &g_palette[HashName(e->name) % PALETTE_SIZE], &g_outline_c, &g_letter_c);
}

/* Sama komorka (ikona + podpis), bez obslugi myszy - ta sama dla stron,
 * docka i "ducha" przeciaganej ikony. */
static void
DrawCell(UiCtx *ctx, const LauncherEntry *e, int cx, int cy, int selected)
{
    UiRect cell_r = { cx, cy, CELL_W, CELL_H };
    UiRect label_r;
    int icon_x, icon_y, label_y;

    if (selected)
        ui_fill_round_rect(ctx, cell_r, &g_select_c);

    icon_x = cx + (CELL_W - ICON_SIZE) / 2;
    icon_y = cy + CELL_VPAD;
    DrawEntryIcon(ctx, e, icon_x, icon_y, ICON_SIZE);

    label_y = icon_y + ICON_SIZE + 3;
    label_r = (UiRect){ cx, label_y, CELL_W, CELL_H - (label_y - cy) };
    /* Mieszczaca sie nazwa - wysrodkowana; za dluga - od lewej, obcieta z
     * prawej na "..." (ui_label_centered przycialby ja z obu stron). */
    if (ui_text_width(ctx, e->name) <= label_r.w)
        ui_label_centered(ctx, label_r, e->name);
    else
        ui_label_ellipsis(ctx, label_r, e->name);
}

/* Wiersz widoku listy: ikona, nazwa, komenda - jak DrawCell bez myszy */
static void
DrawListRow(UiCtx *ctx, const LauncherEntry *e, UiRect r, int selected)
{
    int text_x = r.x + CELL_VPAD + LIST_ICON + FORM_GAP;
    int name_w = LIST_NAME_W;

    if (selected)
        ui_fill_round_rect(ctx, r, &g_select_c);
    if (e->mini != None)
        ui_draw_pixmap(ctx, (UiRect){ r.x + CELL_VPAD, r.y + (r.h - LIST_ICON) / 2,
                                      LIST_ICON, LIST_ICON }, e->mini);
    else
        DrawEntryIcon(ctx, e, r.x + CELL_VPAD, r.y + (r.h - LIST_ICON) / 2, LIST_ICON);
    if (name_w > r.x + r.w - text_x)
        name_w = r.x + r.w - text_x;
    if (name_w > 0)
        ui_label_ellipsis(ctx, (UiRect){ text_x, r.y, name_w, r.h }, e->name);
    text_x += LIST_NAME_W + FORM_GAP;
    if (text_x < r.x + r.w)
        ui_label_ellipsis(ctx, (UiRect){ text_x, r.y, r.x + r.w - text_x, r.h }, e->command);
}

/* klik (press+release bez przeciagania) komorki strony: zaznaczenie,
 * dwuklik = uruchomienie; w trybie Edit laduje pozycje do formularza */
static void
GridCellClick(int index)
{
    struct timeval tv;
    long now;
    int is_double;

    gettimeofday(&tv, NULL);
    now = (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
    is_double = (index == g_last_click_index) && (now - g_last_click_ms <= DOUBLE_CLICK_MS);

    SelectEntry(index);
    if (g_edit_mode) {
        FormLoad(index);
        return;
    }
    g_last_click_index = index;
    g_last_click_ms = is_double ? 0 : now;
    if (is_double)
        LaunchEntry(&entries[index]);
}

/* -------------------------------------------------------------------- */
/* Przeciaganie ikon (strona -> dock, dock -> dock, dock -> poza dock).  */
/* Sesja przeciagania to stan apki (patrz ui_mouse_state w ui.h).        */
/* -------------------------------------------------------------------- */

static int
PointIn(UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* pozycja wstawienia w docku dla kursora x (miedzy komorkami) */
static int
DockSlotAt(int x)
{
    int pos = (x - g_dock_r.x + (CELL_W + CELL_GAP) / 2) / (CELL_W + CELL_GAP);

    if (pos < 0) pos = 0;
    if (pos > g_dock_count) pos = g_dock_count;
    return pos;
}

/* koniec przeciagania (zwolniony LPM) - wynik wedlug miejsca upuszczenia;
 * geometria docka z poprzedniej klatki (g_dock_r), wiec stan zmienia sie
 * PRZED rysowaniem tej klatki */
static void
DragDrop(UiCtx *ctx, int x, int y)
{
    int over_dock = PointIn(g_dock_r, x, y);
    const LauncherEntry *e;

    if (g_drag.from_dock) {
        if (g_drag.index >= g_dock_count)
            return;
        if (over_dock) {
            DockMove(g_drag.index, DockSlotAt(x));
        } else {
            snprintf(g_status, sizeof(g_status), "Removed from dock: %s", g_dock[g_drag.index].name);
            DockRemove(g_drag.index);
        }
        SaveConfig();
        return;
    }
    if (!over_dock || g_drag.index >= entry_count)
        return;
    e = &entries[g_drag.index];
    if (g_dock_count >= g_dock_cap) {
        snprintf(g_status, sizeof(g_status), "Dock is full");
        return;
    }
    if (DockInsert(ctx, e->name, e->command, e->icon_override, DockSlotAt(x)) && SaveConfig())
        snprintf(g_status, sizeof(g_status), "Added to dock: %s", g_dock[DockFind(e->name, e->command)].name);
}

/* Na poczatku klatki: zbocze wcisniecia LPM (dla DragPress), przejscie w
 * przeciaganie po DRAG_THRESHOLD px i upuszczenie po zwolnieniu. Zwraca
 * 1, jesli W TEJ klatce zakonczylo sie przeciaganie - wtedy klik
 * (ui_hit_test, ten sam release) komorek ma byc zignorowany. */
static int
DragUpdate(UiCtx *ctx)
{
    int x, y, down, dropped = 0;

    ui_mouse_state(ctx, &x, &y, &down);
    g_drag.press_edge = down && !g_drag.was_down;
    g_drag.was_down = down;

    if (down && g_drag.pressed && !g_drag.active &&
        (abs(x - g_drag.press_x) > DRAG_THRESHOLD || abs(y - g_drag.press_y) > DRAG_THRESHOLD))
        g_drag.active = 1;

    if (!down && g_drag.pressed) {
        if (g_drag.active) {
            DragDrop(ctx, x, y);
            dropped = 1;
        }
        g_drag.pressed = g_drag.active = 0;
    }
    return dropped;
}

/* wcisniecie LPM nad komorka r - kandydat na przeciaganie */
static void
DragPress(UiCtx *ctx, UiRect r, int from_dock, int index)
{
    int x, y;

    if (!g_drag.press_edge || g_drag.pressed)
        return;
    ui_mouse_state(ctx, &x, &y, NULL);
    if (!PointIn(r, x, y))
        return;
    g_drag.pressed = 1;
    g_drag.from_dock = from_dock;
    g_drag.index = index;
    g_drag.press_x = x;
    g_drag.press_y = y;
}

/* "duch" przeciaganej ikony pod kursorem, na wierzchu wszystkiego */
static void
DrawDragGhost(UiCtx *ctx)
{
    const LauncherEntry *e;
    int x, y;

    if (!g_drag.active)
        return;
    if (g_drag.from_dock ? g_drag.index >= g_dock_count : g_drag.index >= entry_count)
        return;
    e = g_drag.from_dock ? &g_dock[g_drag.index] : &entries[g_drag.index];
    ui_mouse_state(ctx, &x, &y, NULL);
    DrawCell(ctx, e, x - CELL_W / 2, y - CELL_VPAD - ICON_SIZE / 2, 0);
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

/* kropki "stron" przewijania na torze miedzy strzalkami scrollbara -
 * pionowy odpowiednik DrawPageDots z utils/7atodo.c: jedna kropka na
 * strone (strona = page_step px, czyli tyle pelnych wierszy, ile miesci
 * viewport; ostatnia strona to zawsze max_scroll), biezaca wypelniona,
 * pozostale same obwodki. Gdy wszystkie sie nie mieszcza, widac okno
 * kropek wokol biezacej strony, a skrajne sa mniejsze, jesli za nimi sa
 * dalsze strony. Klik kropki ustawia g_scroll_y PRZED rysowaniem (stan
 * najpierw, potem rysowanie - patrz CLAUDE.md), wiec kropki i siatka
 * (rysowana po tej funkcji) od razu pokazuja nowa strone. */
static int
ScrollDotsCurrent(int pages, int page_step, int max_scroll)
{
    if (g_scroll_y >= max_scroll) return pages - 1;
    return (g_scroll_y + page_step / 2) / page_step;
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
DrawScrollDots(UiCtx *ctx, UiRect track, int page_step, int max_scroll)
{
    int pages = 1 + (max_scroll + page_step - 1) / page_step;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = ScrollDotsStart(visible, pages, ScrollDotsCurrent(pages, page_step, max_scroll));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            g_scroll_y = (start + i) * page_step;
            if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
            break;
        }
    }

    current = ScrollDotsCurrent(pages, page_step, max_scroll);
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

static void
DrawFormField(UiCtx *ctx, UiRect row, const char *label, char *buf, int cap, int *cur)
{
    UiRect label_r = { row.x, row.y, FORM_LABEL_W, row.h };
    UiRect field_r = { row.x + FORM_LABEL_W, row.y, row.w - FORM_LABEL_W, row.h };

    ui_label(ctx, label_r, label);
    ui_textbox(ctx, field_r, buf, cap, cur);
}

/* zewnetrzna wysokosc boxa (z marginesami) o zawartosci content_h -
 * liczona wprost, bo ui_box_height w pierwszej klatce zwraca 0 */
static int
BoxOuter(const UiBoxStyle *s, int content_h)
{
    return s->margin_t + 2 * s->border_w + s->padding_t + content_h
         + s->padding_b + s->margin_b;
}

static void
SetTabView(int list)
{
    if (g_tab_list[g_active_tab] == list)
        return;
    g_tab_list[g_active_tab] = (unsigned char) list;
    g_scroll_y = 0;
    g_need_redraw = 1;  /* przycisk przeciwny narysowal sie juz ze starym stanem */
}

/* Przycisk widoku List (list=1) / Grid (list=0) z rysowana ikonka
 * (tylko kolor fg). Rysunek PO
 * ui_button (tlo przycisku by go zakrylo - jak przy ui_draw_arrow). */
static int
ViewButton(UiCtx *ctx, UiRect r, int list)
{
    const XColor *fg = ui_theme_fg(ctx);
    int clicked = ui_button(ctx, r, "");
    int x = r.x + (r.w - VIEW_ICON) / 2, y = r.y + (r.h - VIEW_ICON) / 2;
    int half = VIEW_ICON / 2, i;

    if (list) {
        /* trzy wiersze: kropka-ikona + kreska-nazwa */
        for (i = 0; i < 3; i++) {
            ui_fill_rect(ctx, (UiRect){ x, y + i * 5, 2, 2 }, fg);
            ui_fill_rect(ctx, (UiRect){ x + 4, y + i * 5, VIEW_ICON - 4, 2 }, fg);
        }
    } else {
        /* 2x2 kwadraty */
        for (i = 0; i < 4; i++)
            ui_fill_rect(ctx, (UiRect){ x + (i % 2) * (half + 1), y + (i / 2) * (half + 1),
                                        half - 1, half - 1 }, fg);
    }
    return clicked;
}

/* Przycisk pokaz/schowaj dock: ikonka okna z paskiem na dole (tylko kolor fg). */
static int
DockButton(UiCtx *ctx, UiRect r)
{
    const XColor *fg = ui_theme_fg(ctx);
    int clicked = ui_button(ctx, r, "");
    UiRect icon = { r.x + (r.w - VIEW_ICON) / 2, r.y + (r.h - VIEW_ICON) / 2, VIEW_ICON, VIEW_ICON };

    ui_draw_border(ctx, icon, 1, fg);
    ui_fill_rect(ctx, (UiRect){ icon.x, icon.y + icon.h - 4, icon.w, 4 }, fg);
    return clicked;
}

/* Przycisk Edit/Done: olowek poza trybem edycji, "ptaszek" w nim
 * (tylko kolor fg). Rysunek PO ui_button, jak w ViewButton. */
static int
EditButton(UiCtx *ctx, UiRect r)
{
    const XColor *fg = ui_theme_fg(ctx);
    int clicked = ui_button(ctx, r, "");
    int x = r.x + (r.w - VIEW_ICON) / 2, y = r.y + (r.h - VIEW_ICON) / 2;
    int s = VIEW_ICON - 1;

    if (g_edit_mode) {
        ui_draw_line(ctx, x, y + s / 2, x + s / 3, y + s - 2, 2, fg);
        ui_draw_line(ctx, x + s / 3, y + s - 2, x + s, y + 1, 2, fg);
    } else {
        /* trzonek po skosie + grot w lewym dolnym rogu */
        ui_draw_line(ctx, x + s, y, x + 4, y + s - 4, 3, fg);
        ui_fill_triangle(ctx, x, y + s, x + 1, y + s - 5, x + 5, y + s - 1, fg);
    }
    return clicked;
}

static void
ToggleEditMode(UiCtx *ctx)
{
    g_edit_mode = !g_edit_mode;
    g_tab_mode = 0;
    if (g_edit_mode && g_selected_index >= 0)
        FormLoad(g_selected_index);
    else
        FormReset();
    ui_set_focus(ctx, NULL);
    /* boxy nizej zmieniaja polozenie - ich tlo z cache poprzedniej
     * klatki jest w tej klatce nieaktualne */
    g_need_redraw = 1;
}

/* Kolumna przyciskow na lewo od boxu siatki, jeden pod drugim:
 * Edit/Done, List/Grid aktywnej zakladki, pokaz/schowaj dock. Rysowana
 * PRZED siatka, wiec przelaczenie List/Grid widac juz w tej klatce. */
static void
DrawViewColumn(UiCtx *ctx, UiRect col)
{
    UiRect r = { col.x, col.y, col.w, ROW_H };

    if (EditButton(ctx, r))
        ToggleEditMode(ctx);
    r.y += ROW_H + FORM_GAP;
    if (g_tab_count > 0) {
        if (ViewButton(ctx, r, 1))
            SetTabView(1);
        r.y += ROW_H + FORM_GAP;
        if (ViewButton(ctx, r, 0))
            SetTabView(0);
        r.y += ROW_H + FORM_GAP;
    }
    if (DockButton(ctx, r)) {
        g_dock_hidden = !g_dock_hidden;
        g_dock_r = (UiRect){ 0, 0, 0, 0 };  /* schowany dock nie przyjmuje upuszczen */
        g_need_redraw = 1;  /* wysokosc boxa siatki w tej klatce jest juz nieaktualna */
    }
}

/* Przycisk "+" przy zakladkach: pokazuje/chowa formularz zakladek.
 * Rysunek PO ui_button, jak w ViewButton. */
static int
TabPlusButton(UiCtx *ctx, UiRect r)
{
    const XColor *fg = ui_theme_fg(ctx);
    int clicked = ui_button(ctx, r, "");
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, h = VIEW_ICON / 2;

    ui_fill_rect(ctx, (UiRect){ cx - h, cy - 1, 2 * h, 2 }, fg);
    ui_fill_rect(ctx, (UiRect){ cx - 1, cy - h, 2, 2 * h }, fg);
    return clicked;
}

static void
ToggleTabMode(UiCtx *ctx)
{
    g_tab_mode = !g_tab_mode;
    g_edit_mode = 0;
    g_tab_name[0] = '\0';
    g_tab_name_cur = 0;
    ui_set_focus(ctx, g_tab_mode ? g_tab_name : NULL);
    g_need_redraw = 1;  /* boxy nizej zmieniaja polozenie */
}

/* Grubosc linii pod zakladkami: rowek przezroczystego boxa (dwie ramki
 * border_w) + 1 px wewnetrznego obrysu 3D. */
static int
TabLineH(const UiBoxStyle *style)
{
    return 2 * style->border_w + 1;
}

/* Pod zakladkami tylko GORNA krawedz rowka przezroczystego boxa (bez
 * bokow i dolu): ramki boxa rysowane na prostokacie szerszym od okna,
 * przyciete do paska linii - rogi i boki wypadaja poza clip. */
static void
DrawTabLine(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    int bw = style->border_w, pad = 4 * ROW_H;
    int x0 = style->margin_l + ROW_H + FORM_GAP;  /* za kolumna ikon, jak box siatki */
    UiRect outer = { -pad, y, win_w + 2 * pad, 2 * pad };
    UiRect in = { outer.x + bw, outer.y + bw, outer.w - 2 * bw, outer.h - 2 * bw };

    ui_set_clip(ctx, (UiRect){ x0, y, win_w - x0 - style->margin_r, TabLineH(style) });
    ui_draw_bevel(ctx, outer, bw, &style->border_color, 1);
    ui_draw_bevel(ctx, in, bw, &style->border_color, 0);
    ui_draw_inner_bevel(ctx, in, bw, &style->border_color, ui_theme_bg(ctx), 0);
    ui_clear_clip(ctx);
}

/* Rzad zakladek z linia pod nimi (zamiast pustego boxa): "+" na lewym
 * koncu, w pionie z kolumna przyciskow widoku. Aktywna zakladka wymazuje
 * linie pod soba (ui_tabs), wiec rysowana PO linii; jej klik zmienia stan
 * PRZED rysowaniem boxow nizej. y = gorna krawedz linii. */
static void
DrawTabRow(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    const char *tab_labels[MAX_TABS];
    UiRect tabs_r, plus_r;
    int i, t = g_active_tab;

    DrawTabLine(ctx, style, win_w, y);
    /* "+" w pionie z kolumna przyciskow widoku (ta sama x i szerokosc),
     * zakladki zaczynaja sie dopiero za nim */
    plus_r = (UiRect){ style->margin_l, y + TabLineH(style) - (TAB_H - 2), ROW_H, TAB_H - 2 };
    tabs_r = (UiRect){ plus_r.x + ROW_H + FORM_GAP, y - TAB_H,
                       win_w - style->margin_r - TAB_INDENT - (plus_r.x + ROW_H + FORM_GAP),
                       TAB_H };
    if (TabPlusButton(ctx, plus_r))
        ToggleTabMode(ctx);
    for (i = 0; i < g_tab_count; i++)
        tab_labels[i] = g_tabs[i];
    if (g_tab_count > 0 && ui_tabs(ctx, tabs_r, tab_labels, g_tab_count, &t, style))
        SwitchTab(t);
}

/* Drugi box (tylko w trybie Edit): pola pozycji AKTYWNEJ zakladki +
 * przyciski. Klik ikony w siatce laduje ja do pol (DrawCell -> FormLoad),
 * "New" czysci pola do nowej pozycji. */
static void
DrawFormBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    UiBox *box;
    UiRect in;

    box = ui_box_begin(ctx, "form", 0, y, win_w, style);
    in = ui_box_next_rect(box, ROW_H);
    DrawFormField(ctx, in, "Name:", g_form.name, (int) sizeof(g_form.name), &g_form.name_cur);
    in = ui_box_next_rect(box, ROW_H);
    DrawFormField(ctx, in, "Command:", g_form.command, (int) sizeof(g_form.command), &g_form.command_cur);
    in = ui_box_next_rect(box, ROW_H);
    DrawFormField(ctx, in, "Icon:", g_form.icon, (int) sizeof(g_form.icon), &g_form.icon_cur);

    in = ui_box_next_rect(box, ROW_H);
    if (ui_button(ctx, ui_rect_col(in, 0, 3, FORM_GAP), g_form.index < 0 ? "Add" : "Save"))
        FormSubmit(ctx);
    else if (ui_button(ctx, ui_rect_col(in, 1, 3, FORM_GAP), "Delete"))
        FormDeleteEntry();
    else if (ui_button(ctx, ui_rect_col(in, 2, 3, FORM_GAP), "New")) {
        SelectEntry(-1);
        FormReset();
    } else {
        ui_box_end(box);
        return;
    }
    ui_box_end(box);
    ui_set_focus(ctx, NULL);
    /* przyciski leza pod polami - pola tej klatki pokazuja jeszcze stan
     * sprzed akcji */
    g_need_redraw = 1;
}

/* Box zakladek (tylko po "+" przy zakladkach): nazwa + Add (nowa
 * zakladka) / Rename (aktywna) / Delete (aktywna, z potwierdzeniem). */
static void
DrawTabFormBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    static int delete_tab_armed = 0;  /* 1 + indeks zakladki czekajacej na potwierdzenie */
    UiBox *box;
    UiRect in;
    int armed_before = delete_tab_armed;

    box = ui_box_begin(ctx, "tabform", 0, y, win_w, style);
    in = ui_box_next_rect(box, ROW_H);
    DrawFormField(ctx, in, "Tab:", g_tab_name, (int) sizeof(g_tab_name), &g_tab_name_cur);

    in = ui_box_next_rect(box, ROW_H);
    if (ui_button(ctx, ui_rect_col(in, 0, 3, FORM_GAP), "Add tab"))
        FormAddTab();
    else if (ui_button(ctx, ui_rect_col(in, 1, 3, FORM_GAP), "Rename tab"))
        FormRenameTab();
    else if (ui_button(ctx, ui_rect_col(in, 2, 3, FORM_GAP), "Delete tab"))
        FormDeleteTab(&delete_tab_armed);
    else {
        ui_box_end(box);
        return;
    }
    ui_box_end(box);

    /* kazda inna akcja rozbraja potwierdzenie "Delete tab" */
    if (delete_tab_armed == armed_before)
        delete_tab_armed = 0;
    g_need_redraw = 1;
}

/* Box 4: dock - jeden rzad komorek z ulubionymi ikonami (sekcja [@Dock]
 * w center.conf). Pojedynczy klik uruchamia; wcisniecie zaczyna
 * przeciaganie (zmiana kolejnosci / wyciagniecie poza dock = usuniecie). */
static void
DrawDockBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y, int dropped)
{
    UiBox *box;
    UiRect row;
    int i, shown;

    /* ta sama szerokosc co box siatki: za kolumna przyciskow widoku */
    box = ui_box_begin(ctx, "dock", ROW_H + FORM_GAP, y, win_w - ROW_H - FORM_GAP, style);
    row = ui_box_next_rect(box, CELL_H);
    g_dock_r = row;
    g_dock_cap = (row.w + CELL_GAP) / (CELL_W + CELL_GAP);
    if (g_dock_cap > DOCK_MAX) g_dock_cap = DOCK_MAX;
    if (g_dock_cap < 0) g_dock_cap = 0;

    shown = g_dock_count < g_dock_cap ? g_dock_count : g_dock_cap;
    if (shown == 0 && !g_drag.active)
        ui_label_centered(ctx, row, "Drag icons here");
    for (i = 0; i < shown; i++) {
        UiRect cell_r = { row.x + i * (CELL_W + CELL_GAP), row.y, CELL_W, CELL_H };

        DragPress(ctx, cell_r, 1, i);
        if (!dropped && ui_hit_test(ctx, cell_r))
            LaunchEntry(&g_dock[i]);
        DrawCell(ctx, &g_dock[i], cell_r.x, cell_r.y, 0);
    }
    ui_box_end(box);
}

/* Pole wyszukiwarki po prawej stronie paska statusu r: Enter uruchamia
 * pierwsza pasujaca pozycje, Esc czysci filtr. Zwraca szerokosc, ktora
 * zostaje na tekst statusu. */
static int
DrawSearchField(UiCtx *ctx, UiRect r)
{
    static int label_w = 0;
    UiRect field_r, label_r;
    int i;

    if (label_w == 0)
        label_w = ui_text_width(ctx, "Search:") + 4;
    field_r = (UiRect){ r.x + r.w - SEARCH_W, r.y, SEARCH_W, r.h };
    label_r = (UiRect){ field_r.x - label_w, r.y, label_w, r.h };

    if (ui_textbox_key(ctx, g_query, XK_Escape)) {
        g_query[0] = '\0';
        g_query_cur = 0;
        ui_set_focus(ctx, NULL);
    } else if (ui_textbox_key(ctx, g_query, XK_Return) ||
               ui_textbox_key(ctx, g_query, XK_KP_Enter)) {
        for (i = 0; i < entry_count && !EntryShown(i); i++)
            ;
        if (i < entry_count) {
            SelectEntry(i);
            LaunchEntry(&entries[i]);
        } else {
            snprintf(g_status, sizeof(g_status), "No match");
        }
    }
    ui_label(ctx, label_r, "Search:");
    ui_textbox(ctx, field_r, g_query, (int) sizeof(g_query), &g_query_cur);
    return label_r.x - FORM_GAP - r.x;
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle top_style, form_style, info_style;
    static int ready = 0;
    static const char *palette_names[PALETTE_SIZE] = {
        "steelblue", "indianred", "seagreen", "goldenrod",
        "mediumpurple", "darkorange", "teal", "slategray",
    };
    UiRect status_row;
    UiRect viewport_r, grid_r, sb_r, up_arrow_r, down_arrow_r, track_r;
    UiBox *box;
    int y, viewport_h, dropped;
    int grid_w, columns, rows_total, content_h, max_scroll;
    int first_row, last_row, k;
    int list, cell_w, cell_h, pitch_y;
    int i;

    if (!ready) {
        for (i = 0; i < PALETTE_SIZE; i++)
            ui_color(ctx, palette_names[i], &g_palette[i]);
        ui_color(ctx, "white", &g_letter_c);
        g_outline_c = *ui_theme_line_fg(ctx);
        g_select_c = *ui_theme_accent(ctx);

        form_style = (UiBoxStyle){0};
        form_style.margin_l = form_style.margin_r = ui_window_margin(ctx);
        form_style.margin_t = form_style.margin_b = BOX_MARGIN_TB;
        form_style.padding_l = form_style.padding_r = ui_box_padding(ctx);
        form_style.padding_t = form_style.padding_b = BOX_PAD_TB;
        form_style.border_w = 1;
        form_style.gap = FORM_GAP;
        form_style.border_color = *ui_theme_line_fg(ctx);
        form_style.bg_color = *ui_theme_box_bg(ctx);
        top_style = form_style;
        top_style.transparent = 1;
        info_style = form_style;
        info_style.bg_color = *ui_theme_box_info_bg(ctx);
        ready = 1;
    }

    /* przeciaganie ikon PRZED rysowaniem - upuszczenie zmienia dock/
     * konczy klik, wiec ta klatka ma juz pokazac nowy stan */
    dropped = DragUpdate(ctx);

    /* --- zakladki z linia pod nimi; boxy nizej odsuwa ich margin_t --- */
    y = MARGIN_Y + TAB_H;
    DrawTabRow(ctx, &top_style, win_w, y);
    y += TabLineH(&top_style);

    /* --- box 2: formularz konfiguratora, tylko w trybie Edit --- */
    if (g_edit_mode) {
        DrawFormBox(ctx, &form_style, win_w, y);
        y += BoxOuter(&form_style, FORM_ROWS * ROW_H + (FORM_ROWS - 1) * form_style.gap);
    }
    /* --- albo formularz zakladek, po "+" przy zakladkach --- */
    if (g_tab_mode) {
        DrawTabFormBox(ctx, &form_style, win_w, y);
        y += BoxOuter(&form_style, TAB_FORM_ROWS * ROW_H + (TAB_FORM_ROWS - 1) * form_style.gap);
    }

    /* --- pasek statusu, na dole --- */
    status_row = (UiRect){ ui_window_margin(ctx), win_h - MARGIN_Y - ROW_H,
                           win_w - 2 * ui_window_margin(ctx), ROW_H };
    /* wyszukiwarka PRZED siatka - filtr z tej klatki od razu ja zmienia */
    status_row.w = DrawSearchField(ctx, status_row);

    /* --- box 3 (informacyjny): siatka ikon + scrollbar, do docka.    */
    /* Geometria siatki/scrollbara przejeta wprost z utils/7afm.c.    */
    viewport_h = status_row.y - y - BoxOuter(&info_style, 0)
               - (g_dock_hidden ? 0 : BoxOuter(&form_style, CELL_H));
    /* min. cztery przyciski kolumny widoku (i dwie strzalki scrollbara) */
    if (viewport_h < 4 * ROW_H + 3 * FORM_GAP) viewport_h = 4 * ROW_H + 3 * FORM_GAP;
    /* kolumna przyciskow widoku poza boxem, na lewo od niego; box
     * przesuniety w prawo o jej szerokosc + odstep */
    DrawViewColumn(ctx, (UiRect){ info_style.margin_l, y + info_style.margin_t, ROW_H, viewport_h });
    box = ui_box_begin(ctx, "grid", ROW_H + FORM_GAP, y, win_w - ROW_H - FORM_GAP, &info_style);
    viewport_r = ui_box_next_rect(box, viewport_h);
    g_viewport_r = viewport_r;

    grid_w = viewport_r.w - SCROLLBAR_W - SB_PAD;
    if (grid_w < CELL_W) grid_w = CELL_W;
    list = g_tab_list[g_active_tab];
    if (list) {
        columns = 1;
        cell_w = grid_w;
        cell_h = LIST_ROW_H;
        pitch_y = LIST_ROW_H + LIST_GAP;
    } else {
        columns = (grid_w + CELL_GAP) / (CELL_W + CELL_GAP);
        if (columns < 1) columns = 1;
        cell_w = CELL_W;
        cell_h = CELL_H;
        pitch_y = CELL_H + CELL_GAP;
    }
    g_row_pitch = pitch_y;
    rows_total = (ShownCount() + columns - 1) / columns;
    if (rows_total < 1) rows_total = 1;
    content_h = rows_total * pitch_y;

    max_scroll = content_h - viewport_r.h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
    if (g_scroll_y < 0) g_scroll_y = 0;

    grid_r = (UiRect){ viewport_r.x, viewport_r.y, grid_w, viewport_r.h };

    /* scrollbar PRZED siatka - jego klikniecia zmieniaja g_scroll_y, a
     * siatka ma od razu rysowac sie z nowym przewinieciem */
    sb_r = (UiRect){ grid_r.x + grid_r.w + SB_PAD, viewport_r.y, SCROLLBAR_W, viewport_r.h };
    {
        int arrow_h = ROW_H;

        up_arrow_r = (UiRect){ sb_r.x, sb_r.y, sb_r.w, arrow_h };
        down_arrow_r = (UiRect){ sb_r.x, sb_r.y + sb_r.h - arrow_h, sb_r.w, arrow_h };
        track_r = (UiRect){ sb_r.x, sb_r.y + arrow_h, sb_r.w, sb_r.h - 2 * arrow_h };
        if (track_r.h < 0) track_r.h = 0;
    }
    {
        int up_clicked = ui_button(ctx, up_arrow_r, "");
        int down_clicked = ui_button(ctx, down_arrow_r, "");

        ui_draw_arrow(ctx, up_arrow_r, UI_ARROW_UP);
        ui_draw_arrow(ctx, down_arrow_r, UI_ARROW_DOWN);

        if (up_clicked) g_scroll_y -= pitch_y;
        if (down_clicked) g_scroll_y += pitch_y;
    }
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
    if (g_scroll_y < 0) g_scroll_y = 0;

    {
        /* strona = pelne wiersze mieszczace sie w viewporcie */
        int page_step = (viewport_r.h / pitch_y) * pitch_y;

        if (page_step < pitch_y) page_step = pitch_y;
        DrawScrollDots(ctx, track_r, page_step, max_scroll);
    }

    ui_set_clip(ctx, grid_r);

    first_row = g_scroll_y / pitch_y;
    last_row = (g_scroll_y + viewport_r.h) / pitch_y;
    if (last_row >= rows_total) last_row = rows_total - 1;

    /* k = pozycja w aktywnej zakladce (kolejnosc z configu) */
    for (i = 0, k = 0; i < entry_count; i++) {
        UiRect cell_r;
        int row, col, mx, my;

        if (!EntryShown(i))
            continue;
        row = k / columns;
        col = k % columns;
        k++;
        if (row < first_row || row > last_row)
            continue;
        cell_r = (UiRect){ grid_r.x + col * (CELL_W + CELL_GAP),
                           grid_r.y + row * pitch_y - g_scroll_y, cell_w, cell_h };
        /* komorka czesciowo poza viewportem - mysz tylko w jego widocznej czesci */
        ui_mouse_state(ctx, &mx, &my, NULL);
        if (PointIn(grid_r, mx, my)) {
            DragPress(ctx, cell_r, 0, i);
            if (!dropped && ui_hit_test(ctx, cell_r))
                GridCellClick(i);
        }
        if (list)
            DrawListRow(ctx, &entries[i], cell_r, i == g_selected_index);
        else
            DrawCell(ctx, &entries[i], cell_r.x, cell_r.y, i == g_selected_index);
    }
    ui_clear_clip(ctx);
    ui_box_end(box);
    y += BoxOuter(&info_style, viewport_h);

    /* --- box 4: dock z ulubionymi ikonami, nad paskiem statusu --- */
    if (!g_dock_hidden)
        DrawDockBox(ctx, &form_style, win_w, y, dropped);

    ui_label_ellipsis(ctx, status_row, g_status);
    DrawDragGhost(ctx);

    return 0;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    XSizeHints *sizehints;
    int win_w = 420, win_h = 360;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    char app_name[64] = "7aCenter";
    char app_title[64] = "";
    XEvent ev;

    signal(SIGCHLD, SIG_IGN);

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[i + 1]);
            i++;
        }
    }

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w utils/7afm.c (patrz komentarz
     * tam): to launcher DOWOLNYCH programow z ~/.7a/center.conf
     * (fork+execvp argv[0] z configu uzytkownika w LaunchSelected), wiec
     * unveil dziedziczony po exec ograniczalby wlasnie te programy, ktore
     * apka ma uruchamiac. wpath/cpath - konfigurator przepisuje
     * center.conf (mkdir ~/.7a, mkstemp, rename, unlink w SaveConfig). */
    if (pledge("stdio rpath wpath cpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    g_dpy = dpy;

    /* Dodatkowe katalogi ikon z zasobu X - patrz naglowek pliku i
     * komentarz przy g_extra_icon_dirs_buf. Musi sie stac PRZED
     * pierwszym LoadEntries() (parsuje tez wpisy configu w tej samej
     * klatce). strtok_r (nie strtok) - g_extra_icon_dirs_buf jest
     * globalne, wiec nic innego w tej apce go rownolegle nie uzywa, ale
     * strtok_r jest tanszy do audytu (jawny wskaznik stanu zamiast
     * ukrytego stanu statycznego strtok). */
    ui_resource_string(dpy, "7aCenter.iconPath", "7aCenter.IconPath",
                  g_extra_icon_dirs_buf, sizeof(g_extra_icon_dirs_buf), "");
    {
        char *tok, *saveptr;

        for (tok = strtok_r(g_extra_icon_dirs_buf, ":", &saveptr);
             tok && g_extra_icon_dirs_count < MAX_EXTRA_ICON_DIRS;
             tok = strtok_r(NULL, ":", &saveptr))
            g_extra_icon_dirs[g_extra_icon_dirs_count++] = tok;
    }

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    if (geom_mask & WidthValue) win_w = (int) geom_w;
    if (geom_mask & HeightValue) win_h = (int) geom_h;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative) ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative) ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;

    win = XCreateSimpleWindow(dpy, root, win_x, win_y, win_w, win_h, 0,
                               BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    g_win = win;
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7aCenter";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 180;
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, apps_48_width, apps_48_height, apps_48_bits);

    /* Narysuj OD RAZU jedna (pusta) klatke, ZANIM LoadEntries() przeczyta
     * config - ten sam wzorzec co w utils/7afm.c/7aweather.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    LoadEntries(ctx);

    running = 1;
    redraw = 1;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy (Button4/5) przechwycone TU, PRZED
             * ui_feed_event - identyczny wzorzec/uzasadnienie co w
             * utils/7afm.c. */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress &&
                    ev.xbutton.x >= g_viewport_r.x && ev.xbutton.x < g_viewport_r.x + g_viewport_r.w &&
                    ev.xbutton.y >= g_viewport_r.y && ev.xbutton.y < g_viewport_r.y + g_viewport_r.h) {
                    g_scroll_y += (ev.xbutton.button == Button4) ? -WHEEL_STEP : WHEEL_STEP;
                    if (g_scroll_y < 0) g_scroll_y = 0;
                    redraw = 1;
                }
                continue;
            }

            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
                break;
            case ButtonPress:
            case ButtonRelease:
                redraw = 1;
                break;
            case MotionNotify:
            case KeyPress:
                redraw = 1;
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

        /* akcja w draw() (zakladka/formularz) zmienila stan po narysowaniu
         * czesci okna - druga klatka od razu, bez czekania na event */
        while (redraw) {
            g_need_redraw = 0;
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h)) running = 0;
            ui_end_frame(ctx);
            redraw = g_need_redraw;
        }
        if (!running)
            break;

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy);

            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            select(xfd + 1, &rfds, NULL, NULL, NULL);
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
