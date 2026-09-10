# Osobny buildsystem od gorncego Makefile (C) - patrz TODO.md w korzeniu
# repo. CGO_ENABLED=0, bo modernc.org/sqlite jest czystym Go (bez CGo), wiec
# binarki wychodza statyczne bez zaleznosci od libsqlite3 na docelowej
# maszynie. CGO_ENABLED przekazywane inline (export nie dziala w bmake/OpenBSD).

all: 7asyncd 7async

7asyncd:
	CGO_ENABLED=0 go build -o $@ ./cmd/7asyncd

7async:
	CGO_ENABLED=0 go build -o $@ ./cmd/7async

clean:
	rm -f 7asyncd 7async

.PHONY: all clean
