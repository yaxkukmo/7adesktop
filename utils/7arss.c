/*
 * 7arss.c - nowa apka na biblioteke ui.c/ui.h (nie port istniejacego Xt/Xaw
 * oryginalu, w odroznieniu od reszty utils/7a*.c - tu nie ma czego
 * portowac). Wzorzec zaczerpniety z juz istniejacych apek:
 *  - utils/7aweather.c: RunCommand/UrlEncode (popen+curl) oraz timer
 *    odswiezania przez select() na deskryptorze polaczenia X, zamiast
 *    XtAppAddTimeOut.
 *  - utils/7atube.c: lista o stalej liczbie widocznych wierszy
 *    (VISIBLE_ROWS) przewijana kolkiem myszy i scrollbarem po prawej
 *    (rysowane strzalki gora/dol + kropki stron, DrawScrollbar/
 *    DrawScrollDots przeniesione stamtad, z g_scroll per zrodlo; te same
 *    funkcje przewijaja tez box opisu).
 *  - utils/7afilm.c/7acenter.c: zakladki ui_tabs na gornej krawedzi boxa.
 *
 * Zrodla = zakladki: Wykop, Hejto, Reddit, Google (News). Kazde ma wlasna
 * liste feedow z zasobow X (ParseSources, patrz Xresources.sample):
 *  - 7aRSS.wykop:  URL-e feedow rozdzielone spacjami (domyslnie
 *                  https://wykop.pl/rss - glowna; jest tez .../rss/upcoming).
 *  - 7aRSS.hejto:  jak wykop (domyslnie https://hejto.pl/rss.xml - gorace,
 *                  jedyny znaleziony feed Hejto).
 *  - 7aRSS.reddit: subreddity rozdzielone spacjami, kazdy moze byc
 *                  "a+b+c" (multireddit Reddita = JEDEN request) - domyslnie
 *                  jeden, bo Reddit szybko odpowiada 429 na serie requestow.
 *  - 7aRSS.google: zapytania Google News rozdzielone ';' (zapytanie moze
 *                  miec spacje), "top" = top stories bez /search.
 * Pozycje z kilku feedow jednego zrodla sa mieszane w jedna liste (limit
 * na feed = MAX_ITEMS / liczba feedow, zeby pierwszy nie zajal calosci).
 * UpdateFeeds() pobiera WSZYSTKIE zrodla przy kazdym odswiezeniu (raz na
 * REFRESH_INTERVAL_MS), wiec przelaczenie zakladki nie czeka na siec.
 *
 * Parsowanie jest celowo najprostsze z mozliwych (strstr, bez parsera
 * XML - KISS w CLAUDE.md): RSS 2.0 (Wykop, Google: <item><title><link>)
 * i Atom (Reddit: <entry><title><link href="..."/>). Element jest
 * NUL-terminowany w miejscu w g_rss_buf zamiast kopiowany - wpis Reddita
 * ma przed <title> kilka KB <content>, stala kopia by go ucinala.
 * DecodeEntities zna podstawowe encje XML i numeryczne &#N;/&#xN;
 * (zamieniane na UTF-8 - Reddit pisze tak apostrofy).
 *
 * Kazdy URL laduje w popen("curl ... 'URL'"), wiec przy wczytywaniu
 * konfiguracji odrzucany jest URL z apostrofem/znakiem sterujacym, nazwa
 * subreddita spoza [A-Za-z0-9_+], a zapytanie Google idzie przez UrlEncode.
 * -A (User-Agent): Reddit odrzuca domyslny agent curla.
 *
 * Klik w wiersz zaznacza pozycje i pokazuje jej opis (<description>/
 * <content>/<summary>, CDATA, HTML wyciety) w boxie pod lista, przewijany
 * scrollbarem i kolkiem, gdy nie miesci sie w DESC_ROWS; przycisk
 * "Open in Browser" pod tym boxem odpala firefoksa z URL-em artykulu -
 * fork+execlp, fire-and-forget, ten sam wzorzec co DaySelected/
 * ResolveTodoCommand w utils/7acal.c (SIGCHLD = SIG_IGN w main(), jadro
 * sprzata proces potomny samo).
 *
 * Klawiatura (HandleKey, wzorem HandleListKey w utils/7atube.c): j/k/
 * strzalki = poprzednia/nastepna pozycja, g/G/Home/End, PgUp/PgDn i
 * Ctrl-f/b = strona, Ctrl-d/u = pol strony, h/l/Left/Right i Tab/
 * Shift-Tab = zakladki, J/K = przewijanie opisu, Return = Open in Browser,
 * Escape/q = wyjscie.
 *
 * Caly tekst WIDOCZNY w oknie jest po angielsku, jak w reszcie utils/
 * 7a*.c - komentarze zostaja po polsku.
 */

#define _DEFAULT_SOURCE  /* popen/execlp/fork/strtok_r sa POSIX - patrz ta sama uwaga w utils/7aweather.c */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"
#include "icons/rss_feed_48.xbm"

#define ROW_H 20
#define VISIBLE_ROWS 5       /* stala liczba widocznych wierszy - nadmiar przewijany */
#define SCROLLBAR_W ROW_H    /* przyciski strzalek ROW_H x ROW_H, jak w 7atube */
#define SCROLL_DOT_PITCH 14  /* wysokosc komorki jednej kropki na torze scrollbara */
#define SCROLL_DOT_R 4
#define COL_GAP 6            /* odstep lista | scrollbar */
#define DESC_ROWS 4          /* wiersze zawinietego opisu w boxie pod lista */
#define TAB_H 22
#define TAB_INDENT 8         /* odsuniecie zakladek od zaokraglonego rogu boxa */
#define MAX_ITEMS 100        /* limit pozycji NA ZRODLO - stale bufory, bez malloc */
#define MAX_FEEDS 8          /* limit feedow NA ZRODLO z konfiguracji */
#define TITLE_LEN 256
#define LINK_LEN 512
#define DESC_LEN 2048        /* opis po wycieciu HTML; nadmiar ponad DESC_ROWS przewijany */
#define DESC_HTML_LEN 8192   /* surowy opis z HTML-em przed wycieciem tagow */
#define DESC_MAX_LINES 64    /* limit zawinietych wierszy opisu - reszta w ostatnim z "..." */
#define CONF_LEN 1024        /* bufor jednego zasobu 7aRSS.* */
#define RSS_BUF_SIZE (512 * 1024)  /* 100 wpisow Google z HTML-em opisu to ~300 KB */
#define REFRESH_INTERVAL_MS 300000  /* 5 min */

#define GOOGLE_BASE "https://news.google.com/rss"
#define GOOGLE_LOCALE "hl=pl-PL&gl=PL&ceid=PL:pl"

enum { SRC_WYKOP, SRC_HEJTO, SRC_REDDIT, SRC_GOOGLE, SRC_COUNT };

typedef struct {
    char feeds[MAX_FEEDS][LINK_LEN];
    int feed_count;
    char titles[MAX_ITEMS][TITLE_LEN];
    char links[MAX_ITEMS][LINK_LEN];
    char descs[MAX_ITEMS][DESC_LEN];
    int item_count;
    int scroll;              /* pierwszy widoczny wiersz */
    int desc_scroll;         /* pierwszy widoczny wiersz opisu zaznaczonej pozycji */
    int selected;            /* indeks zaznaczonej pozycji, -1 = brak */
    char status[128];
} Source;

static const char *g_tab_labels[SRC_COUNT] = { "Wykop", "Hejto", "Reddit", "Google" };

static Source g_src[SRC_COUNT];
static int g_tab = SRC_WYKOP;
static UiRect g_list_r;      /* obszar wierszy z ostatniej klatki - kolko myszy */
static UiRect g_desc_r;      /* obszar opisu z ostatniej klatki - kolko myszy */

static char g_rss_buf[RSS_BUF_SIZE];

/* -------------------------------------------------------------------- */
/* Pobieranie feedu - RunCommand/UrlEncode bez zmian wzgledem             */
/* utils/7aweather.c (to zwykle funkcje na char*, niezalezne od        */
/* toolkitu).                                                             */
/* -------------------------------------------------------------------- */

static void
RunCommand(const char *cmd, char *out, size_t outsize)
{
    FILE *fp;
    size_t n = 0;

    out[0] = '\0';
    fp = popen(cmd, "r");
    if (!fp)
        return;
    n = fread(out, 1, outsize - 1, fp);
    out[n] = '\0';
    pclose(fp);
}

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

/* -------------------------------------------------------------------- */
/* Konfiguracja zrodel z zasobow X - patrz komentarz na gorze pliku       */
/* -------------------------------------------------------------------- */

/* URL trafia miedzy apostrofy w poleceniu powloki (FetchFeed), wiec
 * apostrof albo znak sterujacy w nim = odrzucenie. */
static void
AddFeed(Source *s, const char *url)
{
    const unsigned char *p;

    for (p = (const unsigned char *) url; *p; p++) {
        if (*p == '\'' || *p < 0x20 || *p == 0x7f) {
            fprintf(stderr, "7arss: skipping feed with unsafe characters: %s\n", url);
            return;
        }
    }
    if (s->feed_count >= MAX_FEEDS || strlen(url) >= LINK_LEN) {
        fprintf(stderr, "7arss: skipping feed (limit %d feeds/%d chars): %s\n",
                MAX_FEEDS, LINK_LEN - 1, url);
        return;
    }
    snprintf(s->feeds[s->feed_count++], LINK_LEN, "%s", url);
}

static void
ParseWykop(Source *s, char *conf)
{
    char *save = NULL, *tok;

    for (tok = strtok_r(conf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        AddFeed(s, tok);
}

static int
ValidSubreddit(const char *name)
{
    for (; *name; name++)
        if (!isalnum((unsigned char) *name) && *name != '_' && *name != '+')
            return 0;
    return 1;
}

static void
ParseReddit(Source *s, char *conf)
{
    char *save = NULL, *tok;
    char url[LINK_LEN];

    for (tok = strtok_r(conf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (!ValidSubreddit(tok)) {
            fprintf(stderr, "7arss: skipping invalid subreddit: %s\n", tok);
            continue;
        }
        /* limit=100 - bez niego Reddit oddaje tylko 25 wpisow */
        snprintf(url, sizeof(url), "https://www.reddit.com/r/%s/.rss?limit=100", tok);
        AddFeed(s, url);
    }
}

static void
ParseGoogle(Source *s, char *conf)
{
    char *save = NULL, *tok;
    char url[LINK_LEN], encoded[LINK_LEN / 2];

    for (tok = strtok_r(conf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *end;

        while (*tok == ' ' || *tok == '\t')
            tok++;
        end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*tok == '\0')
            continue;

        if (strcmp(tok, "top") == 0) {
            snprintf(url, sizeof(url), GOOGLE_BASE "?" GOOGLE_LOCALE);
        } else {
            UrlEncode(tok, encoded, sizeof(encoded));
            snprintf(url, sizeof(url), GOOGLE_BASE "/search?q=%s&" GOOGLE_LOCALE, encoded);
        }
        AddFeed(s, url);
    }
}

static void
ParseSources(Display *dpy)
{
    char conf[CONF_LEN];
    int i;

    for (i = 0; i < SRC_COUNT; i++)
        snprintf(g_src[i].status, sizeof(g_src[i].status), "Loading...");

    ui_resource_string(dpy, "7aRSS.wykop", "7aRSS.Wykop", conf, sizeof(conf),
                       "https://wykop.pl/rss");
    ParseWykop(&g_src[SRC_WYKOP], conf);
    ui_resource_string(dpy, "7aRSS.hejto", "7aRSS.Hejto", conf, sizeof(conf),
                       "https://hejto.pl/rss.xml");
    ParseWykop(&g_src[SRC_HEJTO], conf);    /* tez zwykla lista URL-i RSS */
    ui_resource_string(dpy, "7aRSS.reddit", "7aRSS.Reddit", conf, sizeof(conf),
                       "linux+openbsd+archaeology");
    ParseReddit(&g_src[SRC_REDDIT], conf);
    ui_resource_string(dpy, "7aRSS.google", "7aRSS.Google", conf, sizeof(conf),
                       "top;linux OR bsd;archeologia");
    ParseGoogle(&g_src[SRC_GOOGLE], conf);
}

/* -------------------------------------------------------------------- */
/* Parsowanie RSS/Atom - patrz komentarz na gorze pliku o zakresie        */
/* -------------------------------------------------------------------- */

/* kod Unicode -> UTF-8; zwraca liczbe bajtow (0 = brak miejsca/zly kod) */
static size_t
PutUtf8(unsigned long cp, char *out, size_t room)
{
    if (cp < 0x80 && room >= 1) {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800 && room >= 2) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0x800 && cp < 0x10000 && room >= 3) {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp >= 0x10000 && cp <= 0x10FFFF && room >= 4) {
        out[0] = (char) (0xF0 | (cp >> 18));
        out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char) (0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* "&#39;"/"&#x27;" pod in -> UTF-8 do out; zwraca dlugosc encji w in
 * (0 = to nie jest poprawna encja numeryczna) */
static size_t
DecodeNumeric(const char *in, char *out, size_t room, size_t *written)
{
    const char *p = in + 2;   /* za "&#" */
    char *end;
    unsigned long cp;
    int hex = (*p == 'x' || *p == 'X');

    if (hex)
        p++;
    if (!isxdigit((unsigned char) *p))
        return 0;
    cp = strtoul(p, &end, hex ? 16 : 10);
    if (*end != ';' || cp == 0)
        return 0;
    *written = PutUtf8(cp, out, room);
    if (*written == 0)
        return 0;
    return (size_t) (end + 1 - in);
}

static void
DecodeEntities(const char *in, char *out, size_t outsz)
{
    static const struct { const char *ent; char ch; } table[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
        { "&quot;", '"' }, { "&apos;", '\'' },
        { "&nbsp;", ' ' },   /* w opisach z HTML-em (Reddit) */
    };
    size_t oi = 0;

    while (*in && oi + 1 < outsz) {
        size_t i, elen = 0, written = 0;

        if (*in == '&' && in[1] == '#') {
            elen = DecodeNumeric(in, out + oi, outsz - 1 - oi, &written);
        } else if (*in == '&') {
            for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
                size_t len = strlen(table[i].ent);

                if (strncmp(in, table[i].ent, len) == 0) {
                    out[oi] = table[i].ch;
                    elen = len;
                    written = 1;
                    break;
                }
            }
        }
        if (elen) {
            in += elen;
            oi += written;
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = '\0';
}

/* Poczatek "<tag>"/"<tag atrybuty>" w block (nie "<tagfoo>"); NULL gdy brak. */
static const char *
FindOpenTag(const char *block, const char *tag)
{
    size_t tlen = strlen(tag);
    const char *p = block;

    while ((p = strchr(p, '<')) != NULL) {
        if (strncmp(p + 1, tag, tlen) == 0 &&
            (p[1 + tlen] == '>' || p[1 + tlen] == ' ' || p[1 + tlen] == '/'))
            return p;
        p++;
    }
    return NULL;
}

/* block = tresc JEDNEGO elementu (NUL-terminated, patrz ParseFeed). CDATA
 * obsluzone, bo czesc generatorow RSS owija w nie title/link. */
static int
ExtractTagContent(const char *block, const char *tag, char *out, size_t outsz)
{
    char close_tag[32];
    const char *p, *q;
    char raw[2048];
    size_t len;

    out[0] = '\0';
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    p = FindOpenTag(block, tag);
    if (!p)
        return -1;
    p = strchr(p, '>');
    if (!p || p[-1] == '/')   /* "<link href=.../>" - pusty element */
        return -1;
    p++;
    q = strstr(p, close_tag);
    if (!q)
        return -1;

    len = (size_t) (q - p);
    if (len >= sizeof(raw))
        len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';

    if (strncmp(raw, "<![CDATA[", 9) == 0) {
        char *ce = strstr(raw, "]]>");
        size_t clen = ce ? (size_t) (ce - (raw + 9)) : strlen(raw + 9);

        memmove(raw, raw + 9, clen);
        raw[clen] = '\0';
    }

    DecodeEntities(raw, out, outsz);
    return out[0] ? 0 : -1;
}

/* RSS: <link>URL</link>; Atom: <link href="URL"/> (pierwszy link wpisu). */
static int
ExtractLink(const char *block, char *out, size_t outsz)
{
    const char *p, *href, *q;
    char raw[LINK_LEN];
    size_t len;

    if (ExtractTagContent(block, "link", out, outsz) == 0)
        return 0;

    p = FindOpenTag(block, "link");
    if (!p)
        return -1;
    q = strchr(p, '>');
    href = strstr(p, "href=\"");
    if (!href || (q && href > q))
        return -1;
    href += strlen("href=\"");
    q = strchr(href, '"');
    if (!q)
        return -1;

    len = (size_t) (q - href);
    if (len >= sizeof(raw))
        len = sizeof(raw) - 1;
    memcpy(raw, href, len);
    raw[len] = '\0';
    DecodeEntities(raw, out, outsz);
    return out[0] ? 0 : -1;
}

/* W miejscu: tagi HTML -> spacja, ciagi bialych znakow -> jedna spacja,
 * bez spacji na poczatku i koncu. Wynik nigdy nie jest dluzszy od
 * wejscia, wiec bez osobnego bufora. Niedomkniety tag (opis uciety
 * przez raw[] w ExtractTagContent) wycina reszte tekstu. */
static void
StripHtml(char *s)
{
    char *in = s, *out = s;
    int in_tag = 0, space = 1;  /* space=1: pomin wiodace biale znaki */

    for (; *in; in++) {
        if (*in == '<') in_tag = 1;
        if (in_tag || isspace((unsigned char) *in)) {
            if (*in == '>') in_tag = 0;
            if (!space) *out++ = ' ';
            space = 1;
            continue;
        }
        *out++ = *in;
        space = 0;
    }
    if (out > s && out[-1] == ' ') out--;
    *out = '\0';
}

/* Opis wpisu jako zwykly tekst: RSS <description> (Wykop: CDATA z <img>,
 * Google: lista <a> z powiazanymi artykulami), Atom <content>/<summary>
 * (Reddit: HTML zakodowany encjami). ExtractTagContent dekoduje encje
 * RAZ - to odslania tagi Reddita, wiec dopiero potem StripHtml, a drugie
 * DecodeEntities rozwija encje z samego tekstu (Reddit: "&amp;nbsp;").
 * Stopka Reddita "submitted by /u/... [link] [comments]" ucinana. */
static void
ExtractDescription(const char *block, char *out, size_t outsz)
{
    char html[DESC_HTML_LEN];
    char *footer;

    out[0] = '\0';
    if (ExtractTagContent(block, "description", html, sizeof(html)) != 0 &&
        ExtractTagContent(block, "content", html, sizeof(html)) != 0 &&
        ExtractTagContent(block, "summary", html, sizeof(html)) != 0)
        return;
    StripHtml(html);
    footer = strstr(html, "submitted by /u/");
    if (footer) {
        while (footer > html && footer[-1] == ' ') footer--;
        *footer = '\0';
    }
    DecodeEntities(html, out, outsz);
}

/* DOPISUJE do s->item_count najwyzej max_new pozycji z buf (RSS <item>
 * albo Atom <entry>). buf jest modyfikowany: koniec kazdego elementu
 * dostaje NUL w miejscu, zeby wyszukiwanie title/link nie wyszlo poza
 * niego (bez kopiowania, patrz komentarz na gorze pliku). */
static void
ParseFeed(Source *s, char *buf, int max_new)
{
    const char *tag = strstr(buf, "<item>") ? "item" : "entry";
    char open_tag[16], close_tag[16];
    char *cursor = buf;
    int added = 0;

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    while (s->item_count < MAX_ITEMS && added < max_new) {
        char *start = strstr(cursor, open_tag);
        char *end;

        if (!start)
            break;
        start += strlen(open_tag);
        end = strstr(start, close_tag);
        if (!end)
            break;
        *end = '\0';

        if (ExtractTagContent(start, "title", s->titles[s->item_count], TITLE_LEN) == 0 &&
            ExtractLink(start, s->links[s->item_count], LINK_LEN) == 0) {
            ExtractDescription(start, s->descs[s->item_count], DESC_LEN);
            s->item_count++;
            added++;
        }

        cursor = end + strlen(close_tag);
    }
}

static void
FetchFeed(Source *s, const char *url, int max_new)
{
    char cmd[LINK_LEN + 128];

    /* -L: news.google.com odpowiada 302 (przekierowanie regionalne) zanim
     * odda faktyczny XML - bez podazania za przekierowaniem curl zwraca
     * puste cialo, co ParseFeed widzi jako "0 pozycji". URL bez apostrofow
     * (AddFeed), wiec cytowanie '...' jest szczelne. */
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 8 -A '7arss/1.0 (X11 RSS reader)' '%s'", url);
    RunCommand(cmd, g_rss_buf, RSS_BUF_SIZE);
    ParseFeed(s, g_rss_buf, max_new);
}

static void
UpdateSource(Source *s)
{
    int f;

    s->item_count = 0;
    s->scroll = 0;
    s->desc_scroll = 0;
    s->selected = -1;
    if (s->feed_count == 0) {
        snprintf(s->status, sizeof(s->status), "No feeds configured (see Xresources.sample)");
        return;
    }
    /* kazdy feed dostaje rowna czesc tego, co ZOSTALO do MAX_ITEMS
     * (zaokraglone w gore) - feed z mniejsza liczba wpisow oddaje reszte
     * kolejnym, ostatni bierze cala reszte */
    for (f = 0; f < s->feed_count; f++) {
        int left = s->feed_count - f;

        FetchFeed(s, s->feeds[f], (MAX_ITEMS - s->item_count + left - 1) / left);
    }

    if (s->item_count == 0)
        snprintf(s->status, sizeof(s->status), "No items (network error?)");
}

static void
UpdateFeeds(void)
{
    int i;

    for (i = 0; i < SRC_COUNT; i++)
        UpdateSource(&g_src[i]);
}

/* -------------------------------------------------------------------- */
/* Otwieranie artykulu w Firefoksie - fork+execlp, ten sam wzorzec co     */
/* DaySelected w utils/7acal.c.                                       */
/* -------------------------------------------------------------------- */

static void
OpenInFirefox(const char *url)
{
    pid_t pid = fork();

    if (pid == 0) {
        execlp("firefox", "firefox", url, (char *) NULL);
        fprintf(stderr, "7arss: could not run 'firefox %s': %s\n", url, strerror(errno));
        _exit(127);
    }
}

/* -------------------------------------------------------------------- */
/* Warstwa UI - jedna funkcja per klatka, wzorzec z utils/demo.c       */
/* -------------------------------------------------------------------- */

static int
MaxScroll(const Source *s)
{
    int m = s->item_count - VISIBLE_ROWS;

    return m < 0 ? 0 : m;
}

static void
ClampTo(int *scroll, int max)
{
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
}

static void
ClampScroll(Source *s)
{
    ClampTo(&s->scroll, MaxScroll(s));
}

/* Kropki stron na torze scrollbara - przeniesione z DrawScrollDots w
 * utils/7atube.c. Strona = page wierszy (VISIBLE_ROWS listy albo
 * DESC_ROWS opisu); ostatnia kropka = koniec przewijanego tekstu. */
static int
ScrollDotsCurrent(int scroll, int max, int page, int pages)
{
    if (scroll >= max) return pages - 1;
    return (scroll + page / 2) / page;
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
DrawScrollDots(UiCtx *ctx, UiRect track, int *scroll, int max, int page)
{
    int pages = 1 + (max + page - 1) / page;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = ScrollDotsStart(visible, pages, ScrollDotsCurrent(*scroll, max, page, pages));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            *scroll = (start + i) * page;
            ClampTo(scroll, max);
            break;
        }
    }

    current = ScrollDotsCurrent(*scroll, max, page, pages);
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

/* Scrollbar obok listy: strzalka w gore / kropki / strzalka w dol, jak
 * DrawResultsScrollbar w utils/7atube.c. Strzalki rysowane PO ui_button
 * (inaczej tlo przycisku je zakryje), a scroll zmieniany PRZED
 * rysowaniem wierszy, wiec ta sama klatka pokazuje nowe przewiniecie.
 * Wspolny dla listy i opisu: scroll w zakresie 0..max, strona = page. */
static void
DrawScrollbar(UiCtx *ctx, UiRect sb, int *scroll, int max, int page)
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
    ClampTo(scroll, max);

    if (track_r.h > 0)
        DrawScrollDots(ctx, track_r, scroll, max, page);
}

/* Sama GORNA krawedz rowka przezroczystego boxa (bez bokow i dolu), wzorem
 * DrawTabLine w utils/7acenter.c: ramki rysowane na prostokacie wyzszym i
 * szerszym od paska linii, przyciete do niego - rogi i dol wypadaja poza
 * clip. y = gorna krawedz boxa. Rysowac PRZED zakladkami (aktywna
 * wymazuje linie pod soba). */
static void
DrawTopLine(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    int bw = style->border_w;
    int x0 = style->margin_l, w = win_w - style->margin_l - style->margin_r;
    UiRect outer = { x0 - 4 * ROW_H, y, w + 8 * ROW_H, 4 * ROW_H };
    UiRect in = { outer.x + bw, outer.y + bw, outer.w - 2 * bw, outer.h - 2 * bw };

    if (bw <= 0) return;
    ui_set_clip(ctx, (UiRect){ x0, y, w, 2 * bw + 1 });
    ui_draw_bevel(ctx, outer, bw, &style->border_color, 1);
    ui_draw_bevel(ctx, in, bw, &style->border_color, 0);
    ui_draw_inner_bevel(ctx, in, bw, &style->border_color, ui_theme_bg(ctx), 0);
    ui_clear_clip(ctx);
}

static int
InRect(UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Zewnetrzna wysokosc boxa (z marginesami) o zawartosci content_h -
 * liczona wprost, bez ui_box_height, zeby box pod nia nie skakal w
 * pierwszej klatce (cache wysokosci jest z POPRZEDNIEJ klatki). */
static int
BoxOuter(const UiBoxStyle *st, int content_h)
{
    return st->margin_t + 2 * st->border_w + st->padding_t + content_h +
           st->padding_b + st->margin_b;
}

/* Klik w wiersz zaznacza pozycje - PRZED rysowaniem wierszy (wzorzec
 * "najpierw stan", jak w utils/7atube.c), inaczej poprzednio zaznaczony
 * wiersz zostalby podswietlony do nastepnej klatki. Geometria z
 * g_list_r poprzedniej klatki (stala wysokosc wierszy), bez kolumny
 * scrollbara - klik strzalki nie zaznacza wiersza obok. */
static void
UpdateSelection(UiCtx *ctx, Source *s, int gap)
{
    int i;

    for (i = 0; i < VISIBLE_ROWS && s->scroll + i < s->item_count; i++) {
        UiRect r = { g_list_r.x, g_list_r.y + i * (ROW_H + gap),
                     g_list_r.w - SCROLLBAR_W - COL_GAP, ROW_H };

        if (ui_hit_test(ctx, r) && s->selected != s->scroll + i) {
            s->selected = s->scroll + i;
            s->desc_scroll = 0;
        }
    }
}

/* Box z lista tytulow: VISIBLE_ROWS wierszy + scrollbar po prawej.
 * Zaznaczony i pod myszka wiersz = wypelnienie accent, jak w 7atube. */
static void
DrawListBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y, Source *s)
{
    int list_h = VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap;
    int mx, my, i;
    UiBox *box;

    UpdateSelection(ctx, s, style->gap);
    ui_mouse_state(ctx, &mx, &my, NULL);
    box = ui_box_begin(ctx, "content", 0, y, win_w, style);
    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int index = s->scroll + i;

        if (i == 0) {
            /* scrollbar na prawo od wierszy, na wysokosc calej listy */
            UiRect sb_r = { row.x + row.w - SCROLLBAR_W, row.y, SCROLLBAR_W, list_h };

            DrawScrollbar(ctx, sb_r, &s->scroll, MaxScroll(s), VISIBLE_ROWS);
            index = s->scroll;
            g_list_r = (UiRect) { row.x, row.y, row.w, list_h };
        }
        row.w -= SCROLLBAR_W + COL_GAP;

        if (index >= s->item_count) {
            if (s->item_count == 0 && i == 0)
                ui_label(ctx, row, s->status);
            continue;
        }
        if (index == s->selected || InRect(row, mx, my))
            ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
        ui_label_ellipsis(ctx, (UiRect) { row.x + UI_CELL_PAD, row.y,
                                          row.w - 2 * UI_CELL_PAD, row.h },
                          s->titles[index]);
    }
    ui_box_end(box);
}

/* Zawija text po slowach do szerokosci w: start[i]/len[i] = i-ty wiersz
 * (bajty w text). Slowo szersze od w zostaje samo w wierszu (rysowane z
 * wielokropkiem). Po max-1 wierszach ostatni dostaje cala reszte.
 * Zwraca liczbe wierszy. Liczone co klatke - tekst ma najwyzej DESC_LEN
 * bajtow, wiec to kilkaset pomiarow ui_text_width, bez cache. */
static int
WrapLines(UiCtx *ctx, const char *text, int w, int *start, int *len, int max)
{
    char line[DESC_LEN];
    size_t pos = 0, total = strlen(text);
    int n = 0;

    while (*text == ' ') { text++; total--; }
    while (pos < total && n < max) {
        const char *t = text + pos;
        size_t l = 0;

        if (n == max - 1) {
            l = total - pos;
        } else {
            /* dokladaj slowa, dopoki prefiks t[0..end) miesci sie w w */
            while (t[l]) {
                const char *sp = strchr(t + l + (l > 0), ' ');
                size_t end = sp ? (size_t) (sp - t) : strlen(t);

                if (end >= sizeof(line))
                    break;
                memcpy(line, t, end);
                line[end] = '\0';
                if (l > 0 && ui_text_width(ctx, line) > w)
                    break;
                l = end;
            }
            if (l == 0)
                l = total - pos;    /* nie powinno sie zdarzyc - bez petli w miejscu */
        }
        start[n] = (int) pos;
        len[n] = (int) l;
        n++;
        pos += l;
        while (text[pos] == ' ')
            pos++;
    }
    return n;
}

/* Box informacyjny pod lista: opis zaznaczonej pozycji, DESC_ROWS
 * widocznych wierszy od s->desc_scroll + scrollbar po prawej (jak przy
 * liscie, przewijanie tez kolkiem nad opisem), a pod boxem (poza nim)
 * "Open in Browser" przy prawej krawedzi boxa. Zawijanie i scrollbar
 * PRZED rysowaniem wierszy - klik strzalki widoczny w tej samej klatce. */
static void
DrawDescBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y, Source *s)
{
    int has = s->selected >= 0 && s->selected < s->item_count;
    const char *text = "No item selected";
    int bw = ui_button_width(ctx, "Open in Browser");
    int desc_h = DESC_ROWS * ROW_H + (DESC_ROWS - 1) * style->gap;
    int start[DESC_MAX_LINES], len[DESC_MAX_LINES];
    int n = 0, i;
    char line[DESC_LEN];
    UiBox *box;
    UiRect btn;

    if (has)
        text = s->descs[s->selected][0] ? s->descs[s->selected] : "No description";
    while (*text == ' ')
        text++;
    box = ui_box_begin(ctx, "desc", 0, y, win_w, style);
    for (i = 0; i < DESC_ROWS; i++) {
        UiRect r = ui_box_next_rect(box, ROW_H);
        int index;

        if (i == 0) {
            UiRect sb_r = { r.x + r.w - SCROLLBAR_W, r.y, SCROLLBAR_W, desc_h };
            int max;

            n = WrapLines(ctx, text, r.w - SCROLLBAR_W - COL_GAP, start, len,
                          DESC_MAX_LINES);
            max = n > DESC_ROWS ? n - DESC_ROWS : 0;
            ClampTo(&s->desc_scroll, max);    /* kolko w main() nie zna n */
            DrawScrollbar(ctx, sb_r, &s->desc_scroll, max, DESC_ROWS);
            g_desc_r = (UiRect) { r.x, r.y, r.w, desc_h };
        }
        r.w -= SCROLLBAR_W + COL_GAP;
        index = s->desc_scroll + i;
        if (index >= n)
            continue;
        memcpy(line, text + start[index], (size_t) len[index]);
        line[len[index]] = '\0';
        ui_label_ellipsis(ctx, r, line);
    }
    ui_box_end(box);

    /* odstep box / przycisk = margin_b boxa, jak Add/Edit/Del w 7atodo */
    btn = (UiRect) { win_w - style->margin_r - bw,
                     y + BoxOuter(style, desc_h), bw, ROW_H };
    if (ui_button(ctx, btn, "Open in Browser") && has)
        OpenInFirefox(s->links[s->selected]);
}

/* Zaznaczenie o delta pozycji (bez zawijania) + przewiniecie listy tak,
 * zeby zaznaczona byla widoczna - jak MoveSelection w utils/7atube.c.
 * Brak zaznaczenia (-1): pierwszy krok w dol wybiera pierwsza pozycje. */
static void
MoveSelection(Source *s, int delta)
{
    int sel = s->selected;

    if (s->item_count == 0)
        return;
    if (sel < 0)
        sel = delta > 0 ? delta - 1 : 0;
    else
        sel += delta;
    if (sel >= s->item_count) sel = s->item_count - 1;
    if (sel < 0) sel = 0;
    if (sel != s->selected)
        s->desc_scroll = 0;
    s->selected = sel;
    if (sel < s->scroll) s->scroll = sel;
    if (sel >= s->scroll + VISIBLE_ROWS) s->scroll = sel - VISIBLE_ROWS + 1;
    ClampScroll(s);
}

static void
SwitchTab(int delta)
{
    g_tab = (g_tab + delta + SRC_COUNT) % SRC_COUNT;
}

/* Skroty klawiszowe (patrz naglowek pliku). Okno nie ma pol tekstowych,
 * wiec kazdy KeyPress trafia tutaj. Zwraca 1 = zakonczyc program. */
static int
HandleKey(XKeyEvent *kev)
{
    Source *s = &g_src[g_tab];
    int ctrl = (kev->state & ControlMask) != 0;
    KeySym ks = XLookupKeysym(kev, (kev->state & ShiftMask) ? 1 : 0);

    if (ctrl) {
        if (ks == XK_d) MoveSelection(s, VISIBLE_ROWS / 2);
        else if (ks == XK_u) MoveSelection(s, -VISIBLE_ROWS / 2);
        else if (ks == XK_f) MoveSelection(s, VISIBLE_ROWS);
        else if (ks == XK_b) MoveSelection(s, -VISIBLE_ROWS);
        return 0;
    }
    switch (ks) {
    case XK_j: case XK_Down:     MoveSelection(s, 1); break;
    case XK_k: case XK_Up:       MoveSelection(s, -1); break;
    case XK_g: case XK_Home:     MoveSelection(s, -MAX_ITEMS); break;
    case XK_G: case XK_End:      MoveSelection(s, MAX_ITEMS); break;
    case XK_Next:                MoveSelection(s, VISIBLE_ROWS); break;
    case XK_Prior:               MoveSelection(s, -VISIBLE_ROWS); break;
    case XK_J:                   s->desc_scroll++; break;    /* ClampTo w DrawDescBox */
    case XK_K:                   if (s->desc_scroll > 0) s->desc_scroll--; break;
    case XK_l: case XK_Right: case XK_Tab:        SwitchTab(1); break;
    case XK_h: case XK_Left: case XK_ISO_Left_Tab: SwitchTab(-1); break;
    case XK_Return: case XK_KP_Enter:
        if (s->selected >= 0 && s->selected < s->item_count)
            OpenInFirefox(s->links[s->selected]);
        break;
    case XK_Escape: case XK_q:
        return 1;
    }
    return 0;
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style, box_style, info_style;
    static int ready = 0;
    Source *s;
    UiRect tabs_r;
    int y, list_h;

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 6;
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = 4;
        style.border_w = 1;
        style.gap = 2;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        style.transparent = 1;
        /* standardowy box pod linia zakladek: z ramka i tlem */
        box_style = style;
        box_style.transparent = 0;
        info_style = box_style;
        info_style.bg_color = *ui_theme_box_info_bg(ctx);
        ready = 1;
    }

    /* zakladki z sama gorna krawedzia rowka pod nimi (bez bokow i dolu),
     * pod linia standardowy box z lista, pod nim box z opisem. Linia
     * PRZED zakladkami (aktywna wymazuje ja pod soba), zakladki PRZED
     * boxami - klik przelacza zrodlo w tej samej klatce. */
    y = TAB_H + style.margin_t;
    DrawTopLine(ctx, &style, win_w, y);
    tabs_r = (UiRect) { style.margin_l + TAB_INDENT, y - TAB_H,
                        win_w - style.margin_l - style.margin_r - 2 * TAB_INDENT, TAB_H };
    ui_tabs(ctx, tabs_r, g_tab_labels, SRC_COUNT, &g_tab, &style);
    y += 2 * style.border_w + 1;

    s = &g_src[g_tab];
    ClampScroll(s);
    DrawListBox(ctx, &box_style, win_w, y, s);
    list_h = VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * box_style.gap;
    y += BoxOuter(&box_style, list_h);
    DrawDescBox(ctx, &info_style, win_w, y, s);

    /* Bez Refresh/Quit - feed odswieza sie sam co REFRESH_INTERVAL_MS
     * (patrz timer w main()); okno zamyka sie przez menedzera okien. */
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

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    XSizeHints *sizehints;
    /* win_h jest DOKLADNY (wzorem 7askm.c): margin_t(6) + TAB_H(22) +
     * linia pod zakladkami(3) + box: margin_t(6) + border*2(2) +
     * padding_t/b(4+4) + lista(VISIBLE_ROWS(5)*ROW_H(20)+4*gap(2)=108) +
     * margin_b(6) = 130 + box opisu: 6+2+4+4 + DESC_ROWS(4)*ROW_H(20)+
     * 3*gap(2)=86 + margin_b(6) = 108 (odstep jak w 7atodo) + przycisk pod
     * boxem ROW_H(20) + margines dolny(6) = 134 -> 31 + 130 + 134 = 295. */
    int win_w = 420, win_h = 295;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    long next_refresh_ms;
    char app_name[64] = "7aRSS";
    char app_title[64] = "";
    XEvent ev;

    /* -geometry/-geom WxH+X+Y jak w utils/7aweather.c - jedyny obslugiwany
     * argument CLI obok -name/-title (zrodlo wybiera sie zakladkami w oknie,
     * feedy - zasobami 7aRSS.*, patrz ParseSources). */
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

    signal(SIGCHLD, SIG_IGN);

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w utils/7afm.c/7aweather.c (patrz
     * komentarze tam): auto-odswiezanie wola popen("curl ...") (potrzebuje
     * szerokiego dostepu jak curl w 7aweather), a "Open in Browser"
     * fork+exec'uje firefoksa - kolejna duza, zewnetrzna apke, ktora
     * unveil (dziedziczony po exec) by okaleczyl (profil, cache, fonty,
     * biblioteki - ten sam problem co terminal/edytor w 7afm/7atodo). */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    ParseSources(dpy);

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
        ch->res_class = "7aRSS";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    /* min != max na OBU osiach - patrz ten sam komentarz w 7aweather.c
     * (bez tego niektore WM traktuja nieokreslone maksimum jako rowne
     * minimum i blokuja caly uchwyt resize). */
    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = win_h; /* MUSI byc <= win_h - inaczej WM (np. dwm)
                                     * wymusza okno wyzsze niz zawartosc,
                                     * z pusta przestrzenia na dole. */
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    /* USSize/USPosition = "uzytkownik jawnie o to poprosil" (-geometry na
     * linii polecen) - bez tych flag wiele WM (zwlaszcza tiling, np. dwm)
     * ignoruje polozenie/rozmiar przekazany do XCreateSimpleWindow ponizej
     * i same decyduja, gdzie/jak duze ma byc okno. Ustawiane TYLKO gdy
     * -geometry faktycznie podano (geom_mask), zeby normalne uruchomienie
     * bez -geometry nadal w pelni podlegalo polityce WM (tiling itp.),
     * tak jak dotychczas. */
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
        XCloseDisplay(dpy);
        return 1;
    }
    ui_set_window_icon(ctx, rss_feed_48_width, rss_feed_48_height, rss_feed_48_bits);

    /* Narysuj OD RAZU jedna klatke z placeholderem "Loading...", ZANIM
     * zaczniemy blokujace UpdateFeeds() (curl) - ten sam mechanizm co w
     * utils/7aweather.c (bez tego okno wisialoby puste przez caly czas
     * pobierania feedu). */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    UpdateFeeds();

    running = 1;
    redraw = 1;
    next_refresh_ms = now_ms() + REFRESH_INTERVAL_MS;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy (Button4/5) przechwycone PRZED ui_feed_event -
             * wzorzec z utils/7atube.c: ui.c nie rozroznia numeru
             * przycisku, wiec kolko zaznaczyloby wiersz pod kursorem. */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                int step = (ev.xbutton.button == Button4) ? -1 : 1;

                if (ev.type == ButtonPress && InRect(g_list_r, ev.xbutton.x, ev.xbutton.y)) {
                    g_src[g_tab].scroll += step;
                    ClampScroll(&g_src[g_tab]);
                    redraw = 1;
                } else if (ev.type == ButtonPress &&
                           InRect(g_desc_r, ev.xbutton.x, ev.xbutton.y)) {
                    g_src[g_tab].desc_scroll += step;    /* ClampTo w DrawDescBox */
                    redraw = 1;
                }
                continue;
            }

            /* klawisze PRZED ui_feed_event - okno nie ma pol tekstowych */
            if (ev.type == KeyPress) {
                if (HandleKey(&ev.xkey)) running = 0;
                redraw = 1;
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
            long remaining = next_refresh_ms - now_ms();

            if (remaining <= 0) {
                UpdateFeeds();
                next_refresh_ms = now_ms() + REFRESH_INTERVAL_MS;
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
    return 0;
}
