/*
 * 7atodo.c - port oryginalnej apki z ../7atodo (Xt/Xaw, wlasny widget
 * TodoList) na biblioteke ui.c/ui.h z tego katalogu - ten sam wzorzec
 * portowania co examples/7aweather.c, examples/7asensors.c i
 * examples/7acal.c (patrz tam obszerniejszy komentarz o roznicach
 * wzgledem Xt/Shell). Ta sama baza SQLite co 7acal (~/.7a/tasks.db).
 *
 * Logika bazy/edycji (OpenDatabase, MigrateOldFiles, RunQuery,
 * GetItemText, SpawnCommand + --import) jest przeniesiona z oryginalu
 * prawie bez zmian - to funkcje na sqlite3/char*, niezalezne od
 * toolkitu. Tresc jest edytowana w ZEWNETRZNYM edytorze (fire-and-forget,
 * mkstemp + "&& 7atodo --import ID PLIK"), tak jak w oryginale - ui.c
 * dostarcza wlasny ui_textbox (z pelnym UTF-8, patrz ui.h), ale tutaj nie
 * jest potrzebny, bo edycja tresci i tak nie dzieje sie w tym oknie.
 *
 * Priority dropdown: oryginalny TodoList.c NIE uzywa prawdziwego popupu
 * (SimpleMenu) - autor probowal i to wieszalo caly serwer X (patrz
 * obszerny komentarz przy SelectAction w ../7atodo/TodoList.c) - zamiast
 * tego to zwykly stan (ktory wiersz ma dropdown "otwarty") rysowany NA
 * WIERZCHU reszty w tym samym oknie, hit-testowany recznie. To dokladnie
 * pasuje do modelu immediate-mode tej biblioteki (zaden osobny widget/
 * grab/okno), wiec port jest tu prostszy niz oryginal, nie bardziej
 * skomplikowany.
 */

#define _DEFAULT_SOURCE  /* popen/execvp/fork/mkstemp sa POSIX - patrz ta sama uwaga w examples/7aweather.c */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20
#define MENU_WIDTH 70        /* szerokosc inline dropdownu priorytetu */
#define TEXT_GAP 4           /* odstep miedzy strzalka priorytetu a tekstem wiersza */
#define MAX_CMD_TOKENS 24
#define MAX_VISIBLE_ROWS 128 /* gorny limit wierszy/strone - patrz draw() */

static char app_dir[1024];   /* ~/.7a */
static char tmp_dir[1200];   /* ~/.7a/tmp - pliki tymczasowe edycji */
static char db_path[1200];   /* ~/.7a/tasks.db */
static sqlite3 *db;
static char *self_path;      /* argv[0], do ponownego odpalenia w --import */
static char filter_date[16]; /* pusty = widok domyslny; "YYYY-MM-DD" = --date */
static char app_name[64] = "7aTodo"; /* nadpisywalne przez -name, uzywa WM_CLASS/tytulu okna */

static sqlite3_int64 *g_item_ids = NULL;
static int g_item_count = 0;
static int g_item_cap = 0;

static int g_selected_index = -1;
static int g_page = 0;
static int g_menu_row_index = -1;  /* -1 = zaden dropdown priorytetu nie jest otwarty */

typedef struct {
    char editor[128];    /* np. "nvim", "emacsclient -c" */
    char terminal[128];  /* np. "urxvt"; pusty = edytor bez terminala */
    char row_bg[64];     /* tlo zwyklego wiersza listy - nazwa koloru X11 lub #rrggbb */
    char select_bg[64];  /* tlo zaznaczonego wiersza listy - nazwa koloru X11 lub #rrggbb */
    char prio_high_bg[64]; /* tlo CALEGO wiersza z priorytetem "High" - patrz draw() */
} AppData;

static AppData app_data;

/* -------------------------------------------------------------------- */
/* Zasoby X (editor/terminal/viewer) - czytane bezposrednio przez Xrm,   */
/* bo to konfiguracja specyficzna dla TEJ apki, nie ogolny motyw ui.c    */
/* (patrz ui_theme_* w ui.h - to tylko background/foreground/...).       */
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

/* -------------------------------------------------------------------- */
/* Baza danych - OpenDatabase/MigrateOldFiles/ItemsTableEmpty/           */
/* ReadWholeFile bez zmian wzgledem ../7atodo/7atodo.c                   */
/* -------------------------------------------------------------------- */

static char *
ReadWholeFile(const char *path)
{
    FILE *fp = fopen(path, "r");
    char *buf = NULL;
    size_t cap = 0, len = 0;

    if (!fp)
        return NULL;
    for (;;) {
        size_t n;

        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            buf = realloc(buf, cap);
        }
        n = fread(buf + len, 1, 4096, fp);
        len += n;
        if (n < 4096)
            break;
    }
    fclose(fp);
    if (!buf)
        buf = malloc(1);
    buf[len] = '\0';
    return buf;
}

static int
ItemsTableEmpty(void)
{
    sqlite3_stmt *stmt;
    int count = 1;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM items;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count == 0;
}

static void
MigrateOldFiles(void)
{
    const char *home = getenv("HOME");
    char dirpath[1200];
    DIR *d;
    struct dirent *de;
    sqlite3_stmt *stmt;

    if (!home)
        return;

    snprintf(dirpath, sizeof(dirpath), "%s/.7atodo", home);
    d = opendir(dirpath);
    if (d) {
        if (sqlite3_prepare_v2(db,
                "INSERT INTO items(priority, due_date, body, created_at)"
                " VALUES (2, NULL, ?1, ?2);", -1, &stmt, NULL) == SQLITE_OK) {
            while ((de = readdir(d)) != NULL) {
                char *dot = strstr(de->d_name, ".txt");
                char *endptr;
                long epoch;
                char path[1600];
                char *body;

                if (!dot || dot[4] != '\0' || dot == de->d_name)
                    continue;
                epoch = strtol(de->d_name, &endptr, 10);
                if (endptr != dot)
                    continue;

                snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
                body = ReadWholeFile(path);
                if (!body)
                    continue;

                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, body, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, (sqlite3_int64) epoch);
                sqlite3_step(stmt);
                free(body);
            }
            sqlite3_finalize(stmt);
        }
        closedir(d);
    }

    snprintf(dirpath, sizeof(dirpath), "%s/.7acal", home);
    d = opendir(dirpath);
    if (d) {
        if (sqlite3_prepare_v2(db,
                "INSERT INTO items(priority, due_date, body, created_at)"
                " VALUES (2, ?1, ?2, ?3);", -1, &stmt, NULL) == SQLITE_OK) {
            while ((de = readdir(d)) != NULL) {
                char path[1600];
                char datebuf[11];
                struct stat st;
                char *body;
                int y, m, dd;

                if (strlen(de->d_name) != 14 ||
                    sscanf(de->d_name, "%4d-%2d-%2d.txt", &y, &m, &dd) != 3)
                    continue;

                snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
                if (stat(path, &st) != 0 || st.st_size == 0)
                    continue;

                body = ReadWholeFile(path);
                if (!body)
                    continue;

                snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", y, m, dd);
                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, datebuf, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, body, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, (sqlite3_int64) st.st_mtime);
                sqlite3_step(stmt);
                free(body);
            }
            sqlite3_finalize(stmt);
        }
        closedir(d);
    }
}

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char *errmsg = NULL;

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", app_dir);
    mkdir(tmp_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/tasks.db", app_dir);

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "7atodo: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        exit(1);
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS items ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " priority INTEGER NOT NULL DEFAULT 2,"
            " due_date TEXT,"
            " body TEXT NOT NULL DEFAULT '',"
            " created_at INTEGER NOT NULL,"
            " alarm BOOLEAN NOT NULL DEFAULT 0"
            ");", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "7atodo: schema: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        exit(1);
    }
    /* Instalacje sprzed dodania kolumny alarm maja juz tabele items bez
     * niej - CREATE TABLE IF NOT EXISTS wyzej nic wtedy nie zmienia, wiec
     * dogrywamy kolumne przez ALTER TABLE. Blad "duplicate column" (gdy
     * kolumna juz istnieje) jest oczekiwany i celowo ignorowany. Ani
     * 7atodo, ani 7acal z tego pola nie korzystaja - jest tu tylko pod
     * przyszle uzycie. */
    sqlite3_exec(db, "ALTER TABLE items ADD COLUMN alarm BOOLEAN NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_items_due_date ON items(due_date);",
        NULL, NULL, NULL);

    if (ItemsTableEmpty())
        MigrateOldFiles();
}

/* -------------------------------------------------------------------- */
/* Zapytania / dane wiersza - RunQuery/GetItemText bez zmian, GetItemColor */
/* zwraca teraz indeks (0/1/2) zamiast wypelniac Pixel* przez wskaznik.  */
/* -------------------------------------------------------------------- */

static void
RunQuery(void)
{
    sqlite3_stmt *stmt = NULL;

    g_item_count = 0;

    if (filter_date[0]) {
        if (sqlite3_prepare_v2(db,
                "SELECT id FROM items WHERE due_date = ?1"
                " ORDER BY priority ASC, created_at ASC;",
                -1, &stmt, NULL) == SQLITE_OK)
            sqlite3_bind_text(stmt, 1, filter_date, -1, SQLITE_STATIC);
    } else {
        sqlite3_prepare_v2(db,
            "SELECT id FROM items WHERE due_date IS NULL"
            " OR due_date = date('now','localtime')"
            " ORDER BY priority ASC, created_at ASC;",
            -1, &stmt, NULL);
    }
    if (!stmt)
        return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (g_item_count >= g_item_cap) {
            g_item_cap = g_item_cap ? g_item_cap * 2 : 16;
            g_item_ids = realloc(g_item_ids, (size_t) g_item_cap * sizeof(sqlite3_int64));
        }
        g_item_ids[g_item_count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
}

/* Jak RunQuery(), ale probuje zachowac zaznaczenie zamiast bezwarunkowo
 * je czyscic - RunQuery() moze przetasowac kolejnosc (sortowanie po
 * priority), wiec szukamy PO ID zaznaczonej pozycji, nie po starym
 * indeksie. Uzywane przy powrocie fokusu/kursora do okna (patrz
 * FocusIn/EnterNotify w main()) - dane maja byc swieze (np. po edycji w
 * zewnetrznym edytorze), ale sama nawigacja mysza (odejscie i powrot bez
 * realnej zmiany czegokolwiek) nie powinna gubic tego, co uzytkownik
 * mial akurat zaznaczone. Jesli zaznaczona pozycja faktycznie zniknela
 * (np. skasowana przez zapisanie pustego pliku w edytorze), zaznaczenie
 * i tak spada do -1, jak poprzednio. */
static void
RefreshKeepingSelection(void)
{
    sqlite3_int64 prev_id = (g_selected_index >= 0 && g_selected_index < g_item_count)
                                 ? g_item_ids[g_selected_index] : -1;
    int i;

    RunQuery();

    g_selected_index = -1;
    if (prev_id >= 0) {
        for (i = 0; i < g_item_count; i++) {
            if (g_item_ids[i] == prev_id) {
                g_selected_index = i;
                break;
            }
        }
    }
    if (g_menu_row_index >= g_item_count)
        g_menu_row_index = -1;
}

static int
GetItemText(int index, char *buf, int bufsize)
{
    sqlite3_stmt *stmt;
    int ok = 0;

    if (index < 0 || index >= g_item_count)
        return 0;
    if (sqlite3_prepare_v2(db, "SELECT due_date, body FROM items WHERE id=?1;",
                            -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, g_item_ids[index]);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *due = sqlite3_column_text(stmt, 0);
        const unsigned char *body = sqlite3_column_text(stmt, 1);
        char first_line[256];
        size_t flen;
        int n = 0;

        if (body) {
            const char *nl = strchr((const char *) body, '\n');
            flen = nl ? (size_t) (nl - (const char *) body) : strlen((const char *) body);
            if (flen >= sizeof(first_line))
                flen = sizeof(first_line) - 1;
            memcpy(first_line, body, flen);
            first_line[flen] = '\0';
        } else {
            first_line[0] = '\0';
        }

        if (due)
            n = snprintf(buf, (size_t) bufsize, "%.5s ", (const char *) due + 5);
        if (n < 0)
            n = 0;
        if (n > bufsize)
            n = bufsize;
        snprintf(buf + n, (size_t) (bufsize - n), "%s",
                 first_line[0] ? first_line : "(untitled)");
        ok = 1;
    }
    sqlite3_finalize(stmt);
    return ok;
}

/* 0 = normalny (tlo wiersza domyslne), 1 = wysoki priorytet (tlo
 * prio_high_bg) - patrz draw(). */
static int
GetItemColorIdx(int index)
{
    sqlite3_stmt *stmt;
    int result = 0;

    if (index < 0 || index >= g_item_count)
        return 0;
    if (sqlite3_prepare_v2(db, "SELECT priority FROM items WHERE id=?1;",
                            -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, g_item_ids[index]);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int p = sqlite3_column_int(stmt, 0);

        if (p <= 1) result = 1;
    }
    sqlite3_finalize(stmt);
    return result;
}

/* -------------------------------------------------------------------- */
/* Tryb "--import ID PLIK" - patrz naglowek pliku                       */
/* -------------------------------------------------------------------- */

static int
IsBlank(const char *s)
{
    for (; *s; s++)
        if (!isspace((unsigned char) *s))
            return 0;
    return 1;
}

static int
ImportBody(sqlite3_int64 id, const char *path)
{
    char *body = ReadWholeFile(path);
    sqlite3_stmt *stmt;
    int ok = 0;

    if (!body)
        return 0;

    if (IsBlank(body)) {
        if (sqlite3_prepare_v2(db, "DELETE FROM items WHERE id=?1;",
                                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, id);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
    } else if (sqlite3_prepare_v2(db, "UPDATE items SET body=?1 WHERE id=?2;",
                                   -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    free(body);
    return ok;
}

/* -------------------------------------------------------------------- */
/* Ikona okna - rysowana wprost Xlibem na 1-bitowej Pixmapie (jak w      */
/* 7aweather.c/7asensors.c/7acal.c).                                     */
/* -------------------------------------------------------------------- */

static void
DrawListIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    int row;

    XDrawRectangle(idpy, p, gc, 2, 2, 27, 27);
    for (row = 0; row < 3; row++) {
        int y = 7 + row * 8;

        XDrawRectangle(idpy, p, gc, 5, y, 5, 5);
        if (row != 2)
            XFillRectangle(idpy, p, gc, 6, y + 1, 3, 3);
        XFillRectangle(idpy, p, gc, 13, y + 1, 13, 2);
    }
}

static Pixmap
MakeListIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawListIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Uruchamianie edytora/viewera + zapis z powrotem do bazy - bez zmian   */
/* wzgledem ../7atodo/7atodo.c (AppendTokens/AppendShellQuoted/          */
/* SpawnCommand sa czystym C, niezaleznym od toolkitu).                  */
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

static void
SpawnCommand(sqlite3_int64 id, const char *initial_body, const char *cmd, int do_import)
{
    char tmp_path[1300];
    int fd;
    FILE *fp;
    char cmd_buf[256];
    char terminal_buf[256];
    char *inner_argv[MAX_CMD_TOKENS];
    int inner_argc = 0;
    static char shell_cmd[4096];
    char id_str[32];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;
    int i;

    snprintf(tmp_path, sizeof(tmp_path), "%s/7aXXXXXX", tmp_dir);
    fd = mkstemp(tmp_path);
    if (fd < 0)
        return;
    fp = fdopen(fd, "w");
    if (fp) {
        fputs(initial_body, fp);
        fclose(fp);
    } else {
        close(fd);
    }

    snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
    snprintf(terminal_buf, sizeof(terminal_buf), "%s", app_data.terminal);

    AppendTokens(inner_argv, &inner_argc, MAX_CMD_TOKENS, cmd_buf);

    shell_cmd[0] = '\0';
    for (i = 0; i < inner_argc; i++) {
        if (i > 0)
            strncat(shell_cmd, " ", sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        AppendShellQuoted(shell_cmd, sizeof(shell_cmd), inner_argv[i]);
    }
    strncat(shell_cmd, " ", sizeof(shell_cmd) - strlen(shell_cmd) - 1);
    AppendShellQuoted(shell_cmd, sizeof(shell_cmd), tmp_path);

    if (do_import) {
        strncat(shell_cmd, " && ", sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        AppendShellQuoted(shell_cmd, sizeof(shell_cmd), self_path);
        strncat(shell_cmd, " --import ", sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        snprintf(id_str, sizeof(id_str), "%lld", (long long) id);
        strncat(shell_cmd, id_str, sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        strncat(shell_cmd, " ", sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        AppendShellQuoted(shell_cmd, sizeof(shell_cmd), tmp_path);
    }

    /* Jesli komenda edytora/przegladarki/importu sie nie powiedzie (np. zle
     * ustawiony 7aTodo.editor/7aTodo.viewer w Xresources, brak binarki),
     * terminal domyslnie zamyka sie natychmiast razem z "sh -c" - blad
     * przemyka bez szans na przeczytanie. Trzymamy wtedy terminal otwarty
     * do Enter, zeby uzytkownik zobaczyl komunikat powloki. */
    {
        char wrapped[4096];
        snprintf(wrapped, sizeof(wrapped),
                 "( %s ) || { echo; echo '[7aTodo] polecenie nie powiodlo sie"
                 " - sprawdz 7aTodo.editor/7aTodo.viewer/7aTodo.terminal'; "
                 "read -r _; }", shell_cmd);
        snprintf(shell_cmd, sizeof(shell_cmd), "%s", wrapped);
    }

    strncat(shell_cmd, " ; rm -f ", sizeof(shell_cmd) - strlen(shell_cmd) - 1);
    AppendShellQuoted(shell_cmd, sizeof(shell_cmd), tmp_path);

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

static void
SpawnEditor(sqlite3_int64 id, const char *initial_body)
{
    SpawnCommand(id, initial_body, app_data.editor, 1);
}

/* -------------------------------------------------------------------- */
/* Akcje - AddCallback/EditCallback/DeleteCallback/PrioritySelectCallback */
/* z oryginalu jako zwykle funkcje, wywolywane z klikniec w draw().      */
/* -------------------------------------------------------------------- */

static char *
FetchSelectedBody(void)
{
    sqlite3_stmt *stmt;
    char *body = NULL;

    if (g_selected_index < 0 || g_selected_index >= g_item_count)
        return NULL;

    if (sqlite3_prepare_v2(db, "SELECT body FROM items WHERE id=?1;",
                            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, g_item_ids[g_selected_index]);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *b = sqlite3_column_text(stmt, 0);
            const char *src = b ? (const char *) b : "";
            size_t len = strlen(src) + 1;

            body = malloc(len);
            if (body)
                snprintf(body, len, "%s", src);
        }
        sqlite3_finalize(stmt);
    }
    return body;
}

static void
EditSelected(void)
{
    char *body;

    if (g_selected_index < 0 || g_selected_index >= g_item_count)
        return;
    body = FetchSelectedBody();
    SpawnEditor(g_item_ids[g_selected_index], body ? body : "");
    free(body);
}

static void
DeleteSelected(void)
{
    sqlite3_stmt *stmt;

    if (g_selected_index < 0 || g_selected_index >= g_item_count)
        return;

    if (sqlite3_prepare_v2(db, "DELETE FROM items WHERE id=?1;",
                            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, g_item_ids[g_selected_index]);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    g_selected_index = -1;
    RunQuery();
}

static void
AddNewItem(int items_per_page)
{
    sqlite3_stmt *stmt;
    sqlite3_int64 id;
    int i;

    if (sqlite3_prepare_v2(db,
            "INSERT INTO items(priority, due_date, body, created_at)"
            " VALUES (2, ?1, '', ?2);", -1, &stmt, NULL) != SQLITE_OK)
        return;
    if (filter_date[0])
        sqlite3_bind_text(stmt, 1, filter_date, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 1);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64) time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    id = sqlite3_last_insert_rowid(db);

    RunQuery();

    /* nowa pozycja moze wyladowac w srodku listy (sortowanie po priority),
     * nie zawsze na koncu - szukamy jej faktycznego indeksu */
    for (i = 0; i < g_item_count && g_item_ids[i] != id; i++)
        ;
    g_page = (g_item_count > 0 && items_per_page > 0) ? i / items_per_page : 0;

    SpawnEditor(id, "");
}

static void
ApplyPriority(int index, int priority)
{
    sqlite3_stmt *stmt;

    if (index < 0 || index >= g_item_count)
        return;

    if (sqlite3_prepare_v2(db, "UPDATE items SET priority=?1 WHERE id=?2;",
                            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, priority);
        sqlite3_bind_int64(stmt, 2, g_item_ids[index]);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* kolejnosc sortowania (po priority) mogla sie zmienic */
    RunQuery();
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static XColor row_bg, select_bg, prio_high_fg, prio_high_bg;
    static int ready = 0;
    int y = 0;
    int i;
    int items_per_page, total_pages;
    int menu_open_at_start;
    int any_click;
    int pending_prio_index = -1, pending_prio_value = 0;
    UiRect menu_row_rect = {0, 0, 0, 0};
    int have_menu_row_rect = 0;
    int mx, my;
    char title[40];

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 6;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = 4;
        style.border_w = 1;
        style.gap = 2;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);

        ui_color(ctx, app_data.row_bg, &row_bg);
        ui_color(ctx, app_data.select_bg, &select_bg);
        ui_color(ctx, app_data.prio_high_bg, &prio_high_bg);
        /* kolor tekstu wpisu "High" w rozwinietym dropdownie - osobny od
         * tla wiersza (prio_high_bg powyzej, konfigurowalnego przez zasoby
         * X), bo dropdown rysuje sie na wlasnym, neutralnym tle
         * (ui_theme_box_bg), nie na tle wiersza. */
        ui_color(ctx, "red", &prio_high_fg);
        ready = 1;
    }

    /* ile wierszy zmiesci sie na stronie - policzone na nowo z biezacej
     * wysokosci okna (tak jak ComputeGeometry w oryginale), wiec resize
     * okna od razu zmienia stronicowanie, bez zadnej dodatkowej logiki. */
    {
        int header_footprint = style.margin_t + (2 * style.border_w + 2 * style.padding_t + ROW_H) + style.margin_b;
        int footer_h = style.margin_t + ROW_H + 10;
        int avail = win_h - header_footprint - 2 * style.border_w - 2 * style.padding_t - footer_h;

        items_per_page = (avail + style.gap) / (ROW_H + style.gap);
        if (items_per_page < 1) items_per_page = 1;
    }

    total_pages = (g_item_count + items_per_page - 1) / items_per_page;
    if (total_pages < 1) total_pages = 1;
    if (g_page >= total_pages) g_page = total_pages - 1;
    if (g_page < 0) g_page = 0;

    menu_open_at_start = (g_menu_row_index >= 0);
    if (menu_open_at_start) {
        int row_in_page = g_menu_row_index - g_page * items_per_page;

        if (row_in_page < 0 || row_in_page >= items_per_page || g_menu_row_index >= g_item_count) {
            g_menu_row_index = -1;
            menu_open_at_start = 0;
        }
    }

    /* "czy w tej klatce w ogole doszlo do klikniecia" - okno pokrywa caly
     * obszar, w ktorym klik moglby wystapic, wiec to dziala jako ogolny
     * "any click" bez wlasnego pola w UiCtx (patrz ui_hit_test w ui.h) -
     * potrzebne do "klik gdziekolwiek poza wpisami dropdownu go zamyka"
     * (DismissPriorityMenu w oryginale), niezaleznie w KTOREJ czesci okna
     * (inny wiersz, strzalka, puste miejsce) klik faktycznie wyladowal. */
    any_click = ui_hit_test(ctx, (UiRect){ 0, 0, win_w, win_h });

    /* header: strzalki </> (zmiana strony) + "(N) strona/stron" */
    UiBox *header = ui_box_begin(ctx, "header", 0, y, win_w, &style);
    UiRect hrow = ui_box_next_rect(header, ROW_H);
    UiRect prev_r, mid_r, next_r;

    ui_rect_split3(hrow, ROW_H, ROW_H, 6, &prev_r, &mid_r, &next_r);
    {
        int prev_clicked = ui_button(ctx, prev_r, "<");
        int next_clicked = ui_button(ctx, next_r, ">");

        if (!menu_open_at_start) {
            if (prev_clicked && g_page > 0) g_page--;
            else if (next_clicked) g_page++;
            if (g_page >= total_pages) g_page = total_pages - 1;
            if (g_page < 0) g_page = 0;
        }
    }
    snprintf(title, sizeof(title), "(%d) %d/%d", g_item_count, g_page + 1, total_pages);
    ui_label_centered(ctx, mid_r, title);
    ui_box_end(header);
    y += style.margin_t + ui_box_height(ctx, "header") + style.margin_b;

    /* content: wiersze - checkbox zaznaczenia + strzalka priorytetu
     * (otwiera dropdown) + tekst.
     * Klikniecia dla CALEJ strony sa rozstrzygniete w PIERWSZEJ petli,
     * zanim jakikolwiek wiersz zostanie narysowany - dopiero w DRUGIEJ
     * petli rysujemy, na podstawie juz w pelni ustalonego g_selected_index/
     * g_menu_row_index. Rect kazdego wiersza jest zapamietywany w
     * row_rects[], zeby druga petla nie musiala drugi raz wolac
     * ui_box_next_rect (ten ma efekt uboczny - przesuwa kursor boxa, wiec
     * wywolanie go dwa razy na wiersz zepsuloby wysokosc/uklad boxa).
     *
     * Bez tego rozdzielenia (pojedyncza petla, w ktorej klik na wierszu N
     * aktualizowal g_selected_index W TRAKCIE tej samej petli) wiersze
     * narysowane PRZED wierszem N w tej samej klatce (np. N-1, jesli to on
     * byl zaznaczony poprzednio) wciaz uzywaly STAREJ wartosci
     * g_selected_index w momencie swojego rysowania - wiec klikniecie
     * kolejnego itemu potrafilo na jedna klatke pokazac OBA wiersze jako
     * zaznaczone naraz, a korekta (znikniecie starego zaznaczenia) byla
     * widoczna dopiero przy nastepnym zdarzeniu (np. ruchu myszy). */
    UiBox *content = ui_box_begin(ctx, "content", 0, y - style.margin_t, win_w, &style);
    UiRect row_rects[MAX_VISIBLE_ROWS];

    if (items_per_page > MAX_VISIBLE_ROWS) items_per_page = MAX_VISIBLE_ROWS;

    for (i = 0; i < items_per_page; i++) {
        int index = g_page * items_per_page + i;
        UiRect row = ui_box_next_rect(content, ROW_H);
        UiRect caret_r, text_r;
        int is_menu_row_at_start;

        row_rects[i] = row;

        if (index >= g_item_count) {
            /* pusty wiersz - tylko domyka box do stalej wysokosci
             * items_per_page (ui_box_next_rect powyzej juz doliczyl go
             * do wysokosci boxa), zeby ostatnia, czesciowo zapelniona
             * strona nie mial nizszego boxa niz strony pelne. */
            continue;
        }

        caret_r = (UiRect){ row.x, row.y, ROW_H, row.h };
        text_r = (UiRect){ row.x + ROW_H + TEXT_GAP, row.y, row.w - ROW_H - TEXT_GAP, row.h };

        is_menu_row_at_start = menu_open_at_start && (index == g_menu_row_index);

        if (!menu_open_at_start) {
            if (ui_hit_test(ctx, caret_r)) {
                g_selected_index = index;
                g_menu_row_index = index;
            } else if (ui_hit_test(ctx, text_r)) {
                g_selected_index = (g_selected_index == index) ? -1 : index;
            }
        } else if (is_menu_row_at_start) {
            /* dropdown priorytetu byl otwarty na TYM wierszu - hit-test
             * przeciw jego wpisom, pozycjonowanym wzgledem "row" w
             * TEJ klatce (wiec zawsze zgodnie z tym, co faktycznie
             * narysujemy po petli). */
            UiRect menu_r;
            int avail = row.w;
            int picked = -1;
            int j;

            menu_r.x = row.x;
            menu_r.y = row.y;
            if (avail < 0) avail = 0;
            menu_r.w = (avail < MENU_WIDTH) ? avail : MENU_WIDTH;
            menu_r.h = 2 * ROW_H;

            for (j = 0; j < 2; j++) {
                UiRect entry_r = { menu_r.x, menu_r.y + j * ROW_H, menu_r.w, ROW_H };

                if (ui_hit_test(ctx, entry_r))
                    picked = j;
            }

            if (picked >= 0) {
                /* ApplyPriority() wola RunQuery(), ktory przebudowuje
                 * g_item_ids "pod nami" - odlozone na PO tej petli, zeby
                 * nie psuc iteracji po liscie, ktora wlasnie przegladamy. */
                pending_prio_index = index;
                pending_prio_value = picked + 1;
                g_menu_row_index = -1;
            } else if (any_click) {
                g_menu_row_index = -1;
            }
        }
    }

    if (pending_prio_index >= 0) {
        ApplyPriority(pending_prio_index, pending_prio_value);
        g_selected_index = -1;
    }

    /* druga petla - czyste rysowanie, g_selected_index/g_menu_row_index sa
     * juz ostatecznie ustalone dla calej strony, wiec kazdy wiersz (w tym
     * te narysowane wczesniej niz ten, na ktorym faktycznie kliknieto)
     * pokazuje ten sam, biezacy stan. */
    ui_mouse_state(ctx, &mx, &my, NULL);
    for (i = 0; i < items_per_page; i++) {
        int index = g_page * items_per_page + i;
        UiRect row = row_rects[i];
        UiRect caret_r, text_r;
        char buf[256];
        int have_item;
        int color_idx;
        const XColor *rowbg;

        if (index >= g_item_count)
            continue;

        caret_r = (UiRect){ row.x, row.y, ROW_H, row.h };
        text_r = (UiRect){ row.x + ROW_H + TEXT_GAP, row.y, row.w - ROW_H - TEXT_GAP, row.h };

        if (g_menu_row_index == index) {
            menu_row_rect = row;
            have_menu_row_rect = 1;
        }

        have_item = GetItemText(index, buf, (int) sizeof(buf));
        if (!have_item) buf[0] = '\0';

        color_idx = GetItemColorIdx(index);
        rowbg = (index == g_selected_index) ? &select_bg
              : (mx >= row.x && mx < row.x + row.w &&
                 my >= row.y && my < row.y + row.h) ? ui_theme_accent(ctx)
              : (color_idx == 1) ? &prio_high_bg
              : &row_bg;
        ui_fill_rect(ctx, row, rowbg);

        /* strzalka w dol zamiast dawnego wypelnionego kwadratu - priorytet
         * jest juz widoczny po tle CALEGO wiersza (rowbg wyzej), wiec ten
         * ksztalt sluzy tylko jako podpowiedz "klik tu otwiera dropdown
         * priorytetu", stad neutralny kolor linii zamiast koloru
         * priorytetu. */
        {
            int cx = caret_r.x + caret_r.w / 2;
            int cy = caret_r.y + caret_r.h / 2;
            int s = ROW_H / 3;

            ui_fill_triangle(ctx, cx - s, cy - s / 2, cx + s, cy - s / 2, cx, cy + s / 2,
                              ui_theme_line_fg(ctx));
        }

        ui_label(ctx, text_r, buf);
    }
    ui_box_end(content);
    y += ui_box_height(ctx, "content") + style.margin_b;

    /* dropdown priorytetu - narysowany NA WIERZCHU wszystkiego powyzej, na
     * podstawie AKTUALNEGO g_menu_row_index (mogl sie wlasnie zmienic w
     * petli wyzej - nowo otwarty/zamkniety) - dzieki temu otwarcie i
     * zamkniecie tez widac od razu w tej samej klatce. */
    if (g_menu_row_index >= 0 && have_menu_row_rect) {
        static const char *const labels[2] = { "High", "Normal" };
        UiRect menu_r;
        int avail = menu_row_rect.w;
        int j;

        menu_r.x = menu_row_rect.x;
        menu_r.y = menu_row_rect.y;
        if (avail < 0) avail = 0;
        menu_r.w = (avail < MENU_WIDTH) ? avail : MENU_WIDTH;
        menu_r.h = 2 * ROW_H;

        ui_fill_rect(ctx, menu_r, ui_theme_box_bg(ctx));
        ui_draw_border(ctx, menu_r, 1, ui_theme_line_fg(ctx));

        for (j = 0; j < 2; j++) {
            UiRect entry_r = { menu_r.x, menu_r.y + j * ROW_H, menu_r.w, ROW_H };
            const XColor *efg = (j == 0) ? &prio_high_fg : ui_theme_fg(ctx);

            if (j > 0)
                ui_draw_line(ctx, menu_r.x, entry_r.y, menu_r.x + menu_r.w, entry_r.y, 1, ui_theme_line_fg(ctx));
            ui_label_fg(ctx, entry_r, labels[j], efg);
        }
    }

    /* Add / Edit / Del */
    {
        int gap = 6;
        int add_w  = ui_button_width(ctx, "Add");
        int edit_w = ui_button_width(ctx, "Edit");
        int del_w  = ui_button_width(ctx, "Del");
        UiRect add_r  = { style.margin_l, y, add_w, ROW_H };
        UiRect edit_r = { add_r.x + add_w + gap, y, edit_w, ROW_H };
        UiRect del_r  = { edit_r.x + edit_w + gap, y, del_w, ROW_H };
        int add_clicked = ui_button(ctx, add_r, "Add");
        int edit_clicked = ui_button(ctx, edit_r, "Edit");
        int del_clicked = ui_button(ctx, del_r, "Del");

        if (!menu_open_at_start) {
            if (add_clicked) AddNewItem(items_per_page);
            else if (edit_clicked) EditSelected();
            else if (del_clicked) DeleteSelected();
        }
    }

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
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 300, win_h = 262;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    XEvent ev;

    self_path = argv[0];

    /* Ukryty tryb "--import ID PLIK" - bez X, szybki, bezokienny. */
    if (argc >= 4 && strcmp(argv[1], "--import") == 0) {
        OpenDatabase();
        return ImportBody((sqlite3_int64) strtoll(argv[2], NULL, 10), argv[3]) ? 0 : 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            snprintf(filter_date, sizeof(filter_date), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[i + 1]);
            i++;
        } else if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
                   && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        }
    }

    signal(SIGCHLD, SIG_IGN);
    OpenDatabase();
    RunQuery();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    /* Bez tego XrmGetResource w ReadAppString nizej potrafi zwrocic
     * poprawna wartosc, ale z type == NULL (zaobserwowane na OpenBSD) -
     * warunek "type && strcmp(type, "String") == 0" wtedy zawsze zawodzi
     * i kazdy zasob 7aTodo.* cicho spada na wartosc domyslna. ui_init
     * (ui.c) wywoluje XrmInitialize() tez, ale dopiero PO tych
     * ReadAppString ponizej, wiec nie ratuje to sytuacji. */
    XrmInitialize();

    ReadAppString(dpy, "7aTodo.editor", "7aTodo.Editor", app_data.editor, sizeof(app_data.editor), "nvim");
    ReadAppString(dpy, "7aTodo.terminal", "7aTodo.Terminal", app_data.terminal, sizeof(app_data.terminal), "urxvt");
    ReadAppString(dpy, "7aTodo.rowBackground", "7aTodo.RowBackground", app_data.row_bg, sizeof(app_data.row_bg), "white");
    ReadAppString(dpy, "7aTodo.selectBackground", "7aTodo.SelectBackground", app_data.select_bg, sizeof(app_data.select_bg), "gray70");
    ReadAppString(dpy, "7aTodo.priorityHighBackground", "7aTodo.PriorityHighBackground", app_data.prio_high_bg, sizeof(app_data.prio_high_bg), "#f6cccc");

    if (getenv("TODO7A_DEBUG")) {
        char *rms_dbg = XResourceManagerString(dpy);
        fprintf(stderr, "[7aTodo debug] editor='%s' terminal='%s' rowBackground='%s' selectBackground='%s'\n",
                app_data.editor, app_data.terminal, app_data.row_bg, app_data.select_bg);
        fprintf(stderr, "[7aTodo debug] XResourceManagerString = %s\n", rms_dbg ? rms_dbg : "(NULL)");
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
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask |
                           FocusChangeMask | EnterWindowMask);
    XStoreName(dpy, win, app_name);
    XSetIconName(dpy, win, app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name = app_name;
        ch->res_class = "7aTodo";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    icon = MakeListIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 160;
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

    running = 1;
    redraw = 1;

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
            case FocusIn:
                /* mode moze byc NotifyGrab/NotifyUngrab - X wysyla je
                 * automatycznie wokol kazdego klikniecia myszy (niejawny
                 * grab przycisku), nie tylko przy realnej zmianie fokusu.
                 * Bez tego warunku KAZDY klik w to okno resetowalby
                 * g_selected_index momencik po tym, jak checkbox go
                 * dopiero co ustawil - patrz to samo zabezpieczenie przy
                 * EnterNotify nizej. */
                if (ev.xfocus.mode == NotifyNormal) {
                    RefreshKeepingSelection();
                    redraw = 1;
                }
                break;
            case EnterNotify:
                /* jw. - NotifyUngrab/NotifyGrab tu to syntetyczne
                 * "wejscie" generowane przy zwolnieniu niejawnego grabu
                 * (kazdy ButtonRelease w oknie), NIE realny ruch myszy
                 * zza krawedzi okna. */
                if (ev.xcrossing.mode == NotifyNormal) {
                    RefreshKeepingSelection();
                    redraw = 1;
                }
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
    sqlite3_close(db);
    return 0;
}
