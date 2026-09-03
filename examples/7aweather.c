/*
 * 7aweather.c - port oryginalnej apki z ../7aweather (Xt/Xaw + wlasny
 * widget WeatherBox) na biblioteke ui.c/ui.h z tego katalogu.
 *
 * Logika pobierania/parsowania pogody (RunCommand, UrlEncode, funkcje
 * FindXxx/GetXxxAtIndex) jest przeniesiona z oryginalu bez zmian - to
 * zwykle funkcje na char*, niezalezne od toolkitu. To, co sie zmienilo,
 * to warstwa UI:
 *  - WeatherBox (osobny widget Xt z GC-ami, Redisplay, ComputeGeometry)
 *    zastapiony przez zwykle wywolania ui_box_begin/ui_box_next_rect/
 *    ui_button/ui_label w jednej funkcji draw(), wywolywanej co klatke -
 *    patrz examples/demo.c po wzorzec.
 *  - ikonki wiersza (termometr/wiatr/chmura/deszcz), w oryginale rysowane
 *    RAZ na 1-bitowe Pixmapy i kopiowane przez XCopyPlane, sa tu rysowane
 *    NA ZYWO co klatke wprost w docelowym rect - immediate mode nie musi
 *    trzymac gotowych Pixmap, wiec cala maszyneria g_line_icon,
 *    wind_icon_current, UpdateWindIcon z oryginalu odpada, zastapiona
 *    pojedyncza zmienna g_wind_dir czytana przez DrawWindIcon.
 *  - Xt/Shell nie ma tu geometrycznych "negocjacji" (okno tworzone wprost
 *    przez XCreateSimpleWindow o zadanym rozmiarze), wiec cala obrobka
 *    -geometry z oryginalu (contest z Shellem o szerokosc) jest zbedna -
 *    pominieta, tak samo jak parsowanie opcji -display (DISPLAY z env
 *    wystarcza, jak w examples/demo.c).
 *  - Timer odswiezania (oryginalnie XtAppAddTimeOut) zastapiony petla
 *    select() na deskryptorze polaczenia X - standardowy wzorzec dla
 *    "czystego" Xlib bez Xt/GLib main loopa.
 *
 * Lokalizacja podawana w linii polecen jak w oryginale:
 *   7aweather Warszawa
 *   7aweather New York
 */

/* popen/pclose (POSIX) i M_PI (XSI/BSD) sa poza ISO C99 - -std=c99 w
 * Makefile ukrywa je w glibc bez tego makra, chyba ze wlaczymy je jawnie;
 * na OpenBSD nie ma to wplywu (tam sa widoczne niezaleznie). */
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ICON_SIZE 32            /* ikonka okna (WM/taskbar), 1-bitowa Pixmap */
#define REFRESH_INTERVAL_MS 600000  /* 10 min - tyle co domyslne w oryginale */
#define FORECAST_BUF_SIZE 8192
#define ROW_H 20                 /* wysokosc wiersza naglowka/tresci/przyciskow */
#define ICON_TEXT_GAP 2          /* odstep miedzy kolumna ikonek a tekstem w wierszach tresci (patrz draw) */

/* -------------------------------------------------------------------- */
/* Stan pogody - te same nazwy/znaczenie co w oryginalnym 7aweather.c   */
/* -------------------------------------------------------------------- */

static char location_query[256];

static char g_line_text[4][64] = { "...", "...", "...", "..." };
static char g_header_text[80] = "...";
static double g_wind_dir = 0.0;  /* kat do DrawWindIcon - zastepuje Pixmap g_line_icon[1] z oryginalu */

static char g_forecast_buf[FORECAST_BUF_SIZE];
static char *g_time_arr, *g_temp_arr, *g_wind_arr, *g_dir_arr, *g_cloud_arr, *g_precip_arr;

static int g_base_index = -1;
static int g_hour_count = 0;
static int g_offset = 0;

static double g_now_temp, g_now_wind_speed, g_now_wind_dir, g_now_cloud_cover;
static int g_now_precip_percent = -1;
static char g_now_time[32];

/* -------------------------------------------------------------------- */
/* Uruchamianie komend i parsowanie tekstu/JSON - bez zmian wzgledem    */
/* oryginalu, patrz tam obszerniejsze komentarze przy kazdej funkcji.   */
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

static char *
ExtractFlatObject(char *start)
{
    char *end = strchr(start, '}');

    if (!end)
        return NULL;
    *end = '\0';
    return start;
}

static char *
FindObjectByKey(char *buf, const char *key)
{
    char *p = strstr(buf, key);

    if (!p)
        return NULL;
    return ExtractFlatObject(p + strlen(key));
}

static char *
FindArrayByKey(char *buf, const char *key)
{
    char *p = strstr(buf, key);
    char *end;

    if (!p)
        return NULL;
    p += strlen(key);
    end = strchr(p, ']');
    if (!end)
        return NULL;
    *end = '\0';
    return p;
}

static char *
FindFirstResultObject(char *buf)
{
    char *p = strstr(buf, "\"results\":[");
    char *obj;

    if (!p)
        return NULL;
    p += strlen("\"results\":[");
    obj = strchr(p, '{');
    if (!obj)
        return NULL;
    return ExtractFlatObject(obj + 1);
}

static double
FindJsonNumber(const char *block, const char *key)
{
    char pattern[64];
    const char *p;

    if (!block)
        return 0.0;
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    p = strstr(block, pattern);
    if (!p)
        return 0.0;
    return strtod(p + strlen(pattern), NULL);
}

static int
FindJsonString(const char *block, const char *key, char *out, size_t outsize)
{
    char pattern[64];
    const char *p, *q;
    size_t len;

    out[0] = '\0';
    if (!block)
        return -1;
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    p = strstr(block, pattern);
    if (!p)
        return -1;
    p += strlen(pattern);
    q = strchr(p, '"');
    if (!q)
        return -1;
    len = (size_t) (q - p);
    if (len >= outsize)
        len = outsize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int
FindIndexBySubstring(const char *arr, const char *needle)
{
    const char *pos, *p;
    int idx = 0;

    if (!arr)
        return -1;
    pos = strstr(arr, needle);
    if (!pos)
        return -1;
    for (p = arr; p < pos; p++)
        if (*p == ',')
            idx++;
    return idx;
}

static int
GetIntAtIndex(const char *csv, int index)
{
    const char *p = csv;
    int i;

    if (!p)
        return 0;
    for (i = 0; i < index; i++) {
        p = strchr(p, ',');
        if (!p)
            return 0;
        p++;
    }
    return (int) strtol(p, NULL, 10);
}

static double
GetDoubleAtIndex(const char *csv, int index)
{
    const char *p = csv;
    int i;

    if (!p)
        return 0.0;
    for (i = 0; i < index; i++) {
        p = strchr(p, ',');
        if (!p)
            return 0.0;
        p++;
    }
    return strtod(p, NULL);
}

static void
GetStringAtIndex(const char *csv, int index, char *out, size_t outsize)
{
    const char *p = csv;
    const char *q;
    int i;
    size_t len;

    out[0] = '\0';
    if (!p)
        return;
    for (i = 0; i < index; i++) {
        p = strchr(p, ',');
        if (!p)
            return;
        p++;
    }
    if (*p == '"')
        p++;
    q = p;
    while (*q && *q != '"' && *q != ',')
        q++;
    len = (size_t) (q - p);
    if (len >= outsize)
        len = outsize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static int
CountCsvItems(const char *csv)
{
    const char *p;
    int count;

    if (!csv || !*csv)
        return 0;
    count = 1;
    for (p = csv; *p; p++)
        if (*p == ',')
            count++;
    return count;
}

static const char *
DegreesToCompass(double deg)
{
    static const char *names[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int idx;

    while (deg < 0.0)
        deg += 360.0;
    while (deg >= 360.0)
        deg -= 360.0;
    idx = ((int) ((deg + 22.5) / 45.0)) % 8;
    return names[idx];
}

static void
FormatDayTime(const char *iso, char *out, size_t outsize)
{
    char daynum[3] = "";
    const char *hm = "";

    if (strlen(iso) >= 10) {
        daynum[0] = iso[8];
        daynum[1] = iso[9];
        daynum[2] = '\0';
    }
    if (strlen(iso) >= 16)
        hm = iso + 11;
    snprintf(out, outsize, "%s %s", daynum, hm);
}

/* -------------------------------------------------------------------- */
/* Renderowanie stanu do g_line_text/g_header_text/g_wind_dir - jak     */
/* RenderForecastOffset w oryginale, ale bez ForceRedraw/XtVaSetValues: */
/* draw() czyta te zmienne wprost w tej samej klatce, w ktorej zostaly  */
/* ustawione (patrz obsluga strzalek </> w draw() nizej).               */
/* -------------------------------------------------------------------- */

static void
RenderForecastOffset(int offset)
{
    int idx;

    if (g_base_index < 0 || g_hour_count <= 0)
        offset = 0;

    idx = g_base_index + offset;
    if (offset != 0) {
        if (idx < 0) {
            idx = 0;
            offset = idx - g_base_index;
        }
        if (idx > g_hour_count - 1) {
            idx = g_hour_count - 1;
            offset = idx - g_base_index;
        }
    }
    g_offset = offset;

    if (offset == 0) {
        char daytime[16];

        snprintf(g_line_text[0], sizeof(g_line_text[0]), "Temperature: %.1f C", g_now_temp);
        snprintf(g_line_text[1], sizeof(g_line_text[1]), "Wind: %.1f km/h %s",
                 g_now_wind_speed, DegreesToCompass(g_now_wind_dir));
        snprintf(g_line_text[2], sizeof(g_line_text[2]), "Cloud cover: %d%%", (int) g_now_cloud_cover);
        if (g_now_precip_percent >= 0)
            snprintf(g_line_text[3], sizeof(g_line_text[3]), "Precipitation: %d%%", g_now_precip_percent);
        else
            snprintf(g_line_text[3], sizeof(g_line_text[3]), "Precipitation: ?");

        g_wind_dir = g_now_wind_dir;

        FormatDayTime(g_now_time, daytime, sizeof(daytime));
        snprintf(g_header_text, sizeof(g_header_text), "now (%s)", daytime);
    } else {
        double temp = GetDoubleAtIndex(g_temp_arr, idx);
        double wind_speed = GetDoubleAtIndex(g_wind_arr, idx);
        double wind_dir = GetDoubleAtIndex(g_dir_arr, idx);
        double cloud_cover = GetDoubleAtIndex(g_cloud_arr, idx);
        int precip_percent = GetIntAtIndex(g_precip_arr, idx);
        char time_str[32];
        char daytime[16];

        snprintf(g_line_text[0], sizeof(g_line_text[0]), "Temperature: %.1f C", temp);
        snprintf(g_line_text[1], sizeof(g_line_text[1]), "Wind: %.1f km/h %s",
                 wind_speed, DegreesToCompass(wind_dir));
        snprintf(g_line_text[2], sizeof(g_line_text[2]), "Cloud cover: %d%%", (int) cloud_cover);
        if (precip_percent >= 0)
            snprintf(g_line_text[3], sizeof(g_line_text[3]), "Precipitation: %d%%", precip_percent);
        else
            snprintf(g_line_text[3], sizeof(g_line_text[3]), "Precipitation: ?");

        g_wind_dir = wind_dir;

        GetStringAtIndex(g_time_arr, idx, time_str, sizeof(time_str));
        FormatDayTime(time_str, daytime, sizeof(daytime));
        snprintf(g_header_text, sizeof(g_header_text), "%+dh (%s)", offset, daytime);
    }
}

static void
SetAllLines(const char *text)
{
    int i;

    for (i = 0; i < 4; i++)
        snprintf(g_line_text[i], sizeof(g_line_text[i]), "%s", text);
}

static void
UpdateWeather(void)
{
    char encoded[512], cmd[768], geobuf[2048], label[256];
    char *geo_block, *current_block, *hourly_block;
    double lat, lon;

    UrlEncode(location_query, encoded, sizeof(encoded));

    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 5 "
             "'https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1'",
             encoded);
    RunCommand(cmd, geobuf, sizeof(geobuf));

    geo_block = FindFirstResultObject(geobuf);
    if (!geo_block) {
        snprintf(label, sizeof(label), "Location not found: %s", location_query);
        snprintf(g_line_text[0], sizeof(g_line_text[0]), "%s", label);
        snprintf(g_line_text[1], sizeof(g_line_text[1]), "-");
        snprintf(g_line_text[2], sizeof(g_line_text[2]), "-");
        snprintf(g_line_text[3], sizeof(g_line_text[3]), "-");
        g_base_index = -1;
        g_hour_count = 0;
        return;
    }
    lat = FindJsonNumber(geo_block, "latitude");
    lon = FindJsonNumber(geo_block, "longitude");

    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 5 "
             "'https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,wind_speed_10m,wind_direction_10m,cloud_cover"
             "&hourly=temperature_2m,wind_speed_10m,wind_direction_10m,cloud_cover,"
             "precipitation_probability&forecast_days=3&timezone=auto"
             "&temperature_unit=celsius&wind_speed_unit=kmh'",
             lat, lon);
    RunCommand(cmd, g_forecast_buf, FORECAST_BUF_SIZE);

    /* kolejnosc wyciecia jest istotna - patrz komentarz w oryginale przy
     * tym samym miejscu (UpdateWeather w ../7aweather/7aweather.c) */
    hourly_block = FindObjectByKey(g_forecast_buf, "\"hourly\":{");

    current_block = FindObjectByKey(g_forecast_buf, "\"current\":{");
    if (!current_block) {
        SetAllLines("?");
        g_base_index = -1;
        g_hour_count = 0;
        return;
    }
    g_now_temp = FindJsonNumber(current_block, "temperature_2m");
    g_now_wind_speed = FindJsonNumber(current_block, "wind_speed_10m");
    g_now_wind_dir = FindJsonNumber(current_block, "wind_direction_10m");
    g_now_cloud_cover = FindJsonNumber(current_block, "cloud_cover");
    FindJsonString(current_block, "time", g_now_time, sizeof(g_now_time));

    g_now_precip_percent = -1;
    g_base_index = -1;
    g_hour_count = 0;

    if (hourly_block) {
        g_precip_arr = FindArrayByKey(hourly_block, "\"precipitation_probability\":[");
        g_cloud_arr = FindArrayByKey(hourly_block, "\"cloud_cover\":[");
        g_dir_arr = FindArrayByKey(hourly_block, "\"wind_direction_10m\":[");
        g_wind_arr = FindArrayByKey(hourly_block, "\"wind_speed_10m\":[");
        g_temp_arr = FindArrayByKey(hourly_block, "\"temperature_2m\":[");
        g_time_arr = FindArrayByKey(hourly_block, "\"time\":[");

        if (g_time_arr) {
            char hour_prefix[16];
            size_t plen = strlen(g_now_time);

            if (plen > 13)
                plen = 13;
            memcpy(hour_prefix, g_now_time, plen);
            hour_prefix[plen] = '\0';

            g_base_index = FindIndexBySubstring(g_time_arr, hour_prefix);
            g_hour_count = CountCsvItems(g_time_arr);

            if (g_base_index >= 0 && g_precip_arr)
                g_now_precip_percent = GetIntAtIndex(g_precip_arr, g_base_index);
        }
    }

    RenderForecastOffset(0);
}

/* -------------------------------------------------------------------- */
/* Ikonki wierszy - rysowane na zywo w docelowym rect z prymitywow      */
/* ui_fill_rect/ui_fill_circle/ui_draw_circle/ui_draw_line, zamiast     */
/* jednorazowych 1-bitowych Pixmap + XCopyPlane jak w oryginale.        */
/* -------------------------------------------------------------------- */

static void
DrawThermoIcon(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int cx = r.x + r.w / 2;
    int stem_w = r.w * 3 / 16;
    int bulb_r = r.w * 4 / 16;
    UiRect stem;

    if (stem_w < 2) stem_w = 2;
    if (bulb_r < 3) bulb_r = 3;

    stem = (UiRect){ cx - stem_w / 2, r.y, stem_w, r.h * 10 / 16 };
    ui_fill_rect(ctx, stem, fg);
    ui_fill_circle(ctx, cx, r.y + r.h * 12 / 16, bulb_r, fg);
}

static void
DrawCloudIcon(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int cy = r.y + r.h * 9 / 16;
    UiRect base;

    ui_fill_circle(ctx, r.x + r.w * 5 / 16, cy, r.w * 4 / 16, fg);
    ui_fill_circle(ctx, r.x + r.w * 8 / 16, r.y + r.h * 6 / 16, r.w * 5 / 16, fg);
    ui_fill_circle(ctx, r.x + r.w * 12 / 16, cy, r.w * 4 / 16, fg);

    base = (UiRect){ r.x + r.w * 3 / 16, cy, r.w * 10 / 16, r.h * 4 / 16 };
    ui_fill_rect(ctx, base, fg);
}

static void
DrawRainIcon(UiCtx *ctx, UiRect r, const XColor *fg)
{
    UiRect cloud_r = { r.x, r.y, r.w, r.h * 11 / 16 };
    int y0 = r.y + r.h * 11 / 16;
    int y1 = r.y + r.h - 1;

    DrawCloudIcon(ctx, cloud_r, fg);
    ui_draw_line(ctx, r.x + r.w * 4 / 16, y0, r.x + r.w * 3 / 16, y1, 1, fg);
    ui_draw_line(ctx, r.x + r.w * 8 / 16, y0, r.x + r.w * 7 / 16, y1, 1, fg);
    ui_draw_line(ctx, r.x + r.w * 12 / 16, y0, r.x + r.w * 11 / 16, y1, 1, fg);
}

/* Strzalka od srodka do brzegu okregu, wskazujaca degrees (0 = polnoc/gora,
 * rosnie zgodnie z ruchem wskazowek zegara) - ten sam pomysl co
 * MakeWindIcon w oryginale, tylko rysowany wprost zamiast na osobnej
 * Pixmapie. */
static void
DrawWindIcon(UiCtx *ctx, UiRect r, const XColor *fg, double degrees)
{
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    int radius = (r.w < r.h ? r.w : r.h) / 2 - 1;
    double theta = degrees * M_PI / 180.0;
    int tip_x, tip_y;

    if (radius < 2) radius = 2;

    ui_draw_circle(ctx, cx, cy, radius, 1, fg);

    tip_x = cx + (int) lround(radius * sin(theta));
    tip_y = cy - (int) lround(radius * cos(theta));
    ui_draw_line(ctx, cx, cy, tip_x, tip_y, 1, fg);
    ui_fill_circle(ctx, tip_x, tip_y, 2, fg);
}

/* Ikonka okna (WM/taskbar) - jedyne miejsce, ktore nadal potrzebuje
 * surowej, 1-bitowej Pixmapy (tego wymaga XWMHints), wiec rysowana wprost
 * Xlibem, nie przez ui.c (ktore rysuje na backbufferze UiCtx, nie na
 * dowolnej Pixmapie) - identyczny ksztalt co DrawWindowIcon w oryginale. */
static void
DrawWindowIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XFillArc(idpy, p, gc, 4, 3, 14, 14, 0, 360 * 64);
    XDrawLine(idpy, p, gc, 11, 0, 11, 3);
    XDrawLine(idpy, p, gc, 21, 3, 24, 0);
    XDrawLine(idpy, p, gc, 24, 10, 28, 10);
    XSetForeground(idpy, gc, 0);
    XFillArc(idpy, p, gc, 2, 14, 20, 14, 0, 360 * 64);
    XSetForeground(idpy, gc, 1);
    XFillArc(idpy, p, gc, 4, 16, 11, 11, 0, 360 * 64);
    XFillArc(idpy, p, gc, 11, 13, 13, 13, 0, 360 * 64);
    XFillArc(idpy, p, gc, 18, 17, 10, 10, 0, 360 * 64);
    XFillRectangle(idpy, p, gc, 8, 22, 18, 6);
}

static Pixmap
MakeWindowIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawWindowIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI - jedna funkcja per klatka, wzorzec z examples/demo.c     */
/* -------------------------------------------------------------------- */

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    /* tlo boxa, obramowanie i kolor ikonek pochodza z osobnych, wezszych
     * zasobow X (boxBackground/lineForeground/iconForeground), kazdy z
     * wlasnym fallbackiem na background/foreground, jesli nie ustawiony -
     * patrz ui_theme_box_bg/ui_theme_line_fg/ui_theme_icon_fg w ui.h i
     * Xresources.sample w katalogu glownym. */
    const XColor *icon_fg = ui_theme_icon_fg(ctx);
    int y = 0;
    int i;

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
        ready = 1;
    }

    /* header: strzalki </> (przewijanie prognozy godzina po godzinie) +
     * tekst naglowka - klik przelicza g_line_text/g_header_text/
     * g_wind_dir OD RAZU (patrz RenderForecastOffset), wiec ta sama
     * klatka pokazuje juz nowy stan przy rysowaniu contentu ponizej. */
    UiBox *header = ui_box_begin(ctx, "header", 0, y, win_w, &style);
    UiRect hrow = ui_box_next_rect(header, ROW_H);
    UiRect prev_r, mid_r, next_r;
    ui_rect_split3(hrow, ROW_H, ROW_H, 6, &prev_r, &mid_r, &next_r);
    if (ui_button(ctx, prev_r, "<"))
        RenderForecastOffset(g_offset - 1);
    if (ui_button(ctx, next_r, ">"))
        RenderForecastOffset(g_offset + 1);
    ui_label_centered(ctx, mid_r, g_header_text);
    ui_box_end(header);
    y += style.margin_t + ui_box_height(ctx, "header") + style.margin_b;

    /* content: 4 stale wiersze - Temperature/Wind/Cloud cover/Precipitation,
     * kazdy z wlasna ikonka po lewej (patrz Draw*Icon powyzej). */
    UiBox *content = ui_box_begin(ctx, "content", 0, y - style.margin_t, win_w, &style);
    for (i = 0; i < 4; i++) {
        UiRect row = ui_box_next_rect(content, ROW_H);
        UiRect icon_r = { row.x, row.y, ROW_H - 2, row.h };
        UiRect text_r = { row.x + ROW_H + ICON_TEXT_GAP, row.y,
                           row.w - (ROW_H + ICON_TEXT_GAP), row.h };

        switch (i) {
        case 0: DrawThermoIcon(ctx, icon_r, icon_fg); break;
        case 1: DrawWindIcon(ctx, icon_r, icon_fg, g_wind_dir); break;
        case 2: DrawCloudIcon(ctx, icon_r, icon_fg); break;
        case 3: DrawRainIcon(ctx, icon_r, icon_fg); break;
        }
        ui_label(ctx, text_r, g_line_text[i]);
    }
    ui_box_end(content);
    y += ui_box_height(ctx, "content") + style.margin_b;

    /* Refresh */
    UiRect refresh_r = { style.margin_l, y, ui_button_width(ctx, "Refresh"), ROW_H };

    if (ui_button(ctx, refresh_r, "Refresh"))
        UpdateWeather();

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
    char title[320];
    int win_w = 260, win_h = 170;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    long next_refresh_ms;
    XEvent ev;

    /* -geometry/-geom WxH+X+Y jak w standardowych apkach X11 - musi byc
     * wychwycone PRZED laczeniem lokalizacji, zeby jego dwa tokeny (flaga
     * + wartosc) nie trafily do location_query. W oryginale ta sama flaga
     * wymagala sporej obrobki (patrz komentarz na gorze pliku), bo Xt/Shell
     * osobno negocjowal szerokosc podczas Realize - tu, bez Shella, wartosci
     * z XParseGeometry sa stosowane wprost przy tworzeniu okna, nizej. */
    location_query[0] = '\0';
    for (i = 1; i < argc; i++) {
        size_t len;

        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
            continue;
        }

        len = strlen(location_query);
        snprintf(location_query + len, sizeof(location_query) - len,
                  "%s%s", (location_query[0] != '\0') ? " " : "", argv[i]);
    }
    if (location_query[0] == '\0') {
        fprintf(stderr, "Usage: %s [-geometry WxH+X+Y] <location>\n", argv[0]);
        return 1;
    }

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

    snprintf(title, sizeof(title), "7aWeather - %s", location_query);
    XStoreName(dpy, win, title);
    XSetIconName(dpy, win, title);

    icon = MakeWindowIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    /* min != max na OBU osiach - bez jawnego PMaxSize niektore WM (np.
     * vtwm) przy nieokreslonym maksimum traktuja je jako rowne minimum,
     * co blokuje caly uchwyt resize (nie tylko dolna granice) - ten sam
     * problem i to samo rozwiazanie co przy XtNminWidth/XtNmaxWidth w
     * ../7aweather/7aweather.c. */
    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 160;
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);
    /* XSetInputFocus dopiero po MapNotify w petli zdarzen ponizej - patrz
     * ten sam komentarz w examples/demo.c. */

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    /* Narysuj OD RAZU jedna klatke z placeholderami ("...", patrz
     * inicjalizacja g_line_text), ZANIM zaczniemy blokujace UpdateWeather()
     * (curl - od kilkudziesieciu ms do sekund, zalezne od sieci) - bez
     * tego okno wisialoby CALKOWICIE puste (niepomalowany backbuffer)
     * przez caly czas pobierania, bo zaden Expose nie jest jeszcze
     * obslugiwany (petla zdarzen ponizej jeszcze sie nie zaczela). Uzytkownik
     * widzi wiec od razu uklad okna z "...", a dopiero po fetchu swieze dane -
     * ten sam mechanizm co placeholder "..." w oryginale (tam Xt i tak
     * najpierw realizuje/pokazuje widgety, dopiero potem UpdateWeather()
     * podmienia ich XtNlabel). */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    UpdateWeather();

    running = 1;
    redraw = 1;  /* pokaz swieze dane od razu, bez czekania na kolejne zdarzenie */
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

        /* Timer odswiezania bez Xt: select() z timeoutem na deskryptorze
         * polaczenia X - gdy uplynie, UpdateWeather() (blokujace, tak samo
         * jak XtAppAddTimeOut->RefreshTimer w oryginale) i redraw. */
        long remaining = next_refresh_ms - now_ms();

        if (remaining <= 0) {
            UpdateWeather();
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

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
