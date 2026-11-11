/*
 * 7atube.c - klient YouTube (odpowiednik pipe-viewer) na bibliotece
 * ui.c/ui.h z tego katalogu.
 *
 * Nowa apka (nie port Xt/Xaw - nie ma czego portowac, jak 7arss.c/
 * 7agroff.c). Apka NIE rozmawia z YouTube sama (zadnego API/JSON/HTML w
 * C) - cala wiedze o serwisie dzwiga zewnetrzny yt-dlp, a odtwarzanie mpv.
 * Tak samo robi pipe-viewer (odtwarzanie i tak oddaje do mpv+yt-dlp), a
 * zmiany po stronie YouTube naprawia aktualizacja yt-dlp, nie tej apki.
 * Zero nowych zaleznosci linkowania - tylko libui.a + X11, jak 7arss.
 *
 * Wyszukiwanie: fork+execvp "yt-dlp --flat-playlist --playlist-end N
 * --print SZABLON URL" z wyjsciem w potoku, czytanym NIEBLOKUJACO przez
 * select() razem z deskryptorem polaczenia X (ten sam mechanizm co timer w
 * 7aweather.c/7arss.c) - yt-dlp potrzebuje zwykle kilku sekund, a popen +
 * fread jak w 7arss.c zamrozilby na ten czas cale okno. Bez powloki
 * (execvp), wiec zapytanie uzytkownika nie wymaga shell-quotowania.
 * Szablon --print to pola rozdzielone tabem (id/kanal/data/czas/
 * wyswietlenia/status/tytul) - jedna linia na wynik, parsowana w
 * ParseResultLine bez parsera JSON. Wszystko z TEJ SAMEJ odpowiedzi co
 * lista (--flat-playlist), bez osobnego zapytania o kazdy film. Data to
 * przyblizona upload_date (--extractor-args youtubetab:approximate_date -
 * yt-dlp wylicza ja z napisow typu "3 days ago"), wiec kolumna "Date" jest
 * dokladna tylko co do dnia/tygodnia/miesiaca, zaleznie od wieku filmu.
 *
 * Box "info" pod lista: szczegoly ZAZNACZONEGO filmu (pelny tytul, kanal,
 * czas trwania, data, wyswietlenia, status live, URL). Stala liczba
 * wierszy (INFO_ROWS) takze bez zaznaczenia - wysokosc okna z
 * ComputeContentHeight nie zalezy od stanu. Przyciski Play/Audio/Save +
 * status przyklejone do DOLU okna (powiekszone okno = wolne miejsce nad
 * nimi, nie pod nimi).
 *
 * Dwie zakladki ui_tabs nad pierwszym boxem: "Search" (pole, lista,
 * akcje) i "Configuration" (DrawConfigBox - filtry wyszukiwania i
 * ustawienia odtwarzania/pobierania, box rozciagniety do dolu okna).
 * Po kazdym kliknieciu main() rysuje od razu druga klatke (wzorem Render
 * w utils/7afilm.c): klik zmienia stan W TRAKCIE klatki (zakladka znana
 * dopiero po ui_box_begin pierwszego boxa, checkbox zmieniony po
 * narysowaniu czesci widgetow) - bez drugiej klatki zmiana bylaby
 * widoczna dopiero po ruchu mysza. Trzecia klatka, bo box "config"
 * narysowany pierwszy raz (w drugiej klatce) nie ma jeszcze wysokosci w
 * cache ui.c, czyli tla/ramki - dopiero w trzeciej ja ma. Klik zyje
 * jedna klatke (ui_end_frame), wiec kolejne niczego nie klikna.
 *
 * Filtry wyszukiwania (checkboxy na zakladce Configuration): URL to strona wynikow
 * YouTube (results?search_query=...&sp=...), NIE "ytsearchN:" - tylko
 * ona przyjmuje filtry. sp to base64 z malej wiadomosci protobuf
 * (BuildSearchParam): pole 1 = sortowanie, pole 2 = filtry (data/typ/
 * dlugosc/cechy). Kody zweryfikowane na zywo przez yt-dlp przy dodawaniu
 * filtrow (2026-10) - YouTube moze je kiedys zmienic, wtedy filtr po
 * cichu przestanie dzialac, wyszukiwanie zostanie. Typ "tylko filmy"
 * jest ustawiony zawsze (kanaly/playlisty i tak odrzuca ValidVideoId).
 * Grupy Sort/Date/Length sa wylaczne (jeden wybor albo zaden - ponowny
 * klik odznacza), cechy (HD/4K/...) niezalezne. Zmiana filtra przy
 * niepustym zapytaniu od razu ponawia wyszukiwanie (zakladka sie nie
 * zmienia).
 *
 * Quality (480p/720p/1080p, wylaczne) i "Audio only" to NIE filtry
 * wyszukiwania, tylko format przy Play ("--ytdl-format=" dla mpv - wiec
 * 7aTube.player MUSI byc mpv albo czyms o tej samej opcji) i Save ("-f"
 * dla yt-dlp; "Audio only" = "-f bestaudio", sam strumien audio bez
 * ffmpeg).
 *
 * Akcje na ZAZNACZONYM wierszu (klik w wiersz = zaznaczenie, przyciski
 * Play/Audio/Save na dole okna zamiast kolumn w kazdym wierszu - mniej
 * elementow w wierszu, wiecej miejsca na tytul):
 *   - Play:  "PLAYER URL" bez terminala (mpv otwiera wlasne okno) - wzorem
 *            RunDirect w utils/7agroff.c.
 *   - Audio: "PLAYER --no-video URL" W TERMINALU - mpv bez okna nie ma
 *            innego interfejsu sterowania (pauza/przewijanie) niz TTY.
 *   - Save:  "mkdir -p DIR && cd DIR && YTDLP URL" W TERMINALU, trzymanym
 *            otwartym do Enter zawsze (nie tylko przy bledzie, jak w
 *            7agroff.c) - zeby bylo widac, czy pobieranie sie skonczylo.
 *
 * Zasoby X (patrz Xresources.sample): 7aTube.player (domyslnie "mpv"),
 * 7aTube.ytdlp ("yt-dlp" - wieloslowne, np. z --cookies-from-browser),
 * 7aTube.terminal ("urxvt"), 7aTube.downloadDir ("~/Videos").
 *
 * Swiadomie BEZ miniatur (JPEG/WebP wymagalby libjpeg albo ImageMagick -
 * patrz dyskusja przy powstaniu apki), logowania, komentarzy i playlist.
 * Subskrypcje kanalow (RSS youtube.com/feeds/videos.xml) - kolejny krok.
 *
 * Popup "w toku" (DrawBusyPopup): mpv po Play przez kilka sekund sam
 * odpytuje YouTube (yt-dlp) zanim pokaze okno, a wyszukiwanie tez trwa
 * kilka sekund - bez sygnalu latwo bylo kliknac Play kilka razy. Popup z
 * klepsydra rysowany NA WIERZCHU okna (ostatni w draw(), bez osobnego
 * okna X/override_redirect - biblioteka nie ma prawdziwych popupow, patrz
 * CLAUDE.md), widoczny przez cale wyszukiwanie albo BUSY_*_MS po akcji.
 * Apka nie wie, kiedy mpv faktycznie otworzy okno (to inny proces), wiec
 * po Play to staly czas, nie "do skutku". Dopoki popup jest widoczny,
 * klikniecia w okno sa polykane w main() PRZED ui_feed_event - to jest
 * wlasciwa ochrona przed wielokrotnym Play. Escape w trakcie wyszukiwania
 * je anuluje (poza nim - zamyka apke). Klepsydra "przesypuje sie" co
 * BUSY_FRAME_MS (select() z timeoutem tylko gdy popup jest widoczny), zeby
 * bylo widac, ze apka nie zawisla.
 *
 * Ikony (klepsydra) to Material Symbols Google (Outlined, wght 400,
 * licencja Apache 2.0) przekonwertowane do 1-bitowych XBM w utils/icons/
 * (#include - XBM to zwykly C): SVG z fonts.gstatic.com/s/i/short-term/
 * release/materialsymbolsoutlined/NAZWA/default/{24,40}px.svg, rsvg-convert
 * do PNG w rozmiarze natywnym (klepsydry 40px), prog alfa 50%,
 * lewa polowa odbita na prawa (rasteryzacja gubi symetrie o piksel),
 * przyciecie do obrysu. Rysowane ui_draw_bitmap (ui.c) przez ui_fill_rect po
 * odcinkach wiersza - bez Pixmap/zasobow serwera do zwalniania, z
 * kolorem motywu i z ui_set_clip. Material Symbols jako font odpada:
 * ui.c to core X11 fonts bez Xft.
 *
 * Lista wynikow: stala liczba widocznych wierszy + scroll kolkiem i
 * scrollbarem po prawej (strzalki + kropki stron, wzorem DrawScrollDots z
 * utils/7acenter.c). Enter w polu wyszukiwania = Search
 * (ui_textbox_key), po nim fokus schodzi z pola na liste.
 *
 * Klawiatura w dwoch trybach, jak w vi: z fokusem w polu (na starcie i po
 * '/') klawisze ida do pola, Escape z niego wychodzi; bez fokusu dzialaja
 * skroty listy (HandleListKey, tylko na zakladce Search): j/k i strzalki - nastepny/poprzedni, g/G
 * i Home/End - pierwszy/ostatni, Ctrl-d/Ctrl-u - pol strony, Ctrl-f/
 * Ctrl-b i PageDown/PageUp - strona, Enter - Play. Fokus sterowany przez
 * ui_set_focus/ui_has_focus. Skrot listy NIE trafia do ui_feed_event -
 * inaczej '/' ustawiajacy fokus wpisalby sie w tej samej klatce do pola.
 * Bez skrotu zamykajacego typu 'q' - Escape poza polem i tak zamyka.
 *
 * Caly tekst WIDOCZNY w oknie jest po angielsku, jak w reszcie utils/
 * 7a*.c - komentarze zostaja po polsku.
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"
#include "icons/smart_display_48.xbm"
#include "icons/hourglass_top.xbm"
#include "icons/hourglass_bottom.xbm"

#define ROW_H 20
#define SCROLLBAR_W ROW_H  /* przyciski strzalek ROW_H x ROW_H, jak w 7acenter */
#define SCROLL_DOT_PITCH 14  /* wysokosc komorki jednej kropki na torze scrollbara */
#define SCROLL_DOT_R 4
#define VISIBLE_ROWS 12 /* stala liczba widocznych wierszy - nadmiar przewijany */
#define COL_GAP 6
#define TAB_H 22
#define TAB_INDENT 8         /* odsuniecie zakladek od zaokraglonego rogu boxa */
#define MAX_CMD_TOKENS 24
#define MAX_RESULTS 50
#define SEARCH_COUNT "30" /* --playlist-end - strona wynikow YouTube jest nieskonczona */
#define CHECK_GAP 10    /* odstep miedzy kolejnymi checkboxami w wierszu filtrow */
#define BUSY_PLAY_MS 5000   /* mpv zwykle potrzebuje 2-5 s na pokazanie okna */
#define BUSY_TERM_MS 1500   /* terminal (Audio/Save) otwiera sie szybko */
#define BUSY_FRAME_MS 600   /* klepsydra pelna u gory <-> pelna u dolu */
#define ID_LEN 16       /* id filmu YouTube ma 11 znakow */
#define TITLE_LEN 256
#define CHANNEL_LEN 96
#define DURATION_LEN 16
#define DATE_LEN 11     /* "YYYY-MM-DD" */
#define VIEWS_LEN 32    /* "1 234 567 890" */
#define LIVE_LEN 16
#define INFO_ROWS 4     /* wiersze boxa "info" - patrz DrawInfoBox */
#define LINE_LEN 1024   /* jedna linia wyjscia yt-dlp; dluzsza jest przycinana */
#define QUERY_LEN 256
#define DIR_LEN 768

/* Te same wartosci, ktorych draw() uzywa do wypelnienia UiBoxStyle -
 * wydzielone, zeby ComputeContentHeight() nie wyszla z synchronizacji ze
 * stylem (ten sam wzorzec i nazwy co w utils/7agroff.c). */
#define BOX_BORDER_W 1
#define BOX_PAD_TB 6
#define BOX_MARGIN_TB 6
#define BOX_GAP 2

/* Separator pol to PRAWDZIWY znak tabulacji w tym stringu (C zamienia \t),
 * przekazany do yt-dlp jako jeden element argv - bez powloki, wiec nic go
 * nie interpretuje po drodze. "|..." to wartosc domyslna yt-dlp dla
 * pustego pola, "channel,uploader" - pierwsze niepuste z dwoch. Tytul
 * OSTATNI - dostaje cala reszte linii (patrz ParseResultLine). */
#define PRINT_TEMPLATE "%(id)s\t%(channel,uploader|?)s\t%(upload_date|-)s\t" \
                       "%(duration_string|-)s\t%(view_count|-)s\t%(live_status|-)s\t%(title)s"
#define PRINT_FIELDS 7

typedef struct {
    char id[ID_LEN];
    char title[TITLE_LEN];
    char channel[CHANNEL_LEN];
    char duration[DURATION_LEN];
    char date[DATE_LEN];   /* przyblizona, patrz naglowek pliku */
    char views[VIEWS_LEN];
    char live[LIVE_LEN];
} Video;

typedef struct {
    char player[128];   /* np. "mpv" */
    char ytdlp[128];    /* np. "yt-dlp" */
    char terminal[128]; /* np. "urxvt"; pusty = polecenie bez terminala */
    char download_dir[DIR_LEN];
} AppData;

static AppData app_data;

/* Jedna pozycja filtra: etykieta checkboxa + wartosc (dla grup
 * wyszukiwania - wartosc pola protobuf, dla cech - TAG pola, dla jakosci -
 * maksymalna wysokosc w px). Patrz naglowek pliku. */
typedef struct {
    const char *label;
    int value;
} Choice;

#define N_CHOICES(a) ((int) (sizeof(a) / sizeof((a)[0])))

static const Choice sort_choices[] = { { "Newest", 2 }, { "Most viewed", 3 } };
static const Choice date_choices[] = {
    { "Today", 2 }, { "Week", 3 }, { "Month", 4 }, { "Year", 5 },
};
static const Choice length_choices[] = { { "<4 min", 1 }, { "4-20 min", 3 }, { ">20 min", 2 } };
/* kolejnosc = rosnacy tag pola, tak tez trafiaja do protobuf */
static const Choice feature_choices[] = {
    { "HD", 0x20 }, { "Subtitles", 0x28 }, { "CC", 0x30 }, { "Live", 0x40 }, { "4K", 0x70 },
};
static const Choice quality_choices[] = { { "480p", 480 }, { "720p", 720 }, { "1080p", 1080 } };
static const Choice save_choices[] = { { "Audio only", 0 } };

/* indeks wybranej pozycji w grupie wylacznej, -1 = brak */
static int g_sort = -1, g_date = -1, g_length = -1, g_quality = -1;
static int g_features[N_CHOICES(feature_choices)];
static int g_save_audio = 0;
enum { TAB_SEARCH, TAB_CONFIG, TAB_COUNT };
static int g_tab = TAB_SEARCH;

static Video g_results[MAX_RESULTS];
static int g_result_count = 0;
static int g_scroll = 0;
static int g_selected = -1; /* indeks w g_results, -1 = brak zaznaczenia */

/* Popup "w toku" po akcji (patrz naglowek pliku) - widoczny do
 * g_busy_until_ms; wyszukiwanie ma wlasny warunek (g_search_fd >= 0). */
static char g_busy_text[64] = "";
static long g_busy_until_ms = 0;
static UiRect g_list_r;
static char g_status[160] = "Type a query and press Enter";

static char g_query[QUERY_LEN] = "";
static int g_query_cursor = 0;

/* Trwajace wyszukiwanie: -1 = brak. g_line zbiera niepelna linie miedzy
 * kolejnymi read() - potok oddaje dane w dowolnych kawalkach. */
static pid_t g_search_pid = -1;
static int g_search_fd = -1;
static char g_line[LINE_LEN];
static size_t g_line_len = 0;

/* "~/Videos" -> "$HOME/Videos" - tylko prefiks "~/" albo samo "~", jak
 * ExpandHome w utils/7agroff.c. */
static void
ExpandHome(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, outsz, "%s%s", home, in + 1);
    else
        snprintf(out, outsz, "%s", in);
}

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
ShowBusy(const char *text, long duration_ms)
{
    snprintf(g_busy_text, sizeof(g_busy_text), "%s", text);
    g_busy_until_ms = now_ms() + duration_ms;
}

static int
BusyActive(void)
{
    return g_search_fd >= 0 || now_ms() < g_busy_until_ms;
}

/* -------------------------------------------------------------------- */
/* Parsowanie wyjscia yt-dlp - niezalezne od ui.c                         */
/* -------------------------------------------------------------------- */

/* Id trafia do URL-a, ktory potem idzie m.in. przez "sh -c" (Audio/Save) -
 * akceptujemy tylko alfabet faktycznych id YouTube, zamiast ufac
 * wyjsciu zewnetrznego procesu. */
static int
ValidVideoId(const char *id)
{
    size_t len = strlen(id), i;

    if (len == 0 || len >= ID_LEN)
        return 0;
    for (i = 0; i < len; i++) {
        char c = id[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* "YYYYMMDD" -> "YYYY-MM-DD"; cokolwiek innego (np. "-") bez zmian. */
static void
FormatDate(const char *in, char *out, size_t outsz)
{
    size_t i;

    for (i = 0; i < 8 && isdigit((unsigned char) in[i]); i++)
        ;
    if (i == 8 && in[8] == '\0')
        snprintf(out, outsz, "%.4s-%.2s-%.2s", in, in + 4, in + 6);
    else
        snprintf(out, outsz, "%s", in);
}

/* "1234567" -> "1 234 567"; nie-liczba (np. "-") bez zmian. */
static void
FormatViews(const char *in, char *out, size_t outsz)
{
    size_t len = strlen(in), i, o = 0;

    for (i = 0; i < len; i++)
        if (!isdigit((unsigned char) in[i]))
            break;
    if (len == 0 || i < len || len > 15) {
        snprintf(out, outsz, "%s", in);
        return;
    }
    for (i = 0; i < len && o + 2 < outsz; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            out[o++] = ' ';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

/* live_status yt-dlp -> tekst w boxie "info". */
static const char *
LiveText(const char *status)
{
    static const struct { const char *key, *text; } map[] = {
        { "not_live", "Video" }, { "is_live", "Live now" },
        { "is_upcoming", "Upcoming" }, { "was_live", "Past stream" },
        { "post_live", "Past stream" },
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(status, map[i].key) == 0)
            return map[i].text;
    return "-";
}

/* Pola w kolejnosci PRINT_TEMPLATE: pierwsze PRINT_FIELDS-1 rozdzielone
 * tabem, tytul to cala reszta linii (gdyby kiedys zawieral tab, nie
 * rozjedzie to pozostalych pol). Zmienia line w miejscu. */
static void
ParseResultLine(char *line)
{
    char *f[PRINT_FIELDS];
    char *p = line;
    Video *v;
    int i;

    if (g_result_count >= MAX_RESULTS)
        return;
    for (i = 0; i < PRINT_FIELDS - 1; i++) {
        char *tab = strchr(p, '\t');

        if (!tab)
            return;
        *tab = '\0';
        f[i] = p;
        p = tab + 1;
    }
    f[PRINT_FIELDS - 1] = p;
    if (!ValidVideoId(f[0]))
        return;

    v = &g_results[g_result_count++];
    snprintf(v->id, sizeof(v->id), "%s", f[0]);
    snprintf(v->channel, sizeof(v->channel), "%s", f[1]);
    FormatDate(f[2], v->date, sizeof(v->date));
    snprintf(v->duration, sizeof(v->duration), "%s", f[3]);
    FormatViews(f[4], v->views, sizeof(v->views));
    snprintf(v->live, sizeof(v->live), "%s", LiveText(f[5]));
    snprintf(v->title, sizeof(v->title), "%s", f[6]);
}

static void
VideoUrl(const Video *v, char *out, size_t outsz)
{
    snprintf(out, outsz, "https://www.youtube.com/watch?v=%s", v->id);
}

/* -------------------------------------------------------------------- */
/* URL wyszukiwania z filtrami - niezalezne od ui.c                       */
/* -------------------------------------------------------------------- */

/* Bez zmian wzgledem utils/7arss.c. */
static void
UrlEncode(const char *in, char *out, size_t outsize)
{
    static const char *hex = "0123456789ABCDEF";
    size_t oi = 0;

    for (; *in && oi + 4 < outsize; in++) {
        unsigned char c = (unsigned char) *in;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[oi++] = (char) c;
        } else {
            out[oi++] = '%';
            out[oi++] = hex[(c >> 4) & 0xF];
            out[oi++] = hex[c & 0xF];
        }
    }
    out[oi] = '\0';
}

static void
Base64Encode(const unsigned char *in, size_t n, char *out, size_t outsz)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;

    for (i = 0; i < n && o + 4 < outsz; i += 3) {
        unsigned long v = (unsigned long) in[i] << 16;

        if (i + 1 < n) v |= (unsigned long) in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Parametr "sp" (base64 protobuf, patrz naglowek pliku). Wszystkie
 * wartosci i tagi sa < 128, wiec kazde pole to dokladnie 2 bajty (tag +
 * varint) - bez ogolnego kodera varintow. Pole 2 (filtry) ma pola w
 * rosnacej kolejnosci tagow: data 0x08, typ 0x10, dlugosc 0x18, cechy. */
static void
BuildSearchParam(char *out, size_t outsz)
{
    unsigned char inner[2 * (3 + N_CHOICES(feature_choices))];
    unsigned char msg[4 + sizeof(inner)];
    size_t ni = 0, nm = 0;
    int i;

    if (g_date >= 0) {
        inner[ni++] = 0x08;
        inner[ni++] = (unsigned char) date_choices[g_date].value;
    }
    inner[ni++] = 0x10; /* typ: tylko filmy */
    inner[ni++] = 1;
    if (g_length >= 0) {
        inner[ni++] = 0x18;
        inner[ni++] = (unsigned char) length_choices[g_length].value;
    }
    for (i = 0; i < N_CHOICES(feature_choices); i++) {
        if (g_features[i]) {
            inner[ni++] = (unsigned char) feature_choices[i].value;
            inner[ni++] = 1;
        }
    }

    if (g_sort >= 0) {
        msg[nm++] = 0x08;
        msg[nm++] = (unsigned char) sort_choices[g_sort].value;
    }
    msg[nm++] = 0x12;
    msg[nm++] = (unsigned char) ni;
    memcpy(msg + nm, inner, ni);
    nm += ni;

    Base64Encode(msg, nm, out, outsz);
}

static void
BuildSearchUrl(const char *query, char *out, size_t outsz)
{
    char enc_query[3 * QUERY_LEN + 1];
    char sp[64], enc_sp[3 * sizeof(sp)];

    UrlEncode(query, enc_query, sizeof(enc_query));
    BuildSearchParam(sp, sizeof(sp));
    UrlEncode(sp, enc_sp, sizeof(enc_sp));
    snprintf(out, outsz, "https://www.youtube.com/results?search_query=%s&sp=%s",
             enc_query, enc_sp);
}

/* Format yt-dlp dla wybranej jakosci, "" gdy brak wyboru. */
static void
QualityFormat(char *out, size_t outsz)
{
    int h;

    out[0] = '\0';
    if (g_quality < 0)
        return;
    h = quality_choices[g_quality].value;
    snprintf(out, outsz, "bestvideo[height<=?%d]+bestaudio/best[height<=?%d]", h, h);
}

/* -------------------------------------------------------------------- */
/* Wyszukiwanie - proces yt-dlp z potokiem, czytany z petli zdarzen       */
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
CancelSearch(void)
{
    if (g_search_fd >= 0) {
        close(g_search_fd);
        g_search_fd = -1;
    }
    if (g_search_pid > 0) {
        kill(g_search_pid, SIGTERM);
        g_search_pid = -1;
    }
    g_line_len = 0;
}

static void
StartSearch(const char *raw)
{
    static char ytdlp_buf[sizeof(app_data.ytdlp)];
    char url[4 * QUERY_LEN + 128];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    int fds[2];
    pid_t pid;

    while (*raw == ' ' || *raw == '\t')
        raw++;
    if (*raw == '\0') {
        snprintf(g_status, sizeof(g_status), "Enter a search query");
        return;
    }

    CancelSearch();
    g_result_count = 0;
    g_scroll = 0;
    g_selected = -1;

    BuildSearchUrl(raw, url, sizeof(url));
    snprintf(ytdlp_buf, sizeof(ytdlp_buf), "%s", app_data.ytdlp);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS - 9, ytdlp_buf);
    if (argc == 0) {
        snprintf(g_status, sizeof(g_status), "Error: empty 7aTube.ytdlp");
        return;
    }
    argv[argc++] = "--flat-playlist";
    argv[argc++] = "--playlist-end";
    argv[argc++] = SEARCH_COUNT;
    argv[argc++] = "--extractor-args";
    argv[argc++] = "youtubetab:approximate_date";
    argv[argc++] = "--print";
    argv[argc++] = PRINT_TEMPLATE;
    argv[argc++] = url;
    argv[argc] = NULL;

    if (pipe(fds) != 0) {
        snprintf(g_status, sizeof(g_status), "Error: pipe (%s)", strerror(errno));
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        snprintf(g_status, sizeof(g_status), "Error: fork (%s)", strerror(errno));
        return;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(fds[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "7atube: could not run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); /* nie przeciekac do Play/Audio/Save */
    g_search_fd = fds[0];
    g_search_pid = pid;
    snprintf(g_status, sizeof(g_status), "Searching...");
}

static void
FinishSearch(void)
{
    /* proces juz sam sie konczy (EOF) - SIGCHLD = SIG_IGN sprzata go bez
     * wait(), wiec tylko zamykamy potok, bez kill(). */
    close(g_search_fd);
    g_search_fd = -1;
    g_search_pid = -1;
    g_line_len = 0;

    if (g_result_count == 0)
        snprintf(g_status, sizeof(g_status), "No results (is yt-dlp installed?)");
    else
        snprintf(g_status, sizeof(g_status), "%d results", g_result_count);
}

/* Wolane, gdy select() zglosi g_search_fd jako gotowy. Kompletne linie
 * trafiaja do ParseResultLine od razu, wiec wyniki pojawiaja sie w miare
 * naplywania; nadmiar ponad LINE_LEN-1 w jednej linii jest odrzucany. */
static void
PollSearch(void)
{
    char buf[4096];
    ssize_t n, i;

    n = read(g_search_fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        FinishSearch();
        return;
    }

    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            g_line[g_line_len] = '\0';
            ParseResultLine(g_line);
            g_line_len = 0;
        } else if (g_line_len < sizeof(g_line) - 1) {
            g_line[g_line_len++] = buf[i];
        }
    }
    snprintf(g_status, sizeof(g_status), "Searching... %d", g_result_count);
}

/* -------------------------------------------------------------------- */
/* Akcje na wyniku - Play bez terminala, Audio/Save w terminalu (patrz    */
/* naglowek pliku). Wzorzec AppendShellQuoted/RunInTerminal/RunDirect z   */
/* utils/7agroff.c.                                                    */
/* -------------------------------------------------------------------- */

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

/* Dopisuje do out tokeny cmd (np. "mpv --ytdl-format=best"), KAZDY osobno
 * shell-quoted i oddzielony spacja. */
static void
AppendQuotedTokens(char *out, size_t outsz, const char *cmd)
{
    char cmd_buf[256];
    char *tokens[MAX_CMD_TOKENS];
    int argc = 0, i;

    snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
    AppendTokens(tokens, &argc, MAX_CMD_TOKENS, cmd_buf);
    for (i = 0; i < argc; i++) {
        if (out[0] != '\0')
            strncat(out, " ", outsz - strlen(out) - 1);
        AppendShellQuoted(out, outsz, tokens[i]);
    }
}

/* inner_cmd juz zquotowane. hold_always: terminal czeka na Enter takze po
 * sukcesie (Save), inaczej tylko po bledzie (ten sam wzorzec co
 * SpawnCommand w utils/7atodo.c). */
static void
RunInTerminal(const char *inner_cmd, int hold_always)
{
    static char shell_cmd[2048];
    char terminal_buf[256];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;

    if (hold_always)
        snprintf(shell_cmd, sizeof(shell_cmd),
                 "%s; echo; echo '[7atube] finished - press Enter to close'; read -r _",
                 inner_cmd);
    else
        snprintf(shell_cmd, sizeof(shell_cmd),
                 "%s || { echo; echo '[7atube] command failed - check 7aTube.* resources;"
                 " press Enter to close'; read -r _; }", inner_cmd);

    snprintf(terminal_buf, sizeof(terminal_buf), "%s", app_data.terminal);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS - 4, terminal_buf);
    if (argc > 0)
        argv[argc++] = "-e";
    argv[argc++] = "sh";
    argv[argc++] = "-c";
    argv[argc++] = shell_cmd;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    /* SIGCHLD = SIG_IGN w main(), jadro sprzatnie proces potomny samo. */
}

static void
ActionPlay(const Video *v)
{
    char cmd_buf[256];
    char url[96];
    char fmt[96], fmt_opt[112];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;

    VideoUrl(v, url, sizeof(url));
    snprintf(cmd_buf, sizeof(cmd_buf), "%s", app_data.player);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS - 2, cmd_buf);
    if (argc == 0)
        return;
    QualityFormat(fmt, sizeof(fmt));
    if (fmt[0] != '\0') {
        /* po opcjach z 7aTube.player - w mpv wygrywa ostatnia */
        snprintf(fmt_opt, sizeof(fmt_opt), "--ytdl-format=%s", fmt);
        argv[argc++] = fmt_opt;
    }
    argv[argc++] = url;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "7atube: could not run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
}

static void
ActionAudio(const Video *v)
{
    char inner[768] = "";
    char url[96];

    VideoUrl(v, url, sizeof(url));
    AppendQuotedTokens(inner, sizeof(inner), app_data.player);
    strncat(inner, " --no-video ", sizeof(inner) - strlen(inner) - 1);
    AppendShellQuoted(inner, sizeof(inner), url);
    RunInTerminal(inner, 0);
}

static void
ActionSave(const Video *v)
{
    char inner[2 * DIR_LEN + 512] = ""; /* 2x quoted_dir + yt-dlp + -f + URL */
    char quoted_dir[DIR_LEN + 8] = "";
    char url[96];
    char fmt[96];
    size_t len;

    VideoUrl(v, url, sizeof(url));
    AppendShellQuoted(quoted_dir, sizeof(quoted_dir), app_data.download_dir);
    snprintf(inner, sizeof(inner), "mkdir -p %s && cd %s &&", quoted_dir, quoted_dir);
    /* AppendQuotedTokens dokleja spacje sam, bo inner nie jest pusty */
    AppendQuotedTokens(inner, sizeof(inner), app_data.ytdlp);
    if (g_save_audio)
        snprintf(fmt, sizeof(fmt), "bestaudio");
    else
        QualityFormat(fmt, sizeof(fmt));
    if (fmt[0] != '\0') {
        strncat(inner, " -f ", sizeof(inner) - strlen(inner) - 1);
        AppendShellQuoted(inner, sizeof(inner), fmt);
    }
    len = strlen(inner);
    snprintf(inner + len, sizeof(inner) - len, " ");
    AppendShellQuoted(inner, sizeof(inner), url);
    RunInTerminal(inner, 1);
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                             */
/* -------------------------------------------------------------------- */

static int
BoxHeightForRows(int n_rows)
{
    return n_rows * ROW_H + (n_rows - 1) * BOX_GAP + 2 * BOX_BORDER_W + 2 * BOX_PAD_TB;
}

/* Bitmapa wysrodkowana w r. */
#define DRAW_ICON(ctx, r, name, c) \
    ui_draw_bitmap((ctx), (r).x + ((r).w - name##_width) / 2, (r).y + ((r).h - name##_height) / 2, \
               name##_width, name##_height, name##_bits, (c))

/* Zakladki nad boxem zaczynajacym sie na y = TAB_H - wolane PO jego
 * ui_box_begin (patrz ui_tabs w ui.h). */
static void
DrawTabs(UiCtx *ctx, const UiBoxStyle *style, int win_w)
{
    static const char *labels[TAB_COUNT] = { "Search", "Configuration" };
    UiRect r = { style->margin_l + TAB_INDENT, style->margin_t,
                 win_w - style->margin_l - style->margin_r - 2 * TAB_INDENT, TAB_H };

    ui_tabs(ctx, r, labels, TAB_COUNT, &g_tab, style);
}

/* Box z jednym wierszem: "Search:" + pole tekstowe + przycisk "Search",
 * wzorem DrawNewFileBox w utils/7agroff.c. Zakladki nad nim. Zwraca y
 * pod boxem. */
static int
DrawSearchBox(UiCtx *ctx, const UiBoxStyle *style, int win_w)
{
    static int label_w = 0, btn_w = 0;
    UiBox *box;
    UiRect row, l, m, r;
    int submit;

    if (label_w == 0) {
        label_w = ui_text_width(ctx, "Search:") + 4;
        btn_w = ui_button_width(ctx, "Search");
    }

    box = ui_box_begin(ctx, "search", 0, TAB_H, win_w, style);
    DrawTabs(ctx, style, win_w);
    row = ui_box_next_rect(box, ROW_H);

    ui_rect_split3(row, label_w, btn_w, COL_GAP, &l, &m, &r);

    ui_label(ctx, l, "Search:");
    submit = ui_textbox_key(ctx, g_query, XK_Return) ||
             ui_textbox_key(ctx, g_query, XK_KP_Enter);
    ui_textbox(ctx, m, g_query, sizeof(g_query), &g_query_cursor);
    if (ui_button(ctx, r, "Search"))
        submit = 1;
    if (submit) {
        StartSearch(g_query);
        ui_set_focus(ctx, NULL); /* dalej klawiatura steruje lista */
    }

    ui_box_end(box);
    return TAB_H + style->margin_t + ui_box_height(ctx, "search") + style->margin_b;
}

/* Szerokosc kolumny checkboxow na zakladce Configuration: najszerszy
 * ui_checkbox z etykieta (kwadrat r.h-4 + 6px + tekst, patrz ui_checkbox
 * w ui.c) ze WSZYSTKICH grup plus odstep - ta sama dla kazdego wiersza,
 * wiec checkboxy ukladaja sie w kolumny. Liczona raz (font sie nie
 * zmienia w trakcie dzialania). */
static int
ColumnWidth(UiCtx *ctx)
{
    static const struct { const Choice *ch; int n; } groups[] = {
        { sort_choices, N_CHOICES(sort_choices) },
        { date_choices, N_CHOICES(date_choices) },
        { length_choices, N_CHOICES(length_choices) },
        { feature_choices, N_CHOICES(feature_choices) },
        { quality_choices, N_CHOICES(quality_choices) },
        { save_choices, N_CHOICES(save_choices) },
    };
    static int col_w = 0;
    int g, i;

    if (col_w > 0)
        return col_w;
    for (g = 0; g < N_CHOICES(groups); g++)
        for (i = 0; i < groups[g].n; i++) {
            int w = (ROW_H - 4) + 6 + ui_text_width(ctx, groups[g].ch[i].label) + CHECK_GAP;

            if (w > col_w)
                col_w = w;
        }
    return col_w;
}

/* Grupa WYLACZNA: *sel aktualizowany przez ui_hit_test PRZED
 * narysowaniem calej grupy - inaczej odznaczenie pozycji narysowanej
 * wczesniej w tej klatce byloby widac dopiero w nastepnej (wzorzec
 * "najpierw stan, potem rysowanie", patrz CLAUDE.md). ui_checkbox tylko
 * rysuje kopie stanu. Zwraca 1, jesli wybor sie zmienil. */
static int
DrawChoiceGroup(UiCtx *ctx, UiRect row, int x, int label_w, const char *title,
                const Choice *ch, int n, int *sel)
{
    UiRect title_r = { x, row.y, label_w, row.h };
    int changed = 0, i, cx;

    for (i = 0, cx = x + label_w; i < n; i++) {
        int w = ColumnWidth(ctx);
        UiRect r = { cx, row.y, w - CHECK_GAP, row.h };

        if (ui_hit_test(ctx, r)) {
            *sel = (*sel == i) ? -1 : i;
            changed = 1;
        }
        cx += w;
    }

    ui_label(ctx, title_r, title);
    for (i = 0, cx = x + label_w; i < n; i++) {
        int w = ColumnWidth(ctx);
        UiRect r = { cx, row.y, w - CHECK_GAP, row.h };
        int state = (*sel == i);

        ui_checkbox(ctx, r, ch[i].label, &state);
        cx += w;
    }
    return changed;
}

/* Grupa niezalezna - zwykle ui_checkbox na tablicy stanow. */
static int
DrawFlagGroup(UiCtx *ctx, UiRect row, int x, int label_w, const char *title,
              const Choice *ch, int n, int *states)
{
    UiRect title_r = { x, row.y, label_w, row.h };
    int changed = 0, i, cx;

    ui_label(ctx, title_r, title);
    for (i = 0, cx = x + label_w; i < n; i++) {
        int w = ColumnWidth(ctx);
        UiRect r = { cx, row.y, w - CHECK_GAP, row.h };

        if (ui_checkbox(ctx, r, ch[i].label, &states[i]))
            changed = 1;
        cx += w;
    }
    return changed;
}

/* Zakladka Configuration: jeden box - tytul, 4 grupy filtrow
 * wyszukiwania, 2 ustawienia odtwarzania/pobierania, pod nimi pusty
 * wiersz-wypelniacz do dolu okna (box rozciagniety w pionie). Zakladki
 * nad nim. */
static void
DrawConfigBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int win_h)
{
    UiBox *box;
    UiRect row;
    int label_w, x, search_changed = 0, fill_h;

    label_w = ui_text_width(ctx, "Quality:") + 8;

    box = ui_box_begin(ctx, "config", 0, TAB_H, win_w, style);
    DrawTabs(ctx, style, win_w);

    row = ui_box_next_rect(box, ROW_H);
    x = row.x;
    ui_label(ctx, row, "Search filters");

    row = ui_box_next_rect(box, ROW_H);
    search_changed |= DrawChoiceGroup(ctx, row, x, label_w, "Sort:",
                                      sort_choices, N_CHOICES(sort_choices), &g_sort);
    row = ui_box_next_rect(box, ROW_H);
    search_changed |= DrawChoiceGroup(ctx, row, x, label_w, "Date:",
                                      date_choices, N_CHOICES(date_choices), &g_date);
    row = ui_box_next_rect(box, ROW_H);
    search_changed |= DrawChoiceGroup(ctx, row, x, label_w, "Length:",
                                      length_choices, N_CHOICES(length_choices), &g_length);
    row = ui_box_next_rect(box, ROW_H);
    search_changed |= DrawFlagGroup(ctx, row, x, label_w, "Has:",
                                    feature_choices, N_CHOICES(feature_choices), g_features);

    row = ui_box_next_rect(box, ROW_H);
    DrawChoiceGroup(ctx, row, x, label_w, "Quality:",
                    quality_choices, N_CHOICES(quality_choices), &g_quality);
    row = ui_box_next_rect(box, ROW_H);
    DrawFlagGroup(ctx, row, x, label_w, "Save:", save_choices, 1, &g_save_audio);

    /* dol tresci boxa = dol okna - margines - ramka - padding */
    fill_h = win_h - style->margin_b - style->border_w - style->padding_b -
             (row.y + row.h) - style->gap;
    if (fill_h > 0)
        ui_box_next_rect(box, fill_h);

    ui_box_end(box);

    /* StartSearch sam odrzuca puste zapytanie, ale wtedy nadpisalby
     * status "Enter a search query" przy samym klikaniu filtrow */
    if (search_changed) {
        const char *q = g_query;

        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '\0')
            StartSearch(g_query);
    }
}

/* Kolumny wiersza od prawej: data | kanal, tytul dostaje reszte. Komorki
 * stykaja sie bez odstepu - naglowek rysuje je jako wypukle pola
 * ui_header_cell, a wiersze danych wcinaja tekst o UI_CELL_PAD (CellText),
 * zeby trzymal sie naglowka i nie dotykal linii siatki na granicach.
 * Szerokosc daty liczona raz (font sie nie zmienia w trakcie dzialania). */
typedef struct {
    UiRect title, channel, date;
} RowLayout;

static RowLayout
LayoutRow(UiCtx *ctx, UiRect row)
{
    static int date_w = 0;
    RowLayout lo;
    int x = row.x + row.w;
    int channel_w, title_w;

    if (date_w == 0)
        date_w = ui_text_width(ctx, "0000-00-00") + 2 * UI_CELL_PAD;

    x -= date_w;
    lo.date = (UiRect) { x, row.y, date_w, row.h };

    channel_w = (x - row.x) / 4;
    if (channel_w < 0) channel_w = 0;
    x -= channel_w;
    lo.channel = (UiRect) { x, row.y, channel_w, row.h };

    title_w = x - row.x;
    if (title_w < 0) title_w = 0;
    lo.title = (UiRect) { row.x, row.y, title_w, row.h };
    return lo;
}

/* Obszar tekstu w komorce wiersza - wciety jak etykieta ui_header_cell. */
static UiRect
CellText(UiRect c)
{
    return (UiRect) { c.x + UI_CELL_PAD, c.y, c.w - 2 * UI_CELL_PAD, c.h };
}

/* Kropki stron na torze scrollbara - przeniesione z DrawScrollDots w
 * utils/7acenter.c, z przewijaniem w wierszach (g_scroll) zamiast pikseli.
 * Strona = VISIBLE_ROWS wierszy; ostatnia kropka = koniec listy. */
static int
ScrollDotsCurrent(int pages, int max_scroll)
{
    if (g_scroll >= max_scroll) return pages - 1;
    return (g_scroll + VISIBLE_ROWS / 2) / VISIBLE_ROWS;
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
DrawScrollDots(UiCtx *ctx, UiRect track, int max_scroll)
{
    int pages = 1 + (max_scroll + VISIBLE_ROWS - 1) / VISIBLE_ROWS;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = ScrollDotsStart(visible, pages, ScrollDotsCurrent(pages, max_scroll));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            g_scroll = (start + i) * VISIBLE_ROWS;
            if (g_scroll > max_scroll) g_scroll = max_scroll;
            break;
        }
    }

    current = ScrollDotsCurrent(pages, max_scroll);
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
 * g_scroll zmieniany PRZED rysowaniem wierszy, wiec ta sama klatka
 * pokazuje juz nowe przewiniecie. */
static void
DrawResultsScrollbar(UiCtx *ctx, UiRect sb, int max_scroll)
{
    UiRect up_r = { sb.x, sb.y, sb.w, ROW_H };
    UiRect down_r = { sb.x, sb.y + sb.h - ROW_H, sb.w, ROW_H };
    UiRect track_r = { sb.x, sb.y + ROW_H, sb.w, sb.h - 2 * ROW_H };
    int up_clicked = ui_button(ctx, up_r, "");
    int down_clicked = ui_button(ctx, down_r, "");

    ui_draw_arrow(ctx, up_r, UI_ARROW_UP);
    ui_draw_arrow(ctx, down_r, UI_ARROW_DOWN);
    if (up_clicked) g_scroll--;
    if (down_clicked) g_scroll++;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    if (track_r.h > 0)
        DrawScrollDots(ctx, track_r, max_scroll);
}

static int
DrawResultsBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y, int mx, int my)
{
    UiBox *box;
    RowLayout hdr_lo;
    int max_scroll, i;

    max_scroll = g_result_count - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    /* Zaznaczenie PRZED rysowaniem wierszy (wzorzec "najpierw stan",
     * patrz DrawChoiceGroup) - inaczej poprzednio zaznaczony wiersz,
     * narysowany wyzej w tej samej klatce, zostalby podswietlony do
     * nastepnej. Geometria wierszy z g_list_r poprzedniej klatki (stala
     * wysokosc wierszy, wiec ta sama co w biezacej). */
    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect r = { g_list_r.x, g_list_r.y + i * (ROW_H + style->gap), g_list_r.w, ROW_H };

        if (g_scroll + i < g_result_count && ui_hit_test(ctx, r))
            g_selected = g_scroll + i;
    }

    box = ui_box_begin(ctx, "results", 0, y, win_w, style);

    {
        UiRect hdr = ui_box_next_rect(box, ROW_H);
        int list_h = VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap;
        /* scrollbar na prawo od wierszy, na wysokosc listy (pod naglowkiem);
         * naglowek zweza sie tak samo, zeby kolumny trzymaly sie wierszy */
        UiRect sb_r = { hdr.x + hdr.w - SCROLLBAR_W, hdr.y + ROW_H + style->gap,
                        SCROLLBAR_W, list_h };
        /* pasek naglowka do 1px od ramki boxa, takze nad scrollbarem -
         * skrajne komorki rozciagniete do jego brzegow, granice kolumn i
         * etykiety (nad tekstem kolumn) bez zmian */
        UiRect strip = ui_box_header_strip(box, hdr);
        int right = strip.x + strip.w;

        hdr.w -= SCROLLBAR_W + COL_GAP;
        hdr_lo = LayoutRow(ctx, hdr);
        ui_header_cell(ctx, (UiRect) { strip.x, strip.y, hdr_lo.channel.x - strip.x, strip.h },
                       hdr_lo.title.x + UI_CELL_PAD, "Title");
        ui_header_cell(ctx, (UiRect) { hdr_lo.channel.x - 1, strip.y, hdr_lo.date.x - hdr_lo.channel.x + 1, strip.h },
                       hdr_lo.channel.x + UI_CELL_PAD, "Channel");
        ui_header_cell(ctx, (UiRect) { hdr_lo.date.x - 1, strip.y, right - hdr_lo.date.x + 1, strip.h },
                       hdr_lo.date.x + UI_CELL_PAD, "Date");
        DrawResultsScrollbar(ctx, sb_r, max_scroll);
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = g_scroll + i;
        RowLayout lo;
        const Video *v;

        row.w -= SCROLLBAR_W + COL_GAP;
        lo = LayoutRow(ctx, row);
        if (i == 0)
            g_list_r = (UiRect) { row.x, row.y, row.w,
                                  VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap };
        if (idx >= g_result_count)
            continue;

        v = &g_results[idx];
        /* zaznaczony i hover = wypelnienie accent, jak w 7atodo */
        if (idx == g_selected ||
            (mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h))
            ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
        ui_label_ellipsis(ctx, CellText(lo.title), v->title);
        ui_label_ellipsis(ctx, CellText(lo.channel), v->channel);
        ui_label(ctx, CellText(lo.date), v->date);
    }

    /* siatka: pionowe linie na granicach kolumn przez cala liste (pod
     * naglowkiem), PO wierszach - inaczej wypelnienie zaznaczonego wiersza
     * by je przerwalo */
    {
        int y0 = g_list_r.y, y1 = g_list_r.y + g_list_r.h - 1;

        ui_draw_line(ctx, hdr_lo.channel.x - 1, y0, hdr_lo.channel.x - 1, y1, 1, ui_theme_line_fg(ctx));
        ui_draw_line(ctx, hdr_lo.date.x - 1, y0, hdr_lo.date.x - 1, y1, 1, ui_theme_line_fg(ctx));
    }

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "results") + style->margin_b;
}

/* Jedna para "Etykieta:" + wartosc w komorce c boxa "info". */
static void
DrawInfoField(UiCtx *ctx, UiRect c, int label_w, const char *label, const char *value)
{
    UiRect l = { c.x, c.y, label_w, c.h };
    UiRect v = { c.x + label_w, c.y, c.w - label_w, c.h };

    if (v.w < 0) v.w = 0;
    ui_label(ctx, l, label);
    ui_label_ellipsis(ctx, v, value);
}

/* Szczegoly zaznaczonego filmu (patrz naglowek pliku) - INFO_ROWS wierszy:
 * tytul na cala szerokosc, pod nim pary w dwoch kolumnach. Bez
 * zaznaczenia te same wiersze z pustymi wartosciami. */
static int
DrawInfoBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    static int label_w = 0;
    const Video *v = NULL;
    UiBox *box;
    UiRect row;
    char url[96] = "";

    if (label_w == 0)
        label_w = ui_text_width(ctx, "Duration:") + 8;
    if (g_selected >= 0 && g_selected < g_result_count) {
        v = &g_results[g_selected];
        VideoUrl(v, url, sizeof(url));
    }

    box = ui_box_begin(ctx, "info", 0, y, win_w, style);

    row = ui_box_next_rect(box, ROW_H);
    DrawInfoField(ctx, row, label_w, "Title:", v ? v->title : "No video selected");

    row = ui_box_next_rect(box, ROW_H);
    DrawInfoField(ctx, ui_rect_col(row, 0, 2, COL_GAP), label_w, "Channel:", v ? v->channel : "");
    DrawInfoField(ctx, ui_rect_col(row, 1, 2, COL_GAP), label_w, "Duration:", v ? v->duration : "");

    row = ui_box_next_rect(box, ROW_H);
    DrawInfoField(ctx, ui_rect_col(row, 0, 2, COL_GAP), label_w, "Date:", v ? v->date : "");
    DrawInfoField(ctx, ui_rect_col(row, 1, 2, COL_GAP), label_w, "Views:", v ? v->views : "");

    row = ui_box_next_rect(box, ROW_H);
    DrawInfoField(ctx, ui_rect_col(row, 0, 2, COL_GAP), label_w, "Type:", v ? v->live : "");
    DrawInfoField(ctx, ui_rect_col(row, 1, 2, COL_GAP), label_w, "URL:", url);

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "info") + style->margin_b;
}

/* Play zaznaczonego wiersza - wspolne dla przycisku i Enter. */
static void
PlaySelected(void)
{
    if (g_selected < 0 || g_selected >= g_result_count) {
        snprintf(g_status, sizeof(g_status), "Select a video first");
        return;
    }
    ActionPlay(&g_results[g_selected]);
    ShowBusy("Starting player...", BUSY_PLAY_MS);
}

/* Pasek na dole okna: Play/Audio/Save dla zaznaczonego wiersza + status
 * (wzorem paska Compile/Reload/Help w utils/7agroff.c). */
static void
DrawActionRow(UiCtx *ctx, int win_w, int y)
{
    static const char *const labels[] = { "Play", "Audio", "Save" };
    int wmargin = ui_window_margin(ctx);
    int x = wmargin, i;

    for (i = 0; i < 3; i++) {
        UiRect r = { x, y, ui_button_width(ctx, labels[i]), ROW_H };

        x += r.w + COL_GAP;
        if (!ui_button(ctx, r, labels[i]))
            continue;
        if (g_selected < 0 || g_selected >= g_result_count) {
            snprintf(g_status, sizeof(g_status), "Select a video first");
            continue;
        }
        if (i == 0) {
            PlaySelected();
        } else if (i == 1) {
            ActionAudio(&g_results[g_selected]);
            ShowBusy("Opening terminal...", BUSY_TERM_MS);
        } else {
            ActionSave(&g_results[g_selected]);
            ShowBusy("Starting download...", BUSY_TERM_MS);
        }
    }

    {
        UiRect status_r = { x + 2, y, win_w - x - 2 - wmargin, ROW_H };

        ui_label_ellipsis(ctx, status_r, g_status);
    }
}

/* Klepsydra wysrodkowana w r: co BUSY_FRAME_MS na zmiane pelna u gory i
 * pelna u dolu (krok z zegara, bez stanu - immediate-mode). */
static void
DrawHourglass(UiCtx *ctx, UiRect r)
{
    const XColor *c = ui_theme_icon_fg(ctx);

    if ((now_ms() / BUSY_FRAME_MS) % 2 == 0)
        DRAW_ICON(ctx, r, hourglass_top, c);
    else
        DRAW_ICON(ctx, r, hourglass_bottom, c);
}

/* Popup na srodku okna, rysowany jako OSTATNI element klatki (na
 * wierzchu listy). No-op, gdy nic nie trwa. */
static void
DrawBusyPopup(UiCtx *ctx, int win_w, int win_h)
{
    const char *text;
    const char *hint = NULL;
    int text_w, w, h = 64;
    UiRect box, icon_r, text_r;

    if (!BusyActive())
        return;
    if (g_search_fd >= 0) {
        text = "Running YouTube query...";
        hint = "Esc to cancel";
    } else {
        text = g_busy_text;
    }

    text_w = ui_text_width(ctx, text);
    if (hint && ui_text_width(ctx, hint) > text_w)
        text_w = ui_text_width(ctx, hint);
    w = 16 + hourglass_top_width + 14 + text_w + 20;
    box = (UiRect) { (win_w - w) / 2, (win_h - h) / 2, w, h };

    ui_fill_round_rect(ctx, box, ui_theme_box_bg(ctx));
    ui_draw_round_border(ctx, box, 2, ui_theme_accent(ctx));

    icon_r = (UiRect) { box.x + 16, box.y, hourglass_top_width, h };
    DrawHourglass(ctx, icon_r);

    if (hint) {
        text_r = (UiRect) { icon_r.x + icon_r.w + 14, box.y + 12, text_w, ROW_H };
        ui_label(ctx, text_r, text);
        text_r.y += ROW_H;
        ui_label(ctx, text_r, hint);
    } else {
        text_r = (UiRect) { icon_r.x + icon_r.w + 14, box.y, text_w, h };
        ui_label(ctx, text_r, text);
    }
}

static void
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style, tab_style, info_style;
    static int ready = 0;
    int y, mx, my;

    if (!ready) {
        style = (UiBoxStyle) { 0 };
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = BOX_MARGIN_TB;
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = BOX_PAD_TB;
        style.border_w = BOX_BORDER_W;
        style.gap = BOX_GAP;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        tab_style = style;
        tab_style.transparent = 1;
        info_style = style;
        info_style.bg_color = *ui_theme_box_info_bg(ctx);
        ready = 1;
    }

    if (g_tab == TAB_CONFIG) {
        DrawConfigBox(ctx, &tab_style, win_w, win_h);
    } else {
        ui_mouse_state(ctx, &mx, &my, NULL);
        y = DrawSearchBox(ctx, &tab_style, win_w);
        y = DrawResultsBox(ctx, &style, win_w, y, mx, my);
        y = DrawInfoBox(ctx, &info_style, win_w, y);
        /* przyciski przy dolnej krawedzi okna, ale nigdy na boxie "info" */
        if (y < win_h - ROW_H - BOX_MARGIN_TB)
            y = win_h - ROW_H - BOX_MARGIN_TB;
        DrawActionRow(ctx, win_w, y);
    }

    DrawBusyPopup(ctx, win_w, win_h);
}

static int
ComputeContentHeight(void)
{
    int search_h = BOX_MARGIN_TB + BoxHeightForRows(1) + BOX_MARGIN_TB;
    int results_h = BOX_MARGIN_TB + BoxHeightForRows(1 + VISIBLE_ROWS) + BOX_MARGIN_TB;
    int info_h = BOX_MARGIN_TB + BoxHeightForRows(INFO_ROWS) + BOX_MARGIN_TB;

    return TAB_H + search_h + results_h + info_h + ROW_H + BOX_MARGIN_TB;
}

static int
InRect(UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Przesuniecie zaznaczenia o delta wierszy (z przycieciem do listy) i
 * przewiniecie tak, zeby zaznaczony wiersz byl widoczny. Bez zaznaczenia
 * start od pierwszego widocznego wiersza. */
static void
MoveSelection(int delta)
{
    if (g_result_count == 0)
        return;
    if (g_selected < 0 || g_selected >= g_result_count)
        g_selected = g_scroll;
    else
        g_selected += delta;
    if (g_selected < 0) g_selected = 0;
    if (g_selected >= g_result_count) g_selected = g_result_count - 1;
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + VISIBLE_ROWS) g_scroll = g_selected - VISIBLE_ROWS + 1;
}

/* Skroty listy (pole wyszukiwania bez fokusu, patrz naglowek pliku).
 * Zwraca 1, gdy klawisz byl skrotem - wtedy NIE idzie do ui_feed_event. */
static int
HandleListKey(UiCtx *ctx, XKeyEvent *kev)
{
    int ctrl = (kev->state & ControlMask) != 0;
    KeySym ks = XLookupKeysym(kev, (kev->state & ShiftMask) ? 1 : 0);

    if (ctrl) {
        if (ks == XK_d) MoveSelection(VISIBLE_ROWS / 2);
        else if (ks == XK_u) MoveSelection(-VISIBLE_ROWS / 2);
        else if (ks == XK_f) MoveSelection(VISIBLE_ROWS);
        else if (ks == XK_b) MoveSelection(-VISIBLE_ROWS);
        else return 0;
        return 1;
    }
    switch (ks) {
    case XK_j: case XK_Down:     MoveSelection(1); break;
    case XK_k: case XK_Up:       MoveSelection(-1); break;
    case XK_g: case XK_Home:     MoveSelection(-MAX_RESULTS); break;
    case XK_G: case XK_End:      MoveSelection(MAX_RESULTS); break;
    case XK_Next:                MoveSelection(VISIBLE_ROWS); break;
    case XK_Prior:               MoveSelection(-VISIBLE_ROWS); break;
    case XK_slash:               ui_set_focus(ctx, g_query); break;
    case XK_Return: case XK_KP_Enter:
        if (!BusyActive()) /* jak polykanie klikniec - bez wielokrotnego Play */
            PlaySelected();
        break;
    default:
        return 0;
    }
    return 1;
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
    int win_w = 760, win_h;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw, clicked = 0;
    char app_name[64] = "7aTube";
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

    /* yt-dlp/mpv/terminal sa fire-and-forget (albo konczone przez EOF
     * potoku, patrz FinishSearch) - jadro sprzata je bez wait(). */
    signal(SIGCHLD, SIG_IGN);

#ifdef __OpenBSD__
    /* rpath - baza zasobow X/fonty; proc exec - yt-dlp/mpv/terminal. Siec
     * i zapis na dysk robia WYLACZNIE procesy potomne po exec (pledge bez
     * execpromises ich nie ogranicza), wiec sama apka nie potrzebuje
     * inet/wpath/cpath. BEZ unveil - z tego samego powodu co 7arss.c
     * (dziedziczony po exec okaleczylby mpv/yt-dlp). */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7atube: brak polaczenia z X11\n");
        return 1;
    }

    ui_resource_string(dpy, "7aTube.player", "7aTube.Player", app_data.player, sizeof(app_data.player), "mpv");
    ui_resource_string(dpy, "7aTube.ytdlp", "7aTube.Ytdlp", app_data.ytdlp, sizeof(app_data.ytdlp), "yt-dlp");
    ui_resource_string(dpy, "7aTube.terminal", "7aTube.Terminal", app_data.terminal, sizeof(app_data.terminal), "urxvt");
    ui_resource_string(dpy, "7aTube.downloadDir", "7aTube.DownloadDir", dir_raw, sizeof(dir_raw), "~/Videos");
    ExpandHome(dir_raw, app_data.download_dir, sizeof(app_data.download_dir));

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
        ch->res_class = "7aTube";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    /* Szerokosc swobodna (tytuly sa dowolnej dlugosci, ui_label_ellipsis
     * je przycina), min_height = dokladna wysokosc zawartosci - wzorem
     * utils/7agroff.c. */
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
        fprintf(stderr, "7atube: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, smart_display_48_width, smart_display_48_height, smart_display_48_bits);
    ui_set_focus(ctx, g_query); /* od razu mozna pisac zapytanie */

    /* Dwie klatki PRZED petla glowna - zapelnienie cache wysokosci boxow,
     * patrz ten sam wzorzec w utils/7agroff.c/7aping.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    running = 1;
    redraw = 1;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy (Button4/5) przechwycone PRZED ui_feed_event -
             * ten sam wzorzec co w utils/7askm.c: ui.c nie rozroznia
             * numeru przycisku, wiec kolko kliknelaby Play pod kursorem.
             * "continue" jest tu poprawne (wewnetrzna petla XPending,
             * redraw i tak wykonuje sie po jej wyjsciu). */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress && InRect(g_list_r, ev.xbutton.x, ev.xbutton.y)) {
                    g_scroll += (ev.xbutton.button == Button4) ? -1 : 1;
                    redraw = 1;
                }
                continue;
            }

            /* Popup widoczny = klikniecia polykane (bez ui_feed_event),
             * patrz naglowek pliku - ochrona przed wielokrotnym Play.
             * Tylko ButtonPress: klik w ui.c to Release po Press, wiec to
             * wystarcza, a Release dalej trafia do ui.c i nie zostawia
             * mouse_down=1 po wcisnieciu sprzed pojawienia sie popupu. */
            if (ev.type == ButtonPress && BusyActive())
                continue;

            /* Skroty listy i wyjscie z pola Escape'em - PRZED
             * ui_feed_event, patrz naglowek pliku. Na zakladce
             * Configuration lista nie reaguje. */
            if (ev.type == KeyPress && g_tab == TAB_SEARCH) {
                if (!ui_has_focus(ctx, g_query) && HandleListKey(ctx, &ev.xkey)) {
                    redraw = 1;
                    continue;
                }
                if (ui_has_focus(ctx, g_query) &&
                    XLookupKeysym(&ev.xkey, 0) == XK_Escape && g_search_fd < 0) {
                    ui_set_focus(ctx, NULL);
                    redraw = 1;
                    continue;
                }
            }

            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
                break;
            case ButtonRelease:
                clicked = 1;
                redraw = 1;
                break;
            case ButtonPress:
            case MotionNotify:
                redraw = 1;
                break;
            case KeyPress: {
                /* Escape poza polem (z pola wychodzi wyzej, przed
                 * ui_feed_event) - skroty listy tez obsluzone wyzej */
                KeySym ks = XLookupKeysym(&ev.xkey, 0);

                if (ks == XK_Escape) {
                    if (g_search_fd >= 0) {
                        CancelSearch();
                        snprintf(g_status, sizeof(g_status), "Search cancelled");
                    } else {
                        running = 0;
                    }
                }
                redraw = 1;
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
            /* po kliku 3 klatki - patrz naglowek pliku */
            for (i = clicked ? 3 : 1; i > 0; i--) {
                ui_begin_frame(ctx);
                draw(ctx, win_w, win_h);
                ui_end_frame(ctx);
            }
            redraw = clicked = 0;
        }

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy);
            int maxfd = xfd;
            struct timeval tv, *tvp = NULL;
            int ready;

            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            if (g_search_fd >= 0) {
                FD_SET(g_search_fd, &rfds);
                if (g_search_fd > maxfd) maxfd = g_search_fd;
            }
            /* timeout tylko przy widocznym popupie - kolejna klatka
             * klepsydry albo jego znikniecie; poza tym czekamy w
             * nieskonczonosc jak dotad */
            if (BusyActive()) {
                tv.tv_sec = 0;
                tv.tv_usec = BUSY_FRAME_MS * 1000;
                tvp = &tv;
            }
            ready = select(maxfd + 1, &rfds, NULL, NULL, tvp);
            if (ready > 0 && g_search_fd >= 0 && FD_ISSET(g_search_fd, &rfds)) {
                PollSearch();
                redraw = 1;
            }
            if (tvp)
                redraw = 1;
        }
    }

    CancelSearch();
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
