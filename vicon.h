/* vicon - ikony wektorowe w stylu IRIX Indigo Magic (ikony SGI .fti
 * przekonwertowane do SVG) rysowane w dowolnym rozmiarze, ostro, bez
 * antyaliasingu i bez rasteryzacji do bitmap.
 *
 * Czyta tylko podzbior SVG, z ktorego skladaja sie takie ikony: <svg
 * viewBox>, zagniezdzone <g>/<path> z transform (matrix/scale/translate),
 * styl plaski (fill/stroke #rgb/#rrggbb/none, fill-opacity) w atrybucie
 * style albo jako atrybuty fill/stroke, sciezki d z poleceniami M/L/H/V/Z
 * (wielkie i male). Krzywe (C/S/Q/T/A), gradienty itp. = plik odrzucony
 * (vicon_load zwraca 0), apka uzywa wtedy innej ikony. Polprzezroczyste
 * wypelnienie (cienie) jest mieszane z kolorem tla bg podanym przy
 * ladowaniu - nie ma tu prawdziwej przezroczystosci.
 *
 * Pamiec: vicon_load alokuje (malloc) punkty, sciezki i kolory jednej
 * ikony; vicon_draw nie alokuje nic (staly bufor na stosie). Wlasnosc:
 * wszystko, co zaladowal vicon_load (w tym piksele kolorow z XAllocColor
 * w DefaultColormap), zwalnia wylacznie vicon_free. Rysowanie wylacznie
 * przez publiczne ui.h (ui_fill_polygon/ui_draw_polyline). */
#ifndef VICON_H
#define VICON_H

#include "ui.h"

/* wspolrzedne punktow w jednostkach 1/VICON_UNIT boku viewBoxa */
#define VICON_UNIT 10000
/* dluzsze sciezki sa obcinane przy ladowaniu */
#define VICON_MAX_PATH_PTS 256

typedef struct {
    int first;              /* indeks pierwszego punktu w Vicon.pts */
    unsigned short n;       /* liczba punktow */
    unsigned char fill;     /* indeks w Vicon.colors, VICON_NONE = brak */
    unsigned char stroke;
    unsigned char closed;   /* Z na koncu */
} ViconPath;

#define VICON_NONE 255

typedef struct {
    short *pts;             /* x0,y0,x1,y1,... (npts par) */
    ViconPath *paths;
    XColor *colors;
    int npts, npaths, ncolors;
} Vicon;

/* laduje plik SVG (dpy = ten sam co w ui_init, kolory w jego
 * DefaultColormap) do *out (zerowanego na starcie). bg = kolor tla,
 * z ktorym mieszane sa polprzezroczyste wypelnienia. Zwraca 1 przy
 * sukcesie, 0 gdy pliku nie da sie przeczytac albo wychodzi poza
 * obslugiwany podzbior SVG (*out zostaje wtedy pusty, bez zasobow). */
int  vicon_load(Display *dpy, const char *path, const XColor *bg, Vicon *out);

/* rysuje ikone wpisana (z zachowaniem proporcji kwadratowego viewBoxa,
 * wysrodkowana) w prostokat r. Respektuje ui_set_clip. */
void vicon_draw(UiCtx *ctx, const Vicon *v, UiRect r);

/* zwalnia pamiec i kolory ikony (dpy = ten sam co w ui_init), *v zostaje
 * wyzerowane - bezpieczne na pustej/juz zwolnionej ikonie. */
void vicon_free(Display *dpy, Vicon *v);

#endif
