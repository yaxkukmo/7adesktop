// Package ics to wlasny, minimalny parser VCALENDAR/VEVENT (RFC 5545) pod
// "7async import-ics" - bez zewnetrznych bibliotek, bo potrzebujemy tylko
// garstki pol (patrz Event nizej), nie pelnego standardu. Swiadomie
// pominiete w v1 (patrz TODO.md): LOCATION, DTEND, oraz RRULE poza
// FREQ=YEARLY (patrz ExpandYearly nizej - to jedyna czestotliwosc, jaka
// realnie wystepuje w eksporcie Google Calendar dla urodzin/rocznic;
// WEEKLY/MONTHLY/DAILY nadal ignorowane, bierzemy tylko pojedynczy
// DTSTART bez ekspansji).
package ics

import (
	"bufio"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// Event - jedno VEVENT po wyciagnieciu interesujacych nas pol.
type Event struct {
	UID         string
	Summary     string
	Description string
	DueDate     string  // YYYY-MM-DD, pusty gdy DTSTART brakuje/niesparsowalny
	DueTime     *string // HH:MM, nil dla wydarzenia calodniowego
	Completed   bool    // STATUS:COMPLETED
	RRule       *RRule  // nil gdy brak RRULE albo FREQ != YEARLY (patrz ExpandYearly)
}

// RRule - fragment RFC 5545 RRULE, TYLKO pola potrzebne do ExpandYearly.
// BYMONTH/BYDAY/BYSETPOS i inne swiadomie nieobslugiwane (patrz naglowek
// pliku) - jesli sie pojawia, Freq zostanie ustawione ale ExpandYearly go
// nie rozpozna jako YEARLY-obslugiwane tylko gdy Freq != "YEARLY".
type RRule struct {
	Freq     string // "YEARLY", "MONTHLY", ... - dalej obslugiwane tylko YEARLY
	Interval int    // domyslnie 1 (brak INTERVAL w pliku)
	Count    int    // 0 = brak limitu z COUNT
	Until    string // YYYY-MM-DD, puste = brak limitu z UNTIL
}

type rawProp struct {
	name  string
	value string
}

// ParseEvents czyta cale VCALENDAR i zwraca liste VEVENT-ow.
func ParseEvents(r io.Reader) ([]Event, error) {
	lines, err := unfoldLines(r)
	if err != nil {
		return nil, err
	}

	var events []Event
	var cur *Event

	for _, line := range lines {
		prop := parseLine(line)

		switch {
		case prop.name == "BEGIN" && prop.value == "VEVENT":
			cur = &Event{}
			continue
		case prop.name == "END" && prop.value == "VEVENT":
			if cur != nil {
				events = append(events, *cur)
				cur = nil
			}
			continue
		}

		if cur == nil {
			continue // poza VEVENT (naglowek VCALENDAR, VTIMEZONE...) - ignorujemy
		}

		switch prop.name {
		case "UID":
			cur.UID = prop.value
		case "SUMMARY":
			cur.Summary = unescapeText(prop.value)
		case "DESCRIPTION":
			cur.Description = unescapeText(prop.value)
		case "STATUS":
			cur.Completed = strings.EqualFold(prop.value, "COMPLETED")
		case "DTSTART":
			if d, t, err := parseDTStart(prop.value); err == nil {
				cur.DueDate = d
				cur.DueTime = t
			}
			// blad parsowania DTSTART - ciche pominiecie, event i tak
			// trafia do listy (z pustym DueDate - wywolujacy go pomija)
		case "RRULE":
			cur.RRule = parseRRule(prop.value)
		}
	}

	return events, nil
}

// unfoldLines sklada linie polamane wg RFC 5545 (kontynuacja zaczyna sie
// od spacji/taba) w pojedyncze logiczne linie, i zdejmuje koncowe \r
// (pliki ICS zwykle maja CRLF).
func unfoldLines(r io.Reader) ([]string, error) {
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024) // DESCRIPTION bywa dlugi

	var lines []string
	for scanner.Scan() {
		raw := strings.TrimRight(scanner.Text(), "\r")
		if len(raw) > 0 && (raw[0] == ' ' || raw[0] == '\t') && len(lines) > 0 {
			lines[len(lines)-1] += raw[1:]
			continue
		}
		lines = append(lines, raw)
	}
	return lines, scanner.Err()
}

// parseLine rozbija "NAZWA;PARAM=WARTOSC:WARTOSC" na nazwe (bez parametrow,
// duze litery) i wartosc. Parametry (np. DTSTART;VALUE=DATE) sa tu
// swiadomie odrzucane - rozrozniamy wydarzenie calodniowe po samej
// obecnosci "T" w wartosci DTSTART (patrz parseDTStart), nie po parametrze
// VALUE=DATE, zeby dzialalo tak samo z plikami, ktore go pomijaja.
func parseLine(line string) rawProp {
	colon := strings.IndexByte(line, ':')
	if colon < 0 {
		return rawProp{name: strings.ToUpper(line)}
	}
	head := line[:colon]
	value := line[colon+1:]

	name := head
	if semi := strings.IndexByte(head, ';'); semi >= 0 {
		name = head[:semi]
	}
	return rawProp{name: strings.ToUpper(name), value: value}
}

// unescapeText odwraca escaping tekstowych wartosci ICS (RFC 5545 3.3.11):
// \n -> nowa linia, \, -> przecinek, \; -> srednik, \\ -> backslash.
func unescapeText(s string) string {
	var b strings.Builder
	b.Grow(len(s))

	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			switch s[i+1] {
			case 'n', 'N':
				b.WriteByte('\n')
			default:
				b.WriteByte(s[i+1])
			}
			i++
			continue
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

// parseDTStart obsluguje "YYYYMMDD" (wydarzenie calodniowe) i
// "YYYYMMDDTHHmmss"/"YYYYMMDDTHHmmssZ" (data+godzina, strefa czasowa
// ignorowana - bierzemy godzine tak, jak jest zapisana w pliku).
func parseDTStart(value string) (dueDate string, dueTime *string, err error) {
	v := strings.TrimSuffix(value, "Z")

	if t := strings.IndexByte(v, 'T'); t >= 0 {
		datePart, timePart := v[:t], v[t+1:]
		if len(datePart) != 8 || len(timePart) < 4 {
			return "", nil, fmt.Errorf("invalid DTSTART: %q", value)
		}
		hhmm := fmt.Sprintf("%s:%s", timePart[0:2], timePart[2:4])
		return formatDate(datePart), &hhmm, nil
	}

	if len(v) != 8 {
		return "", nil, fmt.Errorf("invalid DTSTART: %q", value)
	}
	return formatDate(v), nil, nil
}

func formatDate(yyyymmdd string) string {
	return fmt.Sprintf("%s-%s-%s", yyyymmdd[0:4], yyyymmdd[4:6], yyyymmdd[6:8])
}

// parseRRule rozbija "FREQ=YEARLY;INTERVAL=2;COUNT=5" (kolejnosc pol
// dowolna, nieznane pola jak BYMONTH/BYDAY po prostu ignorowane) na
// RRule. Blad parsowania INTERVAL/COUNT/UNTIL - ciche pominiecie tego
// jednego pola (zostaje wartosc domyslna), nie calego RRULE.
func parseRRule(value string) *RRule {
	rr := &RRule{Interval: 1}
	for _, part := range strings.Split(value, ";") {
		kv := strings.SplitN(part, "=", 2)
		if len(kv) != 2 {
			continue
		}
		switch strings.ToUpper(kv[0]) {
		case "FREQ":
			rr.Freq = strings.ToUpper(kv[1])
		case "INTERVAL":
			if n, err := strconv.Atoi(kv[1]); err == nil && n > 0 {
				rr.Interval = n
			}
		case "COUNT":
			if n, err := strconv.Atoi(kv[1]); err == nil && n > 0 {
				rr.Count = n
			}
		case "UNTIL":
			if d, _, err := parseDTStart(kv[1]); err == nil {
				rr.Until = d
			}
		}
	}
	return rr
}

// ExpandYearly rozwija jeden event z RRULE FREQ=YEARLY na liste konkretnych
// wystapien - kazde z wlasnym UID (oryginalny UID + "-RRRR") i DueDate w
// tym samym miesiacu/dniu co pierwotny DTSTART, ale innym rokiem. Osobne
// UID sa konieczne, bo 7atodo/schema.MigrateSQLite nie ma pojecia zadania
// cyklicznego (jeden wiersz `items` = jeden konkretny termin) - bez tego
// ponowny import nadpisywalby jedno i to samo wystapienie zamiast
// stworzyc kolejne. Inne czestotliwosci (WEEKLY/MONTHLY/DAILY) i eventy
// bez RRULE wracaja bez zmian jako jednoelementowa lista.
//
// fromYear odcina przeszle wystapienia (nie ma sensu zaimportowac 20 lat
// wstecznych urodzin jako zalegle taski). horizonYears ogranicza ekspansje
// "w przod" dla RRULE bez COUNT/UNTIL (typowe dla dorocznych rocznic w
// Google Calendar - powtarzaja sie bez konca). COUNT/UNTIL z pliku, jesli
// obecne, maja pierwszenstwo nad horizonYears.
func ExpandYearly(ev Event, fromYear, horizonYears int) []Event {
	if ev.RRule == nil || ev.RRule.Freq != "YEARLY" || ev.DueDate == "" {
		return []Event{ev}
	}

	origYear, month, day, err := splitDate(ev.DueDate)
	if err != nil {
		return []Event{ev}
	}

	maxYear := fromYear + horizonYears
	if ev.RRule.Until != "" {
		if uy, _, _, err := splitDate(ev.RRule.Until); err == nil && uy < maxYear {
			maxYear = uy
		}
	}

	var out []Event
	year := origYear
	occurrence := 0
	for year <= maxYear {
		if ev.RRule.Count > 0 && occurrence >= ev.RRule.Count {
			break
		}
		if year >= fromYear {
			e := ev
			e.DueDate = fmt.Sprintf("%04d-%02d-%02d", year, month, day)
			e.UID = fmt.Sprintf("%s-%04d", ev.UID, year)
			out = append(out, e)
		}
		occurrence++
		year += ev.RRule.Interval
	}
	if len(out) == 0 {
		return []Event{ev} // wszystkie wystapienia juz w przeszlosci - lepiej zaimportowac oryginal niz nic
	}
	return out
}

func splitDate(d string) (year, month, day int, err error) {
	_, err = fmt.Sscanf(d, "%04d-%02d-%02d", &year, &month, &day)
	return
}
