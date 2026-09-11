/*
 * 7afilm.c - nowa apka uzytkowa dla fotografii (docelowo rozne pomoce
 * do pracy w ciemni/na planie - na razie jednak wywolywanie filmu).
 *
 * Punkt wyjscia to minutnik (Countdown) przeniesiony w calosci z
 * examples/7atimer.c razem z obsluga alarmu (7atimer ma teraz TYLKO
 * stoper, patrz komentarz na gorze tamtego pliku) - edytowalne pola
 * HH/MM/SS + spinnery +/-, alarm "Every: N sec". Kod widgetow/logiki
 * ponizej to niemal 1:1 kopia z 7atimer.c (ten sam wzorzec select()-owego
 * timera w petli glownej co examples/7aweather.c), zeby nie tracic
 * historii/uzasadnien - patrz TAMTEN plik po pelny opis idiomow
 * (AdjustBuf, ReadAppString, RunAlarmCommand).
 *
 * Zamiast JEDNEGO takiego stopera apka pokazuje TRZY rownolegle, niezalezne
 * sekcje (struct CountdownTimer, tablica g_timers w InitTimers()) - to
 * trzy kolejne kapiele chemiczne w klasycznym procesie wywolywania filmu
 * czarno-bialego: "Development" (rozwijacz), "Stop bath" (kapiel
 * przerywajaca) i "Fix" (utrwalacz), kazda z wlasnym czasem i wlasnym
 * rzadem Start/Stop/Reset - patrz DrawCountdownSection(), ktora rysuje
 * jedna sekcje dla przekazanego CountdownTimer*, oraz petla w draw().
 * Plukanie ("wash") to swiadomie NIE czwarta sekcja - to osobny etap po
 * utrwalaczu, czesto pod biezaca woda bez odmierzania stoperem.
 *
 * Alarm ("Every:"/"For:") jest opcjonalny per-sekcja (CountdownTimer.
 * has_alarm) - Stop bath jest zwykle za krotka, zeby potrzebowac
 * jakiegokolwiek dzwiekowego przypomnienia, wiec ma has_alarm=0 i CALA
 * druga polowa jej boxa (dzwonek + oba pola alarmu) w ogole sie nie
 * rysuje (patrz warunek w DrawCountdownSection). Development/Fix maja DWA
 * niezalezne ustawienia alarmu: "Every:" (okresowe przypomnienie co N
 * sekund w trakcie odliczania) i "For:" (jak dlugo ma trwac koncowy alarm
 * po dobiegnieciu czasu do zera, w sekundach) - to drugie zastapilo
 * poprzednia sztywna stala ALARM_DURATION_S, patrz GetAlarmDurationPulses().
 *
 * Jedna roznica wzgledem oryginalnego ukladu w 7atimer.c: pola jednej
 * sekcji sa w DWOCH wierszach zamiast jednego kompaktowego (HH:MM:SS +
 * spinnery w pierwszym, alarm w drugim, tylko gdy has_alarm) - po
 * dolozeniu pola "For:" caly alarm nie zmiescilby sie juz obok pol czasu
 * w jednym wierszu przy rozsadnej szerokosci okna; win_w tej apki i tak
 * jest szerszy niz w 7atimer.c (patrz win_w w main()), zeby wiersz z
 * czasem sie zmiescil; win_h/min_height rosna proporcjonalnie do trzech
 * sekcji (dwie z nich dwuwierszowe).
 *
 * "Saved settings" (DrawPresetsSection, box NAD trzema sekcjami, bo
 * wczytanie presetu wypelnia pola PONIZEJ) - komplet ustawien wszystkich
 * trzech sekcji zapisywany pod wlasna nazwa do WLASNEJ bazy SQLite
 * (~/.7a/film.db, tabela presets, jeden wiersz = jeden komplet 13 pol
 * czasu/alarmu; wzorzec OpenDatabase/schema identyczny jak w
 * examples/7ashop.c, inna domena, wiec osobny plik bazy - nie dzieli
 * schematu z tasks.db). Klik na wierszu listy NATYCHMIAST wczytuje ten
 * preset do g_timers (LoadPresetIntoTimers) i wpisuje jego nazwe do pola
 * nazwy - dokladnie ten sam UX co "Saved lists" w 7ashop.c (klik = load).
 * Przycisk "Save" przy polu nazwy robi INSERT OR REPLACE pod wpisana
 * nazwa (nowa nazwa = nowy preset, nazwa istniejacego presetu = nadpisanie
 * "w miejscu" - to jest ta "mozliwosc edycji", o ktora poprosil
 * uzytkownik: zmien pola w sekcjach, kliknij Save z ta sama nazwa).
 * Kazdy wiersz ma tez przycisk "x" (usuniecie presetu z bazy), wzorem
 * ListItemRow w 7ashop.c. Lista jest CALA w pamieci (g_presets, dynamiczna
 * tablica realloc*2, ten sam wzorzec co g_catalog w 7ashop.c) i
 * przewijana STALA liczba widocznych wierszy (VISIBLE_PRESETS) + kolko
 * myszy przechwycone w main() PRZED ui_feed_event + znak "v" w naglowku -
 * identyczny wzorzec scrolla jak w 7ashop.c/7askm.c (patrz CLAUDE.md).
 * Save/wczytywanie presetu sa zablokowane, gdy KTORYKOLWIEK z trzech
 * stoperow aktualnie dziala (AnyTimerRunning) - inaczej wczytanie presetu
 * nadpisaloby zywe pole odliczajace w dol pod nosem uzytkownika.
 */

#define _DEFAULT_SOURCE  /* execvp/fork sa POSIX - patrz ta sama uwaga w examples/7aweather.c */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <sqlite3.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20
#define STEP_BTN_W 14
#define TICK_MS 1000
#define ALARM_PULSE_MS 500
#define MAX_ALARM_TOKENS 16
#define ARROW_W 20  /* waski slot na znak "v" w naglowku, patrz examples/7ashop.c */

static Display *g_dpy;

/* Minutnik - bufory sa jedynym zrodlem prawdy o ustawionym czasie, czytane
 * przy KAZDYM Start (dziala tak samo przy swiezym uruchomieniu jak i
 * wznowieniu po Stop) - dokladnie jak w oryginale. Trzy sekcje (Development/
 * Stop bath/Fix - kolejne kapiele chemiczne w procesie wywolywania filmu,
 * patrz g_timers w InitTimers()) maja KAZDA WLASNY, niezalezny komplet pol.
 * Stop bath (kapiel przerywajaca) jest zwykle za krotka, zeby potrzebowac
 * jakiegokolwiek dzwiekowego przypomnienia - has_alarm=0 dla niej wylacza
 * CALA sekcje alarmu (bell/"Every:"/"For:", patrz DrawCountdownSection) I
 * finalny sygnal po dobiegnieciu czasu do zera (patrz guard w StartAlarm).
 * Development/Fix maja wlasny alarm z dwoma niezaleznymi ustawieniami:
 * "Every:" (okresowe przypomnienie co N sekund w trakcie odliczania, np. do
 * agitacji) i "For:" (jak dlugo ma trwac koncowy alarm po dobiegnieciu do
 * zera, w sekundach - poprzednio byla to sztywna stala ALARM_DURATION_S). */
typedef struct {
    const char *label;
    int has_alarm;
    int has_temp;            /* pole stopni Celsjusza obok etykiety (na razie tylko Development) */
    int has_recipe;          /* pola developera/rozcienczenia (na razie tylko Development) */
    char hh_buf[8];
    char mm_buf[8];
    char ss_buf[8];
    char alarm_buf[8];       /* "Every:" - odstep miedzy przypomnieniami (s) */
    char alarm_dur_buf[8];   /* "For:" - dlugosc koncowego alarmu (s) */
    char temp_buf[8];        /* temperatura kapieli w stopniach Celsjusza */
    char dev_name_buf[32];   /* nazwa developera, np. "Kodak D-76" */
    char dilution_buf[24];   /* rozcienczenie, np. "Stock", "1:100", "1:25" - dowolny tekst */
    char film_buf[32];       /* nazwa filmu, np. "Kodak Tri-X 400" */
    char iso_buf[8];         /* ISO uzyte do naswietlenia (moze inne niz box speed - pushing/pulling) */
    int hh_cursor, mm_cursor, ss_cursor, alarm_cursor, alarm_dur_cursor, temp_cursor;
    int dev_name_cursor, dilution_cursor, film_cursor, iso_cursor;
    int remaining;
    int running;
    long next_tick_ms;
    int alarm_pulses_left;
    long next_alarm_pulse_ms;
} CountdownTimer;

#define TIMER_COUNT 3
static CountdownTimer g_timers[TIMER_COUNT];

static void
InitTimers(void)
{
    static const char *labels[TIMER_COUNT] = { "Development", "Stop bath", "Fix" };
    static const int has_alarm[TIMER_COUNT] = { 1, 0, 1 };
    static const int has_temp[TIMER_COUNT] = { 1, 0, 0 };
    static const int has_recipe[TIMER_COUNT] = { 1, 0, 0 };
    int i;

    for (i = 0; i < TIMER_COUNT; i++) {
        CountdownTimer *t = &g_timers[i];

        memset(t, 0, sizeof(*t));
        t->label = labels[i];
        t->has_alarm = has_alarm[i];
        t->has_temp = has_temp[i];
        t->has_recipe = has_recipe[i];
        snprintf(t->hh_buf, sizeof(t->hh_buf), "00");
        snprintf(t->mm_buf, sizeof(t->mm_buf), "09");
        snprintf(t->ss_buf, sizeof(t->ss_buf), "30");
        snprintf(t->alarm_buf, sizeof(t->alarm_buf), "0");
        snprintf(t->alarm_dur_buf, sizeof(t->alarm_dur_buf), "10");
        snprintf(t->temp_buf, sizeof(t->temp_buf), "20");
        snprintf(t->dilution_buf, sizeof(t->dilution_buf), "Stock");
    }
}

typedef struct {
    char alarm_player[128];  /* np. "aplay", "paplay" - pusty = XBell */
    char alarm_sound[256];   /* sciezka do pliku dzwiekowego */
} AppData;

static AppData app_data;

/* -------------------------------------------------------------------- */
/* Baza danych presetow - ~/.7a/film.db, WLASNA (nie tasks.db/shop.db) - */
/* inna domena (zapisane komplety ustawien Development/Stop bath/Fix),  */
/* nie dzieli schematu z zadna inna apka. Wzorzec identyczny jak         */
/* OpenDatabase w examples/7ashop.c.                                     */
/* -------------------------------------------------------------------- */

#define PRESET_NAME_LEN 64

static sqlite3 *g_db;

typedef struct {
    sqlite3_int64 id;
    char name[PRESET_NAME_LEN];
} PresetRow;

static PresetRow *g_presets = NULL;
static int g_preset_count = 0;
static int g_preset_cap = 0;

/* Stan sekcji "Saved settings" - lista presetow jest CALA w pamieci
 * (g_presets), wiec przewijanie/klikanie dziala bez zapytan SQL per
 * klatke (patrz naglowek pliku). g_preset_list_r to rect PIERWSZEGO
 * widocznego wiersza listy z ostatniej klatki - main() porownuje go z
 * pozycja kolka myszy PRZED ui_feed_event, ten sam wzorzec co
 * g_saved_list_r w examples/7ashop.c. */
#define VISIBLE_PRESETS 3
static int g_preset_scroll = 0;
static char g_preset_name_buf[PRESET_NAME_LEN] = "";
static int g_preset_name_cursor = 0;
static UiRect g_preset_list_r;
static sqlite3_int64 g_preset_loaded_id = -1;  /* podswietlenie aktualnie wczytanego wiersza */

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char app_dir[1024];
    char db_path[1040];
    char *errmsg = NULL;

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/film.db", app_dir);

    if (sqlite3_open(db_path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "7afilm: cannot open %s: %s\n", db_path, sqlite3_errmsg(g_db));
        exit(1);
    }

    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    /* Jeden wiersz = jeden zapisany komplet ustawien wszystkich trzech
     * sekcji - plaski schemat (kolumna na kazde pole kazdej sekcji)
     * zamiast normalizacji do osobnej tabeli "sections", bo liczba i
     * tozsamosc sekcji (Development/Stop bath/Fix) jest w tej apce na
     * stale ustalona w kodzie (patrz InitTimers), nie dowolna - dodatkowy
     * JOIN nie dawalby tu nic ponad zlozonosc (KISS, patrz CLAUDE.md).
     * Stop bath nie ma kolumn alarmu (has_alarm=0, patrz CountdownTimer). */
    if (sqlite3_exec(g_db,
            "CREATE TABLE IF NOT EXISTS presets ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            " dev_hh TEXT NOT NULL, dev_mm TEXT NOT NULL, dev_ss TEXT NOT NULL,"
            " dev_every TEXT NOT NULL, dev_for TEXT NOT NULL,"
            " stop_hh TEXT NOT NULL, stop_mm TEXT NOT NULL, stop_ss TEXT NOT NULL,"
            " fix_hh TEXT NOT NULL, fix_mm TEXT NOT NULL, fix_ss TEXT NOT NULL,"
            " fix_every TEXT NOT NULL, fix_for TEXT NOT NULL"
            ");", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "7afilm: schema (presets): %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        exit(1);
    }

    /* Instalacje sprzed dodania pola temperatury maja juz tabele presets
     * bez tej kolumny - CREATE TABLE IF NOT EXISTS wyzej wtedy nic nie
     * zmienia, wiec dogrywamy kolumne przez ALTER TABLE. Blad "duplicate
     * column" (gdy kolumna juz istnieje) jest oczekiwany i celowo
     * ignorowany - ten sam wzorzec co w examples/7atodo.c/7acal.c. */
    sqlite3_exec(g_db, "ALTER TABLE presets ADD COLUMN dev_temp TEXT NOT NULL DEFAULT '20';",
        NULL, NULL, NULL);
    /* Tym samym wzorcem: nazwa developera i rozcienczenie ("Stock", "1:100",
     * "1:25"...) - dowolny tekst, wiec TEXT bez walidacji formatu. */
    sqlite3_exec(g_db, "ALTER TABLE presets ADD COLUMN dev_developer TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE presets ADD COLUMN dev_dilution TEXT NOT NULL DEFAULT 'Stock';",
        NULL, NULL, NULL);
    /* Nazwa filmu i ISO uzyte do naswietlenia - czysto informacyjne (nie
     * wchodza do zadnego obliczenia w tej apce), ale czesc tej samej
     * "recepty" co developer/rozcienczenie/temperatura/czas. */
    sqlite3_exec(g_db, "ALTER TABLE presets ADD COLUMN dev_film TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE presets ADD COLUMN dev_iso TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
}

/* Przeladowuje CALA liste presetow z bazy do pamieci (g_presets) - wolane
 * tylko po akcji uzytkownika (Save/Delete/start apki), nigdy per-klatke,
 * wiec zapytanie SQL tu nie kosztuje - ten sam wzorzec co RunCatalogQuery
 * w examples/7ashop.c. */
static void
LoadPresetList(void)
{
    sqlite3_stmt *stmt;

    g_preset_count = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT id, name FROM presets ORDER BY name COLLATE NOCASE;",
                            -1, &stmt, NULL) != SQLITE_OK)
        return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);

        if (g_preset_count >= g_preset_cap) {
            int new_cap = g_preset_cap ? g_preset_cap * 2 : 16;
            PresetRow *tmp = realloc(g_presets, (size_t) new_cap * sizeof(PresetRow));

            if (!tmp)
                break; /* OOM - konczymy z tym, co juz wczytane */
            g_presets = tmp;
            g_preset_cap = new_cap;
        }
        g_presets[g_preset_count].id = sqlite3_column_int64(stmt, 0);
        snprintf(g_presets[g_preset_count].name, PRESET_NAME_LEN, "%s",
                 name ? (const char *) name : "");
        g_preset_count++;
    }
    sqlite3_finalize(stmt);
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

static char *self_path;  /* argv[0], do znalezienia binarki 7amessage - patrz main() */

/* Szuka binarki 7amessage: najpierw obok wlasnej (przypadek docelowy - ten
 * sam katalog builda w repo 7adesktop), potem w $PATH - ten sam wzorzec co
 * ResolveTodoCommand w examples/7acal.c. */
static void
ResolveMessageCommand(char *out, size_t outsz)
{
    char candidate[1200];
    char dir[1100];
    const char *slash = self_path ? strrchr(self_path, '/') : NULL;

    if (slash) {
        size_t dirlen = (size_t) (slash - self_path);

        if (dirlen >= sizeof(dir))
            dirlen = sizeof(dir) - 1;
        memcpy(dir, self_path, dirlen);
        dir[dirlen] = '\0';

        snprintf(candidate, sizeof(candidate), "%s/7amessage", dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, outsz, "%s", candidate);
            return;
        }
    }

    snprintf(out, outsz, "7amessage");
}

/* Pokazuje komunikat w oknie examples/7amessage.c (fork+execlp, fire-and-
 * forget - SIGCHLD=SIG_IGN w main() sprzata proces potomny) zamiast
 * wypisywac na stdout/rysowac wlasny komunikat w UI - prostsze niz wlasny
 * mechanizm statusu/bledu (np. poprzednio rozwazany wiersz w liscie
 * presetow) i spojne z reszta apek repo (7acal robi to samo, odpalajac
 * 7atodo). Caly tekst idzie jako JEDEN argument - 7amessage i tak laczy
 * wszystkie pozycyjne argv spacja w jeden komunikat (patrz jego main()),
 * a samo sam sobie zawija dlugi tekst (ui_text_width/ui_line_height). */
static void
ShowMessage(const char *text)
{
    char cmd[1200];
    pid_t pid;

    ResolveMessageCommand(cmd, sizeof(cmd));

    pid = fork();
    if (pid == 0) {
        execlp(cmd, cmd, "-name", "7aFilmMessage", "-title", "7afilm", text, (char *) NULL);
        fprintf(stderr, "7afilm: could not run '%s': %s\n", cmd, strerror(errno));
        _exit(127);
    }
}

/* Rozcienczenie ma dwa dopuszczalne ksztalty: "Stock" (bez rozroznienia
 * wielkosci liter) albo "liczba:liczba" (np. "1:100", "1:25") - to
 * jedyne dwa sensowne zapisy rozcienczenia w ciemni, wiec walidacja jest
 * TWARDA (blokuje Save), inaczej niz przy polach liczbowych (tam
 * wystarczy ograniczyc klawisze do cyfr przez ui_textbox_digits - tu
 * nie da sie tego zrobic na poziomie pojedynczego znaku, bo "Stock"
 * miesza litery, a rozcienczenie cyfry i dwukropek). */
static int
IsValidDilution(const char *s)
{
    const char *p = s;
    int digits;

    {
        static const char stock[] = "stock";
        int i;

        for (i = 0; stock[i]; i++) {
            unsigned char c = (unsigned char) p[i];

            if (c >= 'A' && c <= 'Z') c = (unsigned char) (c + 32);
            if (c != (unsigned char) stock[i])
                break;
        }
        if (stock[i] == '\0' && p[i] == '\0')
            return 1;
    }

    digits = 0;
    while (*p >= '0' && *p <= '9') { p++; digits++; }
    if (digits == 0 || *p != ':')
        return 0;
    p++;
    digits = 0;
    while (*p >= '0' && *p <= '9') { p++; digits++; }
    return digits > 0 && *p == '\0';
}

/* INSERT OR REPLACE pod wpisana nazwa - nowa nazwa zaklada nowy preset,
 * nazwa istniejacego nadpisuje go "w miejscu" (to jest edycja presetu, o
 * ktora poprosil uzytkownik: wczytaj, zmien pola w sekcjach, Save pod ta
 * sama nazwa). UNIQUE COLLATE NOCASE w schemacie wymusza to na poziomie
 * bazy, INSERT OR REPLACE po prostu korzysta z tego konfliktu. */
static void
SavePreset(const char *name)
{
    sqlite3_stmt *stmt;
    CountdownTimer *dev = &g_timers[0], *stop = &g_timers[1], *fix = &g_timers[2];

    if (AnyTimerRunning()) {
        ShowMessage("Stop all running timers before saving a preset.");
        return;
    }

    if (name[0] == '\0') {
        ShowMessage("Enter a name for the preset before saving.");
        return;
    }

    if (!IsValidDilution(dev->dilution_buf)) {
        ShowMessage("Dilution must be \"Stock\" or \"N:M\" (e.g. 1:100).");
        return;
    }

    if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO presets"
            " (id, name, dev_hh, dev_mm, dev_ss, dev_every, dev_for, dev_temp,"
            "  dev_developer, dev_dilution, dev_film, dev_iso,"
            "  stop_hh, stop_mm, stop_ss, fix_hh, fix_mm, fix_ss, fix_every, fix_for)"
            " VALUES ((SELECT id FROM presets WHERE name=?1 COLLATE NOCASE),"
            "         ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19);",
            -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dev->hh_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, dev->mm_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, dev->ss_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, dev->alarm_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, dev->alarm_dur_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, dev->temp_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, dev->dev_name_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, dev->dilution_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, dev->film_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, dev->iso_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, stop->hh_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, stop->mm_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, stop->ss_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, fix->hh_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, fix->mm_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, fix->ss_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 18, fix->alarm_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 19, fix->alarm_dur_buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    LoadPresetList();
    g_preset_scroll = 0;
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

    LoadPresetList();
    g_preset_scroll = 0;
}

/* sqlite3_column_text() zwraca const unsigned char* (UTF-8) - jawny rzut
 * na const char* + fallback na "" przy NULL (kolumny sa NOT NULL, ale
 * defensywnie na wypadek uszkodzonego wiersza), zeby snprintf("%s", ...)
 * nie zaleznal od cichej konwersji wskaznikow miedzy signed/unsigned char. */
static const char *
ColumnStr(sqlite3_stmt *stmt, int col)
{
    const unsigned char *s = sqlite3_column_text(stmt, col);

    return s ? (const char *) s : "";
}

/* Wczytuje JEDEN preset (po id) do wszystkich trzech CountdownTimer -
 * kursory pol tekstowych resetowane do 0, bo bufory sa nadpisywane w
 * calosci (ten sam powod co przy Start/Stop gdzie tresc buforow tez sie
 * zmienia poza edycja przez uzytkownika). */
static void
LoadPresetIntoTimers(sqlite3_int64 id)
{
    sqlite3_stmt *stmt;
    CountdownTimer *dev = &g_timers[0], *stop = &g_timers[1], *fix = &g_timers[2];

    if (sqlite3_prepare_v2(g_db,
            "SELECT dev_hh, dev_mm, dev_ss, dev_every, dev_for, dev_temp,"
            "       dev_developer, dev_dilution, dev_film, dev_iso,"
            "       stop_hh, stop_mm, stop_ss, fix_hh, fix_mm, fix_ss, fix_every, fix_for"
            " FROM presets WHERE id=?1;", -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        snprintf(dev->hh_buf, sizeof(dev->hh_buf), "%s", ColumnStr(stmt, 0));
        snprintf(dev->mm_buf, sizeof(dev->mm_buf), "%s", ColumnStr(stmt, 1));
        snprintf(dev->ss_buf, sizeof(dev->ss_buf), "%s", ColumnStr(stmt, 2));
        snprintf(dev->alarm_buf, sizeof(dev->alarm_buf), "%s", ColumnStr(stmt, 3));
        snprintf(dev->alarm_dur_buf, sizeof(dev->alarm_dur_buf), "%s", ColumnStr(stmt, 4));
        snprintf(dev->temp_buf, sizeof(dev->temp_buf), "%s", ColumnStr(stmt, 5));
        snprintf(dev->dev_name_buf, sizeof(dev->dev_name_buf), "%s", ColumnStr(stmt, 6));
        snprintf(dev->dilution_buf, sizeof(dev->dilution_buf), "%s", ColumnStr(stmt, 7));
        snprintf(dev->film_buf, sizeof(dev->film_buf), "%s", ColumnStr(stmt, 8));
        snprintf(dev->iso_buf, sizeof(dev->iso_buf), "%s", ColumnStr(stmt, 9));
        snprintf(stop->hh_buf, sizeof(stop->hh_buf), "%s", ColumnStr(stmt, 10));
        snprintf(stop->mm_buf, sizeof(stop->mm_buf), "%s", ColumnStr(stmt, 11));
        snprintf(stop->ss_buf, sizeof(stop->ss_buf), "%s", ColumnStr(stmt, 12));
        snprintf(fix->hh_buf, sizeof(fix->hh_buf), "%s", ColumnStr(stmt, 13));
        snprintf(fix->mm_buf, sizeof(fix->mm_buf), "%s", ColumnStr(stmt, 14));
        snprintf(fix->ss_buf, sizeof(fix->ss_buf), "%s", ColumnStr(stmt, 15));
        snprintf(fix->alarm_buf, sizeof(fix->alarm_buf), "%s", ColumnStr(stmt, 16));
        snprintf(fix->alarm_dur_buf, sizeof(fix->alarm_dur_buf), "%s", ColumnStr(stmt, 17));

        dev->hh_cursor = dev->mm_cursor = dev->ss_cursor = 0;
        dev->alarm_cursor = dev->alarm_dur_cursor = dev->temp_cursor = 0;
        dev->dev_name_cursor = dev->dilution_cursor = dev->film_cursor = dev->iso_cursor = 0;
        stop->hh_cursor = stop->mm_cursor = stop->ss_cursor = 0;
        fix->hh_cursor = fix->mm_cursor = fix->ss_cursor = 0;
        fix->alarm_cursor = fix->alarm_dur_cursor = 0;
    }
    sqlite3_finalize(stmt);
}

/* -------------------------------------------------------------------- */
/* Zasoby X (alarmPlayer/alarmSound) - czytane bezposrednio przez Xrm,   */
/* ten sam wzorzec co ReadAppString w examples/7atodo.c (konfiguracja    */
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

/* -------------------------------------------------------------------- */
/* Pomoce                                                               */
/* -------------------------------------------------------------------- */

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* AdjustBuf(): zmienia bufor liczbowy o +-1 z zawijaniem na granicach
 * (0->maxval schodzac ponizej zera, maxval->0 przekraczajac gore) - ten
 * sam spinner-idiom co AdjustField() w oryginale 7atimer, tylko na char*
 * zamiast na XtNstring widgetu AsciiText. */
static void
AdjustBuf(char *buf, size_t bufsz, int delta, int maxval)
{
    long val = strtol(buf, NULL, 10);

    val += delta;
    if (val < 0) val = maxval;
    else if (val > maxval) val = 0;
    snprintf(buf, bufsz, "%02ld", val);
}

/* Zamiast (domyslnego) XBell - odpala zewnetrzny programik (alarmPlayer)
 * z plikiem dzwiekowym (alarmSound) jako ostatnim argumentem, gdy oba sa
 * ustawione. fork()+execvp, bez czekania na dziecko (SIGCHLD=SIG_IGN w
 * main()) - ten sam idiom co SpawnCommand w examples/7atodo.c. */
static void
RunAlarmCommand(void)
{
    char player_buf[256];
    char *argv[MAX_ALARM_TOKENS];
    int argc = 0;
    char *tok;
    pid_t pid;

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

    pid = fork();
    if (pid == 0) {
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

/* "For:" - dlugosc koncowego alarmu w sekundach, przeliczona na liczbe
 * pulsow FireAlarmPulse() (co ALARM_PULSE_MS) - co najmniej 1, zeby
 * niepoprawna/pusta wartosc dala pojedynczy sygnal zamiast ciszy. */
static int
GetAlarmDurationPulses(CountdownTimer *t)
{
    long secs = strtol(t->alarm_dur_buf, NULL, 10);
    int pulses;

    if (secs < 0) secs = 0;
    pulses = (int) ((secs * 1000) / ALARM_PULSE_MS);
    if (pulses < 1) pulses = 1;
    return pulses;
}

static void
StartAlarm(CountdownTimer *t)
{
    if (!t->has_alarm)
        return;
    t->alarm_pulses_left = GetAlarmDurationPulses(t);
    FireAlarmPulse(t);
}

static void
StopAlarm(CountdownTimer *t)
{
    t->alarm_pulses_left = 0;
}

/* -------------------------------------------------------------------- */
/* Minutnik                                                             */
/* -------------------------------------------------------------------- */

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
UpdateCountdownFields(CountdownTimer *t)
{
    snprintf(t->hh_buf, sizeof(t->hh_buf), "%02d", t->remaining / 3600);
    snprintf(t->mm_buf, sizeof(t->mm_buf), "%02d", (t->remaining % 3600) / 60);
    snprintf(t->ss_buf, sizeof(t->ss_buf), "%02d", t->remaining % 60);
}

static int
GetAlarmInterval(CountdownTimer *t)
{
    long n = strtol(t->alarm_buf, NULL, 10);

    if (n < 0) n = 0;
    return (int) n;
}

static void
CountdownDoStart(CountdownTimer *t)
{
    if (t->running)
        return;

    ParseCountdownFields(t);
    if (t->remaining <= 0) {
        printf("7afilm: set a %s time greater than zero before starting.\n", t->label);
        fflush(stdout);
        return;
    }
    t->running = 1;
    t->next_tick_ms = now_ms() + TICK_MS;
}

static void
CountdownDoStop(CountdownTimer *t)
{
    t->running = 0;
    StopAlarm(t);
}

static void
CountdownDoReset(CountdownTimer *t)
{
    CountdownDoStop(t);
    t->remaining = 0;
    UpdateCountdownFields(t);
}

/* Wywolywane z petli glownej, co TICK_MS, dopoki t->running */
static void
CountdownTick(CountdownTimer *t)
{
    int interval;

    t->remaining--;
    UpdateCountdownFields(t);

    if (t->remaining <= 0) {
        t->running = 0;
        StartAlarm(t);
        return;
    }

    if (!t->has_alarm)
        return;

    interval = GetAlarmInterval(t);
    if (interval > 0 && t->remaining % interval == 0)
        StartAlarm(t);
}

/* -------------------------------------------------------------------- */
/* Ikona okna - klatka filmu, rysowana wprost Xlibem na 1-bitowej         */
/* Pixmapie, jak w pozostalych portach (WM/taskbar potrzebuje tego        */
/* formatu, nie da sie tu uzyc prymitywow ui.c jak np. przy dzwonku,      */
/* ktory kiedys byl tu rysowany na zywo - zastapiony etykieta "Alarm:" w  */
/* ukladzie dwukolumnowym, patrz DrawCountdownSection). */
/* -------------------------------------------------------------------- */

static void
DrawFilmIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    int i;

    /* obrys tasmy filmowej + dwie przegrody klatek */
    XDrawRectangle(idpy, p, gc, 3, 3, 25, 25);
    XDrawLine(idpy, p, gc, 13, 3, 13, 28);
    XDrawLine(idpy, p, gc, 21, 3, 21, 28);

    /* perforacje wzdluz gornej i dolnej krawedzi */
    for (i = 0; i < 5; i++) {
        int x = 6 + i * 5;
        XFillRectangle(idpy, p, gc, x, 5, 3, 3);
        XFillRectangle(idpy, p, gc, x, 23, 3, 3);
    }
}

static Pixmap
MakeFilmIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawFilmIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}


/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

/* Szerokosc kolumny etykiet - STALA (max ze WSZYSTKICH etykiet uzywanych
 * w ktorejkolwiek z trzech sekcji), zeby lewa krawedz kolumny pol byla w
 * dokladnie tym samym x we wszystkich sekcjach (Development/Stop bath/
 * Fix), nawet gdy dana sekcja nie uzywa najszerszej etykiety (np. Stop
 * bath ma tylko "Time:"). Ukladu "etykiety do prawej, pola do lewej" na
 * prosbe uzytkownika - czytelniejsze niz poprzedni mieszany uklad
 * (dwa pola na wiersz, ikona dzwonka zamiast etykiety). */
static int
LabelColWidth(UiCtx *ctx)
{
    static const char *labels[] = {
        "Temperature:", "Film:", "ISO:", "Developer:", "Dilution:", "Time:", "Alarm:"
    };
    size_t i;
    int max_w = 0;

    for (i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        int w = ui_text_width(ctx, labels[i]);

        if (w > max_w) max_w = w;
    }
    return max_w;
}

/* Etykieta wyrownana do PRAWEJ krawedzi w x=right_x - ui.h ma tylko
 * ui_label (lewo) i ui_label_centered (srodek), wiec zawezamy rect do
 * faktycznej szerokosci tekstu i przesuwamy go pod prawa krawedz, zamiast
 * dokladac trzeci wariant do biblioteki dla jednego, wewnetrznego
 * przypadku uzycia w tej apce. */
static void
DrawLabelRight(UiCtx *ctx, int right_x, int y, int h, const char *text)
{
    int tw = ui_text_width(ctx, text);
    UiRect r = { right_x - tw, y, tw, h };

    ui_label(ctx, r, text);
}

static int
DrawCountdownSection(UiCtx *ctx, int win_w, int y, const UiBoxStyle *style, CountdownTimer *t)
{
    UiBox *box;
    UiRect hdr_row;
    UiRect brow, start_r, stop_r, reset_r;
    int label_col_w, input_x;

    /* Etykieta + pola sekcji to KOLEJNE WIERSZE JEDNEGO boxa (odstepy
     * miedzy nimi to WYLACZNIE style->gap=2, bez recznych y+=N) - Start/
     * Stop/Reset natomiast sa POD boxem, nie w nim (patrz koniec funkcji),
     * na prosbe uzytkownika. Id boxa to t->label - kazda z trzech sekcji
     * ma inna etykiete, wiec to wystarczy jako unikalny klucz cache'a
     * wysokosci (patrz box_cache w ui.c); rozne sekcje moga miec rozna
     * wysokosc boxa (np. Stop bath bez wierszy Temperature/Film/ISO/
     * Developer/Dilution/Alarm), bo cache jest per-id.
     *
     * Ponizej etykiety sekcji kazde pole ma WLASNY wiersz w ukladzie
     * dwukolumnowym: etykieta wyrownana do PRAWEJ krawedzi kolumny
     * (DrawLabelRight), pole zaczynajace sie od tej samej lewej krawedzi
     * (input_x) w KAZDYM wierszu KAZDEJ sekcji (LabelColWidth zwraca
     * jedna, stala szerokosc kolumny) - stad rowne kolumny miedzy
     * Development/Stop bath/Fix, nie tylko wewnatrz jednej sekcji. Alarm
     * nie ma juz osobnej ikonki dzwonka - etykieta "Alarm:" w tej samej
     * kolumnie co reszta, prosciej i spojniej z pozostalymi wierszami. */
    box = ui_box_begin(ctx, t->label, 0, y, win_w, style);

    hdr_row = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, hdr_row, t->label);

    label_col_w = LabelColWidth(ctx);
    input_x = hdr_row.x + label_col_w + 8;

    /* Temperatura - na razie tylko Development (t->has_temp, patrz
     * InitTimers), bo to jedyna kapiel, gdzie uzytkownik prosil o to
     * pole; czas Stop bath/Fix nie zalezy tak silnie od temperatury jak
     * czas wywolywacza. Zapisywana/wczytywana razem z reszta presetu
     * (kolumna dev_temp), patrz SavePreset/LoadPresetIntoTimers. */
    if (t->has_temp) {
        UiRect trow = ui_box_next_rect(box, ROW_H);
        int temp_w = 30;
        int unit_w = ui_text_width(ctx, "\xc2\xb0" "C") + 4;
        UiRect temp_r = { input_x, trow.y, temp_w, trow.h };
        UiRect unit_r = { input_x + temp_w + 4, trow.y, unit_w, trow.h };

        DrawLabelRight(ctx, trow.x + label_col_w, trow.y, trow.h, "Temperature:");
        ui_textbox_digits(ctx, temp_r, t->temp_buf, sizeof(t->temp_buf), &t->temp_cursor);
        ui_label(ctx, unit_r, "\xc2\xb0" "C");
    }

    /* Nazwa filmu + ISO uzyte do naswietlenia + nazwa developera +
     * rozcienczenie - kazde we WLASNYM wierszu (na razie tylko
     * Development, t->has_recipe, patrz InitTimers). ISO osobno od pol
     * czasu, bo to wartosc UZYTA przy naswietlaniu (moze byc inna niz box
     * speed przy push/pull), czysto informacyjna - apka jej nigdzie nie
     * liczy. Rozcienczenie to DOWOLNY tekst (ui_textbox, bez walidacji
     * znak-po-znaku - patrz IsValidDilution wolane przy Save), bo notacja
     * bywa rozna - "Stock", "1:100", "1:25", "Stand 1:100" itp. */
    if (t->has_recipe) {
        UiRect frow = ui_box_next_rect(box, ROW_H);
        UiRect irow = ui_box_next_rect(box, ROW_H);
        UiRect drow = ui_box_next_rect(box, ROW_H);
        UiRect dilrow = ui_box_next_rect(box, ROW_H);
        UiRect film_r = { input_x, frow.y, frow.x + frow.w - input_x, frow.h };
        UiRect iso_r = { input_x, irow.y, 50, irow.h };
        UiRect dev_name_r = { input_x, drow.y, drow.x + drow.w - input_x, drow.h };
        UiRect dil_r = { input_x, dilrow.y, 90, dilrow.h };

        DrawLabelRight(ctx, frow.x + label_col_w, frow.y, frow.h, "Film:");
        ui_textbox(ctx, film_r, t->film_buf, sizeof(t->film_buf), &t->film_cursor);

        DrawLabelRight(ctx, irow.x + label_col_w, irow.y, irow.h, "ISO:");
        ui_textbox_digits(ctx, iso_r, t->iso_buf, sizeof(t->iso_buf), &t->iso_cursor);

        DrawLabelRight(ctx, drow.x + label_col_w, drow.y, drow.h, "Developer:");
        ui_textbox(ctx, dev_name_r, t->dev_name_buf, sizeof(t->dev_name_buf), &t->dev_name_cursor);

        DrawLabelRight(ctx, dilrow.x + label_col_w, dilrow.y, dilrow.h, "Dilution:");
        ui_textbox(ctx, dil_r, t->dilution_buf, sizeof(t->dilution_buf), &t->dilution_cursor);
    }

    /* Time: HH:MM:SS + spinnery, w tym samym ukladzie dwukolumnowym -
     * etykieta "Time:" po lewej, pola zaczynajace sie od input_x. */
    {
        UiRect trow = ui_box_next_rect(box, ROW_H);
        int x = input_x;
        UiRect hh_r, hh_up_r, hh_down_r, colon1_r;
        UiRect mm_r, mm_up_r, mm_down_r, colon2_r;
        UiRect ss_r, ss_up_r, ss_down_r;

        DrawLabelRight(ctx, trow.x + label_col_w, trow.y, trow.h, "Time:");

        hh_r = (UiRect){ x, trow.y, 28, ROW_H }; x += 28 + 2;
        hh_up_r = (UiRect){ x, trow.y, STEP_BTN_W, ROW_H / 2 };
        hh_down_r = (UiRect){ x, trow.y + ROW_H / 2, STEP_BTN_W, ROW_H - ROW_H / 2 }; x += STEP_BTN_W + 2;
        colon1_r = (UiRect){ x, trow.y, 8, ROW_H }; x += 8 + 2;
        mm_r = (UiRect){ x, trow.y, 22, ROW_H }; x += 22 + 2;
        mm_up_r = (UiRect){ x, trow.y, STEP_BTN_W, ROW_H / 2 };
        mm_down_r = (UiRect){ x, trow.y + ROW_H / 2, STEP_BTN_W, ROW_H - ROW_H / 2 }; x += STEP_BTN_W + 2;
        colon2_r = (UiRect){ x, trow.y, 8, ROW_H }; x += 8 + 2;
        ss_r = (UiRect){ x, trow.y, 22, ROW_H }; x += 22 + 2;
        ss_up_r = (UiRect){ x, trow.y, STEP_BTN_W, ROW_H / 2 };
        ss_down_r = (UiRect){ x, trow.y + ROW_H / 2, STEP_BTN_W, ROW_H - ROW_H / 2 };

        ui_label_centered(ctx, colon1_r, ":");
        ui_label_centered(ctx, colon2_r, ":");

        if (!t->running) {
            ui_textbox_digits(ctx, hh_r, t->hh_buf, sizeof(t->hh_buf), &t->hh_cursor);
            if (ui_button(ctx, hh_up_r, "+") || ui_textbox_key(ctx, t->hh_buf, XK_Up))
                AdjustBuf(t->hh_buf, sizeof(t->hh_buf), 1, 999);
            if (ui_button(ctx, hh_down_r, "-") || ui_textbox_key(ctx, t->hh_buf, XK_Down))
                AdjustBuf(t->hh_buf, sizeof(t->hh_buf), -1, 999);

            ui_textbox_digits(ctx, mm_r, t->mm_buf, sizeof(t->mm_buf), &t->mm_cursor);
            if (ui_button(ctx, mm_up_r, "+") || ui_textbox_key(ctx, t->mm_buf, XK_Up))
                AdjustBuf(t->mm_buf, sizeof(t->mm_buf), 1, 59);
            if (ui_button(ctx, mm_down_r, "-") || ui_textbox_key(ctx, t->mm_buf, XK_Down))
                AdjustBuf(t->mm_buf, sizeof(t->mm_buf), -1, 59);

            ui_textbox_digits(ctx, ss_r, t->ss_buf, sizeof(t->ss_buf), &t->ss_cursor);
            if (ui_button(ctx, ss_up_r, "+") || ui_textbox_key(ctx, t->ss_buf, XK_Up))
                AdjustBuf(t->ss_buf, sizeof(t->ss_buf), 1, 59);
            if (ui_button(ctx, ss_down_r, "-") || ui_textbox_key(ctx, t->ss_buf, XK_Down))
                AdjustBuf(t->ss_buf, sizeof(t->ss_buf), -1, 59);
        } else {
            /* pola same odliczaja w dol - tylko-do-odczytu (jak
             * XawtextRead w oryginale), spinnery nieaktywne, wiec wcale
             * nie rysowane. */
            ui_label(ctx, hh_r, t->hh_buf);
            ui_label(ctx, mm_r, t->mm_buf);
            ui_label(ctx, ss_r, t->ss_buf);
        }
    }

    /* Alarm: "Every:"/"For:" w tym samym wierszu, w kolumnie pol (bez
     * osobnej ikonki dzwonka - patrz komentarz na gorze funkcji). */
    if (t->has_alarm) {
        UiRect arow = ui_box_next_rect(box, ROW_H);
        UiRect every_r, alarm_r, sec_r, for_r, dur_r, dur_sec_r;
        int ax = input_x;

        DrawLabelRight(ctx, arow.x + label_col_w, arow.y, arow.h, "Alarm:");

        every_r = (UiRect){ ax, arow.y, 40, ROW_H }; ax += 42;
        alarm_r = (UiRect){ ax, arow.y, 26, ROW_H }; ax += 28;
        sec_r = (UiRect){ ax, arow.y, 24, ROW_H }; ax += 30;
        for_r = (UiRect){ ax, arow.y, 26, ROW_H }; ax += 28;
        dur_r = (UiRect){ ax, arow.y, 26, ROW_H }; ax += 28;
        dur_sec_r = (UiRect){ ax, arow.y, 24, ROW_H };

        ui_label(ctx, every_r, "Every:");
        if (!t->running)
            ui_textbox_digits(ctx, alarm_r, t->alarm_buf, sizeof(t->alarm_buf), &t->alarm_cursor);
        else
            ui_label(ctx, alarm_r, t->alarm_buf);
        ui_label(ctx, sec_r, "sec");

        ui_label(ctx, for_r, "For:");
        if (!t->running)
            ui_textbox_digits(ctx, dur_r, t->alarm_dur_buf, sizeof(t->alarm_dur_buf), &t->alarm_dur_cursor);
        else
            ui_label(ctx, dur_r, t->alarm_dur_buf);
        ui_label(ctx, dur_sec_r, "sec");
    }

    ui_box_end(box);
    y += style->margin_t + ui_box_height(ctx, t->label) + style->margin_b;

    /* Start/Stop/Reset dla TEJ sekcji, POD boxem (nie jako jego ostatni
     * wiersz) - kazda z trzech kapieli ma niezalezny stoper, wiec
     * sterowanie tez musi byc niezalezne (nie jeden wspolny rzad
     * przyciskow jak przy pojedynczym Countdown). BEZ dodatkowego y+=N -
     * "y" juz zawiera margin_b boxa (patrz linia wyzej), co jest jedynym
     * odstepem box->przycisk tez w examples/7atodo.c (content -> Add/
     * Edit/Del: `y += ui_box_height(...) + style.margin_b;` i przyciski
     * rysowane wprost na tym y, bez zadnego dodatkowego marginesu). */
    brow = (UiRect){ style->margin_l, y, win_w - 2 * style->margin_l, ROW_H };
    start_r = ui_rect_col(brow, 0, 3, 6);
    stop_r = ui_rect_col(brow, 1, 3, 6);
    reset_r = ui_rect_col(brow, 2, 3, 6);

    if (ui_button(ctx, start_r, "Start")) CountdownDoStart(t);
    if (ui_button(ctx, stop_r, "Stop")) CountdownDoStop(t);
    if (ui_button(ctx, reset_r, "Reset")) CountdownDoReset(t);
    y += ROW_H;

    return y;
}

/* "Saved settings" - nazwa+Save u gory, ponizej przewijana lista presetow
 * (klik na nazwie = natychmiastowy load do g_timers, "x" = usuniecie z
 * bazy). Patrz naglowek pliku po pelny opis UX i wzorzec scrolla
 * (identyczny jak "Saved lists" w examples/7ashop.c). */
static int
DrawPresetsSection(UiCtx *ctx, int win_w, int y, const UiBoxStyle *style)
{
    UiBox *box;
    UiRect hdr, label_r, arrow_r;
    UiRect namerow, name_r, save_r;
    int save_w;
    int mx, my;
    int i;

    ui_mouse_state(ctx, &mx, &my, NULL);

    /* Naglowek "Saved settings" to PIERWSZY WIERSZ boxa (nie osobna
     * etykieta nad nim) - ten sam wzorzec co naglowek "Catalog (%d/%d)" w
     * boxie "catalog" w examples/7ashop.c. */
    box = ui_box_begin(ctx, "presets", 0, y, win_w, style);

    hdr = ui_box_next_rect(box, ROW_H);
    label_r = (UiRect){ hdr.x, hdr.y, hdr.w - ARROW_W, hdr.h };
    arrow_r = (UiRect){ hdr.x + hdr.w - ARROW_W, hdr.y, ARROW_W, hdr.h };

    ui_label(ctx, label_r, "Saved settings");
    if (g_preset_scroll + VISIBLE_PRESETS < g_preset_count)
        ui_label_centered(ctx, arrow_r, "v");

    namerow = ui_box_next_rect(box, ROW_H);
    save_w = ui_button_width(ctx, "Save");
    name_r = (UiRect){ namerow.x, namerow.y, namerow.w - save_w - 6, namerow.h };
    save_r = (UiRect){ namerow.x + namerow.w - save_w, namerow.y, save_w, namerow.h };

    ui_textbox(ctx, name_r, g_preset_name_buf, sizeof(g_preset_name_buf), &g_preset_name_cursor);
    if (ui_button(ctx, save_r, "Save"))
        SavePreset(g_preset_name_buf);

    for (i = 0; i < VISIBLE_PRESETS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = g_preset_scroll + i;

        if (i == 0)
            g_preset_list_r = (UiRect){ row.x, row.y, row.w,
                                         VISIBLE_PRESETS * ROW_H + (VISIBLE_PRESETS - 1) * style->gap };

        if (idx < g_preset_count) {
            UiRect text_r = { row.x, row.y, row.w - ROW_H - 4, row.h };
            UiRect remove_r = { row.x + row.w - ROW_H, row.y, ROW_H, row.h };
            int hover = mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h;
            const XColor *bg = (g_presets[idx].id == g_preset_loaded_id) ? ui_theme_accent(ctx)
                              : hover ? ui_theme_button_bg(ctx) : NULL;

            if (bg) ui_fill_rect(ctx, row, bg);
            ui_label_ellipsis(ctx, text_r, g_presets[idx].name);
            if (!AnyTimerRunning() && ui_hit_test(ctx, text_r)) {
                LoadPresetIntoTimers(g_presets[idx].id);
                g_preset_loaded_id = g_presets[idx].id;
                snprintf(g_preset_name_buf, sizeof(g_preset_name_buf), "%s", g_presets[idx].name);
                g_preset_name_cursor = (int) strlen(g_preset_name_buf);
            }
            if (ui_button(ctx, remove_r, "x"))
                DeletePreset(g_presets[idx].id);
        } else if (g_preset_count == 0 && i == 0) {
            ui_label(ctx, row, "No saved settings yet");
        }
    }

    ui_box_end(box);
    y += style->margin_t + ui_box_height(ctx, "presets") + style->margin_b;

    return y;
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y = 0;
    int i;

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
        ready = 1;
    }

    /* Kazda sekcja (DrawPresetsSection/DrawCountdownSection) zwraca y TUZ
     * PO swoim calym odcisku (box + ew. wiersz przyciskow pod nim), BEZ
     * zadnego konczacego marginesu - kolejny ui_box_begin sam dolicza
     * swoj style.margin_t, wiec odstep box->przyciski, przyciski->box i
     * krawedz okna->pierwszy box sa RAZEM tym samym, jednym marginesem
     * (6px), identycznie jak header->content w examples/7atodo.c (tam tez
     * kolejny box zaczyna sie na "surowym" y, bez dodatkowego y+=N). */
    y = DrawPresetsSection(ctx, win_w, y, &style);

    for (i = 0; i < TIMER_COUNT; i++)
        y = DrawCountdownSection(ctx, win_w, y, &style, &g_timers[i]);

    (void) win_h;
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
    int win_w = 310, win_h = 545;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    char app_name[64] = "7aFilm";
    char app_title[64] = "";
    XEvent ev;

    self_path = argv[0];
    signal(SIGCHLD, SIG_IGN);
    InitTimers();
    OpenDatabase();
    LoadPresetList();

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
    /* Tylko pledge, bez unveil - jak w examples/7afm.c (patrz komentarz
     * tam): PlayAlarm() fork+execvp'uje 7aFilm.alarmPlayer/alarmSound
     * (X resource, wiec DOWOLNY odtwarzacz/plik od uzytkownika) - unveil
     * dziedziczony po exec by go ograniczyl tak samo jak dowolny opener
     * w 7afm. wpath/cpath/flock dolozone wraz z baza presetow (~/.7a/
     * film.db) - baza w trybie WAL wymaga zapisu do -wal/-shm nawet dla
     * samych SELECT-ow i blokowania flock, ten sam wzorzec co
     * examples/7ashop.c/7askm.c. */
    if (pledge("stdio rpath wpath cpath flock proc exec unix prot_exec", NULL) == -1) {
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

    ReadAppString(dpy, "7aFilm.alarmPlayer", "7aFilm.AlarmPlayer",
                  app_data.alarm_player, sizeof(app_data.alarm_player), "");
    ReadAppString(dpy, "7aFilm.alarmSound", "7aFilm.AlarmSound",
                  app_data.alarm_sound, sizeof(app_data.alarm_sound), "");

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
        ch->res_class = "7aFilm";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    icon = MakeFilmIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 300;
    sizehints->min_height = 536;
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

            /* Kolko myszy (Button4/5) przechwycone TU, PRZED ui_feed_event -
             * ten sam powod co w examples/7ashop.c/7askm.c: ui.c nie
             * rozroznia numeru przycisku myszy, wiec para ButtonPress/
             * Release od kolka zostalaby policzona jak zwykly klik na tym,
             * co akurat jest pod kursorem (np. wczytanie presetu). */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress) {
                    int delta = (ev.xbutton.button == Button4) ? -1 : 1;
                    int px = ev.xbutton.x, py = ev.xbutton.y;

                    if (px >= g_preset_list_r.x && px < g_preset_list_r.x + g_preset_list_r.w &&
                        py >= g_preset_list_r.y && py < g_preset_list_r.y + g_preset_list_r.h) {
                        int max_scroll = g_preset_count - VISIBLE_PRESETS;

                        if (max_scroll < 0) max_scroll = 0;
                        g_preset_scroll += delta;
                        if (g_preset_scroll < 0) g_preset_scroll = 0;
                        if (g_preset_scroll > max_scroll) g_preset_scroll = max_scroll;
                        redraw = 1;
                    }
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

        /* minutnik/puls alarmu KAZDEJ z trzech sekcji - kazdy tickuje
         * niezaleznie, obslugiwane jednym pollingiem zamiast osobnych
         * XtIntervalId (patrz komentarz na gorze pliku). Kazdy sam sobie
         * liczy nastepny "due" czas po odpaleniu. */
        {
            long now = now_ms();
            int ti;

            for (ti = 0; ti < TIMER_COUNT; ti++) {
                CountdownTimer *t = &g_timers[ti];

                if (t->running && now >= t->next_tick_ms) {
                    CountdownTick(t);
                    t->next_tick_ms = now + TICK_MS;
                    redraw = 1;
                }
                if (t->alarm_pulses_left > 0 && now >= t->next_alarm_pulse_ms) {
                    FireAlarmPulse(t);
                    t->next_alarm_pulse_ms = now + ALARM_PULSE_MS;
                    redraw = 1;
                }
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
            long now = now_ms();
            long wake = -1;
            int ti;

            for (ti = 0; ti < TIMER_COUNT; ti++) {
                CountdownTimer *t = &g_timers[ti];

                if (t->running && (wake < 0 || t->next_tick_ms < wake)) wake = t->next_tick_ms;
                if (t->alarm_pulses_left > 0 && (wake < 0 || t->next_alarm_pulse_ms < wake)) wake = t->next_alarm_pulse_ms;
            }

            if (wake < 0) {
                fd_set rfds;
                int xfd = ConnectionNumber(dpy);

                FD_ZERO(&rfds);
                FD_SET(xfd, &rfds);
                select(xfd + 1, &rfds, NULL, NULL, NULL);
            } else {
                long remaining = wake - now;

                if (remaining > 0) {
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
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    sqlite3_close(g_db);
    return 0;
}
