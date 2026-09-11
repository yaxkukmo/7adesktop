/*
 * 7askm.c - rozklad odjazdow SKM Trojmiasto (Gdansk/Sopot/Gdynia i dalej)
 * na bibliotece ui.c/ui.h z tego katalogu.
 *
 * Nowa apka (nie port Xt/Xaw - nie ma czego portowac, jak 7arss.c/7acenter.c
 * /7anotify.c/7asys.c). Uklad okna (trzy boxy, wzorem 7aweather.c i
 * 7asys.c):
 *  - "header": TYLKO nazwa przystanku, wysrodkowana, ze strzalkami "<"/">"
 *    po bokach (dokladnie ten sam wzorzec co przewijanie prognozy godzina
 *    po godzinie w 7aweather.c/RenderForecastOffset) - przesuwaja godzine
 *    ODNIESIENIA (g_hour_offset) dla obu list ponizej, wiec strzalka w
 *    prawo pokazuje "co odjezdza za godzine-dwie" zamiast tylko najblizszego
 *    jednego kursu.
 *  - "Towards Gdynia" / "Towards Gdansk": etykieta + tabela 3 kolumn
 *    (Time/In/To - godzina, odliczanie "in N min", stacja docelowa
 *    kursu) z odjazdami w danym kierunku w oknie JEDNEJ GODZINY od
 *    godziny odniesienia (patrz CollectDeparturesInWindow). Box ma STALA
 *    wysokosc - zawsze dokladnie VISIBLE_ROWS wierszy danych - nadmiar
 *    (np. godzina szczytu z 7-8 odjazdami) przewija sie kolkiem myszy nad
 *    danym boxem (g_scroll_gdynia/g_scroll_gdansk, patrz main()), zamiast
 *    rozciagac caly box/okno - tak jak scroll kolkiem w 7amessage.c, tylko
 *    bez samego suwaka (biblioteka i tak nie ma gotowego kontenera do
 *    przewijania, patrz "Brak scrollowalnego kontenera" w CLAUDE.md).
 *    Kierunek: Gdynia = Gdynia/Reda/Rumia/Wejherowo/Lebork (polnoc od
 *    Trojmiasta), Gdansk = Gdansk Glowny/Gdansk Srodmiescie (poludniowy
 *    koniec linii).
 * Caly tekst WIDOCZNY w oknie (etykiety, komunikaty, --help) jest po
 * angielsku, tak jak w reszcie utils/7a*.c (np. monthNames w 7acal.c,
 * "Refresh"/"Add"/"Start" itd. wszedzie indziej) - komentarze w kodzie
 * zostaja po polsku, zgodnie z konwencja calego repo.
 *
 * Zrodlo danych: apka NIE pobiera ani nie parsuje feedu GTFS sama - czyta
 * gotowy, juz przefiltrowany rozklad z SQLite (~/.7a/skm.db, tabele "stops"
 * i "departures"), wypelnianej osobnym narzedziem utils/7askm-fetch.c
 * (samodzielna binarka bez X11, odpalana z crona raz/dzien po 4:00 - patrz
 * naglowek tamtego pliku po pelne uzasadnienie "raz dziennie", model danych
 * feedu GTFS SKM Trojmiasto - route_id==service_id==trip_id, jedyne zrodlo
 * obowiazywania kursu w calendar_dates.txt, itd. - i kolejnosc czterech
 * przebiegow filtrowania stops->service_id->trips->stop_times). Ten sam
 * powod co 7acal.c/7atodo.c dzielace ~/.7a/tasks.db: dwie osobne apki nad
 * jedna baza, zamiast duplikowac curl+4x-unzip+CSV-parsing (drogie,
 * ~23MB/dzien) w KAZDYM uruchomieniu tej apki. Tabela "departures" trzyma
 * JUZ TYLKO dzisiejszy service_date (7askm-fetch czysci i wypelnia ja od
 * nowa co uruchomienie), wiec ta apka nie musi (i NIE POWINNA) znac
 * service_id/trip_id/calendar_dates.txt w ogole - filtruje juz tylko po
 * stop_id + service_date, prostym zapytaniem SQL (patrz LoadDepartures).
 * To STATYCZNY rozklad (planowe godziny odjazdu), NIE dane o opoznieniach
 * w czasie rzeczywistym - SKM nie publikuje takiego API publicznie, wiec
 * "najblizszy odjazd" tutaj znaczy "najblizszy planowy".
 *
 * Format daty service_date w zapytaniach (LoadDepartures) MUSI byc
 * bajt-w-bajt identyczny z tym, co zapisuje 7askm-fetch.c (strftime
 * "%Y%m%d", np. "20260907", BEZ myslnikow) - inaczej WHERE service_date=?
 * nigdy nie trafi w zaden wiersz. Rozjazd formatu miedzy tymi dwoma plikami
 * to cichy bug (pusta lista odjazdow, bez zadnego komunikatu o bledzie).
 *
 * Kierunek pociagu: apka NIE zna geografii trasy (nie parsuje shapes.txt/
 * stop_sequence w obie strony) - klasyfikuje wylacznie po trip_headsign
 * (kolumna "headsign" w tabeli departures): kazdy headsign zaczynajacy sie
 * od "Gdansk"/"GDANSK" (przez ASCII-lowercase pierwszych 3 bajtow, patrz
 * HeadsignIsGdansk) to kierunek Gdanska, wszystko inne (Gdynia/Reda/Rumia/
 * Wejherowo/Lebork - wszystkie konce linii na polnoc od Trojmiasta w tym
 * feedzie) to kierunek Gdyni. To dziala niezaleznie od tego, GDZIE lezy
 * wybrany przystanek wzgledem obu koncow linii, bo headsign zawsze koduje
 * faktyczny cel podrozy pociagu. Klasyfikacja zostaje po stronie apki
 * odczytujacej (nie w bazie) - patrz naglowek 7askm-fetch.c.
 *
 * Ograniczenie: kursy po polnocy w GTFS maja godziny >=24:00:00 (np. 24:10
 * = 00:10 nastepnego dnia kalendarzowego), zapisane w bazie tak jak sa
 * (bez modulo) - CollectDeparturesInWindow liczy to poprawnie w sekundach,
 * wiec dziala tak jak powinno AZ DO PRAWDZIWEJ polnocy (a strzalka w prawo
 * w headerze przestaje dzialac przy probie przejscia poza ok. 27:59:59 -
 * AdjustHourOffset odrzuca taki skok, patrz tam - bo baza nie ma odjazdow
 * na kolejny dzien sluzbowy). Po faktycznej polnocy (kiedy today_str
 * przeskakuje na kolejny dzien) "nocne" kursy naleza juz do WCZORAJSZEGO
 * service_date i znikaja z listy, DOPOKI 7askm-fetch nie doleje
 * dzisiejszego rozkladu (cron, zwykle po 4:00) - swiadomy kompromis (kilka
 * godzin najnizszego ruchu na linii), nie bug do naprawienia "na juz"; box
 * po prostu pokazuje "No departures in the next hour" zamiast bledu.
 *
 * Odswiezanie: RefreshTimetable() to teraz DWA tanie zapytania SQL (stops +
 * departures danego przystanku/dnia) zamiast curl+4x-unzip+parsowanie -
 * wolane raz na starcie i ponownie tylko gdy zmieni sie dzien kalendarzowy
 * (sprawdzane co TICK_MS w main()) albo po "Refresh"/klawiszu 'r' (te dwa
 * ostatnie sa teraz praktycznie darmowe, w przeciwienstwie do poprzedniej
 * wersji tej apki - moga bez obaw byc wolane czesciej niz raz/dzien, gdyby
 * 7askm-fetch kiedys zaczal odswiezac baze czesciej). W miedzyczasie
 * odliczanie "(in X min)" przy kazdym wierszu jest przeliczane co klatke z
 * JUZ zaladowanej listy g_departures (tania operacja w pamieci, bez SQL).
 * Widok "za godzine" (g_hour_offset) przetrwa kazde odswiezenie (reczne
 * albo automatyczne o polnocy) - RefreshTimetable() go nie dotyka.
 *
 * Na OpenBSD: pledge("stdio rpath wpath cpath flock unix prot_exec", NULL), BEZ
 * "proc exec" (w przeciwienstwie do poprzedniej wersji tej apki, ktora
 * fork+exec'owala curl/unzip) - apka juz nigdy nie odpala zadnego procesu
 * potomnego, tylko czyta z SQLite. wpath+cpath sa mimo to potrzebne przez
 * caly czas zycia procesu: skm.db jest w trybie WAL (ustawionym raz przez
 * 7askm-fetch.c), a KAZDE polaczenie do bazy WAL - nawet takie, ktore robi
 * wylacznie SELECT-y - musi miec prawo tworzyc/aktualizowac towarzyszace
 * pliki -wal/-shm w tym samym katalogu. Bez unveil - ten sam powod co w
 * demo.c/7amessage.c (biblioteka Xlib/Xft/fontconfig potrzebuje szerokiego
 * dostepu do bibliotek/fontow/lokalnych baz, ktorego zawezanie nie
 * wniosloby tu realnego zysku bezpieczenstwa wobec i tak juz otwartego
 * "rpath").
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20
#define TICK_MS 20000  /* odswiezanie odliczania "za X min" + kontrola zmiany dnia */

#define STOP_NAME_LEN 48
#define QUERY_LEN 96
#define HEADSIGN_LEN 40

#define MAX_STOPS 128     /* feed ma ~82 przystanki (2026-09) */
#define MAX_DEPARTURES 512
#define MAX_HOUR_ROWS 16  /* zapas ponad zmierzone empirycznie ~7/h na najruchliwszej stacji (Gdansk Wrzeszcz, szczyt poranny) */
#define VISIBLE_ROWS 5    /* stala liczba widocznych wierszy w boxie kierunku - nadmiar przewijany kolkiem myszy, patrz DrawDirectionBox */
#define COL_TIME_W 56     /* kolumna "Time" (HH:MM) */
#define COL_MIN_W 80      /* kolumna "In" (np. "in 60 min") */
#define ARROW_W 20        /* waski slot na prawej krawedzi na znak "v" (jest-wiecej-danych), patrz DrawDirectionBox */

typedef struct {
    int  dep_seconds;
    char headsign[HEADSIGN_LEN];
} Departure;

static sqlite3 *db;

static int  g_stop_ids[MAX_STOPS];
static char g_stop_names[MAX_STOPS][STOP_NAME_LEN];
static int  g_stop_count = 0;

static Departure g_departures[MAX_DEPARTURES];
static int g_departure_count = 0;

static char g_stop_query[QUERY_LEN] = "";
static int  g_target_stop_idx = -1;
static char g_status[160] = "Loading timetable...";
static int  g_last_load_yday = -1;

/* Przesuniecie (w godzinach) godziny ODNIESIENIA dla obu list odjazdow
 * wzgledem aktualnej godziny sciennej - sterowane strzalkami "<"/">" w
 * headerze, dokladnie jak g_offset/RenderForecastOffset w 7aweather.c,
 * tylko ze bez wlasnego bufora godzinowej prognozy - tu przesuwa po prostu
 * okno czasowe [ref, ref+1h) filtrowane na biezaco z g_departures. */
static int g_hour_offset = 0;

/* Przewijanie list odjazdow (patrz VISIBLE_ROWS) - w JEDNOSTKACH WIERSZY,
 * nie pikseli (wszystkie wiersze maja te sama wysokosc ROW_H, wiec nie ma
 * potrzeby przewijania "plynnego"/pikselowego). g_*_list_r to obszar
 * WIDOCZNYCH wierszy danego boxa Z OSTATNIEJ narysowanej klatki, do testu
 * kolka myszy w main() - dokladnie ten sam wzorzec co g_viewport_r w
 * utils/7amessage.c (kolko przechwytywane w petli glownej PRZED
 * ui_feed_event, bo ui.c nie rozroznia numeru przycisku myszy). */
static int g_scroll_gdynia = 0;
static int g_scroll_gdansk = 0;
static UiRect g_gdynia_list_r;
static UiRect g_gdansk_list_r;

typedef struct {
    char row_bg[64]; /* tlo zwyklego wiersza listy - nazwa koloru X11 lub #rrggbb */
} AppData;

static AppData app_data;

/* -------------------------------------------------------------------- */
/* Zasob X (rowBackground) - czytany bezposrednio przez Xrm, ten sam     */
/* wzorzec (i nazwa zasobu) co ReadAppString/7aTodo.rowBackground w      */
/* utils/7atodo.c.                                                    */
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

/* ------------------------------------------------------------------ */
/* Male litery polskich nazw przystankow                               */
/* ------------------------------------------------------------------ */

/* Male litery na bajtach ASCII (tolower) + jawna tabelka dla 9 polskich
 * liter z diakrytykiem, ktore faktycznie wystepuja w stops.txt tego feedu
 * (zweryfikowane empirycznie - Ą/Ć/Ę/Ł/Ń/Ó/Ś/Ź/Ż, kazda jako 2-bajtowa
 * sekwencja UTF-8). Bez tabelki "Gdańsk"/"GDAŃSK" nie sa sobie rowne po
 * "lowerowaniu" (Ń i ń to ROZNE bajty w UTF-8, nie jeden bit odwroceny jak
 * przy ASCII A-Z/a-z) - a to psuje dopasowanie nazwy przystanku dla kazdej
 * stacji z duzej litery, wpisanej przez uzytkownika normalnie z malej (np.
 * "-stop Gdańsk Wrzeszcz" vs dane "GDAŃSK WRZESZCZ"). Pelny unicode case-
 * -folding bylby przewymiarowany (patrz KISS) - ta garstka liter to
 * KOMPLETNY zestaw uzywany w polskich nazwach miejscowosci na tej linii. */
static void
PolishLower(const char *in, char *out, size_t outsz)
{
    static const struct { unsigned char u0, u1, l0, l1; } tbl[] = {
        { 0xC4, 0x84, 0xC4, 0x85 }, /* Ą ą */
        { 0xC4, 0x86, 0xC4, 0x87 }, /* Ć ć */
        { 0xC4, 0x98, 0xC4, 0x99 }, /* Ę ę */
        { 0xC5, 0x81, 0xC5, 0x82 }, /* Ł ł */
        { 0xC5, 0x83, 0xC5, 0x84 }, /* Ń ń */
        { 0xC3, 0x93, 0xC3, 0xB3 }, /* Ó ó */
        { 0xC5, 0x9A, 0xC5, 0x9B }, /* Ś ś */
        { 0xC5, 0xB9, 0xC5, 0xBA }, /* Ź ź */
        { 0xC5, 0xBB, 0xC5, 0xBC }, /* Ż ż */
    };
    size_t inlen = strlen(in);
    size_t ii = 0, oi = 0;

    while (ii < inlen && oi + 1 < outsz) {
        unsigned char c0 = (unsigned char) in[ii];

        if (c0 < 0x80) {
            out[oi++] = (char) tolower(c0);
            ii++;
        } else if ((c0 & 0xE0) == 0xC0 && ii + 1 < inlen && oi + 2 < outsz) {
            unsigned char c1 = (unsigned char) in[ii + 1];
            size_t t;

            for (t = 0; t < sizeof(tbl) / sizeof(tbl[0]); t++) {
                if (tbl[t].u0 == c0 && tbl[t].u1 == c1) {
                    c0 = tbl[t].l0;
                    c1 = tbl[t].l1;
                    break;
                }
            }
            out[oi++] = (char) c0;
            out[oi++] = (char) c1;
            ii += 2;
        } else {
            out[oi++] = (char) c0;
            ii++;
        }
    }
    out[oi] = '\0';
}

/* -------------------------------------------------------------------- */
/* Baza danych - ~/.7a/skm.db, wypelniana przez utils/7askm-fetch.c   */
/* (patrz naglowek pliku). Ta apka NIGDY nie zapisuje do tabel           */
/* stops/departures - tylko SELECT (LoadStops/LoadDepartures nizej), a   */
/* schema jest wlasnoscia 7askm-fetch.c (nie tworzymy jej tutaj).        */
/* -------------------------------------------------------------------- */

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char app_dir[1024];
    char db_path[1200];

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/skm.db", app_dir);

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "7askm: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
}

/* Caly slownik stacji (tabela mala, ~128 wierszy - patrz MAX_STOPS) - brak
 * tabeli "stops" (np. 7askm-fetch jeszcze nigdy nie uruchomiony) sprawia,
 * ze prepare_v2 zawiedzie i funkcja zwroci -1, tak samo jak dawny blad
 * sieci/unzip w poprzedniej wersji tej apki - patrz RefreshTimetable. */
static int
LoadStops(void)
{
    sqlite3_stmt *stmt;

    g_stop_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT stop_id, name FROM stops;", -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    while (g_stop_count < MAX_STOPS && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);

        g_stop_ids[g_stop_count] = sqlite3_column_int(stmt, 0);
        snprintf(g_stop_names[g_stop_count], STOP_NAME_LEN, "%s", name ? (const char *) name : "");
        g_stop_count++;
    }
    sqlite3_finalize(stmt);
    return g_stop_count;
}

/* Dokladne dopasowanie (case-insensitive ASCII) ma pierwszenstwo nad
 * podciagiem - zeby np. "-stop Gdansk Glowny" nie trafil przypadkiem w
 * "Gdansk Glowny Kolonia", gdyby taki przystanek kiedys sie pojawil. */
static int
FindStop(const char *query)
{
    char q_lower[STOP_NAME_LEN];
    int i, substr_match = -1;

    PolishLower(query, q_lower, sizeof(q_lower));
    for (i = 0; i < g_stop_count; i++) {
        char name_lower[STOP_NAME_LEN];

        PolishLower(g_stop_names[i], name_lower, sizeof(name_lower));
        if (strcmp(name_lower, q_lower) == 0) return i;
        if (substr_match < 0 && strstr(name_lower, q_lower)) substr_match = i;
    }
    return substr_match;
}

/* Odjazdy DANEGO przystanku w DZISIEJSZYM service_date - filtr po obu
 * kolumnach naraz trafia w idx_departures_stop_date (patrz OpenDatabase w
 * 7askm-fetch.c), wiec to jedno tanie zapytanie zamiast skanowania calego
 * stop_times.txt jak w poprzedniej wersji tej apki. Bez filtrowania po
 * service_id/trip_id - to juz zrobil 7askm-fetch przy imporcie, patrz
 * naglowek pliku. */
static int
LoadDepartures(int target_stop_id, const char *today_str)
{
    sqlite3_stmt *stmt;

    g_departure_count = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT dep_seconds, headsign FROM departures "
            "WHERE stop_id = ?1 AND service_date = ?2;",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, target_stop_id);
    sqlite3_bind_text(stmt, 2, today_str, -1, SQLITE_STATIC);

    while (g_departure_count < MAX_DEPARTURES && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *headsign = sqlite3_column_text(stmt, 1);

        g_departures[g_departure_count].dep_seconds = sqlite3_column_int(stmt, 0);
        snprintf(g_departures[g_departure_count].headsign, HEADSIGN_LEN, "%s",
                 headsign ? (const char *) headsign : "");
        g_departure_count++;
    }
    sqlite3_finalize(stmt);
    return g_departure_count;
}

/* Patrz naglowek pliku - klasyfikacja WYLACZNIE po tekscie headsignu. */
static int
HeadsignIsGdansk(const char *headsign)
{
    char lower[HEADSIGN_LEN];

    PolishLower(headsign, lower, sizeof(lower));
    return strstr(lower, "gda") != NULL;
}

/* Aktualna godzina scienna w sekundach od polnocy (bez g_hour_offset). */
static int
NowSeconds(void)
{
    time_t now = time(NULL);
    struct tm now_tm;

    localtime_r(&now, &now_tm);
    return now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
}

/* Strzalki "<"/">" w headerze - patrz g_hour_offset. Odrzuca (no-op) skok,
 * ktory wyprowadzilby godzine odniesienia poza zakres pokryty przez
 * g_departures (0..27:59:59, patrz ograniczenie w naglowku pliku o kursach
 * po polnocy) - baza nie ma zaladowanego rozkladu na INNY dzien sluzbowy,
 * wiec nie ma czego pokazac poza tym oknem. */
static void
AdjustHourOffset(int delta)
{
    int new_offset = g_hour_offset + delta;
    int ref_sec = NowSeconds() + new_offset * 3600;

    if (ref_sec < 0 || ref_sec > 27 * 3600 + 3599) return;
    g_hour_offset = new_offset;
}

typedef struct {
    int dep_seconds;
    const char *headsign; /* wskaznik w glab g_departures[].headsign, bez kopiowania */
} HourRow;

/* Wszystkie odjazdy w zadanym kierunku z okna [ref_sec, ref_sec+3600),
 * posortowane rosnaco po czasie (departures nie jest posortowana po
 * czasie, tylko po kolejnosci importu - stad insertion sort na koncu; n
 * jest male, patrz MAX_HOUR_ROWS). Zwraca liczbe znalezionych wierszy
 * (<= max_out). */
static int
CollectDeparturesInWindow(int want_gdansk, int ref_sec, HourRow *out, int max_out)
{
    int n = 0, i, j;

    for (i = 0; i < g_departure_count && n < max_out; i++) {
        int dep = g_departures[i].dep_seconds;

        if (HeadsignIsGdansk(g_departures[i].headsign) != want_gdansk) continue;
        if (dep < ref_sec || dep >= ref_sec + 3600) continue;
        out[n].dep_seconds = dep;
        out[n].headsign = g_departures[i].headsign;
        n++;
    }
    for (i = 1; i < n; i++) {
        HourRow key = out[i];

        j = i - 1;
        while (j >= 0 && out[j].dep_seconds > key.dep_seconds) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

static void
RefreshTimetable(void)
{
    char today_str[16];
    time_t now = time(NULL);
    struct tm now_tm;

    localtime_r(&now, &now_tm);
    /* "%Y%m%d", np. "20260907" - MUSI byc bajt-w-bajt zgodne z formatem
     * zapisywanym przez 7askm-fetch.c, patrz naglowek pliku. */
    strftime(today_str, sizeof(today_str), "%Y%m%d", &now_tm);
    g_last_load_yday = now_tm.tm_yday;

    if (LoadStops() <= 0) {
        /* Krotkie z rozmyslem - to trafia do wysrodkowanego slotu w
         * headerze (miedzy strzalkami "<"/">"), patrz draw(); dluzszy
         * tekst nie miescilby sie tam bez ui_label_ellipsis, ktorego
         * ui_label_centered nie ma. Najczestsza przyczyna: 7askm-fetch
         * jeszcze ani razu nie uruchomiony (baza pusta/nie istnieje). */
        snprintf(g_status, sizeof(g_status), "Error: no data (run 7askm-fetch)");
        g_target_stop_idx = -1;
        return;
    }

    g_target_stop_idx = FindStop(g_stop_query);
    if (g_target_stop_idx < 0) {
        snprintf(g_status, sizeof(g_status), "Stop not found: \"%s\"", g_stop_query);
        return;
    }

    LoadDepartures(g_stop_ids[g_target_stop_idx], today_str);
    g_status[0] = '\0';
}

/* -------------------------------------------------------------------- */
/* Ikona okna - stylizowany pociag, ten sam wzorzec 1-bitowej Pixmapy co  */
/* MakeSysIconPixmap w utils/7asys.c.                                  */
/* -------------------------------------------------------------------- */

static Pixmap
MakeSkmIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    XFillRectangle(idpy, icon, gc, 3, 9, 22, 13);
    XFillArc(idpy, icon, gc, 19, 9, 12, 13, -90 * 64, 180 * 64);
    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 6, 12, 6, 6);
    XFillRectangle(idpy, icon, gc, 15, 12, 6, 6);
    XSetForeground(idpy, gc, 1);
    XFillArc(idpy, icon, gc, 6, 22, 6, 6, 0, 360 * 64);
    XFillArc(idpy, icon, gc, 18, 22, 6, 6, 0, 360 * 64);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                             */
/* -------------------------------------------------------------------- */

/* Cztery kolumny w r - dzielone na STALE szerokosci (nie ui_rect_col, bo
 * kolumny maja z natury rozna tresc: HH:MM jest zawsze tej samej dlugosci,
 * "in NN min" prawie zawsze, nazwa stacji docelowej dostaje cala reszte,
 * a arrow_r to waski slot na prawej krawedzi na wskaznik "v" - patrz
 * DOWN_ARROW nizej). arrow_r jest wyliczany identycznie dla KAZDEGO wiersza
 * (nie tylko naglowka), zeby kolumna "To" miala jednakowa szerokosc w
 * calym boxie, mimo ze sam znak "v" rysowany jest tylko w jednym miejscu. */
static void
SplitColumns(UiRect r, UiRect *time_r, UiRect *min_r, UiRect *dest_r, UiRect *arrow_r)
{
    int dest_w = r.w - COL_TIME_W - COL_MIN_W - ARROW_W;

    *time_r  = (UiRect){ r.x, r.y, COL_TIME_W, r.h };
    *min_r   = (UiRect){ r.x + COL_TIME_W, r.y, COL_MIN_W, r.h };
    *dest_r  = (UiRect){ r.x + COL_TIME_W + COL_MIN_W, r.y, dest_w, r.h };
    *arrow_r = (UiRect){ r.x + r.w - ARROW_W, r.y, ARROW_W, r.h };
}

/* Jeden box na kierunek: naglowek kolumn (Time/In/To, + znak "v" w prawym
 * gornym rogu, gdy ponizej widocznych wierszy jest jeszcze wiecej danych -
 * patrz DOWN_ARROW) + DOKLADNIE VISIBLE_ROWS wierszy danych - stala
 * wysokosc boxa NIEZALEZNIE od liczby odjazdow w tej godzinie (puste
 * sloty, gdy danych jest mniej niz VISIBLE_ROWS), nadmiar przewijany
 * kolkiem myszy przez *scroll (w jednostkach wierszy - patrz
 * g_scroll_gdynia/gdansk). Etykieta kierunku ("Towards Gdynia" itp.) jest
 * rysowana PRZED wywolaniem tej funkcji, POZA boxem (patrz draw() - ten
 * sam wzorzec co sekcje w utils/7asys.c). *out_list_r dostaje obszar
 * WIDOCZNYCH wierszy danych z tej klatki - do testu kolka myszy w main()
 * (patrz g_viewport_r w utils/7amessage.c). Zwraca y ZA tym boxem
 * (uwzglednia juz style->margin_b). */
static int
DrawDirectionBox(UiCtx *ctx, const char *box_id, const UiBoxStyle *style,
                  int win_w, int y, int want_gdansk, int ref_sec, int now_sec,
                  int *scroll, UiRect *out_list_r)
{
    static XColor row_bg;
    static int row_bg_ready = 0;
    HourRow rows[MAX_HOUR_ROWS];
    UiBox *box;
    int n, i, max_scroll;

    if (!row_bg_ready) {
        ui_color(ctx, app_data.row_bg, &row_bg);
        row_bg_ready = 1;
    }

    n = CollectDeparturesInWindow(want_gdansk, ref_sec, rows, MAX_HOUR_ROWS);

    max_scroll = n - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (*scroll > max_scroll) *scroll = max_scroll;
    if (*scroll < 0) *scroll = 0;

    /* BEZ triku "y - style->margin_t" (patrz taki trik przy boxach
     * naslepujacych PO INNYM BOXIE w draw()) - poprzedzajacy element to tu
     * zawsze zwykla etykieta (ui_label), nie box, wiec nie ma czyjegos
     * margin_b do znoszenia; margin_t dolozony przez ui_box_begin (outer_y
     * = y+margin_t, patrz ui.c) daje dodatkowy odstep NAD explicitnym "y +=
     * ROW_H + 4" po etykiecie w draw() - dokladnie ten sam wzorzec co
     * "Uptime / Load" -> box "uptime" w utils/7asys.c. */
    box = ui_box_begin(ctx, box_id, 0, y, win_w, style);

    {
        UiRect hdr = ui_box_next_rect(box, ROW_H);
        UiRect time_r, min_r, dest_r, arrow_r;

        SplitColumns(hdr, &time_r, &min_r, &dest_r, &arrow_r);
        ui_label(ctx, time_r, "Time");
        ui_label(ctx, min_r, "In");
        ui_label(ctx, dest_r, "To");
        /* Jedyny sygnal, ze jest wiecej danych PONIZEJ widocznych wierszy -
         * bez niego uzytkownik nie ma jak sie domyslic, ze box w ogole da
         * sie przewinac (patrz tez g_scroll_gdynia/gdansk). Litera "v", nie
         * "▼" - ten sam wzorzec co strzalki gora/dol w utils/7amessage.c
         * (pewne pokrycie w kazdym foncie, bez polegania na glifie Unicode,
         * ktorego bitmapowy fallback fontu moze nie miec). */
        if (*scroll + VISIBLE_ROWS < n)
            ui_label_centered(ctx, arrow_r, "v");
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = *scroll + i;

        if (i == 0 && out_list_r)
            *out_list_r = (UiRect){ row.x, row.y, row.w,
                                     VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap };

        if (n == 0) {
            if (i == 0) ui_label(ctx, row, "No departures in the next hour");
        } else if (idx < n) {
            UiRect time_r, min_r, dest_r, arrow_r;
            char tbuf[8], mbuf[20];
            int h = (rows[idx].dep_seconds / 3600) % 24; /* normalizacja kursow po polnocy (>=24:00:00) */
            int m = (rows[idx].dep_seconds / 60) % 60;
            int diff_min = (rows[idx].dep_seconds - now_sec + 59) / 60; /* zaokraglenie w gore */

            if (diff_min < 0) diff_min = 0;
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, m);
            snprintf(mbuf, sizeof(mbuf), "in %d min", diff_min);

            ui_fill_rect(ctx, row, &row_bg);
            SplitColumns(row, &time_r, &min_r, &dest_r, &arrow_r);
            ui_label(ctx, time_r, tbuf);
            ui_label(ctx, min_r, mbuf);
            ui_label_ellipsis(ctx, dest_r, rows[idx].headsign);
        }
        /* else: pusty slot - stala wysokosc boxa niezaleznie od liczby danych */
    }

    ui_box_end(box);
    /* +margin_t, bo box_begin powyzej go DOLOZYL do y (patrz komentarz tam)
     * - ten sam wzorzec co po "header" w draw(). */
    return y + style->margin_t + ui_box_height(ctx, box_id) + style->margin_b;
}

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y = 0;
    int now_sec = NowSeconds();
    char buf[QUERY_LEN]; /* moze pomiescic albo g_stop_names[] (<=48), albo g_stop_query (<=96) */

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 6;
        style.padding_l = style.padding_r = 8;
        style.padding_t = style.padding_b = 6;
        style.border_w = 1;
        style.gap = 2; /* liste odjazdow, nie pojedyncze pola - cisniej niz miedzy sekcjami (margin_b) */
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    /* header box: nazwa przystanku (albo g_status, patrz nizej), ze
     * strzalkami "<"/">" przesuwajacymi g_hour_offset - patrz naglowek
     * pliku i dokladnie ten sam wzorzec co "header" w utils/7aweather.c. */
    {
        UiBox *header = ui_box_begin(ctx, "header", 0, y, win_w, &style);
        UiRect hrow = ui_box_next_rect(header, ROW_H);
        UiRect prev_r, mid_r, next_r;

        ui_rect_split3(hrow, ROW_H, ROW_H, 6, &prev_r, &mid_r, &next_r);
        if (ui_button(ctx, prev_r, "<")) AdjustHourOffset(-1);
        if (ui_button(ctx, next_r, ">")) AdjustHourOffset(1);
        /* g_status (np. "Stop not found"/blad bazy) zajmuje TO SAMO
         * miejsce co nazwa przystanku, zamiast dokladac osobny wiersz -
         * dzieki temu uklad (i win_h) zostaje IDENTYCZNY w obu stanach,
         * patrz tez komentarz przy DrawDirectionBox nizej. */
        if (g_status[0] != '\0') {
            ui_label_centered(ctx, mid_r, g_status);
        } else {
            snprintf(buf, sizeof(buf), "%s",
                     g_target_stop_idx >= 0 ? g_stop_names[g_target_stop_idx] : g_stop_query);
            ui_label_centered(ctx, mid_r, buf);
        }
        ui_box_end(header);
        /* +margin_t TYLKO tutaj (pierwszy box, zaczyna sie od y=0) - patrz
         * ten sam wzorzec i wyjasnienie w DrawDirectionBox powyzej. */
        y += style.margin_t + ui_box_height(ctx, "header") + style.margin_b;
    }

    /* Boxy kierunku sa rysowane BEZWARUNKOWO (nie tylko gdy dane sa juz
     * zaladowane) - dwa powody: (1) uklad/win_h zostaje identyczny
     * niezaleznie od stanu (patrz wyzej), (2) box_begin w ui.c rysuje
     * tlo/border na podstawie wysokosci Z POPRZEDNIEJ klatki (patrz "Cache
     * wysokosci boxa" w CLAUDE.md) - gdyby te boxy pojawialy sie dopiero
     * PO zaladowaniu danych, ich PIERWSZA klatka (o nieznanej jeszcze z
     * cache wysokosci) wyszlaby bez obramowania, i naprawilaby sie dopiero
     * przy nastepnym przerysowaniu (ruch myszy/klawisz/TICK_MS - do 20s
     * pozniej). Rysujac je od razu na pierwszej klatce placeholdera
     * "Loading timetable..." (przed RefreshTimetable() w main()), cache
     * jest juz "rozgrzany", zanim uzytkownik zobaczy realne dane. Gdy
     * g_departure_count==0 (jeszcze nie zaladowane/pusty wynik),
     * CollectDeparturesInWindow zwraca 0 i DrawDirectionBox pokazuje
     * "No departures in the next hour" - nieszkodliwe w obu przypadkach. */
    {
        int ref_sec = now_sec + g_hour_offset * 3600;
        UiRect lbl_r;

        /* Etykieta kierunku POZA boxem (nie pierwszy wiersz w srodku) -
         * ten sam wzorzec co naglowki sekcji w utils/7asys.c ("Uptime /
         * Load" itd.: ui_label, potem y += ROW_H + 4, potem dopiero box). */
        lbl_r = (UiRect){ style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        ui_label(ctx, lbl_r, "Towards Gdynia");
        y += ROW_H + 4;
        y = DrawDirectionBox(ctx, "gdynia", &style, win_w, y,
                              0, ref_sec, now_sec, &g_scroll_gdynia, &g_gdynia_list_r);

        lbl_r = (UiRect){ style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        ui_label(ctx, lbl_r, "Towards Gdansk");
        y += ROW_H + 4;
        y = DrawDirectionBox(ctx, "gdansk", &style, win_w, y,
                              1, ref_sec, now_sec, &g_scroll_gdansk, &g_gdansk_list_r);
    }

    {
        int btn_w = ui_button_width(ctx, "Refresh");
        UiRect r = { style.margin_l, y, btn_w, ROW_H };

        if (ui_button(ctx, r, "Refresh"))
            RefreshTimetable();
    }

    (void) win_h;
}

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
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
    /* Boxy kierunku maja stala wysokosc (naglowek kolumn + dokladnie
     * VISIBLE_ROWS wierszy, nadmiar przewijany kolkiem myszy - patrz
     * DrawDirectionBox), a etykieta kierunku ("Towards Gdynia" itp.) jest
     * OSOBNYM wierszem NAD boxem (patrz draw()), wiec win_h jest
     * DOKLADNY: header (margin_t(6)+ROW_H(20)+padding(12)+border(2)+
     * margin_b(6)=46) + 2x [etykieta (ROW_H(20)+4 odstepu=24) + box
     * kierunku o (1+VISIBLE_ROWS) wierszach - naglowek kolumn+5 danych -
     * (margin_t(6)+(1+5)*ROW_H(20)+5*gap(2)+padding(12)+border(2)+
     * margin_b(6)=156)] + ROW_H(20, przycisk Refresh) + symetryczny dolny
     * odstep (6, taki sam jak margin_t nagłówka na samej gorze) =
     * 46 + 2*(24+156) + 20 + 6 = 432.
     *
     * win_w jest TERAZ rowniez dokladny (przedtem 460 bez zadnego
     * uzasadnienia rozciagalo kolumne "To" na cala nadmiarowa przestrzen -
     * patrz tez PMaxSize przy sizehints nizej). Zawartosc kazdego wiersza
     * (patrz SplitColumns) to content_w = win_w - margin_l/r(8+8, domyslne
     * windowMargin) - padding_l/r(8+8) - border*2(2) = win_w-34, co musi
     * pomiescic COL_TIME_W(56) + COL_MIN_W(80) + dest_w + ARROW_W(20);
     * dest_w=150 (dosc miejsca na typowa nazwe stacji docelowej, dluzsze
     * ucinane "..." przez ui_label_ellipsis - patrz DrawDirectionBox) daje
     * win_w = 156+150+34 = 340. Gdy windowMargin (zasob X, patrz
     * Xresources.sample) jest inny niz domyslne 8, box robi sie po prostu
     * odpowiednio wezszy WEWNATRZ tego samego okna (ui_box_begin liczy
     * outer_w z przekazanej szerokosci), bez przepelnienia. */
    int win_w = 340, win_h = 432;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw;
    long next_tick_ms;
    char app_title[64] = "";
    XEvent ev;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-stop") == 0) {
            /* Zbiera WSZYSTKIE kolejne argumenty (do nastepnej znanej opcji
             * albo konca) w jedna nazwe przystanku rozdzielana spacjami -
             * zeby "-stop Gdansk Wrzeszcz" dzialalo bez cudzyslowu (ten sam
             * wzorzec co tresc powiadomienia w utils/7anotify.c). */
            g_stop_query[0] = '\0';
            i++;
            while (i < argc && strcmp(argv[i], "-geometry") != 0 && strcmp(argv[i], "-geom") != 0) {
                size_t off = strlen(g_stop_query);

                if (off > 0 && off + 1 < sizeof(g_stop_query)) g_stop_query[off++] = ' ';
                snprintf(g_stop_query + off, sizeof(g_stop_query) - off, "%s", argv[i]);
                i++;
            }
            i--; /* kompensacja i++ w for */
        }
    }

    if (g_stop_query[0] == '\0') {
        fprintf(stderr, "Usage: %s -stop STOP_NAME [-title TITLE] [-geometry WxH+X+Y]\n", argv[0]);
        return 1;
    }

#ifdef __OpenBSD__
    /* Patrz naglowek pliku po pelne uzasadnienie tego zestawu promise'ow -
     * w skrocie: bez "proc exec" (apka juz niczego nie fork+exec'uje),
     * wpath+cpath mimo to potrzebne caly czas zycia procesu (skm.db jest w
     * trybie WAL, wymaga zapisu do -wal/-shm nawet dla samych SELECT-ow). */
    if (pledge("stdio rpath wpath cpath flock unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    OpenDatabase();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7askm: brak polaczenia z X11\n");
        return 1;
    }

    /* Bez tego XrmGetResource w ReadAppString nizej potrafi zwrocic
     * poprawna wartosc, ale z type == NULL (zaobserwowane na OpenBSD) -
     * patrz ten sam komentarz przy XrmInitialize() w utils/7atodo.c. */
    XrmInitialize();
    ReadAppString(dpy, "7aSKM.rowBackground", "7aSKM.RowBackground",
                  app_data.row_bg, sizeof(app_data.row_bg), "white");

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
    XStoreName(dpy, win, app_title[0] ? app_title : "7aSKM");
    XSetIconName(dpy, win, app_title[0] ? app_title : "7aSKM");

    icon = MakeSkmIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    /* Zawartosc ma stala wysokosc (patrz komentarz przy win_h) i teraz tez
     * dokladnie dopasowana szerokosc (patrz komentarz przy win_w) - min ==
     * max w obu wymiarach, wiec WM w ogole nie pozwoli rozciagnac okna
     * (ponizej wysokosci okno obcinaloby wiersze bez przewijania -
     * przewijanie dziala tylko w PIONIE wewnatrz boxa, nie zmniejsza jego
     * wysokosci; powyzej/szerzej zostawaloby tylko puste miejsce, patrz
     * kolumna "To" w SplitColumns). min/max sa liczone z FINALNEGO
     * win_w/win_h (juz po ewentualnym nadpisaniu przez -geometry powyzej),
     * wiec jawne "-geometry" wciaz dziala - po prostu tez usztywnia okno na
     * podanym rozmiarze zamiast na domyslnym. */
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = sizehints->max_width = win_w;
    sizehints->min_height = sizehints->max_height = win_h;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7askm: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    /* Pierwsza klatka z placeholderem "Loading timetable...", ZANIM
     * zaczniemy pierwsze zapytania SQL (LoadStops/LoadDepartures) - ten sam
     * mechanizm co UpdateFeed() w utils/7arss.c (tam dla curl zamiast
     * SQLite, ale ten sam powod: nie blokowac pierwszej klatki na I/O). */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    RefreshTimetable();

    running = 1;
    redraw = 1;
    next_tick_ms = now_ms() + TICK_MS;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy (Button4/5) przechwycone TU, PRZED ui_feed_event -
             * ten sam powod co w utils/7amessage.c: ui.c nie rozroznia
             * numeru przycisku, wiec para ButtonPress/Release od kolka
             * zostalaby policzona jak zwykly klik na tym, co akurat jest
             * pod kursorem (np. Refresh). g_gdynia_list_r/g_gdansk_list_r
             * to obszar widocznych wierszy z OSTATNIEJ narysowanej klatki -
             * patrz DrawDirectionBox/draw(). */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress) {
                    int delta = (ev.xbutton.button == Button4) ? -1 : 1;
                    int px = ev.xbutton.x, py = ev.xbutton.y;

                    if (px >= g_gdynia_list_r.x && px < g_gdynia_list_r.x + g_gdynia_list_r.w &&
                        py >= g_gdynia_list_r.y && py < g_gdynia_list_r.y + g_gdynia_list_r.h) {
                        g_scroll_gdynia += delta;
                        redraw = 1;
                    } else if (px >= g_gdansk_list_r.x && px < g_gdansk_list_r.x + g_gdansk_list_r.w &&
                               py >= g_gdansk_list_r.y && py < g_gdansk_list_r.y + g_gdansk_list_r.h) {
                        g_scroll_gdansk += delta;
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
                redraw = 1;
                break;
            case KeyPress: {
                char keybuf[16];
                KeySym ks;

                XLookupString(&ev.xkey, keybuf, sizeof(keybuf), &ks, NULL);
                if (ks == XK_Escape || ks == XK_q) running = 0;
                if (ks == XK_r) { RefreshTimetable(); redraw = 1; }
                break;
            }
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

        {
            long remaining = next_tick_ms - now_ms();

            if (remaining <= 0) {
                struct tm tm_now;
                time_t t_now = time(NULL);

                localtime_r(&t_now, &tm_now);
                if (tm_now.tm_yday != g_last_load_yday)
                    RefreshTimetable(); /* zmiana dnia - nowy rozklad/nowy service_date */
                next_tick_ms = now_ms() + TICK_MS;
                redraw = 1;
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
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    sqlite3_close(db);
    return 0;
}
