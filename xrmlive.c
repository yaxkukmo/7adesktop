#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <stdio.h>
#include <string.h>

/* Diagnostyka problemu z Xresources w 7atodo - laczy sie bezposrednio z
 * zywym serwerem X i sprawdza XrmGetResource() dla tych samych nazw co
 * ReadAppString() w examples/7atodo.c, bez zadnego kodu 7atodo pomiedzy.
 *
 * Budowanie:  gcc xrmlive.c -lX11 -o xrmlive
 * Uruchomienie: ./xrmlive
 */

int main(void) {
#ifdef WITH_XRMINIT
    XrmInitialize();
    printf("(XrmInitialize() wywolane PRZED zapytaniami)\n");
#else
    printf("(XrmInitialize() NIE wywolane - tak jak w ReadAppString)\n");
#endif

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    char *rms = XResourceManagerString(dpy);
    printf("DISPLAY = %s\n", XDisplayString(dpy));
    printf("XResourceManagerString == NULL? %s\n", rms ? "nie" : "TAK");
    if (rms) printf("dlugosc stringa = %zu bajtow\n", strlen(rms));

    XrmDatabase db = rms ? XrmGetStringDatabase(rms) : NULL;
    printf("XrmGetStringDatabase == NULL? %s\n", db ? "nie" : "TAK");

    struct { const char *name, *cls; } tests[] = {
        {"background", "Background"},
        {"foreground", "Foreground"},
        {"7aTodo.editor", "7aTodo.Editor"},
        {"7aTodo.viewer", "7aTodo.Viewer"},
        {"7aTodo.rowBackground", "7aTodo.RowBackground"},
    };

    if (db) {
        for (int i = 0; i < 5; i++) {
            char *type;
            XrmValue value;
            if (XrmGetResource(db, tests[i].name, tests[i].cls, &type, &value) && value.addr) {
                printf("%-22s -> FOUND: '%s' (type=%s)\n", tests[i].name, value.addr, type);
            } else {
                printf("%-22s -> NOT FOUND\n", tests[i].name);
            }
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
