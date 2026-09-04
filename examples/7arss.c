/*
 * 7arss.c - nowa apka na biblioteke ui.c/ui.h (nie port istniejacego Xt/Xaw
 * oryginalu, w odroznieniu od reszty examples/7a*.c - tu nie ma czego
 * portowac). Wzorzec zaczerpniety z dwoch juz istniejacych apek:
 *  - examples/7aweather.c: RunCommand/UrlEncode (popen+curl) oraz timer
 *    odswiezania przez select() na deskryptorze polaczenia X, zamiast
 *    XtAppAddTimeOut.
 *  - examples/7atodo.c: header ze strzalkami </> i licznikiem "(N) X/Y" do
 *    stronicowania listy (tu: stalej dlugosci 5 wierszy/strone, zamiast
 *    wyliczanej z wysokosci okna jak w todo, bo wymaganie to konkretnie
 *    "5 wierszy najnowszych wiadomosci").
 *
 * Zrodlo danych: Google News RSS (feed 2.0, plaskie <item><title>/<link>),
 * po polsku (hl=pl-PL&gl=PL&ceid=PL:pl). Parsowanie jest celowo najprostsze
 * z mozliwych (strstr na <item>/</item>, potem na <title>/<link> w obrebie
 * jednego itemu) - RSS 2.0 tych feedow nie zagniezdza <item> ani nie owija
 * title/link w CDATA, wiec pelny parser XML byłby tu przewymiarowanym
 * rozwiazaniem (patrz KISS w CLAUDE.md). DecodeEntities obsluguje tylko
 * encje faktycznie wystepujace w tytulach (&amp; &quot; &#39; ...) - to i
 * tak jedyne, jakie Google News tam wstawia.
 *
 * Tematy (News top stories/Linux+BSD/Archeologia) nie sa przelaczane
 * zakladkami ani dropdownem - UpdateFeed() po prostu pobiera WSZYSTKIE
 * trzy feedy po kolei (topic_queries[] nizej) i dopisuje ich pozycje do
 * jednej wspolnej listy g_titles/g_links, ktora potem jest stronicowana
 * jak dotychczas. Prostszy interfejs (bez zadnego dodatkowego widgetu do
 * wyboru tematu), kosztem 3 sekwencyjnych requestow curl zamiast jednego
 * przy kazdym odswiezeniu (raz na REFRESH_INTERVAL_MS, wiec akceptowalne).
 *
 * Dodatkowa kolumna w kazdym wierszu ("Open") odpala firefoksa z URL-em
 * artykulu - fork+execlp, fire-and-forget, ten sam wzorzec co
 * DaySelected/ResolveTodoCommand w examples/7acal.c (SIGCHLD = SIG_IGN w
 * main(), jadro sprzata proces potomny samo).
 */

#define _DEFAULT_SOURCE  /* popen/execlp/fork sa POSIX - patrz ta sama uwaga w examples/7aweather.c */

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

#define ICON_SIZE 32
#define ROW_H 20
#define ROWS_PER_PAGE 5      /* "5 wierszy najnowszych wiadomosci" - stale, nie z wysokosci okna */
#define OPEN_COL_W 50        /* szerokosc kolumny z przyciskiem "Open" */
#define TEXT_GAP 6           /* odstep miedzy tekstem naglowka a kolumna "Open" */
/* szerokosc PIONOWEGO paska nawigacji stron (patrz draw()) - dobrana tak,
 * zeby content_w boxa (SIDEBAR_W - 2*border_w - padding_l - padding_r,
 * czyli SIDEBAR_W-14 przy obecnym stylu) wyszedl rowny ROW_H (20px), tyle
 * co kwadratowy przycisk "<"/">" - bez tego zostawal zbedny luz po bokach,
 * przez co pasek wygladal na szerszy niz potrzeba. */
#define SIDEBAR_W 34
/* odstep miedzy paskiem nawigacji a lista wiadomosci - musi byc rowny
 * style.margin_b (patrz draw()), bo TYLE (nie margin_t+margin_b) wynosi
 * faktyczny odstep miedzy stackowanymi pionowo boxami w reszcie apek
 * (margin_t kolejnego boxa jest tam znoszony trikiem "y - style.margin_t"
 * przy jego ui_box_begin) - te dwie wartosci maja pozostac zsynchronizowane. */
#define SIDEBAR_GAP 6
#define MAX_ITEMS 20         /* laczny limit wpisow PO POLACZENIU 3 tematow - stale bufory, bez malloc */
#define TITLE_LEN 256
#define LINK_LEN 512
#define RSS_BUF_SIZE (128 * 1024)
#define REFRESH_INTERVAL_MS 300000  /* 5 min */

/* NULL -> feed "top stories" (bez /search), inaczej rss/search?q=... -
 * wszystkie pobierane i mieszane w jedna liste, patrz UpdateFeed(). */
static const char *const topic_queries[] = { NULL, "linux OR bsd", "archeologia" };
#define TOPIC_COUNT (int) (sizeof(topic_queries) / sizeof(topic_queries[0]))

/* Limit pozycji DOPISYWANYCH z jednego tematu - bez tego pierwszy temat
 * (top stories, zwykle najwiecej pozycji w surowym feedzie) zapelnilby
 * caly MAX_ITEMS, zanim FetchTopic() w ogole doszedlby do Linux/BSD czy
 * Archeologii. Zaokraglone w gore, zeby 3 tematy realnie sumowaly sie do
 * MAX_ITEMS, nie mniej. */
#define ITEMS_PER_TOPIC ((MAX_ITEMS + TOPIC_COUNT - 1) / TOPIC_COUNT)

static char g_titles[MAX_ITEMS][TITLE_LEN];
static char g_links[MAX_ITEMS][LINK_LEN];
static int g_item_count = 0;
static int g_page = 0;
static char g_status[128] = "Loading...";

static char g_rss_buf[RSS_BUF_SIZE];

/* -------------------------------------------------------------------- */
/* Pobieranie feedu - RunCommand/UrlEncode bez zmian wzgledem             */
/* examples/7aweather.c (to zwykle funkcje na char*, niezalezne od        */
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
/* Parsowanie RSS - patrz komentarz na gorze pliku o zakresie/zalozeniach */
/* -------------------------------------------------------------------- */

static void
DecodeEntities(const char *in, char *out, size_t outsz)
{
    static const struct { const char *ent; char ch; } table[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
        { "&quot;", '"' }, { "&apos;", '\'' }, { "&#39;", '\'' },
    };
    size_t oi = 0;

    while (*in && oi + 1 < outsz) {
        if (*in == '&') {
            size_t i;
            int matched = 0;

            for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
                size_t elen = strlen(table[i].ent);

                if (strncmp(in, table[i].ent, elen) == 0) {
                    out[oi++] = table[i].ch;
                    in += elen;
                    matched = 1;
                    break;
                }
            }
            if (!matched)
                out[oi++] = *in++;
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = '\0';
}

/* block = tresc JEDNEGO <item>...</item> (NUL-terminated, patrz ParseFeed).
 * Google News nie owija title/link w CDATA, ale inne generatory RSS
 * potrafia - obsluzone na wszelki wypadek, zeby parser nie byl krucho
 * przywiazany do jednego konkretnego zrodla. */
static int
ExtractTagContent(const char *block, const char *tag, char *out, size_t outsz)
{
    char open_tag[32], close_tag[32];
    const char *p, *q;
    char raw[2048];
    size_t len;

    out[0] = '\0';
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    p = strstr(block, open_tag);
    if (!p)
        return -1;
    p += strlen(open_tag);
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
    return 0;
}

/* DOPISUJE do g_item_count, nie resetuje go - wywolywana raz na kazdy z
 * topic_queries[] w UpdateFeed(), zeby polaczyc wszystkie 3 tematy w jedna
 * wspolna liste. max_new ogranicza, ile pozycji WOLNO dopisac z TEGO
 * jednego wywolania (patrz ITEMS_PER_TOPIC) - bez tego pierwszy temat
 * zapelnilby caly MAX_ITEMS, zanim doszlibysmy do kolejnych. */
static void
ParseFeed(const char *buf, int max_new)
{
    const char *cursor = buf;
    int added = 0;

    while (g_item_count < MAX_ITEMS && added < max_new) {
        const char *item_start = strstr(cursor, "<item>");
        const char *item_end;
        char item_buf[4096];
        size_t len;

        if (!item_start)
            break;
        item_start += strlen("<item>");
        item_end = strstr(item_start, "</item>");
        if (!item_end)
            break;

        len = (size_t) (item_end - item_start);
        if (len >= sizeof(item_buf))
            len = sizeof(item_buf) - 1;
        memcpy(item_buf, item_start, len);
        item_buf[len] = '\0';

        if (ExtractTagContent(item_buf, "title", g_titles[g_item_count], TITLE_LEN) == 0 &&
            ExtractTagContent(item_buf, "link", g_links[g_item_count], LINK_LEN) == 0) {
            g_item_count++;
            added++;
        }

        cursor = item_end + strlen("</item>");
    }
}

/* Jeden temat: buduje URL (top stories albo rss/search?q=...), pobiera i
 * DOPISUJE do ITEMS_PER_TOPIC wpisow do g_titles/g_links przez ParseFeed(). */
static void
FetchTopic(const char *query)
{
    char cmd[768];

    /* -L: news.google.com odpowiada 302 (przekierowanie regionalne) zanim
     * odda faktyczny XML - bez podazania za przekierowaniem curl zwraca
     * puste cialo (HTTP 200/302, size=0), co RunCommand/ParseFeed widzi
     * jako "0 pozycji" - stad status "Brak wiadomosci (blad sieci?)" nawet
     * przy w pelni sprawnej sieci. */
    if (query) {
        char encoded[512];

        UrlEncode(query, encoded, sizeof(encoded));
        snprintf(cmd, sizeof(cmd),
                 "curl -sL --max-time 8 "
                 "'https://news.google.com/rss/search?q=%s&hl=pl-PL&gl=PL&ceid=PL:pl'",
                 encoded);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "curl -sL --max-time 8 "
                 "'https://news.google.com/rss?hl=pl-PL&gl=PL&ceid=PL:pl'");
    }

    RunCommand(cmd, g_rss_buf, RSS_BUF_SIZE);
    ParseFeed(g_rss_buf, ITEMS_PER_TOPIC);
}

static void
UpdateFeed(void)
{
    int t;

    g_item_count = 0;
    for (t = 0; t < TOPIC_COUNT; t++)
        FetchTopic(topic_queries[t]);
    g_page = 0;

    if (g_item_count == 0)
        snprintf(g_status, sizeof(g_status), "Brak wiadomosci (blad sieci?)");
}

/* -------------------------------------------------------------------- */
/* Otwieranie artykulu w Firefoksie - fork+execlp, ten sam wzorzec co     */
/* DaySelected w examples/7acal.c.                                       */
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
/* Ikona okna - ksztalt RSS (kropka + dwa cwiartkowe luki), rysowana      */
/* wprost Xlibem na 1-bitowej Pixmapie, jak w reszcie examples/7a*.c.     */
/* -------------------------------------------------------------------- */

static void
DrawRssIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XFillArc(idpy, p, gc, 3, 23, 6, 6, 0, 360 * 64);
    XDrawArc(idpy, p, gc, -6, 14, 24, 24, 0, 90 * 64);
    XDrawArc(idpy, p, gc, -14, 6, 40, 40, 0, 90 * 64);
}

static Pixmap
MakeRssIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawRssIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI - jedna funkcja per klatka, wzorzec z examples/demo.c       */
/* -------------------------------------------------------------------- */

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static UiBoxStyle style_side;   /* jak style, ale margin_r = SIDEBAR_GAP zamiast window_margin */
    static UiBoxStyle style_main;   /* jak style, ale margin_l = 0 (pasek juz zajal lewy margines) */
    static int ready = 0;
    int y = 0;
    int i;
    int total_pages;
    int sidebar_total_w;
    char page_buf[16], total_buf[16];

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

        style_side = style;
        style_side.margin_r = SIDEBAR_GAP;
        style_main = style;
        style_main.margin_l = 0;
        ready = 1;
    }

    total_pages = (g_item_count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (total_pages < 1) total_pages = 1;
    if (g_page >= total_pages) g_page = total_pages - 1;
    if (g_page < 0) g_page = 0;

    /* Pasek nawigacji PIONOWY (zamiast szerokiego poziomego headera) - ta
     * apka jako jedyna w repo otwiera sie w poziomie (win_w > win_h), wiec
     * odzyskujemy szerokosc kosztem waskiego paska po lewej zamiast
     * peinowszerokosciowego boxa na gorze. Zamiast obracac tekst (ui.c nie
     * ma prymitywu do rysowania obroconego Xft - wymagaloby to nowej
     * funkcji w bibliotece, patrz dyskusja), numer biezacej i ostatniej
     * strony sa po prostu ulozone jeden pod drugim jako zwykly, poziomy
     * tekst - "2" nad "20" zamiast "2/20" w jednej linii. Strzalki "<"/">"
     * - te same znaki co prev/next w reszcie examples/7a*.c (np. 7atodo.c),
     * nie "^"/"v" - mimo pionowego ukladu, dla spojnosci znaczenia znaku
     * w calym repo (< zawsze = poprzednia strona, > = nastepna).
     *
     * Wysokosc tego boxa MUSI wyjsc identyczna jak boxa "content" obok -
     * oba dziela ten sam UiBoxStyle (padding/border/gap), wiec wystarczy,
     * zeby SUMA wysokosci wierszy + gapow miedzy nimi (content_h_accum w
     * ui.c) byla taka sama po obu stronach. Content ma ROWS_PER_PAGE
     * wierszy ROW_H; sidebar ma 4 "wiersze" (strzalka/numer/numer/strzalka)
     * - strzalki zostaja ROW_H (jak wszedzie indziej), a obu etykietom
     * numerow przypada CALA reszta wysokosci po rowno, zamiast wlasnej,
     * malej wysokosci linii tekstu. */
    sidebar_total_w = style.margin_l + SIDEBAR_W + SIDEBAR_GAP;
    {
        int content_inner_h = ROWS_PER_PAGE * ROW_H + (ROWS_PER_PAGE - 1) * style.gap;
        int number_row_h = (content_inner_h - 2 * ROW_H - 3 * style.gap) / 2;

        if (number_row_h < 1) number_row_h = 1;

        UiBox *sidebar = ui_box_begin(ctx, "sidebar", 0, y, sidebar_total_w, &style_side);
        UiRect prev_row = ui_box_next_rect(sidebar, ROW_H);
        UiRect page_r = ui_box_next_rect(sidebar, number_row_h);
        UiRect total_r = ui_box_next_rect(sidebar, number_row_h);
        UiRect next_row = ui_box_next_rect(sidebar, ROW_H);
        /* przyciski KWADRATOWE (ROW_H x ROW_H), jak "<"/">" w reszcie
         * examples/7a*.c - next_rect zwraca pelna szerokosc contentu boxa
         * (tu wieksza niz ROW_H), wiec wycinamy z niej wycentrowany
         * kwadrat zamiast rozciagac przycisk na cala szerokosc paska. */
        UiRect prev_r = { prev_row.x + (prev_row.w - ROW_H) / 2, prev_row.y, ROW_H, ROW_H };
        UiRect next_r = { next_row.x + (next_row.w - ROW_H) / 2, next_row.y, ROW_H, ROW_H };

        if (ui_button(ctx, prev_r, "<") && g_page > 0)
            g_page--;
        if (ui_button(ctx, next_r, ">") && g_page < total_pages - 1)
            g_page++;
        snprintf(page_buf, sizeof(page_buf), "%d", g_page + 1);
        snprintf(total_buf, sizeof(total_buf), "%d", total_pages);
        ui_label_centered(ctx, page_r, page_buf);
        ui_label_centered(ctx, total_r, total_buf);
        ui_box_end(sidebar);
    }

    /* content: ROWS_PER_PAGE wierszy - tytul + kolumna "Open", ktora
     * odpala artykul w Firefoksie (patrz OpenInFirefox powyzej). Zaczyna
     * sie od razu obok paska nawigacji (ten sam y), nie pod nim. */
    UiBox *content = ui_box_begin(ctx, "content", sidebar_total_w, y, win_w - sidebar_total_w, &style_main);

    for (i = 0; i < ROWS_PER_PAGE; i++) {
        int index = g_page * ROWS_PER_PAGE + i;
        UiRect row = ui_box_next_rect(content, ROW_H);
        UiRect text_r, open_r;

        if (index >= g_item_count) {
            if (g_item_count == 0 && i == 0)
                ui_label(ctx, row, g_status);
            continue;
        }

        open_r = (UiRect){ row.x + row.w - OPEN_COL_W, row.y, OPEN_COL_W, row.h };
        text_r = (UiRect){ row.x, row.y, row.w - OPEN_COL_W - TEXT_GAP, row.h };

        ui_label(ctx, text_r, g_titles[index]);
        if (ui_button(ctx, open_r, "Open"))
            OpenInFirefox(g_links[index]);
    }
    ui_box_end(content);

    /* Bez zakladek tematow i bez Refresh/Quit - wszystkie 3 tematy sa juz
     * wymieszane w jedna liste przez UpdateFeed()/FetchTopic(), a feed
     * odswieza sie sam co REFRESH_INTERVAL_MS (patrz timer w main()); okno
     * zamyka sie przez menedzera okien. */
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
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 420, win_h = 165;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    long next_refresh_ms;
    XEvent ev;

    /* -geometry/-geom WxH+X+Y jak w examples/7aweather.c - jedyny obslugiwany
     * argument CLI (temat wybiera sie zakladkami w oknie, patrz topics[]
     * wyzej, nie parametrem wywolania). */
    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        }
    }

    signal(SIGCHLD, SIG_IGN);

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w examples/7afm.c/7aweather.c (patrz
     * komentarze tam): auto-odswiezanie wola popen("curl ...") (potrzebuje
     * szerokiego dostepu jak curl w 7aweather), a "Open" w wierszu
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

    XStoreName(dpy, win, "7aRSS");
    XSetIconName(dpy, win, "7aRSS");

    icon = MakeRssIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    /* min != max na OBU osiach - patrz ten sam komentarz w 7aweather.c
     * (bez tego niektore WM traktuja nieokreslone maksimum jako rowne
     * minimum i blokuja caly uchwyt resize). */
    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 140;
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
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    /* Narysuj OD RAZU jedna klatke z placeholderem "Loading...", ZANIM
     * zaczniemy blokujace UpdateFeed() (curl) - ten sam mechanizm co w
     * examples/7aweather.c (bez tego okno wisialoby puste przez caly czas
     * pobierania feedu). */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    UpdateFeed();

    running = 1;
    redraw = 1;
    next_refresh_ms = now_ms() + REFRESH_INTERVAL_MS;

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
                UpdateFeed();
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
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
