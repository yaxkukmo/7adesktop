// Package schema trzyma migracje tabeli items - osobno dla lokalnej bazy
// klienta (SQLite, ten sam plik co 7atodo.c/7acal.c) i dla centralnego
// serwera (MariaDB, patrz mariadb.go). To dwie ROZNE bazy na dwoch roznych
// maszynach - klient synchronizuje sie z serwerem przez HTTP, nie dzieli z
// nim procesu ani polaczenia do bazy.
package schema

import (
	"database/sql"
	"fmt"
)

// Baza tabeli items - identyczna z CREATE TABLE w OpenDatabase() w
// utils/7atodo.c i utils/7acal.c (te dwa pliki to jedyne inne miejsca,
// ktore tworza ta tabele - schemat musi zostac bajt-w-bajt zgodny co do
// nazw/typow kolumn).
const createTableSQLite = `CREATE TABLE IF NOT EXISTS items (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	priority INTEGER NOT NULL DEFAULT 2,
	due_date TEXT,
	body TEXT NOT NULL DEFAULT '',
	created_at INTEGER NOT NULL,
	alarm BOOLEAN NOT NULL DEFAULT 0,
	uuid TEXT,
	updated_at INTEGER,
	deleted INTEGER NOT NULL DEFAULT 0,
	due_time TEXT
);`

// Dla instalacji sprzed dodania ktorejs z tych kolumn - ten sam wzorzec co
// w 7atodo.c/7acal.c (ALTER TABLE ADD COLUMN, blad "duplicate column" gdy
// kolumna juz istnieje jest oczekiwany i celowo ignorowany).
var alterColumnsSQLite = []string{
	"ALTER TABLE items ADD COLUMN alarm BOOLEAN NOT NULL DEFAULT 0;",
	"ALTER TABLE items ADD COLUMN uuid TEXT;",
	"ALTER TABLE items ADD COLUMN updated_at INTEGER;",
	"ALTER TABLE items ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0;",
	"ALTER TABLE items ADD COLUMN due_time TEXT;",
}

// idx_items_due_date jak w 7atodo.c/7acal.c. idx_items_uuid to jedyny
// indeks, ktorego C-owe apki nie potrzebuja (nigdy nie szukaja po uuid) -
// dokladany tu, bo klient robi upsert po uuid przy kazdym pull; UNIQUE +
// WHERE uuid IS NOT NULL, zeby wiele lokalnych rekordow sprzed pierwszego
// push (uuid=NULL) nie kolidowalo ze soba.
const createIndexesSQLite = `
CREATE INDEX IF NOT EXISTS idx_items_due_date ON items(due_date);
CREATE UNIQUE INDEX IF NOT EXISTS idx_items_uuid ON items(uuid) WHERE uuid IS NOT NULL;
`

// MigrateSQLite doprowadza lokalna tabele items (~/.7a/tasks.db, ta sama
// baza co 7atodo/7acal) do finalnego schematu, niezaleznie od tego, w jakim
// stanie ja zastal (nowa baza, baza sprzed alarm, baza sprzed
// uuid/updated_at/deleted/due_time). Wywolywane przy starcie przez 7async.
func MigrateSQLite(db *sql.DB) error {
	if _, err := db.Exec(createTableSQLite); err != nil {
		return fmt.Errorf("create table items: %w", err)
	}
	for _, stmt := range alterColumnsSQLite {
		db.Exec(stmt) //nolint:errcheck // duplicate column jest oczekiwany i ignorowany
	}
	if _, err := db.Exec(createIndexesSQLite); err != nil {
		return fmt.Errorf("create indexes: %w", err)
	}
	return nil
}
