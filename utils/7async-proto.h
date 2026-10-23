/*
 * 7async-proto.h - protokol synchronizacji 7async <-> 7async-serve (patrz
 * TODO.md, "Przepisanie sync na C") i wspolne funkcje do jego czytania.
 * Implementacja w 7async-proto.c, linkowanym do OBU binarek - parser jest
 * jeden, wiec klient i serwer nie moga sie rozjechac w szczegolach formatu.
 *
 * Transport: klient uruchamia `ssh <host> 7async-serve` i rozmawia z nim
 * przez stdin/stdout. Jedno wywolanie = jedna sesja: najpierw push, potem
 * pull. Brak kodu sieciowego, TLS i kluczy API - to wszystko robi ssh.
 *
 * klient -> serwer:
 *
 *   7async 1\n
 *   C <cursor>\n           ostatni kursor z pull; 0 = pobierz wszystko
 *   R <uuid> <kind> <updated_at> <deleted> <len>\n<data>   (0..n razy)
 *   E\n
 *
 * serwer -> klient (dopiero po przeczytaniu CALEGO wejscia - klient moze
 * wiec najpierw wszystko wyslac, potem czytac, bez ryzyka zakleszczenia
 * na pelnych potokach):
 *
 *   7async 1\n
 *   P <n>\n                push zapisany (COMMIT), n = liczba rekordow
 *   R ...\n<data>          rekordy z changed_at > cursor: najpierw todo,
 *                          potem entry, skasowane na koncu (zeby todo_uuid
 *                          wpisu trafial w juz zapisane zadanie)
 *   C <cursor>\n           nowy kursor
 *   E\n
 *
 *   X <komunikat>\n        w dowolnym miejscu zamiast reszty: blad, koniec
 *                          sesji (klient wycofuje swoja transakcje)
 *
 * Rekord: uuid (1..36 znakow [0-9A-Za-z-]), kind (todo|entry), updated_at
 * (sekundy unix wg zegara klienta, ktory zmienil wiersz, > 0; last-write-
 * wins), deleted (0|1), len (bajty data, <= PROTO_DATA_MAX). data to
 * ciag pol, kazde to "-" (NULL) albo "<len>:<bajty>" (bez separatorow,
 * binarnie bezpieczne). Serwer sprawdza tylko STRUKTURE pol, nie ich
 * znaczenie - kolejnosc i sens pol zna wylacznie klient (7async.c), wiec
 * zmiana schematu organizera nie wymaga zmian na serwerze. Brakujace
 * pola na koncu czytane sa jako NULL, nadmiarowe sa ignorowane.
 *
 * Kursor to changed_at: monotoniczny licznik serwera (NIE zegar), nadawany
 * w transakcji z blokada zapisu - patrz naglowek 7async-serve.c.
 *
 * Linie naglowkow: max PROTO_LINE_MAX bajtow, pola rozdzielone JEDNA
 * spacja, liczby to same cyfry (bez znaku i spacji). Wszystko spoza
 * tych regul to blad protokolu - zadna strona
 * nie alokuje pamieci na dlugosc podana przez druga.
 */

#ifndef SEVENA_ASYNC_PROTO_H
#define SEVENA_ASYNC_PROTO_H

#include <stddef.h>
#include <stdio.h>

#define PROTO_HELLO     "7async 1"
#define PROTO_LINE_MAX  128          /* z '\n' i '\0' */
#define PROTO_UUID_MAX  36
#define PROTO_DATA_MAX  (64 * 1024)
#define PROTO_FIELDS_MAX 32

typedef struct {
    char      uuid[PROTO_UUID_MAX + 1];
    char      kind[6];               /* "todo" / "entry" */
    long long updated_at;
    int       deleted;
    size_t    len;                   /* dlugosc data */
} ProtoRecord;

typedef struct {
    const char *p;                   /* wskaznik w bufor data, bez '\0' */
    size_t      len;
    int         null;
} ProtoField;

/* Czyta jedna linie bez '\n' do buf. -1: EOF, blad, za dluga linia, '\0'. */
int  ProtoReadLine(FILE *f, char *buf, size_t size);

/* Tylko cyfry, wynik w [min, max]. -1 przy bledzie. */
int  ProtoParseInt(const char *s, long long min, long long max, long long *out);

/* Parsuje linie "R ..." (modyfikuje line). -1 przy bledzie. */
int  ProtoParseRecord(char *line, ProtoRecord *r);

/* Czyta r->len bajtow data do buf (>= PROTO_DATA_MAX). -1 przy bledzie. */
int  ProtoReadData(FILE *f, const ProtoRecord *r, char *buf);

/* Wypisuje naglowek rekordu + data. -1 przy bledzie zapisu. */
int  ProtoWriteRecord(FILE *f, const ProtoRecord *r, const char *data);

/* Dzieli data na pola; zwraca ich liczbe albo -1 przy blednej strukturze. */
int  ProtoParseFields(const char *data, size_t len, ProtoField *out, int max);

/* Dopisuje pole do buf (used/cap w bajtach). -1 gdy sie nie miesci. */
int  ProtoAppendField(char *buf, size_t *used, size_t cap,
                      const char *p, size_t len, int null);

int  ProtoValidUuid(const char *s);
int  ProtoValidKind(const char *s);

#endif
