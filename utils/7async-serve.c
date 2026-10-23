/*
 * 7async-serve - serwerowa strona synchronizacji 7async (zastepuje Go-wy
 * 7asyncd + MariaDB). Nie jest demonem: uruchamia go sshd na czas jednej
 * sesji, rozmowa idzie przez stdin/stdout (protokol w 7async-proto.h),
 * po sesji proces znika - 0 MB w spoczynku. Baza: jeden plik SQLite.
 *
 * Instalacja na serwerze (osobny uzytkownik, np. "sync"):
 *
 *   ~sync/.ssh/authorized_keys:
 *     command="/usr/local/bin/7async-serve",restrict ssh-ed25519 AAAA... laptop
 *
 * Baza: argv[1], domyslnie $HOME/.7a/sync-server.db (tworzona przy
 * pierwszej sesji; katalog musi istniec). Model danych jak w 7asyncd:
 * tabela records (uuid, kind, updated_at, deleted, data, changed_at),
 * data nieprzezroczyste dla serwera (sprawdzana tylko struktura pol).
 *
 * Wspolbieznosc (2-3 klientow syncujacych co kilka minut): push idzie w
 * BEGIN IMMEDIATE (blokada zapisu od startu, drugi klient czeka w
 * busy_timeout zamiast dostac SQLITE_BUSY przy podnoszeniu blokady), WAL
 * zeby pull nie blokowal push.
 *
 * Kursor changed_at to licznik, nie zegar: kazda transakcja push dostaje
 * MAX(changed_at)+1 liczone WEWNATRZ blokady zapisu, wiec commity sa
 * scisle rosnace w kolejnosci commitu. Usuwa to dwa bledy 7asyncd
 * (changed_at z time.Now()): cofniecie zegara i wyscig, w ktorym
 * transakcja z mniejszym changed_at commituje po pull innego klienta i
 * jest przez niego na zawsze pominieta. Kursor klienta wiekszy niz
 * cokolwiek w bazie (np. baza odtworzona od zera) jest traktowany jak 0.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "7async-proto.h"

static sqlite3 *g_db;
static char     g_data[PROTO_DATA_MAX];

/* Blad sesji: komunikat do klienta (X ...) i na stderr (log sshd). */
static void
Fail(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("X %s\n", msg);
    fflush(stdout);
    fprintf(stderr, "7async-serve: %s\n", msg);
    if (g_db != NULL)
        sqlite3_close(g_db);   /* niezacommitowana transakcja = rollback */
    exit(1);
}

static void
Exec(const char *sql)
{
    if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK)
        Fail("db: %s", sqlite3_errmsg(g_db));
}

static sqlite3_stmt *
Prepare(const char *sql)
{
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        Fail("db: %s", sqlite3_errmsg(g_db));
    return st;
}

static long long
QueryInt(const char *sql)
{
    sqlite3_stmt *st = Prepare(sql);
    long long v;

    if (sqlite3_step(st) != SQLITE_ROW)
        Fail("db: %s", sqlite3_errmsg(g_db));
    v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void
DefaultDBPath(char *buf, size_t size)
{
    const char *home = getenv("HOME");
    int n;

    if (home == NULL || *home == '\0')
        Fail("HOME not set");
    n = snprintf(buf, size, "%s/.7a/sync-server.db", home);
    if (n < 0 || (size_t)n >= size)
        Fail("database path too long");
}

/* Ogranicza proces do katalogu bazy (plik + -wal/-shm/-journal). */
static void
Sandbox(const char *dbpath)
{
#ifdef __OpenBSD__
    char dir[PATH_MAX];
    char *slash;

    snprintf(dir, sizeof dir, "%s", dbpath);
    slash = strrchr(dir, '/');
    if (slash == NULL)
        snprintf(dir, sizeof dir, ".");
    else if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';
    if (unveil(dir, "rwc") == -1 || unveil(NULL, NULL) == -1)
        Fail("unveil failed");
    if (pledge("stdio rpath wpath cpath flock", NULL) == -1)
        Fail("pledge failed");
#else
    (void)dbpath;
#endif
}

static void
OpenDatabase(const char *path)
{
    if (sqlite3_open_v2(path, &g_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)
        != SQLITE_OK)
        Fail("cannot open database");
    sqlite3_busy_timeout(g_db, 5000);
    Exec("PRAGMA journal_mode=WAL;"
         "PRAGMA temp_store=MEMORY;"
         "CREATE TABLE IF NOT EXISTS records ("
         "  uuid       TEXT PRIMARY KEY,"
         "  kind       TEXT NOT NULL,"
         "  updated_at INTEGER NOT NULL,"
         "  deleted    INTEGER NOT NULL,"
         "  data       BLOB NOT NULL,"
         "  changed_at INTEGER NOT NULL);"
         "CREATE INDEX IF NOT EXISTS records_changed ON records(changed_at);");
}

static void
ExpectLine(char *line, const char *what)
{
    if (ProtoReadLine(stdin, line, PROTO_LINE_MAX) != 0)
        Fail("protocol: expected %s", what);
}

/* Last-write-wins: starszy/rowny updated_at nie nadpisuje rekordu. */
static void
Upsert(sqlite3_stmt *st, const ProtoRecord *r, long long changed_at)
{
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, r->uuid, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, r->kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, r->updated_at);
    sqlite3_bind_int(st, 4, r->deleted);
    sqlite3_bind_blob(st, 5, g_data, (int)r->len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, changed_at);
    if (sqlite3_step(st) != SQLITE_DONE)
        Fail("db: %s", sqlite3_errmsg(g_db));
}

/* Czyta rekordy do "E" i zapisuje je w jednej transakcji. */
static long long
Push(void)
{
    ProtoField fields[PROTO_FIELDS_MAX];
    char line[PROTO_LINE_MAX];
    sqlite3_stmt *st;
    long long changed_at, n = 0;
    ProtoRecord r;

    Exec("BEGIN IMMEDIATE");
    changed_at = QueryInt("SELECT COALESCE(MAX(changed_at), 0) + 1 FROM records");
    st = Prepare(
        "INSERT INTO records (uuid, kind, updated_at, deleted, data, changed_at)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6)"
        " ON CONFLICT(uuid) DO UPDATE SET kind = excluded.kind,"
        "   updated_at = excluded.updated_at, deleted = excluded.deleted,"
        "   data = excluded.data, changed_at = excluded.changed_at"
        " WHERE excluded.updated_at > records.updated_at");

    for (;;) {
        ExpectLine(line, "record or E");
        if (strcmp(line, "E") == 0)
            break;
        if (ProtoParseRecord(line, &r) != 0)
            Fail("protocol: bad record header");
        if (ProtoReadData(stdin, &r, g_data) != 0)
            Fail("protocol: truncated data for %s", r.uuid);
        if (ProtoParseFields(g_data, r.len, fields, PROTO_FIELDS_MAX) < 0)
            Fail("protocol: bad data fields for %s", r.uuid);
        Upsert(st, &r, changed_at);
        n++;
    }
    sqlite3_finalize(st);
    Exec("COMMIT");
    return n;
}

/* Kopiuje kolumne tekstowa z limitem; -1 gdy za dluga albo NULL. */
static int
ColumnText(sqlite3_stmt *st, int col, char *buf, size_t size)
{
    const unsigned char *s = sqlite3_column_text(st, col);

    if (s == NULL || (size_t)sqlite3_column_bytes(st, col) >= size)
        return -1;
    snprintf(buf, size, "%s", (const char *)s);
    return 0;
}

/* Wysyla rekordy z changed_at > cursor, zwraca nowy kursor. */
static long long
Pull(long long cursor)
{
    sqlite3_stmt *st;
    long long next = cursor;
    ProtoRecord r;
    int rc;

    st = Prepare("SELECT uuid, kind, updated_at, deleted, data, changed_at"
                 " FROM records WHERE changed_at > ?1"
                 " ORDER BY deleted, kind = 'entry', changed_at");
    sqlite3_bind_int64(st, 1, cursor);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(st, 4);
        int len = sqlite3_column_bytes(st, 4);
        long long changed_at = sqlite3_column_int64(st, 5);

        if (ColumnText(st, 0, r.uuid, sizeof r.uuid) != 0 ||
            ColumnText(st, 1, r.kind, sizeof r.kind) != 0 ||
            len < 0 || len > PROTO_DATA_MAX)
            Fail("db: corrupt record");
        r.updated_at = sqlite3_column_int64(st, 2);
        r.deleted = sqlite3_column_int(st, 3) != 0;
        r.len = (size_t)len;
        if (ProtoWriteRecord(stdout, &r, len > 0 ? data : "") != 0)
            Fail("write failed");
        if (changed_at > next)
            next = changed_at;
    }
    if (rc != SQLITE_DONE)
        Fail("db: %s", sqlite3_errmsg(g_db));
    sqlite3_finalize(st);
    return next;
}

int
main(int argc, char *argv[])
{
    char dbpath[PATH_MAX];
    char line[PROTO_LINE_MAX];
    long long cursor = 0, pushed;

    if (argc > 2) {
        fprintf(stderr, "usage: 7async-serve [database]\n");
        return 1;
    }
    if (argc == 2)
        snprintf(dbpath, sizeof dbpath, "%s", argv[1]);
    else
        DefaultDBPath(dbpath, sizeof dbpath);

    Sandbox(dbpath);
    OpenDatabase(dbpath);

    ExpectLine(line, "hello");
    if (strcmp(line, PROTO_HELLO) != 0)
        Fail("protocol: unsupported version");
    ExpectLine(line, "cursor");
    if (strncmp(line, "C ", 2) != 0 ||
        ProtoParseInt(line + 2, 0, LLONG_MAX, &cursor) != 0)
        Fail("protocol: bad cursor");

    /* kursor spoza bazy = baza odtworzona od zera, klient bierze wszystko */
    if (cursor > QueryInt("SELECT COALESCE(MAX(changed_at), 0) FROM records"))
        cursor = 0;

    pushed = Push();

    printf("%s\nP %lld\n", PROTO_HELLO, pushed);
    cursor = Pull(cursor);
    printf("C %lld\nE\n", cursor);
    if (fflush(stdout) != 0 || ferror(stdout))
        Fail("write failed");
    sqlite3_close(g_db);
    return 0;
}
