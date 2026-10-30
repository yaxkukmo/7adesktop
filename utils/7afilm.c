/*
 * 7afilm.c - pomoc do wywolywania filmu czarno-bialego: trzy kolejne
 * kapiele (Development / Stop bath / Fix) z minutnikiem, zapisane
 * receptury i kalkulatory ciemni.
 *
 * Pelny port terminalowej apki 7afilm-tui.c (osobne repo tui/film) na
 * ui.c/ui.h - te same zakladki i ta sama zawartosc, rysowane immediate-
 * mode nad jednym boxem tresci, z paskiem ui_tabs na jego gornej
 * krawedzi (pierwsza apka repo uzywajaca ui_tabs):
 *
 *   Timer    - przebieg "workflow" (g_workflow_phase, 0..6 jak w TUI):
 *              jeden przycisk prowadzi Development -> Stop bath -> Fix ->
 *              Reset; czas, pasek postepu ze znacznikami alarmu "Every",
 *              Stop przerywa calosc.
 *   Database - lista zapisanych receptur (klik = wczytaj, "x" = usun,
 *              kolko myszy przewija - zamiast wyszukiwarki z TUI) oraz
 *              wszystkie pola trzech kapieli; Save zapisuje recepture
 *              pod nazwa skladana automatycznie (BuildPresetName).
 *   Calc     - kompensacja temperatury (base / 2^((actual-base)/10)) i
 *              rozcienczenie wywolywacza na zadana objetosc.
 *   Config   - edycja czterech list opcji (film/developer/iso/dilution)
 *              i domyslne czasy + temperatura ("Apply defaults").
 *   Help/Quit - skroty klawiszowe; wyjscie (zablokowane gdy minutnik
 *              dziala).
 *   F1-F4/F11/F12 przelaczaja zakladki (przechwycone w main() przed
 *   ui_feed_event, jak w TUI).
 *
 * Baza ~/.7a/film.db jest WSPOLDZIELONA z TUI, wiec warstwa bazy
 * (OpenDatabase, migracje przez PRAGMA user_version, migracja ukladow
 * sprzed wersjonowania, seed opcji, zapytania presetow) jest skopiowana
 * z 7afilm-tui.c + lib/db.c 1:1 co do SQL - zmiana schematu musi trafic
 * do OBU apek naraz (film_migrations: tylko dopisywac kroki, nigdy nie
 * zmieniac istniejacych). Opcje (film/iso/developer/dilution) zyja w
 * tabeli options, presety odwoluja sie do nich przez *_id, czasy to
 * INTEGER w sekundach. Domyslne wartosci z Config, tak jak w TUI, NIE
 * sa zapisywane w bazie (tylko na czas dzialania programu).
 *
 * Roznice wzgledem TUI: ui.h nie ma listy rozwijanej, wiec Dropdown to
 * stan apki rysowany na wierzchu klatki (wzorem priorytetu w 7atodo.c),
 * max DD_MAX_ROWS wierszy + kolko myszy, Escape/klik obok zamyka;
 * komunikaty ida do wiersza statusu na dole okna (jak w TUI), a
 * blokady "stop timers first" dodatkowo do okna 7amessage (ShowMessage).
 * Alarm: 7aFilm.alarmPlayer/alarmSound (zasoby X) albo XBell.
 */

#define _DEFAULT_SOURCE  /* execvp/fork sa POSIX - patrz ta sama uwaga w utils/7aweather.c */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <sqlite3.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/camera_roll_48.xbm"

#define ROW_H 20
#define ROW_GAP 6          /* jak demo.c/7avol.c - pola formularza sie nie stykaja */
#define TICK_MS 1000
#define ALARM_PULSE_MS 500
#define MAX_ALARM_TOKENS 16
#define ARROW_W 20
#define DD_MAX_ROWS 8      /* widoczne wiersze listy rozwijanej, reszta kolkiem */
#define TAB_H 22
#define TAB_INDENT 8
#define BOX_MARGIN 6
#define BOX_PAD_V 4
/* najwyzsza zakladka (Database: 4 presety + nazwa + 5 wyborow + 5 czasow
 * + Save), plus jeden wiersz zapasu na wyzsze przyciski */
#define CONTENT_ROWS 17
#define PRESET_NAME_LEN 64
#define VISIBLE_PRESETS 4
#define MAX_DYN_OPTIONS 128
#define MAX_OPTION_LEN 64

#define TAB_TIMER     0
#define TAB_DATABASE  1
#define TAB_CALC      2
#define TAB_CONFIG    3
#define TAB_HELP      4
#define TAB_QUIT      5
#define TAB_COUNT     6

static Display *g_dpy;
static char g_status[128];
static int g_tab = TAB_TIMER;
static int g_label_w;  /* szerokosc kolumny etykiet, liczona raz (LabelColWidth) */

/* -------------------------------------------------------------------- */
/* Minutniki - jak timer.c w TUI                                         */
/* -------------------------------------------------------------------- */

typedef struct {
    const char *label;
    int has_alarm, has_recipe;
    char hh_buf[8], mm_buf[8], ss_buf[8];
    char alarm_buf[8];       /* "Every" - odstep przypomnien (s) */
    char alarm_dur_buf[8];   /* "For" - dlugosc koncowego alarmu (s) */
    char dev_name_buf[32];
    char dilution_buf[24];
    char film_buf[32];
    char iso_buf[8];
    char iso_used_buf[8];
    int hms_cur[3], alarm_cur, dur_cur;
    int remaining, total, running;
    long next_tick_ms;
    int alarm_pulses_left;
    long next_alarm_pulse_ms;
} CountdownTimer;

#define TIMER_COUNT 3
static CountdownTimer g_timers[TIMER_COUNT];

/* 0=gotowy(dev) 1=dev 2=gotowy(stop) 3=stop 4=gotowy(fix) 5=fix 6=koniec */
static int g_workflow_phase = 0;
static char g_temp_buf[8] = "20";  /* temperatura - jak w TUI, poza baza */
static int g_temp_cur;

static void SetTimerTime(CountdownTimer *t, int secs);

static void
InitTimers(void)
{
    static const char *labels[TIMER_COUNT] = { "Development", "Stop bath", "Fix" };
    static const int has_alarm[TIMER_COUNT] = { 1, 0, 1 };
    static const int secs[TIMER_COUNT] = { 9 * 60 + 30, 60, 5 * 60 };
    int i;

    for (i = 0; i < TIMER_COUNT; i++) {
        CountdownTimer *t = &g_timers[i];

        memset(t, 0, sizeof(*t));
        t->label = labels[i];
        t->has_alarm = has_alarm[i];
        t->has_recipe = (i == 0);
        SetTimerTime(t, secs[i]);
        snprintf(t->alarm_buf, sizeof(t->alarm_buf), "0");
        snprintf(t->alarm_dur_buf, sizeof(t->alarm_dur_buf), "10");
        snprintf(t->dilution_buf, sizeof(t->dilution_buf), "Stock");
        snprintf(t->iso_used_buf, sizeof(t->iso_used_buf), "400");
    }
}

typedef struct {
    char alarm_player[128];  /* np. "aplay" - pusty = XBell */
    char alarm_sound[256];
} AppData;

static AppData app_data;

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int
AnyTimerRunning(void)
{
    int i;

    for (i = 0; i < TIMER_COUNT; i++)
        if (g_timers[i].running)
            return 1;
    return 0;
}

static char *self_path;  /* argv[0], do znalezienia binarki 7amessage */

/* 7amessage obok wlasnej binarki, inaczej z $PATH (jak 7acal -> 7atodo) */
static void
ResolveMessageCommand(char *out, size_t outsz)
{
    char candidate[1200];
    const char *slash = self_path ? strrchr(self_path, '/') : NULL;

    if (slash) {
        snprintf(candidate, sizeof(candidate), "%.*s/7amessage",
                 (int) (slash - self_path), self_path);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, outsz, "%s", candidate);
            return;
        }
    }
    snprintf(out, outsz, "7amessage");
}

/* komunikat w status + okno 7amessage (fork+execlp, fire-and-forget) */
static void
ShowMessage(const char *text)
{
    char cmd[1200];

    snprintf(g_status, sizeof(g_status), "%s", text);
    ResolveMessageCommand(cmd, sizeof(cmd));
    if (fork() == 0) {
        execlp(cmd, cmd, "-name", "7aFilmMessage", "-title", "7afilm", text, (char *) NULL);
        fprintf(stderr, "7afilm: could not run '%s': %s\n", cmd, strerror(errno));
        _exit(127);
    }
}

/* fork+execvp alarmPlayer z alarmSound, bez czekania (SIGCHLD=SIG_IGN) */
static void
RunAlarmCommand(void)
{
    char player_buf[256];
    char *argv[MAX_ALARM_TOKENS];
    int argc = 0;
    char *tok;

    snprintf(player_buf, sizeof(player_buf), "%s", app_data.alarm_player);
    tok = strtok(player_buf, " \t");
    while (tok && argc < MAX_ALARM_TOKENS - 2) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0)
        return;
    argv[argc++] = app_data.alarm_sound;
    argv[argc] = NULL;

    if (fork() == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
}

static void
FireAlarmPulse(CountdownTimer *t)
{
    if (app_data.alarm_player[0] != '\0' && app_data.alarm_sound[0] != '\0')
        RunAlarmCommand();
    else
        XBell(g_dpy, 50);
    t->alarm_pulses_left--;
}

static int
GetAlarmDurationPulses(CountdownTimer *t)
{
    long secs = strtol(t->alarm_dur_buf, NULL, 10);
    int p;

    if (secs < 0) secs = 0;
    p = (int) ((secs * 1000) / ALARM_PULSE_MS);
    return p < 1 ? 1 : p;
}

static void
StartAlarm(CountdownTimer *t)
{
    if (!t->has_alarm)
        return;
    t->alarm_pulses_left = GetAlarmDurationPulses(t);
    t->next_alarm_pulse_ms = now_ms() + ALARM_PULSE_MS;
    FireAlarmPulse(t);
}

static void
ParseCountdownFields(CountdownTimer *t)
{
    long hh = strtol(t->hh_buf, NULL, 10);
    long mm = strtol(t->mm_buf, NULL, 10);
    long ss = strtol(t->ss_buf, NULL, 10);

    if (hh < 0) hh = 0;
    if (hh > 999) hh = 999;
    if (mm < 0) mm = 0;
    if (mm > 59) mm = 59;
    if (ss < 0) ss = 0;
    if (ss > 59) ss = 59;
    t->remaining = (int) (hh * 3600 + mm * 60 + ss);
}

static void
SetTimerTime(CountdownTimer *t, int secs)
{
    if (secs < 0) secs = 0;
    snprintf(t->hh_buf, sizeof(t->hh_buf), "%02d", secs / 3600 % 1000);
    snprintf(t->mm_buf, sizeof(t->mm_buf), "%02d", (secs % 3600) / 60);
    snprintf(t->ss_buf, sizeof(t->ss_buf), "%02d", secs % 60);
    t->hms_cur[0] = t->hms_cur[1] = t->hms_cur[2] = 0;
}

static void
CountdownDoStart(CountdownTimer *t)
{
    if (t->running)
        return;
    ParseCountdownFields(t);
    if (t->remaining <= 0) {
        snprintf(g_status, sizeof(g_status), "%s: set time > 0 first.", t->label);
        return;
    }
    t->total = t->remaining;
    t->running = 1;
    t->next_tick_ms = now_ms() + TICK_MS;
    snprintf(g_status, sizeof(g_status), "%s: started.", t->label);
}

static void
CountdownDoStop(CountdownTimer *t)
{
    t->running = 0;
    t->alarm_pulses_left = 0;
}

static void
CountdownDoReset(CountdownTimer *t)
{
    /* przywraca czas sprzed startu, zeby kolejny przebieg mial te same czasy */
    CountdownDoStop(t);
    if (t->total > 0)
        SetTimerTime(t, t->total);
    t->remaining = 0;
    t->total = 0;
}

/* co TICK_MS dopoki t->running; przy zerze przesuwa workflow dalej */
static void
CountdownTick(CountdownTimer *t)
{
    int interval;

    t->remaining--;
    SetTimerTime(t, t->remaining);
    if (t->remaining <= 0) {
        t->running = 0;
        StartAlarm(t);
        snprintf(g_status, sizeof(g_status), "*** %s: DONE! ***", t->label);
        if (t == &g_timers[0] && g_workflow_phase == 1) g_workflow_phase = 2;
        else if (t == &g_timers[1] && g_workflow_phase == 3) g_workflow_phase = 4;
        else if (t == &g_timers[2] && g_workflow_phase == 5) g_workflow_phase = 6;
        return;
    }
    if (!t->has_alarm)
        return;
    interval = (int) strtol(t->alarm_buf, NULL, 10);
    if (interval > 0 && t->remaining % interval == 0)
        StartAlarm(t);
}

/* -------------------------------------------------------------------- */
/* DynList - lista opcji jednej kategorii (lib/dynlist.c z TUI), stale   */
/* bufory zamiast malloc                                                 */
/* -------------------------------------------------------------------- */

typedef struct {
    char items[MAX_DYN_OPTIONS][MAX_OPTION_LEN];
    const char *ptrs[MAX_DYN_OPTIONS + 1];
    int count;
    int sel;
    const char *category;
    char edit_buf[MAX_OPTION_LEN];
    int edit_cur;
} DynList;

static DynList g_films, g_devs, g_isos, g_dils;
static int g_film_idx, g_iso_nom_idx, g_iso_used_idx, g_dev_idx, g_dil_idx;

static void
dynlist_init(DynList *dl, const char *cat)
{
    memset(dl, 0, sizeof(*dl));
    dl->category = cat;
}

static void
dynlist_rebuild_ptrs(DynList *dl)
{
    int i;

    for (i = 0; i < dl->count; i++)
        dl->ptrs[i] = dl->items[i];
    dl->ptrs[dl->count] = NULL;
}

/* liczby numerycznie (ISO), "N:M" po mianowniku, reszta alfabetycznie */
static int
dynlist_cmp(const void *a, const void *b)
{
    const char *sa = a, *sb = b;
    char *ea, *eb;
    long na = strtol(sa, &ea, 10), nb = strtol(sb, &eb, 10);

    if (*ea == '\0' && *eb == '\0')
        return (na > nb) - (na < nb);
    if (strchr(sa, ':') && strchr(sb, ':')) {
        na = strtol(strchr(sa, ':') + 1, &ea, 10);
        nb = strtol(strchr(sb, ':') + 1, &eb, 10);
        if (*ea == '\0' && *eb == '\0')
            return (na > nb) - (na < nb);
    }
    return strcasecmp(sa, sb);
}

static void
dynlist_add(DynList *dl, const char *item)
{
    int i;

    if (!item || !item[0] || dl->count >= MAX_DYN_OPTIONS)
        return;
    for (i = 0; i < dl->count; i++)
        if (strcmp(dl->items[i], item) == 0)
            return;
    snprintf(dl->items[dl->count], MAX_OPTION_LEN, "%s", item);
    dl->count++;
    qsort(dl->items, (size_t) dl->count, MAX_OPTION_LEN, dynlist_cmp);
    dynlist_rebuild_ptrs(dl);
}

static void
dynlist_delete(DynList *dl, int idx)
{
    int i;

    if (idx < 0 || idx >= dl->count)
        return;
    for (i = idx; i < dl->count - 1; i++)
        memcpy(dl->items[i], dl->items[i + 1], MAX_OPTION_LEN);
    dl->count--;
    dynlist_rebuild_ptrs(dl);
    if (dl->sel >= dl->count) dl->sel = dl->count - 1;
    if (dl->sel < 0) dl->sel = 0;
}

static int
find_option_idx(const DynList *dl, const char *val)
{
    int i;

    if (!val || !val[0])
        return 0;
    for (i = 0; i < dl->count; i++)
        if (strcmp(dl->items[i], val) == 0)
            return i;
    return 0;
}

static void
sync_dropdown_indices(void)
{
    CountdownTimer *dev = &g_timers[0];

    g_film_idx = find_option_idx(&g_films, dev->film_buf);
    g_iso_nom_idx = find_option_idx(&g_isos, dev->iso_buf);
    g_iso_used_idx = find_option_idx(&g_isos, dev->iso_used_buf);
    g_dev_idx = find_option_idx(&g_devs, dev->dev_name_buf);
    g_dil_idx = find_option_idx(&g_dils, dev->dilution_buf);
}

static void
sync_bufs_from_indices(void)
{
    CountdownTimer *dev = &g_timers[0];

    if (g_films.count > 0)
        snprintf(dev->film_buf, sizeof(dev->film_buf), "%s", g_films.items[g_film_idx]);
    if (g_isos.count > 0) {
        snprintf(dev->iso_buf, sizeof(dev->iso_buf), "%s", g_isos.items[g_iso_nom_idx]);
        snprintf(dev->iso_used_buf, sizeof(dev->iso_used_buf), "%s", g_isos.items[g_iso_used_idx]);
    }
    if (g_devs.count > 0)
        snprintf(dev->dev_name_buf, sizeof(dev->dev_name_buf), "%s", g_devs.items[g_dev_idx]);
    if (g_dils.count > 0)
        snprintf(dev->dilution_buf, sizeof(dev->dilution_buf), "%s", g_dils.items[g_dil_idx]);
}

/* -------------------------------------------------------------------- */
/* Baza ~/.7a/film.db - SQL 1:1 z 7afilm-tui.c + lib/db.c (wspolny plik) */
/* -------------------------------------------------------------------- */

static sqlite3 *g_db;

typedef struct {
    sqlite3_int64 id;
    char name[PRESET_NAME_LEN];
} PresetRow;

static PresetRow *g_presets = NULL;
static int g_preset_count = 0;
static int g_preset_cap = 0;
static int g_preset_scroll = 0;
static UiRect g_preset_list_r;  /* lista z ostatniej klatki, dla kolka myszy */
static sqlite3_int64 g_preset_loaded_id = -1;
static char g_preset_name[PRESET_NAME_LEN] = "";

#define OPTIONS_TABLE \
    " (" \
    " id INTEGER PRIMARY KEY AUTOINCREMENT," \
    " category TEXT NOT NULL," \
    " value TEXT NOT NULL," \
    " UNIQUE(category, value)" \
    ");"

#define PRESETS_TABLE \
    " (" \
    " id           INTEGER PRIMARY KEY AUTOINCREMENT," \
    " name         TEXT NOT NULL COLLATE NOCASE UNIQUE," \
    " film_id      INTEGER REFERENCES options(id) ON DELETE SET NULL," \
    " iso_id       INTEGER REFERENCES options(id) ON DELETE SET NULL," \
    " iso_used_id  INTEGER REFERENCES options(id) ON DELETE SET NULL," \
    " developer_id INTEGER REFERENCES options(id) ON DELETE SET NULL," \
    " dilution_id  INTEGER REFERENCES options(id) ON DELETE SET NULL," \
    " dev_time     INTEGER NOT NULL DEFAULT 0," \
    " dev_every    INTEGER NOT NULL DEFAULT 0," \
    " dev_for      INTEGER NOT NULL DEFAULT 10," \
    " stop_time    INTEGER NOT NULL DEFAULT 0," \
    " fix_time     INTEGER NOT NULL DEFAULT 0," \
    " fix_every    INTEGER NOT NULL DEFAULT 0," \
    " fix_for      INTEGER NOT NULL DEFAULT 10" \
    ");"

/* kroki schematu - tylko dopisywac, jak w TUI */
static const char *const film_migrations[] = {
    "CREATE TABLE IF NOT EXISTS options" OPTIONS_TABLE
    "CREATE TABLE IF NOT EXISTS presets" PRESETS_TABLE,
};

static const char *film_options[] = {
    "Kodak Tri-X 400", "Kodak T-Max 100", "Kodak T-Max 400",
    "Ilford HP5 Plus", "Ilford FP4 Plus", "Ilford Delta 100",
    "Ilford Delta 400", "Ilford Delta 3200", "Fomapan 100",
    "Fomapan 200", "Fomapan 400", NULL
};
static const char *iso_options[] = {
    "25", "50", "100", "125", "160", "200", "250", "320",
    "400", "500", "640", "800", "1000", "1250", "1600",
    "2000", "2500", "3200", NULL
};
static const char *dev_options[] = {
    "Kodak D-76", "Kodak HC-110", "Kodak XTOL",
    "Ilford ID-11", "Ilford Ilfosol 3", "Ilford Microphen",
    "Rodinal / R09", "Caffenol-C", NULL
};
static const char *dil_options[] = {
    "Stock", "1:1", "1:2", "1:3", "1:4", "1:9",
    "1:25", "1:31", "1:50", "1:100", NULL
};

static const char *
db_col_str(sqlite3_stmt *s, int c)
{
    const unsigned char *v = sqlite3_column_text(s, c);

    return v ? (const char *) v : "";
}

static void
db_bind_id(sqlite3_stmt *s, int param, sqlite3_int64 id)
{
    if (id < 0) sqlite3_bind_null(s, param);
    else sqlite3_bind_int64(s, param, id);
}

static int
db_user_version(void)
{
    sqlite3_stmt *stmt;
    int version = 0;

    if (sqlite3_prepare_v2(g_db, "PRAGMA user_version;", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return version;
}

/* kroki steps[user_version..nsteps-1], kazdy we wlasnej transakcji */
static int
db_migrate(const char *const *steps, int nsteps)
{
    int version = db_user_version();
    char sql[64];

    if (version < 0)
        return -1;
    for (; version < nsteps; version++) {
        snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", version + 1);
        if (sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK)
            return -1;
        if (sqlite3_exec(g_db, steps[version], NULL, NULL, NULL) != SQLITE_OK ||
            sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            char err[256];

            snprintf(err, sizeof(err), "%s", sqlite3_errmsg(g_db));
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            fprintf(stderr, "7afilm: schema migration %d failed: %s\n", version + 1, err);
            return -1;
        }
        if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
            return -1;
    }
    return 0;
}

static int
db_table_exists(const char *name)
{
    sqlite3_stmt *stmt;
    int exists = 0;

    if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    return exists;
}

static int
db_column_exists(const char *table, const char *col)
{
    sqlite3_stmt *stmt;
    char sql[256];
    int exists = 0;

    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cn = (const char *) sqlite3_column_text(stmt, 1);

            if (cn && strcmp(cn, col) == 0) { exists = 1; break; }
        }
        sqlite3_finalize(stmt);
    }
    return exists;
}

/* presety z kolumnami hh/mm/ss TEXT i nazwami zamiast id opcji */
static void
MigrateFromOldSchema(void)
{
    sqlite3_exec(g_db,
        "INSERT OR IGNORE INTO options(category, value)"
        " SELECT DISTINCT 'dilution', dev_dilution FROM presets"
        " WHERE dev_dilution != '' AND dev_dilution IS NOT NULL;",
        NULL, NULL, NULL);
    sqlite3_exec(g_db, "CREATE TABLE presets_new" PRESETS_TABLE, NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "INSERT OR IGNORE INTO presets_new"
        " (name, film_id, iso_id, iso_used_id, developer_id, dilution_id,"
        "  dev_time, dev_every, dev_for, stop_time, fix_time, fix_every, fix_for)"
        " SELECT p.name,"
        "  (SELECT id FROM options WHERE category='film' AND value=p.dev_film),"
        "  (SELECT id FROM options WHERE category='iso' AND value=p.dev_iso),"
        "  (SELECT id FROM options WHERE category='iso' AND value=p.dev_iso_used),"
        "  (SELECT id FROM options WHERE category='developer' AND value=p.dev_developer),"
        "  (SELECT id FROM options WHERE category='dilution' AND value=p.dev_dilution),"
        "  CAST(COALESCE(p.dev_hh,'0') AS INTEGER)*3600"
        "   + CAST(COALESCE(p.dev_mm,'0') AS INTEGER)*60"
        "   + CAST(COALESCE(p.dev_ss,'0') AS INTEGER),"
        "  CAST(COALESCE(p.dev_every,'0') AS INTEGER),"
        "  CAST(COALESCE(p.dev_for,'10') AS INTEGER),"
        "  CAST(COALESCE(p.stop_hh,'0') AS INTEGER)*3600"
        "   + CAST(COALESCE(p.stop_mm,'0') AS INTEGER)*60"
        "   + CAST(COALESCE(p.stop_ss,'0') AS INTEGER),"
        "  CAST(COALESCE(p.fix_hh,'0') AS INTEGER)*3600"
        "   + CAST(COALESCE(p.fix_mm,'0') AS INTEGER)*60"
        "   + CAST(COALESCE(p.fix_ss,'0') AS INTEGER),"
        "  CAST(COALESCE(p.fix_every,'0') AS INTEGER),"
        "  CAST(COALESCE(p.fix_for,'10') AS INTEGER)"
        " FROM presets p;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "DROP TABLE presets;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE presets_new RENAME TO presets;", NULL, NULL, NULL);
}

/* uklady sprzed PRAGMA user_version, rozpoznawane po tabelach/kolumnach */
static void
UpgradeUnversioned(void)
{
    int user_lists = db_table_exists("user_lists");
    int old_presets = db_column_exists("presets", "dev_hh");

    if (!user_lists && !old_presets)
        return;
    sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "CREATE TABLE IF NOT EXISTS options" OPTIONS_TABLE, NULL, NULL, NULL);
    if (user_lists) {
        sqlite3_exec(g_db,
            "INSERT OR IGNORE INTO options(category, value)"
            " SELECT category, item FROM user_lists;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "DROP TABLE user_lists;", NULL, NULL, NULL);
    }
    if (old_presets)
        MigrateFromOldSchema();
    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
}

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char app_dir[1024], db_path[1040];

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/film.db", app_dir);
    if (sqlite3_open(db_path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "7afilm: cannot open %s\n", db_path);
        exit(1);
    }
    /* bez lookaside (~120 kB) - przy tak malej bazie nic nie daje */
    sqlite3_db_config(g_db, SQLITE_DBCONFIG_LOOKASIDE, NULL, 0, 0);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);

    if (db_user_version() == 0)
        UpgradeUnversioned();
    if (db_migrate(film_migrations,
                   (int) (sizeof(film_migrations) / sizeof(film_migrations[0]))) != 0)
        exit(1);
}

static int
ExecOption(const char *sql, const char *a, const char *b, const char *c)
{
    sqlite3_stmt *stmt;
    int rc;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, b, -1, SQLITE_TRANSIENT);
    if (c)
        sqlite3_bind_text(stmt, 3, c, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static void
SaveOption(const char *category, const char *item)
{
    ExecOption("INSERT OR IGNORE INTO options (category, value) VALUES (?1, ?2);",
               category, item, NULL);
}

/* presety trzymaja id, wiec zmiana nazwy opcji "przenosi sie" sama */
static int
RenameOption(const char *category, const char *old_val, const char *new_val)
{
    return ExecOption("UPDATE options SET value=?1 WHERE category=?2 AND value=?3;",
                      new_val, category, old_val);
}

static void
DeleteOption(const char *category, const char *item)
{
    ExecOption("DELETE FROM options WHERE category=?1 AND value=?2;", category, item, NULL);
}

static sqlite3_int64
UpsertOption(const char *category, const char *value)
{
    sqlite3_stmt *stmt;
    sqlite3_int64 id = -1;

    if (!value || !value[0])
        return -1;
    SaveOption(category, value);
    if (sqlite3_prepare_v2(g_db, "SELECT id FROM options WHERE category=?1 AND value=?2;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, category, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return id;
}

static void
SeedOptionsIfEmpty(void)
{
    sqlite3_stmt *stmt;
    int count = 0, i;

    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM options;", -1, &stmt, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count > 0)
        return;
    for (i = 0; film_options[i]; i++) SaveOption("film", film_options[i]);
    for (i = 0; iso_options[i]; i++) SaveOption("iso", iso_options[i]);
    for (i = 0; dev_options[i]; i++) SaveOption("developer", dev_options[i]);
    for (i = 0; dil_options[i]; i++) SaveOption("dilution", dil_options[i]);
}

static void
LoadOptions(void)
{
    DynList *lists[] = { &g_films, &g_isos, &g_devs, &g_dils };
    sqlite3_stmt *stmt;
    size_t i;

    if (sqlite3_prepare_v2(g_db, "SELECT category, value FROM options ORDER BY id;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cat = db_col_str(stmt, 0);

        for (i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
            if (strcmp(cat, lists[i]->category) == 0)
                dynlist_add(lists[i], db_col_str(stmt, 1));
    }
    sqlite3_finalize(stmt);
}

/* cala lista w pamieci - tylko po akcji uzytkownika, nigdy per klatke */
static void
LoadPresetList(void)
{
    sqlite3_stmt *stmt;

    g_preset_count = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT id, name FROM presets ORDER BY name COLLATE NOCASE;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (g_preset_count >= g_preset_cap) {
            int nc = g_preset_cap ? g_preset_cap * 2 : 16;
            PresetRow *tmp = realloc(g_presets, (size_t) nc * sizeof(PresetRow));

            if (!tmp)
                break;
            g_presets = tmp;
            g_preset_cap = nc;
        }
        g_presets[g_preset_count].id = sqlite3_column_int64(stmt, 0);
        snprintf(g_presets[g_preset_count].name, PRESET_NAME_LEN, "%s", db_col_str(stmt, 1));
        g_preset_count++;
    }
    sqlite3_finalize(stmt);
    if (g_preset_scroll > g_preset_count - VISIBLE_PRESETS)
        g_preset_scroll = g_preset_count > VISIBLE_PRESETS ? g_preset_count - VISIBLE_PRESETS : 0;
}

static void
DeletePreset(sqlite3_int64 id)
{
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(g_db, "DELETE FROM presets WHERE id=?1;", -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (g_preset_loaded_id == id)
        g_preset_loaded_id = -1;
    LoadPresetList();
    snprintf(g_status, sizeof(g_status), "Preset deleted.");
}

static void
LoadPresetIntoTimers(sqlite3_int64 id)
{
    sqlite3_stmt *stmt;
    CountdownTimer *dev = &g_timers[0], *stop = &g_timers[1], *fix = &g_timers[2];

    if (sqlite3_prepare_v2(g_db,
            "SELECT f.value, i.value, iu.value, d.value, dil.value,"
            "       p.dev_time, p.dev_every, p.dev_for,"
            "       p.stop_time,"
            "       p.fix_time, p.fix_every, p.fix_for"
            " FROM presets p"
            " LEFT JOIN options f   ON f.id   = p.film_id"
            " LEFT JOIN options i   ON i.id   = p.iso_id"
            " LEFT JOIN options iu  ON iu.id  = p.iso_used_id"
            " LEFT JOIN options d   ON d.id   = p.developer_id"
            " LEFT JOIN options dil ON dil.id = p.dilution_id"
            " WHERE p.id=?1;", -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        snprintf(dev->film_buf, sizeof(dev->film_buf), "%s", db_col_str(stmt, 0));
        snprintf(dev->iso_buf, sizeof(dev->iso_buf), "%s", db_col_str(stmt, 1));
        snprintf(dev->iso_used_buf, sizeof(dev->iso_used_buf), "%s", db_col_str(stmt, 2));
        snprintf(dev->dev_name_buf, sizeof(dev->dev_name_buf), "%s", db_col_str(stmt, 3));
        snprintf(dev->dilution_buf, sizeof(dev->dilution_buf), "%s", db_col_str(stmt, 4));
        SetTimerTime(dev, sqlite3_column_int(stmt, 5));
        snprintf(dev->alarm_buf, sizeof(dev->alarm_buf), "%d", sqlite3_column_int(stmt, 6));
        snprintf(dev->alarm_dur_buf, sizeof(dev->alarm_dur_buf), "%d", sqlite3_column_int(stmt, 7));
        SetTimerTime(stop, sqlite3_column_int(stmt, 8));
        SetTimerTime(fix, sqlite3_column_int(stmt, 9));
        snprintf(fix->alarm_buf, sizeof(fix->alarm_buf), "%d", sqlite3_column_int(stmt, 10));
        snprintf(fix->alarm_dur_buf, sizeof(fix->alarm_dur_buf), "%d", sqlite3_column_int(stmt, 11));
        dev->alarm_cur = dev->dur_cur = fix->alarm_cur = fix->dur_cur = 0;
    }
    sqlite3_finalize(stmt);
    sync_dropdown_indices();
}

static void
BuildPresetName(char *out, size_t outsz, const char *film, const char *iso_nom,
                const char *iso_used, const char *developer, const char *dilution)
{
    char full[256];
    size_t len;

    if (outsz == 0)
        return;
    if (!iso_nom || !iso_nom[0]) iso_nom = "?";
    if (!iso_used || !iso_used[0]) iso_used = iso_nom;
    if (strcmp(iso_nom, iso_used) == 0)
        snprintf(full, sizeof(full), "%s %s %s %s", film, iso_nom, developer, dilution);
    else
        snprintf(full, sizeof(full), "%s [%s @ %s] %s %s", film, iso_nom, iso_used, developer, dilution);

    /* obciecie nigdy w srodku znaku UTF-8 */
    len = strlen(full);
    if (len >= outsz) {
        len = outsz - 1;
        while (len > 0 && ((unsigned char) full[len] & 0xC0) == 0x80)
            len--;
    }
    memcpy(out, full, len);
    out[len] = '\0';
}

static void
AutoUpdatePresetName(void)
{
    CountdownTimer *dev = &g_timers[0];

    BuildPresetName(g_preset_name, sizeof(g_preset_name), dev->film_buf, dev->iso_buf,
                    dev->iso_used_buf, dev->dev_name_buf, dev->dilution_buf);
}

static int
hms_to_secs(const CountdownTimer *t)
{
    return (int) strtol(t->hh_buf, NULL, 10) * 3600
         + (int) strtol(t->mm_buf, NULL, 10) * 60
         + (int) strtol(t->ss_buf, NULL, 10);
}

/* ?1 nazwa, ?2..?6 id opcji, ?7..?13 czasy/alarmy - wspolne dla obu zapytan */
static void
BindPreset(sqlite3_stmt *stmt, const char *name, const sqlite3_int64 ids[5])
{
    CountdownTimer *dev = &g_timers[0], *stop = &g_timers[1], *fix = &g_timers[2];
    int i;

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    for (i = 0; i < 5; i++)
        db_bind_id(stmt, 2 + i, ids[i]);
    sqlite3_bind_int(stmt, 7, hms_to_secs(dev));
    sqlite3_bind_int(stmt, 8, (int) strtol(dev->alarm_buf, NULL, 10));
    sqlite3_bind_int(stmt, 9, (int) strtol(dev->alarm_dur_buf, NULL, 10));
    sqlite3_bind_int(stmt, 10, hms_to_secs(stop));
    sqlite3_bind_int(stmt, 11, hms_to_secs(fix));
    sqlite3_bind_int(stmt, 12, (int) strtol(fix->alarm_buf, NULL, 10));
    sqlite3_bind_int(stmt, 13, (int) strtol(fix->alarm_dur_buf, NULL, 10));
}

static int
PresetIdentical(const char *name, const sqlite3_int64 ids[5])
{
    sqlite3_stmt *stmt;
    int same;

    if (sqlite3_prepare_v2(g_db,
            "SELECT 1 FROM presets"
            " WHERE name=?1 COLLATE NOCASE"
            "   AND film_id IS ?2 AND iso_id IS ?3"
            "   AND iso_used_id IS ?4 AND developer_id IS ?5 AND dilution_id IS ?6"
            "   AND dev_time=?7 AND dev_every=?8 AND dev_for=?9"
            "   AND stop_time=?10"
            "   AND fix_time=?11 AND fix_every=?12 AND fix_for=?13",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    BindPreset(stmt, name, ids);
    same = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return same;
}

static void
SavePreset(void)
{
    sqlite3_stmt *stmt;
    CountdownTimer *dev = &g_timers[0];
    char name[PRESET_NAME_LEN];
    sqlite3_int64 ids[5];

    if (AnyTimerRunning()) {
        ShowMessage("Stop all timers before saving.");
        return;
    }
    BuildPresetName(name, sizeof(name), dev->film_buf, dev->iso_buf,
                    dev->iso_used_buf, dev->dev_name_buf, dev->dilution_buf);
    if (name[0] == '\0') {
        snprintf(g_status, sizeof(g_status), "Set film, developer and dilution first.");
        return;
    }
    ids[0] = UpsertOption("film", dev->film_buf);
    ids[1] = UpsertOption("iso", dev->iso_buf);
    ids[2] = UpsertOption("iso", dev->iso_used_buf);
    ids[3] = UpsertOption("developer", dev->dev_name_buf);
    ids[4] = UpsertOption("dilution", dev->dilution_buf);

    if (PresetIdentical(name, ids)) {
        snprintf(g_status, sizeof(g_status), "No changes: %s", name);
        return;
    }
    if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO presets"
            " (id, name, film_id, iso_id, iso_used_id, developer_id, dilution_id,"
            "  dev_time, dev_every, dev_for,"
            "  stop_time,"
            "  fix_time, fix_every, fix_for)"
            " VALUES("
            "  (SELECT id FROM presets WHERE name=?1 COLLATE NOCASE),"
            "  ?1,?2,?3,?4,?5,?6,"
            "  ?7,?8,?9,"
            "  ?10,"
            "  ?11,?12,?13);",
            -1, &stmt, NULL) != SQLITE_OK)
        return;
    BindPreset(stmt, name, ids);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    LoadPresetList();
    snprintf(g_status, sizeof(g_status), "Saved: %s", name);
}

/* -------------------------------------------------------------------- */
/* Workflow (Timer)                                                      */
/* -------------------------------------------------------------------- */

static void
ResetWorkflow(void)
{
    int i;

    for (i = 0; i < TIMER_COUNT; i++)
        CountdownDoReset(&g_timers[i]);
    g_workflow_phase = 0;
}

static void
WorkflowAdvance(void)
{
    CountdownTimer *t;

    if (g_workflow_phase == 6) {
        ResetWorkflow();
        snprintf(g_status, sizeof(g_status), "Ready.");
        return;
    }
    if (g_workflow_phase % 2 != 0)
        return;
    t = &g_timers[g_workflow_phase / 2];
    CountdownDoStart(t);
    if (t->running)
        g_workflow_phase++;
}

/* ms do najblizszego tyku/impulsu alarmu, -1 = nic nie czeka */
static long
NextDeadline(void)
{
    long now = now_ms(), best = -1;
    int i;

    for (i = 0; i < TIMER_COUNT; i++) {
        const CountdownTimer *t = &g_timers[i];

        if (t->running && (best < 0 || t->next_tick_ms < best))
            best = t->next_tick_ms;
        if (t->alarm_pulses_left > 0 && (best < 0 || t->next_alarm_pulse_ms < best))
            best = t->next_alarm_pulse_ms;
    }
    if (best < 0)
        return -1;
    return best > now ? best - now : 0;
}

/* 1 gdy cokolwiek sie zmienilo (trzeba przerysowac) */
static int
ProcessTimers(void)
{
    long now = now_ms();
    int i, changed = 0;

    for (i = 0; i < TIMER_COUNT; i++) {
        CountdownTimer *t = &g_timers[i];

        while (t->running && now >= t->next_tick_ms) {
            t->next_tick_ms += TICK_MS;
            CountdownTick(t);
            changed = 1;
        }
        while (t->alarm_pulses_left > 0 && now >= t->next_alarm_pulse_ms) {
            t->next_alarm_pulse_ms += ALARM_PULSE_MS;
            FireAlarmPulse(t);
        }
    }
    return changed;
}

/* -------------------------------------------------------------------- */
/* UI - wspolne wiersze                                                  */
/* -------------------------------------------------------------------- */

static UiBoxStyle g_style;
static CountdownTimer g_defaults[TIMER_COUNT];  /* Config -> Defaults, tylko HH/MM/SS */

static void
LabelColWidth(UiCtx *ctx)
{
    static const char *labels[] = {
        "Development", "Stop bath", "Dev alarm", "Fix alarm", "Developer",
        "Dilution", "Base time", "Actual temp", "Temperature", "ISO used",
        "Volume ml", "Developers", NULL
    };
    int i, w;

    g_label_w = 0;
    for (i = 0; labels[i]; i++) {
        w = ui_text_width(ctx, labels[i]);
        if (w > g_label_w)
            g_label_w = w;
    }
    g_label_w += 10;
}

/* wiersz z etykieta w lewej kolumnie, zwraca reszte wiersza */
static UiRect
RowLabel(UiCtx *ctx, UiBox *box, const char *label)
{
    UiRect row = ui_box_next_rect(box, ROW_H);
    UiRect l = { row.x, row.y, g_label_w, row.h };
    UiRect rest = { row.x + g_label_w, row.y, row.w - g_label_w, row.h };

    ui_label(ctx, l, label);
    return rest;
}

/* lista rozwijana: pole + lista rysowana NA WIERZCHU w DrawDropdownList
 * (jak priorytet w 7atodo.c) - bez prawdziwego popupu/grabu; dopoki lista
 * jest otwarta, mysz pod nia jest zablokowana (ui_set_mouse_blocked) */
static struct {
    const DynList *dl;  /* NULL = zamknieta */
    int *idx;
    UiRect anchor;
    UiRect list;        /* z ostatniej klatki, dla kolka myszy */
    int scroll;
    int skip_click;     /* klik otwierajacy nie zamyka listy w tej samej klatce */
    int *picked;        /* wlasnie wybrany idx -> Dropdown zwroci 1 */
} g_dd;

static void
DropdownClose(void)
{
    g_dd.dl = NULL;
    g_dd.idx = NULL;
}

static int
Dropdown(UiCtx *ctx, UiRect r, const DynList *dl, int *idx)
{
    UiRect txt = { r.x + 4, r.y, r.w - ARROW_W - 8, r.h };
    UiRect mark = { r.x + r.w - ARROW_W, r.y, ARROW_W, r.h };

    if (*idx < 0 || *idx >= dl->count)
        *idx = 0;
    if (ui_button(ctx, r, "") && dl->count > 0) {
        g_dd.dl = dl;
        g_dd.idx = idx;
        g_dd.anchor = r;
        g_dd.scroll = *idx - DD_MAX_ROWS / 2;
        g_dd.skip_click = 1;
    }
    ui_label_ellipsis(ctx, txt, dl->count > 0 ? dl->items[*idx] : "(empty)");
    ui_label_centered(ctx, mark, "v");
    if (g_dd.picked == idx) {
        g_dd.picked = NULL;
        return 1;
    }
    return 0;
}

static void
DropdownScroll(int delta)
{
    int max = g_dd.dl->count - DD_MAX_ROWS;

    g_dd.scroll += delta;
    if (g_dd.scroll > max)
        g_dd.scroll = max;
    if (g_dd.scroll < 0)
        g_dd.scroll = 0;
}

/* wolane na koncu klatki, po odblokowaniu myszy */
static void
DrawDropdownList(UiCtx *ctx, int win_h)
{
    UiRect list, row, lbl;
    int rows, i, mx, my;

    if (!g_dd.dl)
        return;
    ui_mouse_state(ctx, &mx, &my, NULL);
    rows = g_dd.dl->count < DD_MAX_ROWS ? g_dd.dl->count : DD_MAX_ROWS;
    if (rows <= 0) {
        DropdownClose();
        return;
    }
    DropdownScroll(0);
    list = (UiRect){ g_dd.anchor.x, g_dd.anchor.y + g_dd.anchor.h, g_dd.anchor.w, rows * ROW_H + 2 };
    if (list.y + list.h > win_h)
        list.y = g_dd.anchor.y - list.h;
    if (list.y < 0)
        list.y = 0;
    g_dd.list = list;
    ui_fill_rect(ctx, list, ui_theme_box_bg(ctx));
    ui_draw_border(ctx, list, 1, ui_theme_line_fg(ctx));
    for (i = 0; i < rows; i++) {
        int item = g_dd.scroll + i;

        row = (UiRect){ list.x + 1, list.y + 1 + i * ROW_H, list.w - 2, ROW_H };
        if (mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h)
            ui_fill_rect(ctx, row, ui_theme_accent(ctx));  /* hover, jak ui_button */
        else if (item == *g_dd.idx)
            ui_fill_rect(ctx, row, ui_theme_bar_active_bg(ctx));
        lbl = (UiRect){ row.x + 4, row.y, row.w - 8, row.h };
        ui_label_ellipsis(ctx, lbl, g_dd.dl->items[item]);
        /* klik otwierajacy moze lezec pod lista (lista przesunieta w gore) */
        if (!g_dd.skip_click && ui_hit_test(ctx, row)) {
            *g_dd.idx = item;
            g_dd.picked = g_dd.idx;
            DropdownClose();
            return;
        }
    }
    if (!g_dd.skip_click && ui_hit_test(ctx, (UiRect){ 0, 0, 32767, 32767 }))
        DropdownClose();
    g_dd.skip_click = 0;
}

/* pole liczbowe; Up/Down zmienia wartosc o 1 w zakresie 0..max */
static int
DigitField(UiCtx *ctx, UiRect r, char *buf, int cap, int *cur, long max, int pad)
{
    int delta = ui_textbox_key(ctx, buf, XK_Up) - ui_textbox_key(ctx, buf, XK_Down);

    if (delta) {
        long v = strtol(buf, NULL, 10) + delta;

        if (v < 0) v = 0;
        if (v > max) v = max;
        snprintf(buf, (size_t) cap, pad ? "%02ld" : "%ld", v);
        *cur = (int) strlen(buf);
    }
    return ui_textbox_digits(ctx, r, buf, cap, cur) || delta;
}

static int
FieldWidth(UiCtx *ctx)
{
    return ui_text_width(ctx, "0000") + 10;
}

static void
TimeRow(UiCtx *ctx, UiBox *box, const char *label, CountdownTimer *t)
{
    static const long max[3] = { 999, 59, 59 };
    char *bufs[3] = { t->hh_buf, t->mm_buf, t->ss_buf };
    UiRect r = RowLabel(ctx, box, label);
    int fw = FieldWidth(ctx), sep = ui_text_width(ctx, ":") + 6;
    int i, changed = 0;

    for (i = 0; i < 3; i++) {
        UiRect f = { r.x + i * (fw + sep), r.y, fw, r.h };
        UiRect s = { f.x + fw, r.y, sep, r.h };

        changed |= DigitField(ctx, f, bufs[i], i == 0 ? 4 : 3, &t->hms_cur[i], max[i], 1);
        if (i < 2)
            ui_label_centered(ctx, s, ":");
    }
    /* reczna zmiana czasu wygrywa z przywracaniem czasu przez Reset */
    if (changed && !t->running)
        t->total = 0;
}

static void
AlarmRow(UiCtx *ctx, UiBox *box, const char *label, CountdownTimer *t)
{
    UiRect r = RowLabel(ctx, box, label);
    int fw = FieldWidth(ctx), lw = ui_text_width(ctx, "Every") + 8;
    UiRect l1 = { r.x, r.y, lw, r.h };
    UiRect f1 = { l1.x + lw, r.y, fw, r.h };
    UiRect l2 = { f1.x + fw + 12, r.y, lw, r.h };
    UiRect f2 = { l2.x + lw, r.y, fw, r.h };
    UiRect u = { f2.x + fw + 4, r.y, ui_text_width(ctx, "s") + 4, r.h };

    ui_label(ctx, l1, "Every");
    DigitField(ctx, f1, t->alarm_buf, 5, &t->alarm_cur, 9999, 0);
    ui_label(ctx, l2, "For");
    DigitField(ctx, f2, t->alarm_dur_buf, 4, &t->dur_cur, 999, 0);
    ui_label(ctx, u, "s");
}

/* -------------------------------------------------------------------- */
/* Zakladki                                                              */
/* -------------------------------------------------------------------- */

static void
DrawAlarmMarks(UiCtx *ctx, UiRect r, const CountdownTimer *t, int total)
{
    int interval = (int) strtol(t->alarm_buf, NULL, 10);
    int m;

    if (!t->has_alarm || interval <= 0 || total <= 0 || total / interval > r.w / 3)
        return;
    for (m = interval; m < total; m += interval) {
        int x = r.x + (int) ((long) r.w * (total - m) / total);

        ui_draw_line(ctx, x, r.y, x, r.y + 3, 1, ui_theme_line_fg(ctx));
        ui_draw_line(ctx, x, r.y + r.h - 4, x, r.y + r.h - 1, 1, ui_theme_line_fg(ctx));
    }
}

static void
DrawTimerTab(UiCtx *ctx, UiBox *box)
{
    static const char *wf_labels[] = {
        "Start development", "Developing...", "Start stop bath", "Stop bath...",
        "Start fix", "Fixing...", "Reset"
    };
    CountdownTimer *dev = &g_timers[0];
    char txt[160];
    UiRect r, left, mid, right;
    int i;

    for (i = 0; i < TIMER_COUNT; i++) {
        CountdownTimer *t = &g_timers[i];
        int started = t->running || t->total > 0;
        int secs = started ? t->remaining : hms_to_secs(t);
        int total = started ? t->total : secs;
        double frac = total > 0 ? (double) (total - secs) / total : 0.0;

        r = RowLabel(ctx, box, t->label);
        snprintf(txt, sizeof(txt), "%02d:%02d:%02d", secs / 3600, (secs % 3600) / 60, secs % 60);
        ui_meter(ctx, r, frac, txt);
        DrawAlarmMarks(ctx, r, t, total);
    }

    r = RowLabel(ctx, box, "Recipe");
    if (strcmp(dev->iso_buf, dev->iso_used_buf) == 0)
        snprintf(txt, sizeof(txt), "%s %s, %s %s", dev->film_buf, dev->iso_buf,
                 dev->dev_name_buf, dev->dilution_buf);
    else
        snprintf(txt, sizeof(txt), "%s %s@%s, %s %s", dev->film_buf, dev->iso_buf,
                 dev->iso_used_buf, dev->dev_name_buf, dev->dilution_buf);
    ui_label_ellipsis(ctx, r, txt);
    r = RowLabel(ctx, box, "Temperature");
    snprintf(txt, sizeof(txt), "%s C", g_temp_buf);
    ui_label(ctx, r, txt);

    r = ui_box_next_rect(box, ROW_H + 4);
    ui_rect_split3(r, (r.w - 8) / 2, ui_button_width(ctx, "Stop"), 8, &left, &mid, &right);
    if (ui_button(ctx, left, wf_labels[g_workflow_phase]))
        WorkflowAdvance();
    if (ui_button(ctx, right, "Stop")) {
        ResetWorkflow();
        snprintf(g_status, sizeof(g_status), "Stopped.");
    }
}

static void
DrawPresetList(UiCtx *ctx, UiBox *box)
{
    sqlite3_int64 load_id = -1, del_id = -1;
    UiRect first = { 0, 0, 0, 0 }, row = first;
    int i;

    for (i = 0; i < VISIBLE_PRESETS; i++) {
        int idx = g_preset_scroll + i;
        UiRect name, del;

        row = ui_box_next_rect(box, ROW_H);
        if (i == 0)
            first = row;
        if (idx >= g_preset_count) {
            if (i == 0)
                ui_label(ctx, row, "(no saved presets)");
            continue;
        }
        name = (UiRect){ row.x, row.y, row.w - ARROW_W - 4, row.h };
        del = (UiRect){ row.x + row.w - ARROW_W, row.y, ARROW_W, row.h };
        if (g_presets[idx].id == g_preset_loaded_id)
            ui_fill_rect(ctx, name, ui_theme_bar_active_bg(ctx));
        ui_draw_border(ctx, name, 1, ui_theme_line_fg(ctx));
        name.x += 4;
        name.w -= 8;
        ui_label_ellipsis(ctx, name, g_presets[idx].name);
        if (ui_hit_test(ctx, name))
            load_id = g_presets[idx].id;
        if (ui_button(ctx, del, "x"))
            del_id = g_presets[idx].id;
    }
    g_preset_list_r = (UiRect){ first.x, first.y, first.w, row.y + row.h - first.y };

    /* akcje po petli - DeletePreset przeladowuje g_presets */
    if (del_id >= 0) {
        DeletePreset(del_id);
    } else if (load_id >= 0) {
        if (AnyTimerRunning()) {
            ShowMessage("Stop all timers before loading a preset.");
            return;
        }
        ResetWorkflow();
        LoadPresetIntoTimers(load_id);
        g_preset_loaded_id = load_id;
        snprintf(g_status, sizeof(g_status), "Loaded preset.");
    }
}

static void
DrawDatabaseTab(UiCtx *ctx, UiBox *box)
{
    UiRect r;
    int locked = AnyTimerRunning(), ch = 0;

    DrawPresetList(ctx, box);
    AutoUpdatePresetName();
    r = RowLabel(ctx, box, "Name");
    ui_label_ellipsis(ctx, r, g_preset_name);

    /* przy dzialajacym minutniku pola tylko do odczytu (tyk nadpisuje HH/MM/SS) */
    if (locked)
        ui_set_mouse_blocked(ctx, 1);
    ch |= Dropdown(ctx, RowLabel(ctx, box, "Film"), &g_films, &g_film_idx);
    ch |= Dropdown(ctx, RowLabel(ctx, box, "ISO"), &g_isos, &g_iso_nom_idx);
    ch |= Dropdown(ctx, RowLabel(ctx, box, "ISO used"), &g_isos, &g_iso_used_idx);
    ch |= Dropdown(ctx, RowLabel(ctx, box, "Developer"), &g_devs, &g_dev_idx);
    ch |= Dropdown(ctx, RowLabel(ctx, box, "Dilution"), &g_dils, &g_dil_idx);
    if (ch)
        sync_bufs_from_indices();
    TimeRow(ctx, box, "Development", &g_timers[0]);
    AlarmRow(ctx, box, "Dev alarm", &g_timers[0]);
    TimeRow(ctx, box, "Stop bath", &g_timers[1]);
    TimeRow(ctx, box, "Fix", &g_timers[2]);
    AlarmRow(ctx, box, "Fix alarm", &g_timers[2]);
    if (locked)
        ui_set_mouse_blocked(ctx, g_dd.dl != NULL);

    r = ui_box_next_rect(box, ROW_H + 4);
    r.w = ui_button_width(ctx, "Save preset");
    if (ui_button(ctx, r, "Save preset"))
        SavePreset();
}

static char calc_mm[3] = "09", calc_ss[3] = "30", calc_temp[4] = "24", calc_vol[6] = "500";
static int calc_mm_cur, calc_ss_cur, calc_temp_cur, calc_vol_cur, calc_dil_idx;

/* czas skompensowany: base / 2^((actual - base_temp) / 10) */
static int
CompensatedSecs(void)
{
    double base = strtol(calc_mm, NULL, 10) * 60 + strtol(calc_ss, NULL, 10);
    double diff = (double) (strtol(calc_temp, NULL, 10) - strtol(g_temp_buf, NULL, 10));
    double res = base / pow(2.0, diff / 10.0);

    if (res > 999.0 * 3600.0)
        res = 999.0 * 3600.0;
    return (int) (res + 0.5);
}

static void
DilutionText(char *out, size_t outsz, const char *dil, long vol)
{
    const char *colon = strchr(dil, ':');
    long a, b, stock;

    if (strcasecmp(dil, "Stock") == 0) {
        snprintf(out, outsz, "Stock %ld ml, no water", vol);
        return;
    }
    if (!colon) {
        snprintf(out, outsz, "Unknown dilution");
        return;
    }
    a = strtol(dil, NULL, 10);
    b = strtol(colon + 1, NULL, 10);
    if (a <= 0 || b < 0) {
        snprintf(out, outsz, "Unknown dilution");
        return;
    }
    stock = (vol * a + (a + b) / 2) / (a + b);
    snprintf(out, outsz, "Stock %ld ml + water %ld ml", stock, vol - stock);
}

static void
DrawCalcTab(UiCtx *ctx, UiBox *box)
{
    char txt[96];
    UiRect r, f;
    int fw = FieldWidth(ctx), secs;

    r = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, r, "Temperature compensation");
    r = RowLabel(ctx, box, "Base time");
    f = (UiRect){ r.x, r.y, fw, r.h };
    DigitField(ctx, f, calc_mm, sizeof(calc_mm), &calc_mm_cur, 99, 1);
    f.x += fw + 4;
    f.w = ui_text_width(ctx, ":") + 4;
    ui_label(ctx, f, ":");
    f = (UiRect){ f.x + f.w + 4, r.y, fw, r.h };
    DigitField(ctx, f, calc_ss, sizeof(calc_ss), &calc_ss_cur, 59, 1);

    r = RowLabel(ctx, box, "Base temp");
    snprintf(txt, sizeof(txt), "%s C (Config)", g_temp_buf);
    ui_label(ctx, r, txt);
    r = RowLabel(ctx, box, "Actual temp");
    f = (UiRect){ r.x, r.y, fw, r.h };
    DigitField(ctx, f, calc_temp, sizeof(calc_temp), &calc_temp_cur, 99, 0);

    secs = CompensatedSecs();
    r = RowLabel(ctx, box, "Result");
    snprintf(txt, sizeof(txt), "%02d:%02d:%02d", secs / 3600, (secs % 3600) / 60, secs % 60);
    ui_label(ctx, r, txt);
    r = ui_box_next_rect(box, ROW_H + 4);
    r.w = ui_button_width(ctx, "Use as development time");
    if (ui_button(ctx, r, "Use as development time")) {
        if (AnyTimerRunning()) {
            ShowMessage("Stop all timers before changing times.");
        } else {
            ResetWorkflow();
            SetTimerTime(&g_timers[0], secs);
            snprintf(g_status, sizeof(g_status), "Development time set to %s.", txt);
        }
    }

    ui_box_next_rect(box, ROW_H / 2);
    r = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, r, "Developer dilution");
    Dropdown(ctx, RowLabel(ctx, box, "Dilution"), &g_dils, &calc_dil_idx);
    r = RowLabel(ctx, box, "Volume ml");
    f = (UiRect){ r.x, r.y, fw, r.h };
    DigitField(ctx, f, calc_vol, sizeof(calc_vol), &calc_vol_cur, 99999, 0);
    r = RowLabel(ctx, box, "Mix");
    DilutionText(txt, sizeof(txt), g_dils.count > 0 ? g_dils.items[calc_dil_idx] : "",
                 strtol(calc_vol, NULL, 10));
    ui_label(ctx, r, txt);
}

/* nazwa opcji zmieniona w Config - bufory receptury ida za nia */
static void
RenameInRecipe(const char *old_val, const char *new_val)
{
    CountdownTimer *dev = &g_timers[0];
    char *bufs[] = { dev->film_buf, dev->iso_buf, dev->iso_used_buf,
                     dev->dev_name_buf, dev->dilution_buf };
    size_t sizes[] = { sizeof(dev->film_buf), sizeof(dev->iso_buf), sizeof(dev->iso_used_buf),
                       sizeof(dev->dev_name_buf), sizeof(dev->dilution_buf) };
    size_t i;

    for (i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++)
        if (strcmp(bufs[i], old_val) == 0)
            snprintf(bufs[i], sizes[i], "%s", new_val);
}

static void
OptionAdd(DynList *dl)
{
    if (dl->edit_buf[0] == '\0')
        return;
    SaveOption(dl->category, dl->edit_buf);
    dynlist_add(dl, dl->edit_buf);
    dl->sel = find_option_idx(dl, dl->edit_buf);
    sync_dropdown_indices();
    snprintf(g_status, sizeof(g_status), "Added %s: %s", dl->category, dl->edit_buf);
}

static void
OptionRename(DynList *dl)
{
    char old_val[MAX_OPTION_LEN];

    if (dl->count == 0 || dl->edit_buf[0] == '\0')
        return;
    snprintf(old_val, sizeof(old_val), "%s", dl->items[dl->sel]);
    if (strcmp(old_val, dl->edit_buf) == 0)
        return;
    if (!RenameOption(dl->category, old_val, dl->edit_buf)) {
        snprintf(g_status, sizeof(g_status), "Rename failed (already exists?)");
        return;
    }
    dynlist_delete(dl, dl->sel);
    dynlist_add(dl, dl->edit_buf);
    dl->sel = find_option_idx(dl, dl->edit_buf);
    RenameInRecipe(old_val, dl->edit_buf);
    sync_dropdown_indices();
    snprintf(g_status, sizeof(g_status), "Renamed to %s", dl->edit_buf);
}

static void
OptionDelete(DynList *dl)
{
    if (dl->count == 0)
        return;
    snprintf(g_status, sizeof(g_status), "Deleted %s: %s", dl->category, dl->items[dl->sel]);
    DeleteOption(dl->category, dl->items[dl->sel]);
    dynlist_delete(dl, dl->sel);
    sync_dropdown_indices();
}

static void
OptionEditor(UiCtx *ctx, UiBox *box, const char *label, DynList *dl)
{
    UiRect r = RowLabel(ctx, box, label);
    int bw = ui_button_width(ctx, "Rename"), gap = 4;
    UiRect tb, b;

    if (Dropdown(ctx, r, dl, &dl->sel)) {
        snprintf(dl->edit_buf, sizeof(dl->edit_buf), "%s", dl->items[dl->sel]);
        dl->edit_cur = (int) strlen(dl->edit_buf);
    }
    r = RowLabel(ctx, box, "");
    tb = (UiRect){ r.x, r.y, r.w - 3 * (bw + gap), r.h };
    ui_textbox(ctx, tb, dl->edit_buf, (int) sizeof(dl->edit_buf), &dl->edit_cur);
    b = (UiRect){ tb.x + tb.w + gap, r.y, bw, r.h };
    if (ui_button(ctx, b, "Add"))
        OptionAdd(dl);
    b.x += bw + gap;
    if (ui_button(ctx, b, "Rename"))
        OptionRename(dl);
    b.x += bw + gap;
    if (ui_button(ctx, b, "Delete"))
        OptionDelete(dl);
}

static void
ApplyDefaults(void)
{
    int i;

    if (AnyTimerRunning()) {
        ShowMessage("Stop all timers before applying defaults.");
        return;
    }
    ResetWorkflow();
    for (i = 0; i < TIMER_COUNT; i++)
        SetTimerTime(&g_timers[i], hms_to_secs(&g_defaults[i]));
    snprintf(g_status, sizeof(g_status), "Defaults applied.");
}

static void
DrawConfigTab(UiCtx *ctx, UiBox *box)
{
    UiRect r;
    int i;

    OptionEditor(ctx, box, "Films", &g_films);
    OptionEditor(ctx, box, "Developers", &g_devs);
    OptionEditor(ctx, box, "ISO", &g_isos);
    OptionEditor(ctx, box, "Dilutions", &g_dils);

    r = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, r, "Defaults (not saved in the database)");
    for (i = 0; i < TIMER_COUNT; i++)
        TimeRow(ctx, box, g_defaults[i].label, &g_defaults[i]);
    r = RowLabel(ctx, box, "Temperature");
    r.w = FieldWidth(ctx);
    DigitField(ctx, r, g_temp_buf, 3, &g_temp_cur, 99, 0);
    r = ui_box_next_rect(box, ROW_H + 4);
    r.w = ui_button_width(ctx, "Apply defaults");
    if (ui_button(ctx, r, "Apply defaults"))
        ApplyDefaults();
}

static void
DrawHelpTab(UiCtx *ctx, UiBox *box)
{
    static const char *lines[] = {
        "F1 Timer   F2 Database   F3 Calc   F4 Config",
        "F11 Help   F12 Quit",
        "",
        "Timer: one button runs Development -> Stop bath -> Fix.",
        "Stop aborts the run and restores the set times.",
        "Database: click a preset to load it, x deletes it,",
        "mouse wheel scrolls the list. Save stores the recipe",
        "under a name built from film/ISO/developer/dilution.",
        "Up/Down in a number field changes its value.",
        "",
        "Database ~/.7a/film.db is shared with 7afilm-tui.",
        NULL
    };
    int i;

    for (i = 0; lines[i]; i++)
        ui_label(ctx, ui_box_next_rect(box, ROW_H), lines[i]);
}

/* 1 = wyjscie */
static int
TryQuit(void)
{
    if (AnyTimerRunning()) {
        ShowMessage("Stop all timers before quitting.");
        return 0;
    }
    return 1;
}

static int
DrawQuitTab(UiCtx *ctx, UiBox *box)
{
    UiRect r = ui_box_next_rect(box, ROW_H);

    ui_label(ctx, r, "Quit 7afilm?");
    r = ui_box_next_rect(box, ROW_H + 4);
    r.w = ui_button_width(ctx, "Quit");
    return ui_button(ctx, r, "Quit") && TryQuit();
}

static int
Draw(UiCtx *ctx, int win_w, int win_h)
{
    static const char *tabs[TAB_COUNT] = { "Timer", "Database", "Calc", "Config", "Help", "Quit" };
    UiBox *box = ui_box_begin(ctx, "main", 0, TAB_H, win_w, &g_style);
    UiRect rt = { g_style.margin_l + TAB_INDENT, g_style.margin_t,
                  win_w - g_style.margin_l - g_style.margin_r - 2 * TAB_INDENT, TAB_H };
    UiRect st = { g_style.margin_l, win_h - ROW_H - BOX_MARGIN,
                  win_w - g_style.margin_l - g_style.margin_r, ROW_H };
    int quit = 0, bottom;
    UiRect r;

    ui_set_mouse_blocked(ctx, g_dd.dl != NULL);
    ui_tabs(ctx, rt, tabs, TAB_COUNT, &g_tab, &g_style);
    switch (g_tab) {
    case TAB_TIMER:    DrawTimerTab(ctx, box); break;
    case TAB_DATABASE: DrawDatabaseTab(ctx, box); break;
    case TAB_CALC:     DrawCalcTab(ctx, box); break;
    case TAB_CONFIG:   DrawConfigTab(ctx, box); break;
    case TAB_HELP:     DrawHelpTab(ctx, box); break;
    default:           quit = DrawQuitTab(ctx, box); break;
    }
    /* kazda zakladka: box do samej linii statusu (ta sama wysokosc) */
    r = ui_box_next_rect(box, 0);
    bottom = st.y - BOX_MARGIN - g_style.padding_b - g_style.border_w;
    if (bottom - r.y - ROW_GAP > 0)
        ui_box_next_rect(box, bottom - r.y - ROW_GAP);
    ui_box_end(box);
    ui_set_mouse_blocked(ctx, 0);
    ui_label_ellipsis(ctx, st, g_status);
    DrawDropdownList(ctx, win_h);
    return quit;
}

/* -------------------------------------------------------------------- */
/* main                                                                  */
/* -------------------------------------------------------------------- */

/* jak w utils/demo.c: druga klatka gdy zmienila sie wysokosc boxa albo
 * byl klik (clicked=1) - klik zmienia stan w trakcie klatki, po narysowaniu
 * czesci widgetow (zakladka, preset, pozycja listy rozwijanej), wiec bez
 * drugiej klatki zmiana bylaby widoczna dopiero po ruchu mysza; klik zyje
 * jedna klatke (ui_end_frame), wiec druga klatka niczego nie kliknie */
static int
Render(UiCtx *ctx, int win_w, int win_h, int clicked)
{
    int pass, again = 1, quit = 0;

    for (pass = 0; again && !quit && pass < 2; pass++) {
        int h_before = ui_box_height(ctx, "main");

        ui_begin_frame(ctx);
        quit = Draw(ctx, win_w, win_h);
        ui_end_frame(ctx);
        again = (ui_box_height(ctx, "main") != h_before) || clicked;
    }
    g_dd.picked = NULL;
    return quit;
}

static void
InitStyle(UiCtx *ctx)
{
    int pad = ui_box_padding(ctx);

    memset(&g_style, 0, sizeof(g_style));
    g_style.margin_l = g_style.margin_r = ui_window_margin(ctx);
    g_style.margin_t = g_style.margin_b = BOX_MARGIN;
    g_style.padding_l = g_style.padding_r = pad;
    g_style.padding_t = g_style.padding_b = pad + BOX_PAD_V;
    g_style.border_w = 1;
    g_style.border_color = *ui_theme_line_fg(ctx);
    g_style.bg_color = *ui_theme_box_bg(ctx);
    g_style.gap = ROW_GAP;
}

static int
WindowHeight(void)
{
    return TAB_H + 2 * BOX_MARGIN + g_style.padding_t + g_style.padding_b
         + CONTENT_ROWS * (ROW_H + ROW_GAP) + ROW_H + BOX_MARGIN;
}

static void
LoadAppData(void)
{
    ui_resource_string(g_dpy, "7aFilm.alarmPlayer", "7aFilm.AlarmPlayer",
                       app_data.alarm_player, sizeof(app_data.alarm_player), "");
    ui_resource_string(g_dpy, "7aFilm.alarmSound", "7aFilm.AlarmSound",
                       app_data.alarm_sound, sizeof(app_data.alarm_sound), "");
}

static void
InitData(void)
{
    int i;

    OpenDatabase();
    SeedOptionsIfEmpty();
    dynlist_init(&g_films, "film");
    dynlist_init(&g_isos, "iso");
    dynlist_init(&g_devs, "developer");
    dynlist_init(&g_dils, "dilution");
    LoadOptions();
    LoadPresetList();
    InitTimers();
    sync_dropdown_indices();
    sync_bufs_from_indices();
    calc_dil_idx = g_dil_idx;
    memcpy(g_defaults, g_timers, sizeof(g_defaults));
    for (i = 0; i < TIMER_COUNT; i++)
        g_defaults[i].running = 0;
    snprintf(g_status, sizeof(g_status), "Ready.");
}

/* skroty zakladek (przed ui_feed_event); 1 = obsluzony */
static int
HandleTabKey(XKeyEvent *kev)
{
    KeySym ks = XLookupKeysym(kev, 0);

    if (g_dd.dl && ks == XK_Escape) {
        DropdownClose();
        return 1;
    }
    if ((ks >= XK_F1 && ks <= XK_F4) || ks == XK_F11 || ks == XK_F12)
        DropdownClose();
    switch (ks) {
    case XK_F1:  g_tab = TAB_TIMER; return 1;
    case XK_F2:  g_tab = TAB_DATABASE; return 1;
    case XK_F3:  g_tab = TAB_CALC; return 1;
    case XK_F4:  g_tab = TAB_CONFIG; return 1;
    case XK_F11: g_tab = TAB_HELP; return 1;
    case XK_F12: g_tab = TAB_QUIT; return 1;
    }
    return 0;
}

/* kolko myszy nad otwarta lista rozwijana albo lista presetow; 1 =
 * obsluzony (nie idzie do ui) */
static int
HandleWheel(XButtonEvent *bev)
{
    UiRect r = g_preset_list_r;
    int max = g_preset_count > VISIBLE_PRESETS ? g_preset_count - VISIBLE_PRESETS : 0;

    if (bev->button != Button4 && bev->button != Button5)
        return 0;
    if (g_dd.dl) {
        if (bev->type == ButtonPress)
            DropdownScroll(bev->button == Button4 ? -1 : 1);
        return 1;
    }
    if (bev->type == ButtonPress && g_tab == TAB_DATABASE &&
        bev->x >= r.x && bev->x < r.x + r.w && bev->y >= r.y && bev->y < r.y + r.h) {
        g_preset_scroll += (bev->button == Button4) ? -1 : 1;
        if (g_preset_scroll > max) g_preset_scroll = max;
        if (g_preset_scroll < 0) g_preset_scroll = 0;
    }
    return 1;
}

int
main(int argc, char **argv)
{
    Window win;
    GC gc;
    UiCtx *ctx;
    XEvent ev;
    XClassHint class_hint;
    Atom wm_delete;
    int screen, xfd, win_w = 480, win_h = 400, running = 1;

    (void) argc;
    self_path = argv[0];
    signal(SIGCHLD, SIG_IGN);
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "7afilm: cannot open display\n");
        return 1;
    }
    LoadAppData();
    InitData();

    screen = DefaultScreen(g_dpy);
    win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen), 100, 100, win_w, win_h, 0,
                              BlackPixel(g_dpy, screen), WhitePixel(g_dpy, screen));
    XSelectInput(g_dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                             PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(g_dpy, win, "7afilm");
    class_hint.res_name = "7afilm";
    class_hint.res_class = "7aFilm";
    XSetClassHint(g_dpy, win, &class_hint);
    wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, win, &wm_delete, 1);

    gc = XCreateGC(g_dpy, win, 0, NULL);  /* zwalniane XFreeGC na koncu main */
    ctx = ui_init(g_dpy, win, gc, "-misc-fixed-medium-r-normal--14-*-*-*-*-*-iso10646-1",
                  win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7afilm: ui_init failed (font?)\n");
        XFreeGC(g_dpy, gc);
        XCloseDisplay(g_dpy);
        return 1;
    }
    ui_set_window_icon(ctx, camera_roll_48_width, camera_roll_48_height, camera_roll_48_bits);
    InitStyle(ctx);
    LabelColWidth(ctx);
    win_h = WindowHeight();
    XResizeWindow(g_dpy, win, (unsigned) win_w, (unsigned) win_h);
    {
        /* minimalny rozmiar: przy nizszym oknie box nachodzi na status,
         * a dropdown zaslania wlasne pole (hints na stosie, nic do XFree) */
        XSizeHints hints;

        memset(&hints, 0, sizeof hints);
        hints.flags = PMinSize;
        hints.min_width = win_w;
        hints.min_height = win_h;
        XSetWMNormalHints(g_dpy, win, &hints);
    }
    ui_resize(ctx, win_w, win_h);
    XMapWindow(g_dpy, win);
    Render(ctx, win_w, win_h, 0);

    xfd = ConnectionNumber(g_dpy);
    while (running) {
        int redraw = 0;

        if (!XPending(g_dpy)) {
            long wait = NextDeadline();
            struct timeval tv;
            fd_set fds;

            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            tv.tv_sec = wait / 1000;
            tv.tv_usec = (wait % 1000) * 1000;
            if (select(xfd + 1, &fds, NULL, NULL, wait >= 0 ? &tv : NULL) < 0 && errno != EINTR)
                break;
        }
        if (ProcessTimers())
            redraw = 1;

        while (running && XPending(g_dpy)) {
            int ev_redraw = 0;

            XNextEvent(g_dpy, &ev);
            if (ev.type == KeyPress && HandleTabKey(&ev.xkey)) {
                redraw = 1;
                continue;
            }
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) && HandleWheel(&ev.xbutton)) {
                redraw = 1;
                continue;
            }
            ui_feed_event(ctx, &ev);
            switch (ev.type) {
            case Expose:
            case MotionNotify:
                /* hover/odslona: jedna klatka po oproznieniu kolejki */
                redraw = 1;
                break;
            case ButtonPress:
            case ButtonRelease:
            case KeyPress:
                ev_redraw = 1;
                break;
            case MapNotify:
                XSetInputFocus(g_dpy, win, RevertToParent, CurrentTime);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;
                    ui_resize(ctx, win_w, win_h);
                }
                redraw = 1;
                break;
            case ClientMessage:
                if (ev.xclient.format == 32 &&
                    (Atom) ev.xclient.data.l[0] == wm_delete && TryQuit())
                    running = 0;
                redraw = 1;
                break;
            }
            /* klik zyje jedna klatke - rysowac od razu, nie zbiorczo */
            if (ev_redraw && Render(ctx, win_w, win_h, ev.type == ButtonRelease))
                running = 0;
        }
        if (running && redraw && Render(ctx, win_w, win_h, 0))
            running = 0;
    }

    ui_destroy(ctx);
    XFreeGC(g_dpy, gc);
    XCloseDisplay(g_dpy);
    sqlite3_close(g_db);
    free(g_presets);
    return 0;
}
