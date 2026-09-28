// Package schema trzyma schemat bazy po stronie serwera (MariaDB, patrz
// mariadb.go) i sprawdzenie lokalnej bazy klienta (SQLite). To dwie
// ROZNE bazy na dwoch roznych maszynach - klient synchronizuje sie z
// serwerem przez HTTP, nie dzieli z nim procesu ani polaczenia do bazy.
package schema

import (
	"database/sql"
	"fmt"
)

// OrganizerSchema - najnizsza wersja (PRAGMA user_version) lokalnej bazy
// ~/.7a/organizer.db, z ktora 7async umie pracowac: od 3 tabele todos i
// calendar_entries maja uuid/updated_at, a skasowane wiersze trafiaja do
// deleted_items (triggery w organizer/store.c w repo 7afilm-tui). Ta sama
// stala co ORGANIZER_SCHEMA w utils/7atodo.c i utils/7acal.c.
const OrganizerSchema = 3

// CheckSQLite sprawdza wersje lokalnej bazy. Schematem zarzadza
// WYLACZNIE 7aorganizer-tui (jego migracje) - 7async, tak jak 7atodo i
// 7acal, niczego tu nie tworzy ani nie zmienia.
func CheckSQLite(db *sql.DB) error {
	var version int
	if err := db.QueryRow("PRAGMA user_version").Scan(&version); err != nil {
		return err
	}
	if version < OrganizerSchema {
		return fmt.Errorf("database has schema %d, needs %d or newer - "+
			"run a current 7aorganizer-tui once to create or upgrade it",
			version, OrganizerSchema)
	}
	return nil
}
