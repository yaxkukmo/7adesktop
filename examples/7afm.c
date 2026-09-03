/*
 * 7afm.c - port oryginalnej apki z ../7afm (Xt/Xaw, wlasny widget
 * IconGrid osadzony w Xaw Viewport) na biblioteke ui.c/ui.h z tego
 * katalogu - ten sam wzorzec portowania co pozostale examples/7a*.c.
 *
 * Najwieksze roznice wzgledem oryginalu:
 *
 *  - Przewijanie: oryginal dostawal je "za darmo" od Xaw Viewport
 *    (prawdziwy suwak, przewijanie widgetu-dziecka). ui.c nie ma
 *    zadnego kontenera przewijalnego - siatka ikon, scrollbar (strzalki,
 *    przeciaganie kciuka i "kliknij nad/pod kciukiem = strona w gore/
 *    dol") i przewijanie kolkiem myszy (Button4/5) sa wiec zaimplementowane
 *    wprost w tym pliku. Przeciaganie kciuka idzie przez ui_mouse_state
 *    (surowa pozycja kursora + stan LPM, patrz ui.h) - ui.c nadal nie
 *    trzyma stanu "trwajacego draga" w publicznym API, sama sesja
 *    (g_thumb_dragging i in.) jest wlasnoscia TEGO pliku. Kolko myszy
 *    jest przechwytywane W PETLI ZDARZEN, PRZED ui_feed_event - inaczej
 *    para ButtonPress/Release od kolka zostalaby policzona jako zwykly
 *    klik na cokolwiek jest akurat pod kursorem (ui.c nie rozroznia
 *    numeru przycisku).
 *  - Przycinanie: dodane do biblioteki (ui_set_clip/ui_clear_clip, patrz
 *    ui.h) specjalnie na potrzeby tego portu - siatka ikon ma byc
 *    przycieta do swojego viewportu, zeby przewinieta tresc nie
 *    rysowala sie na wierzchu paska sciezki/statusu/przyciskow.
 *  - Ikona "odtwarzalny" (trojkat play) i "wyciete" narozne zagiecie
 *    kartki pliku potrzebuja WYPELNIONEGO trojkata - drugi nowy
 *    prymityw, ui_fill_triangle (patrz ui.h).
 *  - Schowek Copy/Cut/Paste: oryginal uzywa XtOwnSelection/
 *    XtGetSelectionValue (wygodna warstwa Xt nad protokolem X11
 *    selection) - tutaj to protokol ICCCM wprost, Xlib (XSetSelectionOwner/
 *    XConvertSelection + obsluga SelectionRequest/SelectionNotify/
 *    SelectionClear w petli zdarzen, patrz main()) - dziala tak samo
 *    MIEDZY OSOBNYMI PROCESAMI 7afm (kazde "New" to nowy proces), bo to
 *    ten sam mechanizm X11, tylko bez posrednictwa Xt.
 *  - Drag-and-drop miedzy oknami 7afm: NOWY dodatek, ktorego oryginal
 *    Xt/Xaw nie mial. Protokol XDND (freedesktop.org) wprost przez Xlib
 *    ClientMessage, zaimplementowany swiadomie tylko dla interakcji
 *    MIEDZY OKNAMI 7afm (nie z obcymi apkami) - patrz obszerny komentarz
 *    przy sekcji "XDND" w dalszej czesci pliku po pelny opis uproszczen
 *    i uzasadnienie. Transport danych/wykonanie mv/cp reuzywa mechanizmu
 *    schowka opisanego wyzej.
 */

#define _DEFAULT_SOURCE  /* execvp/fork/strcasecmp - patrz ta sama uwaga w examples/7aweather.c */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <magic.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"

#define ICON_SIZE 32
#define CELL_W 80
#define CELL_H 64
#define CELL_GAP 6
#define CELL_VPAD 4
#define SCROLLBAR_W 14
#define ROW_H 20
#define MARGIN_Y 8              /* odstep od GORNEJ/DOLNEJ krawedzi - nie sterowany "windowMargin"
                                  * (patrz ui_window_margin w ui.h), tylko lewa/prawa krawedz jest */
#define DOUBLE_CLICK_MS 400     /* brak Xt -> brak XtGetMultiClickTime, stala rozsadna wartosc */
#define WHEEL_STEP (CELL_H + CELL_GAP)
#define MAX_OPENER_TOKENS 32
#define MENU_ITEM_W 44          /* szerokosc gornego przycisku menu (File/Edit/View) */
#define MENU_DROPDOWN_W 160     /* stala szerokosc rozwinietego menu - nie mamy pomiaru
                                    szerokosci tekstu w ui.c, patrz komentarz przy DrawCell
                                    o tym samym ograniczeniu dla etykiet ikon */
#define MAGIC_HDR_SIZE 4096     /* bufor naglowka dla magic_buffer w ClassifyFile - patrz
                                    komentarz przy tej funkcji */
#define XDND_PROTOCOL_VERSION 5
#define XDND_DRAG_THRESHOLD_PX 6 /* brak Xt -> brak wbudowanego progu przeciagania, stala
                                    rozsadna wartosc, ten sam duch co DOUBLE_CLICK_MS */

/* Kategorie ikon - te same wartosci co XawIconGridFile/Directory/
 * Executable/Image/Text w oryginalnym IconGrid.h (nie ma juz co z niego
 * dziedziczyc, ale ClassifyFile/EntryCompare licza na te konkretne
 * liczby - zostaja niezmienione). */
#define KIND_FILE       0
#define KIND_DIRECTORY  1
#define KIND_EXECUTABLE 2
#define KIND_IMAGE      3
#define KIND_TEXT       4

static Display *g_dpy;
static Window g_win;

static char cur_path[PATH_MAX];
static char program_path[PATH_MAX];  /* argv[0] - do "New" (fork+execvp kolejnej kopii) */
static magic_t magic_cookie;

typedef struct {
    char name[256];
    int kind;
} Entry;

static Entry *entries = NULL;
static int entry_count = 0;
static int entry_cap = 0;

static int g_selected_index = -1;
static char g_status[300] = "";
static char g_path_label[PATH_MAX + 8] = "";

static int g_scroll_y = 0;
static UiRect g_viewport_r = { 0, 0, 0, 0 };  /* zapamietane z ostatniej klatki - do kolka myszy w main() */

/* sesja przeciagania kciuka scrollbara - wlasnosc APKI (patrz komentarz
 * przy ui_mouse_state w ui.h), zyje MIEDZY ramkami tak jak g_scroll_y.
 * g_drag_start_my/g_drag_start_scroll to punkt odniesienia z ramki, w
 * ktorej zaczelo sie przeciaganie (pozycja Y myszy i g_scroll_y w tamtej
 * chwili) - kazda kolejna ramka liczy nowy g_scroll_y z ROZNICY wzgledem
 * tego punktu, a nie z biezacej pozycji kciuka. */
static int g_thumb_dragging = 0;
static int g_drag_start_my = 0;
static int g_drag_start_scroll = 0;

static int g_last_click_index = -1;
static long g_last_click_ms = 0;

/* Schowek - patrz komentarz na gorze pliku. clipboard_path/is_cut sa
 * WAZNE tylko gdy TEN proces jest wlascicielem zaznaczenia (odpowiada
 * na SelectionRequest z tych danych) - inny proces bedacy wlascicielem
 * ma wlasna kopie w SWOICH zmiennych. */
static Atom g_clipboard_atom;
static Atom g_paste_prop_atom;
static char g_clipboard_path[PATH_MAX] = "";
static int g_clipboard_is_cut = 0;

/* XDND (przeciaganie plikow miedzy oknami 7afm) - protokol ClientMessage
 * + selections NAD tym samym mechanizmem co Schowek wyzej (transport i
 * wykonanie mv/cp sa wspoldzielone, patrz HandleSelectionRequest/
 * HandlePasteReceived nizej oraz obszerny komentarz przy sekcji "XDND"
 * w dalszej czesci pliku). Atomy internowane w main(). Caly stan sesji
 * (zrodla i celu) jest WLASNOSCIA TEGO PLIKU, tak jak g_thumb_dragging -
 * ui.c nic o tym nie wie. */
static Atom g_xdnd_aware_atom;
static Atom g_xdnd_enter_atom;
static Atom g_xdnd_position_atom;
static Atom g_xdnd_status_atom;
static Atom g_xdnd_leave_atom;
static Atom g_xdnd_drop_atom;
static Atom g_xdnd_finished_atom;
static Atom g_xdnd_selection_atom;
static Atom g_xdnd_action_copy_atom;
static Atom g_xdnd_action_move_atom;

/* sesja zrodla - drag WYCHODZACY z tego okna */
static int g_xdnd_press_index = -1;  /* kandydat na drag od ButtonPress, przed progiem ruchu */
static int g_xdnd_press_x = 0;
static int g_xdnd_press_y = 0;
static int g_xdnd_dragging = 0;      /* prog ruchu przekroczony, sesja XDND trwa */
static int g_xdnd_drag_index = -1;   /* tylko do tekstu statusu (entries[] stabilne w trakcie draga) */
static char g_xdnd_drag_path[PATH_MAX] = "";  /* CELOWO osobna od g_clipboard_path - patrz
                                                * komentarz przy HandleSelectionRequest, inaczej
                                                * drag nadpisywalby "prawdziwy" schowek Copy/Cut */
static int g_xdnd_drag_is_move = 1;
static Window g_xdnd_target_win = None;
static int g_xdnd_target_ready = 0;   /* czy XdndEnter juz wyslane do target_win */
static int g_xdnd_target_accepts = 0; /* wynik ostatniego XdndStatus */
static Atom g_xdnd_action = None;     /* aktualnie proponowana akcja (Move/Copy) */

/* sesja celu - kazde okno moze byc targetem przychodzacego draga,
 * ten stan jest wiec zawsze aktywny (niezaleznie od g_xdnd_dragging) */
static Window g_xdnd_pending_source = None;
static int g_xdnd_pending_version = 0;
static int g_xdnd_hover_active = 0;   /* feedback w draw() - obramowanie viewportu */
static int g_xdnd_drop_pending = 0;   /* nastepny SelectionNotify to wynik XdndDrop, nie zwyklego Paste */
static Window g_xdnd_drop_source = None;
static Time g_xdnd_drop_time = CurrentTime;
static int g_xdnd_last_paste_was_move = 0;  /* ustawiane przez HandlePasteReceived, do XdndFinished */

/* Pasek menu (File/Edit/View) - -1 zamkniete, 0/1/2 = ktore rozwiniete.
 * Rysowany/hit-testowany wprost, bez prawdziwego okna popup/grab - ten
 * sam, sprawdzony wzorzec co dropdown priorytetu w examples/7atodo.c
 * (patrz obszerny komentarz tam o tym, dlaczego prawdziwy popup w Xt
 * potrafil powiesic caly serwer X). */
static int g_menu_open = -1;

/* Modal Rename/Delete - wygrywa nad menu (otwarcie modala z menu Edit
 * zamyka menu w TEJ SAMEJ klatce, patrz draw()). Zaden "klik gdziekolwiek
 * zamyka" dla modali - to akcje z konsekwencjami (zwlaszcza Delete),
 * wymagaja jawnego OK/Cancel. */
typedef enum { MODAL_NONE, MODAL_RENAME, MODAL_DELETE } ModalKind;
static ModalKind g_modal = MODAL_NONE;
static int g_modal_index = -1;       /* ktora pozycja entries[] */
static char g_rename_buf[256] = "";
static int g_rename_cursor = 0;

typedef struct {
    char opener[256];        /* domyslna komenda otwierania, np. "xterm -e nvim" */
    char mime_openers[1024]; /* "wzorzec=komenda" po przecinku, patrz FindMimeOpener */
    int show_hidden;
} AppData;

static AppData app_data;

/* -------------------------------------------------------------------- */
/* Zasoby X - jak w examples/7atodo.c/7atimer.c (konfiguracja            */
/* specyficzna dla tej apki, nie ogolny motyw ui.c).                     */
/* -------------------------------------------------------------------- */

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

static int
ReadAppBool(Display *dpy, const char *name, const char *class_, int dflt)
{
    char buf[32];

    ReadAppString(dpy, name, class_, buf, sizeof(buf), dflt ? "true" : "false");
    return (strcasecmp(buf, "true") == 0 || strcasecmp(buf, "yes") == 0 ||
            strcasecmp(buf, "on") == 0 || strcmp(buf, "1") == 0);
}

/* -------------------------------------------------------------------- */
/* Sciezki - bez zmian wzgledem ../7afm/7afm.c (strlcpy->snprintf, patrz */
/* ta sama uwaga w examples/7atodo.c).                                   */
/* -------------------------------------------------------------------- */

static void
JoinPath(const char *base, const char *name, char *out, size_t outsize)
{
    if (strcmp(base, "/") == 0)
        snprintf(out, outsize, "/%s", name);
    else
        snprintf(out, outsize, "%s/%s", base, name);
}

static void
GetParentPath(const char *path, char *out, size_t outsize)
{
    char tmp[PATH_MAX];
    char *slash;

    if (strcmp(path, "/") == 0) {
        snprintf(out, outsize, "/");
        return;
    }

    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
    if (slash == tmp) {
        snprintf(out, outsize, "/");
    } else if (slash) {
        *slash = '\0';
        snprintf(out, outsize, "%s", tmp);
    } else {
        snprintf(out, outsize, "/");
    }
}

/* -------------------------------------------------------------------- */
/* Czytanie katalogu - bez zmian wzgledem oryginalu.                     */
/* -------------------------------------------------------------------- */

static int
EntryCompare(const void *a, const void *b)
{
    const Entry *ea = (const Entry *) a;
    const Entry *eb = (const Entry *) b;
    int ea_dir = (ea->kind == KIND_DIRECTORY);
    int eb_dir = (eb->kind == KIND_DIRECTORY);

    if (ea_dir != eb_dir)
        return eb_dir - ea_dir;
    return strcasecmp(ea->name, eb->name);
}

/* Rozszerzenia rozpoznawane bez sięgania po magic w ogóle - w typowym
 * katalogu to większość plików, więc omija się tu open+read+dopasowanie
 * sygnatur libmagic dla nich całkowicie. Nierozpoznane rozszerzenie (albo
 * jego brak) spada do fallbacku przez magic_buffer niżej. */
static const struct {
    const char *ext;
    int kind;
} kExtKind[] = {
    { ".png",  KIND_IMAGE }, { ".jpg",  KIND_IMAGE }, { ".jpeg", KIND_IMAGE },
    { ".gif",  KIND_IMAGE }, { ".bmp",  KIND_IMAGE }, { ".webp", KIND_IMAGE },
    { ".svg",  KIND_IMAGE }, { ".tif",  KIND_IMAGE }, { ".tiff", KIND_IMAGE },
    { ".ico",  KIND_IMAGE }, { ".xpm",  KIND_IMAGE },
    { ".txt",  KIND_TEXT },  { ".md",   KIND_TEXT },  { ".c",    KIND_TEXT },
    { ".h",    KIND_TEXT },  { ".cpp",  KIND_TEXT },  { ".hpp",  KIND_TEXT },
    { ".py",   KIND_TEXT },  { ".sh",   KIND_TEXT },  { ".json", KIND_TEXT },
    { ".xml",  KIND_TEXT },  { ".html", KIND_TEXT },  { ".htm",  KIND_TEXT },
    { ".css",  KIND_TEXT },  { ".conf", KIND_TEXT },  { ".cfg",  KIND_TEXT },
    { ".ini",  KIND_TEXT },  { ".log",  KIND_TEXT },  { ".csv",  KIND_TEXT },
    { ".yaml", KIND_TEXT },  { ".yml",  KIND_TEXT },
};

static int
ExtensionKind(const char *path)
{
    const char *dot = strrchr(path, '.');
    size_t i;

    if (!dot)
        return -1;
    for (i = 0; i < sizeof(kExtKind) / sizeof(kExtKind[0]); i++) {
        if (strcasecmp(dot, kExtKind[i].ext) == 0)
            return kExtKind[i].kind;
    }
    return -1;
}

static int
ClassifyFile(const char *path, const struct stat *st)
{
    unsigned char hdr[MAGIC_HDR_SIZE];
    const char *mime;
    int ext_kind, fd;
    ssize_t n;

    if (S_ISDIR(st->st_mode))
        return KIND_DIRECTORY;
    if (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return KIND_EXECUTABLE;

    ext_kind = ExtensionKind(path);
    if (ext_kind >= 0)
        return ext_kind;

    if (!magic_cookie)
        return KIND_FILE;

    /* magic_buffer nad recznie wczytanym, OGRANICZONYM naglowkiem zamiast
     * magic_file - magic_file samo otwiera i czyta caly plik od nowa
     * (drugi dotyk pliku po stat() wyzej, bez limitu na to ile czyta).
     * Jeden bounded read tutaj kosztuje tyle samo dla malych plikow, a
     * chroni przed czytaniem calosci duzych plikow o nierozpoznanym
     * rozszerzeniu. */
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return KIND_FILE;
    n = read(fd, hdr, sizeof(hdr));
    close(fd);
    if (n <= 0)
        return KIND_FILE;

    mime = magic_buffer(magic_cookie, hdr, (size_t) n);
    if (mime) {
        if (strncmp(mime, "image/", 6) == 0)
            return KIND_IMAGE;
        if (strncmp(mime, "text/", 5) == 0)
            return KIND_TEXT;
    }
    return KIND_FILE;
}

static void
EnsureCap(int needed)
{
    if (needed <= entry_cap)
        return;
    entry_cap = entry_cap ? entry_cap * 2 : 64;
    if (entry_cap < needed)
        entry_cap = needed;
    entries = realloc(entries, (size_t) entry_cap * sizeof(Entry));
}

static void
ReadDirectory(const char *path)
{
    DIR *dp;
    struct dirent *de;
    struct stat st;
    char full[PATH_MAX];
    int has_parent = (strcmp(path, "/") != 0);

    entry_count = 0;

    dp = opendir(path);
    if (!dp)
        return;

    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!app_data.show_hidden && de->d_name[0] == '.')
            continue;

        EnsureCap(entry_count + 1);
        JoinPath(path, de->d_name, full, sizeof(full));
        snprintf(entries[entry_count].name, sizeof(entries[entry_count].name), "%s", de->d_name);
        entries[entry_count].kind = (stat(full, &st) == 0)
                                         ? ClassifyFile(full, &st) : KIND_FILE;
        entry_count++;
    }
    closedir(dp);

    qsort(entries, (size_t) entry_count, sizeof(Entry), EntryCompare);

    if (has_parent) {
        EnsureCap(entry_count + 1);
        memmove(&entries[1], &entries[0], (size_t) entry_count * sizeof(Entry));
        snprintf(entries[0].name, sizeof(entries[0].name), "..");
        entries[0].kind = KIND_DIRECTORY;
        entry_count++;
    }
}

/* -------------------------------------------------------------------- */
/* Otwieranie plikow - bez zmian wzgledem oryginalu.                     */
/* -------------------------------------------------------------------- */

static const char *
DetectMimeType(const char *path)
{
    return magic_cookie ? magic_file(magic_cookie, path) : NULL;
}

static int
FindMimeOpener(const char *mime, char *cmd_out, size_t outsize)
{
    char buf[1024];
    char *tok, *saveptr;

    if (!app_data.mime_openers[0])
        return 0;

    snprintf(buf, sizeof(buf), "%s", app_data.mime_openers);

    for (tok = strtok_r(buf, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
        char *pattern, *cmd, *eq;
        size_t plen;

        while (*tok == ' ')
            tok++;
        eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = '\0';
        pattern = tok;
        cmd = eq + 1;
        plen = strlen(pattern);

        if (plen > 0 && pattern[plen - 1] == '/') {
            if (strncmp(mime, pattern, plen) == 0) {
                snprintf(cmd_out, outsize, "%s", cmd);
                return 1;
            }
        } else if (strcmp(mime, pattern) == 0) {
            snprintf(cmd_out, outsize, "%s", cmd);
            return 1;
        }
    }
    return 0;
}

static void
OpenFile(const char *path)
{
    const char *mime = DetectMimeType(path);
    char cmd[256];
    char buf[512];
    char *argv[MAX_OPENER_TOKENS];
    int argc = 0;
    char *tok;
    pid_t pid;

    if (!(mime && FindMimeOpener(mime, cmd, sizeof(cmd)))) {
        if (app_data.opener[0] == '\0') {
            if (mime)
                snprintf(g_status, sizeof(g_status), "No opener for %s: %s", mime, path);
            else
                snprintf(g_status, sizeof(g_status), "No opener for: %s", path);
            return;
        }
        snprintf(cmd, sizeof(cmd), "%s", app_data.opener);
    }

    snprintf(buf, sizeof(buf), "%s", cmd);
    tok = strtok(buf, " \t");
    while (tok && argc < MAX_OPENER_TOKENS - 2) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc++] = (char *) path;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
}

/* -------------------------------------------------------------------- */
/* Nawigacja                                                             */
/* -------------------------------------------------------------------- */

static const char *
KindLabel(int kind)
{
    switch (kind) {
    case KIND_DIRECTORY:  return "Directory";
    case KIND_EXECUTABLE: return "Executable";
    case KIND_IMAGE:      return "Image";
    case KIND_TEXT:       return "Text file";
    default:              return "File";
    }
}

static void
NavigateTo(const char *path)
{
    char resolved[PATH_MAX];

    if (!realpath(path, resolved))
        return;

    snprintf(cur_path, sizeof(cur_path), "%s", resolved);
    ReadDirectory(cur_path);

    g_selected_index = -1;
    g_scroll_y = 0;

    snprintf(g_path_label, sizeof(g_path_label), "Path: %s", cur_path);
    snprintf(g_status, sizeof(g_status), "%d items", entry_count);
}

static void
ActivateEntry(int index)
{
    char path[PATH_MAX];

    if (index < 0 || index >= entry_count)
        return;

    if (entries[index].kind == KIND_DIRECTORY) {
        if (strcmp(entries[index].name, "..") == 0)
            GetParentPath(cur_path, path, sizeof(path));
        else
            JoinPath(cur_path, entries[index].name, path, sizeof(path));
        NavigateTo(path);
    } else {
        JoinPath(cur_path, entries[index].name, path, sizeof(path));
        OpenFile(path);
    }
}

static void
SelectEntry(int index)
{
    g_selected_index = index;
    if (index < 0 || index >= entry_count)
        snprintf(g_status, sizeof(g_status), "%d items", entry_count);
    else
        snprintf(g_status, sizeof(g_status), "%s: %s",
                 KindLabel(entries[index].kind), entries[index].name);
}

static void
SpawnNewWindow(void)
{
    char *child_argv[3];
    pid_t pid;

    child_argv[0] = program_path;
    child_argv[1] = cur_path;
    child_argv[2] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(program_path, child_argv);
        _exit(127);
    }
}

static void
ActionToggleHidden(void)
{
    app_data.show_hidden = !app_data.show_hidden;
    ReadDirectory(cur_path);
    g_selected_index = -1;
    snprintf(g_status, sizeof(g_status), "%d items", entry_count);
}

/* -------------------------------------------------------------------- */
/* Rename/Delete - modal Yes/No lub pole tekstowe, patrz g_modal na      */
/* gorze pliku. Oryginal nie mial tych akcji wcale (tylko Copy/Cut/      */
/* Paste) - dodane teraz razem z paskiem menu.                           */
/* -------------------------------------------------------------------- */

static void
BeginRename(void)
{
    if (g_selected_index < 0 || g_selected_index >= entry_count) {
        snprintf(g_status, sizeof(g_status), "Nothing selected to rename");
        return;
    }
    if (strcmp(entries[g_selected_index].name, "..") == 0) {
        snprintf(g_status, sizeof(g_status), "Cannot rename \"..\"");
        return;
    }
    g_modal_index = g_selected_index;
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", entries[g_selected_index].name);
    g_rename_cursor = (int) strlen(g_rename_buf);
    g_modal = MODAL_RENAME;
}

static void
BeginDelete(void)
{
    if (g_selected_index < 0 || g_selected_index >= entry_count) {
        snprintf(g_status, sizeof(g_status), "Nothing selected to delete");
        return;
    }
    if (strcmp(entries[g_selected_index].name, "..") == 0) {
        snprintf(g_status, sizeof(g_status), "Cannot delete \"..\"");
        return;
    }
    g_modal_index = g_selected_index;
    g_modal = MODAL_DELETE;
}

static void
ConfirmRename(void)
{
    char old_path[PATH_MAX], new_path[PATH_MAX];

    if (g_modal_index < 0 || g_modal_index >= entry_count) {
        g_modal = MODAL_NONE;
        return;
    }
    if (g_rename_buf[0] == '\0') {
        snprintf(g_status, sizeof(g_status), "Name cannot be empty");
        return;
    }

    JoinPath(cur_path, entries[g_modal_index].name, old_path, sizeof(old_path));
    JoinPath(cur_path, g_rename_buf, new_path, sizeof(new_path));

    if (rename(old_path, new_path) == 0)
        NavigateTo(cur_path);
    else
        snprintf(g_status, sizeof(g_status), "Rename failed: %s", strerror(errno));
    g_modal = MODAL_NONE;
}

/* Blokujace "rm -rf" (fork+exec+waitpid, ten sam wzorzec co cp/mv w
 * HandlePasteReceived) - dziala jednakowo na pliki i katalogi, bez
 * potrzeby wlasnej rekurencji po drzewie katalogow. */
static void
ConfirmDelete(void)
{
    char path[PATH_MAX];
    pid_t pid;
    int wait_status;

    if (g_modal_index < 0 || g_modal_index >= entry_count) {
        g_modal = MODAL_NONE;
        return;
    }
    JoinPath(cur_path, entries[g_modal_index].name, path, sizeof(path));

    signal(SIGCHLD, SIG_DFL);
    pid = fork();
    if (pid == 0) {
        execlp("rm", "rm", "-rf", path, (char *) NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &wait_status, 0);
    signal(SIGCHLD, SIG_IGN);

    if (pid > 0 && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
        NavigateTo(cur_path);
    else
        snprintf(g_status, sizeof(g_status), "Delete failed");
    g_modal = MODAL_NONE;
}

/* -------------------------------------------------------------------- */
/* Schowek Copy/Cut/Paste - patrz komentarz na gorze pliku.              */
/* -------------------------------------------------------------------- */

static void
MarkClipboard(int is_cut)
{
    if (g_selected_index < 0 || g_selected_index >= entry_count) {
        snprintf(g_status, sizeof(g_status), "Nothing selected to %s", is_cut ? "cut" : "copy");
        return;
    }

    JoinPath(cur_path, entries[g_selected_index].name, g_clipboard_path, sizeof(g_clipboard_path));
    g_clipboard_is_cut = is_cut;
    XSetSelectionOwner(g_dpy, g_clipboard_atom, g_win, CurrentTime);

    snprintf(g_status, sizeof(g_status), "%s: %s", is_cut ? "Cut" : "Copied",
             entries[g_selected_index].name);
}

static void
RequestPaste(void)
{
    XConvertSelection(g_dpy, g_clipboard_atom, XA_STRING, g_paste_prop_atom, g_win, CurrentTime);
}

/* Odpowiada na SelectionRequest, gdy TEN proces jest wlascicielem
 * zaznaczenia (po Copy/Cut) - odpowiednik ConvertClipboard w oryginale,
 * tylko protokolem ICCCM wprost zamiast przez XtOwnSelection. */
static void
HandleSelectionRequest(const XSelectionRequestEvent *req)
{
    XSelectionEvent notify;
    char buf[PATH_MAX + 8];
    Atom prop = (req->property != None) ? req->property : req->target;
    /* XDND uzywa WLASNEJ pary sciezka/is_cut (g_xdnd_drag_*), NIE
     * g_clipboard_path/g_clipboard_is_cut - inaczej przeciagniecie pliku
     * w trakcie trwajacego Cut (przed Paste) nadpisaloby "prawdziwy"
     * schowek z menu Edit, patrz komentarz przy globalach g_xdnd_*. */
    const char *path = (req->selection == g_xdnd_selection_atom)
                        ? g_xdnd_drag_path : g_clipboard_path;
    int is_cut = (req->selection == g_xdnd_selection_atom)
                 ? g_xdnd_drag_is_move : g_clipboard_is_cut;

    notify.type = SelectionNotify;
    notify.requestor = req->requestor;
    notify.selection = req->selection;
    notify.target = req->target;
    notify.time = req->time;
    notify.property = None;

    if (req->target == XA_STRING) {
        snprintf(buf, sizeof(buf), "%s:%s", is_cut ? "MOVE" : "COPY", path);
        XChangeProperty(g_dpy, req->requestor, prop, XA_STRING, 8,
                         PropModeReplace, (unsigned char *) buf, (int) strlen(buf));
        notify.property = prop;
    }
    XSendEvent(g_dpy, req->requestor, False, 0, (XEvent *) &notify);
}

/* Wywolywane po SelectionNotify z niepustym property - czyta dane
 * wlasnie skonwertowane przez wlasciciela (patrz RequestPaste) i
 * wykonuje cp/mv - odpowiednik PasteDataCallback w oryginale. Zwraca
 * sukces/porazke (1/0) - potrzebne przez XDND (case SelectionNotify w
 * main()), zeby wyslac XdndFinished z poprawnym statusem do zrodla
 * draga; zwykle wywolanie z menu Paste wynik moze zignorowac. */
static int
HandlePasteReceived(void)
{
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    char raw[PATH_MAX + 8];
    char src[PATH_MAX], dest[PATH_MAX];
    char *path_part, *slash;
    int is_move;
    pid_t pid;
    int wait_status;

    if (XGetWindowProperty(g_dpy, g_win, g_paste_prop_atom, 0, (long) (sizeof(raw) / 4 + 1),
                            True, AnyPropertyType, &actual_type, &actual_format,
                            &nitems, &bytes_after, &data) != Success ||
        !data || actual_type == None || nitems == 0) {
        snprintf(g_status, sizeof(g_status), "Clipboard is empty (Copy/Cut first)");
        if (data) XFree(data);
        return 0;
    }

    {
        unsigned long len = (nitems < sizeof(raw) - 1) ? nitems : sizeof(raw) - 1;

        memcpy(raw, data, len);
        raw[len] = '\0';
    }
    XFree(data);

    if (strncmp(raw, "MOVE:", 5) == 0) {
        is_move = 1;
        path_part = raw + 5;
    } else if (strncmp(raw, "COPY:", 5) == 0) {
        is_move = 0;
        path_part = raw + 5;
    } else {
        is_move = 0;
        path_part = raw;
    }
    g_xdnd_last_paste_was_move = is_move;
    snprintf(src, sizeof(src), "%s", path_part);

    slash = strrchr(src, '/');
    JoinPath(cur_path, slash ? slash + 1 : src, dest, sizeof(dest));

    /* Blokujace cp/mv (fork+exec+waitpid) - SIGCHLD jest globalnie
     * SIG_IGN (patrz main()), na czas TEGO czekania trzeba je przywrocic
     * do SIG_DFL, inaczej jadro mogloby samo sprzatnac potomka przed
     * waitpid (ECHILD) - ten sam wzorzec co w oryginale. */
    signal(SIGCHLD, SIG_DFL);
    pid = fork();
    if (pid == 0) {
        if (is_move)
            execlp("mv", "mv", src, dest, (char *) NULL);
        else
            execlp("cp", "cp", "-r", src, dest, (char *) NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &wait_status, 0);
    signal(SIGCHLD, SIG_IGN);

    if (pid > 0 && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
        NavigateTo(cur_path);
        return 1;
    }
    snprintf(g_status, sizeof(g_status), "%s", is_move ? "Move failed" : "Copy failed");
    return 0;
}

/* -------------------------------------------------------------------- */
/* XDND - przeciaganie plikow miedzy oknami 7afm (kazde okno to osobny   */
/* proces, patrz naglowek pliku).                                        */
/*                                                                        */
/* Transport danych i faktyczne mv/cp sa WSPOLDZIELONE ze schowkiem      */
/* Copy/Cut/Paste wyzej (HandleSelectionRequest/HandlePasteReceived) -   */
/* XDND dostarcza tylko protokol ClientMessage (Enter/Position/Status/   */
/* Leave/Drop/Finished), ktory prowadzi do TEGO SAMEGO XConvertSelection */
/* + SelectionNotify co dzisiejszy Paste, tylko wyzwolonego gestem myszy */
/* zamiast klikniecia w menu Edit. Swiadome uproszczenia wzgledem pelnej */
/* specyfikacji freedesktop.org XDND (bo target to ZAWSZE inne okno      */
/* 7afm, nigdy obca apka):                                               */
/*  - jeden typ danych w XdndEnter, XA_STRING, format "MOVE:path"/       */
/*    "COPY:path" (identyczny jak schowek) - NIE standardowe             */
/*    text/uri-list, wiec drop na Nautilusa/terminal itp. to zwykle      */
/*    no-op (obca apka nie rozpozna typu) lub, jesli akceptuje zwykly    */
/*    tekst, wklejenie doslownego stringu "MOVE:/path" - swiadomy kwirk. */
/*  - brak throttlingu XdndPosition (jeden komunikat na kazdy             */
/*    MotionNotify) i brak prostokata "no more events" w XdndStatus -     */
/*    ok, bo target to zawsze lokalny proces.                            */
/*  - tylko protokol w wersji 5, bez negocjacji wstecznej kompatybilnosci.*/
/*  - drag zawsze dotyczy JEDNEGO pliku (g_selected_index jest skalarem, */
/*    apka nie ma multi-select) i zawsze wkleja do BIEZACEGO katalogu    */
/*    okna-celu (jak dzisiejszy Paste) - drop na konkretna ikone folderu */
/*    w oknie-celu to przyszle rozszerzenie, jak i drag WEWNATRZ jednego */
/*    okna (self jest jawnie wykluczony w XdndFindTargetWindow).         */
/*                                                                        */
/* Bez grabu wskaznika (XGrabPointer) w XdndBeginDrag drag zamarlby w    */
/* chwili opuszczenia okna zrodlowego - serwer X przestaje wtedy slac    */
/* MotionNotify do procesu, nad ktorym nie ma juz kursora.               */
/* -------------------------------------------------------------------- */

static int g_xdnd_probe_had_error = 0;

static int
XdndProbeErrorHandler(Display *dpy, XErrorEvent *ev)
{
    (void) dpy;
    (void) ev;
    g_xdnd_probe_had_error = 1;
    return 0;
}

/* Buduje i wysyla jeden z 6 ksztaltow ClientMessage protokolu XDND -
 * jeden wspolny sender zamiast powtarzania XClientMessageEvent+XSendEvent
 * przy kazdym z osobna. */
static void
XdndSendClientMessage(Display *dpy, Window dest, Atom message_type,
                       long l0, long l1, long l2, long l3, long l4)
{
    XClientMessageEvent m;

    memset(&m, 0, sizeof(m));
    m.type = ClientMessage;
    m.window = dest;
    m.message_type = message_type;
    m.format = 32;
    m.data.l[0] = l0;
    m.data.l[1] = l1;
    m.data.l[2] = l2;
    m.data.l[3] = l3;
    m.data.l[4] = l4;
    XSendEvent(dpy, dest, False, NoEventMask, (XEvent *) &m);
}

/* XGetWindowProperty na XdndAware, opakowane tymczasowym
 * XSetErrorHandler - okno-kandydat moze zniknac w trakcie draga (proces
 * zabity), a wtedy zwykle BadWindow nie moze wywalic calej apki. */
static int
XdndWindowIsAware(Display *dpy, Window w, Atom aware_atom)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    int (*old_handler)(Display *, XErrorEvent *);
    int aware;

    g_xdnd_probe_had_error = 0;
    old_handler = XSetErrorHandler(XdndProbeErrorHandler);
    XGetWindowProperty(dpy, w, aware_atom, 0, 1, False, XA_ATOM,
                        &actual_type, &actual_format, &nitems, &bytes_after, &data);
    XSetErrorHandler(old_handler);

    aware = !g_xdnd_probe_had_error && actual_type == XA_ATOM && nitems >= 1;
    if (data) XFree(data);
    g_xdnd_probe_had_error = 0;
    return aware;
}

/* Szuka okna pod kursorem z ustawiona property XdndAware, dokladnie wg
 * algorytmu spec XDND: schodzi od roota przez XQueryPointer, na kazdym
 * poziomie biorac zwrocone "dziecko"; zatrzymuje sie, gdy dziecko ma
 * XdndAware (to nasz target) albo gdy dziecko nie istnieje (kursor nad
 * oknem bez dalszych potomkow - brak targetu). self jest jawnie
 * wykluczone - to jest wlasnie wylaczenie dragu wewnatrz jednego okna
 * z v1 (patrz komentarz na gorze sekcji). Limit iteracji - obrona przed
 * nietypowym managerem okien. */
static Window
XdndFindTargetWindow(Display *dpy, Window root, Window self)
{
    Window cur = root;
    int i;

    for (i = 0; i < 32; i++) {
        Window ret_root, child = None;
        int rx, ry, wx, wy;
        unsigned int mask;
        int (*old_handler)(Display *, XErrorEvent *);
        Bool ok;

        g_xdnd_probe_had_error = 0;
        old_handler = XSetErrorHandler(XdndProbeErrorHandler);
        ok = XQueryPointer(dpy, cur, &ret_root, &child, &rx, &ry, &wx, &wy, &mask);
        XSetErrorHandler(old_handler);
        if (g_xdnd_probe_had_error) {
            g_xdnd_probe_had_error = 0;
            return None;
        }
        if (!ok || child == None)
            return None;

        if (child != self && XdndWindowIsAware(dpy, child, g_xdnd_aware_atom))
            return child;

        cur = child;
    }
    return None;
}

static Atom
XdndActionForState(unsigned int state)
{
    return (state & ControlMask) ? g_xdnd_action_copy_atom : g_xdnd_action_move_atom;
}

/* Odwrotny hit-test do siatki ikon - odtwarza DOKLADNIE ta sama formule
 * columns/cx/cy co petla rysujaca w draw() (grid_w/columns licza sie
 * WYLACZNIE z viewport_r.w, wiec zadnych dodatkowych globali od geometrii
 * siatki nie trzeba - g_viewport_r juz istnieje, cache'owane "do kolka
 * myszy w main()"). Czysta funkcja, bez wywolan ui.c. */
static int
HitTestCellAt(int local_x, int local_y, int scroll_y, UiRect viewport_r)
{
    int grid_w = viewport_r.w - SCROLLBAR_W - 4;
    int columns, col, row, index;
    int gx, gy;

    if (local_x < viewport_r.x || local_x >= viewport_r.x + viewport_r.w ||
        local_y < viewport_r.y || local_y >= viewport_r.y + viewport_r.h)
        return -1;

    if (grid_w < CELL_W) grid_w = CELL_W;
    columns = (grid_w + CELL_GAP) / (CELL_W + CELL_GAP);
    if (columns < 1) columns = 1;

    gx = local_x - viewport_r.x;
    gy = local_y - viewport_r.y + scroll_y;
    if (gx >= grid_w)
        return -1;

    col = gx / (CELL_W + CELL_GAP);
    if (col >= columns || (gx % (CELL_W + CELL_GAP)) >= CELL_W)
        return -1;
    row = gy / (CELL_H + CELL_GAP);
    if ((gy % (CELL_H + CELL_GAP)) >= CELL_H)
        return -1;

    index = row * columns + col;
    if (index < 0 || index >= entry_count)
        return -1;
    return index;
}

/* --- strona zrodla: drag WYCHODZACY z tego okna --------------------- */

static void
XdndCancelDrag(Time t)
{
    if (g_xdnd_target_win != None && g_xdnd_target_ready)
        XdndSendClientMessage(g_dpy, g_xdnd_target_win, g_xdnd_leave_atom,
                               (long) g_win, 0, 0, 0, 0);
    XUngrabPointer(g_dpy, t);
    g_xdnd_dragging = 0;
    g_xdnd_target_win = None;
    g_xdnd_target_ready = 0;
    g_xdnd_target_accepts = 0;
    snprintf(g_status, sizeof(g_status), "Drag cancelled");
}

/* Poczatek sesji draga - wolane z MotionNotify w main() po przekroczeniu
 * progu ruchu od ostatniego ButtonPress na ikonie. Guard na ".." i zly
 * indeks jak w BeginRename/BeginDelete. */
static void
XdndBeginDrag(int index, Time t)
{
    if (index < 0 || index >= entry_count || strcmp(entries[index].name, "..") == 0)
        return;

    if (XGrabPointer(g_dpy, g_win, False, ButtonReleaseMask | PointerMotionMask,
                      GrabModeAsync, GrabModeAsync, None, None, t) != GrabSuccess) {
        snprintf(g_status, sizeof(g_status), "Cannot start drag (pointer grab failed)");
        return;
    }

    JoinPath(cur_path, entries[index].name, g_xdnd_drag_path, sizeof(g_xdnd_drag_path));
    g_xdnd_drag_index = index;
    g_xdnd_drag_is_move = 1;
    g_xdnd_target_win = None;
    g_xdnd_target_ready = 0;
    g_xdnd_target_accepts = 0;
    g_xdnd_action = g_xdnd_action_move_atom;

    XSetSelectionOwner(g_dpy, g_xdnd_selection_atom, g_win, t);
    g_xdnd_dragging = 1;
    snprintf(g_status, sizeof(g_status), "Dragging: %s", entries[index].name);
}

/* Wolane na kazdy MotionNotify w trakcie sesji draga - lokalizuje okno
 * pod kursorem, wysyla Enter (raz na zmiane targetu) + Position (kazdy
 * ruch), Leave przy zmianie/utracie targetu. */
static void
XdndUpdateDrag(int x_root, int y_root, unsigned int state, Time t)
{
    Window target;

    g_xdnd_action = XdndActionForState(state);
    target = XdndFindTargetWindow(g_dpy, DefaultRootWindow(g_dpy), g_win);

    if (target != g_xdnd_target_win) {
        if (g_xdnd_target_win != None && g_xdnd_target_ready)
            XdndSendClientMessage(g_dpy, g_xdnd_target_win, g_xdnd_leave_atom,
                                   (long) g_win, 0, 0, 0, 0);
        g_xdnd_target_win = target;
        g_xdnd_target_ready = 0;
        g_xdnd_target_accepts = 0;
    }

    if (g_xdnd_target_win == None) {
        snprintf(g_status, sizeof(g_status), "Dragging: %s", entries[g_xdnd_drag_index].name);
        return;
    }

    if (!g_xdnd_target_ready) {
        XdndSendClientMessage(g_dpy, g_xdnd_target_win, g_xdnd_enter_atom,
                               (long) g_win, (long) XDND_PROTOCOL_VERSION << 24,
                               (long) XA_STRING, 0, 0);
        g_xdnd_target_ready = 1;
    }
    XdndSendClientMessage(g_dpy, g_xdnd_target_win, g_xdnd_position_atom,
                           (long) g_win, 0,
                           (long) (((x_root & 0xffff) << 16) | (y_root & 0xffff)),
                           (long) t, (long) g_xdnd_action);
    snprintf(g_status, sizeof(g_status), "Dragging: %s (drop to %s)",
             entries[g_xdnd_drag_index].name,
             (g_xdnd_action == g_xdnd_action_copy_atom) ? "copy" : "move");
}

static void
XdndHandleStatus(const XClientMessageEvent *ev)
{
    if ((Window) ev->data.l[0] != g_xdnd_target_win)
        return;  /* spozniona odpowiedz ze starego targetu - ignoruj */

    g_xdnd_target_accepts = (int) (ev->data.l[1] & 1);
    snprintf(g_status, sizeof(g_status), "Dragging: %s (%s)",
             entries[g_xdnd_drag_index].name,
             g_xdnd_target_accepts ? "drop here" : "target rejects");
}

/* Wolane na ButtonRelease w trakcie sesji draga - Drop jesli aktualny
 * target akceptuje, inaczej anulowanie (Leave, jesli byl hoverowany
 * jakis target). Akcja (Move/Copy) liczona z modyfikatora DOKLADNIE w
 * momencie puszczenia LPM, nie z chwili startu draga. */
static void
XdndEndDrag(unsigned int state, Time t)
{
    if (g_xdnd_target_win == None || !g_xdnd_target_accepts) {
        XdndCancelDrag(t);
        return;
    }

    g_xdnd_action = XdndActionForState(state);
    g_xdnd_drag_is_move = (g_xdnd_action != g_xdnd_action_copy_atom);
    XdndSendClientMessage(g_dpy, g_xdnd_target_win, g_xdnd_drop_atom,
                           (long) g_win, 0, (long) t, 0, 0);
    XUngrabPointer(g_dpy, t);
    g_xdnd_dragging = 0;
    snprintf(g_status, sizeof(g_status), "Waiting for drop...");
}

static void
XdndHandleFinished(const XClientMessageEvent *ev)
{
    int success;

    if ((Window) ev->data.l[0] != g_xdnd_target_win)
        return;

    success = (int) (ev->data.l[1] & 1);
    snprintf(g_status, sizeof(g_status), "%s %s",
             (g_xdnd_action == g_xdnd_action_copy_atom) ? "Copy" : "Move",
             success ? "complete" : "failed");

    g_xdnd_target_win = None;
    g_xdnd_target_ready = 0;
    g_xdnd_target_accepts = 0;
}

/* --- strona celu: kazde okno moze byc targetem przychodzacego draga - */

static void
XdndHandleEnter(const XClientMessageEvent *ev)
{
    g_xdnd_pending_source = (Window) ev->data.l[0];
    g_xdnd_pending_version = (int) (ev->data.l[1] >> 24);
}

/* Odrzuca drop, gdy okno ma otwarty modal/menu (spojnie z input_blocked
 * w draw() - drop nie powinien "kraszc" tego, co akurat robi uzytkownik).
 * Bez prostokata "no more events" w odpowiedzi (l[2]/l[3]=0) - target to
 * zawsze lokalny proces, wiec brak throttlingu Position jest ok, patrz
 * komentarz na gorze sekcji. */
static void
XdndHandlePosition(const XClientMessageEvent *ev)
{
    Window source = (Window) ev->data.l[0];
    Atom requested_action = (Atom) ev->data.l[4];
    int accept = (g_xdnd_pending_source == source) &&
                 g_modal == MODAL_NONE && g_menu_open < 0;

    g_xdnd_hover_active = accept;
    XdndSendClientMessage(g_dpy, source, g_xdnd_status_atom, (long) g_win,
                           accept ? 1L : 0L, 0, 0,
                           (long) (accept ? requested_action : None));
    snprintf(g_status, sizeof(g_status), "%s",
             accept ? "Drop here to receive file" : "Busy - cannot accept drop");
}

static void
XdndHandleLeave(const XClientMessageEvent *ev)
{
    if ((Window) ev->data.l[0] != g_xdnd_pending_source)
        return;

    g_xdnd_pending_source = None;
    g_xdnd_pending_version = 0;
    g_xdnd_hover_active = 0;
    g_status[0] = '\0';
}

/* Puszczenie LPM nad tym oknem (jako celem) - zamiast samemu czytac dane,
 * konwertuje XdndSelection identycznie jak RequestPaste konwertuje
 * g_clipboard_atom (ten sam g_paste_prop_atom); g_xdnd_drop_pending
 * odroznia w case SelectionNotify (main()) wynik TEGO zapytania od
 * zwyklego Paste z menu, zeby wiedziec, komu odeslac XdndFinished. */
static void
XdndHandleDrop(const XClientMessageEvent *ev)
{
    Window source = (Window) ev->data.l[0];

    if (source != g_xdnd_pending_source) {
        XdndSendClientMessage(g_dpy, source, g_xdnd_finished_atom, (long) g_win, 0, 0, 0, 0);
        return;
    }

    g_xdnd_drop_pending = 1;
    g_xdnd_drop_source = source;
    g_xdnd_drop_time = (Time) ev->data.l[2];
    g_xdnd_pending_source = None;
    g_xdnd_hover_active = 0;

    XConvertSelection(g_dpy, g_xdnd_selection_atom, XA_STRING, g_paste_prop_atom,
                       g_win, g_xdnd_drop_time);
}

static void
XdndSendFinished(Window dest, int success, Atom action)
{
    XdndSendClientMessage(g_dpy, dest, g_xdnd_finished_atom,
                           (long) g_win, success ? 1L : 0L, (long) action, 0, 0);
}

/* -------------------------------------------------------------------- */
/* Ikona okna - folder, rysowana wprost Xlibem na 1-bitowej Pixmapie     */
/* (jak w pozostalych portach).                                          */
/* -------------------------------------------------------------------- */

static void
DrawFolderIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XFillRectangle(idpy, p, gc, 3, 6, 12, 6);
    XDrawRectangle(idpy, p, gc, 3, 12, 25, 15);
}

static Pixmap
MakeFolderIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawFolderIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Ikonki komorek - rysowane na zywo przez prymitywy ui.c, ten sam duch  */
/* recznego rysowania ksztaltow co IconGrid.c, tylko bez potrzeby        */
/* wlasnych GC/Pixmap (od razu na backbufferze ctx). Katalog: "zakladka" */
/* + korpus. Plik: kwadrat z wcietym rogiem (ui_fill_triangle koloru tla */
/* "wycina" rog, dwie linie zaznaczaja zagiecie). Dekoracje (exec/image/ */
/* text) - patrz oryginalny IconGrid.c po uzasadnienie ksztaltow.        */
/* -------------------------------------------------------------------- */

static void
DrawDirIcon(UiCtx *ctx, int x, int y, int size, const XColor *fill, const XColor *outline)
{
    int tab_w = size * 3 / 5;
    int tab_h = size / 5;
    int body_h = size - tab_h;
    UiRect tab = { x, y, tab_w, tab_h };
    UiRect body = { x, y + tab_h, size, body_h };

    ui_fill_rect(ctx, tab, fill);
    ui_fill_rect(ctx, body, fill);
    ui_draw_border(ctx, body, 1, outline);
}

static void
DrawFileIcon(UiCtx *ctx, int x, int y, int size,
             const XColor *fill, const XColor *outline, const XColor *bg)
{
    UiRect body = { x, y, size, size };
    int fold = size / 4;

    ui_fill_rect(ctx, body, fill);
    ui_fill_triangle(ctx, x + size - fold, y, x + size, y, x + size, y + fold, bg);
    ui_draw_border(ctx, body, 1, outline);
    ui_draw_line(ctx, x + size - fold, y, x + size - fold, y + fold, 1, outline);
    ui_draw_line(ctx, x + size - fold, y + fold, x + size, y + fold, 1, outline);
}

static void
DrawExecDecoration(UiCtx *ctx, int x, int y, int size, const XColor *c)
{
    int s = size / 3;
    int cx = x + size / 2;
    int cy = y + size / 2 + size / 8;

    ui_fill_triangle(ctx, cx - s / 2, cy - s / 2, cx - s / 2, cy + s / 2, cx + s / 2, cy, c);
}

static void
DrawImageDecoration(UiCtx *ctx, int x, int y, int size, const XColor *c)
{
    int pad = size / 6;
    int fx = x + pad, fy = y + size / 3;
    int fw = size - 2 * pad, fh = size / 2;
    UiRect frame = { fx, fy, fw, fh };

    ui_draw_border(ctx, frame, 1, c);
    ui_draw_circle(ctx, fx + 4, fy + 4, 2, 1, c);
    ui_draw_line(ctx, fx, fy + fh, fx + fw / 2, fy + fh / 3, 1, c);
    ui_draw_line(ctx, fx + fw / 2, fy + fh / 3, fx + fw, fy + fh, 1, c);
}

static void
DrawTextDecoration(UiCtx *ctx, int x, int y, int size, const XColor *c)
{
    int pad = size / 6;
    int lx = x + pad, rx = x + size - pad;
    int i;

    for (i = 0; i < 3; i++) {
        int ly = y + size / 3 + i * (size / 6);

        ui_draw_line(ctx, lx, ly, rx, ly, 1, c);
    }
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
DrawCell(UiCtx *ctx, int index, int cx, int cy, int interactive,
         const XColor *dir_c, const XColor *file_c,
         const XColor *select_c, const XColor *bg_c)
{
    UiRect cell_r = { cx, cy, CELL_W, CELL_H };
    UiRect label_r;
    int icon_x, icon_y, label_y;
    const char *name = entries[index].name;
    int kind = entries[index].kind;
    const XColor *fill_c;

    if (index == g_selected_index)
        ui_fill_rect(ctx, cell_r, select_c);

    icon_x = cx + (CELL_W - ICON_SIZE) / 2;
    icon_y = cy + CELL_VPAD;
    fill_c = (kind == KIND_DIRECTORY) ? dir_c : file_c;

    if (kind == KIND_DIRECTORY) {
        DrawDirIcon(ctx, icon_x, icon_y, ICON_SIZE, fill_c, ui_theme_line_fg(ctx));
    } else {
        DrawFileIcon(ctx, icon_x, icon_y, ICON_SIZE, fill_c, ui_theme_line_fg(ctx), bg_c);
        switch (kind) {
        case KIND_EXECUTABLE: DrawExecDecoration(ctx, icon_x, icon_y, ICON_SIZE, ui_theme_line_fg(ctx)); break;
        case KIND_IMAGE:       DrawImageDecoration(ctx, icon_x, icon_y, ICON_SIZE, ui_theme_line_fg(ctx)); break;
        case KIND_TEXT:        DrawTextDecoration(ctx, icon_x, icon_y, ICON_SIZE, ui_theme_line_fg(ctx)); break;
        default: break;
        }
    }

    label_y = icon_y + ICON_SIZE + 3;
    label_r = (UiRect){ cx, label_y, CELL_W, CELL_H - (label_y - cy) };
    ui_label_centered(ctx, label_r, name);

    if (interactive && ui_hit_test(ctx, cell_r)) {
        long now = now_ms();
        int is_double = (index == g_last_click_index) && (now - g_last_click_ms <= DOUBLE_CLICK_MS);

        SelectEntry(index);
        g_last_click_index = index;
        g_last_click_ms = is_double ? 0 : now;
        if (is_double)
            ActivateEntry(index);
    }
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static XColor dir_color, file_color, select_color;
    static int ready = 0;
    UiRect menubar_r, file_r, edit_r, view_r;
    UiRect status_row;
    char status_line[sizeof(g_path_label) + sizeof(g_status) + 4];
    UiRect viewport_r, grid_r, sb_r, up_arrow_r, down_arrow_r, track_r, thumb_r, above_r, below_r;
    int y, bottom_y, viewport_h;
    int margin = ui_window_margin(ctx); /* odstep od LEWEJ/PRAWEJ krawedzi - patrz ui.h */
    int grid_w, columns, rows_total, content_h, max_scroll;
    int first_row, last_row, row, col;
    int menu_open_at_start, input_blocked, any_click, want_quit = 0;
    int drag_was_active;

    if (!ready) {
        ui_color(ctx, "gray70", &dir_color);
        ui_color(ctx, "white", &file_color);
        ui_color(ctx, "gray70", &select_color);
        ready = 1;
    }

    menu_open_at_start = (g_menu_open >= 0);
    input_blocked = (g_modal != MODAL_NONE) || menu_open_at_start;
    any_click = ui_hit_test(ctx, (UiRect){ 0, 0, win_w, win_h });

    /* --- pasek menu: File / Edit / View --- */
    y = 8;
    menubar_r = (UiRect){ margin, y, win_w - 2 * margin, ROW_H };
    file_r = (UiRect){ menubar_r.x, menubar_r.y, MENU_ITEM_W, ROW_H };
    edit_r = (UiRect){ file_r.x + MENU_ITEM_W, menubar_r.y, MENU_ITEM_W, ROW_H };
    view_r = (UiRect){ edit_r.x + MENU_ITEM_W, menubar_r.y, MENU_ITEM_W, ROW_H };
    {
        int file_clicked = ui_button(ctx, file_r, "File");
        int edit_clicked = ui_button(ctx, edit_r, "Edit");
        int view_clicked = ui_button(ctx, view_r, "View");

        /* Klikniecie paska menu dziala ZAWSZE (otwiera gdy nic nie bylo
         * otwarte, przelacza bezposrednio gdy otwarte bylo INNE menu),
         * chyba ze modal jest aktywny - to jedyny wyjatek od
         * "input_blocked wygasza wszystko poza aktywnym overlayem"
         * ponizej, bo pasek menu jest CZESCIA tego mechanizmu, nie
         * czyms co on wygasza. */
        if (g_modal == MODAL_NONE) {
            if (file_clicked) g_menu_open = (g_menu_open == 0) ? -1 : 0;
            else if (edit_clicked) g_menu_open = (g_menu_open == 1) ? -1 : 1;
            else if (view_clicked) g_menu_open = (g_menu_open == 2) ? -1 : 2;
        }
    }
    y += ROW_H + 6;

    /* --- pasek statusu, sam na dole: sciezka + liczba itemow/komunikat
     * akcji na JEDNEJ linii (przyciski akcji przeniesione do menu
     * Edit/File, Up przeniesiony do menu Edit) --- */
    bottom_y = win_h - MARGIN_Y - ROW_H;
    if (bottom_y < y + 20) bottom_y = y + 20;
    status_row = (UiRect){ margin, bottom_y, win_w - 2 * margin, ROW_H };
    snprintf(status_line, sizeof(status_line), "%s  |  %s", g_path_label, g_status);

    /* --- viewport (siatka ikon + scrollbar) --- */
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
            DrawCell(ctx, index, cx, cy, !input_blocked,
                     &dir_color, &file_color, &select_color, ui_theme_box_bg(ctx));
        }
    }
    ui_clear_clip(ctx);

    /* scrollbar - strzalki (krok = 1 wiersz), przeciaganie kciuka i
     * kliknieca na torze nad/pod kciukiem (strona w gore/dol).
     * Przeciaganie idzie przez ui_mouse_state (surowa pozycja/stan LPM) -
     * ui.c nadal nie trzyma stanu "trwajacy drag" w publicznym API,
     * sesja (g_thumb_dragging i in.) jest wlasnoscia TEJ apki, patrz
     * komentarz przy tych zmiennych na gorze pliku. */
    sb_r = (UiRect){ grid_r.x + grid_r.w + 4, viewport_r.y, SCROLLBAR_W, viewport_r.h };
    {
        int arrow_h = 14;

        up_arrow_r = (UiRect){ sb_r.x, sb_r.y, sb_r.w, arrow_h };
        down_arrow_r = (UiRect){ sb_r.x, sb_r.y + sb_r.h - arrow_h, sb_r.w, arrow_h };
        track_r = (UiRect){ sb_r.x, sb_r.y + arrow_h, sb_r.w, sb_r.h - 2 * arrow_h };
        if (track_r.h < 0) track_r.h = 0;
    }
    /* z POPRZEDNIEJ klatki - jesli przeciaganie wlasnie trwalo, puszczenie
     * LPM w TEJ klatce je konczy (nizej), ale NIE powinno TAKZE liczyc sie
     * jako klik na strzalke/tor - inaczej dublujemy skok przewijania tuz
     * po tym, jak przeciaganie juz ustawilo docelowy g_scroll_y. */
    drag_was_active = g_thumb_dragging;
    {
        int up_clicked = ui_button(ctx, up_arrow_r, "^");
        int down_clicked = ui_button(ctx, down_arrow_r, "v");

        if (!input_blocked && !drag_was_active) {
            if (up_clicked) g_scroll_y -= (CELL_H + CELL_GAP);
            if (down_clicked) g_scroll_y += (CELL_H + CELL_GAP);
        }
    }
    /* przycieta ZARAZ po strzalkach, PRZED policzeniem thumb_y ponizej -
     * inaczej klikniecie strzalki juz na granicy chwilowo przekracza
     * max_scroll, a kciuk rysuje sie tej samej klatki z nieprzycieta
     * wartoscia (wyjezdza poza tor). */
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

        /* start: LPM WLASNIE wcisniety (mdown), kursor nad biezacym
         * kciukiem, zadne przeciaganie jeszcze nie trwa. Punkt odniesienia
         * to Y kursora W TEJ klatce i g_scroll_y SPRZED zmiany - kolejne
         * klatki licza przesuniecie wzgledem NIEGO (nie wzgledem biezacej
         * pozycji kciuka), zeby uchwycenie myszy w dowolnym miejscu na
         * kciuku nie "przeskakiwalo" go pod kursor. */
        if (!input_blocked && !g_thumb_dragging && mdown && drag_range > 0) {
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
                g_thumb_dragging = 0;   /* LPM puszczony - koniec sesji */
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

    if (!input_blocked && !drag_was_active) {
        if (above_r.h > 0 && ui_hit_test(ctx, above_r)) g_scroll_y -= viewport_r.h;
        if (below_r.h > 0 && ui_hit_test(ctx, below_r)) g_scroll_y += viewport_r.h;
        if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
        if (g_scroll_y < 0) g_scroll_y = 0;
    }

    ui_draw_border(ctx, viewport_r, 1, ui_theme_line_fg(ctx));
    if (g_xdnd_hover_active)
        ui_draw_border(ctx, viewport_r, 2, ui_theme_accent(ctx));
    ui_label(ctx, status_row, status_line);

    /* --- rozwiniete menu (jesli otwarte) - rysowane NA WIERZCHU
     * wszystkiego powyzej, ten sam wzorzec co dropdown priorytetu w
     * examples/7atodo.c: apka sama decyduje co jest w srodku, biblioteka
     * (ui_menu_item) tylko rysuje+hit-testuje pojedynczy wiersz. --- */
    if (g_modal == MODAL_NONE && g_menu_open >= 0) {
        int which = g_menu_open;
        UiRect anchor = (which == 0) ? file_r : (which == 1) ? edit_r : view_r;
        int n_items = (which == 0) ? 2 : (which == 1) ? 6 : 1;
        UiRect dropdown_r = { anchor.x, anchor.y + anchor.h, MENU_DROPDOWN_W, n_items * ROW_H };
        int consumed = 0;

        if (dropdown_r.x + dropdown_r.w > win_w - margin)
            dropdown_r.x = win_w - margin - dropdown_r.w;
        if (dropdown_r.x < margin)
            dropdown_r.x = margin;

        ui_fill_rect(ctx, dropdown_r, ui_theme_box_bg(ctx));

        /* ui_menu_item rysuje+hit-testuje w JEDNYM wywolaniu (jak
         * ui_button) - musimy je wiec wywolac zawsze (zeby wpisy sie
         * NARYSOWALY), ale WYNIK klikniecia wolno zastosowac tylko gdy
         * menu bylo juz otwarte NA POCZATKU tej klatki. Bez tego
         * warunku klikniecie samego "File"/"Edit"/"View" (ktore
         * WLASNIE otworzylo menu w tej samej klatce) zostaloby TEZ
         * policzone tutaj jako "klik gdziekolwiek indziej" i
         * natychmiast zamknelo dopiero co otwarte menu - ten sam blad i
         * to samo rozwiazanie (menu_open_at_start) co przy dropdownie
         * priorytetu w examples/7atodo.c. */
        if (which == 0) {
            UiRect r0 = { dropdown_r.x, dropdown_r.y, dropdown_r.w, ROW_H };
            UiRect r1 = { dropdown_r.x, dropdown_r.y + ROW_H, dropdown_r.w, ROW_H };
            int c0 = ui_menu_item(ctx, r0, "New Window");
            int c1 = ui_menu_item(ctx, r1, "Quit");

            if (menu_open_at_start) {
                if (c0) { SpawnNewWindow(); consumed = 1; g_menu_open = -1; }
                else if (c1) { want_quit = 1; consumed = 1; g_menu_open = -1; }
            }
        } else if (which == 1) {
            UiRect r0 = { dropdown_r.x, dropdown_r.y, dropdown_r.w, ROW_H };
            UiRect r1 = { dropdown_r.x, dropdown_r.y + ROW_H, dropdown_r.w, ROW_H };
            UiRect r2 = { dropdown_r.x, dropdown_r.y + 2 * ROW_H, dropdown_r.w, ROW_H };
            UiRect r3 = { dropdown_r.x, dropdown_r.y + 3 * ROW_H, dropdown_r.w, ROW_H };
            UiRect r4 = { dropdown_r.x, dropdown_r.y + 4 * ROW_H, dropdown_r.w, ROW_H };
            UiRect r5 = { dropdown_r.x, dropdown_r.y + 5 * ROW_H, dropdown_r.w, ROW_H };
            int c0 = ui_menu_item(ctx, r0, "Up");
            int c1 = ui_menu_item(ctx, r1, "Copy");
            int c2 = ui_menu_item(ctx, r2, "Cut");
            int c3 = ui_menu_item(ctx, r3, "Paste");
            int c4 = ui_menu_item(ctx, r4, "Rename");
            int c5 = ui_menu_item(ctx, r5, "Delete");

            if (menu_open_at_start) {
                if (c0) {
                    char parent[PATH_MAX];

                    GetParentPath(cur_path, parent, sizeof(parent));
                    NavigateTo(parent);
                    consumed = 1; g_menu_open = -1;
                } else if (c1) { MarkClipboard(0); consumed = 1; g_menu_open = -1; }
                else if (c2) { MarkClipboard(1); consumed = 1; g_menu_open = -1; }
                else if (c3) { RequestPaste(); consumed = 1; g_menu_open = -1; }
                else if (c4) { BeginRename(); consumed = 1; g_menu_open = -1; }
                else if (c5) { BeginDelete(); consumed = 1; g_menu_open = -1; }
            }
        } else {
            UiRect r0 = { dropdown_r.x, dropdown_r.y, dropdown_r.w, ROW_H };
            char label[40];
            int c0;

            snprintf(label, sizeof(label), "%s Show Hidden Files", app_data.show_hidden ? "[x]" : "[ ]");
            c0 = ui_menu_item(ctx, r0, label);
            if (menu_open_at_start && c0) { ActionToggleHidden(); consumed = 1; g_menu_open = -1; }
        }

        ui_draw_border(ctx, dropdown_r, 1, ui_theme_line_fg(ctx));

        if (menu_open_at_start && !consumed && any_click)
            g_menu_open = -1;
    }

    /* --- modal Rename/Delete - wygrywa nad wszystkim, patrz komentarz
     * przy g_modal na gorze pliku --- */
    if (g_modal != MODAL_NONE) {
        int modal_w = 260;
        int modal_h = (g_modal == MODAL_RENAME) ? 90 : 70;
        UiRect modal_r = { (win_w - modal_w) / 2, (win_h - modal_h) / 2, modal_w, modal_h };
        UiRect ok_r = { modal_r.x + modal_r.w - 2 * 70 - 6 - 10, modal_r.y + modal_r.h - ROW_H - 8, 70, ROW_H };
        UiRect cancel_r = { modal_r.x + modal_r.w - 70 - 10, modal_r.y + modal_r.h - ROW_H - 8, 70, ROW_H };

        ui_fill_rect(ctx, modal_r, ui_theme_box_bg(ctx));

        if (g_modal == MODAL_RENAME) {
            UiRect label_r = { modal_r.x + 10, modal_r.y + 8, modal_r.w - 20, ROW_H };
            UiRect field_r = { modal_r.x + 10, modal_r.y + 8 + ROW_H + 4, modal_r.w - 20, ROW_H };

            ui_label(ctx, label_r, "Rename to:");
            ui_textbox(ctx, field_r, g_rename_buf, sizeof(g_rename_buf), &g_rename_cursor);
            if (ui_button(ctx, ok_r, "OK")) ConfirmRename();
            if (ui_button(ctx, cancel_r, "Cancel")) g_modal = MODAL_NONE;
        } else {
            char msg[300];
            UiRect label_r = { modal_r.x + 10, modal_r.y + 10, modal_r.w - 20, ROW_H };

            if (g_modal_index >= 0 && g_modal_index < entry_count)
                snprintf(msg, sizeof(msg), "Delete \"%s\"?", entries[g_modal_index].name);
            else
                snprintf(msg, sizeof(msg), "Delete?");
            ui_label_centered(ctx, label_r, msg);
            if (ui_button(ctx, ok_r, "Delete")) ConfirmDelete();
            if (ui_button(ctx, cancel_r, "Cancel")) g_modal = MODAL_NONE;
        }

        ui_draw_border(ctx, modal_r, 1, ui_theme_line_fg(ctx));
    }

    return want_quit;
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
    const char *start_path = ".";
    int i;
    int running, redraw;
    XEvent ev;

    signal(SIGCHLD, SIG_IGN);
    snprintf(program_path, sizeof(program_path), "%s", argv[0]);

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        } else if (argv[i][0] != '-') {
            start_path = argv[i];
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    g_dpy = dpy;

    ReadAppString(dpy, "7aFm.opener", "7aFm.Opener", app_data.opener, sizeof(app_data.opener), "");
    ReadAppString(dpy, "7aFm.mimeOpeners", "7aFm.MimeOpeners",
                  app_data.mime_openers, sizeof(app_data.mime_openers), "");
    app_data.show_hidden = ReadAppBool(dpy, "7aFm.showHidden", "7aFm.ShowHidden", 0);

    magic_cookie = magic_open(MAGIC_MIME_TYPE);
    if (magic_cookie && magic_load(magic_cookie, NULL) != 0) {
        magic_close(magic_cookie);
        magic_cookie = NULL;
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
    XStoreName(dpy, win, "7aFm");
    XSetIconName(dpy, win, "7aFm");

    g_clipboard_atom = XInternAtom(dpy, "7AFM_FILE", False);
    g_paste_prop_atom = XInternAtom(dpy, "7AFM_PASTE_PROP", False);

    g_xdnd_aware_atom = XInternAtom(dpy, "XdndAware", False);
    g_xdnd_enter_atom = XInternAtom(dpy, "XdndEnter", False);
    g_xdnd_position_atom = XInternAtom(dpy, "XdndPosition", False);
    g_xdnd_status_atom = XInternAtom(dpy, "XdndStatus", False);
    g_xdnd_leave_atom = XInternAtom(dpy, "XdndLeave", False);
    g_xdnd_drop_atom = XInternAtom(dpy, "XdndDrop", False);
    g_xdnd_finished_atom = XInternAtom(dpy, "XdndFinished", False);
    g_xdnd_selection_atom = XInternAtom(dpy, "XdndSelection", False);
    g_xdnd_action_copy_atom = XInternAtom(dpy, "XdndActionCopy", False);
    g_xdnd_action_move_atom = XInternAtom(dpy, "XdndActionMove", False);
    {
        /* format 32 - Xlib oczekuje elementow typu long, nawet na LP64,
         * nie int32_t (patrz komentarz w sekcji XDND wyzej). */
        long xdnd_version = XDND_PROTOCOL_VERSION;

        XChangeProperty(dpy, win, g_xdnd_aware_atom, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &xdnd_version, 1);
    }

    icon = MakeFolderIconPixmap(dpy, root);
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

    /* Narysuj OD RAZU jedna (pusta) klatke, ZANIM NavigateTo() przeczyta
     * katalog (opendir/readdir/stat + magic_file per wpis - dla duzych
     * katalogow to realny czas) - bez tego okno wisialoby puste przez
     * caly ten czas. Ten sam mechanizm co w examples/7aweather.c/
     * 7asensors.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    NavigateTo(start_path);

    running = 1;
    redraw = 1;  /* pokaz swiezy listing od razu, bez czekania na kolejne zdarzenie */

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy (Button4/5 - gora/dol) przechwycone TU, PRZED
             * ui_feed_event: ui.c nie rozroznia numeru przycisku, wiec
             * para ButtonPress/Release od kolka zostalaby policzona jak
             * zwykly klik na cokolwiek jest akurat pod kursorem. */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress && g_modal == MODAL_NONE && g_menu_open < 0 &&
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
                /* kandydat na drag - patrz sekcja XDND. Reset na kazdy
                 * press (rowniez gdy input_blocked), zeby stary indeks
                 * nie przetrwal np. otwarcia menu miedzy press a motion. */
                g_xdnd_press_index = -1;
                if (ev.xbutton.button == Button1 && g_modal == MODAL_NONE && g_menu_open < 0) {
                    int idx = HitTestCellAt(ev.xbutton.x, ev.xbutton.y, g_scroll_y, g_viewport_r);

                    if (idx >= 0 && strcmp(entries[idx].name, "..") != 0) {
                        g_xdnd_press_index = idx;
                        g_xdnd_press_x = ev.xbutton.x;
                        g_xdnd_press_y = ev.xbutton.y;
                    }
                }
                redraw = 1;
                break;
            case ButtonRelease:
                if (g_xdnd_dragging)
                    XdndEndDrag(ev.xbutton.state, ev.xbutton.time);
                g_xdnd_press_index = -1;
                redraw = 1;
                break;
            case MotionNotify:
                if (g_xdnd_dragging) {
                    XdndUpdateDrag(ev.xmotion.x_root, ev.xmotion.y_root,
                                    ev.xmotion.state, ev.xmotion.time);
                } else if (g_xdnd_press_index >= 0) {
                    int dx = ev.xmotion.x - g_xdnd_press_x;
                    int dy = ev.xmotion.y - g_xdnd_press_y;

                    if (dx * dx + dy * dy > XDND_DRAG_THRESHOLD_PX * XDND_DRAG_THRESHOLD_PX)
                        XdndBeginDrag(g_xdnd_press_index, ev.xmotion.time);
                }
                redraw = 1;
                break;
            case KeyPress:
                if (g_xdnd_dragging && XLookupKeysym(&ev.xkey, 0) == XK_Escape)
                    XdndCancelDrag(ev.xkey.time);
                redraw = 1;
                break;
            case ClientMessage:
                if (ev.xclient.message_type == g_xdnd_enter_atom)
                    XdndHandleEnter(&ev.xclient);
                else if (ev.xclient.message_type == g_xdnd_position_atom)
                    XdndHandlePosition(&ev.xclient);
                else if (ev.xclient.message_type == g_xdnd_status_atom)
                    XdndHandleStatus(&ev.xclient);
                else if (ev.xclient.message_type == g_xdnd_leave_atom)
                    XdndHandleLeave(&ev.xclient);
                else if (ev.xclient.message_type == g_xdnd_drop_atom)
                    XdndHandleDrop(&ev.xclient);
                else if (ev.xclient.message_type == g_xdnd_finished_atom)
                    XdndHandleFinished(&ev.xclient);
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
            case SelectionRequest:
                HandleSelectionRequest(&ev.xselectionrequest);
                break;
            case SelectionNotify:
                if (ev.xselection.property == None) {
                    snprintf(g_status, sizeof(g_status), "Clipboard is empty (Copy/Cut first)");
                    if (g_xdnd_drop_pending) {
                        XdndSendFinished(g_xdnd_drop_source, 0, None);
                        g_xdnd_drop_pending = 0;
                    }
                } else {
                    int ok = HandlePasteReceived();

                    if (g_xdnd_drop_pending) {
                        XdndSendFinished(g_xdnd_drop_source, ok,
                                          g_xdnd_last_paste_was_move
                                              ? g_xdnd_action_move_atom : g_xdnd_action_copy_atom);
                        g_xdnd_drop_pending = 0;
                    }
                }
                redraw = 1;
                break;
            case SelectionClear:
                /* inny klient przejal zaznaczenie - nic do zrobienia */
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
    if (magic_cookie) magic_close(magic_cookie);
    return 0;
}
