#!/bin/sh
# Wypisuje flagi kompilatora/linkera potrzebne do zbudowania z X11/Xpm.
# Uzycie: x11-flags.sh cflags | x11-flags.sh libs | x11-flags.sh xpm-libs
#
# Na Linuksie zwykle wystarcza pkg-config. Na OpenBSD naglowki i biblioteki
# Xlib/Xpm sa czescia bazowego systemu (Xenocara) pod /usr/X11R6 i nie
# maja wpisow w pkg-config. Brak Xft - galaz core-fonts uzywa XFontSet.
#
# xpm-cflags nie istnieje - X11/xpm.h zyje w tym samym drzewie naglowkow co
# X11/Xlib.h, wiec $(X11_CFLAGS) juz wystarcza (patrz uzycie w Makefile).

set -eu
mode=$1

case "$mode" in
    cflags|libs)
        if pkg-config --exists x11 2>/dev/null; then
            case "$mode" in
                cflags) exec pkg-config --cflags x11 ;;
                libs)   exec pkg-config --libs x11 ;;
            esac
        fi

        # Fallback dla systemow bez danych pkg-config dla X11 (np. OpenBSD).
        case "$mode" in
            cflags) printf '%s\n' "-I/usr/X11R6/include" ;;
            libs)   printf '%s\n' "-L/usr/X11R6/lib -lX11" ;;
        esac
        ;;
    xpm-libs)
        if pkg-config --exists xpm 2>/dev/null; then
            exec pkg-config --libs xpm
        fi
        printf '%s\n' "-L/usr/X11R6/lib -lXpm"
        ;;
esac
