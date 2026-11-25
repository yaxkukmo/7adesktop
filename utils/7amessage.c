/*
 * 7amessage.c - odpowiednik xmessage na bibliotece ui.c/ui.h z tego
 * katalogu: jeden box z tekstem (podanym jako argumenty wiersza polecen,
 * laczone spacja - tak jak robi to xmessage) i pod nim jeden przycisk
 * "Quit". Tekst moze byc wielowierszowy (jawne \n w argumencie LUB
 * automatyczne zawijanie dlugich wierszy do szerokosci okna) - gdy nie
 * miesci sie w dostepnej wysokosci, box dostaje scrollbar przewijany
 * wierszami (strzalki + kropki stron, jak w utils/7arss.c/7atube.c). Po
 * lewej przezroczysty box stalego rozmiaru (rowek jak
 * UiBoxStyle.transparent) z ikona info
 * (domyslnie) albo warning (-warning, a takze -confirm), po prawej
 * box informacyjny z tekstem tej samej wysokosci, pod nimi przyciski;
 * obrysy jak ui_box_begin (ui_draw_bevel, 3D przy borderStyle: 3d).
 *
 * Tlo boxa z tekstem (boxInfoBackground) i kolor tekstu (foreground) sa
 * NIEZALEZNIE konfigurowalne przez baze zasobow X - to nie nowy mechanizm,
 * tylko ui_theme_box_info_bg()/ui_theme_fg() z ui.c (patrz Xresources.sample), ktore
 * ta apka po prostu uzywa tak jak kazdy inny port w tym katalogu.
 *
 * ui.c nie ma wlasnego zawijania tekstu (ui_label rysuje jeden wiersz i
 * przycina go do rect) - dochodzi tu wiec ui_text_width/ui_line_height
 * (dodane do ui.h razem z ta apka), zeby zmierzyc tekst bez otwierania
 * drugiego, niezaleznego XftFont tylko do pomiaru.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"
#include "icons/chat_48.xbm"
#include "icons/info_48.xbm"
#include "icons/warning_48.xbm"

#define MAX_MSG_WORDS 512   /* ten sam bounded-array idiom co MAX_ALARM_TOKENS w 7atimer.c */
#define MAX_LINES 4096
#define MEASURE_BUF 512
#define BTN_H 20            /* ROW_H z reszty apek (7atimer/7afm) - jednolita wysokosc przyciskow */
#define SCROLLBAR_W BTN_H   /* przyciski strzalek BTN_H x BTN_H, jak w 7atube/7arss */
#define SCROLL_DOT_PITCH 14 /* wysokosc komorki jednej kropki na torze scrollbara */
#define SCROLL_DOT_R 4
#define COL_GAP 6           /* odstep tekst | scrollbar */

typedef struct {
    const char *ptr;
    int len;
} LineSpan;

static char *g_message;                 /* zbudowany raz w main(), zwolniony na koncu */
static LineSpan g_lines[MAX_LINES];
static int g_line_count = 0;

static UiRect g_viewport_r = { 0, 0, 0, 0 };  /* z ostatniej klatki - do kolka myszy w main() */
static int g_scroll = 0;   /* indeks pierwszego widocznego wiersza */
static int g_confirm = 0;  /* -confirm: Yes/No zamiast Quit; Yes=1, No=2 */
static int g_warning = 0;  /* -warning (lub -confirm): ikona warning zamiast info */

/* -------------------------------------------------------------------- */
/* Zawijanie tekstu                                                     */
/* -------------------------------------------------------------------- */

/* szerokosc podciagu [s, s+len) - ui_text_width chce NUL-terminated
 * stringa, wiec kopiujemy do ograniczonego bufora na stosie (dluzsze
 * "slowa" po prostu mierzone sa uciete - patrz WrapMessage nizej) */
static int
MeasureWidth(UiCtx *ctx, const char *s, int len)
{
    char buf[MEASURE_BUF];
    int n = len;

    if (n < 0) n = 0;
    if (n > (int) sizeof(buf) - 1) n = (int) sizeof(buf) - 1;
    memcpy(buf, s, (size_t) n);
    buf[n] = '\0';
    return ui_text_width(ctx, buf);
}

/* Dzieli msg na wiersze do g_lines: najpierw po jawnych '\n' (akapity),
 * potem kazdy akapit zachlannie po spacjach do szerokosci wrap_w.
 * Pojedyncze "slowo" dluzsze niz wrap_w NIE jest dzielone znak-po-znaku
 * (swiadomie pominiete - rzadki przypadek, ui_label i tak przytnie je do
 * szerokosci rect przy rysowaniu, wiec nic nie wyjezdza poza box). */
static void
WrapMessage(UiCtx *ctx, const char *msg, int wrap_w)
{
    const char *p = msg;

    g_line_count = 0;
    if (wrap_w < 10)
        wrap_w = 10;

    while (*p && g_line_count < MAX_LINES) {
        const char *para_end = strchr(p, '\n');
        const char *seg_start = p;
        const char *last_break = NULL;
        const char *cur = p;

        if (!para_end)
            para_end = p + strlen(p);

        if (para_end == p) {
            /* pusty akapit (dwa '\n' pod rzad) - jeden pusty wiersz */
            g_lines[g_line_count].ptr = p;
            g_lines[g_line_count].len = 0;
            g_line_count++;
            p = (*para_end == '\n') ? para_end + 1 : para_end;
            continue;
        }

        while (g_line_count < MAX_LINES) {
            int at_end = (cur == para_end);

            if (at_end || *cur == ' ') {
                int seg_len = (int) (cur - seg_start);

                if (MeasureWidth(ctx, seg_start, seg_len) > wrap_w && last_break) {
                    g_lines[g_line_count].ptr = seg_start;
                    g_lines[g_line_count].len = (int) (last_break - seg_start);
                    g_line_count++;
                    seg_start = last_break + 1;
                    last_break = NULL;
                    continue;  /* nie ruszaj cur - przelicz od nowego seg_start */
                }
                if (at_end) {
                    g_lines[g_line_count].ptr = seg_start;
                    g_lines[g_line_count].len = (int) (cur - seg_start);
                    g_line_count++;
                    break;
                }
                last_break = cur;
            }
            cur++;
        }

        p = (*para_end == '\n') ? para_end + 1 : para_end;
    }
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

static void
ClampTo(int *scroll, int max)
{
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
}

/* Kropki stron na torze scrollbara - przeniesione z DrawScrollDots w
 * utils/7arss.c. Strona = page wierszy; ostatnia kropka = koniec tekstu. */
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
DrawScrollDots(UiCtx *ctx, UiRect track, int max, int page)
{
    int pages = 1 + (max + page - 1) / page;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1) return;
    if (visible > pages) visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = ScrollDotsStart(visible, pages, ScrollDotsCurrent(g_scroll, max, page, pages));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };

        if (ui_hit_test(ctx, cell)) {
            g_scroll = (start + i) * page;
            ClampTo(&g_scroll, max);
            break;
        }
    }

    current = ScrollDotsCurrent(g_scroll, max, page, pages);
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

/* Scrollbar obok tekstu: strzalka w gore / kropki / strzalka w dol, jak
 * DrawScrollbar w utils/7arss.c - strzalki przewijaja o jeden wiersz. */
static void
DrawScrollbar(UiCtx *ctx, UiRect sb, int max, int page)
{
    UiRect up_r = { sb.x, sb.y, sb.w, BTN_H };
    UiRect down_r = { sb.x, sb.y + sb.h - BTN_H, sb.w, BTN_H };
    UiRect track_r = { sb.x, sb.y + BTN_H, sb.w, sb.h - 2 * BTN_H };
    int up_clicked = ui_arrow_button(ctx, up_r, UI_ARROW_UP);
    int down_clicked = ui_arrow_button(ctx, down_r, UI_ARROW_DOWN);

    if (up_clicked) g_scroll--;
    if (down_clicked) g_scroll++;
    ClampTo(&g_scroll, max);

    if (track_r.h > 0)
        DrawScrollDots(ctx, track_r, max, page);
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int style_ready = 0;
    /* cache zawijania - przeliczane tylko gdy szerokosc/wysokosc dostepna
     * na tekst faktycznie sie zmieni (np. zmiana rozmiaru okna), nie co
     * klatke */
    static int cache_w = -1, cache_h = -1, cache_need_scroll = 0;

    int margin = ui_window_margin(ctx);
    int line_h = ui_line_height(ctx);
    UiRect icon_r, box_r, text_col_r, btn_r;
    int y0, btn_y, full_text_w, rows, max_scroll;
    int i;

    if (line_h < 1) line_h = 14;

    if (!style_ready) {
        style = (UiBoxStyle){0};
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = 8;
        style.border_w = 1;
        style_ready = 1;
    }

    /* uklad: po lewej przezroczysty box z ikona info/warning, po prawej
     * box informacyjny z tekstem (ta sama wysokosc), pod nimi przyciski.
     * Wszystkie odstepy = ui_window_margin: od krawedzi okna, ikona-tekst,
     * boxy-przyciski. */
    y0 = margin;
    btn_y = win_h - margin - BTN_H;
    if (btn_y < y0 + line_h + 2 * style.padding_t + margin)
        btn_y = y0 + line_h + 2 * style.padding_t + margin;

    /* box ikony ma STALY rozmiar (ikona + padding), przyklejony do gory -
     * z oknem rosnie tylko box z tekstem */
    icon_r.x = margin;
    icon_r.y = y0;
    icon_r.w = info_48_width + 2 * style.padding_l;
    icon_r.h = info_48_height + 2 * style.padding_t;

    box_r.x = icon_r.x + icon_r.w + margin;
    box_r.y = y0;
    box_r.w = win_w - margin - box_r.x;
    box_r.h = (btn_y - margin) - y0;
    if (box_r.w < 20) box_r.w = 20;

    /* box ikony jak ui_box_begin z transparent=1: bez wypelnienia, ramka
     * jako wklesly rowek w 3D, plasko pojedyncza ramka */
    ui_draw_groove(ctx, icon_r, style.border_w, ui_theme_line_fg(ctx), ui_theme_bg(ctx));
    ui_draw_bitmap(ctx, icon_r.x + (icon_r.w - info_48_width) / 2,
                   icon_r.y + (icon_r.h - info_48_height) / 2,
                   info_48_width, info_48_height,
                   g_warning ? warning_48_bits : info_48_bits,
                   ui_theme_icon_fg(ctx));

    /* tlo calego boxa (WRAZ z paddingiem) narysowane TERAZ, przed
     * wyliczeniem text_col_r - inaczej pierscien paddingu miedzy ramka a
     * tekstem zostawalby niewypelniony i przeswitywalby przez niego tlo
     * OKNA (ctx->bg), a nie boxInfoBackground, bo dotychczas wypelniany byl
     * tylko sam text_col_r. */
    ui_fill_round_rect(ctx, box_r, ui_theme_box_info_bg(ctx));

    text_col_r.x = box_r.x + style.padding_l;
    text_col_r.y = box_r.y + style.padding_t;
    text_col_r.h = box_r.h - 2 * style.padding_t;
    if (text_col_r.h < line_h) text_col_r.h = line_h;
    full_text_w = box_r.w - 2 * style.padding_l;
    if (full_text_w < 10) full_text_w = 10;

    /* przelicz zawijanie tylko gdy zmienila sie dostepna szerokosc/
     * wysokosc tekstu - najpierw na pelnej szerokosci (bez suwaka), a
     * jesli tresc i tak nie miesci sie w wysokosci, ponownie na wezszej
     * (z miejscem na suwak) - raz ustalone "potrzebny suwak" juz sie nie
     * cofa w tej samej probie, wiec nie ma oscylacji miedzy dwoma
     * szerokosciami. */
    rows = text_col_r.h / line_h;
    if (rows < 1) rows = 1;
    if (cache_w != full_text_w || cache_h != text_col_r.h) {
        WrapMessage(ctx, g_message, full_text_w);
        cache_need_scroll = (g_line_count > rows);
        if (cache_need_scroll) {
            int reduced_w = full_text_w - SCROLLBAR_W - COL_GAP;

            if (reduced_w < 10) reduced_w = 10;
            WrapMessage(ctx, g_message, reduced_w);
        }
        cache_w = full_text_w;
        cache_h = text_col_r.h;
        g_scroll = 0;
    }

    if (cache_need_scroll)
        text_col_r.w = full_text_w - SCROLLBAR_W - COL_GAP;
    else
        text_col_r.w = full_text_w;

    max_scroll = g_line_count - rows;
    if (max_scroll < 0) max_scroll = 0;
    ClampTo(&g_scroll, max_scroll);

    /* scrollbar PRZED wierszami (jak w 7atube/7arss) - klik strzalki/
     * kropki widac juz w tej samej klatce */
    if (cache_need_scroll) {
        UiRect sb_r = { text_col_r.x + text_col_r.w + COL_GAP, text_col_r.y,
                        SCROLLBAR_W, text_col_r.h };

        DrawScrollbar(ctx, sb_r, max_scroll, rows);
    }

    ui_set_clip(ctx, text_col_r);
    for (i = g_scroll; i < g_scroll + rows && i < g_line_count; i++) {
        UiRect row;
        char buf[MEASURE_BUF];
        int len = g_lines[i].len;

        if (len > (int) sizeof(buf) - 1) len = (int) sizeof(buf) - 1;
        memcpy(buf, g_lines[i].ptr, (size_t) len);
        buf[len] = '\0';

        row = (UiRect){ text_col_r.x, text_col_r.y + (i - g_scroll) * line_h, text_col_r.w, line_h };
        ui_label(ctx, row, buf);
    }
    ui_clear_clip(ctx);

    g_viewport_r = box_r;

    /* obrys jak ui_box_begin (wklesly przy borderStyle: 3d) */
    ui_draw_bevel(ctx, box_r, style.border_w, ui_theme_line_fg(ctx), 1);
    ui_draw_inner_bevel(ctx, box_r, style.border_w, ui_theme_line_fg(ctx),
                        ui_theme_box_info_bg(ctx), 1);

    if (g_confirm) {
        int gap = 10;
        int yes_w = ui_button_width(ctx, "Yes");
        int no_w  = ui_button_width(ctx, "No");
        int x0 = (win_w - yes_w - no_w - gap) / 2;
        UiRect yes_r = { x0,              btn_y, yes_w, BTN_H };
        UiRect no_r  = { x0 + yes_w + gap, btn_y, no_w,  BTN_H };
        if (ui_button(ctx, yes_r, "Yes")) return 1;
        if (ui_button(ctx, no_r,  "No"))  return 2;
    } else {
        int quit_w = ui_button_width(ctx, "Quit");
        btn_r = (UiRect){ (win_w - quit_w) / 2, btn_y, quit_w, BTN_H };
        if (ui_button(ctx, btn_r, "Quit")) return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    XSizeHints *sizehints;
    int win_w = 360, win_h = 160;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    const char *msg_words[MAX_MSG_WORDS];
    int msg_word_count = 0;
    size_t msg_total = 0;
    int i;
    char app_name[64] = "7aMessage";
    char app_title[64] = "";
    int running, redraw, exit_code;
    XEvent ev;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
            continue;
        }
        if (strcmp(argv[i], "-confirm") == 0) {
            g_confirm = 1;
            g_warning = 1;
            continue;
        }
        if (strcmp(argv[i], "-warning") == 0) {
            g_warning = 1;
            continue;
        }
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[i + 1]);
            i++;
            continue;
        }
        if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[i + 1]);
            i++;
            continue;
        }
        if (msg_word_count < MAX_MSG_WORDS) {
            msg_words[msg_word_count++] = argv[i];
            msg_total += strlen(argv[i]) + 1;  /* +1 na spacje/separator */
        }
    }

    if (msg_word_count == 0) {
        fprintf(stderr, "Usage: %s [-geometry WxH+X+Y] [-confirm] [-warning] <message...>\n", argv[0]);
        return 1;
    }

    g_message = malloc(msg_total);
    if (!g_message) {
        fprintf(stderr, "7amessage: brak pamieci\n");
        return 1;
    }
    g_message[0] = '\0';
    {
        size_t off = 0;

        for (i = 0; i < msg_word_count; i++) {
            int n = snprintf(g_message + off, msg_total - off, "%s%s",
                              i > 0 ? " " : "", msg_words[i]);
            if (n > 0)
                off += (size_t) n;
        }
    }

#ifdef __OpenBSD__
    /* unveil + pledge (nie samo pledge) - w odroznieniu od 7afm/7atodo/
     * 7aweather itd. ta apka NIE fork+exec'uje niczego, wiec zawezenie
     * widocznych sciezek nie kolisuje z niczym. Ten sam wzorzec/zestaw
     * sciezek co utils/7aclip.c:388-415 (X11/fontconfig/Xauthority) -
     * to minimum, zeby polaczyc sie z serwerem X i otworzyc font. */
    {
        const char *home = getenv("HOME");
        char home_fontcfg[1024];
        char xauth_buf[1024];
        const char *xauth_path;

        snprintf(home_fontcfg, sizeof(home_fontcfg), "%s/.fontconfig",
                 home ? home : ".");
        if (unveil("/usr", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/etc", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/var", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/tmp/.X11-unix", "rw") == -1) { perror("unveil"); return 1; }
        if (unveil(home_fontcfg, "r")      == -1) { perror("unveil"); return 1; }
        xauth_path = getenv("XAUTHORITY");
        if (!xauth_path) {
            snprintf(xauth_buf, sizeof(xauth_buf), "%s/.Xauthority",
                     home ? home : ".");
            xauth_path = xauth_buf;
        }
        if (unveil(xauth_path, "r")        == -1) { perror("unveil"); return 1; }
        if (unveil(NULL, NULL)             == -1) { perror("unveil"); return 1; }
        if (pledge("stdio rpath unix prot_exec", NULL) == -1) {
            perror("pledge");
            return 1;
        }
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        free(g_message);
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
        ch->res_name  = app_name;
        ch->res_class = "7aMessage";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 160;
    sizehints->min_height = 90;
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
        XCloseDisplay(dpy);
        free(g_message);
        return 1;
    }
    ui_set_window_icon(ctx, chat_48_width, chat_48_height, chat_48_bits);

    running = 1;
    redraw = 1;
    exit_code = 0;

    while (running) {
        XNextEvent(dpy, &ev);

        /* Kolko myszy (Button4/5) przechwycone TU, PRZED ui_feed_event -
         * ten sam powod co w utils/7afm.c: ui.c nie rozroznia numeru
         * przycisku, wiec para ButtonPress/Release od kolka zostalaby
         * policzona jak zwykly klik na tym, co akurat jest pod kursorem
         * (np. przycisk Quit).
         * Bez "continue" na koncu (w odroznieniu od 7askm.c, gdzie ten
         * sam wzorzec zyje w WEWNETRZNYM "while (XPending)" - tam
         * "continue" wraca do sprawdzenia kolejnego pendingowego eventu,
         * a blok redraw ponizej i tak wykonuje sie po wyjsciu z tej
         * petli). Tutaj jest tylko JEDNA petla z blokujacym XNextEvent,
         * wiec "continue" przeskakiwalby prosto do kolejnego XNextEvent,
         * omijajac blok redraw ponizej w tej samej iteracji - scroll
         * ustawial redraw=1, ale okno nie bylo przerysowywane, dopoki
         * nie nadszedl inny event (np. MotionNotify przy ruchu myszka
         * poza viewportem). */
        if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
            (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
            if (ev.type == ButtonPress &&
                ev.xbutton.x >= g_viewport_r.x && ev.xbutton.x < g_viewport_r.x + g_viewport_r.w &&
                ev.xbutton.y >= g_viewport_r.y && ev.xbutton.y < g_viewport_r.y + g_viewport_r.h) {
                /* gorna granica przycinana w draw() (ClampTo) */
                g_scroll += (ev.xbutton.button == Button4) ? -1 : 1;
                if (g_scroll < 0) g_scroll = 0;
                redraw = 1;
            }
        } else {
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
            int ret;

            ui_begin_frame(ctx);
            ret = draw(ctx, win_w, win_h);
            ui_end_frame(ctx);
            redraw = 0;
            if (ret) {
                /* ret==1: Quit lub Yes (exit 0); ret==2: No (exit 1) */
                exit_code = (ret == 2) ? 1 : 0;
                running = 0;
            }
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    free(g_message);
    return exit_code;
}
