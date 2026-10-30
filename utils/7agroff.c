/*
 * 7agroff.c - pomoc do edycji dokumentow groff/mm w ~/projects/groff (albo
 * innym katalogu, patrz zasob 7aGroff.dir) na bibliotece ui.c/ui.h z tego
 * katalogu.
 *
 * Nowa apka (nie port Xt/Xaw - nie ma czego portowac, jak 7arss.c/7askm.c/
 * 7ashop.c). Katalog docelowy trzyma pary plikow o tej samej nazwie bazowej:
 * DOKUMENT.mm (zrodlo) i DOKUMENT.pdf (wynik kompilacji przez "make"). Uklad
 * okna to JEDEN box z lista dokumentow (nazwa bazowa - para .mm/.pdf to
 * jeden wiersz, kolumna "Type" mowi, ktore pliki istnieja). Wiersz wybiera
 * sie kliknieciem (ui_hit_test, stan PRZED rysowaniem wiersza), a pod lista
 * jest pasek przyciskow KONTEKSTOWYCH zaleznych od wybranego dokumentu:
 * jest .mm -> Edit/Compile/Delete, jest .pdf -> Open, oba -> wszystkie.
 * Reload/Help nie dotycza pliku, wiec sa zawsze widoczne (po prawej). Box
 * ma STALA liczbe widocznych wierszy (VISIBLE_ROWS) i scroll kolkiem myszy
 * - ten sam wzorzec co w 7askm.c (znak "v" w naglowku, gdy ponizej jest
 * wiecej danych; g_list_r przechwytywany PRZED ui_feed_event w main(), bo
 * ui.c nie rozroznia numeru przycisku myszy). Wybor pamietany po nazwie,
 * wiec przezywa Reload (patrz ScanDirectory).
 *
 * Akcje dla .mm:
 *   - Edit:    "cd DIR && EDITOR nazwa.mm" W TERMINALU (zasob
 *              7aGroff.editor) - nvim/vim to TUI, wymaga TTY niezaleznie od
 *              widocznosci bledow.
 *   - Delete:  potwierdzenie przez "7amessage -confirm" (ten sam wzorzec co
 *              g_cmds w utils/7aexit.c: "7amessage -confirm '...' && rm
 *              -f '...'" jako JEDNA komenda powloki, odpalona fire-and-
 *              -forget - rodzic (7agroff) NIE czeka na wynik, dialog
 *              potwierdzenia to WLASNE okno X, nie terminal, wiec BEZ
 *              RunInTerminal). Usuwa TYLKO plik .mm (nie odpowiadajacy .pdf).
 * Akcja dla .pdf:
 *   - Open:    "VIEWER nazwa.pdf" BEZ terminala/powloki (zasob
 *              7aGroff.viewer, domyslnie "gv --watch") - patrz RunDirect.
 *              Viewer to apka GUI (wlasne okno), nie TUI, i - w
 *              odroznieniu od Compile - widocznosc jej bledow nie jest tu
 *              istotna, wiec terminal by tylko przeszkadzal (osobne, puste
 *              okno terminala obok okna gv). "--watch" samo odswieza
 *              podglad po recznej kompilacji, wiec apka NIE robi wlasnego
 *              watchowania plikow/auto-refresh listy - patrz nizej.
 *   - Compile: "cd DIR && make" W TERMINALU, BEZ argumentu (nie per-plik,
 *              zgodnie z Makefile uzytkownika, ktory sam decyduje co
 *              kompiluje domyslny target - ta apka nie zna i nie zaklada
 *              niczego o jego regulach). Kontekstowy (tylko gdy wybrany
 *              dokument ma .mm), choc samo "make" od wyboru nie zalezy.
 *              Terminal tu jest wyrazne zyczenie uzytkownika, zeby bledy
 *              make/groff byly widoczne.
 * Przyciski niezalezne od wyboru (patrz draw()):
 *   - New:     tworzy PUSTY plik nazwa.mm (bez szablonu - patrz
 *              CreateNewMmFile) z wlasnego boxa nad lista, wzorem pola
 *              "New product"/AddCatalogItem w utils/7ashop.c (tu jako
 *              ODREBNY box - wyrazne zyczenie uzytkownika).
 *   - Help:    "man groff_mm" W TERMINALU (jak Edit - man to pager, wymaga
 *              TTY niezaleznie od widocznosci bledow).
 *   - Reload:  reskan katalogu.
 * Edit/Compile/Help trzymaja terminal otwarty do Enter przy bledzie (ten
 * sam wzorzec co SpawnCommand w utils/7atodo.c) - inaczej terminal
 * domkniety razem z "sh -c" zamknalby sie od razu i blad przemknalby bez
 * szans na przeczytanie.
 *
 * Odswiezanie listy: WYLACZNIE na starcie i przez przycisk "Reload"/klawisz
 * 'r' (reskan katalogu, patrz ScanDirectory) - bez timera/select() jak w
 * 7aweather.c/7asys.c, bo kompilacja jest jawna akcja uzytkownika (Compile),
 * a podglad PDF sam sie odswieza dzieki "gv --watch". Z tego samego powodu
 * New/Delete/Compile NIE wywoluja ScanDirectory() automatycznie po swoim
 * skutku (New dziala synchronicznie w tym procesie, wiec odswieza liste od
 * razu po sukcesie; Delete/Compile dzialaja asynchronicznie w oddzielnym
 * procesie potomnym, wiec efekt widac po recznym Reload/'r'). Wskaznik
 * "outdated" (nazwa pliku .mm w innym kolorze, patrz g_warn_color w draw())
 * pojawia sie, gdy odpowiadajacy .pdf nie istnieje albo jest starszy niz .mm
 * (mtime) - tani koszt, bo stat() jest juz wykonywany przy skanowaniu.
 *
 * Pliki zaczynajace sie od "." (w tym typowe pliki tymczasowe nvim/vim jak
 * ".dokument.mm.un~") sa pomijane - patrz filtr w ScanDirectory.
 *
 * Caly tekst WIDOCZNY w oknie jest po angielsku, jak w reszcie utils/
 * 7a*.c - komentarze zostaja po polsku.
 */

#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"
#include "icons/description_48.xbm"

#define ROW_H 20
#define ARROW_W 20      /* waski slot na prawej krawedzi naglowka na znak "v" */
#define VISIBLE_ROWS 10 /* stala liczba widocznych wierszy listy - nadmiar przewijany kolkiem */
#define TYPE_COL_W 90   /* kolumna "Type" (".mm .pdf") przy prawej krawedzi wiersza */
#define BTN_GAP 6
#define MAX_CMD_TOKENS 24
#define MAX_FILES 256
#define NAME_LEN 200
#define DIR_LEN 768
#define PATH_LEN 1024

/* Te same wartosci, ktorych draw() uzywa do wypelnienia UiBoxStyle -
 * wydzielone tutaj, zeby ComputeContentHeight() w main() nie mogla wyjsc
 * z synchronizacji ze stylem, ktory faktycznie renderuje boxy (ten sam
 * wzorzec i nazwy co w utils/7ashop.c). */
#define BOX_BORDER_W 1
#define BOX_PAD_TB 6
#define BOX_MARGIN_TB 6
#define BOX_GAP 2

/* Jeden dokument = nazwa bazowa; .mm i .pdf o tej samej nazwie laduja w
 * TYM SAMYM wpisie (has_mm/has_pdf), mtime wazne tylko gdy has_* = 1. */
typedef struct {
    char name[NAME_LEN]; /* nazwa bazowa, bez rozszerzenia */
    time_t mm_mtime;
    time_t pdf_mtime;
    char has_mm;
    char has_pdf;
} DocEntry;

typedef struct {
    char dir[DIR_LEN];
    char editor[128];   /* np. "nvim" */
    char terminal[128]; /* np. "urxvt"; pusty = polecenie bez terminala */
    char viewer[128];   /* np. "gv --watch" */
} AppData;

static AppData app_data;

static DocEntry g_docs[MAX_FILES];
static int g_doc_count = 0;
static int g_sel = -1; /* indeks w g_docs albo -1 = nic nie wybrano */

static char g_status[160] = "";

static int g_scroll = 0;
static UiRect g_list_r;

/* Pole "New:" nad lewym boxem - ten sam wzorzec co g_new_item_buf/cursor w
 * utils/7ashop.c. */
static char g_new_name[NAME_LEN] = "";
static int  g_new_cursor = 0;

/* "~/projects/groff" -> "$HOME/projects/groff" - tylko prefiks "~/" albo
 * samo "~", bez pelnej semantyki powloki (~user itp. - niepotrzebne tutaj). */
static void
ExpandHome(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, outsz, "%s%s", home, in + 1);
    else
        snprintf(out, outsz, "%s", in);
}

/* -------------------------------------------------------------------- */
/* Skanowanie katalogu - listy .mm/.pdf, posortowane alfabetycznie.      */
/* -------------------------------------------------------------------- */

static int
CompareEntries(const void *a, const void *b)
{
    const DocEntry *da = a, *db = b;

    return strcmp(da->name, db->name);
}

static int
FindDoc(const char *name)
{
    int i;

    for (i = 0; i < g_doc_count; i++)
        if (strcmp(g_docs[i].name, name) == 0)
            return i;
    return -1;
}

/* Wpis dla nazwy bazowej name[0..len) - istniejacy albo nowy; NULL gdy
 * tablica pelna. Liniowe szukanie wystarcza przy MAX_FILES=256 i skanie
 * tylko na starcie/Reload (nie per-klatke). */
static DocEntry *
DocForName(const char *name, size_t len)
{
    char base[NAME_LEN];
    int i;

    snprintf(base, sizeof(base), "%.*s", (int) len, name);
    i = FindDoc(base);
    if (i >= 0)
        return &g_docs[i];
    if (g_doc_count >= MAX_FILES)
        return NULL;
    memset(&g_docs[g_doc_count], 0, sizeof(DocEntry));
    snprintf(g_docs[g_doc_count].name, NAME_LEN, "%s", base);
    return &g_docs[g_doc_count++];
}

static void
ScanDirectory(void)
{
    DIR *dp;
    struct dirent *de;
    char sel_name[NAME_LEN] = "";

    /* wybor po NAZWIE, nie indeksie - po reskanie indeksy moga sie przesunac */
    if (g_sel >= 0 && g_sel < g_doc_count)
        snprintf(sel_name, sizeof(sel_name), "%s", g_docs[g_sel].name);
    g_sel = -1;
    g_doc_count = 0;
    g_status[0] = '\0';

    dp = opendir(app_data.dir);
    if (!dp) {
        snprintf(g_status, sizeof(g_status), "Error: cannot open %.130s", app_data.dir);
        return;
    }

    while ((de = readdir(dp)) != NULL) {
        size_t len = strlen(de->d_name);
        char path[PATH_LEN];
        struct stat st;
        DocEntry *doc;
        int is_mm, is_pdf;

        /* pomija dotfiles - w tym typowe pliki tymczasowe nvim/vim, patrz
         * naglowek pliku (np. ".dokument.mm.un~" ma sufiks "un~", nie
         * ".mm", ale dotfile filtrujemy jawnie i tak, dla przejrzystosci). */
        if (de->d_name[0] == '.') continue;

        is_mm = len > 3 && strcmp(de->d_name + len - 3, ".mm") == 0;
        is_pdf = len > 4 && strcmp(de->d_name + len - 4, ".pdf") == 0;
        if (!is_mm && !is_pdf) continue;

        snprintf(path, sizeof(path), "%s/%s", app_data.dir, de->d_name);
        if (stat(path, &st) != 0) continue;

        doc = DocForName(de->d_name, len - (is_mm ? 3 : 4));
        if (!doc) continue;
        if (is_mm) {
            doc->has_mm = 1;
            doc->mm_mtime = st.st_mtime;
        } else {
            doc->has_pdf = 1;
            doc->pdf_mtime = st.st_mtime;
        }
    }
    closedir(dp);

    qsort(g_docs, g_doc_count, sizeof(DocEntry), CompareEntries);
    if (sel_name[0] != '\0')
        g_sel = FindDoc(sel_name);
}

/* Tworzy PUSTY plik "raw.mm" w app_data.dir (bez szablonu tresci - to
 * apka do zarzadzania plikami, nie edytor; tresc dopisuje sie przez Edit).
 * Obciecie bialych znakow z obu stron - ten sam wzorzec co AddCatalogItem w
 * utils/7ashop.c. O_EXCL (nie O_TRUNC) - nigdy nie nadpisuje istniejacego
 * pliku, nawet w wyscigu z inna kopia tej apki/recznym "touch" w terminalu. */
static void
CreateNewMmFile(const char *raw)
{
    char name[NAME_LEN];
    char path[PATH_LEN];
    size_t start = 0, len;
    int fd;

    while (raw[start] == ' ' || raw[start] == '\t')
        start++;
    snprintf(name, sizeof(name), "%s", raw + start);
    len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
        name[--len] = '\0';

    if (len == 0) {
        snprintf(g_status, sizeof(g_status), "Enter a file name");
        return;
    }
    if (name[0] == '.' || strchr(name, '/') != NULL) {
        snprintf(g_status, sizeof(g_status), "Invalid name: \"%.100s\"", name);
        return;
    }

    snprintf(path, sizeof(path), "%s/%s.mm", app_data.dir, name);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "Cannot create \"%.100s.mm\" (%s)", name, strerror(errno));
        return;
    }
    close(fd);

    ScanDirectory(); /* przeladowanie listy - czysci g_status, wiec komunikat sukcesu ustawiamy PO */
    g_sel = FindDoc(name); /* nowy plik od razu wybrany - Edit pod reka */
    snprintf(g_status, sizeof(g_status), "Created \"%.100s.mm\"", name);
}

/* -------------------------------------------------------------------- */
/* Odpalanie akcji - Edit/Compile/Open, wszystkie przez terminal (patrz   */
/* naglowek pliku). Tokenizacja+re-quoting polecenia (editor/terminal/    */
/* viewer moze byc wieloslowne, np. "gv --watch") - ten sam wzorzec co    */
/* AppendTokens/AppendShellQuoted/SpawnCommand w utils/7atodo.c.       */
/* -------------------------------------------------------------------- */

static void
AppendTokens(char *argv[], int *argc, int max, char *cmd)
{
    char *tok = strtok(cmd, " \t");

    while (tok && *argc < max - 1) {
        argv[(*argc)++] = tok;
        tok = strtok(NULL, " \t");
    }
}

static void
AppendShellQuoted(char *out, size_t outsz, const char *s)
{
    size_t len = strlen(out);
    char *p = out + len;
    size_t remaining = (outsz > len) ? outsz - len : 0;

    if (remaining < 3)
        return;
    *p++ = '\'';
    remaining--;
    for (; *s && remaining > 5; s++) {
        if (*s == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
            remaining -= 4;
        } else {
            *p++ = *s;
            remaining--;
        }
    }
    *p++ = '\'';
    *p = '\0';
}

/* Rozbija cmd (moze byc wieloslowne, np. "gv --watch") na tokeny i sklada
 * je z powrotem, KAZDY osobno shell-quoted, po czym dokleja tak samo
 * quoted arg (nazwa pliku) - bezpieczne nawet gdyby nazwa pliku zawierala
 * spacje/apostrofy. */
static void
BuildQuotedCommand(char *out, size_t outsz, const char *cmd, const char *arg)
{
    char cmd_buf[256];
    char *tokens[MAX_CMD_TOKENS];
    int argc = 0, i;

    snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
    AppendTokens(tokens, &argc, MAX_CMD_TOKENS, cmd_buf);

    out[0] = '\0';
    for (i = 0; i < argc; i++) {
        if (i > 0) strncat(out, " ", outsz - strlen(out) - 1);
        AppendShellQuoted(out, outsz, tokens[i]);
    }
    strncat(out, " ", outsz - strlen(out) - 1);
    AppendShellQuoted(out, outsz, arg);
}

/* Odpala inner_cmd (juz zbudowane przez BuildQuotedCommand) w katalogu
 * app_data.dir, w terminalu z app_data.terminal - z Enterem-do-zamkniecia
 * przy bledzie, tak jak SpawnCommand w utils/7atodo.c. */
static void
RunInTerminal(const char *inner_cmd)
{
    static char shell_cmd[4096];
    char quoted_dir[DIR_LEN + 8];
    char terminal_buf[256];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;

    quoted_dir[0] = '\0';
    AppendShellQuoted(quoted_dir, sizeof(quoted_dir), app_data.dir);

    snprintf(shell_cmd, sizeof(shell_cmd),
             "( cd %s && %s ) || { echo; echo '[7agroff] polecenie nie powiodlo sie"
             " - sprawdz 7aGroff.editor/7aGroff.terminal/Makefile'; "
             "read -r _; }", quoted_dir, inner_cmd);

    snprintf(terminal_buf, sizeof(terminal_buf), "%s", app_data.terminal);
    if (*terminal_buf) {
        AppendTokens(argv, &argc, MAX_CMD_TOKENS, terminal_buf);
        argv[argc++] = "-e";
        argv[argc++] = "sh";
        argv[argc++] = "-c";
        argv[argc++] = shell_cmd;
    } else {
        argv[argc++] = "sh";
        argv[argc++] = "-c";
        argv[argc++] = shell_cmd;
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    /* SIGCHLD = SIG_IGN w main(), jadro sprzatnie proces potomny samo. */
}

/* Jak RunInTerminal, ale BEZ terminala i BEZ powloki - do polecen GUI
 * (viewer), gdzie ani TTY, ani widocznosc bledow nie sa potrzebne (patrz
 * naglowek pliku - w odroznieniu od Edit/Compile/Help). cmd moze byc
 * wieloslowne (np. "gv --watch") - tokenizowane tak jak terminal_buf w
 * RunInTerminal, ale bez shell-quotowania (execvp nie przechodzi przez
 * powloke, wiec filename jako WLASNY element argv jest bezpieczny nawet ze
 * spacjami - nie trzeba go quotowac jak w BuildQuotedCommand). */
static void
RunDirect(const char *cmd, const char *filename)
{
    char cmd_buf[256];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;

    snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS, cmd_buf);
    argv[argc++] = (char *) filename;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        if (chdir(app_data.dir) != 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
}

static void
ActionEdit(const char *name)
{
    char filename[NAME_LEN + 4];
    char inner[700];

    snprintf(filename, sizeof(filename), "%s.mm", name);
    BuildQuotedCommand(inner, sizeof(inner), app_data.editor, filename);
    RunInTerminal(inner);
}

/* Globalne "make" BEZ argumentu (nie per-plik) - zgodnie z Makefile w
 * katalogu uzytkownika, ktory sam decyduje, co kompiluje domyslny target
 * (patrz naglowek pliku) - apka nie zna i nie zaklada nic o jego regulach. */
static void
ActionCompile(void)
{
    RunInTerminal("make");
}

static void
ActionOpen(const char *name)
{
    char filename[NAME_LEN + 4];

    snprintf(filename, sizeof(filename), "%s.pdf", name);
    RunDirect(app_data.viewer, filename);
}

/* Usuwanie TYLKO pliku .mm, z potwierdzeniem przez "7amessage -confirm" -
 * ten sam wzorzec co g_cmds w utils/7aexit.c: "7amessage -confirm '...'
 * && rm -f '...'" jako JEDNA komenda powloki, odpalona fire-and-forget
 * (BEZ terminala - dialog potwierdzenia to wlasne okno X, nie TUI - i BEZ
 * czekania w rodzicu: 7amessage sam decyduje w swoim procesie, czy "rm"
 * po niej sie wykona, patrz "&&"). Efekt (plik znika z listy) widac dopiero
 * po recznym Reload/'r' - patrz naglowek pliku. */
static void
ActionDelete(const char *name)
{
    char filename[NAME_LEN + 4];
    char path[PATH_LEN];
    char msg[NAME_LEN + 40];
    char quoted_msg[NAME_LEN + 80];
    char quoted_path[PATH_LEN + 8];
    char shell_cmd[PATH_LEN + NAME_LEN + 200];
    char *argv[4];
    pid_t pid;

    snprintf(filename, sizeof(filename), "%s.mm", name);
    snprintf(path, sizeof(path), "%s/%s", app_data.dir, filename);
    snprintf(msg, sizeof(msg), "Delete %.100s?", filename);

    quoted_msg[0] = '\0';
    AppendShellQuoted(quoted_msg, sizeof(quoted_msg), msg);
    quoted_path[0] = '\0';
    AppendShellQuoted(quoted_path, sizeof(quoted_path), path);

    snprintf(shell_cmd, sizeof(shell_cmd), "7amessage -confirm %s && rm -f %s",
             quoted_msg, quoted_path);

    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = shell_cmd;
    argv[3] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
}

static void
ActionHelp(void)
{
    RunInTerminal("man groff_mm");
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                             */
/* -------------------------------------------------------------------- */

/* Wysokosc jednego boxa (naglowek kolumn + VISIBLE_ROWS wierszy danych),
 * ta sama formula co BoxHeightForRows w utils/7ashop.c. */
static int
BoxHeightForRows(int n_rows)
{
    return n_rows * ROW_H + (n_rows - 1) * BOX_GAP + 2 * BOX_BORDER_W + 2 * BOX_PAD_TB;
}

/* Jeden box z lista dokumentow na cala szerokosc okna (x=0/width=win_w,
 * margin_l/margin_r w style - jak DrawNewFileBox). Klik w wiersz ustawia
 * g_sel PRZED narysowaniem tego wiersza (ui_hit_test), wiec podswietlenie
 * i pasek przyciskow pod lista widza nowy wybor w TEJ SAMEJ klatce. */
static int
DrawDocBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y, const XColor *warn_color)
{
    UiBox *box;
    int max_scroll, i, mx, my;

    ui_mouse_state(ctx, &mx, &my, NULL);

    max_scroll = g_doc_count - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    box = ui_box_begin(ctx, "docs", 0, y, win_w, style);

    {
        UiRect hdr = ui_box_next_rect(box, ROW_H);
        UiRect arrow_r = { hdr.x + hdr.w - ARROW_W, hdr.y, ARROW_W, hdr.h };
        UiRect name_r = { hdr.x, hdr.y, hdr.w - TYPE_COL_W, hdr.h };
        UiRect type_r = { hdr.x + hdr.w - TYPE_COL_W, hdr.y, TYPE_COL_W - ARROW_W, hdr.h };

        ui_label(ctx, name_r, "Document");
        ui_label(ctx, type_r, "Type");
        if (g_scroll + VISIBLE_ROWS < g_doc_count)
            ui_label_centered(ctx, arrow_r, "v");
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = g_scroll + i;
        int name_w = row.w - TYPE_COL_W;
        UiRect name_r, type_r;

        if (name_w < 0) name_w = 0;
        name_r = (UiRect) { row.x + 4, row.y, name_w - 4, row.h };
        type_r = (UiRect) { row.x + name_w, row.y, TYPE_COL_W, row.h };

        if (i == 0)
            g_list_r = (UiRect) { row.x, row.y, row.w,
                                  VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap };

        if (g_doc_count == 0) {
            if (i == 0) ui_label(ctx, name_r, "No .mm/.pdf files");
        } else if (idx < g_doc_count) {
            const DocEntry *d = &g_docs[idx];
            int hover = mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h;
            /* .mm bez .pdf albo .pdf starszy od .mm - patrz naglowek pliku */
            int outdated = d->has_mm && (!d->has_pdf || d->pdf_mtime < d->mm_mtime);
            const char *type = d->has_mm && d->has_pdf ? ".mm .pdf" : d->has_mm ? ".mm" : ".pdf";

            if (ui_hit_test(ctx, row))
                g_sel = idx;
            if (idx == g_sel)
                ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
            else if (hover)
                ui_fill_round_rect(ctx, row, ui_theme_button_bg(ctx));
            ui_label_fg(ctx, name_r, d->name, outdated ? warn_color : ui_theme_fg(ctx));
            ui_label(ctx, type_r, type);
        }
    }

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "docs") + style->margin_b;
}

/* Przycisk od *x w prawo, przesuwa *x za niego - dla paska akcji. */
static int
BarButton(UiCtx *ctx, int *x, int y, const char *label)
{
    UiRect r = { *x, y, ui_button_width(ctx, label), ROW_H };

    *x += r.w + BTN_GAP;
    return ui_button(ctx, r, label);
}

/* Pasek pod lista: po lewej przyciski KONTEKSTOWE wybranego dokumentu
 * (.mm -> Edit/Compile/Delete, .pdf -> Open), po prawej zawsze Reload/Help,
 * miedzy nimi status. Kopia nazwy, bo Reload (ScanDirectory) w tej samej
 * klatce przebudowuje g_docs. */
static void
DrawActionBar(UiCtx *ctx, int win_w, int y)
{
    int wmargin = ui_window_margin(ctx);
    int x = wmargin, rx, status_w;
    int has_mm = 0, has_pdf = 0;
    char name[NAME_LEN] = "";

    if (g_sel >= 0 && g_sel < g_doc_count) {
        snprintf(name, sizeof(name), "%s", g_docs[g_sel].name);
        has_mm = g_docs[g_sel].has_mm;
        has_pdf = g_docs[g_sel].has_pdf;
    }

    if (has_mm) {
        if (BarButton(ctx, &x, y, "Edit")) ActionEdit(name);
        if (BarButton(ctx, &x, y, "Compile")) ActionCompile();
        if (BarButton(ctx, &x, y, "Delete")) ActionDelete(name);
    }
    if (has_pdf && BarButton(ctx, &x, y, "Open"))
        ActionOpen(name);

    rx = win_w - wmargin - ui_button_width(ctx, "Help") - BTN_GAP - ui_button_width(ctx, "Reload");
    status_w = rx - BTN_GAP - x;
    if (g_status[0] != '\0' && status_w > 0) {
        UiRect status_r = { x, y, status_w, ROW_H };

        ui_label(ctx, status_r, g_status);
    }
    if (BarButton(ctx, &rx, y, "Reload")) ScanDirectory();
    if (BarButton(ctx, &rx, y, "Help")) ActionHelp();
}

/* Box NA CALA SZEROKOSC okna (x=0/width=win_w, margin_l/margin_r wewnatrz
 * style robia reszte - ten sam wzorzec co box "header" w utils/7askm.c),
 * z jednym wierszem: label "New:" + pole tekstowe + przycisk "New". Osobny
 * box (nie wiersz WEWNATRZ listy, jak "New product:" w utils/7ashop.c) -
 * wyrazne zyczenie uzytkownika, zeby obszar dodawania nowego pliku byl
 * wizualnie odrebna sekcja, nie czescia listy dokumentow. */
static int
DrawNewFileBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    static int label_w = 0, btn_w = 0;
    UiBox *box;
    UiRect row, l, m, r;

    if (label_w == 0) {
        label_w = ui_text_width(ctx, "New:") + 4;
        btn_w = ui_button_width(ctx, "New");
    }

    box = ui_box_begin(ctx, "new_mm", 0, y, win_w, style);
    row = ui_box_next_rect(box, ROW_H);

    ui_rect_split3(row, label_w, btn_w, 6, &l, &m, &r);
    ui_label(ctx, l, "New:");
    ui_textbox(ctx, m, g_new_name, sizeof(g_new_name), &g_new_cursor);
    if (ui_button(ctx, r, "New")) {
        CreateNewMmFile(g_new_name);
        g_new_name[0] = '\0';
        g_new_cursor = 0;
    }

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "new_mm") + style->margin_b;
}

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static XColor warn_color;
    static int ready = 0;
    int y;

    if (!ready) {
        style.margin_t = style.margin_b = BOX_MARGIN_TB;
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = BOX_PAD_TB;
        style.border_w = BOX_BORDER_W;
        style.gap = BOX_GAP;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ui_color(ctx, "#b34700", &warn_color); /* PDF nieaktualny/brak - patrz naglowek pliku */
        ready = 1;
    }

    y = DrawNewFileBox(ctx, &style, win_w, 0);
    y = DrawDocBox(ctx, &style, win_w, y, &warn_color);
    DrawActionBar(ctx, win_w, y);

    (void) win_h;
}

static int
ComputeContentHeight(void)
{
    int new_box_h = BOX_MARGIN_TB + BoxHeightForRows(1) + BOX_MARGIN_TB;
    int list_h = BOX_MARGIN_TB + BoxHeightForRows(1 + VISIBLE_ROWS) + BOX_MARGIN_TB;

    return new_box_h + list_h + 6 + ROW_H;
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
    int win_w = 560, win_h;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw;
    char app_name[64] = "7aGroff";
    char app_title[64] = "";
    char dir_raw[512];
    XEvent ev;

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

    win_h = ComputeContentHeight();

    /* Dzieci (Edit/Compile/Open, patrz RunInTerminal) sa fire-and-forget -
     * jadro je sprzatnie samo, bez wait(), tak jak w utils/7atodo.c. */
    signal(SIGCHLD, SIG_IGN);

#ifdef __OpenBSD__
    /* rpath - katalog docelowy (opendir/stat) i baza zasobow X/fonty; proc
     * exec - fork+exec terminala/edytora/make/viewera; BEZ wpath/cpath, ta
     * apka niczego nie zapisuje (patrz tez brak sqlite w tym pliku). */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7agroff: brak polaczenia z X11\n");
        return 1;
    }

    ui_resource_string(dpy, "7aGroff.dir", "7aGroff.Dir", dir_raw, sizeof(dir_raw), "~/projects/groff");
    ExpandHome(dir_raw, app_data.dir, sizeof(app_data.dir));
    ui_resource_string(dpy, "7aGroff.editor", "7aGroff.Editor", app_data.editor, sizeof(app_data.editor), "nvim");
    ui_resource_string(dpy, "7aGroff.terminal", "7aGroff.Terminal", app_data.terminal, sizeof(app_data.terminal), "urxvt");
    ui_resource_string(dpy, "7aGroff.viewer", "7aGroff.Viewer", app_data.viewer, sizeof(app_data.viewer), "gv --watch");

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
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7aGroff";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    /* Szerokosc swobodna (nazwy plikow sa rozne dlugosci - wzorem
     * utils/7ashop.c), min_height = dokladna wysokosc zawartosci
     * (VISIBLE_ROWS to stala liczba wierszy), max_height duzy - okno moze
     * byc wyzsze (puste miejsce pod "Reload"), ale nigdy nizsze niz trzeba. */
    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = ComputeContentHeight();
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    if (geom_mask & (WidthValue | HeightValue)) {
        sizehints->flags |= USSize;
        sizehints->width = win_w;
        sizehints->height = win_h;
    }
    if (geom_mask & (XValue | YValue)) {
        sizehints->flags |= USPosition;
        sizehints->x = win_x;
        sizehints->y = win_y;
    }
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7agroff: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, description_48_width, description_48_height, description_48_bits);

    ScanDirectory();

    /* Dwie klatki PRZED petla glowna, zeby zapelnic cache wysokosci boxa
     * zanim cokolwiek jest widoczne - patrz komentarz w utils/7aping.c
     * (ten sam wzorzec) i architektura w CLAUDE.md. Bez tego boxy
     * pokazywalyby sie bez tla/bordera az do pierwszego X eventu. */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    running = 1;
    redraw = 1;

    while (running) {
        XNextEvent(dpy, &ev);

        /* Kolko myszy (Button4/5) przechwycone TU, PRZED ui_feed_event -
         * ten sam wzorzec co w utils/7askm.c/7amessage.c: ui.c nie
         * rozroznia numeru przycisku, wiec para ButtonPress/Release od
         * kolka zostalaby policzona jak zwykly klik (np. na Edit/Compile/
         * Open pod kursorem). g_list_r to obszar widocznych
         * wierszy z OSTATNIEJ narysowanej klatki. */
        if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
            (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
            /* Bez "continue" na koncu (w odroznieniu od 7askm.c, gdzie
             * ten sam wzorzec zyje w WEWNETRZNYM "while (XPending)" -
             * tam "continue" wraca do sprawdzenia kolejnego pendingowego
             * eventu, a blok redraw ponizej i tak wykonuje sie po
             * wyjsciu z tej petli). Tutaj jest tylko JEDNA petla z
             * blokujacym XNextEvent, wiec "continue" przeskakiwalby
             * prosto do kolejnego XNextEvent, omijajac blok redraw
             * ponizej w tej samej iteracji - scroll ustawial redraw=1,
             * ale okno nie bylo przerysowywane, dopoki nie nadszedl
             * inny event (np. MotionNotify przy ruchu myszka poza
             * boxem). */
            if (ev.type == ButtonPress) {
                int delta = (ev.xbutton.button == Button4) ? -1 : 1;
                int px = ev.xbutton.x, py = ev.xbutton.y;

                if (px >= g_list_r.x && px < g_list_r.x + g_list_r.w &&
                    py >= g_list_r.y && py < g_list_r.y + g_list_r.h) {
                    g_scroll += delta;
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
                char keybuf[16];
                KeySym ks;

                XLookupString(&ev.xkey, keybuf, sizeof(keybuf), &ks, NULL);
                if (ks == XK_Escape || ks == XK_q) running = 0;
                if (ks == XK_r) { ScanDirectory(); redraw = 1; }
                break;
            }
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
            draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            redraw = 0;
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
