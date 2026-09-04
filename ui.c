#include "ui.h"
#include <X11/Xresource.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#define UI_MAX_BOX_DEPTH 8
#define UI_MAX_BOX_CACHE 32
#define UI_BTN_PAD 12  /* padding lewy/prawy tekstu w ui_button - patrz ui_button_width */

typedef struct {
    char id[32];
    int height;
} UiBoxCacheEntry;

struct UiBox {
    UiCtx *ctx;
    char id[32];
    UiBoxStyle style;
    int outer_x, outer_y, outer_w;
    int content_x, content_w;
    int cursor_y;
    int content_h_accum;
    int child_count;
};

struct UiCtx {
    Display *dpy;
    Window win;
    GC gc;
    int screen;
    Pixmap backbuf;   /* cel rysowania - blitowany na win dopiero w ui_end_frame */
    int buf_w, buf_h;
    XFontStruct *font; /* iso10646-1; rysowanie: XDrawString16 z wlasnym UTF-8->UCS-2 */
    int ascent;        /* font->ascent */
    int descent;       /* font->descent */
    XColor fg, bg, accent;
    /* wezsze kategorie kolorow, kazda z osobnym zasobem X, domyslnie
     * dziedziczaca po bg/fg powyzej - patrz init_theme_colors */
    XColor box_bg, button_bg, icon_fg, line_fg;
    /* kolory pasow/kwadracikow ui_meter/ui_segment_meter */
    XColor bar_active_bg, bar_inactive_bg;

    int window_margin; /* patrz ui_window_margin/"windowMargin" w ui.h */

    XIM xim;   /* NULL jesli lokalna metoda wejscia niedostepna */
    XIC xic;
    int xim_tried;

    int mx, my;
    int mouse_down;
    int mouse_clicked;

    void *focused;     /* wskaznik buf aktywnego ui_textbox, NULL = brak fokusu */
    KeySym key_sym;
    char key_utf8[16];
    int key_utf8_len;
    int key_pending;

    UiBox box_stack[UI_MAX_BOX_DEPTH];
    int box_stack_top;

    UiBoxCacheEntry box_cache[UI_MAX_BOX_CACHE];
    int box_cache_count;

    UiRect clip_rect;
    int clip_active;
};

static int point_in_rect(UiCtx *ctx, UiRect r) {
    return ctx->mx >= r.x && ctx->mx < r.x + r.w &&
           ctx->my >= r.y && ctx->my < r.y + r.h;
}

static int box_cache_get(UiCtx *ctx, const char *id) {
    for (int i = 0; i < ctx->box_cache_count; i++)
        if (strcmp(ctx->box_cache[i].id, id) == 0)
            return ctx->box_cache[i].height;
    return 0;
}

static void box_cache_set(UiCtx *ctx, const char *id, int height) {
    for (int i = 0; i < ctx->box_cache_count; i++) {
        if (strcmp(ctx->box_cache[i].id, id) == 0) {
            ctx->box_cache[i].height = height;
            return;
        }
    }
    if (ctx->box_cache_count < UI_MAX_BOX_CACHE) {
        UiBoxCacheEntry *e = &ctx->box_cache[ctx->box_cache_count++];
        snprintf(e->id, sizeof(e->id), "%s", id);
        e->height = height;
    }
}

/* Laduje font iso10646-1 przez XLoadQueryFont (XLFD lub alias, np.
 * "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1").
 * Bez locale, bez XFontSet - rysowanie przez XDrawString16 z wlasnym
 * konwerterem UTF-8->UCS-2, niezaleznym od setlocale/mbstowcs. */
static XFontStruct *ui_open_font(UiCtx *ctx, const char *name) {
    return XLoadQueryFont(ctx->dpy, name);
}

static void compute_font_metrics(UiCtx *ctx) {
    ctx->ascent  = ctx->font->ascent;
    ctx->descent = ctx->font->descent;
}

/* Konwertuje UTF-8 (str, len bajtow) na tablice XChar2b (UCS-2 big-endian).
 * Zwraca liczbe wypelnionych elementow (<=out_max). Znaki >U+FFFF (poza BMP)
 * zastepowane '?'. Niepoprawne sekwencje UTF-8 - bajt traktowany jako U+FFFD. */
static int utf8_to_ucs2(const char *str, int len, XChar2b *out, int out_max) {
    int n = 0, i = 0;
    while (i < len && n < out_max) {
        unsigned char c = (unsigned char)str[i];
        unsigned long u;
        if (c < 0x80) {
            u = c; i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len &&
                   ((unsigned char)str[i+1] & 0xC0) == 0x80) {
            u = ((unsigned long)(c & 0x1F) << 6) |
                ((unsigned char)str[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len &&
                   ((unsigned char)str[i+1] & 0xC0) == 0x80 &&
                   ((unsigned char)str[i+2] & 0xC0) == 0x80) {
            u = ((unsigned long)(c & 0x0F) << 12) |
                ((unsigned long)((unsigned char)str[i+1] & 0x3F) << 6) |
                ((unsigned char)str[i+2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len &&
                   ((unsigned char)str[i+1] & 0xC0) == 0x80 &&
                   ((unsigned char)str[i+2] & 0xC0) == 0x80 &&
                   ((unsigned char)str[i+3] & 0xC0) == 0x80) {
            u = ((unsigned long)(c & 0x07) << 18) |
                ((unsigned long)((unsigned char)str[i+1] & 0x3F) << 12) |
                ((unsigned long)((unsigned char)str[i+2] & 0x3F) << 6) |
                ((unsigned char)str[i+3] & 0x3F);
            i += 4;
        } else {
            u = 0xFFFD; i++;
        }
        if (u > 0xFFFF) u = '?';
        out[n].byte1 = (unsigned char)((u >> 8) & 0xFF);
        out[n].byte2 = (unsigned char)(u & 0xFF);
        n++;
    }
    return n;
}

/* Pomocnicze: szerokosc tekstu UTF-8 w pikselach (przez UCS-2). */
static int text_width_utf8(UiCtx *ctx, const char *text, int len) {
    XChar2b buf[1024];
    int nc = utf8_to_ucs2(text, len, buf, 1024);
    return XTextWidth16(ctx->font, buf, nc);
}

/* Pomocnicze: rysuje tekst UTF-8 na backbufferze w punkcie (x, baseline). */
static void draw_string_utf8(UiCtx *ctx, int x, int y,
                              const char *text, int len) {
    XChar2b buf[1024];
    int nc = utf8_to_ucs2(text, len, buf, 1024);
    XSetFont(ctx->dpy, ctx->gc, ctx->font->fid);
    XDrawString16(ctx->dpy, ctx->backbuf, ctx->gc, x, y, buf, nc);
}

/* Czyta background/foreground/activeBackground/uiFont/windowMargin
 * oraz wezsze kategorie kolorow z bazy zasobow X (patrz komentarz
 * w oryginale - te same zasoby, ta sama logika fallbackow).
 * Roznica vs galaz master: uiFont to XLFD lub alias bitmapowy
 * (np. "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1"),
 * NIE wzorzec fontconfig - ladowany przez XLoadQueryFont, nie Xft. */
static void init_theme(UiCtx *ctx, const char *fallback_fontname) {
    const char *bg_hex = "#ffffff";
    const char *fg_hex = "#000000";
    const char *accent_hex = "#3366cc";
    const char *box_bg_hex;
    const char *button_bg_hex;
    const char *icon_fg_hex;
    const char *line_fg_hex;
    const char *bar_active_bg_hex;
    const char *bar_inactive_bg_hex;
    const char *font_name = fallback_fontname;
    int window_margin = 8;

    XrmInitialize();
    char *rms = XResourceManagerString(ctx->dpy);
    XrmDatabase db = rms ? XrmGetStringDatabase(rms) : NULL;

    if (db) {
        char *type;
        XrmValue value;

        if (XrmGetResource(db, "background", "Background", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            bg_hex = value.addr;

        if (XrmGetResource(db, "foreground", "Foreground", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            fg_hex = value.addr;

        if (XrmGetResource(db, "activeBackground", "ActiveBackground", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            accent_hex = value.addr;

        if (XrmGetResource(db, "uiFont", "UiFont", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            font_name = value.addr;

        if (XrmGetResource(db, "windowMargin", "WindowMargin", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr) {
            int v = atoi(value.addr);
            if (v >= 0) window_margin = v;
        }
    }
    ctx->window_margin = window_margin;

    box_bg_hex = bg_hex;
    button_bg_hex = bg_hex;
    icon_fg_hex = fg_hex;
    line_fg_hex = fg_hex;

    if (db) {
        char *type;
        XrmValue value;

        if (XrmGetResource(db, "boxBackground", "BoxBackground", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            box_bg_hex = value.addr;

        if (XrmGetResource(db, "buttonBackground", "ButtonBackground", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            button_bg_hex = value.addr;

        if (XrmGetResource(db, "iconForeground", "IconForeground", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            icon_fg_hex = value.addr;

        if (XrmGetResource(db, "lineForeground", "LineForeground", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            line_fg_hex = value.addr;
    }

    bar_active_bg_hex = accent_hex;
    bar_inactive_bg_hex = box_bg_hex;

    if (db) {
        char *type;
        XrmValue value;

        if (XrmGetResource(db, "activeBarBg", "ActiveBarBg", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            bar_active_bg_hex = value.addr;

        if (XrmGetResource(db, "inactiveBarBg", "InactiveBarBg", &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            bar_inactive_bg_hex = value.addr;
    }

    ui_color(ctx, bg_hex, &ctx->bg);
    ui_color(ctx, fg_hex, &ctx->fg);
    ui_color(ctx, accent_hex, &ctx->accent);
    ui_color(ctx, box_bg_hex, &ctx->box_bg);
    ui_color(ctx, button_bg_hex, &ctx->button_bg);
    ui_color(ctx, icon_fg_hex, &ctx->icon_fg);
    ui_color(ctx, line_fg_hex, &ctx->line_fg);
    ui_color(ctx, bar_active_bg_hex, &ctx->bar_active_bg);
    ui_color(ctx, bar_inactive_bg_hex, &ctx->bar_inactive_bg);

    ctx->font = ui_open_font(ctx, font_name);
    if (!ctx->font && font_name != fallback_fontname)
        ctx->font = ui_open_font(ctx, fallback_fontname);

    if (ctx->font)
        compute_font_metrics(ctx);

    if (db) XrmDestroyDatabase(db);
}

static int (*ui_prev_xerror_handler)(Display *, XErrorEvent *) = NULL;

static int ui_xerror(Display *dpy, XErrorEvent *ee) {
    if (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
        return 0;
    if (ui_prev_xerror_handler) return ui_prev_xerror_handler(dpy, ee);
    return 0;
}

static void free_theme_colors(UiCtx *ctx) {
    Colormap cmap = DefaultColormap(ctx->dpy, ctx->screen);
    unsigned long pixels[9];
    pixels[0] = ctx->fg.pixel;
    pixels[1] = ctx->bg.pixel;
    pixels[2] = ctx->accent.pixel;
    pixels[3] = ctx->box_bg.pixel;
    pixels[4] = ctx->button_bg.pixel;
    pixels[5] = ctx->icon_fg.pixel;
    pixels[6] = ctx->line_fg.pixel;
    pixels[7] = ctx->bar_active_bg.pixel;
    pixels[8] = ctx->bar_inactive_bg.pixel;
    XFreeColors(ctx->dpy, cmap, pixels, 9, 0);
}

UiCtx *ui_init(Display *dpy, Window win, GC gc, const char *fontname, int w, int h) {
    UiCtx *ctx = calloc(1, sizeof(UiCtx));
    if (!ctx) return NULL;

    if (!ui_prev_xerror_handler)
        ui_prev_xerror_handler = XSetErrorHandler(ui_xerror);

    ctx->dpy = dpy;
    ctx->win = win;
    ctx->gc = gc;
    ctx->screen = DefaultScreen(dpy);

    init_theme(ctx, fontname);
    if (!ctx->font) {
        /* init_theme() alokuje 9 kolorow z palety serwera X (XAllocColor)
         * PRZED probą otwarcia fontu - bez tego zwolnienia kazdy nieudany
         * ui_init (np. zly fontname bez dzialajacego fallbacku) wyciekalby
         * je na stale w colormapie. */
        free_theme_colors(ctx);
        free(ctx);
        return NULL;
    }

    ui_resize(ctx, w, h);

    return ctx;
}

void ui_destroy(UiCtx *ctx) {
    if (!ctx) return;
    free_theme_colors(ctx);
    if (ctx->font) XFreeFont(ctx->dpy, ctx->font);
    if (ctx->backbuf) XFreePixmap(ctx->dpy, ctx->backbuf);
    if (ctx->xic) XDestroyIC(ctx->xic);
    if (ctx->xim) XCloseIM(ctx->xim);
    free(ctx);
}

void ui_resize(UiCtx *ctx, int w, int h) {
    if (ctx->backbuf && w == ctx->buf_w && h == ctx->buf_h) return;
    if (w <= 0 || h <= 0) return;
    if (ctx->backbuf) XFreePixmap(ctx->dpy, ctx->backbuf);
    ctx->backbuf = XCreatePixmap(ctx->dpy, ctx->win, w, h, DefaultDepth(ctx->dpy, ctx->screen));
    ctx->buf_w = w;
    ctx->buf_h = h;
}

int ui_color(UiCtx *ctx, const char *name, XColor *out) {
    Colormap cmap = DefaultColormap(ctx->dpy, ctx->screen);
    if (!XParseColor(ctx->dpy, cmap, name, out)) return 0;
    return XAllocColor(ctx->dpy, cmap, out);
}

const XColor *ui_theme_fg(UiCtx *ctx) { return &ctx->fg; }
const XColor *ui_theme_bg(UiCtx *ctx) { return &ctx->bg; }
const XColor *ui_theme_accent(UiCtx *ctx) { return &ctx->accent; }
const XColor *ui_theme_box_bg(UiCtx *ctx) { return &ctx->box_bg; }
const XColor *ui_theme_button_bg(UiCtx *ctx) { return &ctx->button_bg; }
const XColor *ui_theme_icon_fg(UiCtx *ctx) { return &ctx->icon_fg; }
const XColor *ui_theme_line_fg(UiCtx *ctx) { return &ctx->line_fg; }
const XColor *ui_theme_bar_active_bg(UiCtx *ctx) { return &ctx->bar_active_bg; }
const XColor *ui_theme_bar_inactive_bg(UiCtx *ctx) { return &ctx->bar_inactive_bg; }

int ui_window_margin(UiCtx *ctx) { return ctx->window_margin; }

void ui_feed_event(UiCtx *ctx, XEvent *ev) {
    if (ev->type == KeyPress && !ctx->xim_tried) {
        ctx->xim_tried = 1;
        if (!setlocale(LC_CTYPE, ""))
            setlocale(LC_CTYPE, "UTF-8");
        XSetLocaleModifiers("");
        ctx->xim = XOpenIM(ctx->dpy, NULL, NULL, NULL);
        if (ctx->xim) {
            ctx->xic = XCreateIC(ctx->xim,
                                  XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                  XNClientWindow, ctx->win,
                                  XNFocusWindow, ctx->win,
                                  NULL);
        }
    }

    if (ctx->xic && XFilterEvent(ev, None))
        return;

    switch (ev->type) {
    case MotionNotify:
        ctx->mx = ev->xmotion.x;
        ctx->my = ev->xmotion.y;
        break;
    case ButtonPress:
        ctx->mx = ev->xbutton.x;
        ctx->my = ev->xbutton.y;
        ctx->mouse_down = 1;
        break;
    case ButtonRelease:
        ctx->mx = ev->xbutton.x;
        ctx->my = ev->xbutton.y;
        if (ctx->mouse_down) ctx->mouse_clicked = 1;
        ctx->mouse_down = 0;
        break;
    case KeyPress: {
        char text[16];
        KeySym sym = NoSymbol;
        int n;
        if (ctx->xic) {
            Status status = 0;
            n = Xutf8LookupString(ctx->xic, &ev->xkey, text, (int)sizeof(text) - 1, &sym, &status);
            if (status == XBufferOverflow) n = 0;
        } else {
            n = XLookupString(&ev->xkey, text, (int)sizeof(text) - 1, &sym, NULL);
        }
        if (n < 0) n = 0;
        text[n] = 0;
        ctx->key_sym = sym;
        memcpy(ctx->key_utf8, text, n + 1);
        ctx->key_utf8_len = n;
        ctx->key_pending = 1;
        break;
    }
    default:
        break;
    }
}

void ui_begin_frame(UiCtx *ctx) {
    XSetForeground(ctx->dpy, ctx->gc, ctx->bg.pixel);
    XFillRectangle(ctx->dpy, ctx->backbuf, ctx->gc, 0, 0, ctx->buf_w, ctx->buf_h);
}

void ui_end_frame(UiCtx *ctx) {
    XCopyArea(ctx->dpy, ctx->backbuf, ctx->win, ctx->gc, 0, 0, ctx->buf_w, ctx->buf_h, 0, 0);
    XFlush(ctx->dpy);
    ctx->mouse_clicked = 0;
    ctx->key_pending = 0;
}

void ui_fill_rect(UiCtx *ctx, UiRect r, const XColor *c) {
    XSetForeground(ctx->dpy, ctx->gc, c->pixel);
    XFillRectangle(ctx->dpy, ctx->backbuf, ctx->gc, r.x, r.y, r.w, r.h);
}

void ui_draw_border(UiCtx *ctx, UiRect r, int t, const XColor *c) {
    if (t <= 0) return;
    XSetForeground(ctx->dpy, ctx->gc, c->pixel);
    XFillRectangle(ctx->dpy, ctx->backbuf, ctx->gc, r.x, r.y, r.w, t);
    XFillRectangle(ctx->dpy, ctx->backbuf, ctx->gc, r.x, r.y + r.h - t, r.w, t);
    XFillRectangle(ctx->dpy, ctx->backbuf, ctx->gc, r.x, r.y, t, r.h);
    XFillRectangle(ctx->dpy, ctx->backbuf, ctx->gc, r.x + r.w - t, r.y, t, r.h);
}

void ui_draw_line(UiCtx *ctx, int x1, int y1, int x2, int y2, int thickness, const XColor *c) {
    if (thickness < 1) thickness = 1;
    XSetForeground(ctx->dpy, ctx->gc, c->pixel);
    XSetLineAttributes(ctx->dpy, ctx->gc, thickness, LineSolid, CapButt, JoinMiter);
    XDrawLine(ctx->dpy, ctx->backbuf, ctx->gc, x1, y1, x2, y2);
    XSetLineAttributes(ctx->dpy, ctx->gc, 0, LineSolid, CapButt, JoinMiter);
}

void ui_fill_circle(UiCtx *ctx, int cx, int cy, int radius, const XColor *c) {
    if (radius < 1) return;
    XSetForeground(ctx->dpy, ctx->gc, c->pixel);
    XFillArc(ctx->dpy, ctx->backbuf, ctx->gc, cx - radius, cy - radius,
             2 * radius, 2 * radius, 0, 360 * 64);
}

void ui_draw_circle(UiCtx *ctx, int cx, int cy, int radius, int thickness, const XColor *c) {
    if (radius < 1) return;
    if (thickness < 1) thickness = 1;
    XSetForeground(ctx->dpy, ctx->gc, c->pixel);
    XSetLineAttributes(ctx->dpy, ctx->gc, thickness, LineSolid, CapButt, JoinMiter);
    XDrawArc(ctx->dpy, ctx->backbuf, ctx->gc, cx - radius, cy - radius,
             2 * radius, 2 * radius, 0, 360 * 64);
    XSetLineAttributes(ctx->dpy, ctx->gc, 0, LineSolid, CapButt, JoinMiter);
}

void ui_fill_triangle(UiCtx *ctx, int x0, int y0, int x1, int y1, int x2, int y2, const XColor *c) {
    XPoint pts[3];

    pts[0].x = (short) x0; pts[0].y = (short) y0;
    pts[1].x = (short) x1; pts[1].y = (short) y1;
    pts[2].x = (short) x2; pts[2].y = (short) y2;
    XSetForeground(ctx->dpy, ctx->gc, c->pixel);
    XFillPolygon(ctx->dpy, ctx->backbuf, ctx->gc, pts, 3, Convex, CoordModeOrigin);
}

void ui_draw_pixmap(UiCtx *ctx, UiRect r, Pixmap p) {
    if (r.w <= 0 || r.h <= 0) return;
    XCopyArea(ctx->dpy, p, ctx->backbuf, ctx->gc, 0, 0, (unsigned) r.w, (unsigned) r.h, r.x, r.y);
}

/* Bez XftDraw - GC clip dotyczy rowniez Xutf8DrawString, wiec jeden
 * mechanizm wystarczy (w odroznieniu od galazi master, gdzie clip
 * trzeba bylo ustawiac i na GC, i na XftDraw osobno). */
void ui_set_clip(UiCtx *ctx, UiRect r) {
    XRectangle rect;

    rect.x = (short) r.x; rect.y = (short) r.y;
    rect.width = (unsigned short) (r.w > 0 ? r.w : 0);
    rect.height = (unsigned short) (r.h > 0 ? r.h : 0);
    XSetClipRectangles(ctx->dpy, ctx->gc, 0, 0, &rect, 1, Unsorted);
    ctx->clip_rect = r;
    ctx->clip_active = 1;
}

void ui_clear_clip(UiCtx *ctx) {
    XSetClipMask(ctx->dpy, ctx->gc, None);
    ctx->clip_active = 0;
}

static UiRect rect_intersect(UiRect a, UiRect b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    UiRect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

static void label_clip_push(UiCtx *ctx, UiRect r, UiRect *prev_rect, int *prev_active) {
    *prev_rect = ctx->clip_rect;
    *prev_active = ctx->clip_active;
    ui_set_clip(ctx, *prev_active ? rect_intersect(r, *prev_rect) : r);
}

static void label_clip_pop(UiCtx *ctx, UiRect prev_rect, int prev_active) {
    if (prev_active)
        ui_set_clip(ctx, prev_rect);
    else
        ui_clear_clip(ctx);
}

static void draw_text_hcentered_fg(UiCtx *ctx, UiRect r, const char *text, const XColor *color) {
    int len = (int)strlen(text);
    int tw = text_width_utf8(ctx, text, len);
    int tx = r.x + (r.w - tw) / 2;
    int ty = r.y + (r.h + ctx->ascent - ctx->descent) / 2;
    UiRect prev_rect;
    int prev_active;
    label_clip_push(ctx, r, &prev_rect, &prev_active);
    XSetForeground(ctx->dpy, ctx->gc, color->pixel);
    draw_string_utf8(ctx, tx, ty, text, len);
    label_clip_pop(ctx, prev_rect, prev_active);
}

static void draw_text_hcentered(UiCtx *ctx, UiRect r, const char *text) {
    draw_text_hcentered_fg(ctx, r, text, &ctx->fg);
}

void ui_label_fg(UiCtx *ctx, UiRect r, const char *text, const XColor *color) {
    int ty = r.y + (r.h + ctx->ascent - ctx->descent) / 2;
    int len = (int)strlen(text);
    UiRect prev_rect;
    int prev_active;
    label_clip_push(ctx, r, &prev_rect, &prev_active);
    XSetForeground(ctx->dpy, ctx->gc, color->pixel);
    draw_string_utf8(ctx, r.x, ty, text, len);
    label_clip_pop(ctx, prev_rect, prev_active);
}

void ui_label_centered_fg(UiCtx *ctx, UiRect r, const char *text, const XColor *color) {
    draw_text_hcentered_fg(ctx, r, text, color);
}

void ui_label(UiCtx *ctx, UiRect r, const char *text) {
    ui_label_fg(ctx, r, text, &ctx->fg);
}

void ui_label_centered(UiCtx *ctx, UiRect r, const char *text) {
    draw_text_hcentered(ctx, r, text);
}

void ui_label_ellipsis(UiCtx *ctx, UiRect r, const char *text) {
    static const char ellipsis[] = "...";
    int len = (int)strlen(text);
    int ell_w;
    int lo, hi, mid, n;
    char buf[1024];

    /* fast path: tekst miesci sie bez obcinania */
    if (text_width_utf8(ctx, text, len) <= r.w) {
        ui_label_fg(ctx, r, text, &ctx->fg);
        return;
    }

    ell_w = text_width_utf8(ctx, ellipsis, 3);

    lo = 0;
    hi = len;
    n  = 0;
    while (lo <= hi) {
        int snap, tw;

        mid  = lo + (hi - lo) / 2;
        snap = mid;
        while (snap > 0 && ((unsigned char)text[snap] & 0xC0) == 0x80)
            snap--;
        tw = text_width_utf8(ctx, text, snap);
        if (tw + ell_w <= r.w) {
            n  = snap;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (n + 3 < (int)sizeof(buf)) {
        memcpy(buf, text, n);
        memcpy(buf + n, ellipsis, 4); /* 4 = len("...") + NUL */
        ui_label_fg(ctx, r, buf, &ctx->fg);
    } else {
        ui_label_fg(ctx, r, ellipsis, &ctx->fg);
    }
}

int ui_text_width(UiCtx *ctx, const char *text) {
    return text_width_utf8(ctx, text, (int)strlen(text));
}

int ui_button_width(UiCtx *ctx, const char *label) {
    return ui_text_width(ctx, label) + 2 * UI_BTN_PAD;
}

int ui_line_height(UiCtx *ctx) {
    return ctx->ascent + ctx->descent;
}

int ui_button(UiCtx *ctx, UiRect r, const char *label) {
    int hover = point_in_rect(ctx, r);
    ui_fill_rect(ctx, r, hover ? &ctx->accent : &ctx->button_bg);
    ui_draw_border(ctx, r, 1, &ctx->line_fg);
    draw_text_hcentered(ctx, r, label);

    return hover && ctx->mouse_clicked;
}

void ui_meter(UiCtx *ctx, UiRect r, double frac, const char *label) {
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    ui_fill_rect(ctx, r, &ctx->bar_inactive_bg);

    int fill_w = (int) (r.w * frac);
    if (fill_w > 0) {
        UiRect fill_r = { r.x, r.y, fill_w, r.h };
        ui_fill_rect(ctx, fill_r, &ctx->bar_active_bg);
    }

    ui_draw_border(ctx, r, 1, &ctx->line_fg);

    if (label && label[0])
        draw_text_hcentered(ctx, r, label);
}

void ui_segment_meter(UiCtx *ctx, UiRect r, int active, int total, int gap) {
    if (total <= 0) return;
    if (active < 0) active = 0;
    if (active > total) active = total;

    for (int i = 0; i < total; i++) {
        UiRect col = ui_rect_col(r, i, total, gap);
        int side = col.w < r.h ? col.w : r.h;
        UiRect sq = { col.x + (col.w - side) / 2, r.y + (r.h - side) / 2, side, side };

        ui_fill_rect(ctx, sq, i < active ? &ctx->bar_active_bg : &ctx->bar_inactive_bg);
        ui_draw_border(ctx, sq, 1, &ctx->line_fg);
    }
}

int ui_checkbox(UiCtx *ctx, UiRect r, const char *label, int *state) {
    int hover = point_in_rect(ctx, r);
    int toggled = hover && ctx->mouse_clicked;
    if (toggled) *state = !*state;

    int box_size = r.h - 4;
    if (box_size < 4) box_size = r.h;
    UiRect box = { r.x, r.y + (r.h - box_size) / 2, box_size, box_size };

    ui_fill_rect(ctx, box, *state ? &ctx->accent : &ctx->bg);
    ui_draw_border(ctx, box, 1, &ctx->line_fg);

    if (*state) {
        int inset = box_size / 4;
        if (inset < 1) inset = 1;
        UiRect mark = { box.x + inset, box.y + inset, box.w - 2 * inset, box.h - 2 * inset };
        ui_fill_rect(ctx, mark, &ctx->fg);
    }

    int label_x = box.x + box.w + 6;
    UiRect label_r = { label_x, r.y, r.x + r.w - label_x, r.h };
    ui_label(ctx, label_r, label);

    return toggled;
}

void ui_selection_mark(UiCtx *ctx, UiRect r, int checked) {
    int box_size = r.h - 4;
    UiRect box;

    if (box_size < 4) box_size = r.h;
    box = (UiRect){ r.x + (r.w - box_size) / 2, r.y + (r.h - box_size) / 2, box_size, box_size };

    ui_draw_border(ctx, box, 1, &ctx->line_fg);
    if (checked) {
        int radius = box_size / 3;

        if (radius < 2) radius = 2;
        ui_fill_circle(ctx, box.x + box.w / 2, box.y + box.h / 2, radius, &ctx->accent);
    }
}

int ui_list(UiCtx *ctx, UiRect r, const char **items, int n, int *selected) {
    if (n <= 0) return 0;
    int row_h = r.h / n;
    if (row_h < 1) row_h = 1;
    int changed = 0;

    for (int i = 0; i < n; i++) {
        UiRect row = { r.x, r.y + i * row_h, r.w, row_h };
        if (point_in_rect(ctx, row) && ctx->mouse_clicked) {
            *selected = i;
            changed = 1;
            break;
        }
    }

    for (int i = 0; i < n; i++) {
        UiRect row = { r.x, r.y + i * row_h, r.w, row_h };
        int hover = point_in_rect(ctx, row);
        int is_selected = (i == *selected);

        ui_fill_rect(ctx, row, is_selected ? &ctx->accent : &ctx->bg);
        if (hover && !is_selected) ui_draw_border(ctx, row, 1, &ctx->accent);

        UiRect label_r = { row.x + 4, row.y, row.w - 8, row.h };
        ui_label(ctx, label_r, items[i]);
    }

    return changed;
}

static int utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_prev_len(const char *buf, int cursor) {
    if (cursor <= 0) return 0;
    int i = cursor - 1;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--;
    return cursor - i;
}

int ui_textbox(UiCtx *ctx, UiRect r, char *buf, int buf_cap, int *cursor) {
    int hover = point_in_rect(ctx, r);

    if (ctx->mouse_clicked) {
        if (hover) ctx->focused = buf;
        else if (ctx->focused == buf) ctx->focused = NULL;
    }

    int focused = (ctx->focused == buf);
    int len = (int)strlen(buf);
    if (*cursor < 0) *cursor = 0;
    if (*cursor > len) *cursor = len;

    int changed = 0;

    if (focused && ctx->key_pending) {
        if (ctx->key_sym == XK_BackSpace) {
            if (*cursor > 0) {
                int del = utf8_prev_len(buf, *cursor);
                memmove(buf + *cursor - del, buf + *cursor, len - *cursor + 1);
                *cursor -= del;
                changed = 1;
            }
        } else if (ctx->key_sym == XK_Delete) {
            if (*cursor < len) {
                int del = utf8_seq_len((unsigned char)buf[*cursor]);
                if (*cursor + del > len) del = len - *cursor;
                memmove(buf + *cursor, buf + *cursor + del, len - *cursor - del + 1);
                changed = 1;
            }
        } else if (ctx->key_sym == XK_Left) {
            if (*cursor > 0) *cursor -= utf8_prev_len(buf, *cursor);
        } else if (ctx->key_sym == XK_Right) {
            if (*cursor < len) {
                int adv = utf8_seq_len((unsigned char)buf[*cursor]);
                if (*cursor + adv > len) adv = len - *cursor;
                *cursor += adv;
            }
        } else if (ctx->key_sym == XK_Home) {
            *cursor = 0;
        } else if (ctx->key_sym == XK_End) {
            *cursor = len;
        } else if (ctx->key_utf8_len > 0 &&
                   (unsigned char)ctx->key_utf8[0] >= 32 &&
                   (unsigned char)ctx->key_utf8[0] != 127) {
            int ins_len = ctx->key_utf8_len;
            if (len + ins_len < buf_cap) {
                memmove(buf + *cursor + ins_len, buf + *cursor, len - *cursor + 1);
                memcpy(buf + *cursor, ctx->key_utf8, ins_len);
                *cursor += ins_len;
                changed = 1;
            }
        }
        len = (int)strlen(buf);
    }

    ui_fill_rect(ctx, r, &ctx->bg);
    ui_draw_border(ctx, r, 1, focused ? &ctx->accent : &ctx->line_fg);

    UiRect text_r = { r.x + 4, r.y, r.w - 8, r.h };
    ui_label(ctx, text_r, buf);

    if (focused) {
        int cw = text_width_utf8(ctx, buf, *cursor);
        UiRect caret = { text_r.x + cw, r.y + 3, 1, r.h - 6 };
        ui_fill_rect(ctx, caret, &ctx->fg);
    }

    return changed;
}

int ui_hit_test(UiCtx *ctx, UiRect r) {
    return point_in_rect(ctx, r) && ctx->mouse_clicked;
}

void ui_mouse_state(UiCtx *ctx, int *x, int *y, int *down) {
    if (x) *x = ctx->mx;
    if (y) *y = ctx->my;
    if (down) *down = ctx->mouse_down;
}

int ui_menu_item(UiCtx *ctx, UiRect r, const char *label) {
    int hover = point_in_rect(ctx, r);

    ui_fill_rect(ctx, r, hover ? &ctx->accent : &ctx->box_bg);
    ui_label(ctx, (UiRect){ r.x + 6, r.y, (r.w > 10 ? r.w - 10 : 0), r.h }, label);
    return hover && ctx->mouse_clicked;
}

UiRect ui_rect_col(UiRect row, int col, int n, int gap) {
    if (n <= 0) n = 1;
    if (col < 0) col = 0;
    if (col >= n) col = n - 1;

    int total_gap = gap * (n - 1);
    int col_w = (row.w - total_gap) / n;
    if (col_w < 0) col_w = 0;

    UiRect r = { row.x + col * (col_w + gap), row.y, col_w, row.h };
    return r;
}

void ui_rect_split3(UiRect row, int left_w, int right_w, int gap,
                     UiRect *left, UiRect *mid, UiRect *right) {
    if (left)  *left  = (UiRect){ row.x, row.y, left_w, row.h };
    if (right) *right = (UiRect){ row.x + row.w - right_w, row.y, right_w, row.h };

    if (mid) {
        int mid_x = row.x + left_w + gap;
        int mid_w = row.w - left_w - right_w - 2 * gap;
        if (mid_w < 0) mid_w = 0;
        *mid = (UiRect){ mid_x, row.y, mid_w, row.h };
    }
}

UiBox *ui_box_begin(UiCtx *ctx, const char *id, int x, int y, int width, const UiBoxStyle *style) {
    if (ctx->box_stack_top >= UI_MAX_BOX_DEPTH)
        ctx->box_stack_top = UI_MAX_BOX_DEPTH - 1;

    UiBox *box = &ctx->box_stack[ctx->box_stack_top++];
    box->ctx = ctx;
    snprintf(box->id, sizeof(box->id), "%s", id);
    box->style = *style;

    int outer_x = x + style->margin_l;
    int outer_y = y + style->margin_t;
    int outer_w = width - style->margin_l - style->margin_r;
    int cached_h = box_cache_get(ctx, id);

    if (cached_h > 0) {
        UiRect outer = { outer_x, outer_y, outer_w, cached_h };
        ui_fill_rect(ctx, outer, &style->bg_color);
        ui_draw_border(ctx, outer, style->border_w, &style->border_color);
    }

    box->outer_x = outer_x;
    box->outer_y = outer_y;
    box->outer_w = outer_w;
    box->content_x = outer_x + style->border_w + style->padding_l;
    box->content_w = outer_w - 2 * style->border_w - style->padding_l - style->padding_r;
    box->cursor_y = outer_y + style->border_w + style->padding_t;
    box->content_h_accum = 0;
    box->child_count = 0;

    return box;
}

UiRect ui_box_next_rect(UiBox *box, int height) {
    if (box->child_count > 0) {
        box->cursor_y += box->style.gap;
        box->content_h_accum += box->style.gap;
    }

    UiRect r = { box->content_x, box->cursor_y, box->content_w, height };
    box->cursor_y += height;
    box->content_h_accum += height;
    box->child_count++;
    return r;
}

void ui_box_end(UiBox *box) {
    int total_h = box->content_h_accum
                + box->style.padding_t + box->style.padding_b
                + 2 * box->style.border_w;
    box_cache_set(box->ctx, box->id, total_h);
    if (box->ctx->box_stack_top > 0) box->ctx->box_stack_top--;
}

int ui_box_height(UiCtx *ctx, const char *id) {
    return box_cache_get(ctx, id);
}
