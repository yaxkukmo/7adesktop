CC = cc

# != dziala zarowno w GNU make, jak i w bmake (domyslny make na OpenBSD).
# $(shell ...) jest rozszerzeniem tylko GNU make, wiec go unikamy.
X11_CFLAGS != sh x11-flags.sh cflags
X11_LIBS != sh x11-flags.sh libs
SQLITE_CFLAGS != pkg-config --cflags sqlite3
SQLITE_LIBS != pkg-config --libs sqlite3
MAGIC_CFLAGS != pkg-config --cflags libmagic
MAGIC_LIBS != pkg-config --libs libmagic
XPM_LIBS != sh x11-flags.sh xpm-libs

CFLAGS = -Wall -Wextra -O2 -std=c99 $(X11_CFLAGS)
LIBS = $(X11_LIBS)

STRIP = strip

all: libui.a demo 7aweather 7asensors 7acal 7atodo 7atimer 7afm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit
	$(STRIP) demo 7aweather 7asensors 7acal 7atodo 7atimer 7afm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit

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

7afm: examples/7afm.c libui.a ui.h
	$(CC) $(CFLAGS) $(MAGIC_CFLAGS) examples/7afm.c -o 7afm -L. -lui $(LIBS) $(MAGIC_LIBS)

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

clean:
	rm -f *.o *.a demo 7aweather 7asensors 7acal 7atodo 7atimer 7afm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit

.PHONY: all clean
