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
	mux.Handle("GET /api/items", requireAPIKey(apiKey, handleGetItems(db)))
	mux.Handle("POST /api/items/batch", requireAPIKey(apiKey, handlePostItemsBatch(db)))

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

func handleGetItems(db *sql.DB) http.HandlerFunc {
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

		items, err := store.ListSince(r.Context(), db, since)
		if err != nil {
			log.Printf("7asyncd: GET /api/items: %v", err)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, items)
	}
}

func handlePostItemsBatch(db *sql.DB) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var items []store.Item

		if err := json.NewDecoder(r.Body).Decode(&items); err != nil {
			http.Error(w, "invalid JSON body", http.StatusBadRequest)
			return
		}
		if err := store.BatchUpsert(r.Context(), db, items); err != nil {
			log.Printf("7asyncd: POST /api/items/batch: %v", err)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
	}
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("7asyncd: write response: %v", err)
	}
}
