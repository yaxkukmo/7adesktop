CC = cc

# != dziala zarowno w GNU make, jak i w bmake (domyslny make na OpenBSD).
# $(shell ...) jest rozszerzeniem tylko GNU make, wiec go unikamy.
X11_CFLAGS != sh x11-flags.sh cflags
X11_LIBS != sh x11-flags.sh libs
SQLITE_CFLAGS != pkg-config --cflags sqlite3
SQLITE_LIBS != pkg-config --libs sqlite3
XPM_LIBS != sh x11-flags.sh xpm-libs
CAIRO_CFLAGS != pkg-config --cflags cairo
CAIRO_LIBS != pkg-config --libs cairo

CFLAGS = -Wall -Wextra -O2 -std=c99 $(X11_CFLAGS)
LIBS = $(X11_LIBS)

# 7askm-fetch nie uzywa X11/Xft (samodzielne narzedzie crona, patrz naglowek
# examples/7askm-fetch.c) - osobne CFLAGS bez X11_CFLAGS, zeby nie linkowac
# niepotrzebnych naglowkow/bibliotek do binarki, ktora ich nie potrzebuje.
CFLAGS_STD = -Wall -Wextra -O2 -std=c99

STRIP = strip

all: libui.a demo 7aweather 7asensors 7acal 7atodo 7atimer 7afilm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit 7anotify 7asys 7askm 7askm-fetch 7aclock 7aping 7ashop
	$(STRIP) demo 7aweather 7asensors 7acal 7atodo 7atimer 7afilm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit 7anotify 7asys 7askm 7askm-fetch 7aclock 7aping 7ashop

libui.a: ui.o
	ar rcs $@ ui.o

ui.o: ui.c ui.h
	$(CC) $(CFLAGS) -c ui.c -o ui.o

demo: examples/demo.c libui.a ui.h
	$(CC) $(CFLAGS) examples/demo.c -o demo -L. -lui $(LIBS)

7aweather: examples/7aweather.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7aweather.c -o 7aweather -L. -lui $(LIBS) -lm

7asensors: examples/7asensors.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7asensors.c -o 7asensors -L. -lui $(LIBS)

7acal: examples/7acal.c libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) examples/7acal.c -o 7acal -L. -lui $(LIBS) $(SQLITE_LIBS)

7atodo: examples/7atodo.c libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) examples/7atodo.c -o 7atodo -L. -lui $(LIBS) $(SQLITE_LIBS)

7atimer: examples/7atimer.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7atimer.c -o 7atimer -L. -lui $(LIBS)

7afilm: examples/7afilm.c libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) examples/7afilm.c -o 7afilm -L. -lui $(LIBS) $(SQLITE_LIBS)

7amessage: examples/7amessage.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7amessage.c -o 7amessage -L. -lui $(LIBS)

7arss: examples/7arss.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7arss.c -o 7arss -L. -lui $(LIBS)

7acenter: examples/7acenter.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7acenter.c -o 7acenter -L. -lui $(LIBS) $(XPM_LIBS)

7abubbles: examples/7abubbles.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7abubbles.c -o 7abubbles -L. -lui $(LIBS)

7aclip: examples/7aclip.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7aclip.c -o 7aclip -L. -lui $(LIBS)

7aexit: examples/7aexit.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7aexit.c -o 7aexit -L. -lui $(LIBS)

7anotify: examples/7anotify.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7anotify.c -o 7anotify -L. -lui $(LIBS)

7asys: examples/7asys.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7asys.c -o 7asys -L. -lui $(LIBS)

7askm: examples/7askm.c libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) examples/7askm.c -o 7askm -L. -lui $(LIBS) $(SQLITE_LIBS)

7askm-fetch: examples/7askm-fetch.c
	$(CC) $(CFLAGS_STD) $(SQLITE_CFLAGS) examples/7askm-fetch.c -o 7askm-fetch $(SQLITE_LIBS)

7aclock: examples/7aclock.c
	$(CC) $(CFLAGS) examples/7aclock.c -o 7aclock $(LIBS) -lm

7aping: examples/7aping.c libui.a ui.h
	$(CC) $(CFLAGS) examples/7aping.c -o 7aping -L. -lui $(LIBS)

7ashop: examples/7ashop.c libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) examples/7ashop.c -o 7ashop -L. -lui $(LIBS) $(SQLITE_LIBS)

clean:
	rm -f *.o *.a demo 7aweather 7asensors 7acal 7atodo 7atimer 7afilm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit 7anotify 7asys 7askm 7askm-fetch 7aclock 7aping 7ashop

.PHONY: all clean
