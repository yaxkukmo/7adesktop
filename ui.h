#ifndef UI_H
#define UI_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

typedef struct UiCtx UiCtx;
typedef struct UiBox UiBox;

typedef struct {
    int x, y, w, h;
} UiRect;

typedef struct {
    int margin_l, margin_t, margin_r, margin_b;
    int padding_l, padding_t, padding_r, padding_b;
    int border_w;
    XftColor border_color;
    XftColor bg_color;
    int gap; /* odstep pionowy miedzy kolejnymi ui_box_next_rect (nie przed pierwszym) */
} UiBoxStyle;

/* dpy/win/gc dostarcza wywolujacy - juz utworzone i zmapowane okno.
 * w/h to poczatkowy rozmiar okna, uzywany do zaalokowania backbuffera
 * (patrz ui_resize ponizej - rysowanie jest podwojnie buforowane, zeby
 * uniknac migotania przy czestym przerysowywaniu, np. na MotionNotify).
 * Domyslne kolory (fg/bg/accent) sa czytane z bazy zasobow X (background/
 * foreground/activeBackground - to, co laduje xrdb z ~/.Xresources), zeby
 * apka pasowala do reszty srodowiska; jesli baza jest pusta, uzywane sa
 * hardkodowane fallbacki. Apka moze je nadpisac po ui_init przez ui_color.
 * fontname to TYLKO fallback - zasob "uiFont" w tej samej bazie (skladnia
 * fontconfig, jak sam fontname, np. "DejaVu Sans-10", NIE XLFD; nazwa
 * CELOWO nie jest samym "font" - to by kolidowalo z XtNfont, powszechnym
 * zasobem kazdej apki Xt/Xaw/Motif na tym systemie, patrz Xresources.sample)
 * ma pierwszenstwo, jesli ustawiony; jesli resource jest zly (literowka,
 * nieznana rodzina), cichy powrot do fontname z tego argumentu. */
UiCtx *ui_init(Display *dpy, Window win, GC gc, const char *fontname, int w, int h);
void   ui_destroy(UiCtx *ctx);

/* name: "#rrggbb" lub nazwa X11 (np. "steelblue") */
int    ui_color(UiCtx *ctx, const char *name, XftColor *out);

/* kolory motywu zaladowane w ui_init z bazy zasobow X (patrz tam) lub z
 * hardkodowanych fallbackow, jesli baza jest pusta - do wlasnych
 * elementow apki (np. wlasnych UiBoxStyle), zeby tez respektowaly
 * background/foreground/activeBackground z .Xresources zamiast wlasnych,
 * zaszytych na sztywno kolorow. Zwracany wskaznik jest wlasnoscia ctx -
 * wazny do ui_destroy, nie zwalniac go samodzielnie.
 *
 * box_bg/button_bg/icon_fg/line_fg to wezsze kategorie z wlasnymi
 * zasobami (boxBackground/buttonBackground/iconForeground/lineForeground),
 * kazda domyslnie dziedziczaca po bg/fg, jesli nie ustawiona osobno -
 * patrz init_theme_colors w ui.c. ui_button/ui_checkbox/ui_textbox juz z
 * nich korzystaja wewnetrznie (button_bg/line_fg); box_bg/icon_fg sa do
 * dyspozycji apki przy wlasnych UiBoxStyle/ikonkach, tak jak w
 * examples/7aweather.c.
 *
 * bar_active_bg/bar_inactive_bg to kolory ui_meter/ui_segment_meter (patrz
 * nizej), z wlasnymi zasobami activeBarBg/inactiveBarBg - domyslnie
 * dziedzicza po accent/box_bg, ale MAJA WLASNY zasob, wiec da sie
 * przekolorowac paski bez zmiany koloru hover ui_button (ktory tez uzywa
 * accent). */
const XftColor *ui_theme_fg(UiCtx *ctx);
const XftColor *ui_theme_bg(UiCtx *ctx);
const XftColor *ui_theme_accent(UiCtx *ctx);
const XftColor *ui_theme_box_bg(UiCtx *ctx);
const XftColor *ui_theme_button_bg(UiCtx *ctx);
const XftColor *ui_theme_icon_fg(UiCtx *ctx);
const XftColor *ui_theme_line_fg(UiCtx *ctx);
const XftColor *ui_theme_bar_active_bg(UiCtx *ctx);
const XftColor *ui_theme_bar_inactive_bg(UiCtx *ctx);

/* odleglosc box-ow od LEWEJ/PRAWEJ krawedzi okna (px) - domyslnie 8,
 * nadpisywalne zasobem X "windowMargin"/"WindowMargin" (patrz
 * Xresources.sample), wczytywanym raz w ui_init. Apki uzywaja tej
 * wartosci zamiast wlasnych hardkodowanych stalych przy pozycjonowaniu
 * box-ow wzgledem lewej/prawej krawedzi (np. style.margin_l/margin_r).
 * Odstepy PIONOWE (gora/dol, UiBoxStyle.margin_t/margin_b) to osobna
 * sprawa - czesto pelnia TEZ role odstepu miedzy kolejnymi box-ami, nie
 * tylko odleglosci od krawedzi, wiec zostaja hardkodowane per-apka. */
int ui_window_margin(UiCtx *ctx);

void   ui_feed_event(UiCtx *ctx, XEvent *ev);

/* wywolac po zmianie rozmiaru okna (np. w obsludze ConfigureNotify) -
 * przealokowuje backbuffer; no-op jesli rozmiar sie nie zmienil */
void   ui_resize(UiCtx *ctx, int w, int h);

/* czysci backbuffer kolorem tla - wywolac przed rysowaniem widgetow */
void   ui_begin_frame(UiCtx *ctx);
/* kopiuje backbuffer na okno (XCopyArea) i czysci jednoklatkowe flagi
 * (np. click) - wywolac po narysowaniu wszystkich widgetow w tej klatce */
void   ui_end_frame(UiCtx *ctx);

/* kontener - szerokosc na caly przekazany width, wysokosc z zawartosci.
 * tlo/border rysowane sa na podstawie wysokosci z POPRZEDNIEJ klatki
 * (cache po id), wiec pierwsza klatka moze byc bez tla - patrz README. */
UiBox *ui_box_begin(UiCtx *ctx, const char *id, int x, int y, int width, const UiBoxStyle *style);
UiRect ui_box_next_rect(UiBox *box, int height);
void   ui_box_end(UiBox *box);

/* wysokosc boxa o danym id (border+padding+zawartosc, BEZ marginesow)
 * z POPRZEDNIEJ klatki, 0 jesli jeszcze nieznana - do pozycjonowania
 * kolejnego boxa pod poprzednim (dolicz margin_t+margin_b samodzielnie) */
int    ui_box_height(UiCtx *ctx, const char *id);

/* widgety - zwracaja true jesli w tej klatce doszlo do akcji */
void   ui_label(UiCtx *ctx, UiRect r, const char *text);          /* wyrownanie do lewej */
void   ui_label_centered(UiCtx *ctx, UiRect r, const char *text); /* wyrownanie do srodka */

/* jak ui_label/ui_label_centered, ale z wlasnym kolorem tekstu zamiast
 * zawsze ctx->fg - np. inny kolor cyfry dnia dla weekendow/swiat w
 * kalendarzu (patrz examples/7acal.c). */
void   ui_label_fg(UiCtx *ctx, UiRect r, const char *text, const XftColor *color);
void   ui_label_centered_fg(UiCtx *ctx, UiRect r, const char *text, const XftColor *color);
int    ui_button(UiCtx *ctx, UiRect r, const char *label);
int    ui_checkbox(UiCtx *ctx, UiRect r, const char *label, int *state);

/* dlugi prostokat pokazujacy wartosc jako proporcje dwoma kolorami (wypelniona
 * czesc = bar_active_bg, tlo = bar_inactive_bg, ramka = line_fg - patrz
 * activeBarBg/inactiveBarBg przy ui_theme_bar_active_bg wyzej) - do
 * przedstawienia wartosci typu "1.8G z 8.3G" (RAM) czy "Signal: 67%" jako
 * pasek zamiast samego tekstu, patrz examples/7asensors.c. frac spoza
 * zakresu 0..1 jest przycinany do tego zakresu. label (moze byc NULL/"")
 * rysowany wysrodkowany na wierzchu paska kolorem fg (tym samym wzorcem co
 * tekst w ui_button) - apka sama formatuje jego tresc, widget nie zna
 * jednostek. */
void   ui_meter(UiCtx *ctx, UiRect r, double frac, const char *label);

/* rzad total rownych kwadracikow w dwoch kolorach (pierwsze active =
 * bar_active_bg, reszta = bar_inactive_bg, kazdy z ramka line_fg) -
 * odpowiednik ui_meter dla wartosci z natury DYSKRETNEJ/policzalnej
 * zamiast ciaglej, np. "ktore rdzenie CPU sa online" (4 z 8) zamiast
 * tekstu "Cores: 4/8", patrz examples/7asensors.c. Kwadracik ma bok =
 * min(szerokosc swojej kolumny, r.h), wyśrodkowany w kolumnie o
 * szerokosci (r.w - gap*(total-1))/total (patrz ui_rect_col) - przy zbyt
 * waskim r kwadraciki po prostu sie scisniete. active/total przycinane do
 * 0..total, total<=0 to no-op. */
void   ui_segment_meter(UiCtx *ctx, UiRect r, int active, int total, int gap);

/* mala "zaznaczalna" ikonka - kwadrat z ramka (line_fg), wypelniona
 * kropka (accent) w srodku gdy checked - do list z WYLACZNYM
 * zaznaczeniem (radio-jak: co najwyzej jeden wiersz na raz), gdzie
 * ui_checkbox (niezalezny bool + wlasna etykieta obok) nie pasuje, bo
 * apka sama decyduje, co "checked" znaczy (np. index == selected_index) -
 * patrz examples/7atodo.c. Tylko rysuje, nie hit-testuje ani nie zmienia
 * zadnego stanu - wykryj klikniecie osobno przez ui_hit_test na TYM
 * SAMYM rect i zaktualizuj wlasny stan PRZED wywolaniem tej funkcji
 * (ten sam wzorzec "najpierw stan, potem rysowanie na jego podstawie"
 * co ui_checkbox - w przeciwnym razie zmiana bedzie widoczna dopiero w
 * NASTEPNEJ klatce, nie w tej, w ktorej user faktycznie kliknal). */
void   ui_selection_mark(UiCtx *ctx, UiRect r, int checked);

/* r.h dzielone rowno na n wierszy (bez przewijania - to future work).
 * *selected to indeks aktywnego elementu, zmieniany po kliknieciu. */
int    ui_list(UiCtx *ctx, UiRect r, const char **items, int n, int *selected);

/* pole tekstowe. buf to bufor apki, NUL-terminated, o pojemnosci buf_cap
 * (wraz z NUL) - apka go alokuje/inicjalizuje, widget go edytuje w miejscu.
 * *cursor to pozycja karetki (w BAJTACH bufora UTF-8, zawsze na granicy
 * punktu kodowego, 0..strlen(buf)), tez wlasnosc apki, zachowywana miedzy
 * klatkami tak jak *state w ui_checkbox.
 * Fokus klawiatury nabywany klikiem w widget, tracony klikiem poza nim -
 * identyfikowany przez wskaznik buf, wiec kazdy bufor to osobne pole.
 * Wejscie tekstowe idzie przez Xutf8LookupString (jesli ui_init zdolal
 * otworzyc lokalna metode wejscia - patrz XIM/XIC w ui_init), wiec pelny
 * UTF-8 (w tym polskie znaki diakrytyczne) dziala; Backspace/Delete/Left/
 * Right przeskakuja cale punkty kodowe. Ograniczenia v1: bez zaznaczania/
 * kopiowania. */
int    ui_textbox(UiCtx *ctx, UiRect r, char *buf, int buf_cap, int *cursor);

/* true jesli w tej klatce doszlo do kliknieca WEWNATRZ r - do budowania
 * wlasnych klikalnych obszarow (np. komorek siatki dni w
 * examples/7acal.c), gdy zaden z gotowych widgetow (ui_button/ui_list/...)
 * nie pasuje do potrzebnego wygladu/kolorowania. */
int    ui_hit_test(UiCtx *ctx, UiRect r);

/* pozycja kursora (wspolrzedne w oknie) oraz czy LPM jest W TEJ KLATCE
 * wcisniety (surowy stan, nie "kliknieto") - do widgetow z przeciaganiem
 * (np. kciuk scrollbara w examples/7afm.c), gdzie ui_hit_test (caly klik
 * = press+release) nie wystarcza, bo trzeba sledzic pozycje MIEDZY
 * ButtonPress a ButtonRelease. Sesja przeciagania (kiedy sie zaczyna/
 * konczy, punkt odniesienia) to wlasnosc APKI, nie biblioteki - ten sam
 * podzial odpowiedzialnosci co przy pozostalych widgetach "najpierw
 * stan, potem rysowanie". x/y/down moga byc NULL, jesli apka nie
 * potrzebuje danej wartosci. */
void   ui_mouse_state(UiCtx *ctx, int *x, int *y, int *down);

/* wiersz w rozwinietym menu (lub podobnej liscie tekstowej) - podswietlony
 * (accent) na hover, tekst wyrownany do lewej - do budowania prostych,
 * rysowanych "na wierzchu" dropdownow bez prawdziwego okna popup/grab
 * (ten sam, sprawdzony wzorzec co dropdown priorytetu w
 * examples/7atodo.c), np. paska menu File/Edit/View w examples/7afm.c.
 * Sam pasek menu (gorne "File"/"Edit"/...) to zwykly ui_button - ten
 * prymityw jest tylko do WIERSZY w rozwinietej liscie ponizej niego.
 * Zwraca true jesli kliknieto ta klatke. */
int    ui_menu_item(UiCtx *ctx, UiRect r, const char *label);

/* szerokosc tekstu w foncie ctx, w pikselach - do recznego zawijania
 * dlugich tekstow na wiersze (np. examples/7amessage.c), zeby apka nie
 * musiala otwierac wlasnego, drugiego XftFont tylko do pomiaru. */
int    ui_text_width(UiCtx *ctx, const char *text);

/* wysokosc pojedynczego wiersza tekstu w foncie ctx (ascent+descent), w
 * pikselach - do ukladania wielu wierszy jeden pod drugim (patrz
 * ui_text_width). Wysokosc UiRect o tej wartosci przekazana do ui_label
 * wyswietli tekst dokladnie od gory tego wiersza (formula centrowania w
 * ui_label_fg sprowadza sie wtedy do baseline = r.y + ascent). */
int    ui_line_height(UiCtx *ctx);

/* prymitywy */
void   ui_fill_rect(UiCtx *ctx, UiRect r, const XftColor *c);
void   ui_draw_border(UiCtx *ctx, UiRect r, int thickness, const XftColor *c);
void   ui_draw_line(UiCtx *ctx, int x1, int y1, int x2, int y2, int thickness, const XftColor *c);
void   ui_fill_circle(UiCtx *ctx, int cx, int cy, int radius, const XftColor *c);
void   ui_draw_circle(UiCtx *ctx, int cx, int cy, int radius, int thickness, const XftColor *c);
void   ui_fill_triangle(UiCtx *ctx, int x0, int y0, int x1, int y1, int x2, int y2, const XftColor *c);

/* rysuje gotowa Pixmap p (o rozmiarze DOKLADNIE r.w x r.h, tej samej
 * glebi/wizualu co okno przekazane do ui_init) na backbufferze - do
 * osadzania zewnetrznych obrazkow, ktorych ui.c nie umie samo wczytac/
 * przeskalowac (np. ikon *.xpm zaladowanych przez libXpm w
 * examples/7acenter.c). Wlasnosc p (kiedy ja zwolnic przez XFreePixmap)
 * zostaje PO STRONIE WYWOLUJACEGO - ta funkcja tylko kopiuje piksele,
 * nie przejmuje pixmapy. Respektuje biezacy ui_set_clip (uzywa tego
 * samego ctx->gc co pozostale prymitywy). Brak obslugi maski ksztaltu/
 * przezroczystosci (v1) - obrazek rysuje sie jako pelny prostokat.
 * r.w<=0 lub r.h<=0 to no-op. */
void   ui_draw_pixmap(UiCtx *ctx, UiRect r, Pixmap p);

/* ogranicza kolejne rysowanie (fill/border/line/circle/tekst - obejmuje
 * TEZ ui_label*, ma wlasny mechanizm przyciecia w Xft) do r, dopoki nie
 * wywolane ui_clear_clip - do przewijalnych obszarow (np. siatka ikon w
 * examples/7afm.c), gdzie tresc poza widocznym viewportem NIE ma sie
 * rysowac na wierzchu sasiednich elementow UI. Brak stosu (jeden poziom) -
 * kolejne ui_set_clip zastepuje poprzednie, nie zageszcza go. */
void   ui_set_clip(UiCtx *ctx, UiRect r);
void   ui_clear_clip(UiCtx *ctx);

/* dzieli row na n rownych kolumn z odstepem gap miedzy nimi (px) i zwraca
 * kolumne o indeksie col (0-based) - do osadzania widgetow obok siebie
 * w jednym wierszu zwroconym przez ui_box_next_rect */
UiRect ui_rect_col(UiRect row, int col, int n, int gap);

/* dzieli row na lewa/srodkowa/prawa czesc: left/right maja stala szerokosc
 * left_w/right_w (np. na przyciski), srodek wypelnia reszte (np. na tekst),
 * z odstepem gap po obu stronach srodka. Kazdy z wskaznikow wyjsciowych
 * moze byc NULL, jesli dana czesc nie jest potrzebna. */
void   ui_rect_split3(UiRect row, int left_w, int right_w, int gap,
                       UiRect *left, UiRect *mid, UiRect *right);

#endif
