package schema

import (
	"database/sql"
	"fmt"
)

// Tabela items po stronie serwera (MariaDB) - NIE jest tym samym schematem
// co lokalna baza SQLite (sqlite.go): brak kolumny `id` (autoincrement
// lokalny do jednego urzadzenia, bez znaczenia miedzy urzadzeniami - kazdy
// klient ma wlasna, niezalezna sekwencje), `uuid` jest tu kluczem glownym
// zamiast osobnej nullable kolumny, bo serwer nigdy nie widzi rekordu bez
// uuid (klient generuje je przed pierwszym push). `alarm` jest
// zsynchronizowane razem z reszta pola - to wciaz "ten sam element listy",
// nie osobny, per-urzadzeniowy ustawienie.
//
// W przeciwienstwie do sqlite.go tabela jest tworzona tu od zera (serwer
// nigdy nie mial wczesniejszej wersji tej tabeli w innym ksztalcie), wiec
// CREATE TABLE IF NOT EXISTS wystarcza - nie ma tu odpowiednika
// ALTER TABLE ADD COLUMN z sqlite.go.
const createTableMariaDB = `CREATE TABLE IF NOT EXISTS items (
	uuid VARCHAR(36) NOT NULL PRIMARY KEY,
	priority INT NOT NULL DEFAULT 2,
	due_date VARCHAR(10),
	due_time VARCHAR(5),
	body TEXT NOT NULL,
	created_at BIGINT NOT NULL,
	updated_at BIGINT NOT NULL,
	alarm TINYINT(1) NOT NULL DEFAULT 0,
	deleted TINYINT(1) NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;`

// GET /api/items?since=<unix_ts> filtruje po updated_at na kazdym
// pull - warty indeksu tak samo jak idx_items_due_date po stronie klienta.
const createIndexMariaDBUpdatedAt = `CREATE INDEX idx_items_updated_at ON items(updated_at);`

// MigrateMariaDB tworzy tabele items na serwerze (jesli jeszcze nie
// istnieje). Wywolywane przy starcie przez 7asyncd.
//
// NIEPRZETESTOWANE na zywym MariaDB (brak serwera MariaDB/dockera w tym
// srodowisku, patrz TODO.md) - skladnia zweryfikowana recznie, ale
// wymaga potwierdzenia na docelowym OpenBSD+MariaDB, tak jak bloki
// #ifdef __OpenBSD__ w kodzie C w tym repo.
func MigrateMariaDB(db *sql.DB) error {
	if _, err := db.Exec(createTableMariaDB); err != nil {
		return fmt.Errorf("create table items: %w", err)
	}
	// "Duplicate key name" (indeks juz istnieje) jest oczekiwany i
	// ignorowany - ten sam wzorzec co "duplicate column" w sqlite.go,
	// bez zaleznosci od wersji MariaDB (CREATE INDEX IF NOT EXISTS jest
	// dostepne dopiero od 10.5.2).
	db.Exec(createIndexMariaDBUpdatedAt) //nolint:errcheck
	return nil
}
