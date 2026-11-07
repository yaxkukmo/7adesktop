/*
 * 7askm.c - rozklad odjazdow SKM Trojmiasto (Gdansk/Sopot/Gdynia i dalej)
 * na bibliotece ui.c/ui.h z tego katalogu.
 *
 * Nowa apka (nie port Xt/Xaw - nie ma czego portowac, jak 7arss.c/7acenter.c
 * /7anotify.c/7asys.c). Uklad okna (trzy boxy, wzorem 7aweather.c i
 * 7asys.c):
 *  - nazwa przystanku jako etykieta NAD boxem "header" (jak etykiety
 *    kierunku nizej).
 *  - "header": zakres godzin ("HH:MM - HH:MM"), ktorego dotycza obie listy,
 *    wysrodkowany, ze strzalkami (ui_draw_arrow) po bokach (dokladnie ten sam wzorzec co przewijanie prognozy godzina
 *    po godzinie w 7aweather.c/RenderForecastOffset) - przesuwaja godzine
 *    ODNIESIENIA (g_hour_offset) dla obu list ponizej, wiec strzalka w
 *    prawo pokazuje "co odjezdza za godzine-dwie" zamiast tylko najblizszego
 *    jednego kursu.
 *  - "Towards Wejherowo" / "Towards Gdansk": etykieta + tabela 3 kolumn
 *    (Time/In/To - godzina, odliczanie "in N min", stacja docelowa
 *    kursu) z odjazdami w danym kierunku w oknie JEDNEJ GODZINY od
 *    godziny odniesienia (patrz CollectDeparturesInWindow). Box ma STALA
 *    wysokosc - zawsze dokladnie VISIBLE_ROWS wierszy danych - nadmiar
 *    (np. godzina szczytu z 7-8 odjazdami) przewija sie kolkiem myszy nad
 *    danym boxem albo scrollbarem po prawej (strzalki + kropki stron,
 *    wzorem DrawScrollDots z 7acenter.c; g_scroll_gdynia/g_scroll_gdansk,
 *    patrz main()), zamiast rozciagac caly box/okno (biblioteka i tak
 *    nie ma gotowego kontenera do przewijania, patrz "Brak
 *    scrollowalnego kontenera" w CLAUDE.md).
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
 * Na OpenBSD: pledge("stdio rpath wpath cpath fattr flock unix prot_exec", NULL), BEZ
 * "proc exec" (w przeciwienstwie do poprzedniej wersji tej apki, ktora
 * fork+exec'owala curl/unzip) - apka juz nigdy nie odpala zadnego procesu
 * potomnego, tylko czyta z SQLite. wpath+cpath sa mimo to potrzebne przez
 * caly czas zycia procesu: skm.db jest w trybie WAL (ustawionym raz przez
 * 7askm-fetch.c), a KAZDE polaczenie do bazy WAL - nawet takie, ktore robi
 * wylacznie SELECT-y - musi miec prawo tworzyc/aktualizowac towarzyszace
 * pliki -wal/-shm w tym samym katalogu. "fattr": SQLite (robust_open)
 * robi fchmod pustego -wal/-shm, gdy jego prawa roznia sie od praw bazy
 * (np. -wal 0600 utworzony przy umask 077 obok skm.db 0644) - bez tego
 * pledge zabija proces SIGABRT przy pierwszym zapytaniu. Bez unveil - ten sam powod co w
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
#include <X11/Xutil.h>
#include "../ui.h"
#include "icons/train_48.xbm"

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
#define SCROLLBAR_W ROW_H /* przyciski strzalek ROW_H x ROW_H, jak w 7acenter, patrz DrawDirectionBox */
#define SB_GAP 6          /* odstep scrollbara od wierszy listy */
#define SCROLL_DOT_PITCH 14 /* wysokosc komorki jednej kropki na torze scrollbara */
#define SCROLL_DOT_R 4

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
 * wzgledem aktualnej godziny sciennej - sterowane strzalkami lewo/prawo w
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
static const struct { unsigned char u0, u1, l0, l1; } polish_tbl[] = {
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
#define POLISH_TBL_N (sizeof(polish_tbl) / sizeof(polish_tbl[0]))

static void
PolishLower(const char *in, char *out, size_t outsz)
{
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

            for (t = 0; t < POLISH_TBL_N; t++) {
                if (polish_tbl[t].u0 == c0 && polish_tbl[t].u1 == c1) {
                    c0 = polish_tbl[t].l0;
                    c1 = polish_tbl[t].l1;
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

/* Czy bajty [p, p+2) to mala polska litera z polish_tbl. */
static int
IsPolishLower2(const unsigned char *p)
{
    size_t t;

    for (t = 0; t < POLISH_TBL_N; t++)
        if (polish_tbl[t].l0 == p[0] && polish_tbl[t].l1 == p[1]) return 1;
    return 0;
}

/* Feed SKM ma czesc nazw stacji (i headsignow) zapisanych WERSALIKAMI
 * ("GDAŃSK GŁÓWNY", "SOPOT") obok zwyklych ("Gdańsk Oliwa"). Nazwa bez
 * ani jednej malej litery jest zamieniana w miejscu na "Gdańsk Główny":
 * PolishLower na calosci, potem wielka litera na poczatku kazdego slowa
 * (po spacji, '-' lub '.'). Nazwy mieszane zostaja nietkniete - nie psuje
 * to skrotow typu "AWFiS" czy "Wzg. Św.". PolishLower nie zmienia
 * dlugosci w bajtach, wiec bufor wystarcza. */
static void
NormalizeName(char *name, size_t namesz)
{
    char lower[HEADSIGN_LEN > STOP_NAME_LEN ? HEADSIGN_LEN : STOP_NAME_LEN];
    unsigned char *p;
    int word_start = 1;
    size_t t;

    for (p = (unsigned char *) name; *p; p++) {
        if (*p >= 'a' && *p <= 'z') return;
        if ((*p & 0xE0) == 0xC0 && p[1] && IsPolishLower2(p)) return;
    }

    PolishLower(name, lower, sizeof(lower));
    snprintf(name, namesz, "%s", lower);
    for (p = (unsigned char *) name; *p; p++) {
        if (word_start && *p < 0x80) {
            *p = (unsigned char) toupper(*p);
        } else if (word_start && (*p & 0xE0) == 0xC0 && p[1]) {
            for (t = 0; t < POLISH_TBL_N; t++) {
                if (polish_tbl[t].l0 == p[0] && polish_tbl[t].l1 == p[1]) {
                    p[0] = polish_tbl[t].u0;
                    p[1] = polish_tbl[t].u1;
                    break;
                }
            }
        }
        if ((*p & 0xC0) != 0x80) /* tylko na pierwszym bajcie znaku */
            word_start = (*p == ' ' || *p == '-' || *p == '.');
    }
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
    /* bez bufora lookaside (domyslnie ~120 kB na polaczenie) - przy tak
     * malej bazie nie przyspiesza niczego zauwazalnie, a to najwiekszy
     * pojedynczy koszt pamieci SQLite w procesie */
    sqlite3_db_config(db, SQLITE_DBCONFIG_LOOKASIDE, NULL, 0, 0);
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
        NormalizeName(g_stop_names[g_stop_count], STOP_NAME_LEN);
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
        NormalizeName(g_departures[g_departure_count].headsign, HEADSIGN_LEN);
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

/* Strzalki lewo/prawo w headerze - patrz g_hour_offset. Odrzuca (no-op) skok,
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
        /* Krotkie z rozmyslem - to trafia w miejsce nazwy przystanku nad
         * headerem, patrz draw(); dluzszy tekst bylby uciety "...".
         * Najczestsza przyczyna: 7askm-fetch jeszcze ani razu nie
         * uruchomiony (baza pusta/nie istnieje). */
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
/* Warstwa UI                                                             */
/* -------------------------------------------------------------------- */

/* Trzy kolumny w r - dzielone na STALE szerokosci (nie ui_rect_col, bo
 * kolumny maja z natury rozna tresc: HH:MM jest zawsze tej samej dlugosci,
 * "in NN min" prawie zawsze, nazwa stacji docelowej dostaje cala reszte).
 * r to wiersz JUZ zwezony o scrollbar (patrz DrawDirectionBox), tak samo
 * dla naglowka i wierszy danych, zeby kolumny trzymaly sie siebie. */
static void
SplitColumns(UiRect r, UiRect *time_r, UiRect *min_r, UiRect *dest_r)
{
    int dest_w = r.w - COL_TIME_W - COL_MIN_W;

    if (dest_w < 0) dest_w = 0;
    *time_r = (UiRect){ r.x, r.y, COL_TIME_W, r.h };
    *min_r  = (UiRect){ r.x + COL_TIME_W, r.y, COL_MIN_W, r.h };
    *dest_r = (UiRect){ r.x + COL_TIME_W + COL_MIN_W, r.y, dest_w, r.h };
}

/* Obszar tekstu w komorce wiersza - wciety jak etykieta ui_header_cell,
 * zeby tekst trzymal sie naglowka i nie dotykal linii siatki. */
static UiRect
CellText(UiRect c)
{
    return (UiRect){ c.x + UI_CELL_PAD, c.y, c.w - 2 * UI_CELL_PAD, c.h };
}

/* Kropki stron na torze scrollbara - przeniesione z DrawScrollDots w
 * utils/7acenter.c, z przewijaniem w wierszach (*scroll) zamiast pikseli.
 * Strona = VISIBLE_ROWS wierszy; ostatnia kropka = koniec listy. *scroll
 * jako parametr, bo kazdy box kierunku ma wlasne przewiniecie. */
static int
ScrollDotsCurrent(int scroll, int pages, int max_scroll)
{
    if (scroll >= max_scroll) return pages - 1;
    return (scroll + VISIBLE_ROWS / 2) / VISIBLE_ROWS;
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
DrawScrollDots(UiCtx *ctx, UiRect track, int *scroll, int max_scroll)
{
    int pages = 1 + (max_scroll + VISIBLE_ROWS - 1) / VISIBLE_ROWS;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = ScrollDotsStart(visible, pages, ScrollDotsCurrent(*scroll, pages, max_scroll));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            *scroll = (start + i) * VISIBLE_ROWS;
            if (*scroll > max_scroll) *scroll = max_scroll;
            break;
        }
    }

    current = ScrollDotsCurrent(*scroll, pages, max_scroll);
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

/* Scrollbar obok listy: strzalka w gore / kropki / strzalka w dol.
 * Strzalki rysowane PO ui_button (inaczej tlo przycisku je zakryje), a
 * *scroll zmieniany PRZED rysowaniem wierszy, wiec ta sama klatka
 * pokazuje juz nowe przewiniecie. */
static void
DrawListScrollbar(UiCtx *ctx, UiRect sb, int *scroll, int max_scroll)
{
    UiRect up_r = { sb.x, sb.y, sb.w, ROW_H };
    UiRect down_r = { sb.x, sb.y + sb.h - ROW_H, sb.w, ROW_H };
    UiRect track_r = { sb.x, sb.y + ROW_H, sb.w, sb.h - 2 * ROW_H };
    int up_clicked = ui_button(ctx, up_r, "");
    int down_clicked = ui_button(ctx, down_r, "");

    ui_draw_arrow(ctx, up_r, UI_ARROW_UP);
    ui_draw_arrow(ctx, down_r, UI_ARROW_DOWN);
    if (up_clicked) (*scroll)--;
    if (down_clicked) (*scroll)++;
    if (*scroll > max_scroll) *scroll = max_scroll;
    if (*scroll < 0) *scroll = 0;

    if (track_r.h > 0)
        DrawScrollDots(ctx, track_r, scroll, max_scroll);
}

/* Jeden box na kierunek: naglowek kolumn (Time/In/To) + DOKLADNIE
 * VISIBLE_ROWS wierszy danych - stala wysokosc boxa NIEZALEZNIE od liczby
 * odjazdow w tej godzinie (puste sloty, gdy danych jest mniej niz
 * VISIBLE_ROWS), nadmiar przewijany kolkiem myszy albo scrollbarem po
 * prawej od wierszy (strzalki + kropki stron) przez *scroll (w
 * jednostkach wierszy - patrz g_scroll_gdynia/gdansk). Etykieta kierunku ("Towards Wejherowo" itp.) jest
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
    int grid_x1, grid_x2, grid_y0, grid_y1; /* linie siatki, z naglowka */

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
        int list_h = VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap;
        /* scrollbar na prawo od wierszy, na wysokosc listy (pod naglowkiem
         * kolumn); naglowek zweza sie tak samo, zeby kolumny trzymaly sie
         * wierszy */
        UiRect sb_r = { hdr.x + hdr.w - SCROLLBAR_W, hdr.y + ROW_H + style->gap,
                        SCROLLBAR_W, list_h };
        UiRect time_r, min_r, dest_r;
        /* pasek naglowka do 1px od ramki boxa, takze nad scrollbarem -
         * skrajne komorki rozciagniete do jego brzegow (patrz 7atube.c) */
        UiRect strip = ui_box_header_strip(box, hdr);

        hdr.w -= SCROLLBAR_W + SB_GAP;
        SplitColumns(hdr, &time_r, &min_r, &dest_r);
        ui_header_cell(ctx, (UiRect) { strip.x, strip.y, min_r.x - strip.x, strip.h },
                       time_r.x + UI_CELL_PAD, "Time");
        ui_header_cell(ctx, (UiRect) { min_r.x - 1, strip.y, dest_r.x - min_r.x + 1, strip.h },
                       min_r.x + UI_CELL_PAD, "In");
        ui_header_cell(ctx, (UiRect) { dest_r.x - 1, strip.y, strip.x + strip.w - dest_r.x + 1, strip.h },
                       dest_r.x + UI_CELL_PAD, "To");
        DrawListScrollbar(ctx, sb_r, scroll, max_scroll);
        grid_x1 = min_r.x - 1; /* na wspolnej krawedzi komorek naglowka */
        grid_x2 = dest_r.x - 1;
        grid_y0 = sb_r.y;
        grid_y1 = sb_r.y + sb_r.h - 1;
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = *scroll + i;

        row.w -= SCROLLBAR_W + SB_GAP;

        if (i == 0 && out_list_r)
            *out_list_r = (UiRect){ row.x, row.y, row.w,
                                     VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap };

        if (n == 0) {
            if (i == 0) ui_label(ctx, row, "No departures in the next hour");
        } else if (idx < n) {
            UiRect time_r, min_r, dest_r;
            char tbuf[8], mbuf[20];
            int h = (rows[idx].dep_seconds / 3600) % 24; /* normalizacja kursow po polnocy (>=24:00:00) */
            int m = (rows[idx].dep_seconds / 60) % 60;
            int diff_min = (rows[idx].dep_seconds - now_sec + 59) / 60; /* zaokraglenie w gore */

            if (diff_min < 0) diff_min = 0;
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, m);
            snprintf(mbuf, sizeof(mbuf), "in %d min", diff_min);

            ui_fill_round_rect(ctx, row, &row_bg);
            SplitColumns(row, &time_r, &min_r, &dest_r);
            ui_label(ctx, CellText(time_r), tbuf);
            ui_label(ctx, CellText(min_r), mbuf);
            ui_label_ellipsis(ctx, CellText(dest_r), rows[idx].headsign);
        }
        /* else: pusty slot - stala wysokosc boxa niezaleznie od liczby danych */
    }

    /* siatka: pionowe linie na granicach kolumn przez cala liste (pod
     * naglowkiem), PO wierszach - inaczej tlo wiersza by je przerwalo.
     * Bez danych wiersz 0 to komunikat na cala szerokosc - bez siatki. */
    if (n > 0) {
        ui_draw_line(ctx, grid_x1, grid_y0, grid_x1, grid_y1, 1, ui_theme_line_fg(ctx));
        ui_draw_line(ctx, grid_x2, grid_y0, grid_x2, grid_y1, 1, ui_theme_line_fg(ctx));
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
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = 6;
        style.border_w = 1;
        style.gap = 2; /* liste odjazdow, nie pojedyncze pola - cisniej niz miedzy sekcjami (margin_b) */
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    /* Nazwa przystanku (albo g_status, patrz nizej) jako etykieta NAD
     * boxem "header", tym samym wzorcem co etykiety kierunku ponizej
     * (ui_label, potem y += ROW_H + 4, potem box). y startuje od
     * margin_b, tak jakby nad etykieta byl box - ten sam odstep od gory
     * okna co miedzy sekcjami. g_status (np. "Stop not found"/blad bazy)
     * zajmuje TO SAMO miejsce co nazwa, zamiast dokladac osobny wiersz -
     * uklad (i win_h) zostaje IDENTYCZNY w obu stanach. */
    {
        UiRect lbl_r;

        y = style.margin_b;
        lbl_r = (UiRect){ style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
        if (g_status[0] != '\0') {
            ui_label_ellipsis(ctx, lbl_r, g_status);
        } else {
            snprintf(buf, sizeof(buf), "%s",
                     g_target_stop_idx >= 0 ? g_stop_names[g_target_stop_idx] : g_stop_query);
            ui_label_ellipsis(ctx, lbl_r, buf);
        }
        y += ROW_H + 4;
    }

    /* header box: zakres godzin, ktorego dotycza obie listy ponizej
     * ([ref_sec, ref_sec + 1h), patrz CollectDeparturesInWindow), ze
     * strzalkami lewo/prawo przesuwajacymi g_hour_offset - patrz naglowek
     * pliku i ten sam wzorzec co "header" w utils/7aweather.c. */
    {
        UiBox *header = ui_box_begin(ctx, "header", 0, y, win_w, &style);
        UiRect hrow = ui_box_next_rect(header, ROW_H);
        UiRect prev_r, mid_r, next_r;
        int ref_sec, end_sec;

        ui_rect_split3(hrow, ROW_H, ROW_H, 6, &prev_r, &mid_r, &next_r);
        /* strzalka rysowana PO przycisku - inaczej tlo przycisku ja zakryje */
        int prev_clicked = ui_button(ctx, prev_r, "");
        int next_clicked;

        ui_draw_arrow(ctx, prev_r, UI_ARROW_LEFT);
        next_clicked = ui_button(ctx, next_r, "");
        ui_draw_arrow(ctx, next_r, UI_ARROW_RIGHT);
        if (prev_clicked) AdjustHourOffset(-1);
        if (next_clicked) AdjustHourOffset(1);
        /* zakres liczony PO AdjustHourOffset - inaczej klik bylby widoczny
         * dopiero w nastepnej klatce (np. po ruchu myszy) */
        ref_sec = now_sec + g_hour_offset * 3600;
        end_sec = ref_sec + 3600;
        /* % 24 - g_hour_offset moze wyjsc poza polnoc (kursy >= 24:00:00) */
        snprintf(buf, sizeof(buf), "%02d:%02d - %02d:%02d",
                 (ref_sec / 3600) % 24, (ref_sec / 60) % 60,
                 (end_sec / 3600) % 24, (end_sec / 60) % 60);
        ui_label_centered(ctx, mid_r, buf);
        ui_box_end(header);
        /* +margin_t, bo box_begin go DOLOZYL do y - patrz ten sam wzorzec
         * i wyjasnienie w DrawDirectionBox powyzej. */
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
        ui_label(ctx, lbl_r, "Towards Wejherowo");
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
    XSizeHints *sizehints;
    /* Boxy kierunku maja stala wysokosc (naglowek kolumn + dokladnie
     * VISIBLE_ROWS wierszy, nadmiar przewijany kolkiem myszy - patrz
     * DrawDirectionBox), a etykieta kierunku ("Towards Wejherowo" itp.) jest
     * OSOBNYM wierszem NAD boxem (patrz draw()), wiec win_h jest
     * DOKLADNY: nazwa przystanku nad headerem (odstep 6 + ROW_H(20) + 4
     * = 30) + header z zakresem godzin (margin_t(6)+ROW_H(20)+padding(12)+
     * border(2)+margin_b(6)=46) + 2x [etykieta (ROW_H(20)+4 odstepu=24) + box
     * kierunku o (1+VISIBLE_ROWS) wierszach - naglowek kolumn+5 danych -
     * (margin_t(6)+(1+5)*ROW_H(20)+5*gap(2)+padding(12)+border(2)+
     * margin_b(6)=156)] + ROW_H(20, przycisk Refresh) + symetryczny dolny
     * odstep (6, taki sam jak margin_t nagłówka na samej gorze) =
     * 30 + 46 + 2*(24+156) + 20 + 6 = 462.
     *
     * win_w jest TERAZ rowniez dokladny (przedtem 460 bez zadnego
     * uzasadnienia rozciagalo kolumne "To" na cala nadmiarowa przestrzen -
     * patrz tez PMaxSize przy sizehints nizej). Zawartosc kazdego wiersza
     * (patrz SplitColumns) to content_w = win_w - margin_l/r(8+8, domyslne
     * windowMargin) - padding_l/r(6+6, domyslne boxPadding) - border*2(2)
     * = win_w-30, co musi pomiescic COL_TIME_W(56) + COL_MIN_W(80) + dest_w
     * + SB_GAP(6) + SCROLLBAR_W(20); dest_w=150 (dosc miejsca na typowa nazwe
     * stacji docelowej, dluzsze ucinane "..." przez ui_label_ellipsis - patrz
     * DrawDirectionBox) daje 312+30 = 342, zaokraglone do win_w = 346.
     * Gdy windowMargin/boxPadding (zasoby X, patrz Xresources.sample) sa
     * inne niz domyslne, box/kolumna "To" robi sie po prostu
     * odpowiednio wezszy WEWNATRZ tego samego okna (ui_box_begin liczy
     * outer_w z przekazanej szerokosci), bez przepelnienia. */
    int win_w = 346, win_h = 462;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw;
    long next_tick_ms;
    char app_name[64] = "7aSKM";
    char app_title[64] = "";
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
        fprintf(stderr, "Usage: %s -stop STOP_NAME [-name NAME] [-title TITLE] [-geometry WxH+X+Y]\n", argv[0]);
        return 1;
    }

#ifdef __OpenBSD__
    /* Patrz naglowek pliku po pelne uzasadnienie tego zestawu promise'ow -
     * w skrocie: bez "proc exec" (apka juz niczego nie fork+exec'uje),
     * wpath+cpath mimo to potrzebne caly czas zycia procesu (skm.db jest w
     * trybie WAL, wymaga zapisu do -wal/-shm nawet dla samych SELECT-ow). */
    if (pledge("stdio rpath wpath cpath fattr flock unix prot_exec", NULL) == -1) {
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

    ui_resource_string(dpy, "7aSKM.rowBackground", "7aSKM.RowBackground",
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
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7aSKM";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

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
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, train_48_width, train_48_height, train_48_bits);

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
    XCloseDisplay(dpy);
    sqlite3_close(db);
    return 0;
}
