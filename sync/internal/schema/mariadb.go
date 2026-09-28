package schema

import (
	"database/sql"
	"fmt"
)

// Tabela records po stronie serwera (MariaDB) - ogolny magazyn
// zsynchronizowanych wierszy organizera (~/.7a/organizer.db klienta:
// todos i calendar_entries), patrz store.Record. data to JSON, ktorego
// serwer nie interpretuje, wiec nowe kolumny w organizerze nie wymagaja
// tu migracji. uuid jest kluczem glownym (klient generuje je sam, patrz
// triggery w organizer/store.c), bez lokalnego id.
//
// Poprzednia tabela items (synchronizacja tasks.db sprzed przejscia
// 7atodo/7acal na organizer.db) NIE jest tu ani tworzona, ani usuwana -
// zostaje w bazie jako archiwum.
const createTableMariaDB = `CREATE TABLE IF NOT EXISTS records (
	uuid VARCHAR(36) NOT NULL PRIMARY KEY,
	kind VARCHAR(8) NOT NULL,
	updated_at BIGINT NOT NULL,
	deleted TINYINT(1) NOT NULL DEFAULT 0,
	data MEDIUMTEXT NOT NULL,
	changed_at BIGINT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;`

// GET /api/records?since=<changed_at> filtruje po changed_at (czas zapisu
// na serwerze, patrz store.Record) na kazdym pull.
const createIndexMariaDBChangedAt = `CREATE INDEX idx_records_changed_at ON records(changed_at);`

// MigrateMariaDB tworzy tabele records na serwerze (jesli jeszcze nie
// istnieje). Wywolywane przy starcie przez 7asyncd.
func MigrateMariaDB(db *sql.DB) error {
	if _, err := db.Exec(createTableMariaDB); err != nil {
		return fmt.Errorf("create table records: %w", err)
	}
	// "Duplicate key name" (indeks juz istnieje) jest oczekiwany i
	// ignorowany - bez zaleznosci od wersji MariaDB (CREATE INDEX IF NOT
	// EXISTS jest dostepne dopiero od 10.5.2).
	db.Exec(createIndexMariaDBChangedAt) //nolint:errcheck
	return nil
}
