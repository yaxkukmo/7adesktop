// Package localdb trzyma zapytania SQL do lokalnej bazy klienta (SQLite,
// ~/.7a/tasks.db, ta sama co 7atodo.c/7acal.c) - odpowiednik internal/store
// po stronie serwera, ale dla INNEGO schematu (kolumna lokalna `id`, `uuid`
// nullable zamiast klucza glownego - patrz internal/schema/sqlite.go vs
// mariadb.go).
package localdb

import (
	"database/sql"
	"time"

	"github.com/google/uuid"

	"7adesktop/sync/internal/store"
)

// GenerateMissingUUIDs dopisuje uuid do lokalnych items, ktore go jeszcze
// nie maja (rekordy sprzed pierwszego uzycia 7async). Wolane na poczatku
// kazdego push - serwer nigdy nie widzi rekordu bez uuid.
func GenerateMissingUUIDs(db *sql.DB) error {
	rows, err := db.Query("SELECT id FROM items WHERE uuid IS NULL")
	if err != nil {
		return err
	}

	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return err
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		return err
	}
	rows.Close()

	stmt, err := db.Prepare("UPDATE items SET uuid=? WHERE id=?")
	if err != nil {
		return err
	}
	defer stmt.Close()

	for _, id := range ids {
		if _, err := stmt.Exec(uuid.NewString(), id); err != nil {
			return err
		}
	}
	return nil
}

// ItemsForPush zwraca lokalne items zmienione po since (updated_at >
// since). GenerateMissingUUIDs powinno byc wolane wczesniej w tym samym
// push, zeby wszystkie mialy juz uuid.
func ItemsForPush(db *sql.DB, since int64) ([]store.Item, error) {
	rows, err := db.Query(
		`SELECT uuid, priority, due_date, due_time, body, created_at, updated_at, alarm, deleted
		 FROM items WHERE uuid IS NOT NULL AND updated_at IS NOT NULL AND updated_at > ?`, since)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	items := []store.Item{}
	for rows.Next() {
		var it store.Item
		if err := rows.Scan(&it.UUID, &it.Priority, &it.DueDate, &it.DueTime, &it.Body,
			&it.CreatedAt, &it.UpdatedAt, &it.Alarm, &it.Deleted); err != nil {
			return nil, err
		}
		items = append(items, it)
	}
	return items, rows.Err()
}

// ApplyPulled wstawia/aktualizuje items pobrane z serwera do lokalnej
// bazy, po uuid, w jednej transakcji - last-write-wins, ten sam wzorzec co
// store.BatchUpsert po stronie serwera.
func ApplyPulled(db *sql.DB, items []store.Item) error {
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback() //nolint:errcheck // no-op po udanym Commit

	for _, it := range items {
		if err := applyOne(tx, it); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func applyOne(tx *sql.Tx, it store.Item) error {
	var id int64
	var existingUpdatedAt sql.NullInt64

	err := tx.QueryRow("SELECT id, updated_at FROM items WHERE uuid=?", it.UUID).
		Scan(&id, &existingUpdatedAt)

	switch {
	case err == sql.ErrNoRows:
		_, err = tx.Exec(
			`INSERT INTO items (priority, due_date, due_time, body, created_at, updated_at, alarm, deleted, uuid)
			 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			it.Priority, it.DueDate, it.DueTime, it.Body,
			it.CreatedAt, it.UpdatedAt, it.Alarm, it.Deleted, it.UUID)
		return err
	case err != nil:
		return err
	case !existingUpdatedAt.Valid || it.UpdatedAt > existingUpdatedAt.Int64:
		_, err = tx.Exec(
			`UPDATE items SET priority=?, due_date=?, due_time=?, body=?, created_at=?, updated_at=?, alarm=?, deleted=?
			 WHERE id=?`,
			it.Priority, it.DueDate, it.DueTime, it.Body,
			it.CreatedAt, it.UpdatedAt, it.Alarm, it.Deleted, id)
		return err
	default:
		return nil
	}
}

// CountItems zwraca liczbe lokalnych, nieusunietych items - do "7async
// status".
func CountItems(db *sql.DB) (int, error) {
	var n int
	err := db.QueryRow("SELECT COUNT(*) FROM items WHERE deleted=0").Scan(&n)
	return n, err
}

// ItemExistsByUUID - do "7async import-ics --dry-run" (zeby wypisac, czy
// dany UID z pliku ICS zrobilby insert czy update).
func ItemExistsByUUID(db *sql.DB, uid string) bool {
	var id int64
	return db.QueryRow("SELECT id FROM items WHERE uuid=?", uid).Scan(&id) == nil
}

// ImportICSItem wstawia nowy item albo aktualizuje istniejacy (po uuid =
// UID z pliku ICS) na podstawie danych z importera. Na UPDATE swiadomie
// NIE dotyka priority ani created_at - to lokalne pola, ktorych kalendarz
// nie zna, wiec ponowny import nie ma nadpisywac tego, co uzytkownik juz
// ustawil w 7atodo. Zwraca true, gdy to byl INSERT (nowy rekord).
func ImportICSItem(db *sql.DB, uid, body, dueDate string, dueTime *string, completed bool) (bool, error) {
	var id int64
	err := db.QueryRow("SELECT id FROM items WHERE uuid=?", uid).Scan(&id)

	now := time.Now().Unix()
	deleted := 0
	if completed {
		deleted = 1
	}

	switch {
	case err == sql.ErrNoRows:
		_, err = db.Exec(
			`INSERT INTO items (priority, due_date, due_time, body, created_at, updated_at, deleted, uuid)
			 VALUES (2, ?, ?, ?, ?, ?, ?, ?)`,
			dueDate, dueTime, body, now, now, deleted, uid)
		return true, err
	case err != nil:
		return false, err
	default:
		_, err = db.Exec(
			`UPDATE items SET due_date=?, due_time=?, body=?, updated_at=?, deleted=? WHERE id=?`,
			dueDate, dueTime, body, now, deleted, id)
		return false, err
	}
}
