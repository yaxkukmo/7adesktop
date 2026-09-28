// Package httpapi trzyma routing/handlery HTTP dla 7asyncd - oddzielone od
// zapytan SQL w internal/store.
package httpapi

import (
	"database/sql"
	"encoding/json"
	"log"
	"net/http"
	"strconv"

	"7adesktop/sync/internal/store"
)

// NewHandler buduje router dla 7asyncd. apiKey pusty oznaczaloby wylaczone
// auth - nie powinno sie zdarzyc, main.go wymaga niepustego SYNC_API_KEY
// zanim tu w ogole trafi.
func NewHandler(db *sql.DB, apiKey string) http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /api/health", handleHealth)
	// /api/items (tabela items z tasks.db) usuniete razem z przejsciem
	// 7atodo/7acal na organizer.db - stary klient dostaje 404 zamiast
	// po cichu synchronizowac nieuzywana juz baze.
	mux.Handle("GET /api/records", requireAPIKey(apiKey, handleGetRecords(db)))
	mux.Handle("POST /api/records/batch", requireAPIKey(apiKey, handlePostRecordsBatch(db)))

	return mux
}

func handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// requireAPIKey porownuje naglowek X-API-Key z oczekiwanym kluczem -
// brak/zly klucz to 401, zanim jakikolwiek handler dotknie bazy.
func requireAPIKey(apiKey string, next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-API-Key") != apiKey {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		next(w, r)
	}
}

func handleGetRecords(db *sql.DB) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		raw := r.URL.Query().Get("since")
		since := int64(0)
		if raw != "" {
			v, err := strconv.ParseInt(raw, 10, 64)
			if err != nil {
				http.Error(w, "invalid since", http.StatusBadRequest)
				return
			}
			since = v
		}

		records, err := store.ListSince(r.Context(), db, since)
		if err != nil {
			log.Printf("7asyncd: GET /api/records: %v", err)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, records)
	}
}

func handlePostRecordsBatch(db *sql.DB) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var records []store.Record

		if err := json.NewDecoder(r.Body).Decode(&records); err != nil {
			http.Error(w, "invalid JSON body", http.StatusBadRequest)
			return
		}
		if msg := validateRecords(records); msg != "" {
			http.Error(w, msg, http.StatusBadRequest)
			return
		}
		if err := store.BatchUpsert(r.Context(), db, records); err != nil {
			log.Printf("7asyncd: POST /api/records/batch: %v", err)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
	}
}

// validateRecords odrzuca rekordy, ktore i tak nie zmiescilyby sie w
// tabeli albo nie dalyby sie odtworzyc u klienta - lepiej 400 z
// konkretnym powodem niz 500 z bledu MariaDB.
func validateRecords(records []store.Record) string {
	for _, rec := range records {
		switch {
		case rec.UUID == "" || len(rec.UUID) > 36:
			return "invalid uuid: " + rec.UUID
		case rec.Kind != store.KindTodo && rec.Kind != store.KindEntry:
			return "invalid kind for " + rec.UUID + ": " + rec.Kind
		case rec.UpdatedAt <= 0:
			return "missing updated_at for " + rec.UUID
		case !json.Valid(rec.Data):
			return "invalid data for " + rec.UUID
		}
	}
	return ""
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("7asyncd: write response: %v", err)
	}
}
