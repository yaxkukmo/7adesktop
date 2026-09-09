// 7asyncd - serwer synchronizacji dla 7atodo/7acal (patrz TODO.md w korzeniu
// repo, sekcja "Synchronizacja z centralnym serwerem"). Baza: MariaDB (nie
// SQLite - patrz internal/schema/mariadb.go).
package main

import (
	"context"
	"database/sql"
	"errors"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	_ "github.com/go-sql-driver/mysql"

	"7adesktop/sync/internal/httpapi"
	"7adesktop/sync/internal/schema"
)

// DSN w formacie sterownika go-sql-driver/mysql, np.
// "7async:haslo@tcp(127.0.0.1:3306)/7async?parseTime=true" albo przez gniazdo
// unix: "7async:haslo@unix(/var/run/mysql/mysql.sock)/7async".
func dbDSN() string {
	dsn := os.Getenv("SYNC_DB_DSN")
	if dsn == "" {
		log.Fatal("7asyncd: brak SYNC_DB_DSN (DSN do MariaDB, format go-sql-driver/mysql)")
	}
	return dsn
}

func apiKey() string {
	key := os.Getenv("SYNC_API_KEY")
	if key == "" {
		log.Fatal("7asyncd: brak SYNC_API_KEY (wymagany naglowek X-API-Key)")
	}
	return key
}

func listenAddr() string {
	if a := os.Getenv("SYNC_ADDR"); a != "" {
		return a
	}
	return ":8080"
}

func main() {
	db, err := sql.Open("mysql", dbDSN())
	if err != nil {
		log.Fatalf("7asyncd: cannot open MariaDB: %v", err)
	}
	defer db.Close()

	if err := db.Ping(); err != nil {
		log.Fatalf("7asyncd: cannot connect to MariaDB: %v", err)
	}

	if err := schema.MigrateMariaDB(db); err != nil {
		log.Fatalf("7asyncd: schema migration: %v", err)
	}

	srv := &http.Server{
		Addr:    listenAddr(),
		Handler: httpapi.NewHandler(db, apiKey()),
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdownCtx); err != nil {
			log.Printf("7asyncd: shutdown: %v", err)
		}
	}()

	log.Printf("7asyncd: listening on %s", srv.Addr)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("7asyncd: %v", err)
	}
	log.Print("7asyncd: stopped")
}
