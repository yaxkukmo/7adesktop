// Package store trzyma zapytania SQL do tabeli items na serwerze
// (MariaDB, schemat w internal/schema/mariadb.go) - oddzielone od warstwy
// HTTP w internal/httpapi, tak jak logika bazy w 7atodo.c/7acal.c jest
// oddzielona od wywolan ui_*.
package store

import (
	"context"
	"database/sql"
)

// Item - ksztalt jednego zadania/wydarzenia tak, jak go widzi serwer. Bez
// lokalnego `id` z SQLite klienta (bez znaczenia miedzy urzadzeniami) -
// uuid jest jedynym identyfikatorem.
type Item struct {
	UUID      string  `json:"uuid"`
	Priority  int     `json:"priority"`
	DueDate   *string `json:"due_date"`
	DueTime   *string `json:"due_time"`
	Body      string  `json:"body"`
	CreatedAt int64   `json:"created_at"`
	UpdatedAt int64   `json:"updated_at"`
	Alarm     bool    `json:"alarm"`
	Deleted   bool    `json:"deleted"`
}

// ListSince zwraca items z updated_at > since. since=0 zwraca wszystkie -
// kazdy prawdziwy rekord ma updated_at > 0 (unix timestamp), wiec nie
// trzeba osobnego przypadku.
func ListSince(ctx context.Context, db *sql.DB, since int64) ([]Item, error) {
	rows, err := db.QueryContext(ctx,
		`SELECT uuid, priority, due_date, due_time, body, created_at, updated_at, alarm, deleted
		 FROM items WHERE updated_at > ? ORDER BY updated_at ASC`, since)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	items := []Item{}
	for rows.Next() {
		var it Item
		if err := rows.Scan(&it.UUID, &it.Priority, &it.DueDate, &it.DueTime, &it.Body,
			&it.CreatedAt, &it.UpdatedAt, &it.Alarm, &it.Deleted); err != nil {
			return nil, err
		}
		items = append(items, it)
	}
	return items, rows.Err()
}

// BatchUpsert wstawia/aktualizuje liste items po uuid w jednej transakcji.
// Last-write-wins: aktualizuje tylko jesli przyslany UpdatedAt jest
// wiekszy niz to, co juz jest w bazie (patrz upsertOne) - starszy/rowny
// przychodzacy zapis jest cicho ignorowany.
func BatchUpsert(ctx context.Context, db *sql.DB, items []Item) error {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback() //nolint:errcheck // no-op po udanym Commit

	for _, it := range items {
		if err := upsertOne(ctx, tx, it); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func upsertOne(ctx context.Context, tx *sql.Tx, it Item) error {
	var existingUpdatedAt int64

	err := tx.QueryRowContext(ctx, "SELECT updated_at FROM items WHERE uuid=?", it.UUID).
		Scan(&existingUpdatedAt)

	switch {
	case err == sql.ErrNoRows:
		_, err = tx.ExecContext(ctx,
			`INSERT INTO items (uuid, priority, due_date, due_time, body, created_at, updated_at, alarm, deleted)
			 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			it.UUID, it.Priority, it.DueDate, it.DueTime, it.Body,
			it.CreatedAt, it.UpdatedAt, it.Alarm, it.Deleted)
		return err
	case err != nil:
		return err
	case it.UpdatedAt > existingUpdatedAt:
		_, err = tx.ExecContext(ctx,
			`UPDATE items SET priority=?, due_date=?, due_time=?, body=?, created_at=?, updated_at=?, alarm=?, deleted=?
			 WHERE uuid=?`,
			it.Priority, it.DueDate, it.DueTime, it.Body,
			it.CreatedAt, it.UpdatedAt, it.Alarm, it.Deleted, it.UUID)
		return err
	default:
		return nil
	}
}
