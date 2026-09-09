// Package ics to wlasny, minimalny parser VCALENDAR/VEVENT (RFC 5545) pod
// "7async import-ics" - bez zewnetrznych bibliotek, bo potrzebujemy tylko
// garstki pol (patrz Event nizej), nie pelnego standardu. Swiadomie
// pominiete w v1 (patrz TODO.md): LOCATION, RRULE (wydarzenia cykliczne -
// bierzemy tylko pojedynczy DTSTART, bez ekspansji powtorzen), DTEND.
package ics

import (
	"bufio"
	"fmt"
	"io"
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
			return "", nil, fmt.Errorf("nieprawidlowy DTSTART: %q", value)
		}
		hhmm := fmt.Sprintf("%s:%s", timePart[0:2], timePart[2:4])
		return formatDate(datePart), &hhmm, nil
	}

	if len(v) != 8 {
		return "", nil, fmt.Errorf("nieprawidlowy DTSTART: %q", value)
	}
	return formatDate(v), nil, nil
}

func formatDate(yyyymmdd string) string {
	return fmt.Sprintf("%s-%s-%s", yyyymmdd[0:4], yyyymmdd[4:6], yyyymmdd[6:8])
}
