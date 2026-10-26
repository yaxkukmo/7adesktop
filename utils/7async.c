/*
 * 7async - klient synchronizacji ~/.7a/organizer.db (7atodo/7acal/
 * 7aorganizer-tui) z serwerem, w C. Zastepuje sync/cmd/7async (Go) w
 * czesci push/pull; import-ics zostaje na razie w wersji Go (TODO.md).
 *
 * Jedna sesja = `ssh <server> 7async-serve`, rozmowa przez potoki
 * (protokol w 7async-proto.h): klient wysyla lokalne zmiany od ostatniego
 * push i swoj kursor, serwer zapisuje je i odsyla wszystko od kursora.
 * Uwierzytelnianie i szyfrowanie robi ssh - tu nie ma kodu sieciowego.
 *
 * Mapowanie wierszy na rekordy (to samo co sync/internal/localdb w Go):
 * data to kolumny wiersza bez id/uuid/updated_at, w stalej kolejnosci
 * (KINDS nizej), wpis w kalendarzu wskazuje zadanie po uuid (todo_uuid),
 * bo lokalne id sa na kazdej maszynie inne. Skasowane wiersze bierzemy z
 * deleted_items (triggery organizera). Pobrane rekordy zapisujemy
 * last-write-wins po updated_at; skasowanie tez ma swoj czas i wygrywa ze
 * starsza zmiana. Jawnie ustawiony updated_at nie jest nadpisywany przez
 * triggery *_touch.
 *
 * Config: ~/.7a/sync.conf (klucz=wartosc, patrz sync.conf.sample).
 * Punkty synchronizacji pushed_at/cursor to NOWE klucze - dawne
 * records_pushed_at/records_cursor (7asyncd) sa ignorowane, wiec pierwsza
 * sesja z nowym serwerem wysyla i pobiera wszystko.
 */

#define _DEFAULT_SOURCE  /* fork/execlp/fdopen/kill to POSIX - bez tego glibc z -std=c99 ich nie deklaruje */

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "7async-proto.h"

/* Ta sama stala co ORGANIZER_SCHEMA w 7atodo.c/7acal.c. */
#define ORGANIZER_SCHEMA 3
#define CONF_MAX         (16 * 1024)
#define EPOCH(col)       "CAST(strftime('%s', " col ") AS INTEGER)"

typedef struct {
    char      path[PATH_MAX];
    char      server[256];        /* cel ssh, np. sync@host */
    char      serve_cmd[256];
    char      db_path[PATH_MAX];
    long long pushed_at;          /* unix wg zegara TEGO klienta */
    long long cursor;             /* licznik changed_at serwera */
} Config;

/*
 * Rodzaj rekordu. Pola data = kolumny 2.. zapytania select (po uuid i
 * updated_at); select: ?1 = od kiedy, ?2 = jeden uuid albo NULL (wszystkie).
 * W update/insert pola to ?1..?nfields, updated_at to
 * ?(nfields+1), a ?(nfields+2) to id (update) albo uuid (insert).
 */
typedef struct {
    const char *kind;
    const char *table;
    int         nfields;
    const char *select;
    const char *update;
    const char *insert;
} Kind;

#define TODO_ID "(SELECT id FROM todos WHERE uuid = ?10)"

static const Kind KINDS[] = {
    { "todo", "todos", 6,
      "SELECT uuid, " EPOCH("updated_at") ", title, description, priority,"
      " status, done_at, created_at FROM todos"
      " WHERE uuid IS NOT NULL AND " EPOCH("updated_at") " >= ?1"
      " AND (?2 IS NULL OR uuid = ?2)",
      "UPDATE todos SET title = ?1, description = ?2, priority = ?3,"
      " status = ?4, done_at = ?5, updated_at = datetime(?7, 'unixepoch')"
      " WHERE id = ?8",
      "INSERT INTO todos (uuid, title, description, priority, status,"
      " done_at, created_at, updated_at) VALUES (?8, ?1, ?2, ?3, ?4, ?5,"
      " COALESCE(?6, datetime('now')), datetime(?7, 'unixepoch'))" },
    { "entry", "calendar_entries", 11,
      "SELECT e.uuid, " EPOCH("e.updated_at") ", e.title, e.description,"
      " e.entry_date, e.entry_time, e.duration_min, e.recurrence_type,"
      " e.recurrence_weekday, e.recurrence_day, e.recurrence_month, t.uuid,"
      " e.created_at FROM calendar_entries e"
      " LEFT JOIN todos t ON t.id = e.todo_id"
      " WHERE e.uuid IS NOT NULL AND " EPOCH("e.updated_at") " >= ?1"
      " AND (?2 IS NULL OR e.uuid = ?2)",
      "UPDATE calendar_entries SET title = ?1, description = ?2,"
      " entry_date = ?3, entry_time = ?4, duration_min = ?5,"
      " recurrence_type = ?6, recurrence_weekday = ?7, recurrence_day = ?8,"
      " recurrence_month = ?9, todo_id = " TODO_ID ","
      " updated_at = datetime(?12, 'unixepoch') WHERE id = ?13",
      "INSERT INTO calendar_entries (uuid, title, description, entry_date,"
      " entry_time, duration_min, recurrence_type, recurrence_weekday,"
      " recurrence_day, recurrence_month, todo_id, created_at, updated_at)"
      " VALUES (?13, ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, " TODO_ID ","
      " COALESCE(?11, datetime('now')), datetime(?12, 'unixepoch'))" },
};

static sqlite3 *g_db;
static char     g_data[PROTO_DATA_MAX];
static char     g_local[PROTO_DATA_MAX];   /* lokalny wiersz do porownania */

static void
Die(const char *msg)
{
    fprintf(stderr, "7async: %s\n", msg);
    exit(1);
}

static void
DieDb(const char *what)
{
    fprintf(stderr, "7async: %s: %s\n", what, sqlite3_errmsg(g_db));
    exit(1);   /* niezacommitowana transakcja wycofuje sie przy exit */
}

static const Kind *
KindOf(const char *kind)
{
    size_t i;

    for (i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++)
        if (strcmp(KINDS[i].kind, kind) == 0)
            return &KINDS[i];
    return NULL;
}

/* ---- config ---- */

/* Przycina line w miejscu; 0 i key/val dla "klucz=wartosc", -1 inaczej. */
static int
ParseConfLine(char *line, char **key, char **val)
{
    char *eq, *end;

    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '#' || (eq = strchr(line, '=')) == NULL)
        return -1;
    *eq = '\0';
    for (end = eq; end > line && (end[-1] == ' ' || end[-1] == '\t'); end--)
        end[-1] = '\0';
    *key = line;
    for (*val = eq + 1; **val == ' ' || **val == '\t'; (*val)++)
        ;
    end = *val + strlen(*val);
    while (end > *val && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return 0;
}

static void
SetString(char *dst, size_t size, const char *val, const char *key)
{
    if ((size_t)snprintf(dst, size, "%s", val) >= size) {
        fprintf(stderr, "7async: sync.conf: %s too long\n", key);
        exit(1);
    }
}

static void
LoadConfig(Config *c)
{
    const char *home = getenv("HOME");
    char line[1024], *key, *val;
    long long v;
    FILE *f;

    if (home == NULL || *home == '\0')
        Die("HOME not set");
    memset(c, 0, sizeof *c);
    if (strlen(home) + sizeof "/.7a/organizer.db" > sizeof c->path)
        Die("HOME too long");
    snprintf(c->path, sizeof c->path, "%s/.7a/sync.conf", home);
    snprintf(c->db_path, sizeof c->db_path, "%s/.7a/organizer.db", home);
    snprintf(c->serve_cmd, sizeof c->serve_cmd, "7async-serve");

    if ((f = fopen(c->path, "r")) == NULL) {
        if (errno == ENOENT)
            return;   /* brak configu: dziala tylko "status" */
        Die("cannot read sync.conf");
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (ParseConfLine(line, &key, &val) != 0)
            continue;
        if (strcmp(key, "server") == 0)
            SetString(c->server, sizeof c->server, val, key);
        else if (strcmp(key, "serve_cmd") == 0)
            SetString(c->serve_cmd, sizeof c->serve_cmd, val, key);
        else if (strcmp(key, "db_path") == 0)
            SetString(c->db_path, sizeof c->db_path, val, key);
        else if (strcmp(key, "pushed_at") == 0 &&
                 ProtoParseInt(val, 0, LLONG_MAX, &v) == 0)
            c->pushed_at = v;
        else if (strcmp(key, "cursor") == 0 &&
                 ProtoParseInt(val, 0, LLONG_MAX, &v) == 0)
            c->cursor = v;
    }
    fclose(f);
}

/* Czy linia configu ustawia key (bez modyfikacji line). */
static int
LineHasKey(const char *line, const char *want)
{
    char copy[1024], *key, *val;

    snprintf(copy, sizeof copy, "%s", line);
    return ParseConfLine(copy, &key, &val) == 0 && strcmp(key, want) == 0;
}

/*
 * Zapisuje pushed_at/cursor w sync.conf, reszte linii (w tym komentarze)
 * zostawia bez zmian. Przez plik tymczasowy + rename - awaria w trakcie
 * nie zostawi pol-zapisanego configu.
 */
static void
SaveConfig(const Config *c)
{
    static char buf[CONF_MAX];
    char tmp[PATH_MAX + 16], *line, *next;
    int fd, have_p = 0, have_c = 0;
    size_t len = 0;
    FILE *in, *out;

    if ((in = fopen(c->path, "r")) != NULL) {
        len = fread(buf, 1, sizeof buf - 1, in);
        if (!feof(in))
            Die("sync.conf too large");
        fclose(in);
    }
    buf[len] = '\0';

    snprintf(tmp, sizeof tmp, "%s.XXXXXX", c->path);
    if ((fd = mkstemp(tmp)) == -1 || (out = fdopen(fd, "w")) == NULL)
        Die("cannot write sync.conf");
    for (line = buf; *line; line = next) {
        if ((next = strchr(line, '\n')) != NULL)
            *next++ = '\0';
        else
            next = line + strlen(line);
        if (LineHasKey(line, "pushed_at") && !have_p++)
            fprintf(out, "pushed_at=%lld\n", c->pushed_at);
        else if (LineHasKey(line, "cursor") && !have_c++)
            fprintf(out, "cursor=%lld\n", c->cursor);
        else if (!LineHasKey(line, "pushed_at") && !LineHasKey(line, "cursor"))
            fprintf(out, "%s\n", line);
    }
    if (!have_p)
        fprintf(out, "pushed_at=%lld\n", c->pushed_at);
    if (!have_c)
        fprintf(out, "cursor=%lld\n", c->cursor);
    if (fflush(out) != 0 || ferror(out) || fsync(fd) != 0) {
        fclose(out);
        unlink(tmp);
        Die("cannot write sync.conf");
    }
    fclose(out);
    if (rename(tmp, c->path) != 0) {
        unlink(tmp);
        Die("cannot replace sync.conf");
    }
}

/* ---- lokalna baza ---- */

static long long
QueryInt(const char *sql)
{
    sqlite3_stmt *st;
    long long v;

    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_ROW)
        DieDb(sql);
    v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/*
 * Schematem zarzadza WYLACZNIE 7aorganizer-tui - brak bazy to blad, nie
 * nowa pusta baza. foreign_keys: skasowanie zadania zeruje todo_id
 * wpisow, tak jak w organizerze.
 */
static void
OpenDatabase(const char *path)
{
    if (sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        DieDb(path);
    sqlite3_busy_timeout(g_db, 5000);
    if (sqlite3_exec(g_db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL)
        != SQLITE_OK)
        DieDb("foreign_keys");
    if (QueryInt("PRAGMA user_version") < ORGANIZER_SCHEMA) {
        fprintf(stderr, "7async: %s: schema older than %d - run a current"
                " 7aorganizer-tui once to create or upgrade it\n",
                path, ORGANIZER_SCHEMA);
        exit(1);
    }
}

static sqlite3_stmt *
Prepare(const char *sql)
{
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        DieDb(sql);
    return st;
}

static void
Exec(const char *sql)
{
    if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK)
        DieDb(sql);
}

/* Koduje kolumny 2..nfields+1 wiersza st jako pola data; -1 gdy za duze. */
static int
EncodeFields(sqlite3_stmt *st, int nfields, char *buf, size_t size,
             size_t *used)
{
    int i;

    *used = 0;
    for (i = 0; i < nfields; i++) {
        int col = 2 + i;
        int null = sqlite3_column_type(st, col) == SQLITE_NULL;
        const char *p = null ? NULL : (const char *)sqlite3_column_text(st, col);
        size_t n = null ? 0 : (size_t)sqlite3_column_bytes(st, col);

        if (ProtoAppendField(buf, used, size, p, n, null) != 0)
            return -1;
    }
    return 0;
}

/* Wysyla jeden rekord z wiersza st (uuid, updated_at, pola od kolumny 2). */
static void
SendRow(FILE *out, sqlite3_stmt *st, const char *kind, int nfields,
        int deleted, long long *sent)
{
    ProtoRecord r;
    size_t used = 0;

    if (sqlite3_column_type(st, 0) == SQLITE_NULL ||
        (size_t)sqlite3_column_bytes(st, 0) > PROTO_UUID_MAX)
        Die("local row with invalid uuid");
    snprintf(r.uuid, sizeof r.uuid, "%s",
             (const char *)sqlite3_column_text(st, 0));
    if (!ProtoValidUuid(r.uuid)) {
        fprintf(stderr, "7async: invalid local uuid: %s\n", r.uuid);
        exit(1);
    }
    snprintf(r.kind, sizeof r.kind, "%s", kind);
    r.updated_at = sqlite3_column_int64(st, 1);
    r.deleted = deleted;
    if (r.updated_at <= 0)
        return;   /* bez czasu zmiany serwer by odrzucil */

    if (EncodeFields(st, nfields, g_data, sizeof g_data, &used) != 0) {
        fprintf(stderr, "7async: %s %s too large to sync\n", kind, r.uuid);
        exit(1);
    }
    r.len = used;
    if (ProtoWriteRecord(out, &r, g_data) != 0)
        Die("write to ssh failed");
    (*sent)++;
}

/*
 * Lokalne zmiany od since (wlacznie - zmiana w tej samej sekundzie co
 * poprzedni push nie moze przepasc, ponowne wyslanie jest nieszkodliwe).
 */
static long long
SendChanges(FILE *out, long long since)
{
    sqlite3_stmt *st;
    long long sent = 0;
    size_t k;

    for (k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        st = Prepare(KINDS[k].select);
        sqlite3_bind_int64(st, 1, since);
        while (sqlite3_step(st) == SQLITE_ROW)
            SendRow(out, st, KINDS[k].kind, KINDS[k].nfields, 0, &sent);
        sqlite3_finalize(st);
    }

    st = Prepare("SELECT uuid, " EPOCH("deleted_at") ", kind"
                 " FROM deleted_items WHERE " EPOCH("deleted_at") " >= ?1");
    sqlite3_bind_int64(st, 1, since);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *kind = (const char *)sqlite3_column_text(st, 2);

        if (kind != NULL && KindOf(kind) != NULL)
            SendRow(out, st, kind, 0, 1, &sent);
    }
    sqlite3_finalize(st);
    return sent;
}

/* Lokalny stan wiersza o danym uuid: id i updated_at (0 gdy brak). */
static int
LocalRow(const Kind *k, const char *uuid, long long *id, long long *at)
{
    char sql[160];
    sqlite3_stmt *st;
    int found;

    /* EPOCH zawiera '%s' strftime - jako argument, nie w formacie */
    snprintf(sql, sizeof sql, "SELECT id, %s FROM %s WHERE uuid = ?1",
             EPOCH("updated_at"), k->table);
    st = Prepare(sql);
    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
    found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        *id = sqlite3_column_int64(st, 0);
        *at = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return found;
}

/* Czas lokalnego skasowania uuid albo -1, gdy nie byl kasowany. */
static long long
LocalGoneAt(const char *uuid)
{
    sqlite3_stmt *st = Prepare("SELECT " EPOCH("deleted_at")
                               " FROM deleted_items WHERE uuid = ?1");
    long long at = -1;

    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        at = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return at;
}

static void
StepDone(sqlite3_stmt *st, const char *uuid)
{
    if (sqlite3_step(st) != SQLITE_DONE) {
        fprintf(stderr, "7async: applying %s: %s\n", uuid,
                sqlite3_errmsg(g_db));
        exit(1);
    }
    sqlite3_finalize(st);
}

static void
ApplyDeleted(const Kind *k, const ProtoRecord *r, int exists, long long id)
{
    sqlite3_stmt *st;
    char sql[64];

    st = Prepare("INSERT INTO deleted_items (uuid, kind, deleted_at)"
                 " VALUES (?1, ?2, datetime(?3, 'unixepoch'))"
                 " ON CONFLICT(uuid) DO UPDATE SET deleted_at = excluded.deleted_at"
                 " WHERE excluded.deleted_at > deleted_items.deleted_at");
    sqlite3_bind_text(st, 1, r->uuid, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, r->kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, r->updated_at);
    StepDone(st, r->uuid);
    if (!exists)
        return;
    /* trigger *_gone robi INSERT OR IGNORE, wiec zostaje czas z serwera */
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE id = ?1", k->table);
    st = Prepare(sql);
    sqlite3_bind_int64(st, 1, id);
    StepDone(st, r->uuid);
}

static void
ApplyRow(const Kind *k, const ProtoRecord *r, int exists, long long id)
{
    ProtoField f[PROTO_FIELDS_MAX];
    sqlite3_stmt *st;
    int nf, i;

    nf = ProtoParseFields(g_data, r->len, f, PROTO_FIELDS_MAX);
    if (nf < 0) {
        fprintf(stderr, "7async: bad data from server for %s\n", r->uuid);
        exit(1);
    }
    st = Prepare(exists ? k->update : k->insert);
    for (i = 0; i < k->nfields; i++) {
        if (i >= nf || f[i].null)
            sqlite3_bind_null(st, i + 1);
        else
            sqlite3_bind_text(st, i + 1, f[i].p, (int)f[i].len, SQLITE_STATIC);
    }
    sqlite3_bind_int64(st, k->nfields + 1, r->updated_at);
    if (exists)
        sqlite3_bind_int64(st, k->nfields + 2, id);
    else
        sqlite3_bind_text(st, k->nfields + 2, r->uuid, -1, SQLITE_STATIC);
    StepDone(st, r->uuid);
}

/* Czy lokalny wiersz uuid ma dokladnie te same pola co rekord w g_data. */
static int
LocalSame(const Kind *k, const ProtoRecord *r)
{
    sqlite3_stmt *st = Prepare(k->select);
    size_t used = 0;
    int same = 0;

    sqlite3_bind_int64(st, 1, 0);
    sqlite3_bind_text(st, 2, r->uuid, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW &&
        EncodeFields(st, k->nfields, g_local, sizeof g_local, &used) == 0)
        same = used == r->len && memcmp(g_local, g_data, used) == 0;
    sqlite3_finalize(st);
    return same;
}

/*
 * Przy remisie cofa lokalne updated_at o sekunde, zeby nastepny UPDATE
 * (z updated_at serwera) zmienil te kolumne i nie odpalil *_touch.
 */
static void
TouchBack(const Kind *k, long long id, const ProtoRecord *r)
{
    sqlite3_stmt *st;
    char sql[128];

    snprintf(sql, sizeof sql, "UPDATE %s SET updated_at ="
             " datetime(?1 - 1, 'unixepoch') WHERE id = ?2", k->table);
    st = Prepare(sql);
    sqlite3_bind_int64(st, 1, r->updated_at);
    sqlite3_bind_int64(st, 2, id);
    StepDone(st, r->uuid);
}

/*
 * Last-write-wins zapis pobranego rekordu (dane w g_data). Remis
 * updated_at (dwie zmiany w tej samej sekundzie) rozstrzyga serwer: trzyma
 * wersje, ktora dotarla pierwsza, a klient ja przyjmuje - inaczej kazda
 * strona zostalaby przy swojej na zawsze. Identycznej wersji (np. echo
 * wlasnego push) nie zapisujemy ponownie: UPDATE z tym samym updated_at
 * odpalilby trigger *_touch i podbil czas na "teraz".
 */
static void
ApplyPulled(const ProtoRecord *r)
{
    const Kind *k = KindOf(r->kind);
    long long id = 0, local_at = 0, gone_at;
    int exists;
    sqlite3_stmt *st;

    exists = LocalRow(k, r->uuid, &id, &local_at);
    if (exists && r->updated_at < local_at)
        return;   /* lokalna wersja nowsza */
    if (r->deleted) {
        ApplyDeleted(k, r, exists, id);
        return;
    }
    if (exists && r->updated_at == local_at) {
        if (LocalSame(k, r))
            return;
        TouchBack(k, id, r);
    }
    gone_at = LocalGoneAt(r->uuid);
    if (gone_at > r->updated_at)
        return;   /* skasowane lokalnie pozniej niz ta zmiana */
    ApplyRow(k, r, exists, id);
    if (gone_at >= 0) {
        st = Prepare("DELETE FROM deleted_items WHERE uuid = ?1");
        sqlite3_bind_text(st, 1, r->uuid, -1, SQLITE_STATIC);
        StepDone(st, r->uuid);
    }
}

/* ---- sesja ssh ---- */

static pid_t
SpawnSsh(const Config *c, FILE **to, FILE **from)
{
    int in[2], out[2];
    pid_t pid;

    if (pipe(in) == -1 || pipe(out) == -1)
        Die("pipe failed");
    if ((pid = fork()) == -1)
        Die("fork failed");
    if (pid == 0) {
        if (dup2(in[0], 0) == -1 || dup2(out[1], 1) == -1)
            _exit(127);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        /* "--": cel z configu nigdy nie zostanie wziety za opcje ssh */
        execlp("ssh", "ssh", "-T", "--", c->server, c->serve_cmd, (char *)NULL);
        fprintf(stderr, "7async: cannot run ssh\n");
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if ((*to = fdopen(in[1], "w")) == NULL ||
        (*from = fdopen(out[0], "r")) == NULL)
        Die("fdopen failed");
    return pid;
}

/* Blad z serwera ("X ...") albo urwany/niezgodny strumien. */
static void
ReadLineOrDie(FILE *in, char *line)
{
    if (ProtoReadLine(in, line, PROTO_LINE_MAX) != 0)
        Die("connection closed or bad response from server");
    if (line[0] == 'X' && line[1] == ' ') {
        fprintf(stderr, "7async: server: %s\n", line + 2);
        exit(1);
    }
}

/* Czyta odpowiedz serwera i zapisuje pobrane rekordy w jednej transakcji. */
static void
ReceivePull(FILE *in, long long *pushed, long long *received,
            long long *cursor)
{
    char line[PROTO_LINE_MAX];
    ProtoRecord r;

    ReadLineOrDie(in, line);
    if (strcmp(line, PROTO_HELLO) != 0)
        Die("server speaks an unsupported protocol");
    ReadLineOrDie(in, line);
    if (strncmp(line, "P ", 2) != 0 ||
        ProtoParseInt(line + 2, 0, LLONG_MAX, pushed) != 0)
        Die("bad push confirmation from server");

    Exec("BEGIN IMMEDIATE");
    *received = 0;
    for (;;) {
        ReadLineOrDie(in, line);
        if (line[0] != 'R')
            break;
        if (ProtoParseRecord(line, &r) != 0 ||
            ProtoReadData(in, &r, g_data) != 0)
            Die("bad record from server");
        ApplyPulled(&r);
        (*received)++;
    }
    if (strncmp(line, "C ", 2) != 0 ||
        ProtoParseInt(line + 2, 0, LLONG_MAX, cursor) != 0)
        Die("bad cursor from server");
    ReadLineOrDie(in, line);
    if (strcmp(line, "E") != 0)
        Die("bad end of response from server");
    Exec("COMMIT");
}

static void
DoSync(Config *c)
{
    long long started, sent, pushed, received, cursor;
    FILE *to, *from;
    pid_t pid;
    int status;

    if (c->server[0] == '\0') {
        fprintf(stderr, "7async: missing server in %s\n", c->path);
        exit(1);
    }
    OpenDatabase(c->db_path);
    /* czas sprzed odczytu: zmiana w trakcie sesji pojdzie nastepnym razem */
    started = (long long)time(NULL);

    pid = SpawnSsh(c, &to, &from);
    fprintf(to, "%s\nC %lld\n", PROTO_HELLO, c->cursor);
    sent = SendChanges(to, c->pushed_at);
    fprintf(to, "E\n");
    /* blad zapisu (ssh padl) - powod przeczyta sie z odpowiedzi/stderr */
    fclose(to);

    ReceivePull(from, &pushed, &received, &cursor);
    fclose(from);
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        Die("ssh session failed");
    if (pushed != sent)
        Die("server stored a different number of records than sent");

    c->pushed_at = started;
    c->cursor = cursor;
    SaveConfig(c);
    printf("sync: sent %lld record(s), received %lld\n", sent, received);
}

static void
DoStatus(const Config *c)
{
    time_t t = (time_t)c->pushed_at;
    char when[32];

    OpenDatabase(c->db_path);
    printf("local database: %s (%lld todo(s), %lld calendar entr(y/ies))\n",
           c->db_path, QueryInt("SELECT COUNT(*) FROM todos"),
           QueryInt("SELECT COUNT(*) FROM calendar_entries"));
    printf("config file: %s\n", c->path);
    printf("server: %s\n", c->server[0] ? c->server : "(not configured)");
    if (c->pushed_at == 0) {
        printf("last sync: never\n");
        return;
    }
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("last sync: %s, server cursor %lld\n", when, c->cursor);
}

static void
Usage(FILE *f)
{
    fprintf(f,
        "usage: 7async <command>\n"
        "\n"
        "commands:\n"
        "  sync     send local changes and fetch remote ones (one ssh session)\n"
        "  status   show local counts, config path, server, last sync\n"
        "  help     show this help\n");
}

int
main(int argc, char *argv[])
{
    Config c;

    if (argc != 2) {
        Usage(stderr);
        return 1;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0) {
        Usage(stdout);
        return 0;
    }
#ifdef __OpenBSD__
    if (pledge("stdio rpath wpath cpath flock proc exec", NULL) == -1)
        Die("pledge failed");
#endif
    signal(SIGPIPE, SIG_IGN);   /* padniety ssh = blad zapisu, nie smierc */
    LoadConfig(&c);

    if (strcmp(argv[1], "sync") == 0)
        DoSync(&c);
    else if (strcmp(argv[1], "status") == 0)
        DoStatus(&c);
    else {
        Usage(stderr);
        return 1;
    }
    sqlite3_close(g_db);
    return 0;
}
