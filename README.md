# 7adesktop

Minimalistyczny desktop manager dla X11 (Linux) budowany z jak najniższego
poziomu — bez toolkitów typu FLTK/GTK/Qt. Cel: mniejsze i szybsze binarki
niż przy użyciu klasycznych bibliotek widgetów, poprzez pisanie bezpośrednio
na Xlib/Xft, bez dodatkowych warstw abstrakcji.

Inspiracja: projekty w duchu [suckless](https://suckless.org/) (dwm, dmenu,
slstatus) — mały, czytelny kod C, statyczne linkowanie, zero zbędnych
zależności runtime.

## Stan projektu

Obecnie istnieje zalążek własnej biblioteki widgetów (`ui.h` / `ui.c`),
kompilowanej do `libui.a` i linkowanej statycznie do każdej apki.

Zaimplementowane elementy:

- `UiCtx` — kontekst rysowania (Display/Window/GC, Xft font, kolory, stan
  myszy, backbuffer)
- `UiBox` — kontener na szerokość okna, wysokość z zawartości; obsługuje
  margin/padding/border (grubość + kolor), kolor tła oraz `gap` (odstęp
  pionowy między kolejnymi `ui_box_next_rect`, pomijany przed pierwszym)
- `ui_label` / `ui_label_centered`, `ui_button`, `ui_checkbox`, `ui_list` —
  podstawowe widgety
- `ui_rect_col`, `ui_rect_split3` — bezstanowe prymitywy geometryczne do
  osadzania widgetów obok siebie w jednym wierszu (N równych kolumn, albo
  lewo/środek/prawo ze stałą szerokością krawędzi) — biblioteka nie ma
  automatycznego layoutu, apka sama liczy `UiRect`
- `ui_box_height` — wysokość boxa z poprzedniej klatki (ten sam cache co
  tło/border), do ustawiania kolejnego boxa pod poprzednim
- `examples/demo.c` — działający przykład: box "main" (label + przycisk z
  checkboxem w jednym wierszu + lista) i box "nav" pod nim (przycisk /
  wyśrodkowany tekst / przycisk), z obsługą zmiany rozmiaru okna

### Ważne szczegóły architektoniczne

- **Cache wysokości boxa**: `UiBox` rysuje tło/border na podstawie wysokości
  **z poprzedniej klatki** (cache po `id` w `UiCtx`), bo w immediate-mode nie
  da się poznać wysokości zawartości przed jej narysowaniem, a tło musi być
  pod dziećmi. Skutek: pierwsza klatka danego boxa może pojawić się bez
  tła/bordera — samoczynnie naprawia się w kolejnej klatce. To świadomy
  kompromis, nie bug.
- **Double buffering**: wszystko rysowane jest do offscreenowego `Pixmap`
  (`ui_begin_frame`), a na okno trafia jednym `XCopyArea` w `ui_end_frame`.
  Bez tego częste przerysowywanie (np. na `MotionNotify`) migało przez
  `XClearWindow` przed każdą klatką. Backbuffer trzeba przealokować przy
  zmianie rozmiaru okna — stąd `ui_resize`, wołane też z `ui_init`.
- **Motyw z X resource database**: `ui_init` czyta `background`/
  `foreground`/`activeBackground` z `RESOURCE_MANAGER` (czyli to, co ładuje
  `xrdb` z `~/.Xresources`), żeby apki korzystające z biblioteki miały taki
  sam motyw kolorów jak reszta środowiska (np. terminal, WM). Zapytanie
  używa nazw bez prefiksu klasy aplikacji, więc trafia tylko w globalne
  wpisy typu `*background: ...`, a nie w ustawienia specyficzne dla innych
  konkretnych programów (zweryfikowane bezpośrednio przez `XrmGetResource`).
  Jeśli baza zasobów jest pusta/niedostępna, zostają hardkodowane
  fallbacki. Apka może nadpisać kolory po `ui_init` przez `ui_color`.

## Build

```sh
make          # buduje libui.a i demo (dziala na Linuksie i OpenBSD)
DISPLAY=:0 ./demo
make clean
```

Zależności: `libX11`, `libXft` (dev headers). Flagi kompilatora/linkera
wykrywa `x11-flags.sh`: najpierw próbuje `pkg-config` (Linux), a jeśli go
brak lub nie zna X11/Xft (OpenBSD — Xlib/Xft są częścią bazowego systemu
Xenocara pod `/usr/X11R6` i nie mają wpisów w pkg-config), używa ścieżek
`/usr/X11R6` + FreeType spod `/usr/local`. Makefile używa `!=` zamiast
`$(shell ...)`, żeby działać zarówno pod GNU make, jak i pod `bmake`
(domyślny `make` na OpenBSD) — `$(shell ...)` to rozszerzenie tylko GNU make.

## Filozofia projektu

- Zero zależności poza Xlib/Xft — żadnego toolkitu, żadnego frameworka.
- Immediate-mode UI: apka woła funkcje widgetów co klatkę, biblioteka nie
  trzyma drzewa obiektów ani nie zarządza pamięcią widgetów za apkę.
- Współdzielenie kodu między apkami przez statyczne linkowanie `libui.a`,
  a nie przez współdzielone procesy/IPC widgetów (to osobny, cięższy temat
  — potrzebny tylko gdyby pojawiła się potrzeba np. XEmbed).
- Iterujemy w locie — API nie jest zamrożone, zmiany sygnatur są ok, jeśli
  upraszczają kod lub eliminują klasę błędów.

## Bezpieczeństwo i jakość kodu — priorytet w tym projekcie

Ten projekt ma być punktem odniesienia jakości, nie tylko działającym
prototypem. Przy każdej zmianie:

- **Brak przepełnień bufora**: żadnego `strcpy`/`sprintf`/`strcat` bez
  limitu. Używać `snprintf` i jawnie sprawdzać rozmiary (patrz
  `box_cache_set` w `ui.c` jako wzorzec).
- **Walidacja danych z X**: pola `XEvent` (współrzędne, liczniki) traktować
  jako dane wejściowe spoza kontrolowanej domeny — nie zakładać zakresów
  bez sprawdzenia, szczególnie tam, gdzie indeksują tablice.
- **Jawna własność zasobów**: każdy `XftColorAllocName`, `XftFontOpenName`,
  `XCreateGC` itp. musi mieć jasno udokumentowane, kto i kiedy go zwalnia
  (`XftColorFree`, `XftFontClose`, `XFreeGC`). Nie zostawiać tego
  domyślnie/domniemanie.
- **Stałe rozmiary zamiast alokacji w pętli zdarzeń**: unikać `malloc` w
  ścieżce rysowania/obsługi zdarzeń tam, gdzie da się użyć stałych buforów
  (jak `box_stack`/`box_cache` w `UiCtx`) — przewidywalna wydajność i mniej
  miejsc na wycieki.
- **Kompilacja bez ostrzeżeń**: `-Wall -Wextra` musi przechodzić czysto
  (patrz `Makefile`). Nowe ostrzeżenie = do naprawienia przed commitem,
  nie do zignorowania.
- **Czytelność ponad sprytność**: krótkie funkcje, jedna odpowiedzialność,
  bez przedwczesnych abstrakcji — kod ma zostać mały i łatwy do audytu,
  co jest częścią głównego celu projektu (mniejsze i szybsze niż FLTK).
- **Brak niezdefiniowanego zachowania**: uważać na typy w arytmetyce
  wskaźników/rozmiarów (np. `unsigned` vs `int` przy szerokościach/
  wysokościach widgetów), sprawdzać wartości zwracane z funkcji Xlib/Xft
  tam, gdzie mogą zwrócić błąd (np. `XftFontOpenName` może zwrócić `NULL`).

Każda nowa funkcja publiczna w `ui.h` powinna być rozpatrywana pod kątem:
da się to samo zrobić prościej / bezpieczniej / mniejszym kosztem?

### Log audytu

Pierwszy pełny przegląd `ui.c` względem powyższej checklisty (2026-07-24)
wykrył i naprawił:

- `ui_box_begin` zapisywał do `box_stack[box_stack_top++]` bez sprawdzenia
  granicy `UI_MAX_BOX_DEPTH` — przy zbyt głębokim zagnieżdżeniu/zbyt wielu
  niedomkniętych boxach byłby to zapis poza tablicą. Naprawione: nadmiarowe
  wywołania dzielą teraz ostatni slot zamiast wychodzić poza pamięć.
  `ui_box_end` dostał analogiczne zabezpieczenie przed niedopełnieniem.
- `examples/demo.c` alokowało kolory stylu (`ui_color` → `XftColorAllocName`)
  w `draw()`, czyli przy każdej klatce (a więc przy każdym ruchu myszką) —
  niepotrzebny narzut po stronie serwera X bez zwalniania poprzednich
  alokacji. Naprawione: kolory i styl budowane są raz (guard `style_ready`).
- `ui_init` mógł zwrócić `NULL` (brak fontu), a `demo.c` nie sprawdzało
  wyniku przed użyciem `ctx`. Dodano sprawdzenie.

Wciąż świadomie pominięte (niska waga / inny rodzaj granicy zaufania): brak
sprawdzania wyniku `ui_color()` przy nazwach kolorów zaszytych w kodzie —
to input od programisty, nie od użytkownika/systemu w runtime.
