// 7async - kliencki CLI synchronizacji dla 7atodo/7acal (patrz TODO.md w
// korzeniu repo, sekcja "Synchronizacja z centralnym serwerem"). Lokalna
// baza: SQLite (~/.7a/tasks.db, ta sama co 7atodo.c/7acal.c).
package main

import (
	"context"
	"database/sql"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"time"

	_ "modernc.org/sqlite"

	"7adesktop/sync/internal/config"
	"7adesktop/sync/internal/ics"
	"7adesktop/sync/internal/localdb"
	"7adesktop/sync/internal/schema"
	"7adesktop/sync/internal/syncclient"
)

// usage - pelny help wypisywany na "7async" bez argumentow, "7async
// help"/"-h"/"--help", oraz przy nieznanej komendzie. Flagi import-ics
// wymienione tu wprost (nie tylko przez fs.Usage w doImportICS), zeby
// "7async" bez argumentow od razu pokazywal cala powierzchnie CLI, nie
// tylko liste komend.
func usage(w io.Writer) {
	fmt.Fprint(w, `usage: 7async <command> [args]

commands:
  push                              send local changes to the server
  pull                              fetch remote changes into the local db
  sync                              push, then pull
  status                            show local item count, config path, server URL, last sync
  import-ics [flags] <file.ics>     import events from an exported Google Calendar .ics file
  help                              show this help

import-ics flags:
  --dry-run          print what would be imported, don't write to the db
  --no-description   ignore DESCRIPTION, use SUMMARY only
  --recur-years N    for yearly-recurring events (RRULE FREQ=YEARLY) without
                      their own COUNT/UNTIL, how many years ahead to expand
                      (default 10)

examples:
  7async sync
  7async status
  7async import-ics --dry-run kalendarz.ics
  7async import-ics kalendarz.ics
  7async import-ics --recur-years 20 kalendarz.ics
  7async import-ics --no-description --dry-run kalendarz.ics
`)
}

func requireServerConfig(cfg *config.Config) {
	if cfg.ServerURL == "" || cfg.APIKey == "" {
		confPath, _ := config.Path()
		log.Fatalf("7async: missing server_url/api_key in %s", confPath)
	}
}

func main() {
	if len(os.Args) < 2 {
		usage(os.Stderr)
		os.Exit(1)
	}
	cmd := os.Args[1]

	if cmd == "help" || cmd == "-h" || cmd == "--help" {
		usage(os.Stdout)
		return
	}

	cfg, err := config.Load()
	if err != nil {
		log.Fatalf("7async: config: %v", err)
	}

	db, err := sql.Open("sqlite", cfg.DBPath)
	if err != nil {
		log.Fatalf("7async: cannot open %s: %v", cfg.DBPath, err)
	}
	defer db.Close()

	if err := schema.MigrateSQLite(db); err != nil {
		log.Fatalf("7async: schema migration: %v", err)
	}

	ctx := context.Background()

	switch cmd {
	case "push":
		requireServerConfig(cfg)
		if err := doPush(ctx, db, cfg); err != nil {
			log.Fatalf("7async push: %v", err)
		}
	case "pull":
		requireServerConfig(cfg)
		if err := doPull(ctx, db, cfg); err != nil {
			log.Fatalf("7async pull: %v", err)
		}
	case "sync":
		requireServerConfig(cfg)
		if err := doPush(ctx, db, cfg); err != nil {
			log.Fatalf("7async sync (push): %v", err)
		}
		if err := doPull(ctx, db, cfg); err != nil {
			log.Fatalf("7async sync (pull): %v", err)
		}
	case "status":
		doStatus(db, cfg)
	case "import-ics":
		if err := doImportICS(db, os.Args[2:]); err != nil {
			log.Fatalf("7async import-ics: %v", err)
		}
	default:
		usage(os.Stderr)
		os.Exit(1)
	}
}

func doPush(ctx context.Context, db *sql.DB, cfg *config.Config) error {
	if err := localdb.GenerateMissingUUIDs(db); err != nil {
		return fmt.Errorf("generating uuid: %w", err)
	}

	items, err := localdb.ItemsForPush(db, cfg.LastSync)
	if err != nil {
		return fmt.Errorf("reading local items: %w", err)
	}
	if len(items) == 0 {
		fmt.Println("push: no changes to send")
		return nil
	}

	c, err := syncclient.New(cfg.ServerURL, cfg.APIKey, cfg.TLSCACert)
	if err != nil {
		return fmt.Errorf("client: %w", err)
	}
	if err := c.PushBatch(ctx, items); err != nil {
		return fmt.Errorf("sending: %w", err)
	}
	fmt.Printf("push: sent %d item(s)\n", len(items))
	return nil
}

func doPull(ctx context.Context, db *sql.DB, cfg *config.Config) error {
	c, err := syncclient.New(cfg.ServerURL, cfg.APIKey, cfg.TLSCACert)
	if err != nil {
		return fmt.Errorf("client: %w", err)
	}

	items, err := c.Pull(ctx, cfg.LastSync)
	if err != nil {
		return fmt.Errorf("fetching: %w", err)
	}
	if err := localdb.ApplyPulled(db, items); err != nil {
		return fmt.Errorf("local write: %w", err)
	}
	if err := cfg.SetLastSync(time.Now().Unix()); err != nil {
		return fmt.Errorf("writing last_sync: %w", err)
	}
	fmt.Printf("pull: received %d item(s)\n", len(items))
	return nil
}

func doImportICS(db *sql.DB, args []string) error {
	fs := flag.NewFlagSet("import-ics", flag.ExitOnError)
	dryRun := fs.Bool("dry-run", false, "print what would be imported, don't write to the db")
	noDescription := fs.Bool("no-description", false, "ignore DESCRIPTION, use SUMMARY only")
	recurYears := fs.Int("recur-years", 10, "how many years ahead to expand yearly-recurring (RRULE FREQ=YEARLY) events without their own COUNT/UNTIL")
	if err := fs.Parse(args); err != nil {
		return err
	}

	rest := fs.Args()
	if len(rest) != 1 {
		return fmt.Errorf("usage: 7async import-ics [--dry-run] [--no-description] [--recur-years N] <file.ics>")
	}

	f, err := os.Open(rest[0])
	if err != nil {
		return err
	}
	defer f.Close()

	parsed, err := ics.ParseEvents(f)
	if err != nil {
		return fmt.Errorf("parsing ICS: %w", err)
	}

	currentYear := time.Now().Year()
	var events []ics.Event
	for _, ev := range parsed {
		events = append(events, ics.ExpandYearly(ev, currentYear, *recurYears)...)
	}

	inserted, updated, skipped := 0, 0, 0
	for _, ev := range events {
		if ev.UID == "" || ev.DueDate == "" {
			skipped++
			continue
		}

		body := ev.Summary
		if !*noDescription && ev.Description != "" {
			body += "\n---\n" + ev.Description
		}

		if *dryRun {
			action := "insert"
			if localdb.ItemExistsByUUID(db, ev.UID) {
				action = "update"
			}
			fmt.Printf("[%s] %s | due=%s %s | completed=%v\n",
				action, ev.UID, ev.DueDate, dueTimeLabel(ev.DueTime), ev.Completed)
			continue
		}

		wasInsert, err := localdb.ImportICSItem(db, ev.UID, body, ev.DueDate, ev.DueTime, ev.Completed)
		if err != nil {
			return fmt.Errorf("writing %s: %w", ev.UID, err)
		}
		if wasInsert {
			inserted++
		} else {
			updated++
		}
	}

	if *dryRun {
		fmt.Printf("dry-run: %d event(s) in file (%d after yearly-recurrence expansion), %d skipped (missing UID/DTSTART)\n",
			len(parsed), len(events), skipped)
	} else {
		fmt.Printf("import: %d new, %d updated, %d skipped\n", inserted, updated, skipped)
	}
	return nil
}

func dueTimeLabel(t *string) string {
	if t == nil {
		return "(all day)"
	}
	return *t
}

func doStatus(db *sql.DB, cfg *config.Config) {
	count, err := localdb.CountItems(db)
	if err != nil {
		log.Fatalf("7async status: %v", err)
	}
	confPath, _ := config.Path()

	fmt.Printf("local database: %s (%d item(s), excluding deleted)\n", cfg.DBPath, count)
	fmt.Printf("config file: %s\n", confPath)
	if cfg.ServerURL == "" {
		fmt.Println("server: (server_url not configured)")
	} else {
		fmt.Printf("server: %s\n", cfg.ServerURL)
	}
	if cfg.LastSync == 0 {
		fmt.Println("last sync: never")
	} else {
		fmt.Printf("last sync: %s (unix %d)\n",
			time.Unix(cfg.LastSync, 0).Format(time.RFC3339), cfg.LastSync)
	}
}
