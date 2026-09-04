/*
 * 7atimer.c - port oryginalnej apki z ../7atimer (Xt/Xaw, zwykle Form/
 * Label/Command/Toggle/AsciiText) na biblioteke ui.c/ui.h z tego
 * katalogu - ten sam wzorzec portowania co examples/7aweather.c,
 * examples/7asensors.c, examples/7acal.c i examples/7atodo.c.
 *
 * Dwa NIEZALEZNE liczniki, oba widoczne na stale: stoper (liczy w gore)
 * i minutnik (liczy w dol, edytowalne pola HH/MM/SS + spinnery +/-,
 * alarm "Every: N sec"). Przelacznik trybu u gory wybiera tylko, KTORY
 * z nich obsluguje wspolny rzad Start/Stop/Reset - oba tykaja niezaleznie
 * od wyboru, tak jak w oryginale.
 *
 * Najwieksza roznica wzgledem oryginalu: w Xt/Xaw Form NIE przelicza
 * swojej naturalnej szerokosci synchronicznie, wiec zeby obie ramki
 * (stoper/minutnik) mialy IDENTYCZNA szerokosc, oryginal musial recznie
 * zmierzyc szerokosc wszystkich dzieci obu ramek i przekazac wyliczone
 * box_w W ARGUMENTACH TWORZENIA (ponad 100 linii komentarzy w
 * ../7atimer/7atimer.c o tym, dlaczego kazda inna kolejnosc dziala
 * zawodnie) - tutaj obie ramki to zwykle boxy rysowane co klatke,
 * rozciagniete do win_w jak wszystko inne w tej bibliotece (patrz
 * examples/7acal.c/7atodo.c), wiec cala ta gimnastyka po prostu nie ma
 * czego dotyczyc.
 *
 * Pola HH/MM/SS/alarm to pierwszy port w tym katalogu, ktory faktycznie
 * potrzebuje ui_textbox (poprzednie apki edytowaly tresc w zewnetrznym
 * edytorze) - w tym miejscu przydaje sie tez pelne UTF-8 w ui_textbox
 * (patrz ui.h), choc same pola tutaj sa czysto numeryczne.
 *
 * Trzy niezalezne timery na raz (stoper/minutnik/puls alarmu) sa
 * obslugiwane jednym, wspolnym pollingiem w petli glownej (min. z
 * "due" czasow wszystkich AKTYWNYCH timerow, patrz main()) zamiast
 * osobnych XtIntervalId - ten sam mechanizm co select()-owy timer w
 * examples/7aweather.c, tylko z wieloma niezaleznymi terminami zamiast
 * jednego.
 */

#define _DEFAULT_SOURCE  /* execvp/fork sa POSIX - patrz ta sama uwaga w examples/7aweather.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20
#define CD_ROW2_EXTRA_GAP 8  /* dodatkowy odstep miedzy wierszem HH:MM:SS a wierszem alarmu w boxie Countdown */
#define STEP_BTN_W 14
#define TICK_MS 1000
#define ALARM_PULSE_MS 500
#define ALARM_DURATION_S 10
#define ALARM_PULSES ((ALARM_DURATION_S * 1000) / ALARM_PULSE_MS)
#define MAX_ALARM_TOKENS 16

typedef enum { MODE_STOPWATCH, MODE_COUNTDOWN } Mode;

static Display *g_dpy;
static Mode g_mode = MODE_STOPWATCH;

/* Stoper */
static char g_sw_buf[16] = "00:00:00";
static int g_sw_elapsed = 0;
static int g_sw_running = 0;
static long g_next_sw_tick_ms = 0;

/* Minutnik - bufory sa jedynym zrodlem prawdy o ustawionym czasie,
 * czytane przy KAZDYM Start (dziala tak samo przy swiezym uruchomieniu
 * jak i wznowieniu po Stop) - dokladnie jak w oryginale. */
static char g_cd_hh_buf[8] = "00";
static char g_cd_mm_buf[8] = "09";
static char g_cd_ss_buf[8] = "30";
static char g_cd_alarm_buf[8] = "0";
static int g_cd_hh_cursor = 0, g_cd_mm_cursor = 0, g_cd_ss_cursor = 0, g_cd_alarm_cursor = 0;
static int g_cd_remaining = 0;
static int g_cd_running = 0;
static long g_next_cd_tick_ms = 0;

/* Alarm - uzywany tylko przez minutnik, ale nie ma powodu wiazac go na
 * sztywno z jednym trybem */
static int g_alarm_pulses_left = 0;
static long g_next_alarm_pulse_ms = 0;

typedef struct {
    char alarm_player[128];  /* np. "aplay", "paplay" - pusty = XBell */
    char alarm_sound[256];   /* sciezka do pliku dzwiekowego */
} AppData;

static AppData app_data;

/* -------------------------------------------------------------------- */
/* Zasoby X (alarmPlayer/alarmSound) - czytane bezposrednio przez Xrm,   */
/* ten sam wzorzec co ReadAppString w examples/7atodo.c (konfiguracja    */
/* specyficzna dla tej apki, nie ogolny motyw ui.c).                     */
/* -------------------------------------------------------------------- */

static void
ReadAppString(Display *dpy, const char *name, const char *class_,
              char *out, size_t outsz, const char *dflt)
{
    char *rms;
    XrmDatabase rdb;

    snprintf(out, outsz, "%s", dflt);

    rms = XResourceManagerString(dpy);
    rdb = rms ? XrmGetStringDatabase(rms) : NULL;
    if (rdb) {
        char *type;
        XrmValue value;

        if (XrmGetResource(rdb, name, class_, &type, &value) &&
            type && strcmp(type, "String") == 0 && value.addr)
            snprintf(out, outsz, "%s", value.addr);
        XrmDestroyDatabase(rdb);
    }
}

/* -------------------------------------------------------------------- */
/* Pomoce                                                               */
/* -------------------------------------------------------------------- */

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
FormatHMS(int total_seconds, char *buf, size_t bufsize)
{
    int h = total_seconds / 3600;
    int m = (total_seconds % 3600) / 60;
    int s = total_seconds % 60;

    snprintf(buf, bufsize, "%02d:%02d:%02d", h, m, s);
}

/* AdjustBuf(): zmienia bufor liczbowy o +-1 z zawijaniem na granicach
 * (0->maxval schodzac ponizej zera, maxval->0 przekraczajac gore) - ten
 * sam spinner-idiom co AdjustField() w oryginale, tylko na char* zamiast
 * na XtNstring widgetu AsciiText. */
static void
AdjustBuf(char *buf, size_t bufsz, int delta, int maxval)
{
    long val = strtol(buf, NULL, 10);

    val += delta;
    if (val < 0) val = maxval;
    else if (val > maxval) val = 0;
    snprintf(buf, bufsz, "%02ld", val);
}

/* Zamiast (domyslnego) XBell - odpala zewnetrzny programik (alarmPlayer)
 * z plikiem dzwiekowym (alarmSound) jako ostatnim argumentem, gdy oba sa
 * ustawione. fork()+execvp, bez czekania na dziecko (SIGCHLD=SIG_IGN w
 * main()) - ten sam idiom co SpawnCommand w examples/7atodo.c. */
static void
RunAlarmCommand(void)
{
    char player_buf[256];
    char *argv[MAX_ALARM_TOKENS];
    int argc = 0;
    char *tok;
    pid_t pid;

    snprintf(player_buf, sizeof(player_buf), "%s", app_data.alarm_player);

    tok = strtok(player_buf, " \t");
    while (tok && argc < MAX_ALARM_TOKENS - 2) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0)
        return;
    argv[argc++] = app_data.alarm_sound;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
}

static void
FireAlarmPulse(void)
{
    if (app_data.alarm_player[0] != '\0' && app_data.alarm_sound[0] != '\0')
        RunAlarmCommand();
    else
        XBell(g_dpy, 50);
    g_alarm_pulses_left--;
}

static void
StartAlarm(void)
{
    g_alarm_pulses_left = ALARM_PULSES;
    FireAlarmPulse();
}

static void
StopAlarm(void)
{
    g_alarm_pulses_left = 0;
}

/* -------------------------------------------------------------------- */
/* Stoper                                                               */
/* -------------------------------------------------------------------- */

static void
StopwatchDoStart(void)
{
    if (g_sw_running)
        return;
    g_sw_running = 1;
    g_next_sw_tick_ms = now_ms() + TICK_MS;
}

static void
StopwatchDoStop(void)
{
    g_sw_running = 0;
}

static void
StopwatchDoReset(void)
{
    StopwatchDoStop();
    g_sw_elapsed = 0;
    snprintf(g_sw_buf, sizeof(g_sw_buf), "00:00:00");
}

/* -------------------------------------------------------------------- */
/* Minutnik                                                             */
/* -------------------------------------------------------------------- */

static void
ParseCountdownFields(void)
{
    long hh = strtol(g_cd_hh_buf, NULL, 10);
    long mm = strtol(g_cd_mm_buf, NULL, 10);
    long ss = strtol(g_cd_ss_buf, NULL, 10);

    if (hh < 0) hh = 0;
    if (hh > 999) hh = 999;
    if (mm < 0) mm = 0;
    if (mm > 59) mm = 59;
    if (ss < 0) ss = 0;
    if (ss > 59) ss = 59;

    g_cd_remaining = (int) (hh * 3600 + mm * 60 + ss);
}

static void
UpdateCountdownFields(void)
{
    snprintf(g_cd_hh_buf, sizeof(g_cd_hh_buf), "%02d", g_cd_remaining / 3600);
    snprintf(g_cd_mm_buf, sizeof(g_cd_mm_buf), "%02d", (g_cd_remaining % 3600) / 60);
    snprintf(g_cd_ss_buf, sizeof(g_cd_ss_buf), "%02d", g_cd_remaining % 60);
}

static int
GetAlarmInterval(void)
{
    long n = strtol(g_cd_alarm_buf, NULL, 10);

    if (n < 0) n = 0;
    return (int) n;
}

static void
CountdownDoStart(void)
{
    if (g_cd_running)
        return;

    ParseCountdownFields();
    if (g_cd_remaining <= 0) {
        printf("7atimer: set a countdown time greater than zero before starting.\n");
        fflush(stdout);
        return;
    }
    g_cd_running = 1;
    g_next_cd_tick_ms = now_ms() + TICK_MS;
}

static void
CountdownDoStop(void)
{
    g_cd_running = 0;
    StopAlarm();
}

static void
CountdownDoReset(void)
{
    CountdownDoStop();
    g_cd_remaining = 0;
    UpdateCountdownFields();
}

/* Wywolywane z petli glownej, co TICK_MS, dopoki g_cd_running */
static void
CountdownTick(void)
{
    int interval;

    g_cd_remaining--;
    UpdateCountdownFields();

    if (g_cd_remaining <= 0) {
        g_cd_running = 0;
        StartAlarm();
        return;
    }

    interval = GetAlarmInterval();
    if (interval > 0 && g_cd_remaining % interval == 0)
        StartAlarm();
}

/* -------------------------------------------------------------------- */
/* Ikony - okna (zegar) i dzwonka przy "Every:" - rysowane wprost przez  */
/* prymitywy ui.c (fill_circle/fill_rect), NA ZYWO w docelowym rect,     */
/* ten sam pomysl co ikonki wiersza w examples/7aweather.c. Ikonka okna  */
/* (WM/taskbar) nadal potrzebuje surowej 1-bitowej Pixmapy - jak w        */
/* pozostalych portach.                                                  */
/* -------------------------------------------------------------------- */

static void
DrawClockIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XDrawArc(idpy, p, gc, 2, 2, 27, 27, 0, 360 * 64);
    XDrawLine(idpy, p, gc, 16, 16, 16, 6);
    XDrawLine(idpy, p, gc, 16, 16, 23, 16);
}

static Pixmap
MakeClockIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawClockIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

static void
DrawBellIcon(UiCtx *ctx, UiRect r, const XColor *fg)
{
    int cx = r.x + r.w / 2;
    UiRect clapper;

    ui_fill_circle(ctx, cx, r.y + r.h * 4 / 10, r.w * 3 / 10, fg);
    ui_fill_circle(ctx, cx, r.y + r.h * 6 / 10, r.w * 4 / 10, fg);
    clapper = (UiRect){ cx - 1, r.y + r.h * 9 / 10, 2, 2 };
    ui_fill_rect(ctx, clapper, fg);
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

/* Naglowek sekcji ("Countdown"/"Stopwatch") ZASTEPUJE gorny rzad
 * przyciskow trybu - ui_selection_mark (radio-jak, patrz ui.h) przy
 * etykiecie pokazuje ktory tryb obsluguje wspolny Start/Stop/Reset,
 * klikniecie calego wiersza (znacznik LUB tekst) go przelacza. */
static int
DrawSectionHeader(UiCtx *ctx, int win_w, int y, const UiBoxStyle *style,
                   const char *label, Mode mode)
{
    UiRect header_r, mark_r, label_r;

    header_r = (UiRect){ style->margin_l, y, win_w - 2 * style->margin_l, ROW_H };
    mark_r = (UiRect){ header_r.x, header_r.y, ROW_H, ROW_H };
    label_r = (UiRect){ header_r.x + ROW_H + 4, header_r.y,
                         header_r.w - ROW_H - 4, ROW_H };

    ui_selection_mark(ctx, mark_r, g_mode == mode);
    ui_label(ctx, label_r, label);
    if (ui_hit_test(ctx, header_r))
        g_mode = mode;

    return y + ROW_H + 4;
}

static int
DrawCountdownSection(UiCtx *ctx, int win_w, int y, const UiBoxStyle *style)
{
    UiRect row1, row2;
    UiBox *box;
    int x;
    UiRect hh_r, hh_up_r, hh_down_r, colon1_r;
    UiRect mm_r, mm_up_r, mm_down_r, colon2_r;
    UiRect ss_r, ss_up_r, ss_down_r;
    UiRect bell_r, every_r, alarm_r, sec_r;

    y = DrawSectionHeader(ctx, win_w, y, style, "Countdown", MODE_COUNTDOWN);

    box = ui_box_begin(ctx, "cdbox", 0, y, win_w, style);
    row1 = ui_box_next_rect(box, ROW_H);
    ui_box_next_rect(box, CD_ROW2_EXTRA_GAP);  /* dodatkowy odstep - patrz #define */
    row2 = ui_box_next_rect(box, ROW_H);

    x = row1.x;
    hh_r = (UiRect){ x, row1.y, 28, ROW_H }; x += 28 + 2;
    hh_up_r = (UiRect){ x, row1.y, STEP_BTN_W, ROW_H / 2 };
    hh_down_r = (UiRect){ x, row1.y + ROW_H / 2, STEP_BTN_W, ROW_H - ROW_H / 2 }; x += STEP_BTN_W + 2;
    colon1_r = (UiRect){ x, row1.y, 8, ROW_H }; x += 8 + 2;
    mm_r = (UiRect){ x, row1.y, 22, ROW_H }; x += 22 + 2;
    mm_up_r = (UiRect){ x, row1.y, STEP_BTN_W, ROW_H / 2 };
    mm_down_r = (UiRect){ x, row1.y + ROW_H / 2, STEP_BTN_W, ROW_H - ROW_H / 2 }; x += STEP_BTN_W + 2;
    colon2_r = (UiRect){ x, row1.y, 8, ROW_H }; x += 8 + 2;
    ss_r = (UiRect){ x, row1.y, 22, ROW_H }; x += 22 + 2;
    ss_up_r = (UiRect){ x, row1.y, STEP_BTN_W, ROW_H / 2 };
    ss_down_r = (UiRect){ x, row1.y + ROW_H / 2, STEP_BTN_W, ROW_H - ROW_H / 2 };

    ui_label_centered(ctx, colon1_r, ":");
    ui_label_centered(ctx, colon2_r, ":");

    if (!g_cd_running) {
        ui_textbox(ctx, hh_r, g_cd_hh_buf, sizeof(g_cd_hh_buf), &g_cd_hh_cursor);
        if (ui_button(ctx, hh_up_r, "+")) AdjustBuf(g_cd_hh_buf, sizeof(g_cd_hh_buf), 1, 999);
        if (ui_button(ctx, hh_down_r, "-")) AdjustBuf(g_cd_hh_buf, sizeof(g_cd_hh_buf), -1, 999);

        ui_textbox(ctx, mm_r, g_cd_mm_buf, sizeof(g_cd_mm_buf), &g_cd_mm_cursor);
        if (ui_button(ctx, mm_up_r, "+")) AdjustBuf(g_cd_mm_buf, sizeof(g_cd_mm_buf), 1, 59);
        if (ui_button(ctx, mm_down_r, "-")) AdjustBuf(g_cd_mm_buf, sizeof(g_cd_mm_buf), -1, 59);

        ui_textbox(ctx, ss_r, g_cd_ss_buf, sizeof(g_cd_ss_buf), &g_cd_ss_cursor);
        if (ui_button(ctx, ss_up_r, "+")) AdjustBuf(g_cd_ss_buf, sizeof(g_cd_ss_buf), 1, 59);
        if (ui_button(ctx, ss_down_r, "-")) AdjustBuf(g_cd_ss_buf, sizeof(g_cd_ss_buf), -1, 59);
    } else {
        /* pola same odliczaja w dol - tylko-do-odczytu (jak XawtextRead
         * w oryginale), spinnery nieaktywne, wiec wcale nie rysowane. */
        ui_label(ctx, hh_r, g_cd_hh_buf);
        ui_label(ctx, mm_r, g_cd_mm_buf);
        ui_label(ctx, ss_r, g_cd_ss_buf);
    }

    x = row2.x;
    bell_r = (UiRect){ x, row2.y, 14, ROW_H }; x += 18;
    every_r = (UiRect){ x, row2.y, 40, ROW_H }; x += 42;
    alarm_r = (UiRect){ x, row2.y, 30, ROW_H }; x += 34;
    sec_r = (UiRect){ x, row2.y, 28, ROW_H };

    DrawBellIcon(ctx, bell_r, ui_theme_icon_fg(ctx));
    ui_label(ctx, every_r, "Every:");
    if (!g_cd_running)
        ui_textbox(ctx, alarm_r, g_cd_alarm_buf, sizeof(g_cd_alarm_buf), &g_cd_alarm_cursor);
    else
        ui_label(ctx, alarm_r, g_cd_alarm_buf);
    ui_label(ctx, sec_r, "sec");

    ui_box_end(box);
    y += style->margin_t + ui_box_height(ctx, "cdbox") + style->margin_b;
    return y;
}

static int
DrawStopwatchSection(UiCtx *ctx, int win_w, int y, const UiBoxStyle *style)
{
    UiRect row;
    UiBox *box;

    y = DrawSectionHeader(ctx, win_w, y, style, "Stopwatch", MODE_STOPWATCH);

    box = ui_box_begin(ctx, "swbox", 0, y, win_w, style);
    row = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, row, g_sw_buf);
    ui_box_end(box);
    y += style->margin_t + ui_box_height(ctx, "swbox") + style->margin_b;
    return y;
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y = 10;  /* odstep od gornej krawedzi okna - bez gornego rzedu przyciskow
                  * (patrz DrawSectionHeader) nie ma nic innego, co by go dawalo */
    UiRect brow, start_r, stop_r, reset_r;

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 6;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = 4;
        style.border_w = 1;
        style.gap = 2;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    y = DrawCountdownSection(ctx, win_w, y, &style);
    y = DrawStopwatchSection(ctx, win_w, y, &style);

    y += 10;
    brow = (UiRect){ style.margin_l, y, win_w - 2 * style.margin_l, ROW_H };
    start_r = ui_rect_col(brow, 0, 3, 6);
    stop_r = ui_rect_col(brow, 1, 3, 6);
    reset_r = ui_rect_col(brow, 2, 3, 6);

    if (ui_button(ctx, start_r, "Start")) {
        if (g_mode == MODE_STOPWATCH) StopwatchDoStart(); else CountdownDoStart();
    }
    if (ui_button(ctx, stop_r, "Stop")) {
        if (g_mode == MODE_STOPWATCH) StopwatchDoStop(); else CountdownDoStop();
    }
    if (ui_button(ctx, reset_r, "Reset")) {
        if (g_mode == MODE_STOPWATCH) StopwatchDoReset(); else CountdownDoReset();
    }

    (void) win_h;
    return 0;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    int screen;
    Window root, win;
    GC gc;
    UiCtx *ctx;
    Pixmap icon;
    XWMHints *wmhints;
    XSizeHints *sizehints;
    int win_w = 210, win_h = 215;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    XEvent ev;

    signal(SIGCHLD, SIG_IGN);

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        }
    }

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w examples/7afm.c (patrz komentarz
     * tam): PlayAlarm() fork+execvp'uje 7aTimer.alarmPlayer/alarmSound
     * (X resource, wiec DOWOLNY odtwarzacz/plik od uzytkownika) - unveil
     * dziedziczony po exec by go ograniczyl tak samo jak dowolny opener
     * w 7afm. Bez wpath/cpath - apka nic nie zapisuje na dysk. */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }
    g_dpy = dpy;

    ReadAppString(dpy, "7aTimer.alarmPlayer", "7aTimer.AlarmPlayer",
                  app_data.alarm_player, sizeof(app_data.alarm_player), "");
    ReadAppString(dpy, "7aTimer.alarmSound", "7aTimer.AlarmSound",
                  app_data.alarm_sound, sizeof(app_data.alarm_sound), "");

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    if (geom_mask & WidthValue) win_w = (int) geom_w;
    if (geom_mask & HeightValue) win_h = (int) geom_h;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative) ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative) ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;

    win = XCreateSimpleWindow(dpy, root, win_x, win_y, win_w, win_h, 0,
                               BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, "7aTimer");
    XSetIconName(dpy, win, "7aTimer");

    icon = MakeClockIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 200;
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    running = 1;
    redraw = 1;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
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
        }

        /* stoper/minutnik/puls alarmu - trzy niezalezne timery,
         * obslugiwane jednym pollingiem zamiast osobnych XtIntervalId
         * (patrz komentarz na gorze pliku). Kazdy sam sobie liczy
         * nastepny "due" czas po odpaleniu. */
        {
            long now = now_ms();

            if (g_sw_running && now >= g_next_sw_tick_ms) {
                g_sw_elapsed++;
                FormatHMS(g_sw_elapsed, g_sw_buf, sizeof(g_sw_buf));
                g_next_sw_tick_ms = now + TICK_MS;
                redraw = 1;
            }
            if (g_cd_running && now >= g_next_cd_tick_ms) {
                CountdownTick();
                g_next_cd_tick_ms = now + TICK_MS;
                redraw = 1;
            }
            if (g_alarm_pulses_left > 0 && now >= g_next_alarm_pulse_ms) {
                FireAlarmPulse();
                g_next_alarm_pulse_ms = now + ALARM_PULSE_MS;
                redraw = 1;
            }
        }

        if (redraw) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h)) running = 0;
            ui_end_frame(ctx);
            redraw = 0;
        }
        if (!running)
            break;

        {
            long now = now_ms();
            long wake = -1;

            if (g_sw_running && (wake < 0 || g_next_sw_tick_ms < wake)) wake = g_next_sw_tick_ms;
            if (g_cd_running && (wake < 0 || g_next_cd_tick_ms < wake)) wake = g_next_cd_tick_ms;
            if (g_alarm_pulses_left > 0 && (wake < 0 || g_next_alarm_pulse_ms < wake)) wake = g_next_alarm_pulse_ms;

            if (wake < 0) {
                fd_set rfds;
                int xfd = ConnectionNumber(dpy);

                FD_ZERO(&rfds);
                FD_SET(xfd, &rfds);
                select(xfd + 1, &rfds, NULL, NULL, NULL);
            } else {
                long remaining = wake - now;

                if (remaining > 0) {
                    fd_set rfds;
                    int xfd = ConnectionNumber(dpy);
                    struct timeval tv;

                    FD_ZERO(&rfds);
                    FD_SET(xfd, &rfds);
                    tv.tv_sec = remaining / 1000;
                    tv.tv_usec = (remaining % 1000) * 1000;
                    select(xfd + 1, &rfds, NULL, NULL, &tv);
                }
            }
        }
    }

    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
