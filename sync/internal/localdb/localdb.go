// Package localdb trzyma zapytania SQL do lokalnej bazy klienta (SQLite,
// ~/.7a/organizer.db, ta sama co 7aorganizer-tui/7atodo.c/7acal.c) -
// odpowiednik internal/store po stronie serwera. Tutaj (i tylko tutaj)
// wiersze tabel todos/calendar_entries sa zamieniane na store.Record i
// z powrotem; serwer traktuje Data jako nieprzezroczysty JSON.
//
// uuid, updated_at i deleted_items utrzymuja triggery w bazie (patrz
// SYNC_TRIGGERS w organizer/store.c w repo 7afilm-tui): nowy wiersz
// dostaje uuid, zmiana podbija updated_at, DELETE zostawia uuid w
// deleted_items. Zapis pobranego rekordu ustawia updated_at jawnie (czas
// z serwera), a wtedy trigger go nie nadpisuje.
package localdb

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"sort"

	"github.com/google/uuid"

	"7adesktop/sync/internal/store"
)

// todoData - Data rekordu rodzaju "todo" (kolumny tabeli todos bez id,
// uuid i updated_at, ktore sa w samym Record).
type todoData struct {
	Title       string  `json:"title"`
	Description *string `json:"description"`
	Priority    int     `json:"priority"`
	Status      string  `json:"status"`
	DoneAt      *string `json:"done_at"`
	CreatedAt   *string `json:"created_at"`
}

// entryData - Data rekordu rodzaju "entry" (calendar_entries). Powiazanie
// z zadaniem idzie po uuid (TodoUUID), bo lokalne id sa na kazdej
// maszynie inne.
type entryData struct {
	Title             string  `json:"title"`
	Description       *string `json:"description"`
	EntryDate         *string `json:"entry_date"`
	EntryTime         *string `json:"entry_time"`
	DurationMin       *int64  `json:"duration_min"`
	RecurrenceType    *string `json:"recurrence_type"`
	RecurrenceWeekday *int64  `json:"recurrence_weekday"`
	RecurrenceDay     *int64  `json:"recurrence_day"`
	RecurrenceMonth   *int64  `json:"recurrence_month"`
	TodoUUID          *string `json:"todo_uuid"`
	CreatedAt         *string `json:"created_at"`
}

// epochOf - updated_at/deleted_at w bazie to tekst UTC "YYYY-MM-DD
// HH:MM:SS" (datetime('now')), a serwer porownuje sekundy unix.
func epochOf(col string) string {
	return "CAST(strftime('%s', " + col + ") AS INTEGER)"
}

// ChangesSince zwraca lokalne zmiany od since (unix, wlacznie - zmiana w
// tej samej sekundzie co poprzedni push nie moze przepasc, a ponowne
// wyslanie tego samego jest nieszkodliwe): zadania, wpisy w kalendarzu i
// skasowane wiersze.
func ChangesSince(db *sql.DB, since int64) ([]store.Record, error) {
	var records []store.Record

	todos, err := db.Query(`SELECT uuid, `+epochOf("updated_at")+`, title, description,
		priority, status, done_at, created_at
		FROM todos WHERE uuid IS NOT NULL AND `+epochOf("updated_at")+` >= ?`, since)
	if err != nil {
		return nil, err
	}
	for todos.Next() {
		var r store.Record
		var d todoData
		if err := todos.Scan(&r.UUID, &r.UpdatedAt, &d.Title, &d.Description,
			&d.Priority, &d.Status, &d.DoneAt, &d.CreatedAt); err != nil {
			todos.Close()
			return nil, err
		}
		if r.Data, err = json.Marshal(d); err != nil {
			todos.Close()
			return nil, err
		}
		r.Kind = store.KindTodo
		records = append(records, r)
	}
	todos.Close()
	if err := todos.Err(); err != nil {
		return nil, err
	}

	entries, err := db.Query(`SELECT e.uuid, `+epochOf("e.updated_at")+`, e.title, e.description,
		e.entry_date, e.entry_time, e.duration_min, e.recurrence_type,
		e.recurrence_weekday, e.recurrence_day, e.recurrence_month, t.uuid, e.created_at
		FROM calendar_entries e LEFT JOIN todos t ON t.id = e.todo_id
		WHERE e.uuid IS NOT NULL AND `+epochOf("e.updated_at")+` >= ?`, since)
	if err != nil {
		return nil, err
	}
	for entries.Next() {
		var r store.Record
		var d entryData
		if err := entries.Scan(&r.UUID, &r.UpdatedAt, &d.Title, &d.Description,
			&d.EntryDate, &d.EntryTime, &d.DurationMin, &d.RecurrenceType,
			&d.RecurrenceWeekday, &d.RecurrenceDay, &d.RecurrenceMonth,
			&d.TodoUUID, &d.CreatedAt); err != nil {
			entries.Close()
			return nil, err
		}
		if r.Data, err = json.Marshal(d); err != nil {
			entries.Close()
			return nil, err
		}
		r.Kind = store.KindEntry
		records = append(records, r)
	}
	entries.Close()
	if err := entries.Err(); err != nil {
		return nil, err
	}

	gone, err := db.Query(`SELECT uuid, kind, `+epochOf("deleted_at")+`
		FROM deleted_items WHERE `+epochOf("deleted_at")+` >= ?`, since)
	if err != nil {
		return nil, err
	}
	defer gone.Close()
	for gone.Next() {
		r := store.Record{Deleted: true, Data: json.RawMessage("{}")}
		if err := gone.Scan(&r.UUID, &r.Kind, &r.UpdatedAt); err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	return records, gone.Err()
}

// ApplyPulled zapisuje rekordy pobrane z serwera do lokalnej bazy, po
// uuid, w jednej transakcji - last-write-wins, ten sam wzorzec co
// store.BatchUpsert po stronie serwera, z tym ze skasowanie (wpis w
// deleted_items) tez ma swoj czas i wygrywa z kazda starsza zmiana.
// Zadania ida przed wpisami, zeby todo_uuid wpisu trafial w juz zapisane
// zadanie.
func ApplyPulled(db *sql.DB, records []store.Record) error {
	order := func(r store.Record) int {
		switch {
		case r.Deleted:
			return 2
		case r.Kind == store.KindTodo:
			return 0
		default:
			return 1
		}
	}
	sorted := append([]store.Record(nil), records...)
	sort.SliceStable(sorted, func(i, j int) bool { return order(sorted[i]) < order(sorted[j]) })

	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback() //nolint:errcheck // no-op po udanym Commit

	for _, r := range sorted {
		if err := applyOne(tx, r); err != nil {
			return fmt.Errorf("%s %s: %w", r.Kind, r.UUID, err)
		}
	}
	return tx.Commit()
}

func tableOf(kind string) (string, error) {
	switch kind {
	case store.KindTodo:
		return "todos", nil
	case store.KindEntry:
		return "calendar_entries", nil
	}
	return "", fmt.Errorf("unknown kind %q", kind)
}

func applyOne(tx *sql.Tx, r store.Record) error {
	table, err := tableOf(r.Kind)
	if err != nil {
		return err
	}

	var id, localAt int64
	err = tx.QueryRow("SELECT id, "+epochOf("updated_at")+" FROM "+table+" WHERE uuid=?", r.UUID).
		Scan(&id, &localAt)
	exists := err == nil
	if err != nil && err != sql.ErrNoRows {
		return err
	}
	if exists && r.UpdatedAt <= localAt {
		return nil // lokalna wersja jest nowsza albo ta sama
	}

	if r.Deleted {
		if _, err := tx.Exec(`INSERT INTO deleted_items (uuid, kind, deleted_at)
			VALUES (?, ?, datetime(?, 'unixepoch'))
			ON CONFLICT(uuid) DO UPDATE SET deleted_at = excluded.deleted_at
			WHERE excluded.deleted_at > deleted_items.deleted_at`,
			r.UUID, r.Kind, r.UpdatedAt); err != nil {
			return err
		}
		if !exists {
			return nil
		}
		// trigger *_gone robi INSERT OR IGNORE, wiec zostaje czas z serwera
		_, err = tx.Exec("DELETE FROM "+table+" WHERE id=?", id)
		return err
	}

	var goneAt sql.NullInt64
	err = tx.QueryRow("SELECT "+epochOf("deleted_at")+" FROM deleted_items WHERE uuid=?", r.UUID).
		Scan(&goneAt)
	if err != nil && err != sql.ErrNoRows {
		return err
	}
	if goneAt.Valid && goneAt.Int64 >= r.UpdatedAt {
		return nil // skasowane lokalnie pozniej niz ta zmiana
	}

	if r.Kind == store.KindTodo {
		err = applyTodo(tx, r, exists, id)
	} else {
		err = applyEntry(tx, r, exists, id)
	}
	if err == nil && goneAt.Valid {
		_, err = tx.Exec("DELETE FROM deleted_items WHERE uuid=?", r.UUID)
	}
	return err
}

func applyTodo(tx *sql.Tx, r store.Record, exists bool, id int64) error {
	var d todoData
	if err := json.Unmarshal(r.Data, &d); err != nil {
		return err
	}
	if exists {
		_, err := tx.Exec(`UPDATE todos SET title=?, description=?, priority=?, status=?,
			done_at=?, updated_at=datetime(?, 'unixepoch') WHERE id=?`,
			d.Title, d.Description, d.Priority, d.Status, d.DoneAt, r.UpdatedAt, id)
		return err
	}
	_, err := tx.Exec(`INSERT INTO todos (uuid, title, description, priority, status,
		done_at, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, COALESCE(?, datetime('now')), datetime(?, 'unixepoch'))`,
		r.UUID, d.Title, d.Description, d.Priority, d.Status, d.DoneAt, d.CreatedAt, r.UpdatedAt)
	return err
}

func applyEntry(tx *sql.Tx, r store.Record, exists bool, id int64) error {
	var d entryData
	if err := json.Unmarshal(r.Data, &d); err != nil {
		return err
	}
	const todoID = "(SELECT id FROM todos WHERE uuid = ?)"
	if exists {
		_, err := tx.Exec(`UPDATE calendar_entries SET title=?, description=?,
			entry_date=?, entry_time=?, duration_min=?, recurrence_type=?,
			recurrence_weekday=?, recurrence_day=?, recurrence_month=?,
			todo_id=`+todoID+`, updated_at=datetime(?, 'unixepoch') WHERE id=?`,
			d.Title, d.Description, d.EntryDate, d.EntryTime, d.DurationMin,
			d.RecurrenceType, d.RecurrenceWeekday, d.RecurrenceDay, d.RecurrenceMonth,
			d.TodoUUID, r.UpdatedAt, id)
		return err
	}
	_, err := tx.Exec(`INSERT INTO calendar_entries (uuid, title, description,
		entry_date, entry_time, duration_min, recurrence_type, recurrence_weekday,
		recurrence_day, recurrence_month, todo_id, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, `+todoID+`,
		        COALESCE(?, datetime('now')), datetime(?, 'unixepoch'))`,
		r.UUID, d.Title, d.Description, d.EntryDate, d.EntryTime, d.DurationMin,
		d.RecurrenceType, d.RecurrenceWeekday, d.RecurrenceDay, d.RecurrenceMonth,
		d.TodoUUID, d.CreatedAt, r.UpdatedAt)
	return err
}

// Counts zwraca liczbe lokalnych zadan i wpisow w kalendarzu - do
// "7async status".
func Counts(db *sql.DB) (todos, entries int, err error) {
	err = db.QueryRow("SELECT (SELECT COUNT(*) FROM todos), (SELECT COUNT(*) FROM calendar_entries)").
		Scan(&todos, &entries)
	return todos, entries, err
}

// icsNamespace - staly (musi byc, patrz RFC 4122 4.3) namespace do
// wyprowadzania deterministycznych uuid z surowych ICS UID (patrz
// stableUUIDFromICS nizej). Wartosc bez znaczenia poza stalosc -
// wygenerowana raz jako losowy v4. Ta sama co przed przejsciem na
// organizer.db, wiec wydarzenia zaimportowane kiedys do tasks.db (import
// tasks.db zachowuje uuid) sa przy ponownym imporcie .ics rozpoznawane.
var icsNamespace = uuid.MustParse("6f1c1e2a-2f0a-4f8b-9c1e-0a5b7d3c9e4f")

// stableUUIDFromICS wyprowadza deterministyczny (ten sam wejsciowy UID
// zawsze daje ten sam wynik uuid v5 - kluczowe dla idempotencji ponownego
// importu) 36-znakowy uuid z surowego ICS UID. Potrzebne, bo ICS UID
// (zwlaszcza z Google Calendar, bywa >70 znakow, plus "-RRRR" doklejane
// przez ics.ExpandYearly dla wydarzen cyklicznych) nie miesci sie w
// kolumnie `uuid VARCHAR(36)` po stronie serwera
// (internal/schema/mariadb.go).
func stableUUIDFromICS(icsUID string) string {
	return uuid.NewSHA1(icsNamespace, []byte(icsUID)).String()
}

// EntryExistsByICS - do "7async import-ics --dry-run" (zeby wypisac, czy
// dany UID z pliku ICS zrobilby insert czy update). uid to surowy UID z
// pliku ICS.
func EntryExistsByICS(db *sql.DB, uid string) bool {
	var id int64
	return db.QueryRow("SELECT id FROM calendar_entries WHERE uuid=?", stableUUIDFromICS(uid)).
		Scan(&id) == nil
}

// ImportICSEntry wstawia nowy wpis w kalendarzu albo aktualizuje
// istniejacy (po stableUUIDFromICS(uid), uid = surowy UID z pliku ICS).
// Wydarzenie zakonczone (STATUS:COMPLETED) kasuje istniejacy wpis, a
// nowego nie tworzy. Zwraca true, gdy to byl INSERT (nowy wpis).
func ImportICSEntry(db *sql.DB, uid, title string, description *string,
	date string, time *string, completed bool) (bool, error) {
	dbUUID := stableUUIDFromICS(uid)

	var id int64
	err := db.QueryRow("SELECT id FROM calendar_entries WHERE uuid=?", dbUUID).Scan(&id)

	switch {
	case err != nil && err != sql.ErrNoRows:
		return false, err
	case completed:
		if err == nil {
			_, err = db.Exec("DELETE FROM calendar_entries WHERE id=?", id)
			return false, err
		}
		return false, nil
	case err == sql.ErrNoRows:
		// uuid podany jawnie, wiec trigger *_new nie ustawi updated_at;
		// wczesniejsze lokalne skasowanie tego wydarzenia przestaje obowiazywac
		_, err = db.Exec(`INSERT INTO calendar_entries
			(uuid, title, description, entry_date, entry_time, updated_at)
			VALUES (?, ?, ?, ?, ?, datetime('now'))`,
			dbUUID, title, description, date, time)
		if err == nil {
			_, err = db.Exec("DELETE FROM deleted_items WHERE uuid=?", dbUUID)
		}
		return true, err
	default:
		_, err = db.Exec(`UPDATE calendar_entries SET title=?, description=?,
			entry_date=?, entry_time=?, recurrence_type=NULL WHERE id=?`,
			title, description, date, time, id)
		return false, err
	}
}
