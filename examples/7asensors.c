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
 *
 * Linux: UpdateMemory/UpdateCPU/UpdateNetwork/UpdateBattery mialy
 * PIERWOTNIE tylko sciezke OpenBSD (vmstat w formacie kolumnowym OpenBSD,
 * sysctl hw.ncpu i hw.cpuspeed/hw.smt - te OID-y nie istnieja na Linuksie,
 * ifconfig w skladni "ieee80211: ... join <ssid>", komenda "apm") - na
 * Linuksie (w tym Slackware) kazda z tych komend albo nie istnieje, albo
 * zwraca inny format, wiec caly panel byl pusty. Kazda z czterech funkcji
 * ma teraz gala #ifdef __linux__ czytajaca /proc i /sys BEZPOSREDNIO
 * (bez popen) zamiast parsowac wyjscie komend - to dystrybucyjnie
 * neutralne (dziala tak samo na Slackware/Debianie/Arch, bez zaleznosci od
 * konkretnych narzedzi jak "free"/"lscpu") i tanie (mniej fork+exec co
 * REFRESH_INTERVAL_MS). Jedyny wyjatek to SSID Wi-Fi (UpdateNetwork) -
 * /proc nie ma tej informacji, wiec zostaje popen("iwgetid -r ...");
 * signal/quality natomiast czytany z /proc/net/wireless (poziom dBm,
 * przeliczany na % wzorem 2*(dBm+100), ten sam co uzywa NetworkManager -
 * bardziej przenosny miedzy sterownikami niz kolumna "link", ktorej max
 * bywa 70 albo 100 zaleznie od sterownika). SMT (/sys/devices/system/cpu/
 * smt/active) jest tylko odczytywany (jak hw.smt na OpenBSD); domyslne
 * komendy przelaczajace w ReadAppString to "pkexec sh -c 'echo on/off >
 * /sys/devices/system/cpu/smt/control'" - jak doas na OpenBSD, wymagaja
 * konfiguracji uprawnien u uzytkownika (polkit), inaczej klikniecie
 * przycisku po prostu nic nie zrobi (SpawnDetached nie sprawdza wyniku).
 */

#define _DEFAULT_SOURCE  /* popen/pclose sa POSIX, poza -std=c99 - patrz ta sama uwaga w examples/7aweather.c */

#include <ctype.h>
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
#include "../ui.h"

#define ICON_SIZE 32
#define REFRESH_INTERVAL_MS 5000   /* tyle co domyslne w oryginale (refreshInterval) */
#define ROW_H 20
#define DEFAULT_IFACE "iwm0"       /* jak w oryginale - dostosuj do wlasnej maszyny */

/* -------------------------------------------------------------------- */
/* Stan - wiersze tresci trzech sekcji, aktualizowane przy odswiezeniu  */
/* -------------------------------------------------------------------- */

static const char *g_iface = DEFAULT_IFACE;

/* g_iface trafia bez cudzyslowow do "ifconfig %s 2>/dev/null" w
 * UpdateNetwork, ktore idzie przez popen() czyli /bin/sh -c - argument z
 * CLI musi wiec byc ograniczony do znakow legalnych w nazwie interfejsu
 * sieciowego (litery/cyfry/kropka/podkreslnik/minus), inaczej np.
 * "7asensors ';rm -rf ~'" wykonalby dowolna komende. */
static int
IsValidIfaceName(const char *s)
{
    size_t i;

    if (!s || !s[0])
        return 0;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char) s[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-')
            return 0;
    }
    return 1;
}

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
static XColor g_led_on_color;
static XColor g_led_off_color;

/* Koleczko-wskaznik stanu przed przyciskiem (patrz DrawCpuSection) -
 * kolor tez konfigurowalny przez zasoby X (7aSensors.smtOnColor/
 * smtOffColor), nazwa koloru czytana do stringa przed ui_init (jak
 * smtOn/OffCommand wyzej), sama XColor alokowana raz w main PO
 * ui_init, bo dopiero wtedy istnieje Display/Visual/Colormap potrzebny
 * ui_color (patrz XColorAllocName w ui.c). */

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

#ifdef __linux__
/* Czyta caly plik /proc lub /sys do bufora - odpowiednik RunCommand, ale
 * bez fork+exec (te pliki sa generowane przez jadro na biezaco, popen
 * byloby tu zbednym kosztem). Niektore wezly /sys (np. wirtualna bateria
 * BAT0 w kontenerze/VM) potrafia zwrocic blad odczytu (ENODEV) mimo ze
 * fopen sie udaje - traktowane tak samo jak pusty plik (out[0]='\0'),
 * wolajacy i tak juz ma fallback na ten przypadek (patrz UpdateBattery). */
static int
ReadFileAll(const char *path, char *out, size_t outsize)
{
    FILE *fp;
    size_t n = 0;

    out[0] = '\0';
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    n = fread(out, 1, outsize - 1, fp);
    out[n] = '\0';
    fclose(fp);
    return 0;
}

/* Usuwa koncowe \n/\r - wyjscie iwgetid/wpa_cli (popen) ma je zwykle na
 * koncu, w odroznieniu od plikow /proc/sys czytanych przez ReadFileAll
 * powyzej (tam koncowa nowa linia i tak jest czescia liczby/pola parsowanego
 * przez sscanf/strtok, wiec nie przeszkadza). */
static void
TrimTrailingNewline(char *s)
{
    size_t l = strlen(s);

    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r'))
        s[--l] = '\0';
}

/* DEFAULT_IFACE ("iwm0") to nazwa sterownika OpenBSD - na Linuksie nigdy
 * nie pasuje (karty to zwykle wlan0/wlp*s0), wiec bez tej auto-detekcji
 * apka zawsze pokazywalaby puste dane dopoki uzytkownik recznie nie poda
 * "7asensors wlan0". Bierzemy PIERWSZY interfejs z /proc/net/wireless
 * (pomija 2 linie naglowka) - pokrywa typowy desktop z jedna karta WiFi
 * bez zadnego argumentu; wielokartowe maszyny nadal wymagaja jawnego CLI. */
static char g_auto_iface[16]; /* IFNAMSIZ na Linuksie to 16 */

static const char *
AutoDetectIface(void)
{
    char buf[1024];
    char *line, *saveptr;
    int lineno = 0;

    if (ReadFileAll("/proc/net/wireless", buf, sizeof(buf)) != 0)
        return NULL;

    line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        lineno++;
        if (lineno > 2) {
            char *p = line;
            char *colon;

            while (*p == ' ' || *p == '\t')
                p++;
            colon = strchr(p, ':');
            if (colon && colon > p) {
                size_t len = (size_t) (colon - p);

                if (len >= sizeof(g_auto_iface))
                    len = sizeof(g_auto_iface) - 1;
                memcpy(g_auto_iface, p, len);
                g_auto_iface[len] = '\0';
                return g_auto_iface;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return NULL;
}
#endif

/* Uzywane tylko przez OpenBSD-owe warianty Update* (sysctl "key=value" i
 * sufiksy K/M/G/T z vmstat) - na Linuksie zastapione przez ReadMeminfoField
 * (/proc/meminfo jest zawsze w kB) i bezposrednie parsowanie /proc/cpuinfo,
 * wiec te dwie funkcje musza zniknac z build-a Linux, inaczej -Wall zglosi
 * nieuzywana funkcje statyczna (a to w tym repo jest bledem do naprawienia,
 * patrz CLAUDE.md). */
#ifndef __linux__
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
#endif /* !__linux__ */

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

#ifdef __linux__
/* Szuka "KEY:" w /proc/meminfo (np. "MemTotal:") i zwraca wartosc w kB,
 * lub -1 gdy nieznaleziona. Wartosci w tym pliku sa zawsze w kB (jadro),
 * inaczej niz sysctl na OpenBSD gdzie ParseHumanKMGT musial rozpoznawac
 * sufiks K/M/G/T z wyjscia vmstat. */
static long
ReadMeminfoField(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    long val = -1;

    if (p)
        sscanf(p + strlen(key), "%ld", &val);
    return val;
}

static void
UpdateMemory(void)
{
    char buf[4096];
    long total_kb, avail_kb;

    ReadFileAll("/proc/meminfo", buf, sizeof(buf));
    total_kb = ReadMeminfoField(buf, "MemTotal:");
    avail_kb = ReadMeminfoField(buf, "MemAvailable:");

    {
        double total_bytes = total_kb > 0 ? (double) total_kb * 1024.0 : 0.0;
        /* MemAvailable (nie MemFree) to metryka jadra "ile faktycznie mozna
         * przydzielic bez swapowania" - liczy w cache/bufory odzyskiwalne,
         * MemFree zawyzalby zajecie o cache dyskowy. */
        double avail_bytes = avail_kb > 0 ? (double) avail_kb * 1024.0 : 0.0;
        double used_bytes = total_bytes - avail_bytes;
        char used_str[32], total_str[32];

        FormatHumanBytes(total_bytes, total_str, sizeof(total_str));
        FormatHumanBytes(used_bytes, used_str, sizeof(used_str));

        g_mem_frac = total_bytes > 0.0 ? used_bytes / total_bytes : 0.0;
        snprintf(g_mem_bar_label, sizeof(g_mem_bar_label), "%s / %s", used_str, total_str);
    }
}
#else
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
#endif

#ifdef __linux__
static void
UpdateCPU(void)
{
    char buf[16384]; /* /proc/cpuinfo - jeden blok ~1KB/rdzen, starcza z zapasem do ~16 rdzeni */
    char *line, *saveptr;
    int cores = 0;
    double mhz = -1.0;
    char smt[8];

    ReadFileAll("/proc/cpuinfo", buf, sizeof(buf));

    line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        if (strncmp(line, "processor", 9) == 0) {
            cores++;
        } else if (mhz < 0.0 && strncmp(line, "cpu MHz", 7) == 0) {
            const char *colon = strchr(line, ':');

            if (colon)
                mhz = strtod(colon + 1, NULL);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* /proc/cpuinfo wylicza TYLKO rdzenie aktualnie online (offline znikaja
     * z listy) - w odroznieniu od hw.ncpu/hw.ncpuonline na OpenBSD (ktore
     * rozroznia calkowita liczbe od online), tu total==online zawsze.
     * Wystarczajace dla zwyklego desktopu bez CPU hotplug. */
    g_cpu_cores_total = cores > 0 ? cores : -1;
    g_cpu_cores_online = g_cpu_cores_total;

    if (mhz >= 0.0)
        snprintf(g_cpu_speed_line, sizeof(g_cpu_speed_line), "Speed: %.0f MHz", mhz);
    else
        snprintf(g_cpu_speed_line, sizeof(g_cpu_speed_line), "Speed: ? MHz");

    /* /sys/devices/system/cpu/smt/active istnieje od jadra 4.19 - "1"/"0".
     * Odpowiednik hw.smt na OpenBSD, tylko do odczytu (przelaczanie idzie
     * przez skonfigurowana komende, patrz komentarz na gorze pliku). */
    if (ReadFileAll("/sys/devices/system/cpu/smt/active", smt, sizeof(smt)) == 0 && smt[0])
        g_smt_state = (smt[0] == '0') ? 0 : 1;
    else
        g_smt_state = -1;
}
#else
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
#endif

#ifdef __linux__
/* /proc/net/wireless (jadro, bez popen) format (nagrywek pomijamy):
 *   wlan0: 0000   45.  -65.  -256        0      0      0      0     26   0
 * pola po nazwie: status(hex) link. level. noise. dyskretne liczniki...
 * link/level/noise zawsze koncza sie kropka. Uzywamy level (dBm), nie
 * link (0-70 albo 0-100 zaleznie od sterownika - mniej przenosne). */
/* iwgetid (wireless-tools) i wpa_cli (wpa_supplicant) siedza czesto w
 * /usr/sbin albo /sbin - katalogach ktorych NIE ma w PATH powloki
 * uruchamiajacej apke z menu WM (zaobserwowane na Slackware: wpa_cli w
 * /usr/sbin, poza domyslnym PATH zwyklego uzytkownika). "PATH=...cmd" na
 * poczatku komendy DOKLEJA te katalogi do istniejacego PATH (nie
 * nadpisuje), wiec dziala niezaleznie od tego, gdzie dystrybucja je
 * zainstalowala. Kolejnosc prob: iwgetid najpierw (jedna linia, najlatwiej
 * sparsowac, obecny na wielu dystrybucjach z wireless-tools), potem
 * wpa_cli status (prawie zawsze obecny, bo dostarcza go pakiet
 * wpa_supplicant - a tego uzywa wiekszosc polaczen WPA, tez wtedy gdy
 * NetworkManager/inny manager akurat nie jest uruchomiony). */
static void
GetSsidLinux(const char *iface, char *out, size_t outsize)
{
    char cmd[192], buf[512];

    snprintf(cmd, sizeof(cmd),
             "PATH=\"$PATH:/usr/sbin:/sbin:/usr/local/sbin\" iwgetid -r %s 2>/dev/null",
             iface);
    RunCommand(cmd, buf, sizeof(buf));
    TrimTrailingNewline(buf);
    if (buf[0]) {
        snprintf(out, outsize, "%s", buf);
        return;
    }

    snprintf(cmd, sizeof(cmd),
             "PATH=\"$PATH:/usr/sbin:/sbin:/usr/local/sbin\" wpa_cli -i %s status 2>/dev/null",
             iface);
    RunCommand(cmd, buf, sizeof(buf));
    {
        /* Sentinel '\n' na poczatku, zeby "ssid=" na SAMYM poczatku wyjscia
         * (gdyby kiedys nie bylo linii przed nim) tez trafialo w "\nssid=" -
         * bez tego trzeba by osobno sprawdzac pozycje 0. */
        char tagged[514];
        char *p, *nl;

        tagged[0] = '\n';
        snprintf(tagged + 1, sizeof(tagged) - 1, "%s", buf);
        p = strstr(tagged, "\nssid=");
        if (p) {
            p += 6;
            nl = strchr(p, '\n');
            if (nl)
                *nl = '\0';
            if (p[0]) {
                snprintf(out, outsize, "%s", p);
                return;
            }
        }
    }

    snprintf(out, outsize, "-");
}

static void
UpdateNetwork(void)
{
    char ssid[64]; /* tyle co g_net_ssid, do ktorego trafia nizej */
    char wbuf[4096];
    char *line, *saveptr;
    double pct = -1.0;

    GetSsidLinux(g_iface, ssid, sizeof(ssid));
    snprintf(g_net_ssid, sizeof(g_net_ssid), "%s", ssid);

    ReadFileAll("/proc/net/wireless", wbuf, sizeof(wbuf));
    line = strtok_r(wbuf, "\n", &saveptr);
    while (line) {
        char ifname[64];
        double link, level;

        /* BEZ literalnej kropki po %lf - %lf sam ja konsumuje jako czesc
         * liczby (np. "45." to poprawny zapis liczby zmiennoprzecinkowej
         * bez czesci ulamkowej), wiec kropka w formacie nigdy by sie nie
         * dopasowala i sscanf konczylby z n==2 zamiast 3. */
        if (sscanf(line, " %63[^:]: %*x %lf %lf", ifname, &link, &level) == 3
            && strcmp(ifname, g_iface) == 0) {
            /* wzor NetworkManagera: dBm -> % w przyblizeniu liniowym */
            pct = 2.0 * (level + 100.0);
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            break;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (pct >= 0.0) {
        g_net_signal_frac = pct / 100.0;
        snprintf(g_net_signal_label, sizeof(g_net_signal_label), "%.0f%%", pct);
    } else {
        g_net_signal_frac = -1.0;
        snprintf(g_net_signal_label, sizeof(g_net_signal_label), "-");
    }
}
#else
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
#endif

#ifdef __linux__
/* /sys/class/power_supply/BAT0 (lub BAT1, jesli BAT0 nie istnieje/nie
 * odpowiada - zaobserwowane w praktyce: niektore maszyny wirtualne maja
 * BAT0 zwracajace ENODEV przy odczycie mimo ze plik istnieje, prawdziwa
 * bateria jest wtedy pod BAT1). "capacity" = 0-100, "status" = "Charging"/
 * "Discharging"/"Full"/"Not charging" - g_batt_on_battery tylko dla
 * "Discharging" (w odroznieniu od UpdateNetwork/apm gdzie kazdy stan poza
 * "connected" liczy sie jako "na baterii" - tu "Not charging"/"Full" na
 * AC nie powinny swiecic kropki). */
static void
UpdateBattery(void)
{
    static const char *bases[] = {
        "/sys/class/power_supply/BAT0",
        "/sys/class/power_supply/BAT1",
    };
    char capacity[8] = "", status[32] = "";
    size_t i;

    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        char path[80];

        snprintf(path, sizeof(path), "%s/capacity", bases[i]);
        if (ReadFileAll(path, capacity, sizeof(capacity)) == 0 && capacity[0]) {
            snprintf(path, sizeof(path), "%s/status", bases[i]);
            ReadFileAll(path, status, sizeof(status));
            break;
        }
        capacity[0] = '\0';
    }

    if (capacity[0]) {
        int pct = atoi(capacity);

        g_batt_frac = pct / 100.0;
        snprintf(g_batt_bar_label, sizeof(g_batt_bar_label), "%d%%", pct);
        g_batt_on_battery = strncmp(status, "Discharging", 11) == 0;
    } else {
        g_batt_frac = -1.0;
        snprintf(g_batt_bar_label, sizeof(g_batt_bar_label), "-");
        g_batt_on_battery = 0;
    }
}
#else
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
#endif

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
    int iface_from_cli = 0;
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
            if (IsValidIfaceName(argv[i])) {
                g_iface = argv[i];
                iface_from_cli = 1;
            } else {
                fprintf(stderr, "7asensors: ignoruje nieprawidlowa nazwe "
                        "interfejsu '%s'\n", argv[i]);
            }
        }
    }

#ifdef __linux__
    if (!iface_from_cli) {
        const char *detected = AutoDetectIface();

        if (detected)
            g_iface = detected;
    }
#endif

    signal(SIGCHLD, SIG_IGN); /* SpawnDetached nie robi wait() na komendzie SMT */

#ifdef __OpenBSD__
    /* Tylko pledge, bez unveil - jak w examples/7afm.c (patrz komentarz
     * tam): UpdateNetwork/UpdateCpu/UpdateMemory/UpdateBattery odpalaja
     * przez popen() stale komendy (ifconfig/sysctl/vmstat/apm), ale
     * SpawnDetached() (przelacznik SMT) wola DOWOLNA komende z zasobu X
     * 7aSensors.smtOnCmd/smtOffCmd (np. "doas sysctl hw.smt=1") - unveil
     * dziedziczony po exec by ja ograniczyl tak samo jak dowolny opener
     * w 7afm. */
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

    /* Bez tego XrmGetResource w ReadAppString nizej potrafi zwrocic
     * poprawna wartosc, ale z type == NULL (patrz ten sam komentarz w
     * examples/7atodo.c - zaobserwowane na OpenBSD). */
    XrmInitialize();
#ifdef __linux__
    ReadAppString(dpy, "7aSensors.smtOnCommand", "7aSensors.SmtOnCommand",
                  g_smt_on_cmd, sizeof(g_smt_on_cmd),
                  "pkexec sh -c 'echo on > /sys/devices/system/cpu/smt/control'");
    ReadAppString(dpy, "7aSensors.smtOffCommand", "7aSensors.SmtOffCommand",
                  g_smt_off_cmd, sizeof(g_smt_off_cmd),
                  "pkexec sh -c 'echo off > /sys/devices/system/cpu/smt/control'");
#else
    ReadAppString(dpy, "7aSensors.smtOnCommand", "7aSensors.SmtOnCommand",
                  g_smt_on_cmd, sizeof(g_smt_on_cmd), "doas sysctl hw.smt=1");
    ReadAppString(dpy, "7aSensors.smtOffCommand", "7aSensors.SmtOffCommand",
                  g_smt_off_cmd, sizeof(g_smt_off_cmd), "doas sysctl hw.smt=0");
#endif
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
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
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

    XFreeColors(dpy, DefaultColormap(dpy, screen), &g_led_on_color.pixel, 1, 0);
    XFreeColors(dpy, DefaultColormap(dpy, screen), &g_led_off_color.pixel, 1, 0);
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
