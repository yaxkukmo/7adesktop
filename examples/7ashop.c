/*
 * 7ashop.c - nowa apka na biblioteke ui.c/ui.h (nie port istniejacego
 * Xt/Xaw oryginalu - podobnie jak examples/7arss.c/7acenter.c/7anotify.c,
 * tu nie ma czego portowac). Generator listy zakupow z dwoma panelami:
 *
 *  - LEWY panel: "Saved lists" (spis juz istniejacych list, klik na
 *    wierszu wczytuje jej pozycje ponizej) + "Current list" (pozycje
 *    aktualnie wczytanej/utworzonej listy, z mozliwoscia EDYCJI - patrz
 *    nizej).
 *  - PRAWY panel, od gory: "List name (optional)" (wlasny box, bo dotyczy
 *    calej tworzonej/edytowanej listy, nie samego katalogu) + pole
 *    wyszukiwania + przewijany katalog produktow z checkboxami
 *    (niezalezne zaznaczenie, ui_checkbox), a pod nim - w TYM SAMYM boxie
 *    co katalog, bo to akcja NA katalogu - "New product" (dodawanie
 *    nowego produktu) + przycisk "Create list"/"Add to list" (patrz
 *    nizej).
 *
 * Przeplyw pracy (dokladnie ten, o ktory poprosil uzytkownik): zaznacz
 * dowolna liczbe produktow w katalogu (checkbox, mozna filtrowac polem
 * wyszukiwania - zaznaczenie zyje NA PRODUKCIE, nie na filtrowanej
 * pozycji, wiec przezywa zmiane tekstu wyszukiwania), kliknij "Create
 * list" - nowa lista trafia do bazy ORAZ pojawia sie od razu w lewym
 * panelu jako biezaca. Brak osobnego kroku "przejrzyj przed zapisem" -
 * to zamierzone uproszczenie zgodne z opisanym przez uzytkownika
 * przeplywem (zaznacz -> create -> gotowe).
 *
 * Edycja listy (dopisana po pierwszej wersji, na prosbe uzytkownika):
 *  - "Current list" ma przycisk "Remove" ("x") przy KAZDYM wierszu -
 *    kasuje te jedna pozycje z bazy (list_items) i z widoku, patrz
 *    RemoveCurrentItem/ListItemRow.id.
 *  - Gdy jakas lista jest wczytana (g_current_list_id>=0), przycisk w
 *    prawym panelu zmienia etykiete z "Create list" na "Add to list" -
 *    zaznaczone produkty katalogu DOPISUJA sie do TEJ listy zamiast
 *    zakladac nowa (InsertSelectedCatalogItems dzielone miedzy oba
 *    tryby, patrz CreateListFromSelection).
 *  - "New list" w naglowku "Current list" czysci widok (BEZ ruszania
 *    bazy - ResetCurrentList) - to jedyny sposob powrotu do trybu
 *    "zaloz nowa liste" po wczytaniu/utworzeniu istniejacej, bo bez
 *    tego kazde kolejne "Create list" po wczytaniu jakiejs listy
 *    dopisywaloby do niej zamiast zakladac nowa.
 *
 * Ukladu dwoch kolumn (boxy obok siebie w tym samym oknie, nie jeden pod
 * drugim) uzyto wzorem sidebar/content w examples/7arss.c. Kazdy z trzech
 * przewijanych obszarow (zapisane listy / biezaca lista / katalog) uzywa
 * TEGO SAMEGO wzorca co examples/7askm.c: STALA liczba widocznych
 * wierszy (VISIBLE_*), przewijanie kolkiem myszy w jednostkach wierszy
 * (bez wlasnego scrollbara - biblioteka nie ma wbudowanego kontenera do
 * przewijania, patrz CLAUDE.md), znak "v" w naglowku jako jedyny sygnal
 * "jest wiecej ponizej", oraz wychwytywanie Button4/5 W main() PRZED
 * ui_feed_event (ui.c nie rozroznia numeru przycisku myszy).
 *
 * Katalog produktow trzymany jest CALY w pamieci (g_catalog, dynamiczna
 * tablica rosnaca przez realloc*2, dokladnie jak g_item_ids w
 * examples/7atodo.c) - filtrowanie/scroll dziala na tej tablicy w
 * pamieci, bez zapytania SQL przy kazdej klatce. Dodanie nowego produktu
 * NIE przeladowuje calej tablicy z bazy (co zgubiloby biezace
 * zaznaczenia uzytkownika) - tylko dopisuje jeden wpis i sortuje w
 * miejscu (CatalogAppendSorted), patrz komentarz tam.
 *
 * Dopasowanie wyszukiwania jest z NIEWRazliwe na wielkosc liter, w tym na
 * polskie znaki diakrytyczne - Utf8LowerFold to ten sam mechanizm co
 * PolishLower w examples/7askm.c (tabelka 9 polskich liter, bo pelny
 * unicode case-folding bylby tu przewymiarowany, patrz KISS w
 * CLAUDE.md), tylko przemianowany, bo nie dotyczy juz nazw przystankow.
 *
 * Bez wlasnych zasobow X (7aShop.*) - kolorystyka w calosci z ogolnego
 * motywu ui.c (ui_theme_accent/ui_theme_button_bg jako hover/wyroznienie
 * wiersza), bo apka nie ma nic specyficznego do skonfigurowania (bez
 * edytora/terminala jak w 7atodo, bez fork+exec w ogole - stad tez
 * pledge nizej jest bez "proc exec", tak jak w examples/7askm.c od kiedy
 * ta apka przestala pobierac dane sama).
 */

#define _DEFAULT_SOURCE  /* localtime_r/strftime sa POSIX - patrz ta sama uwaga w examples/7aweather.c */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

#include <sqlite3.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ITEM_NAME_LEN 64   /* nazwa produktu/pozycji listy */
#define LIST_NAME_LEN 96   /* nazwa zapisanej listy - moze byc dluzsza, wpisana recznie */
#define ROW_H 20
#define PANEL_GAP 10        /* odstep POZIOMY miedzy lewa a prawa kolumna */
#define ARROW_W 20          /* waski slot na znak "v" w naglowkach, patrz examples/7askm.c */
#define VISIBLE_SAVED 4     /* widocznych wierszy w "Saved lists" */
#define VISIBLE_CURRENT 8   /* widocznych wierszy w "Current list" */
#define VISIBLE_CATALOG 8   /* widocznych wierszy w katalogu */
#define ICON_SIZE 32

/* Te same wartosci, ktorych draw() uzywa do wypelnienia UiBoxStyle -
 * wydzielone tutaj, zeby ComputeContentHeight() w main() (liczy domyslny/
 * minimalny rozmiar okna) NIE mogla wyjsc z synchronizacji ze STYLEM, ktory
 * faktycznie renderuje boxy. Wszystkie boxy w tej apce dziela TEN SAM
 * styl (tylko margin_l/margin_r roznia sie miedzy kolumnami, co na
 * wysokosc nie wplywa). */
#define BOX_BORDER_W 1
#define BOX_PAD_TB 4    /* padding_t == padding_b */
#define BOX_MARGIN_TB 6 /* margin_t == margin_b */
#define BOX_GAP 2       /* style.gap - odstep miedzy wierszami W jednym boxie */

typedef struct {
    sqlite3_int64 id;
    char name[ITEM_NAME_LEN];
    int selected;  /* niezalezne zaznaczenie w katalogu - przezywa filtrowanie/scroll */
} CatalogItem;

typedef struct {
    sqlite3_int64 id;
    char name[LIST_NAME_LEN];
    time_t created_at;
} SavedList;

/* jedna pozycja BIEZACEJ listy - id (list_items.id) jest potrzebne, zeby
 * "Remove" na wierszu wiedzialo, KTORY wiersz bazy skasowac (patrz
 * RemoveCurrentItem). */
typedef struct {
    sqlite3_int64 id;
    char name[ITEM_NAME_LEN];
} ListItemRow;

static sqlite3 *db;
static char db_path[1200];

static CatalogItem *g_catalog = NULL;
static int g_catalog_count = 0, g_catalog_cap = 0;

static SavedList *g_lists = NULL;
static int g_list_count = 0, g_list_cap = 0;

/* pozycje BIEZACEJ (wczytanej lub wlasnie tworzonej/edytowanej) listy -
 * 7ashop nie odznacza kupionych pozycji (poza zakresem, patrz naglowek
 * pliku), ale POZWALA dodawac/usuwac pozycje z juz zapisanej listy. */
static ListItemRow *g_current_items = NULL;
static int g_current_count = 0, g_current_cap = 0;
static sqlite3_int64 g_current_list_id = -1;
static char g_current_list_name[LIST_NAME_LEN] = "";

static int g_saved_scroll = 0;
static int g_current_scroll = 0;
static int g_catalog_scroll = 0;

/* obszary WIDOCZNYCH wierszy z OSTATNIEJ narysowanej klatki - do testu
 * kolka myszy w main(), ten sam wzorzec co g_gdynia_list_r w
 * examples/7askm.c. */
static UiRect g_saved_list_r = { 0, 0, 0, 0 };
static UiRect g_current_list_r = { 0, 0, 0, 0 };
static UiRect g_catalog_list_r = { 0, 0, 0, 0 };

static char g_new_item_buf[ITEM_NAME_LEN] = "";
static int  g_new_item_cursor = 0;
static char g_search_buf[ITEM_NAME_LEN] = "";
static int  g_search_cursor = 0;
static char g_list_name_buf[LIST_NAME_LEN] = "";
static int  g_list_name_cursor = 0;

static char g_status[128] = "";

/* -------------------------------------------------------------------- */
/* Male litery + polskie znaki diakrytyczne - patrz naglowek pliku       */
/* -------------------------------------------------------------------- */

static void
Utf8LowerFold(const char *in, char *out, size_t outsz)
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
/* Baza danych - ~/.7a/shop.db, WLASNA (nie tasks.db) - inna domena       */
/* (katalog produktow/listy zakupow), nie dzieli schematu z 7atodo/7acal. */
/* -------------------------------------------------------------------- */

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char app_dir[1024];
    char *errmsg = NULL;

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/shop.db", app_dir);

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "7ashop: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        exit(1);
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS catalog_items ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " name TEXT NOT NULL COLLATE NOCASE UNIQUE"
            ");", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "7ashop: schema (catalog_items): %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        exit(1);
    }
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS lists ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " name TEXT NOT NULL,"
            " created_at INTEGER NOT NULL"
            ");", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "7ashop: schema (lists): %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        exit(1);
    }
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS list_items ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " list_id INTEGER NOT NULL,"
            " name TEXT NOT NULL"
            ");", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "7ashop: schema (list_items): %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        exit(1);
    }
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_list_items_list ON list_items(list_id);",
        NULL, NULL, NULL);
}

/* -------------------------------------------------------------------- */
/* Katalog produktow - w calosci w pamieci, patrz naglowek pliku.        */
/* -------------------------------------------------------------------- */

static void
RunCatalogQuery(void)
{
    sqlite3_stmt *stmt;

    g_catalog_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, name FROM catalog_items ORDER BY name COLLATE NOCASE;",
                            -1, &stmt, NULL) != SQLITE_OK)
        return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);

        if (g_catalog_count >= g_catalog_cap) {
            int new_cap = g_catalog_cap ? g_catalog_cap * 2 : 16;
            CatalogItem *tmp = realloc(g_catalog, (size_t) new_cap * sizeof(CatalogItem));

            if (!tmp)
                break; /* OOM - konczymy z tym, co juz wczytane */
            g_catalog = tmp;
            g_catalog_cap = new_cap;
        }
        g_catalog[g_catalog_count].id = sqlite3_column_int64(stmt, 0);
        snprintf(g_catalog[g_catalog_count].name, ITEM_NAME_LEN, "%s", name ? (const char *) name : "");
        g_catalog[g_catalog_count].selected = 0;
        g_catalog_count++;
    }
    sqlite3_finalize(stmt);
}

/* Male litery ASCII (bez tabelki polskich liter - kolejnosc sortowania
 * nie musi byc idealna, tylko sensowna; DOPASOWANIE wyszukiwania to
 * osobna sprawa i tam uzywane jest pelne Utf8LowerFold, patrz draw()). */
static int
CmpCatalogByName(const void *a, const void *b)
{
    const CatalogItem *ca = a, *cb = b;
    const unsigned char *pa = (const unsigned char *) ca->name;
    const unsigned char *pb = (const unsigned char *) cb->name;

    for (;;) {
        unsigned char x = *pa, y = *pb;

        if (x >= 'A' && x <= 'Z') x = (unsigned char) (x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char) (y + 32);
        if (x != y)
            return (int) x - (int) y;
        if (!x)
            return 0;
        pa++;
        pb++;
    }
}

/* Dopisuje JEDEN nowy produkt do juz zaladowanej tablicy w pamieci i
 * sortuje w miejscu - NIE przeladowuje calej tablicy z bazy, bo to
 * zgubiloby zaznaczenia (CatalogItem.selected), ktore uzytkownik mogl
 * juz poustawiac w innych wierszach przed dodaniem kolejnego produktu. */
static void
CatalogAppendSorted(sqlite3_int64 id, const char *name)
{
    if (g_catalog_count >= g_catalog_cap) {
        int new_cap = g_catalog_cap ? g_catalog_cap * 2 : 16;
        CatalogItem *tmp = realloc(g_catalog, (size_t) new_cap * sizeof(CatalogItem));

        if (!tmp)
            return;
        g_catalog = tmp;
        g_catalog_cap = new_cap;
    }
    g_catalog[g_catalog_count].id = id;
    snprintf(g_catalog[g_catalog_count].name, ITEM_NAME_LEN, "%s", name);
    g_catalog[g_catalog_count].selected = 0;
    g_catalog_count++;
    qsort(g_catalog, (size_t) g_catalog_count, sizeof(CatalogItem), CmpCatalogByName);
}

static void
AddCatalogItem(const char *raw)
{
    char name[ITEM_NAME_LEN];
    size_t start = 0, len;
    sqlite3_stmt *stmt;

    while (raw[start] == ' ' || raw[start] == '\t')
        start++;
    snprintf(name, sizeof(name), "%s", raw + start);
    len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
        name[--len] = '\0';

    if (len == 0) {
        snprintf(g_status, sizeof(g_status), "Enter a product name");
        return;
    }

    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO catalog_items(name) VALUES(?1);",
                            -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (sqlite3_changes(db) == 0) {
        snprintf(g_status, sizeof(g_status), "\"%s\" is already in the catalog", name);
        return;
    }

    CatalogAppendSorted(sqlite3_last_insert_rowid(db), name);
    snprintf(g_status, sizeof(g_status), "Added \"%s\" to the catalog", name);
}

/* -------------------------------------------------------------------- */
/* Zapisane listy + ich pozycje                                          */
/* -------------------------------------------------------------------- */

static void
RunListsQuery(void)
{
    sqlite3_stmt *stmt;

    g_list_count = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT id, name, created_at FROM lists ORDER BY created_at DESC, id DESC;",
            -1, &stmt, NULL) != SQLITE_OK)
        return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);

        if (g_list_count >= g_list_cap) {
            int new_cap = g_list_cap ? g_list_cap * 2 : 16;
            SavedList *tmp = realloc(g_lists, (size_t) new_cap * sizeof(SavedList));

            if (!tmp)
                break;
            g_lists = tmp;
            g_list_cap = new_cap;
        }
        g_lists[g_list_count].id = sqlite3_column_int64(stmt, 0);
        snprintf(g_lists[g_list_count].name, LIST_NAME_LEN, "%s", name ? (const char *) name : "");
        g_lists[g_list_count].created_at = (time_t) sqlite3_column_int64(stmt, 2);
        g_list_count++;
    }
    sqlite3_finalize(stmt);
}

static void
LoadListItems(sqlite3_int64 list_id)
{
    sqlite3_stmt *stmt;

    g_current_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, name FROM list_items WHERE list_id=?1 ORDER BY id ASC;",
                            -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, list_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);

        if (g_current_count >= g_current_cap) {
            int new_cap = g_current_cap ? g_current_cap * 2 : 16;
            ListItemRow *tmp = realloc(g_current_items, (size_t) new_cap * sizeof(ListItemRow));

            if (!tmp)
                break;
            g_current_items = tmp;
            g_current_cap = new_cap;
        }
        g_current_items[g_current_count].id = sqlite3_column_int64(stmt, 0);
        snprintf(g_current_items[g_current_count].name, ITEM_NAME_LEN, "%s", name ? (const char *) name : "");
        g_current_count++;
    }
    sqlite3_finalize(stmt);
}

static void
LoadSavedList(int index)
{
    if (index < 0 || index >= g_list_count)
        return;
    g_current_list_id = g_lists[index].id;
    snprintf(g_current_list_name, LIST_NAME_LEN, "%s", g_lists[index].name);
    LoadListItems(g_current_list_id);
    g_current_scroll = 0;
}

/* Czysci widok biezacej listy (BEZ ruszania bazy) - tak, zeby kolejne
 * "Create list" zalozylo NOWA liste zamiast dopisywac do poprzednio
 * wczytanej/utworzonej. */
static void
ResetCurrentList(void)
{
    g_current_list_id = -1;
    g_current_list_name[0] = '\0';
    g_current_count = 0;
    g_current_scroll = 0;
}

/* Usuwa POJEDYNCZA pozycje z biezacej listy - zarowno z bazy (DELETE po
 * list_items.id, patrz ListItemRow), jak i z tablicy w pamieci (przesuniecie
 * w dol, ten sam wzorzec co gdziekolwiek indziej w repo usuwa sie element
 * ze srodka rosnacej tablicy). */
static void
RemoveCurrentItem(int index)
{
    sqlite3_stmt *stmt;

    if (index < 0 || index >= g_current_count)
        return;

    if (sqlite3_prepare_v2(db, "DELETE FROM list_items WHERE id=?1;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, g_current_items[index].id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    memmove(&g_current_items[index], &g_current_items[index + 1],
            (size_t) (g_current_count - index - 1) * sizeof(ListItemRow));
    g_current_count--;
    if (g_current_scroll > 0 && g_current_scroll >= g_current_count)
        g_current_scroll--;
}

/* Wstawia WSZYSTKIE zaznaczone produkty katalogu jako pozycje list_items
 * pod list_id i DOPISUJE je do g_current_items (bez ponownego odpytywania
 * bazy) - wspolne dla "Create list" (nowa lista) i "Add to list" (juz
 * wczytana/utworzona lista), patrz CreateListFromSelection. Zaznaczenia sa
 * czyszczone na koncu. */
static void
InsertSelectedCatalogItems(sqlite3_int64 list_id)
{
    sqlite3_stmt *stmt;
    int i;

    if (sqlite3_prepare_v2(db, "INSERT INTO list_items(list_id, name) VALUES(?1,?2);",
                            -1, &stmt, NULL) != SQLITE_OK)
        return;

    for (i = 0; i < g_catalog_count; i++) {
        sqlite3_int64 item_id;

        if (!g_catalog[i].selected)
            continue;

        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, list_id);
        sqlite3_bind_text(stmt, 2, g_catalog[i].name, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        item_id = sqlite3_last_insert_rowid(db);

        if (g_current_count >= g_current_cap) {
            int new_cap = g_current_cap ? g_current_cap * 2 : 16;
            ListItemRow *tmp = realloc(g_current_items, (size_t) new_cap * sizeof(ListItemRow));

            if (tmp) {
                g_current_items = tmp;
                g_current_cap = new_cap;
            }
        }
        if (g_current_count < g_current_cap) {
            g_current_items[g_current_count].id = item_id;
            snprintf(g_current_items[g_current_count].name, ITEM_NAME_LEN, "%s", g_catalog[i].name);
            g_current_count++;
        }

        g_catalog[i].selected = 0;
    }
    sqlite3_finalize(stmt);
}

/* Zaznaczone produkty katalogu trafiaja albo do NOWEJ listy (gdy zadna nie
 * jest wczytana, g_current_list_id<0), albo sa DOPISYWANE do biezacej -
 * patrz "New list" w draw()/ResetCurrentList, ktore pozwala wrocic do
 * trybu "nowa lista" po wczytaniu istniejacej. */
static void
CreateListFromSelection(void)
{
    char name[LIST_NAME_LEN];
    size_t start = 0, len;
    time_t now;
    sqlite3_stmt *stmt;
    sqlite3_int64 list_id;
    int i, any = 0;

    for (i = 0; i < g_catalog_count; i++) {
        if (g_catalog[i].selected) {
            any = 1;
            break;
        }
    }
    if (!any) {
        snprintf(g_status, sizeof(g_status), "Select at least one product in the catalog");
        return;
    }

    if (g_current_list_id >= 0) {
        int before = g_current_count;

        InsertSelectedCatalogItems(g_current_list_id);
        g_search_buf[0] = '\0';
        g_search_cursor = 0;
        snprintf(g_status, sizeof(g_status), "Added %d item(s) to \"%s\"",
                 g_current_count - before, g_current_list_name);
        return;
    }

    while (g_list_name_buf[start] == ' ' || g_list_name_buf[start] == '\t')
        start++;
    snprintf(name, sizeof(name), "%s", g_list_name_buf + start);
    len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
        name[--len] = '\0';

    now = time(NULL);
    if (name[0] == '\0') {
        struct tm tmv;

        localtime_r(&now, &tmv);
        strftime(name, sizeof(name), "List %Y-%m-%d %H:%M", &tmv);
    }

    if (sqlite3_prepare_v2(db, "INSERT INTO lists(name, created_at) VALUES(?1,?2);",
                            -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64) now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    list_id = sqlite3_last_insert_rowid(db);

    g_current_count = 0;
    InsertSelectedCatalogItems(list_id);

    g_current_list_id = list_id;
    snprintf(g_current_list_name, LIST_NAME_LEN, "%s", name);
    g_current_scroll = 0;
    g_list_name_buf[0] = '\0';
    g_list_name_cursor = 0;
    g_search_buf[0] = '\0';
    g_search_cursor = 0;

    RunListsQuery();
    g_saved_scroll = 0;

    snprintf(g_status, sizeof(g_status), "Created list \"%s\" (%d items)", name, g_current_count);
}

/* -------------------------------------------------------------------- */
/* Ikona okna - koszyk, rysowany wprost Xlibem na 1-bitowej Pixmapie,     */
/* jak w reszcie examples/7a*.c.                                         */
/* -------------------------------------------------------------------- */

static void
DrawShopIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XPoint basket[4] = { { 6, 12 }, { 26, 12 }, { 23, 27 }, { 9, 27 } };

    XFillPolygon(idpy, p, gc, basket, 4, Convex, CoordModeOrigin);
    XDrawArc(idpy, p, gc, 9, 2, 14, 16, 0, 180 * 64);
    XDrawLine(idpy, p, gc, 12, 14, 10, 25);
    XDrawLine(idpy, p, gc, 20, 14, 22, 25);
}

static Pixmap
MakeShopIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawShopIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                             */
/* -------------------------------------------------------------------- */

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style, style_left, style_right;
    static int ready = 0;
    int gap = PANEL_GAP;
    int wmargin, avail, left_outer_w, left_total_w, right_w;
    int y_left = 0, y_right = 0;
    int mx, my;
    int i;

    if (!ready) {
        style = (UiBoxStyle) { 0 };
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = BOX_MARGIN_TB;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = BOX_PAD_TB;
        style.border_w = BOX_BORDER_W;
        style.gap = BOX_GAP;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);

        style_left = style;
        style_left.margin_r = gap;
        style_right = style;
        style_right.margin_l = 0;
        ready = 1;
    }

    wmargin = ui_window_margin(ctx);
    avail = win_w - 2 * wmargin - gap;
    if (avail < 0) avail = 0;
    left_outer_w = avail / 2;
    left_total_w = wmargin + left_outer_w + gap;
    right_w = win_w - left_total_w;
    if (right_w < 0) right_w = 0;

    ui_mouse_state(ctx, &mx, &my, NULL);

    /* ================================================================ */
    /* LEWA KOLUMNA: zapisane listy + biezaca lista                      */
    /* ================================================================ */

    {
        UiBox *saved = ui_box_begin(ctx, "saved", 0, y_left, left_total_w, &style_left);
        UiRect hdr = ui_box_next_rect(saved, ROW_H);
        UiRect label_r = { hdr.x, hdr.y, hdr.w - ARROW_W, hdr.h };
        UiRect arrow_r = { hdr.x + hdr.w - ARROW_W, hdr.y, ARROW_W, hdr.h };

        ui_label(ctx, label_r, "Saved lists");
        if (g_saved_scroll + VISIBLE_SAVED < g_list_count)
            ui_label_centered(ctx, arrow_r, "v");

        for (i = 0; i < VISIBLE_SAVED; i++) {
            UiRect row = ui_box_next_rect(saved, ROW_H);
            int idx = g_saved_scroll + i;

            if (i == 0)
                g_saved_list_r = (UiRect) { row.x, row.y, row.w,
                                             VISIBLE_SAVED * ROW_H + (VISIBLE_SAVED - 1) * style.gap };

            if (idx < g_list_count) {
                char buf[LIST_NAME_LEN + 24];
                struct tm tmv;
                char datebuf[20];
                int hover = mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h;
                const XColor *bg = (g_lists[idx].id == g_current_list_id) ? ui_theme_accent(ctx)
                                  : hover ? ui_theme_button_bg(ctx) : NULL;

                localtime_r(&g_lists[idx].created_at, &tmv);
                strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", &tmv);
                snprintf(buf, sizeof(buf), "%s (%s)", g_lists[idx].name, datebuf);

                if (bg) ui_fill_rect(ctx, row, bg);
                ui_label_ellipsis(ctx, row, buf);
                if (ui_hit_test(ctx, row))
                    LoadSavedList(idx);
            }
        }
        ui_box_end(saved);
        y_left += style.margin_t + ui_box_height(ctx, "saved") + style.margin_b;
    }

    {
        UiBox *current = ui_box_begin(ctx, "current", 0, y_left - style.margin_t, left_total_w, &style_left);
        UiRect hdr = ui_box_next_rect(current, ROW_H);
        int new_w = ui_button_width(ctx, "New list");
        UiRect label_r = { hdr.x, hdr.y, hdr.w - new_w - 6 - ARROW_W, hdr.h };
        UiRect new_r = { hdr.x + hdr.w - new_w - ARROW_W, hdr.y, new_w, hdr.h };
        UiRect arrow_r = { hdr.x + hdr.w - ARROW_W, hdr.y, ARROW_W, hdr.h };
        char title[LIST_NAME_LEN + 32];

        if (g_current_list_id >= 0)
            snprintf(title, sizeof(title), "%s (%d)", g_current_list_name, g_current_count);
        else
            snprintf(title, sizeof(title), "Current list");
        ui_label_ellipsis(ctx, label_r, title);
        if (ui_button(ctx, new_r, "New list"))
            ResetCurrentList();
        if (g_current_scroll + VISIBLE_CURRENT < g_current_count)
            ui_label_centered(ctx, arrow_r, "v");

        for (i = 0; i < VISIBLE_CURRENT; i++) {
            UiRect row = ui_box_next_rect(current, ROW_H);
            int idx = g_current_scroll + i;

            if (i == 0)
                g_current_list_r = (UiRect) { row.x, row.y, row.w,
                                               VISIBLE_CURRENT * ROW_H + (VISIBLE_CURRENT - 1) * style.gap };

            if (idx < g_current_count) {
                UiRect text_r = { row.x, row.y, row.w - ROW_H - 4, row.h };
                UiRect remove_r = { row.x + row.w - ROW_H, row.y, ROW_H, row.h };

                ui_label_ellipsis(ctx, text_r, g_current_items[idx].name);
                if (ui_button(ctx, remove_r, "x"))
                    RemoveCurrentItem(idx);
            } else if (g_current_count == 0 && i == 0) {
                ui_label(ctx, row, "Select products in the catalog and click \"Create list\"");
            }
        }
        ui_box_end(current);
    }

    /* ================================================================ */
    /* PRAWA KOLUMNA: dodawanie/szukanie + katalog                       */
    /* ================================================================ */

    {
        UiBox *listname = ui_box_begin(ctx, "listname", left_total_w, y_right, right_w, &style_right);
        int label_w = ui_text_width(ctx, "List name (optional):") + 8;
        UiRect row = ui_box_next_rect(listname, ROW_H);
        UiRect l = { row.x, row.y, label_w, row.h };
        UiRect m = { row.x + label_w + 6, row.y, row.w - label_w - 6, row.h };

        ui_label(ctx, l, "List name (optional):");
        ui_textbox(ctx, m, g_list_name_buf, sizeof(g_list_name_buf), &g_list_name_cursor);

        ui_box_end(listname);
        y_right += style.margin_t + ui_box_height(ctx, "listname") + style.margin_b;
    }

    {
        UiBox *controls = ui_box_begin(ctx, "controls", left_total_w, y_right - style.margin_t, right_w, &style_right);
        int label_w = ui_text_width(ctx, "Search:") + 8;
        UiRect row = ui_box_next_rect(controls, ROW_H);
        UiRect l = { row.x, row.y, label_w, row.h };
        UiRect m = { row.x + label_w + 6, row.y, row.w - label_w - 6, row.h };

        ui_label(ctx, l, "Search:");
        ui_textbox(ctx, m, g_search_buf, sizeof(g_search_buf), &g_search_cursor);

        ui_box_end(controls);
        y_right += style.margin_t + ui_box_height(ctx, "controls") + style.margin_b;
    }

    {
        UiBox *catalog = ui_box_begin(ctx, "catalog", left_total_w, y_right - style.margin_t, right_w, &style_right);
        char folded_query[ITEM_NAME_LEN];
        int total_matches, max_scroll;
        int slot_idx[VISIBLE_CATALOG];
        int s, ordinal;
        UiRect hdr, label_r, arrow_r;
        char hdr_buf[48];
        int label_w, name_w;
        UiRect row_name, row_button;

        Utf8LowerFold(g_search_buf, folded_query, sizeof(folded_query));

        total_matches = 0;
        for (i = 0; i < g_catalog_count; i++) {
            char folded_name[ITEM_NAME_LEN];

            Utf8LowerFold(g_catalog[i].name, folded_name, sizeof(folded_name));
            if (folded_query[0] == '\0' || strstr(folded_name, folded_query))
                total_matches++;
        }
        max_scroll = total_matches - VISIBLE_CATALOG;
        if (max_scroll < 0) max_scroll = 0;
        if (g_catalog_scroll > max_scroll) g_catalog_scroll = max_scroll;
        if (g_catalog_scroll < 0) g_catalog_scroll = 0;

        for (s = 0; s < VISIBLE_CATALOG; s++)
            slot_idx[s] = -1;
        ordinal = 0;
        for (i = 0; i < g_catalog_count; i++) {
            char folded_name[ITEM_NAME_LEN];

            Utf8LowerFold(g_catalog[i].name, folded_name, sizeof(folded_name));
            if (folded_query[0] != '\0' && !strstr(folded_name, folded_query))
                continue;
            if (ordinal >= g_catalog_scroll && ordinal - g_catalog_scroll < VISIBLE_CATALOG)
                slot_idx[ordinal - g_catalog_scroll] = i;
            ordinal++;
        }

        hdr = ui_box_next_rect(catalog, ROW_H);
        label_r = (UiRect) { hdr.x, hdr.y, hdr.w - ARROW_W, hdr.h };
        arrow_r = (UiRect) { hdr.x + hdr.w - ARROW_W, hdr.y, ARROW_W, hdr.h };
        snprintf(hdr_buf, sizeof(hdr_buf), "Catalog (%d/%d)", total_matches, g_catalog_count);
        ui_label(ctx, label_r, hdr_buf);
        if (g_catalog_scroll + VISIBLE_CATALOG < total_matches)
            ui_label_centered(ctx, arrow_r, "v");

        for (s = 0; s < VISIBLE_CATALOG; s++) {
            UiRect row = ui_box_next_rect(catalog, ROW_H);

            if (s == 0)
                g_catalog_list_r = (UiRect) { row.x, row.y, row.w,
                                               VISIBLE_CATALOG * ROW_H + (VISIBLE_CATALOG - 1) * style.gap };

            if (slot_idx[s] >= 0)
                ui_checkbox(ctx, row, g_catalog[slot_idx[s]].name, &g_catalog[slot_idx[s]].selected);
            else if (g_catalog_count == 0 && s == 0)
                ui_label(ctx, row, "Catalog is empty - add the first product below");
        }

        label_w = ui_text_width(ctx, "New product:") + 8;
        row_name = ui_box_next_rect(catalog, ROW_H);
        {
            int add_w = ui_button_width(ctx, "Add");
            UiRect l, m, r;

            ui_rect_split3(row_name, label_w, add_w, 6, &l, &m, &r);
            ui_label(ctx, l, "New product:");
            ui_textbox(ctx, m, g_new_item_buf, sizeof(g_new_item_buf), &g_new_item_cursor);
            if (ui_button(ctx, r, "Add")) {
                AddCatalogItem(g_new_item_buf);
                g_new_item_buf[0] = '\0';
                g_new_item_cursor = 0;
            }
        }

        {
            /* stala szerokosc niezaleznie od tego, ktora z dwoch etykiet
             * jest akurat rysowana - patrz ui_button_width w ui.h. */
            int w1 = ui_button_width(ctx, "Create list");
            int w2 = ui_button_width(ctx, "Add to list");
            const char *action_label = (g_current_list_id >= 0) ? "Add to list" : "Create list";
            UiRect btn_r, status_r;

            name_w = (w1 > w2) ? w1 : w2;
            row_button = ui_box_next_rect(catalog, ROW_H);
            btn_r = (UiRect) { row_button.x, row_button.y, name_w, row_button.h };
            status_r = (UiRect) { row_button.x + name_w + 8, row_button.y,
                                   row_button.w - name_w - 8, row_button.h };

            if (ui_button(ctx, btn_r, action_label))
                CreateListFromSelection();
            ui_label_ellipsis(ctx, status_r, g_status);
        }

        ui_box_end(catalog);
    }

    (void) win_h;
    return 0;
}

/* Wysokosc JEDNEGO boxa o n_rows wierszach (ROW_H kazdy) - patrz
 * BOX_BORDER_W/BOX_PAD_TB/BOX_GAP wyzej. Musi liczyc DOKLADNIE to samo,
 * co ui_box_begin/ui_box_next_rect w ui.c dla stylu ustawionego w draw(),
 * inaczej domyslny/minimalny rozmiar okna nizej rozjedzie sie z tym, co
 * faktycznie renderuje draw(). */
static int
BoxHeightForRows(int n_rows)
{
    return n_rows * ROW_H + (n_rows - 1) * BOX_GAP + 2 * BOX_BORDER_W + 2 * BOX_PAD_TB;
}

/* Calkowita wysokosc KOLUMNY zlozonej z n_boxes boxow ulozonych jeden pod
 * drugim (rows[i] = liczba wierszy i-tego boxa) - margin_t przed
 * pierwszym + margin_b PO KAZDYM (w tym po ostatnim, dla symetrycznego
 * odstepu od dolnej krawedzi okna, patrz draw() - kolejne boxy znosza
 * sobie nawzajem margin_t/margin_b tym samym trikiem "y - style.margin_t"
 * co examples/7arss.c, wiec miedzy dwoma boxami wychodzi TYLKO JEDEN
 * odstep BOX_MARGIN_TB, nie dwa). */
static int
ColumnHeight(const int *rows, int n_boxes)
{
    int i, total = BOX_MARGIN_TB * (n_boxes + 1);

    for (i = 0; i < n_boxes; i++)
        total += BoxHeightForRows(rows[i]);
    return total;
}

/* Wysokosc TRESCI okna (bez dekoracji WM) potrzebna, zeby zmiescic obie
 * kolumny BEZ nadmiaru pustego miejsca ponizej - uzywana i jako domyslny
 * win_h, i jako min_height (VISIBLE_* to STALE liczby wierszy, wiec w
 * odroznieniu od np. examples/7atodo.c okno nizsze niz to i tak nie
 * pokazaloby wiecej/mniej wierszy, tylko by je ucialo). */
static int
ComputeContentHeight(void)
{
    int left_rows[2]  = { 1 + VISIBLE_SAVED, 1 + VISIBLE_CURRENT };
    int right_rows[3] = { 1, 1, 1 + VISIBLE_CATALOG + 2 };
    int left_h  = ColumnHeight(left_rows, 2);
    int right_h = ColumnHeight(right_rows, 3);

    return (left_h > right_h) ? left_h : right_h;
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
    int win_w = 680;
    int win_h = ComputeContentHeight();
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    char app_name[64] = "7aShop";
    char app_title[64] = "";
    XEvent ev;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0) && i + 1 < argc) {
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
    /* Bez "proc exec" - 7ashop niczego nie fork+exec'uje (brak edytora/
     * terminala zewnetrznego jak w 7atodo), patrz naglowek pliku. wpath/
     * cpath potrzebne caly czas dzialania (baza SQLite w trybie WAL
     * zapisuje do -wal/-shm nawet dla samych SELECT-ow, ten sam powod co
     * w examples/7askm.c). */
    if (pledge("stdio rpath wpath cpath flock unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    OpenDatabase();
    RunCatalogQuery();
    RunListsQuery();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
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
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);

    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name = app_name;
        ch->res_class = "7aShop";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    icon = MakeShopIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

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
             * ten sam powod co w examples/7askm.c/7amessage.c: ui.c nie
             * rozroznia numeru przycisku, wiec para ButtonPress/Release
             * od kolka zostalaby policzona jak zwykly klik na tym, co
             * akurat jest pod kursorem (np. checkbox katalogu). */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress) {
                    int delta = (ev.xbutton.button == Button4) ? -1 : 1;
                    int px = ev.xbutton.x, py = ev.xbutton.y;

                    if (px >= g_saved_list_r.x && px < g_saved_list_r.x + g_saved_list_r.w &&
                        py >= g_saved_list_r.y && py < g_saved_list_r.y + g_saved_list_r.h) {
                        g_saved_scroll += delta;
                        redraw = 1;
                    } else if (px >= g_current_list_r.x && px < g_current_list_r.x + g_current_list_r.w &&
                               py >= g_current_list_r.y && py < g_current_list_r.y + g_current_list_r.h) {
                        g_current_scroll += delta;
                        redraw = 1;
                    } else if (px >= g_catalog_list_r.x && px < g_catalog_list_r.x + g_catalog_list_r.w &&
                               py >= g_catalog_list_r.y && py < g_catalog_list_r.y + g_catalog_list_r.h) {
                        g_catalog_scroll += delta;
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
