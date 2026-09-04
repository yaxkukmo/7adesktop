/*
 * 7amessage.c - odpowiednik xmessage na bibliotece ui.c/ui.h z tego
 * katalogu: jeden box z tekstem (podanym jako argumenty wiersza polecen,
 * laczone spacja - tak jak robi to xmessage) i pod nim jeden przycisk
 * "Quit". Tekst moze byc wielowierszowy (jawne \n w argumencie LUB
 * automatyczne zawijanie dlugich wierszy do szerokosci okna) - gdy nie
 * miesci sie w dostepnej wysokosci, box dostaje suwak (ten sam wzorzec
 * strzalki+tor+kciuk co siatka plikow w examples/7afm.c).
 *
 * Tlo boxa (boxBackground) i kolor tekstu (foreground) sa juz NIEZALEZNIE
 * konfigurowalne przez baze zasobow X - to nie nowy mechanizm, tylko
 * ui_theme_box_bg()/ui_theme_fg() z ui.c (patrz Xresources.sample), ktore
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

#define ICON_SIZE 32
#define MAX_MSG_WORDS 512   /* ten sam bounded-array idiom co MAX_ALARM_TOKENS w 7atimer.c */
#define MAX_LINES 4096
#define MEASURE_BUF 512
#define SCROLLBAR_W 14
#define BTN_H 20            /* ROW_H z reszty apek (7atimer/7afm) - jednolita wysokosc przyciskow */
#define MARGIN_Y 12         /* odstep od GORNEJ/DOLNEJ krawedzi okna i miedzy boxem a przyciskiem */

typedef struct {
    const char *ptr;
    int len;
} LineSpan;

static char *g_message;                 /* zbudowany raz w main(), zwolniony na koncu */
static LineSpan g_lines[MAX_LINES];
static int g_line_count = 0;

static UiRect g_viewport_r = { 0, 0, 0, 0 };  /* z ostatniej klatki - do kolka myszy w main() */
static int g_scroll_y = 0;
static int g_confirm = 0;  /* -confirm: Yes/No zamiast Quit; Yes=1, No=2 */

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
/* Ikonka - "dymek" tekstowy - ten sam wzorzec co MakeClockIconPixmap w   */
/* examples/7atimer.c: rysowana raz w main() surowym Xlib na 1-bitowej   */
/* Pixmapie.                                                             */
/* -------------------------------------------------------------------- */

static void
DrawMessageIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XPoint tail[3];

    XDrawRectangle(idpy, p, gc, 3, 3, 25, 19);
    XDrawLine(idpy, p, gc, 8, 10, 23, 10);
    XDrawLine(idpy, p, gc, 8, 16, 19, 16);

    tail[0].x = 8;  tail[0].y = 22;
    tail[1].x = 8;  tail[1].y = 28;
    tail[2].x = 14; tail[2].y = 22;
    XFillPolygon(idpy, p, gc, tail, 3, Convex, CoordModeOrigin);
}

static Pixmap
MakeMessageIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawMessageIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

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
    UiRect box_r, text_col_r, sb_r, btn_r;
    UiRect up_arrow_r, down_arrow_r, track_r, thumb_r, above_r, below_r;
    int y0, btn_y, full_text_w, content_h, max_scroll;
    int first_line, last_line, i;

    if (line_h < 1) line_h = 14;

    if (!style_ready) {
        style = (UiBoxStyle){0};
        style.padding_l = style.padding_r = 8;
        style.padding_t = style.padding_b = 8;
        style.border_w = 1;
        style_ready = 1;
    }

    y0 = 5;  /* odstep boxa od GORNEJ krawedzi okna - mniejszy niz MARGIN_Y (dolny/miedzy boxem a przyciskiem) */
    btn_y = win_h - MARGIN_Y - BTN_H;
    if (btn_y < y0 + line_h + 2 * style.padding_t + MARGIN_Y)
        btn_y = y0 + line_h + 2 * style.padding_t + MARGIN_Y;

    box_r = (UiRect){ margin, y0, win_w - 2 * margin, (btn_y - MARGIN_Y) - y0 };
    if (box_r.w < 20) box_r.w = 20;

    /* tlo calego boxa (WRAZ z paddingiem) narysowane TERAZ, przed
     * wyliczeniem text_col_r - inaczej pierscien paddingu miedzy ramka a
     * tekstem zostawalby niewypelniony i przeswitywalby przez niego tlo
     * OKNA (ctx->bg), a nie boxBackground, bo dotychczas wypelniany byl
     * tylko sam text_col_r. */
    ui_fill_rect(ctx, box_r, ui_theme_box_bg(ctx));

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
    if (cache_w != full_text_w || cache_h != text_col_r.h) {
        WrapMessage(ctx, g_message, full_text_w);
        cache_need_scroll = (g_line_count * line_h > text_col_r.h);
        if (cache_need_scroll) {
            int reduced_w = full_text_w - SCROLLBAR_W - 4;

            if (reduced_w < 10) reduced_w = 10;
            WrapMessage(ctx, g_message, reduced_w);
        }
        cache_w = full_text_w;
        cache_h = text_col_r.h;
        g_scroll_y = 0;
    }

    if (cache_need_scroll)
        text_col_r.w = full_text_w - SCROLLBAR_W - 4;
    else
        text_col_r.w = full_text_w;

    content_h = g_line_count * line_h;
    max_scroll = content_h - text_col_r.h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
    if (g_scroll_y < 0) g_scroll_y = 0;

    ui_set_clip(ctx, text_col_r);
    first_line = g_scroll_y / line_h;
    last_line = (g_scroll_y + text_col_r.h) / line_h;
    if (last_line >= g_line_count) last_line = g_line_count - 1;
    for (i = first_line; i <= last_line; i++) {
        UiRect row;
        char buf[MEASURE_BUF];
        int len = g_lines[i].len;

        if (len > (int) sizeof(buf) - 1) len = (int) sizeof(buf) - 1;
        memcpy(buf, g_lines[i].ptr, (size_t) len);
        buf[len] = '\0';

        row = (UiRect){ text_col_r.x, text_col_r.y + i * line_h - g_scroll_y, text_col_r.w, line_h };
        ui_label(ctx, row, buf);
    }
    ui_clear_clip(ctx);

    g_viewport_r = box_r;

    if (cache_need_scroll) {
        int arrow_h = 14;

        sb_r = (UiRect){ text_col_r.x + text_col_r.w + 4, text_col_r.y, SCROLLBAR_W, text_col_r.h };
        up_arrow_r = (UiRect){ sb_r.x, sb_r.y, sb_r.w, arrow_h };
        down_arrow_r = (UiRect){ sb_r.x, sb_r.y + sb_r.h - arrow_h, sb_r.w, arrow_h };
        track_r = (UiRect){ sb_r.x, sb_r.y + arrow_h, sb_r.w, sb_r.h - 2 * arrow_h };
        if (track_r.h < 0) track_r.h = 0;

        if (ui_button(ctx, up_arrow_r, "^")) g_scroll_y -= line_h;
        if (ui_button(ctx, down_arrow_r, "v")) g_scroll_y += line_h;
        if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
        if (g_scroll_y < 0) g_scroll_y = 0;

        {
            int thumb_h = (content_h > 0) ? (track_r.h * text_col_r.h) / content_h : track_r.h;

            if (thumb_h < 10) thumb_h = 10;
            if (thumb_h > track_r.h) thumb_h = track_r.h;
            thumb_r.x = track_r.x;
            thumb_r.y = track_r.y + (max_scroll > 0 ? (track_r.h - thumb_h) * g_scroll_y / max_scroll : 0);
            thumb_r.w = track_r.w;
            thumb_r.h = thumb_h;
        }
        above_r = (UiRect){ track_r.x, track_r.y, track_r.w, thumb_r.y - track_r.y };
        below_r = (UiRect){ track_r.x, thumb_r.y + thumb_r.h, track_r.w,
                             track_r.y + track_r.h - (thumb_r.y + thumb_r.h) };

        ui_fill_rect(ctx, track_r, ui_theme_bg(ctx));
        ui_draw_border(ctx, track_r, 1, ui_theme_line_fg(ctx));
        ui_fill_rect(ctx, thumb_r, ui_theme_accent(ctx));

        if (above_r.h > 0 && ui_hit_test(ctx, above_r)) g_scroll_y -= text_col_r.h;
        if (below_r.h > 0 && ui_hit_test(ctx, below_r)) g_scroll_y += text_col_r.h;
        if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
        if (g_scroll_y < 0) g_scroll_y = 0;
    }

    ui_draw_border(ctx, box_r, style.border_w, ui_theme_line_fg(ctx));

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
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 360, win_h = 160;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    const char *msg_words[MAX_MSG_WORDS];
    int msg_word_count = 0;
    size_t msg_total = 0;
    int i;
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
            continue;
        }
        if (msg_word_count < MAX_MSG_WORDS) {
            msg_words[msg_word_count++] = argv[i];
            msg_total += strlen(argv[i]) + 1;  /* +1 na spacje/separator */
        }
    }

    if (msg_word_count == 0) {
        fprintf(stderr, "Usage: %s [-geometry WxH+X+Y] [-confirm] <message...>\n", argv[0]);
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
     * sciezek co examples/7aclip.c:388-415 (X11/fontconfig/Xauthority) -
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
    XStoreName(dpy, win, "7aMessage");
    XSetIconName(dpy, win, "7aMessage");

    icon = MakeMessageIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

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
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        free(g_message);
        return 1;
    }

    running = 1;
    redraw = 1;
    exit_code = 0;

    while (running) {
        XNextEvent(dpy, &ev);

        /* Kolko myszy (Button4/5) przechwycone TU, PRZED ui_feed_event -
         * ten sam powod co w examples/7afm.c: ui.c nie rozroznia numeru
         * przycisku, wiec para ButtonPress/Release od kolka zostalaby
         * policzona jak zwykly klik na tym, co akurat jest pod kursorem
         * (np. przycisk Quit). */
        if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
            (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
            if (ev.type == ButtonPress &&
                ev.xbutton.x >= g_viewport_r.x && ev.xbutton.x < g_viewport_r.x + g_viewport_r.w &&
                ev.xbutton.y >= g_viewport_r.y && ev.xbutton.y < g_viewport_r.y + g_viewport_r.h) {
                g_scroll_y += (ev.xbutton.button == Button4) ? -ui_line_height(ctx) : ui_line_height(ctx);
                if (g_scroll_y < 0) g_scroll_y = 0;
                redraw = 1;
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
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    free(g_message);
    return exit_code;
}
