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

---

## Synchronizacja z centralnym serwerem (`sync/`)

Nowy podprojekt Go w katalogu `sync/` — osobny `go.mod`, buduje się przez
`make` w `sync/`. Nie zmienia buildsystemu C ani żadnego pliku `examples/*.c`.

Stack: **Go**, auth: **API key** (`X-API-Key` w nagłówku), sync: **CLI
`7async`** (ręczne lub cron), Google Calendar: **jednorazowy import `.ics`**.

**Baza — DWIE różne, na dwóch różnych maszynach, nie jedna dzielona:**
- **Klient** (`7async`, oraz `7atodo.c`/`7acal.c`) — **SQLite**
  (`modernc.org/sqlite`, pure Go, zero CGo), lokalny plik `~/.7a/tasks.db`,
  bez zmian względem wcześniejszych ustaleń.
- **Serwer** (`7asyncd`) — **MariaDB** (ustalone w rozmowie — serwer
  docelowy, na OpenBSD, już ma MariaDB). Sterownik:
  `github.com/go-sql-driver/mysql` (pure Go, protokół MySQL, kompatybilny
  z MariaDB, nie łamie celu statycznego/bez-CGo binarnego). Konfiguracja
  przez `SYNC_DB_DSN` (format sterownika, np.
  `użytkownik:hasło@tcp(host:3306)/baza` albo przez gniazdo unix), bez
  wartości domyślnej — brak zmiennej to fatal błąd przy starcie.
  Tabela `items` po stronie serwera ma INNY układ niż po stronie klienta:
  `uuid` jest kluczem głównym (`VARCHAR(36) PRIMARY KEY`) zamiast osobnej
  nullable kolumny — serwer z definicji nigdy nie widzi rekordu bez `uuid`
  (klient generuje je przed pierwszym push) — i bez kolumny `id`
  (autoincrement lokalny do jednego urządzenia, bez znaczenia między
  urządzeniami). Reszta pól (`priority`/`due_date`/`due_time`/`body`/
  `created_at`/`updated_at`/`alarm`/`deleted`) identyczna z klientem —
  `alarm` też synchronizowane, bo to wciąż "ten sam element listy", nie
  osobne, per-urządzeniowe ustawienie.
  Zaimplementowane w `sync/internal/schema/mariadb.go` (`MigrateMariaDB`)
  — **przetestowane na żywym MariaDB 10.5.29** (lokalny serwer
  użytkownika, gniazdo unix, `--skip-networking`): `CREATE TABLE`/`CREATE
  INDEX` przechodzą, `SHOW CREATE TABLE` potwierdza dokładnie zamierzone
  typy (`uuid VARCHAR(36) PRIMARY KEY`, `created_at`/`updated_at
  BIGINT`, `due_time VARCHAR(5)` itd.), `PRIMARY KEY` na `uuid` faktycznie
  wymusza unikalność (`INSERT` duplikatu → `ERROR 1062 Duplicate entry`,
  zgodnie z oczekiwaniem pod przyszły upsert), ponowne uruchomienie
  `7asyncd` na już zmigrowanej bazie jest idempotentne (bez błędu na
  "table already exists"/"duplicate key"). Nadal niesprawdzone: zachowanie
  na docelowym OpenBSD (testowano na Linuksie) — ale to już różnica
  systemu operacyjnego klienta MariaDB, nie samego schematu/SQL.

### Schemat i migracja

- [x] Dodać do tabeli `items` cztery kolumny (migracja `ALTER TABLE ...
      ADD COLUMN`, ignorując oczekiwany błąd "duplicate column" — wzorzec
      już użyty dla `alarm` w `7atodo.c:291`; kolumny nullable/default, więc
      stare zapytania bez nich nadal działają):
      - `uuid TEXT` — globalny UUID generowany przy pierwszym push
      - `updated_at INTEGER` — Unix timestamp ostatniej modyfikacji (NULL = stare rekordy)
      - `deleted INTEGER DEFAULT 0` — soft delete zamiast fizycznego DELETE
      - `due_time TEXT` — godzina powiązana z `due_date` (`HH:MM`, 24h);
        `NULL` = zadanie/wydarzenie całodniowe albo bez konkretnej godziny

  Migrację uruchamiają przy starcie zarówno `7async` jak i `7asyncd`, w tym
  `CREATE TABLE IF NOT EXISTS items (...)` z pełnym schematem — na wypadek
  gdyby `7async`/`7asyncd` odpalono zanim ktokolwiek uruchomił `7atodo`/`7acal`.
  Zrobione: `sync/internal/schema/schema.go` (`Migrate(db *sql.DB) error`,
  bajt-w-bajt to samo `CREATE TABLE`/`ALTER TABLE ADD COLUMN` co w
  `7atodo.c`/`7acal.c`), plus `idx_items_uuid` (`UNIQUE ... WHERE uuid IS
  NOT NULL`) — jedyny indeks bez odpowiednika w C, potrzebny bo
  serwer/klient robią upsert po `uuid` na każdym push/pull, czego C-owe
  apki nigdy nie robią. Przetestowane na trzech stanach bazy w scratchpadzie
  (świeża, sprzed kolumny `alarm`, ponowne uruchomienie/idempotentność) —
  identyczny efekt końcowy co migracja w C, dane nienaruszone.

### Zmiany w `7atodo.c`/`7acal.c` (wymagane, żeby sync w ogóle miał sens)

Ustalone w rozmowie: apki dokłada się do listy modyfikowanych plików, mimo
że pierwotny plan zakładał "bez zmian" — bez tego `updated_at`/`deleted`
byłyby polami, których nic lokalnie nie ustawia, więc sync nie miałby jak
wykryć lokalnych edycji ani usunięć.

- [x] Każdy `INSERT INTO items`/`UPDATE items` w `7atodo.c` (dodanie zadania,
      edycja treści z zewnętrznego edytora, zmiana priorytetu, oba
      historyczne importy w `MigrateOldFiles`) ustawia `updated_at`
      (`time(NULL)` bindowany jak `created_at`, ten sam styl co reszta pliku).
- [x] Zapytania listujące w `7atodo.c` (`RunQuery`) i zakresowe w `7acal.c`
      (`RefreshEntries`) dokładają `AND deleted=0` — potwierdzone testem na
      osobnej testowej bazie w scratchpadzie, że soft-deleted wiersz znika
      z obu widoków.
- [x] `DELETE FROM items` w `7atodo.c` (`DeleteSelected`, `ImportBody` przy
      wyczyszczeniu pliku w edytorze) zamienione na
      `UPDATE items SET deleted=1, updated_at=...` (soft delete) — inaczej
      lokalne skasowanie zadania nigdy by się nie zsynchronizowało. Efekt
      uboczny: skasowane zadania nie są już fizycznie usuwane z lokalnej
      bazy — ewentualny okresowy "purge" starych `deleted=1` to świadomie
      osobny temat, poza zakresem tego TODO.
      Build czysty (`-Wall -Wextra`).

### Serwer `sync/cmd/7asyncd/main.go`

- [x] Struktura projektu: `sync/go.mod` (moduł `7adesktop/sync`, Go 1.25),
      `sync/Makefile` (`CGO_ENABLED=0`, cele `7asyncd`/`7async`/`all`/`clean`
      — zrealizowany też punkt z sekcji "Deploy" niżej), katalogi
      `cmd/7asyncd/` i `cmd/7async/`. Zależności: `modernc.org/sqlite`
      (klient, pure Go) i `github.com/go-sql-driver/mysql` (serwer, pure
      Go, do MariaDB — patrz sekcja "Baza" wyżej). Potwierdzone `file(1)`,
      że binarka klienta (`7async`) wychodzi statycznie linkowana mimo
      `CGO_ENABLED=0`; `7asyncd` też się buduje bez CGo (sam sterownik SQL
      nie wymaga go), ale to nieistotne dla samej bazy - MariaDB stoi w
      osobnym procesie, więc "statyczność" `7asyncd` dotyczy tylko braku
      zależności runtime samego Go/CGo, nie samej bazy.
- [x] Inicjalizacja bazy i migracja schematu przy starcie — `schema.go`
      rozdzielone na `sqlite.go` (`MigrateSQLite`, wołane przez `7async`) i
      `mariadb.go` (`MigrateMariaDB`, wołane przez `7asyncd`) — dwa różne
      schematy, patrz sekcja "Baza" wyżej. `main.go` obu binarek na razie
      tylko otwiera bazę i woła migrację (loguje i kończy), bez endpointów
      HTTP/podkomend — to kolejne punkty niżej. Zweryfikowane: `7asyncd`
      zgłasza czytelny fatal błąd przy braku `SYNC_DB_DSN` i przy
      nieosiągalnym serwerze MariaDB (`connection refused`), zamiast
      cichego zawieszenia.
- [x] Middleware auth: nagłówek `X-API-Key` weryfikowany względem ENV
      `SYNC_API_KEY` (`requireAPIKey` w `internal/httpapi/httpapi.go`);
      brak/zły klucz → 401. Bez wariantu "plik konfiguracyjny" — jedna
      zmienna środowiskowa wystarcza, brak (`SYNC_API_KEY=""`) to fatal
      błąd przy starcie, więc serwer nigdy nie wystartuje z wyłączonym auth.
- [x] `GET /api/health` — zwraca `{"ok":true}`, bez auth.
- [x] `GET /api/items?since=<unix_ts>` — zwraca items z `updated_at > since`
      (`since=0`/brak parametru = wszystkie, bo każdy prawdziwy rekord ma
      `updated_at > 0`); JSON array; wymaga auth. `since` niepoprawne
      (nie-liczba) → 400.
- [x] `POST /api/items/batch` — upsert listy items (po `uuid`) w
      `internal/store/item.go` (`BatchUpsert`/`upsertOne`, cała partia w
      jednej transakcji); aktualizuje tylko jeśli przysłany `updated_at` >
      tego w bazie (last-write-wins) — starszy/równy zapis cicho
      ignorowany; nowy `uuid` → `INSERT`. Body inne niż JSON array → 400.
- [x] Konfiguracja przez ENV: `SYNC_DB_DSN`, `SYNC_API_KEY` (oba bez
      wartości domyślnej, fatal błąd przy starcie gdy brak), `SYNC_ADDR`
      (domyślnie `:8080`).
- [x] Graceful shutdown (`os.Signal`) — `signal.NotifyContext` na
      `SIGINT`/`SIGTERM`, `srv.Shutdown()` z timeoutem 5s.

  **Przetestowane end-to-end na żywym MariaDB** (ten sam serwer co sekcja
  "Baza" wyżej): `/api/health` bez auth, `/api/items`/`/api/items/batch`
  z 401 przy braku/złym kluczu, insert przez batch, last-write-wins
  (starszy `updated_at` faktycznie zignorowany, nowszy faktycznie wszedł —
  sprawdzone na tym samym `uuid` w dwóch kolejnych batchach), filtr
  `since` (ścisłe `>`, nie `>=` — sprawdzone przy `since` równym
  dokładnie `updated_at` rekordu), `bool` (`alarm`/`deleted`) poprawnie
  wędruje przez JSON ↔ Go ↔ `TINYINT(1)` w obie strony, `kill` (SIGTERM)
  → poprawny graceful shutdown w logach. Nowe pliki: `internal/store/item.go`,
  `internal/httpapi/httpapi.go`.

### Klient CLI `sync/cmd/7async/main.go`

- [x] Czytanie `~/.7a/sync.conf` (format `klucz=wartość`, ignorować linie
      `#`): `server_url`, `api_key`, `db_path` (domyślnie `~/.7a/tasks.db`)
      — `internal/config/config.go` (`Load`). Brakujący plik NIE jest
      błędem (zwraca same domyślne wartości), żeby `7async status`
      działało przed pierwszą konfiguracją; `push`/`pull`/`sync`
      wymagają `server_url`/`api_key` i kończą się czytelnym błędem, gdy
      ich brak.
- [x] Śledzenie czasu ostatniej synchronizacji w `~/.7a/sync.conf` (pole
      `last_sync`, Unix timestamp); aktualizowane po udanym pull —
      `Config.SetLastSync` podmienia/dopisuje TYLKO tę jedną linię, reszta
      pliku (w tym komentarze użytkownika) zostaje nietknięta; zapis przez
      plik tymczasowy + `rename`. Potwierdzone testem: komentarz na
      początku pliku przetrwał zapis.
- [x] `7async push` — generuje brakujące UUID dla lokalnych items
      (`localdb.GenerateMissingUUIDs`, `github.com/google/uuid`), wysyła
      items z `updated_at > last_sync` przez `POST /api/items/batch`
      (`localdb.ItemsForPush` + `syncclient.PushBatch`).
- [x] `7async pull` — pobiera `GET /api/items?since=<last_sync>`, upsert
      do lokalnego SQLite po `uuid`, last-write-wins
      (`localdb.ApplyPulled`/`applyOne` — ten sam wzorzec co
      `store.BatchUpsert` po stronie serwera, tylko dopasowujący istniejący
      lokalny `id` zamiast robić `INSERT ... ON DUPLICATE`).
- [x] `7async sync` — push, potem pull (typowe użycie).
- [x] `7async status` — wypisuje liczbę lokalnych items (bez usuniętych),
      ścieżkę configu, URL serwera (albo informację o braku konfiguracji),
      `last_sync` (czytelny format + unix timestamp, albo "nigdy").
- [x] `7async import-ics <plik.ics>` — na razie placeholder (komunikat +
      `exit 1`), realny parser to osobny punkt niżej.

  **Przetestowane end-to-end** (lokalna testowa baza SQLite + żywy
  `7asyncd` na tym samym MariaDB co wcześniej): `push` nadaje `uuid` i
  wysyła obie pozycje; `pull` odbiera wszystko przy pierwszym starcie,
  potem TYLKO realnie zmieniony rekord (filtr `since` faktycznie działa);
  symulacja edycji "z innego urządzenia" (bezpośredni `POST` na serwer z
  nowszym `updated_at`) poprawnie nadpisuje lokalny wiersz PO `uuid`
  (dopasowany istniejący lokalny `id`, nie tworzy duplikatu); `sync`
  (push+pull) działa łącznie po lokalnej edycji; ponowne wysłanie
  niezmienionego (już zsynchronizowanego) rekordu jest nieszkodliwe —
  serwerowe last-write-wins po równym `updated_at` nic nie nadpisuje;
  `status` przed i po synchronizacji pokazuje poprawne dane; `import-ics`
  bez implementacji i brak argumentów obydwa kończą się czytelnym
  komunikatem i `exit 1`. Nowe pliki: `internal/config/config.go`,
  `internal/localdb/localdb.go`, `internal/syncclient/syncclient.go`.

### Importer ICS (`import-ics`)

- [x] Parser VCALENDAR/VEVENT bez zewnętrznych bibliotek —
      `sync/internal/ics/ics.go` (`ParseEvents`):
      - Pola: `UID` → `uuid`, `SUMMARY` → `body`, `DTSTART` → `due_date` +
        `due_time`:
        - `DTSTART;VALUE=DATE:YYYYMMDD` (wydarzenie całodniowe, brak
          człona `T`) → tylko `due_date`, `due_time` zostaje `NULL`
        - `DTSTART:YYYYMMDDTHHmmssZ` → `due_date` z części daty,
          `due_time` (`HH:MM`) z części godziny
      - `DESCRIPTION` dołączany do `body` jeśli niepusty (oddzielony `\n---\n`)
      - `STATUS:COMPLETED` → `deleted=1` (ukryty w 7atodo dzięki filtrowi
        `deleted=0` z sekcji wyżej, nie usunięty)
      - Obsługa line-folding (RFC 5545: kontynuacja znakiem spacji na
        początku linii) i odwrócenie text-escapingu (`\,`/`\;`/`\\`/`\n`)
      - Rozróżnienie wydarzenia całodniowego po samej obecności `T` w
        wartości `DTSTART`, nie po parametrze `VALUE=DATE` — działa
        identycznie z plikami, które ten parametr pomijają.
      - Świadomie POMINIĘTE w v1 (decyzja ze scope, nie przeoczenie):
        `LOCATION` (ustalone: brak sensu dla tej apki, nie dokładamy),
        `RRULE` (wydarzenia cykliczne — bez ekspansji powtórzeń, bierzemy
        tylko pojedynczy `DTSTART` z eventu) i `DTEND` (todo ma deadline,
        nie zakres czasu)
- [x] Deduplikacja po `uuid` (`localdb.ImportICSItem`) — przy powtórnym
      imporcie aktualizuje zamiast duplikować. Decyzja podjęta przy
      implementacji: `UPDATE` świadomie NIE dotyka `priority` ani
      `created_at` — to lokalne pola, których kalendarz nie zna, więc
      ponowny import nie nadpisuje tego, co użytkownik już ustawił w
      `7atodo` (potwierdzone testem: priorytet ustawiony ręcznie na `1`
      przetrwał kolejny import tego samego pliku).
- [x] Flaga `--dry-run`: wypisuje co by zaimportował (`[insert]`/`[update]`
      per event, rozróżnione przez sprawdzenie istniejącego `uuid`), nie
      pisze do bazy.
- [x] Flaga `--no-description`: ignoruje `DESCRIPTION` (tylko `SUMMARY`).

  **Przetestowane** na ręcznie skonstruowanym pliku `.ics` (CRLF, 3
  VEVENT: zdarzenie z godziną, całodniowe, ukończone z `STATUS:COMPLETED`,
  plus line-folding i znaki wymagające unescapingu w `SUMMARY`/
  `DESCRIPTION`): `--dry-run` poprawnie rozpoznaje `insert` vs `update`,
  realny import wstawia 3 nowe rekordy z poprawnymi `due_date`/`due_time`/
  `deleted`, ponowny import tego samego pliku aktualizuje (nie duplikuje —
  nadal 3 wiersze, nie 6) i zachowuje ręcznie zmieniony `priority`,
  `--no-description` faktycznie pomija `DESCRIPTION`, brakujący plik i
  brak argumentu kończą się czytelnym błędem (`exit 1`).

### Deploy i dokumentacja

- [x] `sync/Makefile`: cele `7async`, `7asyncd`, `all`, `clean`; flaga
      `CGO_ENABLED=0` dla statycznego binaru — zrobione wcześniej, patrz
      sekcja "Struktura projektu" wyżej
- [x] **Serwer docelowy to OpenBSD** — Go NIE musi być zainstalowane na
      serwerze, kompilacja krzyżowa z maszyny deweloperskiej:
      `GOOS=openbsd GOARCH=amd64 make` w `sync/` (Makefile już to
      obsługuje bez zmian, `go build` czyta `GOOS`/`GOARCH` ze
      środowiska). Zweryfikowane praktycznie (`file(1)` na wynikowym
      `7asyncd`): binarka dla OpenBSD wychodzi "dynamically linked,
      interpreter /usr/libexec/ld.so" — to NIE jest zależność od Go ani
      od `libsqlite3`, tylko wymóg samego OpenBSD (syscall origin
      verification od 6.4: nawet statyczny kod Go musi wołać syscalle
      przez `libc.so` z bazowej instalacji) — więc nadal wystarczy
      skopiować sam plik binarny, zero dodatkowych paczek po stronie
      OpenBSD.
- [x] `sync/sync.conf.sample`: przykładowy plik konfiguracyjny z
      komentarzami, ten sam styl co `center.conf.sample` (nagłówek z
      instrukcją kopiowania do `~/.7a/sync.conf`, `#` = komentarz/wyłączone
      pole). Dokumentuje `server_url`/`api_key`/`db_path`/`last_sync`,
      z jasnym zaznaczeniem, że `last_sync` jest zarządzane automatycznie
      przez `7async pull` i nie powinno być ustawiane ręcznie.
- [x] `sync/rc.d.7asyncd`: skrypt rc.d dla OpenBSD (`rc.subr(8)`) —
      `rc_start` przekazuje `SYNC_DB_DSN`/`SYNC_API_KEY`/`SYNC_ADDR` do
      środowiska demona przez `env` w `rc_exec` (funkcja, nie zmienna
      `$rcexec` — ta ostatnia usunięta z `rc.subr` w 2022, rev 1.160),
      `daemon_user` pod dedykowanego, nieuprzywilejowanego użytkownika,
      `rc_bg=YES` (demon nie forkuje się sam).
      **Poprawiona błędna wersja robocza**: pierwotny plan (te zmienne
      jako zwykłe klucze w `/etc/rc.conf.local`) był zły — sprawdziłem
      realne źródło `rc.subr` (cvsweb.openbsd.org, rev 1.167) i
      `_rc_parse_conf` importuje z `rc.conf.local` WYŁĄCZNIE klucze
      kończące się na `_flags`/`_user`/`_execdir`/`_logger`/`_rtable`/
      `_timeout` (plus wąska globalna whitelist bez związku z tym
      demonem) — dowolna własna zmienna jak `sync_db_dsn` byłaby po
      prostu cicho pomijana, więc `7asyncd` startowałby bez żadnej z tych
      wartości i od razu kończył się błędem "brak SYNC_DB_DSN". Naprawione:
      osobny plik `/etc/7asyncd.env` (chmod 600, root), czytany wprost
      przez sam skrypt (`.`/source) z pominięciem mechanizmu
      `rc.conf.local` — z ostrzeżeniem w komentarzu, żeby hasło/klucz
      unikały znaków specjalnych powłoki (`"`, `` ` ``, `$`, `\` — bo
      trafiają do stringa budowanego dla `su -c`), czego generator w
      rodzaju `openssl rand -base64` naturalnie nie produkuje.
      **Przetestowane na żywym OpenBSD (produkcyjny serwer użytkownika)**:
      `rcctl start`/`check`/`restart`/`stop` działają poprawnie; proces
      widoczny w `ps` jako `_7asyncd` (nie `root`), z linią poleceń
      dokładnie `/usr/local/bin/7asyncd` (bez śladu `env`/`su`) —
      potwierdza to, że `pgrep -xf "${pexp}"` w `rc_check` faktycznie
      dopasowuje proces, zgodnie z przewidywaniem z czytania źródła
      `rc.subr`; `/api/health` i `/api/items` (z `X-API-Key`) odpowiadają
      poprawnie przez usługę zarządzaną przez `rcctl`.
- [x] Zaktualizowano `CLAUDE.md` o sekcję "Podprojekt `sync/`" — mapa
      warstw kodu (`internal/store`/`httpapi`/`localdb`/`syncclient`/
      `config`/`ics`), przypomnienie o dwóch różnych schematach
      (SQLite klient / MariaDB serwer) i status testów, żeby przyszła
      sesja nie musiała odtwarzać tego z historii `TODO.md`. (Plik
      `CLAUDE.md` jest lokalny/gitignored w tym repo — ta zmiana nie
      trafi do commitów.)

---

## Usunięte

- **`examples/7afm.c` (menedżer plików) usunięty na życzenie użytkownika**
      (2026-09-04) — pomiar pod Xvfb wykazał, że to zdecydowanie najcięższa
      apka w repo: ~22MB PSS (unikalna pamięć procesu, nie licząc
      bibliotek dzielonych z innymi procesami) w porównaniu do ~1.2MB PSS
      dla reszty apek (demo/7atimer/7amessage/7aclip/7asensors);
      dochodzenie wskazywało na duży prywatny heap (~21MB) uruchamiany
      przy starcie, prawdopodobnie `magic_load()` z libmagic budujące
      bazę typów MIME w pamięci procesu — nie doprowadzone do końca,
      bo użytkownik zdecydował się po prostu usunąć apkę zamiast szukać
      optymalizacji (nigdy jej nie używa). Usunięto: `examples/7afm.c`,
      cel `7afm` + `MAGIC_CFLAGS`/`MAGIC_LIBS` z `Makefile`, wpis w
      `center.conf.sample`, wiersz w tabeli apek + wzmianki jako "wzorzec"
      w CLAUDE.md, wzmianka w README.md. Przy okazji: `ui_menu_item()`
      w `ui.h`/`ui.c` usunięty jako martwy kod — był używany WYŁĄCZNIE
      przez pasek menu File/Edit/View w `7afm.c`, zero innych callerów
      w `examples/`. Komentarze "wzorem 7afm.c" w innych plikach (patrz
      `git log`/`git grep 7afm` dla historii) świadomie NIE wyczyszczone
      wszędzie — tylko tam gdzie odwołanie było user-facing (dokumentacja,
      configi) albo w publicznym API `ui.h`; reszta to historyczne
      adnotacje pochodzenia wzorca w komentarzach wewnątrz innych apek,
      nieszkodliwe mimo że plik już nie istnieje. `make clean && make`
      czysty (`-Wall -Wextra`, 14 apek zamiast 15).
