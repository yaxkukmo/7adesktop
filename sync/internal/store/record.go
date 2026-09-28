// Package store trzyma zapytania SQL do tabeli records na serwerze
// (MariaDB, schemat w internal/schema/mariadb.go) - oddzielone od warstwy
// HTTP w internal/httpapi, tak jak logika bazy w 7atodo.c/7acal.c jest
// oddzielona od wywolan ui_*.
package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"time"
)

// Rodzaje rekordow - odpowiadaja tabelom todos i calendar_entries w
// ~/.7a/organizer.db klienta.
const (
	KindTodo  = "todo"
	KindEntry = "entry"
)

// Record - jeden zsynchronizowany wiersz tak, jak go widzi serwer: uuid,
// rodzaj, czas zmiany u klienta (UpdatedAt, do last-write-wins), czas
// zapisu na serwerze (ChangedAt, kursor pull) i tresc (Data) jako JSON,
// ktorej serwer NIE interpretuje - pola wiersza zna tylko klient
// (internal/localdb). Dzieki temu zmiana schematu organizera (nowa
// kolumna) nie wymaga zmian ani migracji po stronie serwera.
type Record struct {
	UUID      string          `json:"uuid"`
	Kind      string          `json:"kind"`
	UpdatedAt int64           `json:"updated_at"` // unix, zegar klienta, ktory zmienil wiersz
	Deleted   bool            `json:"deleted"`
	Data      json.RawMessage `json:"data"` // {} dla skasowanych

	// ChangedAt - mikrosekundy unix wg zegara SERWERA, ustawiane przy
	// kazdym zapisie rekordu (klient go nie wysyla, dostaje w pull).
	// Pull filtruje po nim, nie po UpdatedAt: zmiana zrobiona offline
	// dawno temu (stary UpdatedAt), a wyslana dopiero teraz, i tak ma
	// swiezy ChangedAt, wiec inni klienci jej nie przegapia.
	ChangedAt int64 `json:"changed_at"`
}

// ListSince zwraca rekordy zapisane na serwerze po since (changed_at >
// since, mikrosekundy). since=0 zwraca wszystkie.
func ListSince(ctx context.Context, db *sql.DB, since int64) ([]Record, error) {
	rows, err := db.QueryContext(ctx,
		`SELECT uuid, kind, updated_at, deleted, data, changed_at
		 FROM records WHERE changed_at > ? ORDER BY changed_at ASC`, since)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	records := []Record{}
	for rows.Next() {
		var r Record
		var data string
		if err := rows.Scan(&r.UUID, &r.Kind, &r.UpdatedAt, &r.Deleted, &data, &r.ChangedAt); err != nil {
			return nil, err
		}
		r.Data = json.RawMessage(data)
		records = append(records, r)
	}
	return records, rows.Err()
}

// BatchUpsert wstawia/aktualizuje liste rekordow po uuid w jednej
// transakcji. Last-write-wins: aktualizuje tylko jesli przyslany UpdatedAt
// jest wiekszy niz to, co juz jest w bazie (patrz upsertOne) - starszy/
// rowny przychodzacy zapis jest cicho ignorowany.
func BatchUpsert(ctx context.Context, db *sql.DB, records []Record) error {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback() //nolint:errcheck // no-op po udanym Commit

	for _, r := range records {
		if err := upsertOne(ctx, tx, r); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func upsertOne(ctx context.Context, tx *sql.Tx, r Record) error {
	var existingUpdatedAt int64
	changedAt := time.Now().UnixMicro()

	err := tx.QueryRowContext(ctx, "SELECT updated_at FROM records WHERE uuid=?", r.UUID).
		Scan(&existingUpdatedAt)

	switch {
	case err == sql.ErrNoRows:
		_, err = tx.ExecContext(ctx,
			`INSERT INTO records (uuid, kind, updated_at, deleted, data, changed_at)
			 VALUES (?, ?, ?, ?, ?, ?)`,
			r.UUID, r.Kind, r.UpdatedAt, r.Deleted, string(r.Data), changedAt)
		return err
	case err != nil:
		return err
	case r.UpdatedAt > existingUpdatedAt:
		_, err = tx.ExecContext(ctx,
			`UPDATE records SET kind=?, updated_at=?, deleted=?, data=?, changed_at=? WHERE uuid=?`,
			r.Kind, r.UpdatedAt, r.Deleted, string(r.Data), changedAt, r.UUID)
		return err
	default:
		return nil
	}
}
