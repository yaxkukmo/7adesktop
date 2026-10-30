CC = cc

# != dziala zarowno w GNU make, jak i w bmake (domyslny make na OpenBSD).
# $(shell ...) jest rozszerzeniem tylko GNU make, wiec go unikamy.
X11_CFLAGS != sh x11-flags.sh cflags
X11_LIBS != sh x11-flags.sh libs
SQLITE_CFLAGS != pkg-config --cflags sqlite3
SQLITE_LIBS != pkg-config --libs sqlite3
XPM_LIBS != sh x11-flags.sh xpm-libs

CFLAGS = -Wall -Wextra -O2 -std=c99 $(X11_CFLAGS)
LIBS = $(X11_LIBS)

# 7askm-fetch nie uzywa X11/Xft (samodzielne narzedzie crona, patrz naglowek
# utils/7askm-fetch.c) - osobne CFLAGS bez X11_CFLAGS, zeby nie linkowac
# niepotrzebnych naglowkow/bibliotek do binarki, ktora ich nie potrzebuje.
CFLAGS_STD = -Wall -Wextra -O2 -std=c99

STRIP = strip

all: libui.a demo 7aweather 7asensors 7acal 7atodo 7atimer 7afilm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit 7anotify 7askm 7askm-fetch 7aclock 7aping 7ashop 7agroff 7atube 7avol 7arun 7async 7async-serve
	$(STRIP) demo 7aweather 7asensors 7acal 7atodo 7atimer 7afilm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit 7anotify 7askm 7askm-fetch 7aclock 7aping 7ashop 7agroff 7atube 7avol 7arun 7async 7async-serve

libui.a: ui.o
	ar rcs $@ ui.o

ui.o: ui.c ui.h
	$(CC) $(CFLAGS) -c ui.c -o ui.o

demo: utils/demo.c utils/icons/widgets_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/demo.c -o demo -L. -lui $(LIBS)

7aweather: utils/7aweather.c utils/icons/partly_cloudy_day_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7aweather.c -o 7aweather -L. -lui $(LIBS) -lm

7asensors: utils/7asensors.c utils/icons/monitor_heart_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7asensors.c -o 7asensors -L. -lui $(LIBS)

7acal: utils/7acal.c utils/icons/calendar_month_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) utils/7acal.c -o 7acal -L. -lui $(LIBS) $(SQLITE_LIBS)

7atodo: utils/7atodo.c utils/icons/checklist_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) utils/7atodo.c -o 7atodo -L. -lui $(LIBS) $(SQLITE_LIBS)

7atimer: utils/7atimer.c utils/icons/timer_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7atimer.c -o 7atimer -L. -lui $(LIBS)

7afilm: utils/7afilm.c utils/icons/camera_roll_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) utils/7afilm.c -o 7afilm -L. -lui $(LIBS) $(SQLITE_LIBS) -lm

7amessage: utils/7amessage.c utils/icons/chat_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7amessage.c -o 7amessage -L. -lui $(LIBS)

7arss: utils/7arss.c utils/icons/rss_feed_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7arss.c -o 7arss -L. -lui $(LIBS)

7acenter: utils/7acenter.c utils/icons/apps_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7acenter.c -o 7acenter -L. -lui $(LIBS) $(XPM_LIBS)

7abubbles: utils/7abubbles.c utils/icons/bubble_chart_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7abubbles.c -o 7abubbles -L. -lui $(LIBS)

7aclip: utils/7aclip.c utils/icons/content_paste_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7aclip.c -o 7aclip -L. -lui $(LIBS)

7aexit: utils/7aexit.c utils/icons/power_settings_new_48.xbm utils/icons/restart_alt.xbm utils/icons/power_settings_new.xbm utils/icons/refresh.xbm utils/icons/logout.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7aexit.c -o 7aexit -L. -lui $(LIBS)

7anotify: utils/7anotify.c utils/icons/notifications_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7anotify.c -o 7anotify -L. -lui $(LIBS)

7askm: utils/7askm.c utils/icons/train_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) utils/7askm.c -o 7askm -L. -lui $(LIBS) $(SQLITE_LIBS)

7askm-fetch: utils/7askm-fetch.c
	$(CC) $(CFLAGS_STD) $(SQLITE_CFLAGS) utils/7askm-fetch.c -o 7askm-fetch $(SQLITE_LIBS)

7aclock: utils/7aclock.c utils/icons/schedule_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7aclock.c -o 7aclock -L. -lui $(LIBS) -lm

7aping: utils/7aping.c utils/icons/network_ping_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7aping.c -o 7aping -L. -lui $(LIBS)

7ashop: utils/7ashop.c utils/icons/shopping_cart_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) utils/7ashop.c -o 7ashop -L. -lui $(LIBS) $(SQLITE_LIBS)

7agroff: utils/7agroff.c utils/icons/description_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7agroff.c -o 7agroff -L. -lui $(LIBS)

7atube: utils/7atube.c utils/icons/smart_display_48.xbm utils/icons/menu.xbm utils/icons/hourglass_top.xbm utils/icons/hourglass_bottom.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7atube.c -o 7atube -L. -lui $(LIBS)

7avol: utils/7avol.c utils/icons/volume_up_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7avol.c -o 7avol -L. -lui $(LIBS)

7arun: utils/7arun.c utils/icons/terminal_48.xbm libui.a ui.h
	$(CC) $(CFLAGS) utils/7arun.c -o 7arun -L. -lui $(LIBS)

# 7async/7async-serve: synchronizacja organizer.db przez ssh (protokol w
# utils/7async-proto.h), bez X11 - jak 7askm-fetch.
7async: utils/7async.c utils/7async-proto.c utils/7async-proto.h
	$(CC) $(CFLAGS_STD) $(SQLITE_CFLAGS) utils/7async.c utils/7async-proto.c -o 7async $(SQLITE_LIBS)

7async-serve: utils/7async-serve.c utils/7async-proto.c utils/7async-proto.h
	$(CC) $(CFLAGS_STD) $(SQLITE_CFLAGS) utils/7async-serve.c utils/7async-proto.c -o 7async-serve $(SQLITE_LIBS)

clean:
	rm -f *.o *.a demo 7aweather 7asensors 7acal 7atodo 7atimer 7afilm 7amessage 7arss 7acenter 7abubbles 7aclip 7aexit 7anotify 7askm 7askm-fetch 7aclock 7aping 7ashop 7agroff 7atube 7avol 7arun 7async 7async-serve

.PHONY: all clean
