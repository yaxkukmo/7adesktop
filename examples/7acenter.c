/*
 * 7acenter.c - launcher programow, NOWA apka (nie port Xt/Xaw - w
 * przeciwienstwie do wiekszosci examples/7a*.c, patrz np. naglowek
 * examples/7afm.c). Architektura siatki ikon + scrollbara jest jednak
 * wprost oparta o examples/7afm.c: ta sama geometria kolumn/wierszy,
 * ten sam wzorzec scrollbara (strzalki, przeciaganie kciuka przez
 * ui_mouse_state, klik nad/pod kciukiem = strona gora/dol) i to samo
 * rozroznienie klik=zaznaczenie / dwuklik=aktywacja (DOUBLE_CLICK_MS).
 *
 * Swiadomie POMINIETE wzgledem 7afm.c (bo tu nie ma zastosowania):
 *  - klasyfikacja plikow (KIND_*) - kazda pozycja to jeden zdefiniowany
 *    w configu program, bez rozroznienia typu;
 *  - XDND i schowek Copy/Cut/Paste - nie ma czego przeciagac/wklejac
 *    miedzy oknami launchera;
 *  - rename/delete i pasek menu File/Edit/View - launcher tylko
 *    uruchamia programy, nie zarzadza plikami. Jedyna akcja poza
 *    uruchomieniem to "Reload" (przeladowanie configu), zwykly przycisk
 *    w gornym pasku zamiast dropdownu.
 *
 * Zrodlo listy programow: ~/.7a/center.conf, jedna pozycja na linie w
 * formacie "Nazwa|komenda argumenty" (# = komentarz, puste linie
 * ignorowane). Patrz center.conf.sample w katalogu glownym repo po
 * pelny opis formatu. Plik NIE jest tworzony automatycznie - brak
 * pozycji to tylko komunikat w pasku statusu, zeby apka nigdy nie
 * pisala na dysk bez wyraznej akcji uzytkownika.
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
 * istnieje) dostaja kolor tla siatki (ui_theme_box_bg) zamiast koloru z
 * pliku - wypiekane RAZ przy ladowaniu, bo ui_draw_pixmap w ui.h/ui.c
 * nadal nie zna pojecia prawdziwej maski/przezroczystosci (patrz komentarz
 * przy ComposeIconPixmap po pelne uzasadnienie i znany kompromis przy
 * zaznaczonej komorce).
 */

#define _DEFAULT_SOURCE  /* execvp/fork - patrz ta sama uwaga w examples/7aweather.c */

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include "../ui.h"

#define ICON_SIZE 32
#define CELL_W 80
#define CELL_H 64
#define CELL_GAP 6
#define CELL_VPAD 4
#define SCROLLBAR_W 14
#define ROW_H 20
#define MARGIN_Y 8
#define DOUBLE_CLICK_MS 400      /* brak Xt -> brak XtGetMultiClickTime, patrz ta sama
                                    uwaga przy tej stalej w examples/7afm.c */
#define WHEEL_STEP (CELL_H + CELL_GAP)
#define MAX_CMD_TOKENS 32
#define TOP_BUTTON_W 60
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
    Pixmap icon;               /* None, jesli has_icon == 0 */
    int has_icon;
} LauncherEntry;

static LauncherEntry *entries = NULL;
static int entry_count = 0;
static int entry_cap = 0;

/* Potrzebne do zaladowania ikon XPM (XpmReadFileToPixmap) z LoadEntries(),
 * ktora jest wolana TEZ z przycisku Reload wewnatrz draw() (bez dostepu do
 * surowego Display/Window, ui.h celowo nie eksponuje ich z UiCtx) - ten
 * sam wzorzec globali co g_dpy/g_win w examples/7afm.c. */
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
static char g_status[1280] = "";  /* musi pomiescic g_conf_path (do 1200B) w komunikatach o braku/pustym configu */
static char g_conf_path[1200] = "";

static int g_scroll_y = 0;
static UiRect g_viewport_r = { 0, 0, 0, 0 };  /* z ostatniej klatki - do kolka myszy w main() */

/* sesja przeciagania kciuka scrollbara - wlasnosc APKI, ten sam wzorzec
 * (i te same nazwy) co w examples/7afm.c, patrz komentarz tam przy
 * ui_mouse_state w ui.h po pelne uzasadnienie. */
static int g_thumb_dragging = 0;
static int g_drag_start_my = 0;
static int g_drag_start_scroll = 0;

static int g_last_click_index = -1;
static long g_last_click_ms = 0;

/* -------------------------------------------------------------------- */
/* Config - wlasny plik tej apki (~/.7a/center.conf, patrz naglowek);    */
/* jedyny zasob X, jaki 7acenter czyta, to dodatkowe katalogi ikon       */
/* ("7aCenter.iconPath") - patrz ReadAppString/main() nizej.             */
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

/* Zasoby X specyficzne dla tej apki (nie globalny motyw ui.c) - ten sam
 * wzorzec co ReadAppString w examples/7afm.c/7atodo.c: klasa "7aCenter."
 * (z duza literka na start, jak "7aTodo."), zeby nie kolidowac z zadnym
 * globalnym wpisem ui.c. */
static void
ReadAppString(Display *dpy, const char *name, const char *class_,
              char *out, size_t outsz, const char *dflt)
{
    char *rms;
    XrmDatabase rdb;

    snprintf(out, outsz, "%s", dflt);

    rms = XResourceManagerString(dpy);
    rdb = rms ? XrmGetStringDatabase(rms) : NULL;
    if (rdb) {
        char *type;
        XrmValue value;

        if (XrmGetResource(rdb, name, class_, &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            snprintf(out, outsz, "%s", value.addr);
        XrmDestroyDatabase(rdb);
    }
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

/* Doskalowuje Pixmap src (sw x sh, ta sama glebia co win) do ICON_SIZE x
 * ICON_SIZE najblizszym sasiadem, czystym Xlib (XGetImage/XPutImage) -
 * bez zaleznosci od biblioteki skalujacej. Jesli mask != None (maska
 * ksztaltu z XpmReadFileToPixmap, TEJ SAMEJ wielkosci co src - tak
 * zawsze generuje ja libXpm), piksele spoza maski dostaja kolor tla
 * komorki siatki (ui_theme_box_bg) zamiast koloru z pliku .xpm - w ten
 * sposob "przezroczyste" fragmenty ikony (np. zaokraglone rogi) zlewaja
 * sie z tlem zamiast rysowac sie jako czarny prostokat. ui_draw_pixmap w
 * ui.h/ui.c NADAL nie zna pojecia maski/przezroczystosci (patrz komentarz
 * tam) - to podejscie polega na tym, ze tlo siatki jest jednolitym
 * kolorem znanym W MOMENCIE LADOWANIA ikony, wiec przezroczystosc mozna
 * "wypiec" raz, tutaj, zamiast prawdziwie maskowac przy KAZDYM rysowaniu
 * (co wymagaloby laczenia maski ksztaltu z prostokatnym przycieciem
 * viewportu w jednym GC clipie - ui.c go dla prostoty NIE udostepnia).
 * Znany kompromis: gdy komorka jest ZAZNACZONA (inne tlo, select_c
 * zamiast box_bg - patrz DrawCell), "przezroczyste" fragmenty ikony nadal
 * pokazuja box_bg, nie select_c - drobna niespojnosc kolorystyczna tylko
 * w tym jednym, przejsciowym stanie. Zwraca None przy bledzie (wywolujacy
 * spada wtedy na ikone proceduralna). */
static Pixmap
ComposeIconPixmap(UiCtx *ctx, Pixmap src, Pixmap mask, int sw, int sh)
{
    XWindowAttributes wattr;
    XImage *src_img, *mask_img = NULL, *dst_img;
    Pixmap dst;
    GC gc;
    unsigned long bg_pixel = ui_theme_box_bg(ctx)->pixel;
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
                            ICON_SIZE, ICON_SIZE, 32, 0);
    if (!dst_img) {
        XDestroyImage(src_img);
        if (mask_img) XDestroyImage(mask_img);
        return None;
    }
    dst_img->data = malloc((size_t) dst_img->bytes_per_line * ICON_SIZE);
    if (!dst_img->data) {
        XDestroyImage(src_img);
        if (mask_img) XDestroyImage(mask_img);
        XDestroyImage(dst_img);  /* data == NULL, XFree(NULL) w srodku jest bezpieczne */
        return None;
    }

    for (y = 0; y < ICON_SIZE; y++) {
        int sy = y * sh / ICON_SIZE;

        if (sy >= sh) sy = sh - 1;
        for (x = 0; x < ICON_SIZE; x++) {
            int sx = x * sw / ICON_SIZE;
            unsigned long pixel;

            if (sx >= sw) sx = sw - 1;
            pixel = (mask_img && !XGetPixel(mask_img, sx, sy))
                    ? bg_pixel : XGetPixel(src_img, sx, sy);
            XPutPixel(dst_img, x, y, pixel);
        }
    }
    XDestroyImage(src_img);
    if (mask_img) XDestroyImage(mask_img);

    dst = XCreatePixmap(g_dpy, g_win, ICON_SIZE, ICON_SIZE, wattr.depth);
    gc = XCreateGC(g_dpy, dst, 0, NULL);
    XPutImage(g_dpy, dst, gc, dst_img, 0, 0, 0, 0, ICON_SIZE, ICON_SIZE);
    XFreeGC(g_dpy, gc);
    XDestroyImage(dst_img);  /* zwalnia tez dst_img->data (malloc powyzej) */

    return dst;
}

/* Proba zaladowania JEDNEGO konkretnego pliku .xpm do e->icon (doskalowujac
 * w razie potrzeby). Zwraca 1 przy sukcesie (e->has_icon tez ustawione), 0
 * gdy plik nie istnieje/nie jest poprawnym .xpm/ma podejrzany rozmiar -
 * wspolny rdzen dla obu sciezek w LoadIconForEntry nizej (bezwzglednej
 * sciezki z configu i przeszukiwania katalogow). */
static int
TryLoadXpmPath(UiCtx *ctx, LauncherEntry *e, const char *path)
{
    Pixmap loaded = None, mask = None;
    XpmAttributes attrs;

    memset(&attrs, 0, sizeof(attrs));
    if (XpmReadFileToPixmap(g_dpy, g_win, path, &loaded, &mask, &attrs) != XpmSuccess)
        return 0;

    if (attrs.width == 0 || attrs.height == 0 ||
        attrs.width > ICON_MAX_SRC || attrs.height > ICON_MAX_SRC) {
        XFreePixmap(g_dpy, loaded);
        if (mask != None) XFreePixmap(g_dpy, mask);
        XpmFreeAttributes(&attrs);
        return 0;
    }

    if (mask == None && attrs.width == ICON_SIZE && attrs.height == ICON_SIZE) {
        e->icon = loaded;  /* szybka sciezka: bez maski (pelna nieprzezroczystosc)
                               i juz wlasciwego rozmiaru - nic do przetworzenia */
    } else {
        e->icon = ComposeIconPixmap(ctx, loaded, mask, (int) attrs.width, (int) attrs.height);
        XFreePixmap(g_dpy, loaded);
        if (mask != None) XFreePixmap(g_dpy, mask);
    }
    XpmFreeAttributes(&attrs);

    e->has_icon = (e->icon != None);
    return e->has_icon;
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

    if (!g_dpy)
        return;

    if (e->icon_override[0] == '/') {
        TryLoadXpmPath(ctx, e, e->icon_override);
        return;
    }

    if (e->icon_override[0] != '\0')
        snprintf(base, sizeof(base), "%s", e->icon_override);
    else
        FirstCommandToken(e->command, base, sizeof(base));
    if (base[0] == '\0')
        return;

    for (i = 0; i < (size_t) g_extra_icon_dirs_count; i++) {
        char path[1024];

        snprintf(path, sizeof(path), "%s/%s.xpm", g_extra_icon_dirs[i], base);
        if (TryLoadXpmPath(ctx, e, path))
            return;
    }
    for (i = 0; i < sizeof(kIconDirs) / sizeof(kIconDirs[0]); i++) {
        char path[1024];

        snprintf(path, sizeof(path), "%s/%s.xpm", kIconDirs[i], base);
        if (TryLoadXpmPath(ctx, e, path))
            return;
    }
}

/* Po "Reload" na duzo krotszym center.conf entry_cap zostawalby na stale
 * przy poprzednim, wiekszym szczycie (EnsureCap tylko rosnie) - to
 * skurcza bufor z powrotem, gdy zapas jest juz absurdalnie duzy wzgledem
 * biezacej zawartosci (patrz identyczny wzorzec w examples/7afm.c). */
static void
ShrinkCapIfOversized(void)
{
    LauncherEntry *tmp;
    int new_cap;

    if (entry_cap <= 256 || entry_count >= entry_cap / 4)
        return;
    new_cap = entry_count > 32 ? entry_count : 32;
    tmp = realloc(entries, (size_t) new_cap * sizeof(LauncherEntry));
    if (tmp) {
        entries = tmp;
        entry_cap = new_cap;
    }
}

static void
LoadEntries(UiCtx *ctx)
{
    const char *home = getenv("HOME");
    char app_dir[1024];
    FILE *fp;
    char line[512];
    int i;

    /* Zwolnij ikony z POPRZEDNIEGO wczytania (Reload) - serwer X i tak
     * zwolnilby je przy rozlaczeniu klienta, ale wielokrotne klikniecie
     * Reload w JEDNEJ sesji bez tego kumulowaloby pixmapy po stronie
     * serwera przez caly czas dzialania apki. */
    for (i = 0; i < entry_count; i++) {
        if (entries[i].has_icon)
            XFreePixmap(g_dpy, entries[i].icon);
    }
    entry_count = 0;

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    snprintf(g_conf_path, sizeof(g_conf_path), "%s/center.conf", app_dir);

    fp = fopen(g_conf_path, "r");
    if (!fp) {
        snprintf(g_status, sizeof(g_status),
                 "No config - create %s (see center.conf.sample)", g_conf_path);
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = TrimInPlace(line);
        char *sep1, *sep2;

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        sep1 = strchr(trimmed, '|');
        if (!sep1)
            continue;

        *sep1 = '\0';
        /* trzecie, OPCJONALNE pole "ikona" (patrz naglowek pliku) - jesli
         * nie ma drugiego '|', cala reszta linii to samo pole "komenda". */
        sep2 = strchr(sep1 + 1, '|');
        if (sep2)
            *sep2 = '\0';

        if (!EnsureCap(entry_count + 1))
            break; /* OOM - konczymy z tym, co juz wczytane, zamiast crashowac */
        snprintf(entries[entry_count].name, sizeof(entries[entry_count].name),
                 "%s", TrimInPlace(trimmed));
        snprintf(entries[entry_count].command, sizeof(entries[entry_count].command),
                 "%s", TrimInPlace(sep1 + 1));
        snprintf(entries[entry_count].icon_override, sizeof(entries[entry_count].icon_override),
                 "%s", sep2 ? TrimInPlace(sep2 + 1) : "");
        if (entries[entry_count].name[0] != '\0' && entries[entry_count].command[0] != '\0') {
            LoadIconForEntry(ctx, &entries[entry_count]);
            entry_count++;
        }
    }
    fclose(fp);

    g_selected_index = -1;
    g_scroll_y = 0;

    if (entry_count == 0)
        snprintf(g_status, sizeof(g_status), "%s is empty (see center.conf.sample)", g_conf_path);
    else
        snprintf(g_status, sizeof(g_status), "%d programs", entry_count);

    ShrinkCapIfOversized();
}

static void
LaunchEntry(int index)
{
    char buf[sizeof(entries[0].command)];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    char *tok;
    pid_t pid;

    if (index < 0 || index >= entry_count)
        return;

    snprintf(buf, sizeof(buf), "%s", entries[index].command);
    tok = strtok(buf, " \t");
    while (tok && argc < MAX_CMD_TOKENS - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0)
        return;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        /* Nowa sesja, zeby zamkniecie 7acenter (SIGHUP/SIGTERM do grupy
         * procesow terminala, ktory je uruchomil) nie zabijalo tez
         * odpalonego programu - patrz setsid() w dwm/dmenu_run. */
        setsid();
        execvp(argv[0], argv);
        _exit(127);
    }
    snprintf(g_status, sizeof(g_status), "Launched: %s", entries[index].name);
}

static void
SelectEntry(int index)
{
    g_selected_index = index;
    if (index < 0 || index >= entry_count)
        snprintf(g_status, sizeof(g_status), "%d programs", entry_count);
    else
        snprintf(g_status, sizeof(g_status), "%s: %s",
                 entries[index].name, entries[index].command);
}

/* -------------------------------------------------------------------- */
/* Ikonki komorek - proceduralne, patrz naglowek pliku. Ten sam duch     */
/* "rysowane na zywo przez prymitywy ui.c" co DrawDirIcon/DrawFileIcon   */
/* w examples/7afm.c, tylko jeden ksztalt zamiast kilku wariantow.       */
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

    ui_fill_rect(ctx, body, fill);
    ui_draw_border(ctx, body, 1, outline);

    initial[0] = (char) toupper((unsigned char) name[0]);
    initial[1] = '\0';
    ui_label_centered_fg(ctx, body, initial, letter_c);
}

static void
DrawCell(UiCtx *ctx, int index, int cx, int cy, int interactive,
         const XColor *palette, const XColor *outline_c,
         const XColor *select_c, const XColor *letter_c)
{
    UiRect cell_r = { cx, cy, CELL_W, CELL_H };
    UiRect label_r;
    int icon_x, icon_y, label_y;
    const char *name = entries[index].name;
    const XColor *fill_c = &palette[HashName(name) % PALETTE_SIZE];

    if (index == g_selected_index)
        ui_fill_rect(ctx, cell_r, select_c);

    icon_x = cx + (CELL_W - ICON_SIZE) / 2;
    icon_y = cy + CELL_VPAD;
    if (entries[index].has_icon)
        ui_draw_pixmap(ctx, (UiRect){ icon_x, icon_y, ICON_SIZE, ICON_SIZE }, entries[index].icon);
    else
        DrawAppIcon(ctx, icon_x, icon_y, ICON_SIZE, name, fill_c, outline_c, letter_c);

    label_y = icon_y + ICON_SIZE + 3;
    label_r = (UiRect){ cx, label_y, CELL_W, CELL_H - (label_y - cy) };
    ui_label_centered(ctx, label_r, name);

    if (interactive && ui_hit_test(ctx, cell_r)) {
        struct timeval tv;
        long now;
        int is_double;

        gettimeofday(&tv, NULL);
        now = (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
        is_double = (index == g_last_click_index) && (now - g_last_click_ms <= DOUBLE_CLICK_MS);

        SelectEntry(index);
        g_last_click_index = index;
        g_last_click_ms = is_double ? 0 : now;
        if (is_double)
            LaunchEntry(index);
    }
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static XColor palette[PALETTE_SIZE];
    static XColor outline_color, select_color, letter_color;
    static int ready = 0;
    static const char *palette_names[PALETTE_SIZE] = {
        "steelblue", "indianred", "seagreen", "goldenrod",
        "mediumpurple", "darkorange", "teal", "slategray",
    };
    UiRect top_r, title_r, reload_r, status_row;
    UiRect viewport_r, grid_r, sb_r, up_arrow_r, down_arrow_r, track_r, thumb_r, above_r, below_r;
    int y, bottom_y, viewport_h;
    int margin = ui_window_margin(ctx);
    int grid_w, columns, rows_total, content_h, max_scroll;
    int first_row, last_row, row, col;
    int drag_was_active;
    int i;

    if (!ready) {
        for (i = 0; i < PALETTE_SIZE; i++)
            ui_color(ctx, palette_names[i], &palette[i]);
        ui_color(ctx, "white", &letter_color);
        outline_color = *ui_theme_line_fg(ctx);
        select_color = *ui_theme_accent(ctx);
        ready = 1;
    }

    /* --- gorny pasek: tytul + Reload --- */
    y = 8;
    top_r = (UiRect){ margin, y, win_w - 2 * margin, ROW_H };
    reload_r = (UiRect){ top_r.x + top_r.w - TOP_BUTTON_W, top_r.y, TOP_BUTTON_W, top_r.h };
    title_r = (UiRect){ top_r.x, top_r.y, top_r.w - TOP_BUTTON_W - 6, top_r.h };
    ui_label(ctx, title_r, "7aCenter");
    if (ui_button(ctx, reload_r, "Reload"))
        LoadEntries(ctx);
    y += ROW_H + 6;

    /* --- pasek statusu, na dole --- */
    bottom_y = win_h - MARGIN_Y - ROW_H;
    if (bottom_y < y + 20) bottom_y = y + 20;
    status_row = (UiRect){ margin, bottom_y, win_w - 2 * margin, ROW_H };

    /* --- viewport (siatka ikon + scrollbar) - geometria/scrollbar     */
    /* przejete wprost z draw() w examples/7afm.c.                      */
    viewport_h = bottom_y - 8 - y;
    if (viewport_h < 20) viewport_h = 20;
    viewport_r = (UiRect){ margin, y, win_w - 2 * margin, viewport_h };
    g_viewport_r = viewport_r;

    grid_w = viewport_r.w - SCROLLBAR_W - 4;
    if (grid_w < CELL_W) grid_w = CELL_W;
    columns = (grid_w + CELL_GAP) / (CELL_W + CELL_GAP);
    if (columns < 1) columns = 1;
    rows_total = (entry_count + columns - 1) / columns;
    if (rows_total < 1) rows_total = 1;
    content_h = rows_total * (CELL_H + CELL_GAP);

    max_scroll = content_h - viewport_r.h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
    if (g_scroll_y < 0) g_scroll_y = 0;

    grid_r = (UiRect){ viewport_r.x, viewport_r.y, grid_w, viewport_r.h };
    ui_set_clip(ctx, grid_r);
    ui_fill_rect(ctx, grid_r, ui_theme_box_bg(ctx));

    first_row = g_scroll_y / (CELL_H + CELL_GAP);
    last_row = (g_scroll_y + viewport_r.h) / (CELL_H + CELL_GAP);
    if (last_row >= rows_total) last_row = rows_total - 1;

    for (row = first_row; row <= last_row; row++) {
        for (col = 0; col < columns; col++) {
            int index = row * columns + col;
            int cx, cy;

            if (index >= entry_count)
                continue;
            cx = grid_r.x + col * (CELL_W + CELL_GAP);
            cy = grid_r.y + row * (CELL_H + CELL_GAP) - g_scroll_y;
            DrawCell(ctx, index, cx, cy, 1, palette, &outline_color, &select_color, &letter_color);
        }
    }
    ui_clear_clip(ctx);

    sb_r = (UiRect){ grid_r.x + grid_r.w + 4, viewport_r.y, SCROLLBAR_W, viewport_r.h };
    {
        int arrow_h = 14;

        up_arrow_r = (UiRect){ sb_r.x, sb_r.y, sb_r.w, arrow_h };
        down_arrow_r = (UiRect){ sb_r.x, sb_r.y + sb_r.h - arrow_h, sb_r.w, arrow_h };
        track_r = (UiRect){ sb_r.x, sb_r.y + arrow_h, sb_r.w, sb_r.h - 2 * arrow_h };
        if (track_r.h < 0) track_r.h = 0;
    }
    drag_was_active = g_thumb_dragging;
    {
        int up_clicked = ui_button(ctx, up_arrow_r, "^");
        int down_clicked = ui_button(ctx, down_arrow_r, "v");

        if (!drag_was_active) {
            if (up_clicked) g_scroll_y -= (CELL_H + CELL_GAP);
            if (down_clicked) g_scroll_y += (CELL_H + CELL_GAP);
        }
    }
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
    if (g_scroll_y < 0) g_scroll_y = 0;

    {
        int thumb_h = (content_h > 0) ? (track_r.h * viewport_r.h) / content_h : track_r.h;
        int drag_range, thumb_y;
        int mx, my, mdown;

        if (thumb_h < 10) thumb_h = 10;
        if (thumb_h > track_r.h) thumb_h = track_r.h;
        drag_range = track_r.h - thumb_h;

        ui_mouse_state(ctx, &mx, &my, &mdown);

        if (!g_thumb_dragging && mdown && drag_range > 0) {
            int thumb_y0 = track_r.y + (max_scroll > 0 ? drag_range * g_scroll_y / max_scroll : 0);

            if (mx >= track_r.x && mx < track_r.x + track_r.w &&
                my >= thumb_y0 && my < thumb_y0 + thumb_h) {
                g_thumb_dragging = 1;
                g_drag_start_my = my;
                g_drag_start_scroll = g_scroll_y;
            }
        }

        if (g_thumb_dragging) {
            if (!mdown)
                g_thumb_dragging = 0;
            else if (drag_range > 0 && max_scroll > 0)
                g_scroll_y = g_drag_start_scroll
                           + (my - g_drag_start_my) * max_scroll / drag_range;

            if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
            if (g_scroll_y < 0) g_scroll_y = 0;
        }

        thumb_y = track_r.y + (max_scroll > 0 ? drag_range * g_scroll_y / max_scroll : 0);
        thumb_r = (UiRect){ track_r.x, thumb_y, track_r.w, thumb_h };
    }
    above_r = (UiRect){ track_r.x, track_r.y, track_r.w, thumb_r.y - track_r.y };
    below_r = (UiRect){ track_r.x, thumb_r.y + thumb_r.h, track_r.w,
                         track_r.y + track_r.h - (thumb_r.y + thumb_r.h) };

    ui_fill_rect(ctx, track_r, ui_theme_bg(ctx));
    ui_draw_border(ctx, track_r, 1, ui_theme_line_fg(ctx));
    ui_fill_rect(ctx, thumb_r, ui_theme_accent(ctx));

    if (!drag_was_active) {
        if (above_r.h > 0 && ui_hit_test(ctx, above_r)) g_scroll_y -= viewport_r.h;
        if (below_r.h > 0 && ui_hit_test(ctx, below_r)) g_scroll_y += viewport_r.h;
        if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
        if (g_scroll_y < 0) g_scroll_y = 0;
    }

    ui_draw_border(ctx, viewport_r, 1, ui_theme_line_fg(ctx));
    ui_label(ctx, status_row, g_status);

    return 0;
}

/* -------------------------------------------------------------------- */
/* Ikona okna - siatka 2x2 kwadracikow ("launcher"), rysowana wprost     */
/* Xlibem na 1-bitowej Pixmapie, ten sam wzorzec co MakeFolderIconPixmap */
/* w examples/7afm.c.                                                    */
/* -------------------------------------------------------------------- */

static void
DrawLauncherIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    int cell = 10, gap = 3, off = 4;
    int i, j;

    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++)
            XFillRectangle(idpy, p, gc, off + j * (cell + gap), off + i * (cell + gap), cell, cell);
    }
}

static Pixmap
MakeLauncherIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawLauncherIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 420, win_h = 360;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    XEvent ev;

    signal(SIGCHLD, SIG_IGN);

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        }
    }

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w examples/7afm.c (patrz komentarz
     * tam): to launcher DOWOLNYCH programow z ~/.7a/center.conf
     * (fork+execvp argv[0] z configu uzytkownika w LaunchSelected), wiec
     * unveil dziedziczony po exec ograniczalby wlasnie te programy, ktore
     * apka ma uruchamiac. Zapis nie jest potrzebny - center.conf i ikony
     * .xpm sa tylko czytane. */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
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
    ReadAppString(dpy, "7aCenter.iconPath", "7aCenter.IconPath",
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
                           PointerMotionMask | StructureNotifyMask);
    XStoreName(dpy, win, "7aCenter");
    XSetIconName(dpy, win, "7aCenter");

    icon = MakeLauncherIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

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
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    /* Narysuj OD RAZU jedna (pusta) klatke, ZANIM LoadEntries() przeczyta
     * config - ten sam wzorzec co w examples/7afm.c/7aweather.c. */
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
             * examples/7afm.c. */
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

        if (redraw) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h)) running = 0;
            ui_end_frame(ctx);
            redraw = 0;
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
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
