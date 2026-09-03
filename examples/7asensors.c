/*
 * 7asensors.c - port oryginalnej apki z ../7asensors (Xt/Xaw, zwykle
 * Label/Form, bez wlasnego widgetu) na biblioteke ui.c/ui.h z tego
 * katalogu - ten sam wzorzec portowania co examples/7aweather.c (patrz
 * tam obszerniejszy komentarz o roznicach wzgledem Xt/Shell).
 *
 * Logika czytania/parsowania danych (RunCommand, FindSysctlValue,
 * ParseHumanKMGT, FormatHumanBytes, UpdateMemory/UpdateCPU/UpdateNetwork)
 * jest przeniesiona z oryginalu prawie bez zmian - to funkcje na char*,
 * niezalezne od toolkitu; jedyna roznica to cel zapisu (globalne bufory
 * tekstowe zamiast XtVaSetValues(widget, XtNlabel, ...)).
 *
 * Warstwa UI (cztery sekcje w kolejnosci CPU/Memory/Battery/Network, kazda
 * naglowek + biala ramka z wierszami tresci) to w oryginale CreateSection
 * budujace osobne widgety Label/Form; tutaj to jedna funkcja DrawSection
 * wywolywana co klatke, rysujaca naglowek (ui_label) i box (ui_box_begin/
 * ui_box_next_rect) - patrz DrawSection nizej.
 *
 * Interfejs sieciowy dla "ifconfig" byl w oryginale zasobem X
 * (XtNiface, domyslnie "iwm0") - tutaj to opcjonalny pierwszy argument
 * linii polecen (7asensors iwm0), bo ui.c nie ma odpowiednika
 * XtGetApplicationResources dla dowolnych, wlasnych zasobow apki (tylko
 * globalny motyw kolorow - patrz ui_theme_* w ui.h).
 *
 * Nowosc bez odpowiednika w oryginale: koleczko-wskaznik + przycisk SMT
 * On/Off w naglowku sekcji CPU (DrawCpuSection), pokazujace AKTUALNY stan
 * sysctl hw.smt (tylko OpenBSD >=6.4 - oba ukryte, gdy sysctl niedostepny,
 * np. na Linuksie). Klikniecie przycisku odpala komende przelaczajaca z
 * zasobu X 7aSensors.smtOnCommand/smtOffCommand (domyslnie "doas sysctl
 * hw.smt=1"/"=0"); kolor koleczka tez z zasobow (smtOnColor/smtOffColor,
 * domyslnie green/gray50) - wszystkie cztery czytane bezposrednio przez
 * Xrm (ReadAppString), tym samym wzorcem co editor/terminal/viewer w
 * examples/7atodo.c.
 *
 * Kolejna nowosc: uzycie widgetow ui_meter/ui_segment_meter (patrz ui.h)
 * zamiast czystego tekstu tam, gdzie wartosc ma naturalny ulamek "cos z
 * czegos". ui_meter (ciagly pasek dwoma kolorami) - zuzycie RAM
 * (DrawMemorySection, ulamek uzyte/total) i sila sygnalu WiFi
 * (DrawNetworkSection, ulamek procent/100) - dzieki temu boxy Memory/
 * Battery/Network zawieraja JEDEN wiersz: sam pasek. ui_segment_meter (rzad
 * kwadracikow, dyskretny odpowiednik ui_meter) - rdzenie CPU online/total
 * (DrawCpuSection, np. 4 z 8 kwadracikow zamiast tekstu "Cores: 4/8").
 * Speed zostaje tekstem w boxie CPU - nie ma dla niego sensownego "z czego".
 * SSID (DrawNetworkSection) przeniesiony do naglowka, obok etykiety "Wifi" -
 * IP usuniete jako malo przydatne w tym widoku.
 *
 * Czwarta sekcja bez odpowiednika w oryginale: Battery (DrawBatterySection/
 * UpdateBattery), zasilana komenda "apm" (OpenBSD - patrz komentarz przy
 * UpdateBattery o formacie jej wyjscia). Procent baterii to znowu ui_meter,
 * tym samym wzorcem co RAM/Signal; na maszynie bez baterii/bez apm w PATH
 * (typowy desktop, caly Linux) sekcja pokazuje tekstowe fallbacki zamiast
 * pustego paska, bez ukrywania calej sekcji. Zamiast tekstowego wiersza
 * "AC: ..." - koleczko-wskaznik przy etykiecie "Battery" (ten sam pomysl co
 * przy SMT w DrawCpuSection), swiecace sie (wypelnione accent) gdy maszyna
 * dziala na baterii (AC nie podlaczone), samo obrys (line_fg) gdy podlaczona
 * do zasilania - ukryte calkowicie, gdy stan baterii jest nieznany.
 */

#define _DEFAULT_SOURCE  /* popen/pclose sa POSIX, poza -std=c99 - patrz ta sama uwaga w examples/7aweather.c */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include "../ui.h"

#define ICON_SIZE 32
#define REFRESH_INTERVAL_MS 5000   /* tyle co domyslne w oryginale (refreshInterval) */
#define ROW_H 20
#define DEFAULT_IFACE "iwm0"       /* jak w oryginale - dostosuj do wlasnej maszyny */

/* -------------------------------------------------------------------- */
/* Stan - wiersze tresci trzech sekcji, aktualizowane przy odswiezeniu  */
/* -------------------------------------------------------------------- */

static const char *g_iface = DEFAULT_IFACE;

/* RAM jako pasek (ui_meter) zamiast dwoch tekstowych wierszy - frac =
 * uzyte/total, label = "uzyte / total" (np. "1.8G / 8.3G"). */
static double g_mem_frac = 0.0;
static char   g_mem_bar_label[72] = "..."; /* "%s / %s" z dwoch buforow po 32 bajty (FormatHumanBytes) + separator */

/* Cores jako rzad kwadracikow (ui_segment_meter) zamiast tekstu "Cores:
 * 4/8" - total<=0 oznacza nieznane (np. sysctl niedostepny), wtedy
 * DrawCpuSection pokazuje tekst "?" zamiast pustego rzedu. Speed zostaje
 * tekstem - to pojedyncza wartosc, nie ma tu "z czego". */
static int  g_cpu_cores_total = -1;
static int  g_cpu_cores_online = -1;
static char g_cpu_speed_line[64] = "...";

/* SSID (g_net_ssid) rysowany w naglowku sekcji obok etykiety "Wifi" -
 * linie Interface:/IP: usuniete, byly czysto informacyjne/malo przydatne
 * (g_iface i tak widac w wywolaniu apki z CLI). Signal to pasek (ui_meter) -
 * frac = procent/100, -1.0 gdy nieznany/brak sygnalu (np. polaczenie
 * przewodowe), wtedy DrawNetworkSection pokazuje zwykly tekst zamiast
 * pustego paska. */
static char   g_net_ssid[64] = "...";
static double g_net_signal_frac = -1.0;
static char   g_net_signal_label[16] = "-";

/* Bateria (komenda "apm" - OpenBSD, patrz UpdateBattery) - procent jako
 * pasek (ui_meter, ten sam wzorzec co RAM). frac < 0 oznacza brak baterii/
 * apm niedostepne (np. Linux/desktop bez apm w PATH - RunCommand dostaje
 * wtedy puste wyjscie), wtedy DrawBatterySection pokazuje tekst zamiast
 * pustego paska, ten sam wzorzec co Signal wyzej. g_batt_on_battery - stan
 * zasilania (1 = na baterii/AC niepodlaczone, 0 = na AC), uzywany TYLKO do
 * koloru koleczka-wskaznika w naglowku (DrawBatterySection), bez wlasnego
 * tekstowego wiersza. */
static double g_batt_frac = -1.0;
static char   g_batt_bar_label[16] = "-"; /* "%d%%" - 16 zamiast oczekiwanych max. 4 znakow ("100%"), zeby uciszyc -Wformat-truncation (sscanf %d teoretycznie moze dac wiecej cyfr) */
static int    g_batt_on_battery = 0;

/* SMT (Simultaneous Multi-Threading) - stan czytany z sysctl hw.smt (patrz
 * UpdateCPU) i dwie komendy do jego przelaczania, konfigurowalne przez
 * zasoby X (7aSensors.smtOnCommand/smtOffCommand - patrz ReadAppString w
 * main i Xresources.sample), bo wlaczanie/wylaczanie SMT zwykle wymaga
 * podniesienia uprawnien (doas/sudo), specyficznego dla maszyny usera. */
static int  g_smt_state = -1; /* -1 = nieznany (np. brak hw.smt na Linuksie), 0 = off, 1 = on */
static char g_smt_on_cmd[128];
static char g_smt_off_cmd[128];
static char     g_led_on_color_name[32];
static char     g_led_off_color_name[32];
static XftColor g_led_on_color;
static XftColor g_led_off_color;

/* Koleczko-wskaznik stanu przed przyciskiem (patrz DrawCpuSection) -
 * kolor tez konfigurowalny przez zasoby X (7aSensors.smtOnColor/
 * smtOffColor), nazwa koloru czytana do stringa przed ui_init (jak
 * smtOn/OffCommand wyzej), sama XftColor alokowana raz w main PO
 * ui_init, bo dopiero wtedy istnieje Display/Visual/Colormap potrzebny
 * ui_color (patrz XftColorAllocName w ui.c). */

/* -------------------------------------------------------------------- */
/* Uruchamianie komend i parsowanie ich wyjscia - bez zmian wzgledem    */
/* oryginalu (poza strlcpy->snprintf, patrz examples/7aweather.c po ten */
/* sam powod: strlcpy nie jest ISO C i nie ma go na kazdym libc).       */
/* -------------------------------------------------------------------- */

/* Zasoby X specyficzne dla tej apki (smtOnCommand/smtOffCommand) - ten sam
 * wzorzec ReadAppString co w examples/7atodo.c (editor/terminal/viewer). */
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

/* Odpala komende przelaczajaca SMT w tle, bez czekania na wynik (SIGCHLD
 * zignorowany w main, wiec proces potomny nie zostaje zombie) - komenda
 * idzie przez "sh -c", bo to pelny string powloki z zasobu X (np. "doas
 * sysctl hw.smt=1"), nie pojedyncza binarka z argumentami. */
static void
SpawnDetached(const char *cmd)
{
    pid_t pid;

    if (!cmd || !cmd[0])
        return;

    pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit(127);
    }
}

static void
RunCommand(const char *cmd, char *out, size_t outsize)
{
    FILE *fp;
    size_t n = 0;

    out[0] = '\0';
    fp = popen(cmd, "r");
    if (!fp)
        return;
    n = fread(out, 1, outsize - 1, fp);
    out[n] = '\0';
    pclose(fp);
}

static int
FindSysctlValue(const char *buf, const char *key, char *out, size_t outsize)
{
    size_t keylen = strlen(key);
    const char *p = strstr(buf, key);
    size_t i;

    while (p) {
        if ((p == buf || p[-1] == '\n') && p[keylen] == '=')
            break;
        p = strstr(p + 1, key);
    }
    if (!p) {
        out[0] = '\0';
        return -1;
    }
    p += keylen + 1;

    i = 0;
    while (p[i] && p[i] != '\n' && i < outsize - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

static double
ParseHumanKMGT(const char *s)
{
    char *end;
    double val = strtod(s, &end);

    switch (*end) {
    case 'K': case 'k': return val * 1024.0;
    case 'M': case 'm': return val * 1024.0 * 1024.0;
    case 'G': case 'g': return val * 1024.0 * 1024.0 * 1024.0;
    case 'T': case 't': return val * 1024.0 * 1024.0 * 1024.0 * 1024.0;
    default:            return val;
    }
}

static void
FormatHumanBytes(double bytes, char *out, size_t outsize)
{
    const char *unit = "B";

    if (bytes >= 1024.0 * 1024.0 * 1024.0) {
        bytes /= 1024.0 * 1024.0 * 1024.0;
        unit = "G";
    } else if (bytes >= 1024.0 * 1024.0) {
        bytes /= 1024.0 * 1024.0;
        unit = "M";
    } else if (bytes >= 1024.0) {
        bytes /= 1024.0;
        unit = "K";
    }
    snprintf(out, outsize, "%.1f%s", bytes, unit);
}

static void
UpdateMemory(void)
{
    char buf[2048];
    char avm_tok[16] = "?", fre_tok[16] = "?";
    char *line3;
    char *tok;
    int idx;

    RunCommand("vmstat 2>/dev/null", buf, sizeof(buf));

    line3 = strchr(buf, '\n');
    if (line3)
        line3 = strchr(line3 + 1, '\n');
    if (line3) {
        line3++;
        idx = 0;
        tok = strtok(line3, " \t\r\n");
        while (tok) {
            if (idx == 2)
                snprintf(avm_tok, sizeof(avm_tok), "%s", tok);
            else if (idx == 3) {
                snprintf(fre_tok, sizeof(fre_tok), "%s", tok);
                break;
            }
            idx++;
            tok = strtok(NULL, " \t\r\n");
        }
    }

    {
        double avm_bytes = ParseHumanKMGT(avm_tok);
        double fre_bytes = ParseHumanKMGT(fre_tok);
        double total_bytes = avm_bytes + fre_bytes;
        char used_str[32], total_str[32];

        FormatHumanBytes(total_bytes, total_str, sizeof(total_str));
        FormatHumanBytes(avm_bytes, used_str, sizeof(used_str));

        g_mem_frac = total_bytes > 0.0 ? avm_bytes / total_bytes : 0.0;
        snprintf(g_mem_bar_label, sizeof(g_mem_bar_label), "%s / %s", used_str, total_str);
    }
}

static void
UpdateCPU(void)
{
    char buf[2048];
    char ncpu[16], ncpuonline[16], cpuspeed[16], smt[16];

    RunCommand("sysctl hw.ncpu hw.ncpufound hw.ncpuonline hw.cpuspeed hw.smt 2>/dev/null",
               buf, sizeof(buf));

    g_cpu_cores_total = FindSysctlValue(buf, "hw.ncpu", ncpu, sizeof(ncpu)) == 0
                         ? atoi(ncpu) : -1;
    g_cpu_cores_online = FindSysctlValue(buf, "hw.ncpuonline", ncpuonline, sizeof(ncpuonline)) == 0
                          ? atoi(ncpuonline) : g_cpu_cores_total;
    if (FindSysctlValue(buf, "hw.cpuspeed", cpuspeed, sizeof(cpuspeed)) != 0)
        snprintf(cpuspeed, sizeof(cpuspeed), "?");

    /* hw.smt istnieje tylko na OpenBSD (>=6.4) - jego brak w wyjsciu
     * sysctl (np. na Linuksie) zostawia stan nieznany i chowa przycisk w
     * DrawCpuSection, zamiast pokazywac mylaca etykiete. */
    if (FindSysctlValue(buf, "hw.smt", smt, sizeof(smt)) == 0)
        g_smt_state = (smt[0] == '0') ? 0 : 1;
    else
        g_smt_state = -1;

    snprintf(g_cpu_speed_line, sizeof(g_cpu_speed_line), "Speed: %s MHz", cpuspeed);
}

static void
UpdateNetwork(void)
{
    char cmd[128], buf[4096];
    char ssid[128] = "-";
    char signal[8] = "";
    char *p, *nl;

    snprintf(cmd, sizeof(cmd), "ifconfig %s 2>/dev/null", g_iface);
    RunCommand(cmd, buf, sizeof(buf));

    p = buf;
    while (p && *p) {
        nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';

        if (strstr(p, "ieee80211:") != NULL && strstr(p, "join") != NULL) {
            char *join = strstr(p, "join");
            char *tok;

            sscanf(join + 5, "%127s", ssid);
            tok = strtok(p, " \t");
            while (tok) {
                size_t tl = strlen(tok);
                if (tl > 1 && tok[tl - 1] == '%') {
                    snprintf(signal, sizeof(signal), "%s", tok);
                    break;
                }
                tok = strtok(NULL, " \t");
            }
        }

        p = nl ? nl + 1 : NULL;
    }

    snprintf(g_net_ssid, sizeof(g_net_ssid), "%s", ssid);

    if (signal[0]) {
        g_net_signal_frac = atoi(signal) / 100.0; /* atoi zatrzymuje sie na '%' z konca tokena */
        snprintf(g_net_signal_label, sizeof(g_net_signal_label), "%s", signal);
    } else {
        g_net_signal_frac = -1.0;
        snprintf(g_net_signal_label, sizeof(g_net_signal_label), "-");
    }
}

/* Parsuje wyjscie "apm" (OpenBSD), np.:
 *   Battery state: high, 64% remaining, unknown life estimate
 *   AC adapter state: connected
 *   Performance adjustment mode: auto (400 MHz)
 * Format pierwszej linii jest staly, wiec sscanf z literalnym ", " i "%%"
 * wyciaga stan i procent jednym rzutem - prostsze niz recznie skanowac
 * tokeny jak w UpdateNetwork (tam trzeba, bo kolejnosc/obecnosc pol w
 * ifconfig jest zmienna). Na maszynie bez baterii/bez apm w PATH RunCommand
 * dostaje puste wyjscie (2>/dev/null tlumi "command not found"), sscanf nie
 * dopasuje niczego i zmienne zostaja przy wartosciach domyslnych - patrz
 * DrawBatterySection. Linia AC adapter state (druga linia wyjscia) czytana
 * CALA (%31[^\n], nie %s) - wartosc "not connected" ma spacje w srodku,
 * samo %s wyciagnieoby tylko "not"; g_batt_on_battery = wszystko, co NIE
 * zaczyna sie od "connected" (a wiec tez "not connected"/"backup power"). */
static void
UpdateBattery(void)
{
    char buf[512];
    char ac[32] = "?";
    int pct = -1;
    char *acline;

    RunCommand("apm 2>/dev/null", buf, sizeof(buf));

    sscanf(buf, "Battery state: %*[^,], %d%%", &pct); /* %* pomija slowo stanu - nigdzie nie wyswietlane */

    acline = strstr(buf, "AC adapter state:");
    if (acline)
        sscanf(acline, "AC adapter state: %31[^\n]", ac);

    g_batt_on_battery = strncmp(ac, "connected", 9) != 0;

    if (pct >= 0) {
        g_batt_frac = pct / 100.0;
        snprintf(g_batt_bar_label, sizeof(g_batt_bar_label), "%d%%", pct);
    } else {
        g_batt_frac = -1.0;
        snprintf(g_batt_bar_label, sizeof(g_batt_bar_label), "-");
    }
}

static void
UpdateAll(void)
{
    UpdateMemory();
    UpdateCPU();
    UpdateNetwork();
    UpdateBattery();
}

/* -------------------------------------------------------------------- */
/* Ikona okna - prosty gauge (luk + igla + kropka), rysowany wprost     */
/* Xlibem na 1-bitowej Pixmapie (jak w 7aweather.c - to jedyne miejsce, */
/* ktore potrzebuje surowej Pixmapy, nie backbuffera z ui.c).           */
/* -------------------------------------------------------------------- */

static void
DrawGaugeIconBitmap(Display *idpy, Pixmap p, GC gc)
{
    XDrawArc(idpy, p, gc, 3, 3, 26, 26, 0, 180 * 64);
    XDrawLine(idpy, p, gc, 16, 16, 24, 8);
    XFillArc(idpy, p, gc, 13, 13, 6, 6, 0, 360 * 64);
}

static Pixmap
MakeGaugeIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    DrawGaugeIconBitmap(idpy, icon, gc);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                            */
/* -------------------------------------------------------------------- */

/* Wszystkie cztery sekcje (CPU/Memory/Battery/Network) rysowane sa w
 * JEDNYM boxie "main" otwieranym w draw(). Funkcje Draw*Section przyjmuja
 * gotowy UiBox * i dodaja do niego wiersze przez ui_box_next_rect - nie
 * tworza wlasnych boxow. Wiersz naglowka sekcji i wiersze tresci sa
 * rownoprawnymi wierszami tego samego boxa; gap=4 i padding_t/b tworza
 * wystarczajaca biale przestrzen bez oddzielnych ramek per-sekcja. */

static void
DrawCpuSection(UiCtx *ctx, UiBox *box)
{
    UiRect row, dot_r, btn_r;
    int btn_w, dot_d;

    row = ui_box_next_rect(box, ROW_H);
    btn_w = ui_button_width(ctx, "SMT Off");
    dot_d = ROW_H - 10;
    {
        int label_w = ui_text_width(ctx, "CPU") + 6;
        ui_label(ctx, (UiRect){ row.x, row.y, label_w, row.h }, "CPU");
        dot_r = (UiRect){ row.x + label_w, row.y, dot_d, row.h };
        btn_r = (UiRect){ row.x + row.w - btn_w, row.y, btn_w, row.h };
    }

    if (g_smt_state != -1) {
        const char *btn_label = g_smt_state ? "SMT On" : "SMT Off";
        int cx = dot_r.x + dot_r.w / 2;
        int cy = dot_r.y + dot_r.h / 2;

        ui_fill_circle(ctx, cx, cy, dot_d / 2, g_smt_state ? &g_led_on_color : &g_led_off_color);
        ui_draw_circle(ctx, cx, cy, dot_d / 2, 1, ui_theme_line_fg(ctx));

        if (ui_button(ctx, btn_r, btn_label))
            SpawnDetached(g_smt_state ? g_smt_off_cmd : g_smt_on_cmd);
    }

    {
        UiRect cores_label_r, cores_r;

        row = ui_box_next_rect(box, ROW_H);
        ui_rect_split3(row, ui_text_width(ctx, "Cores:") + 6, 0, 6, &cores_label_r, &cores_r, NULL);
        ui_label(ctx, cores_label_r, "Cores:");
        if (g_cpu_cores_total > 0)
            ui_segment_meter(ctx, cores_r, g_cpu_cores_online, g_cpu_cores_total, 4);
        else
            ui_label(ctx, cores_r, "?");
    }

    row = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, row, g_cpu_speed_line);
}

static void
DrawMemorySection(UiCtx *ctx, UiBox *box)
{
    UiRect row;

    row = ui_box_next_rect(box, ROW_H);
    ui_label(ctx, row, "Memory");

    row = ui_box_next_rect(box, ROW_H);
    ui_meter(ctx, row, g_mem_frac, g_mem_bar_label);
}

static void
DrawBatterySection(UiCtx *ctx, UiBox *box)
{
    UiRect row, dot_r;
    int dot_d, label_w;

    row = ui_box_next_rect(box, ROW_H);
    dot_d = ROW_H - 10;
    label_w = ui_text_width(ctx, "Battery") + 6;

    ui_label(ctx, (UiRect){ row.x, row.y, label_w, row.h }, "Battery");
    dot_r = (UiRect){ row.x + label_w, row.y, dot_d, row.h };

    if (g_batt_frac >= 0.0) {
        int cx = dot_r.x + dot_r.w / 2;
        int cy = dot_r.y + dot_r.h / 2;

        ui_fill_circle(ctx, cx, cy, dot_d / 2, g_batt_on_battery ? &g_led_on_color : &g_led_off_color);
        ui_draw_circle(ctx, cx, cy, dot_d / 2, 1, ui_theme_line_fg(ctx));
    }

    row = ui_box_next_rect(box, ROW_H);
    if (g_batt_frac >= 0.0)
        ui_meter(ctx, row, g_batt_frac, g_batt_bar_label);
    else
        ui_label(ctx, row, "Battery: -");
}

static void
DrawNetworkSection(UiCtx *ctx, UiBox *box)
{
    UiRect row, label_r, ssid_r;
    int label_w;

    row = ui_box_next_rect(box, ROW_H);
    label_w = ui_text_width(ctx, "Wifi") + 6;
    ui_rect_split3(row, label_w, 0, 6, &label_r, &ssid_r, NULL);
    ui_label(ctx, label_r, "Wifi");
    ui_label(ctx, ssid_r, g_net_ssid);

    row = ui_box_next_rect(box, ROW_H);
    if (g_net_signal_frac >= 0.0)
        ui_meter(ctx, row, g_net_signal_frac, g_net_signal_label);
    else
        ui_label(ctx, row, "Signal: -");
}

static int
draw(UiCtx *ctx, int win_w, int win_h)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y = 0;
    UiBox *box;

    if (!ready) {
        style = (UiBoxStyle){0};
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = 6;
        style.padding_l = style.padding_r = 6;
        style.padding_t = style.padding_b = 4;
        style.border_w = 1;
        style.gap = 4;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    box = ui_box_begin(ctx, "main", 0, y, win_w, &style);
    DrawCpuSection(ctx, box);
    DrawMemorySection(ctx, box);
    DrawBatterySection(ctx, box);
    DrawNetworkSection(ctx, box);
    ui_box_end(box);
    y += style.margin_t + ui_box_height(ctx, "main") + style.margin_b;

    {
        UiRect refresh_r = { style.margin_l, y, ui_button_width(ctx, "Refresh"), ROW_H };

        if (ui_button(ctx, refresh_r, "Refresh"))
            UpdateAll();
    }

    (void) win_h;
    return 0;
}

static long
now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
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
    int win_w = 280, win_h = 260; /* Network/Battery skurczone do 1 wiersza (SSID w naglowku, IP/AC usuniete) */
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i;
    int running, redraw;
    long next_refresh_ms;
    XEvent ev;

    /* -geometry/-geom jak w examples/7aweather.c - musi byc wychwycone
     * PRZED odczytaniem ewentualnego argumentu z nazwa interfejsu, zeby
     * jego dwa tokeny (flaga + wartosc) nie zostaly wziete za iface. */
    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
            continue;
        } else if (argv[i][0] != '-') {
            g_iface = argv[i];
        }
    }

    signal(SIGCHLD, SIG_IGN); /* SpawnDetached nie robi wait() na komendzie SMT */

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "brak polaczenia z X11 (sprawdz $DISPLAY)\n");
        return 1;
    }

    /* Bez tego XrmGetResource w ReadAppString nizej potrafi zwrocic
     * poprawna wartosc, ale z type == NULL (patrz ten sam komentarz w
     * examples/7atodo.c - zaobserwowane na OpenBSD). */
    XrmInitialize();
    ReadAppString(dpy, "7aSensors.smtOnCommand", "7aSensors.SmtOnCommand",
                  g_smt_on_cmd, sizeof(g_smt_on_cmd), "doas sysctl hw.smt=1");
    ReadAppString(dpy, "7aSensors.smtOffCommand", "7aSensors.SmtOffCommand",
                  g_smt_off_cmd, sizeof(g_smt_off_cmd), "doas sysctl hw.smt=0");
    ReadAppString(dpy, "7aSensors.ledOn", "7aSensors.LedOn",
                  g_led_on_color_name, sizeof(g_led_on_color_name), "green");
    ReadAppString(dpy, "7aSensors.ledOff", "7aSensors.LedOff",
                  g_led_off_color_name, sizeof(g_led_off_color_name), "gray50");
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
    XStoreName(dpy, win, "7aSensors");
    XSetIconName(dpy, win, "7aSensors");

    icon = MakeGaugeIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    /* min != max na OBU osiach - patrz ten sam komentarz w
     * examples/7aweather.c (i oryginalny XtNminWidth/XtNmaxWidth w
     * ../7asensors/7asensors.c). */
    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = 260; /* Network/Battery skurczone do 1 wiersza (SSID w naglowku, IP/AC usuniete) */
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "DejaVu Sans-9", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "ui_init nie powiodlo sie (brak fontu?)\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    ui_color(ctx, g_led_on_color_name, &g_led_on_color);
    ui_color(ctx, g_led_off_color_name, &g_led_off_color);

    /* Narysuj OD RAZU jedna klatke z placeholderami, ZANIM UpdateAll()
     * odpali vmstat/sysctl/ifconfig (popen+fread - lokalne, ale wciaz
     * fork+exec trzech osobnych procesow) - bez tego okno wisialoby
     * puste przez caly ten czas, bo zaden Expose nie jest jeszcze
     * obslugiwany. Ten sam mechanizm co w examples/7aweather.c (tam
     * bardziej odczuwalne, bo UpdateWeather() czeka na siec). */
    ui_begin_frame(ctx);
    draw(ctx, win_w, win_h);
    ui_end_frame(ctx);

    UpdateAll();

    running = 1;
    redraw = 1;  /* pokaz swieze dane od razu, bez czekania na kolejne zdarzenie */
    next_refresh_ms = now_ms() + REFRESH_INTERVAL_MS;

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

        if (redraw) {
            ui_begin_frame(ctx);
            if (draw(ctx, win_w, win_h)) running = 0;
            ui_end_frame(ctx);
            redraw = 0;
        }
        if (!running)
            break;

        long remaining = next_refresh_ms - now_ms();

        if (remaining <= 0) {
            UpdateAll();
            next_refresh_ms = now_ms() + REFRESH_INTERVAL_MS;
            redraw = 1;
            continue;
        }

        {
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

    XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &g_led_on_color);
    XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &g_led_off_color);
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
