#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../ui.h"

static int draw(UiCtx *ctx, int win_w, int win_h) {
    static int checked = 0;
    static int selected = 0;
    static const char *items[] = { "Pierwszy", "Drugi", "Trzeci" };
    static char name_buf[64] = "";
    static int name_cursor = 0;

    /* styl (w tym kolory alokowane przez ui_color) budowany raz, nie co
     * klatke - draw() jest wolane przy kazdym ruchu myszki, wiec ponowna
     * alokacja tych samych kolorow za kazdym razem bylaby zbednym
     * obciazeniem serwera X */
    static UiBoxStyle style;
    static int style_ready = 0;
    if (!style_ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = 10;
        style.margin_t = style.margin_b = 10;
        style.padding_l = style.padding_r = 12;
        style.padding_t = style.padding_b = 12;
        style.border_w = 2;
        style.gap = 6;
        ui_color(ctx, "#888888", &style.border_color);
        ui_color(ctx, "#eeeeee", &style.bg_color);
        style_ready = 1;
    }

    UiBox *box = ui_box_begin(ctx, "main", 0, 0, win_w, &style);

    UiRect r1 = ui_box_next_rect(box, 24);
    ui_label(ctx, r1, "Hello, X11!");

    UiRect r1b = ui_box_next_rect(box, 26);
    if (ui_textbox(ctx, r1b, name_buf, sizeof(name_buf), &name_cursor)) {
        printf("tekst: %s\n", name_buf);
        fflush(stdout);
    }

    /* przycisk i checkbox obok siebie w jednym wierszu */
    UiRect r2 = ui_box_next_rect(box, 20);
    UiRect r2a = { r2.x, r2.y, ui_button_width(ctx, "Kliknij mnie"), r2.h };
    UiRect r2b = { r2a.x + r2a.w + 8, r2.y, r2.w - r2a.w - 8, r2.h };

    if (ui_button(ctx, r2a, "Kliknij mnie")) {
        printf("kliknieto\n");
        fflush(stdout);
    }
    if (ui_checkbox(ctx, r2b, "Zaznacz mnie", &checked)) {
        printf("checkbox: %s\n", checked ? "zaznaczony" : "odznaczony");
        fflush(stdout);
    }

    UiRect r4 = ui_box_next_rect(box, 60);
    if (ui_list(ctx, r4, items, 3, &selected)) {
        printf("wybrano: %s\n", items[selected]);
        fflush(stdout);
    }

    ui_box_end(box);

    /* drugi box pod pierwszym - przycisk po lewej, tekst na srodku,
     * przycisk po prawej. Y liczony na podstawie wysokosci "main"
     * z poprzedniej klatki (patrz ui_box_height) + jego marginesy. */
    int nav_y = style.margin_t + ui_box_height(ctx, "main") + style.margin_b;
    UiBox *nav = ui_box_begin(ctx, "nav", 0, nav_y, win_w, &style);

    UiRect r5 = ui_box_next_rect(nav, 20);
    UiRect left, mid, right;
    ui_rect_split3(r5, ui_button_width(ctx, "Wstecz"), ui_button_width(ctx, "Dalej"), 8, &left, &mid, &right);

    if (ui_button(ctx, left, "Wstecz")) {
        printf("wstecz\n");
        fflush(stdout);
    }
    ui_label_centered(ctx, mid, "Strona 1 z 3");
    if (ui_button(ctx, right, "Dalej")) {
        printf("dalej\n");
        fflush(stdout);
    }

    ui_box_end(nav);

    (void) win_h;
    return 0;
}

int main(void) {
#ifdef __OpenBSD__
    /* unveil + pledge (nie samo pledge) - demo.c nie fork+exec'uje niczego,
     * wiec zawezenie widocznych sciezek nie kolisuje z niczym. Ten sam
     * wzorzec/zestaw sciezek co examples/7aclip.c:388-415 (patrz komentarz
     * tam) - minimum do polaczenia z serwerem X i otwarcia fontu. */
    {
        const char *home = getenv("HOME");
        char home_fontcfg[1024];
        char xauth_buf[1024];
        const char *xauth_path;

        snprintf(home_fontcfg, sizeof(home_fontcfg), "%s/.fontconfig",
                 home ? home : ".");
        if (unveil("/usr", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/etc", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/var", "r")            == -1) { perror("unveil"); return 1; }
        if (unveil("/tmp/.X11-unix", "rw") == -1) { perror("unveil"); return 1; }
        if (unveil(home_fontcfg, "r")      == -1) { perror("unveil"); return 1; }
        xauth_path = getenv("XAUTHORITY");
        if (!xauth_path) {
            snprintf(xauth_buf, sizeof(xauth_buf), "%s/.Xauthority",
                     home ? home : ".");
            xauth_path = xauth_buf;
        }
        if (unveil(xauth_path, "r")        == -1) { perror("unveil"); return 1; }
        if (unveil(NULL, NULL)             == -1) { perror("unveil"); return 1; }
        if (pledge("stdio rpath unix prot_exec", NULL) == -1) {
            perror("pledge");
            return 1;
        }
    }
#endif

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int win_w = 320, win_h = 330;

    Window win = XCreateSimpleWindow(dpy, root, 100, 100, win_w, win_h, 0,
                                      BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, "ui demo");
    XMapWindow(dpy, win);
    /* XSetInputFocus wymaga, zeby okno bylo juz viewable - XMapWindow jest
     * asynchroniczne, wiec fokus ustawiamy dopiero po odebraniu MapNotify
     * (ponizej w petli zdarzen), inaczej serwer odpowiada BadMatch */

    GC gc = XCreateGC(dpy, win, 0, NULL);
    UiCtx *ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--14-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }

    XEvent ev;
    int running = 1;
    while (running) {
        XNextEvent(dpy, &ev);
        ui_feed_event(ctx, &ev);

        int redraw = 0;
        switch (ev.type) {
        case Expose:
            redraw = (ev.xexpose.count == 0);
            break;
        case ButtonPress:
        case ButtonRelease:
        case MotionNotify:
        case KeyPress:
            redraw = 1;
            break;
        case MapNotify:
            XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                ui_resize(ctx, win_w, win_h);
            }
            redraw = 1;
            break;
        }

        if (redraw) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h)) running = 0;
            ui_end_frame(ctx);
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
