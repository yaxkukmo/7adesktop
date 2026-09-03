/*
 * 7acal.c - port oryginalnej apki z ../7acal (Xt/Xaw, wlasny widget
 * Calendar) na biblioteke ui.c/ui.h z tego katalogu - ten sam wzorzec
 * portowania co examples/7aweather.c i examples/7asensors.c (patrz tam
 * obszerniejszy komentarz o roznicach wzgledem Xt/Shell).
 *
 * 7acal jest cienkim widokiem+launcherem nad ta sama baza SQLite co
 * 7atodo (~/.7a/tasks.db) - klikniecie dnia NIE edytuje niczego samo,
 * tylko odpala "7atodo --date YYYY-MM-DD" (fire-and-forget), ktory
 * pokazuje/zarzadza pozycjami tego dnia. Logika bazy/swiat/daty
 * (OpenDatabase, EasterSunday, AddDays, IsPolishHoliday,
 * ResolveTodoCommand) jest przeniesiona z oryginalu prawie bez zmian.
 *
 * Jedna realna roznica: oryginalny widget Calendar wywoluje
 * XtNhasEntryProc (zapytanie SQLite) RAZ NA DZIEN przy kazdym Redisplay,
 * co u niego jest tanie, bo Xaw przerysowuje tylko na prawdziwy Expose/
 * akcje. Nasza petla immediate-mode przerysowuje tez na kazdy
 * MotionNotify (potrzebne do hover strzalek prev/next - patrz ui_button),
 * wiec odpytywanie SQLite per-dzien-per-klatke byloby ~40 zapytan przy
 * kazdym drgnieciu myszy. Zamiast tego RefreshEntries() robi JEDNO
 * zapytanie zakresowe na cely widoczny miesiac i cache'uje wynik w
 * g_has_entry - wywolywane przy starcie, zmianie miesiaca i na Expose
 * (odpowiednik "wracam do okna, moze cos sie zmienilo w 7atodo").
 */

#define _DEFAULT_SOURCE  /* popen/execlp/fork sa POSIX - patrz ta sama uwaga w examples/7aweather.c */

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
#include <X11/Xft/Xft.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20            /* wysokosc naglowka i wiersza nazw dni tygodnia */
#define CELL_H 22            /* wysokosc pojedynczej komorki dnia */
#define GRID_COLS 7
#define GRID_ROWS 6
#define FIRST_DOW 1          /* 0=niedziela..6=sobota - 1 = tydzien zaczyna sie w poniedzialek, jak domyslnie w oryginale */

static sqlite3 *db;
static char *self_path;      /* argv[0], do znalezienia binarki 7atodo */

static int g_year, g_month;
static int g_selected_day = 0;
static int g_has_entry[32];  /* indeks 1..31, patrz RefreshEntries */

typedef struct {
    char day_bg[64];     /* tlo zwyklego dnia - nazwa koloru X11 lub #rrggbb */
    char weekend_bg[64]; /* tlo soboty/niedzieli/swieta - jw. */
} AppData;

static AppData app_data;

/* -------------------------------------------------------------------- */
/* Zasoby X (day/weekend background) - czytane bezposrednio przez Xrm,  */
/* bo to konfiguracja specyficzna dla TEJ apki, nie ogolny motyw ui.c    */
/* (patrz ui_theme_* w ui.h - to tylko background/foreground/...), ten  */
/* sam wzorzec co ReadAppString w examples/7atodo.c.                    */
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

static const char *const dayNames[7] =
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const monthNames[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* -------------------------------------------------------------------- */
/* Baza danych - ten sam bootstrap co OpenDatabase() w ../7acal/7acal.c */
/* -------------------------------------------------------------------- */

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char app_dir[1024];
    char db_path[1200];
    char *errmsg = NULL;

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/tasks.db", app_dir);

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "7acal: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
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
        fprintf(stderr, "7acal: schema: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        exit(1);
    }
    /* Instalacje sprzed dodania kolumny alarm maja juz tabele items bez
     * niej - CREATE TABLE IF NOT EXISTS wyzej nic wtedy nie zmienia, wiec
     * dogrywamy kolumne przez ALTER TABLE. Blad "duplicate column" (gdy
     * kolumna juz istnieje) jest oczekiwany i celowo ignorowany. Ani
     * 7acal, ani 7atodo z tego pola nie korzystaja - jest tu tylko pod
     * przyszle uzycie. */
    sqlite3_exec(db, "ALTER TABLE items ADD COLUMN alarm BOOLEAN NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_items_due_date ON items(due_date);",
        NULL, NULL, NULL);
}

/* Jedno zapytanie zakresowe na caly widoczny miesiac (zamiast 1 zapytania
 * na dzien co klatke - patrz komentarz na gorze pliku) - wypelnia
 * g_has_entry[1..31]. due_date to TEXT w formacie ISO "YYYY-MM-DD", wiec
 * porownanie leksykograficzne BETWEEN dziala poprawnie w obrebie jednego
 * miesiaca. */
static void
RefreshEntries(int year, int month)
{
    char start[11], end[11];
    sqlite3_stmt *stmt;
    int i;

    for (i = 0; i <= 31; i++)
        g_has_entry[i] = 0;

    snprintf(start, sizeof(start), "%04d-%02d-01", year, month);
    snprintf(end, sizeof(end), "%04d-%02d-31", year, month);

    if (sqlite3_prepare_v2(db,
            "SELECT due_date FROM items WHERE due_date BETWEEN ?1 AND ?2;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, start, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, end, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *d = sqlite3_column_text(stmt, 0);

            if (d && strlen((const char *) d) >= 10) {
                int day = atoi((const char *) (d + 8));

                if (day >= 1 && day <= 31)
                    g_has_entry[day] = 1;
            }
        }
        sqlite3_finalize(stmt);
    }
}

/* -------------------------------------------------------------------- */
/* Arytmetyka kalendarza - bez zmian wzgledem Calendar.c z ../7acal      */
/* -------------------------------------------------------------------- */

static int
IsLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int
DaysInMonth(int year, int month)
{
    static const int days[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && IsLeapYear(year))
        return 29;
    return days[month - 1];
}

/* Tomohiko Sakamoto's algorithm; zwraca 0=niedziela..6=sobota. */
static int
DayOfWeek(int year, int month, int day)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3)
        year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

static int
FirstCellOffset(int year, int month)
{
    int dow = DayOfWeek(year, month, 1);
    return (dow - FIRST_DOW + 7) % 7;
}

/* -------------------------------------------------------------------- */
/* Swieta - EasterSunday/AddDays/IsPolishHoliday bez zmian wzgledem      */
/* ../7acal/7acal.c                                                      */
/* -------------------------------------------------------------------- */

static void
EasterSunday(int year, int *out_month, int *out_day)
{
    int a = year % 19;
    int b = year / 100;
    int c = year % 100;
    int d = b / 4;
    int e = b % 4;
    int f = (b + 8) / 25;
    int g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4;
    int k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;

    *out_month = (h + l - 7 * m + 114) / 31;
    *out_day = ((h + l - 7 * m + 114) % 31) + 1;
}

static void
AddDays(int *year, int *month, int *day, int delta)
{
    struct tm tmv;
    time_t t;
    struct tm *norm;

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = *year - 1900;
    tmv.tm_mon = *month - 1;
    tmv.tm_mday = *day + delta;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    t = mktime(&tmv);
    norm = localtime(&t);
    *year = norm->tm_year + 1900;
    *month = norm->tm_mon + 1;
    *day = norm->tm_mday;
}

static int
IsPolishHoliday(int year, int month, int day)
{
    static const struct { int month, day; } fixed[] = {
        { 1, 1 }, { 1, 6 }, { 5, 1 }, { 5, 3 }, { 8, 15 },
        { 11, 1 }, { 11, 11 }, { 12, 25 }, { 12, 26 },
    };
    size_t i;
    int easter_month, easter_day;
    int y, m, d;

    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (month == fixed[i].month && day == fixed[i].day)
            return 1;

    EasterSunday(year, &easter_month, &easter_day);

    y = year; m = easter_month; d = easter_day;
    AddDays(&y, &m, &d, 1);
    if (y == year && m == month && d == day)
        return 1;

    y = year; m = easter_month; d = easter_day;
    AddDays(&y, &m, &d, 60);
    if (y == year && m == month && d == day)
        return 1;

    return 0;
}

/* -------------------------------------------------------------------- */
/* Odpalanie 7atodo - ResolveTodoCommand/DaySelected z ../7acal/7acal.c, */
/* bez zasobu-nadpisania samej komendy (ui_theme_* w ui.h to tylko       */
/* globalny motyw kolorow, wspolny dla calej rodziny 7a* - nie ma tam    */
/* miejsca na cos specyficznego dla 7acal jak sciezka do 7atodo).        */
/* Kolory dnia/weekendu (7aCal.dayBackground/weekendBackground) SA juz   */
/* zasobem wlasnym tej apki - patrz ReadAppString/app_data wyzej.        */
/* -------------------------------------------------------------------- */

static void
ResolveTodoCommand(char *out, size_t outsz)
{
    char candidate[1200];
    char dir[1100];
    const char *slash;

    slash = strrchr(self_path, '/');
    if (slash) {
        size_t dirlen = (size_t) (slash - self_path);

        if (dirlen >= sizeof(dir))
            dirlen = sizeof(dir) - 1;
        memcpy(dir, self_path, dirlen);
        dir[dirlen] = '\0';

        /* 1. ten sam katalog co wlasna binarka - przypadek docelowy:
         *    port 7atodo w tym samym repo (7adesktop). */
        snprintf(candidate, sizeof(candidate), "%s/7atodo", dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, outsz, "%s", candidate);
            return;
        }

        /* 2. stary, oryginalny Xt/Xaw 7atodo w siostrzanym katalogu -
         *    przypadek deweloperski, dopoki port 7atodo tu nie powstanie. */
        snprintf(candidate, sizeof(candidate), "%s/../7atodo/7atodo", dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, outsz, "%s", candidate);
            return;
        }
    }

    /* 3. ostatecznosc: niech execlp szuka w $PATH */
    snprintf(out, outsz, "7atodo");
}

static void
DaySelected(int year, int month, int day)
{
    char datebuf[11];
    char cmd[1200];
    pid_t pid;

    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", year, month, day);
    ResolveTodoCommand(cmd, sizeof(cmd));

    pid = fork();
    if (pid == 0) {
        execlp(cmd, cmd, "-name", "7aTodoInternal", "--date", datebuf, (char *) NULL);
        fprintf(stderr, "7acal: could not run '%s --date %s': %s\n",
                cmd, datebuf, strerror(errno));
        _exit(127);
    }
    /* SIGCHLD ustawione na SIG_IGN w main() - proces potomny sprzatnie jadro. */
}

/* -------------------------------------------------------------------- */
/* Ikona okna - kartka kalendarza, rysowana wprost Xlibem na 1-bitowej  */
/* Pixmapie (jak w 7aweather.c/7asensors.c - jedyne miejsce, ktore       */
/* potrzebuje surowej Pixmapy, nie backbuffera z ui.c).                  */
/* -------------------------------------------------------------------- */

static void
DrawCalendarIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XFillRectangle(idpy, p, gc, 7, 0, 3, 5);
    XFillRectangle(idpy, p, gc, 22, 0, 3, 5);

    XDrawRectangle(idpy, p, gc, 2, 5, 27, 24);
    XFillRectangle(idpy, p, gc, 3, 6, 26, 6);

    XDrawLine(idpy, p, gc, 2, 18, 29, 18);
    XDrawLine(idpy, p, gc, 11, 12, 11, 29);
    XDrawLine(idpy, p, gc, 20, 12, 20, 29);
}

static Pixmap
MakeCalendarIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawCalendarIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

/* Granica (x) lewej krawedzi kolumny "col" (0..GRID_COLS) w siatce
 * zaczynajacej sie w grid_x o szerokosci avail_w. Mnozenie PRZED
 * dzieleniem (zamiast pojedynczego "avail_w / GRID_COLS" i mnozenia
 * cell_w * col) sprawia, ze kolumna GRID_COLS konczy sie DOKLADNIE w
 * grid_x + avail_w - reszta z dzielenia (avail_w % GRID_COLS) rozklada
 * sie po 1px na kilka OSTATNICH kolumn zamiast wpadac w caloski jako
 * dodatkowy odstep po prawej stronie siatki (patrz komentarz przy
 * wywolaniu w draw()). */
static int
grid_col_x(int grid_x, int avail_w, int col)
{
    return grid_x + (col * avail_w) / GRID_COLS;
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    /* Kolory komorek dnia sa specyficzne dla kalendarza (dzisiaj/
     * zaznaczony/z-wpisem/weekend), nie sa czescia ogolnego motywu ui.c
     * (ui_theme_*) - alokowane raz, tak jak wlasne kolory demo.c. */
    static XftColor day_bg, today_bg, select_bg, highlight_bg,
                     select_highlight_bg, weekend_bg, weekend_fg;
    static int ready = 0;
    int y = 0;
    int i;
    time_t now;
    struct tm *tmv;
    int ty, tmo, td;
    int dim, offset, day;
    UiRect content_r;
    int grid_x, grid_y, grid_avail_w;

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

        ui_color(ctx, app_data.day_bg, &day_bg);
        ui_color(ctx, "red", &today_bg);
        ui_color(ctx, "gray70", &select_bg);
        ui_color(ctx, "yellow", &highlight_bg);
        ui_color(ctx, "orange", &select_highlight_bg);
        ui_color(ctx, app_data.weekend_bg, &weekend_bg);
        ui_color(ctx, "red", &weekend_fg);
        ready = 1;
    }

    /* header: strzalki </> (zmiana miesiaca) + "Miesiac Rok" */
    UiBox *header = ui_box_begin(ctx, "header", 0, y, win_w, &style);
    UiRect hrow = ui_box_next_rect(header, ROW_H);
    UiRect prev_r, mid_r, next_r;
    char title[32];

    ui_rect_split3(hrow, ROW_H, ROW_H, 6, &prev_r, &mid_r, &next_r);
    if (ui_button(ctx, prev_r, "<")) {
        g_month--;
        if (g_month < 1) { g_month = 12; g_year--; }
        g_selected_day = 0;
        RefreshEntries(g_year, g_month);
    }
    if (ui_button(ctx, next_r, ">")) {
        g_month++;
        if (g_month > 12) { g_month = 1; g_year++; }
        g_selected_day = 0;
        RefreshEntries(g_year, g_month);
    }
    snprintf(title, sizeof(title), "%s %d", monthNames[g_month - 1], g_year);
    ui_label_centered(ctx, mid_r, title);
    ui_box_end(header);
    y += style.margin_t + ui_box_height(ctx, "header") + style.margin_b;

    /* content: rysowany recznie (nie ui_box_begin/ui_box_next_rect) - to
     * prawdziwa siatka 2D (7x6), nie stos wierszy jednej kolumny, wiec
     * wysokosc jest znana z gory zamiast dopiero z poprzedniej klatki. */
    content_r.x = style.margin_l;
    content_r.y = y;
    content_r.w = win_w - 2 * style.margin_l;
    content_r.h = 2 * style.border_w + 2 * style.padding_t + ROW_H + GRID_ROWS * CELL_H;
    ui_fill_rect(ctx, content_r, &style.bg_color);
    ui_draw_border(ctx, content_r, style.border_w, &style.border_color);

    grid_x = content_r.x + style.border_w + style.padding_l;
    grid_avail_w = content_r.w - 2 * style.border_w - style.padding_l - style.padding_r;
    if (grid_avail_w < GRID_COLS) grid_avail_w = GRID_COLS;

    /* wiersz nazw dni tygodnia */
    for (i = 0; i < GRID_COLS; i++) {
        int dow = (FIRST_DOW + i) % 7;
        int colx0 = grid_col_x(grid_x, grid_avail_w, i);
        int colx1 = grid_col_x(grid_x, grid_avail_w, i + 1);
        UiRect r = { colx0, content_r.y + style.border_w + style.padding_t,
                     colx1 - colx0, ROW_H };
        ui_label_centered(ctx, r, dayNames[dow]);
    }
    grid_y = content_r.y + style.border_w + style.padding_t + ROW_H;

    /* dzisiejsza data - do podswietlenia "today" */
    now = time(NULL);
    tmv = localtime(&now);
    ty = tmv->tm_year + 1900;
    tmo = tmv->tm_mon + 1;
    td = tmv->tm_mday;

    dim = DaysInMonth(g_year, g_month);
    offset = FirstCellOffset(g_year, g_month);

    for (day = 1; day <= dim; day++) {
        int idx = offset + day - 1;
        int row = idx / GRID_COLS;
        int col = idx % GRID_COLS;
        UiRect cell;
        int dow, is_dayoff, is_selected, is_today, has_entry;
        const XftColor *bg;
        const XftColor *fg;
        char buf[4];

        int colx0, colx1;

        if (row >= GRID_ROWS)
            break;

        colx0 = grid_col_x(grid_x, grid_avail_w, col);
        colx1 = grid_col_x(grid_x, grid_avail_w, col + 1);
        cell.x = colx0;
        cell.y = grid_y + row * CELL_H;
        cell.w = colx1 - colx0 - 2;
        cell.h = CELL_H - 2;

        dow = DayOfWeek(g_year, g_month, day);
        is_dayoff = (dow == 0 || dow == 6) || IsPolishHoliday(g_year, g_month, day);
        is_selected = (day == g_selected_day);
        is_today = (g_year == ty && g_month == tmo && day == td);
        has_entry = g_has_entry[day];

        if (is_selected && has_entry) bg = &select_highlight_bg;
        else if (is_selected)         bg = &select_bg;
        else if (is_today)            bg = &today_bg;
        else if (has_entry)           bg = &highlight_bg;
        else if (is_dayoff)           bg = &weekend_bg;
        else                          bg = &day_bg;

        fg = is_dayoff ? &weekend_fg : ui_theme_fg(ctx);

        ui_fill_rect(ctx, cell, bg);
        snprintf(buf, sizeof(buf), "%d", day);
        ui_label_centered_fg(ctx, cell, buf, fg);

        if (ui_hit_test(ctx, cell)) {
            g_selected_day = day;
            DaySelected(g_year, g_month, day);
        }
    }

    (void) win_h;
    return 0;
}

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Sekund do najblizszej polnocy - "today" ma sie przesunac nawet gdy
 * apka stoi bezczynnie, bez zadnego innego zdarzenia. Ten sam pomysl co
 * ScheduleMidnightTimer w ../7acal/Calendar.c. */
static long
seconds_until_midnight(void)
{
    time_t now = time(NULL);
    struct tm tmv = *localtime(&now);
    time_t next_midnight;
    long seconds;

    tmv.tm_hour = 0;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    tmv.tm_mday += 1;
    tmv.tm_isdst = -1;
    next_midnight = mktime(&tmv);
    seconds = (long) difftime(next_midnight, now);
    if (seconds < 1)
        seconds = 1;
    return seconds;
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
    int win_w = 260, win_h = 210;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    long next_wake_ms;
    XEvent ev;
    time_t now;
    struct tm *tmv;

    self_path = argv[0];
    signal(SIGCHLD, SIG_IGN);
    OpenDatabase();

    now = time(NULL);
    tmv = localtime(&now);
    g_year = tmv->tm_year + 1900;
    g_month = tmv->tm_mon + 1;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    /* Bez tego XrmGetResource w ReadAppString nizej potrafi zwrocic
     * poprawna wartosc, ale z type == NULL (zaobserwowane na OpenBSD) -
     * warunek "type && strcmp(type, "String") == 0" wtedy zawsze zawodzi
     * i kazdy zasob 7aCal.* cicho spada na wartosc domyslna. ui_init
     * (ui.c) wywoluje XrmInitialize() tez, ale dopiero PO tym ponizej,
     * wiec nie ratuje to sytuacji - patrz ten sam komentarz/obejscie w
     * examples/7atodo.c. */
    XrmInitialize();

    ReadAppString(dpy, "7aCal.dayBackground", "7aCal.DayBackground",
                  app_data.day_bg, sizeof(app_data.day_bg), "white");
    ReadAppString(dpy, "7aCal.weekendBackground", "7aCal.WeekendBackground",
                  app_data.weekend_bg, sizeof(app_data.weekend_bg), "gray95");

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
    XStoreName(dpy, win, "7aCal");
    XSetIconName(dpy, win, "7aCal");

    icon = MakeCalendarIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 200;
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "DejaVu Sans-9", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    RefreshEntries(g_year, g_month);

    running = 1;
    redraw = 1;
    next_wake_ms = now_ms() + seconds_until_midnight() * 1000;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    redraw = 1;
                    /* okno wraca do widoku - odswiez wpisy, moze cos sie
                     * zmienilo w 7atodo, patrz komentarz na gorze pliku */
                    RefreshEntries(g_year, g_month);
                }
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

        long remaining = next_wake_ms - now_ms();

        if (remaining <= 0) {
            redraw = 1;
            next_wake_ms = now_ms() + seconds_until_midnight() * 1000;
            continue;
        }

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy);
            struct timeval tv;

            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
            select(xfd + 1, &rfds, NULL, NULL, &tv);
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    sqlite3_close(db);
    return 0;
}
