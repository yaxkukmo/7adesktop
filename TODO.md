# TODO — audyt bezpieczeństwa i optymalizacji (2026-09-04)

Lista z audytu całego repo (ui.c/ui.h + examples/*.c), do przerabiania po
kolei. Priorytety: 🔴 krytyczne, 🟠 bezpieczeństwo, 🟡 pamięć RAM,
🟢 higiena kodu/build.

## Zrobione

- [x] 🔴 `examples/7abubbles.c` nie linkował się na Linuksie (glibc < 2.36)
      — `arc4random_uniform` bez fallbacku. Naprawione: `rand_uniform()`
      (OpenBSD → `arc4random_uniform`, reszta → `random()`/`srandom()`).
      Commit `abd34b0`.
- [x] 🟠 Brak `pledge`/`unveil` w `7afm.c` mimo że to apka z największą
      powierzchnią ataku w repo (fork+exec rm/mv/cp/opener, rename()).
      Naprawione: `pledge("stdio rpath cpath proc exec unix prot_exec")`
      przed `XOpenDisplay`, świadomie bez `unveil` (patrz komentarz w
      kodzie i uzasadnienie w `7aexit.c`). Commit `abd34b0`.

## Zrobione (c.d.)

- [x] 🟠 **Command injection w `examples/7asensors.c:343`** — argument CLI
      (interfejs sieciowy, `g_iface` z `argv[i]`) trafiał bez sanityzacji do
      `snprintf(cmd, ..., "ifconfig %s 2>/dev/null", g_iface)` →
      `popen(cmd, "r")` czyli `/bin/sh -c`. Naprawione: nowa
      `IsValidIfaceName()` (dozwolone `[A-Za-z0-9._-]`) filtruje argument
      przy parsowaniu `argv` w `main`, przed przypisaniem do `g_iface` —
      nieprawidłowa nazwa jest odrzucana (komunikat na `stderr`, zostaje
      `DEFAULT_IFACE`).

## Do zrobienia

### 🟠 Bezpieczeństwo

- [x] **Niespójny `pledge`/`unveil` na OpenBSD w `7atodo.c`/`7acal.c`** —
      naprawione: obie dodają `pledge("stdio rpath wpath cpath proc exec
      unix prot_exec", NULL)` przed `XOpenDisplay` (`7atodo.c` też
      węższy `"stdio rpath wpath cpath"` w headless `--import`), świadomie
      bez `unveil` — ten sam powód co `7afm.c` (`SpawnCommand`/
      `ResolveTodoCommand` fork+exec'ują dowolny terminal/edytor z X
      resource albo jedną z trzech lokalizacji `7atodo`, więc `unveil`
      dziedziczony po exec by je zablokował). `wpath`/`cpath` na stałe
      (nie tylko przy inicjalizacji), bo zapis do SQLite dzieje się
      bezpośrednio w tym procesie przez cały czas działania, nie przez
      fork+exec jak w `7afm`. Build czysty (`-Wall -Wextra`, Linux —
      blok jest pod `#ifdef __OpenBSD__`, więc niesprawdzone na docelowym
      systemie).

- [x] **Niespójny `pledge`/`unveil` na OpenBSD w pozostałych apkach** —
      naprawione we wszystkich, `7aexit.c` świadomie pominięty (patrz
      komentarz przy `run_cmd`, zostaje bez):
      - Samo `pledge` (bez `unveil`, bo fork+exec'ują zewnętrzne/dowolne
        komendy, które `unveil` by okaleczył — ten sam powód co
        `7afm.c`/`7atodo.c`/`7acal.c`): `7aweather.c`/`7arss.c`
        (`popen(curl)`, `7arss` dodatkowo firefox), `7acenter.c`
        (launcher dowolnych programów z `center.conf`), `7atimer.c`
        (dowolny odtwarzacz alarmu z zasobu X), `7abubbles.c` (terminal
        `urxvtc` — promise dodatkowo z `getpw`, bo `getpwuid()` w
        `scan_projects`/`run_dir`), `7asensors.c` (stałe komendy
        sysctl/ifconfig/vmstat/apm, ale `SpawnDetached` dla przełącznika
        SMT bierze dowolną komendę z zasobu X).
      - `unveil` + `pledge` (wzorzec `7aclip.c:388-415`, bo BEZ
        fork+exec): `demo.c`, `7amessage.c`.
      Build czysty (`-Wall -Wextra`, Linux — bloki pod `#ifdef
      __OpenBSD__`, więc niesprawdzone na docelowym systemie).

- [x] **`realloc()` bez sprawdzenia błędu, z nadpisaniem oryginalnego
      wskaźnika** — naprawione we wszystkich czterech miejscach wzorcem
      `tmp = realloc(p, n); if (!tmp) { ... } else { p = tmp; ... }`:
      - `examples/7afm.c` — `EnsureCap()` zwraca teraz `int`, obaj callerzy
        (`ReadDirectory`) przerywają wczytywanie przy OOM zamiast pisać po
        starym/zwolnionym buforze.
      - `examples/7acenter.c` — analogicznie `EnsureCap()` w
        `LoadLauncherConfig`.
      - `examples/7atodo.c` (`ReadWholeFile`) — przy OOM zwalnia stary
        `buf` i zwraca `NULL` (caller już tak traktuje błąd `fopen`).
      - `examples/7atodo.c` (`RunQuery`, `g_item_ids`) — przy OOM przerywa
        pętlę `sqlite3_step`, zostając przy już wczytanych ID.

### 🟡 Pamięć RAM

- [x] **`entries`/`entry_cap` w `7afm.c` i `7acenter.c` nigdy się nie
      kurczą** — naprawione: nowa `ShrinkCapIfOversized()` w obu plikach
      (ten sam wzorzec), wołana raz na końcu `ReadDirectory`/`LoadEntries`.
      Próg celowo nieagresywny (`entry_cap > 256 && entry_count <
      entry_cap/4`), żeby przechodzenie między katalogami/reloady
      podobnej wielkości nie realokowały bufora bez potrzeby — reaguje
      dopiero po odwiedzeniu naprawdę dużego katalogu/configu.

- [x] **Wyciek 9 kolorów X (`XAllocColor`) przy nieudanym `ui_init`** —
      naprawione: blok `XFreeColors` z `ui_destroy` wydzielony do nowej
      `free_theme_colors()` (statycznej, przed `ui_init`), wołanej teraz
      też w ścieżce błędu `ui_init` (gdy `ctx->font` zostaje `NULL` po
      `init_theme`) przed `free(ctx)`. `ui_destroy` używa tej samej
      funkcji — zero duplikacji.

### 🟢 Higiena kodu / build

- [x] **`-Wall -Wextra` nie przechodzi czysto** — wszystkie 19 ostrzeżeń
      `-Wformat-truncation` naprawione, `make clean && make` teraz zero
      warningów:
      - `7afm.c` (`OpenSelected`) — realnie osiągalne obcięcie
        `g_status[300]` przy zwykłych głębokich ścieżkach: `%.64s`/`%.200s`/
        `%.250s` z jawnym limitem zamiast gołego `%s`.
      - `7afm.c` (`JoinPath`) — główne źródło reszty ostrzeżeń (9 z 19,
        wołane z ~10 miejsc). Rozbite z jednego `snprintf("%s/%s", ...)`
        na dwa sekwencyjne `snprintf` z pojedynczym `%s` każdy (drugi
        pisze od `out + n` z przeliczonym `outsize - n`) — zachowanie
        identyczne (nadal bezpieczne ucięcie), ale gcc potrafi to
        udowodnić statycznie dla pojedynczego `%s`, czego nie potrafił
        dla dwóch w jednym formacie.
      - `7afm.c` (`HandlePasteReceived`) — `src` powiększony z `PATH_MAX`
        do `PATH_MAX + 8`, żeby pasował do `raw`, z którego jest kopiowany.
      - `7aweather.c` (`UpdateWeather`) — pośredni bufor `label[256]`
        usunięty, `g_line_text[0]` (64 bajty) budowany bezpośrednio z
        `%.42s` na `location_query`.
      - `7abubbles.c` (`scan_projects`) — `full[512]` → `full[800]`
        (zapas na `base`(512) + `/` + `d_name`(do 256)).
      - `7aclip.c` (draw historii) — `page_label[32]` → `page_label[64]`
        (worst-case trzech `%d` w formacie nie mieścił się w 32).

- [x] **`examples/7anotify.c` i `examples/7asys.c` poza `make all`** —
      naprawione: dołączone do builda (cele `7anotify`/`7asys` w
      `Makefile`, dopisane do `all`/`clean`, bez dodatkowych zależności
      poza `libX11`) i do tabeli apek w CLAUDE.md. Zdecydowano: dołączyć,
      nie usuwać — kompilują się czysto (`-Wall -Wextra`) i mają
      najszerszy `pledge`/`unveil` hardening w repo, szkoda by było je
      trzymać jako martwy kod.

- [x] **Nieaktualny komentarz w `x11-flags.sh`** — zaktualizowany: teraz
      opisuje `XFontStruct`/`XDrawString16` zamiast nieaktualnego
      `XFontSet`.
