/*
 * 7askm-fetch.c - pobiera dzienny rozklad GTFS SKM Trojmiasto i zapisuje go
 * do bazy SQLite (~/.7a/skm.db), zeby 7askm.c mogl docelowo czytac dane z
 * bazy zamiast wlasnego curl+4x-unzip w petli glownej. Osobna, samodzielna
 * binarka BEZ zaleznosci od X11/ui.c (w duchu xrmlive.c) - ma byc wolana z
 * crona (np. raz dziennie, po 4:00 - patrz uzasadnienie "raz dziennie" w
 * naglowku 7askm.c), nie interaktywnie.
 *
 * Logika pobierania/cache'owania ZIP-a (GetCachePath/FetchGtfsIfStale) i
 * strumieniowego czytania czlonkow ZIP-a przez `unzip -p`
 * (OpenGtfsMember/CsvSplit/StripNewline/ParseGtfsTime), a takze kolejnosc
 * czterech przebiegow filtrowania (stops -> dzisiejsze service_id ->
 * dzisiejsze trips -> stop_times), jest SWIADOMIE skopiowana z 7askm.c, nie
 * wydzielona do wspolnego pliku - to dwie osobne binarki bez wzajemnej
 * zaleznosci (ten sam kompromis co duplikacja RunCommand/ParseHumanKMGT
 * miedzy 7aweather.c/7asensors.c), a logika jest krotka i stabilna (format
 * GTFS SKM). Patrz naglowek 7askm.c po pelne uzasadnienie modelu danych
 * feedu (route_id==service_id==trip_id, calendar_dates.txt jako jedyne
 * zrodlo obowiazywania kursu, itd.) - tu nie powtarzane.
 *
 * Roznica wzgledem LoadDepartures() w 7askm.c: tam filtrowanie po JEDNYM
 * target_stop_id (apka zna tylko swoj przystanek z -stop), tutaj ZADNEGO
 * filtra po stop_id - baza ma sluzyc jako zrodlo dla DOWOLNEGO przystanku
 * sieci, wiec zapisujemy odjazdy wszystkich stacji w dzisiejszym dniu
 * sluzbowym (nadal tylko ~230 kursow/dzien, patrz naglowek 7askm.c).
 *
 * Tabele (schema - patrz OpenDatabase): "stops" (caly, biezacy slownik
 * stacji, upsert co uruchomienie) i "departures" (odjazdy TYLKO dla
 * dzisiejszego service_date - kazde uruchomienie czysci cala tabele przed
 * ponownym wypelnieniem, zeby baza nie rosla bez konca; to samo okno
 * danych co 7askm.c trzyma dzis w pamieci). Kierunek pociagu (Gdynia vs
 * Gdansk) NIE jest tu klasyfikowany - to tania operacja na tekscie
 * headsignu, zostaje po stronie apki odczytujacej (patrz HeadsignIsGdansk
 * w 7askm.c), zeby ta baza byla uzyteczna tez dla innych ewentualnych
 * konsumentow bez wbudowanej wiedzy o klasyfikacji kierunku.
 *
 * Domyslnie CICHY na sukces (tylko bledy na stderr) - standardowa etykieta
 * skryptow crona, zeby MAILTO nie zasypywalo skrzynki co udany bieg; `-v`
 * wypisuje krotkie podsumowanie liczby zaimportowanych wierszy na stdout.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define GTFS_URL "https://www.skm.pkp.pl/gtfs-mi-kpd.zip"
#define LINE_MAX_LEN 512
#define HEADSIGN_LEN 40

#define MAX_TODAY 512  /* ~230 kursow/dzien w calym feedzie (2026-09), zapas x2 - patrz 7askm.c */

static int  g_today_services[MAX_TODAY];
static int  g_today_service_count = 0;

static int  g_today_trip_ids[MAX_TODAY];
static char g_today_trip_headsigns[MAX_TODAY][HEADSIGN_LEN];
static int  g_today_trip_count = 0;

/* -------------------------------------------------------------------- */
/* Male pomoce - string/czas (kopia z 7askm.c, patrz naglowek pliku)      */
/* -------------------------------------------------------------------- */

static void
StripNewline(char *s)
{
    size_t len = strlen(s);

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

static int
IntArrayContains(const int *arr, int n, int val)
{
    int i;

    for (i = 0; i < n; i++)
        if (arr[i] == val) return 1;
    return 0;
}

/* Dzieli line (W MIEJSCU, przecinki -> '\0') na max max_fields tokenow -
 * patrz naglowek 7askm.c o zalozeniach (brak cudzyslowow/przecinkow w
 * polach tego konkretnego feedu). Zwraca liczbe znalezionych pol. */
static int
CsvSplit(char *line, char *fields[], int max_fields)
{
    int n = 0;
    char *p = line;

    if (max_fields <= 0) return 0;
    fields[n++] = p;
    while (n < max_fields) {
        p = strchr(p, ',');
        if (!p) break;
        *p = '\0';
        p++;
        fields[n++] = p;
    }
    return n;
}

/* "HH:MM:SS" -> sekundy od polnocy DNIA SLUZBOWEGO (GTFS pozwala HH>=24 dla
 * kursow po polnocy) - zwraca -1 przy zlym formacie. */
static int
ParseGtfsTime(const char *s)
{
    int h, m, sec;

    if (sscanf(s, "%d:%d:%d", &h, &m, &sec) != 3) return -1;
    if (h < 0 || m < 0 || m > 59 || sec < 0 || sec > 59) return -1;
    return h * 3600 + m * 60 + sec;
}

/* -------------------------------------------------------------------- */
/* Cache pliku GTFS - ~/.7a/skm-gtfs.zip, wspoldzielony z 7askm.c         */
/* -------------------------------------------------------------------- */

static void
GetCachePath(char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    char dir[1024];

    snprintf(dir, sizeof(dir), "%s/.7a", home ? home : ".");
    mkdir(dir, 0700);
    snprintf(out, outsz, "%s/skm-gtfs.zip", dir);
}

static void
FetchGtfsIfStale(const char *cache_path)
{
    struct stat st;
    time_t now = time(NULL);
    struct tm now_tm, file_tm;
    char tmp_path[1400]; /* cache_path (max 1300 z GetCachePath) + ".XXXXXX" + NUL */
    char cmd[1500];
    int fd, status;
    FILE *fp;

    localtime_r(&now, &now_tm);

    if (stat(cache_path, &st) == 0) {
        localtime_r(&st.st_mtime, &file_tm);
        if (file_tm.tm_year == now_tm.tm_year && file_tm.tm_yday == now_tm.tm_yday)
            return; /* juz mamy dzisiejszy plik - SKM publikuje raz/dzien */
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.XXXXXX", cache_path);
    fd = mkstemp(tmp_path);
    if (fd < 0) return;
    close(fd);

    snprintf(cmd, sizeof(cmd), "curl -sL --max-time 60 -o '%s' '%s'", tmp_path, GTFS_URL);
    fp = popen(cmd, "r");
    if (!fp) { unlink(tmp_path); return; }
    status = pclose(fp);

    if (status == 0)
        rename(tmp_path, cache_path);
    else
        unlink(tmp_path);
}

/* Strumieniuje JEDEN plik z wnetrza ZIP-a przez `unzip -p` - patrz naglowek
 * 7askm.c o tym, dlaczego apka nie linkuje wlasnej biblioteki ZIP/inflate. */
static FILE *
OpenGtfsMember(const char *cache_path, const char *member)
{
    char cmd[1500];

    snprintf(cmd, sizeof(cmd), "unzip -p '%s' '%s' 2>/dev/null", cache_path, member);
    return popen(cmd, "r");
}

/* -------------------------------------------------------------------- */
/* Baza danych                                                            */
/* -------------------------------------------------------------------- */

static int
OpenDatabase(sqlite3 **db_out)
{
    const char *home = getenv("HOME");
    char dir[1024], db_path[1200];
    char *errmsg = NULL;
    sqlite3 *db;

    snprintf(dir, sizeof(dir), "%s/.7a", home ? home : ".");
    mkdir(dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/skm.db", dir);

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "7askm-fetch: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS stops ("
            " stop_id INTEGER PRIMARY KEY,"
            " name TEXT NOT NULL"
            ");", NULL, NULL, &errmsg) != SQLITE_OK ||
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS departures ("
            " service_date TEXT NOT NULL,"
            " stop_id INTEGER NOT NULL,"
            " dep_seconds INTEGER NOT NULL,"
            " headsign TEXT NOT NULL"
            ");", NULL, NULL, &errmsg) != SQLITE_OK ||
        sqlite3_exec(db,
            "CREATE INDEX IF NOT EXISTS idx_departures_stop_date "
            "ON departures(stop_id, service_date);", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "7askm-fetch: schema: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return -1;
    }

    *db_out = db;
    return 0;
}

/* Upsert calego slownika stacji - tabela mala (~128 wierszy), wiec bez
 * czyszczenia przed wpisaniem (stara stacja usunieta z feedu zostaje jako
 * nieszkodliwy, martwy wiersz zamiast dodatkowej logiki DELETE). */
static int
ImportStops(sqlite3 *db, const char *cache_path)
{
    FILE *fp = OpenGtfsMember(cache_path, "stops.txt");
    char line[LINE_MAX_LEN];
    sqlite3_stmt *stmt = NULL;
    int first = 1, n = 0;

    if (!fp) return -1;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stops(stop_id, name) VALUES(?1, ?2);",
            -1, &stmt, NULL) != SQLITE_OK) {
        pclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *fields[5];

        StripNewline(line);
        if (first) { first = 0; continue; } /* naglowek CSV */
        if (CsvSplit(line, fields, 5) < 2) continue;

        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, atoi(fields[0]));
        sqlite3_bind_text(stmt, 2, fields[1], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) n++;
    }
    sqlite3_finalize(stmt);
    pclose(fp);
    return n;
}

static int
LoadTodayServices(const char *cache_path, const char *today_str)
{
    FILE *fp = OpenGtfsMember(cache_path, "calendar_dates.txt");
    char line[LINE_MAX_LEN];
    int first = 1;

    if (!fp) return -1;
    g_today_service_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *fields[3];

        StripNewline(line);
        if (first) { first = 0; continue; }
        if (CsvSplit(line, fields, 3) < 3) continue;
        if (strcmp(fields[1], today_str) != 0) continue;
        if (atoi(fields[2]) != 1) continue; /* tylko ADD, patrz naglowek 7askm.c */
        if (g_today_service_count >= MAX_TODAY) break;
        g_today_services[g_today_service_count++] = atoi(fields[0]);
    }
    pclose(fp);
    return g_today_service_count;
}

static int
LoadTodayTrips(const char *cache_path)
{
    FILE *fp = OpenGtfsMember(cache_path, "trips.txt");
    char line[LINE_MAX_LEN];
    int first = 1;

    if (!fp) return -1;
    g_today_trip_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *fields[7];
        int service_id;

        StripNewline(line);
        if (first) { first = 0; continue; }
        if (CsvSplit(line, fields, 7) < 4) continue;
        service_id = atoi(fields[1]);
        if (!IntArrayContains(g_today_services, g_today_service_count, service_id))
            continue;
        if (g_today_trip_count >= MAX_TODAY) break;
        g_today_trip_ids[g_today_trip_count] = atoi(fields[2]);
        snprintf(g_today_trip_headsigns[g_today_trip_count], HEADSIGN_LEN, "%s", fields[3]);
        g_today_trip_count++;
    }
    pclose(fp);
    return g_today_trip_count;
}

/* Czysci cala tabele przed ponownym wypelnieniem - "departures" trzyma
 * WYLACZNIE dzisiejszy service_date (ten sam zakres co 7askm.c w pamieci),
 * wiec przy zmianie dnia stare wiersze maja zniknac, a nie rosnac bez
 * konca. Bez filtra po stop_id (patrz naglowek pliku) - wszystkie stacje
 * naraz. */
static int
ImportDepartures(sqlite3 *db, const char *cache_path, const char *today_str)
{
    FILE *fp = OpenGtfsMember(cache_path, "stop_times.txt");
    char line[LINE_MAX_LEN];
    sqlite3_stmt *stmt = NULL;
    int first = 1, n = 0;

    if (!fp) return -1;

    sqlite3_exec(db, "DELETE FROM departures;", NULL, NULL, NULL);

    if (sqlite3_prepare_v2(db,
            "INSERT INTO departures(service_date, stop_id, dep_seconds, headsign) "
            "VALUES(?1, ?2, ?3, ?4);",
            -1, &stmt, NULL) != SQLITE_OK) {
        pclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *fields[5];
        int trip_id, i;

        StripNewline(line);
        if (first) { first = 0; continue; }
        if (CsvSplit(line, fields, 5) < 5) continue;

        trip_id = atoi(fields[0]);
        for (i = 0; i < g_today_trip_count; i++) {
            int dep;

            if (g_today_trip_ids[i] != trip_id) continue;
            dep = ParseGtfsTime(fields[2]);
            if (dep < 0) break;

            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, today_str, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, atoi(fields[3]));
            sqlite3_bind_int(stmt, 3, dep);
            sqlite3_bind_text(stmt, 4, g_today_trip_headsigns[i], -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_DONE) n++;
            break;
        }
    }
    sqlite3_finalize(stmt);
    pclose(fp);
    return n;
}

int
main(int argc, char **argv)
{
    char cache_path[1300];
    char today_str[16];
    time_t now;
    struct tm now_tm;
    sqlite3 *db;
    int verbose = 0;
    int n_stops, n_deps;
    int i;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            verbose = 1;

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - popen fork+exec'uje curl i unzip, obu
     * potrzebny szeroki dostep do bibliotek/certyfikatow, ktory unveil
     * dziedziczony po exec by okaleczyl (ten sam powod co 7askm.c). */
    if (pledge("stdio rpath wpath cpath flock proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    GetCachePath(cache_path, sizeof(cache_path));
    FetchGtfsIfStale(cache_path);

    now = time(NULL);
    localtime_r(&now, &now_tm);
    strftime(today_str, sizeof(today_str), "%Y%m%d", &now_tm);

    if (OpenDatabase(&db) != 0)
        return 1;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "7askm-fetch: cannot start transaction: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    n_stops = ImportStops(db, cache_path);
    if (n_stops <= 0) {
        fprintf(stderr, "7askm-fetch: could not read stops.txt from %s "
                        "(missing cache/network/unzip?)\n", cache_path);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return 1;
    }

    if (LoadTodayServices(cache_path, today_str) < 0 ||
        LoadTodayTrips(cache_path) < 0) {
        fprintf(stderr, "7askm-fetch: could not read calendar_dates.txt/trips.txt\n");
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return 1;
    }

    n_deps = ImportDepartures(db, cache_path, today_str);
    if (n_deps < 0) {
        fprintf(stderr, "7askm-fetch: could not read stop_times.txt\n");
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return 1;
    }

    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "7askm-fetch: commit failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);

    if (verbose)
        printf("7askm-fetch: %d stops, %d departures for %s\n", n_stops, n_deps, today_str);

    return 0;
}
