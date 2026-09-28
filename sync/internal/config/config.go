// Package config czyta/zapisuje ~/.7a/sync.conf - format klucz=wartosc,
// linie zaczynajace sie od '#' ignorowane (jak w reszcie configow tego
// projektu, patrz center.conf.sample).
package config

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Punkty synchronizacji w sync.conf, osobno dla push i pull:
//   - records_pushed_at: unix wg zegara TEGO klienta - push wysyla wiersze
//     z updated_at >= tej wartosci (updated_at ustawia ten sam zegar);
//   - records_cursor: najwiekszy changed_at (mikrosekundy wg zegara
//     SERWERA, patrz store.Record) z ostatniego pull.
// Stary klucz "last_sync" opisywal synchronizacje tabeli items z tasks.db
// (przed przejsciem 7atodo/7acal na organizer.db) - zostaje w pliku
// nieuzywany, a nowa baza przy pierwszym push wysyla WSZYSTKO (od 0).
const (
	pushedAtKey = "records_pushed_at"
	cursorKey   = "records_cursor"
)

type Config struct {
	ServerURL string
	APIKey    string
	DBPath    string
	TLSCACert string // sciezka do PEM self-signed certu serwera, patrz syncclient.New
	PushedAt  int64  // patrz pushedAtKey
	Cursor    int64  // patrz cursorKey

	path string // do set - sciezka, z ktorej Config zostal wczytany
}

// Path zwraca ~/.7a/sync.conf.
func Path() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".7a", "sync.conf"), nil
}

func defaultDBPath() string {
	home, err := os.UserHomeDir()
	if err != nil {
		home = "."
	}
	return filepath.Join(home, ".7a", "organizer.db")
}

// Load czyta ~/.7a/sync.conf. Brakujacy plik NIE jest bledem - zwraca
// Config z samymi domyslnymi wartosciami (DBPath), zeby np. "7async
// status" dzialalo przed pierwsza konfiguracja. Polecenia wymagajace
// server_url/api_key (push/pull/sync) sprawdzaja to osobno w main.go i
// zglaszaja czytelny blad.
func Load() (*Config, error) {
	path, err := Path()
	if err != nil {
		return nil, err
	}

	cfg := &Config{DBPath: defaultDBPath(), path: path}

	f, err := os.Open(path)
	if os.IsNotExist(err) {
		return cfg, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		key, value, ok := parseLine(scanner.Text())
		if !ok {
			continue
		}
		switch key {
		case "server_url":
			cfg.ServerURL = value
		case "api_key":
			cfg.APIKey = value
		case "db_path":
			cfg.DBPath = value
		case "ca_cert":
			cfg.TLSCACert = value
		case pushedAtKey:
			if v, err := strconv.ParseInt(value, 10, 64); err == nil {
				cfg.PushedAt = v
			}
		case cursorKey:
			if v, err := strconv.ParseInt(value, 10, 64); err == nil {
				cfg.Cursor = v
			}
		}
	}
	return cfg, scanner.Err()
}

func parseLine(line string) (key, value string, ok bool) {
	line = strings.TrimSpace(line)
	if line == "" || strings.HasPrefix(line, "#") {
		return "", "", false
	}
	k, v, found := strings.Cut(line, "=")
	if !found {
		return "", "", false
	}
	return strings.TrimSpace(k), strings.TrimSpace(v), true
}

// SetPushedAt / SetCursor zapisuja punkt synchronizacji (patrz
// pushedAtKey/cursorKey) w ~/.7a/sync.conf.
func (c *Config) SetPushedAt(t int64) error {
	if err := c.set(pushedAtKey, t); err != nil {
		return err
	}
	c.PushedAt = t
	return nil
}

func (c *Config) SetCursor(v int64) error {
	if err := c.set(cursorKey, v); err != nil {
		return err
	}
	c.Cursor = v
	return nil
}

// set aktualizuje jedno pole w ~/.7a/sync.conf, zachowujac pozostale
// linie (w tym komentarze uzytkownika) bez zmian - podmienia istniejaca
// linie albo dopisuje nowa na koncu. Zapis przez plik tymczasowy +
// rename, zeby nie zostawic pol-zapisanego pliku przy awarii w trakcie
// zapisu.
func (c *Config) set(key string, v int64) error {
	raw, err := os.ReadFile(c.path)
	if err != nil && !os.IsNotExist(err) {
		return err
	}

	var lines []string
	if len(raw) > 0 {
		lines = strings.Split(strings.TrimRight(string(raw), "\n"), "\n")
	}

	newLine := fmt.Sprintf("%s=%d", key, v)
	replaced := false
	for i, line := range lines {
		if k, _, ok := parseLine(line); ok && k == key {
			lines[i] = newLine
			replaced = true
			break
		}
	}
	if !replaced {
		lines = append(lines, newLine)
	}

	dir := filepath.Dir(c.path)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".sync.conf.tmp*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())

	if _, err := tmp.WriteString(strings.Join(lines, "\n") + "\n"); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmp.Name(), c.path)
}
