/*
 * 7atube.c - klient YouTube (odpowiednik pipe-viewer) na bibliotece
 * ui.c/ui.h z tego katalogu.
 *
 * Nowa apka (nie port Xt/Xaw - nie ma czego portowac, jak 7arss.c/
 * 7agroff.c). Apka NIE rozmawia z YouTube sama (zadnego API/JSON/HTML w
 * C) - cala wiedze o serwisie dzwiga zewnetrzny yt-dlp, a odtwarzanie mpv.
 * Tak samo robi pipe-viewer (odtwarzanie i tak oddaje do mpv+yt-dlp), a
 * zmiany po stronie YouTube naprawia aktualizacja yt-dlp, nie tej apki.
 * Zero nowych zaleznosci linkowania - tylko libui.a + X11, jak 7arss.
 *
 * Wyszukiwanie: fork+execvp "yt-dlp --flat-playlist --print SZABLON
 * ytsearchN:zapytanie" z wyjsciem w potoku, czytanym NIEBLOKUJACO przez
 * select() razem z deskryptorem polaczenia X (ten sam mechanizm co timer w
 * 7aweather.c/7arss.c) - yt-dlp potrzebuje zwykle kilku sekund, a popen +
 * fread jak w 7arss.c zamrozilby na ten czas cale okno. Bez powloki
 * (execvp), wiec zapytanie uzytkownika nie wymaga shell-quotowania.
 * Szablon --print to pola rozdzielone tabem (id/tytul/kanal/czas) - jedna
 * linia na wynik, parsowana w ParseResultLine bez parsera JSON.
 *
 * Akcje na wierszu wyniku:
 *   - Play:  "PLAYER URL" bez terminala (mpv otwiera wlasne okno) - wzorem
 *            RunDirect w utils/7agroff.c.
 *   - Audio: "PLAYER --no-video URL" W TERMINALU - mpv bez okna nie ma
 *            innego interfejsu sterowania (pauza/przewijanie) niz TTY.
 *   - Save:  "mkdir -p DIR && cd DIR && YTDLP URL" W TERMINALU, trzymanym
 *            otwartym do Enter zawsze (nie tylko przy bledzie, jak w
 *            7agroff.c) - zeby bylo widac, czy pobieranie sie skonczylo.
 *
 * Zasoby X (patrz Xresources.sample): 7aTube.player (domyslnie "mpv"),
 * 7aTube.ytdlp ("yt-dlp" - wieloslowne, np. z --cookies-from-browser),
 * 7aTube.terminal ("urxvt"), 7aTube.downloadDir ("~/Videos").
 *
 * Swiadomie BEZ miniatur (JPEG/WebP wymagalby libjpeg albo ImageMagick -
 * patrz dyskusja przy powstaniu apki), logowania, komentarzy i playlist.
 * Subskrypcje kanalow (RSS youtube.com/feeds/videos.xml) - kolejny krok.
 *
 * Lista wynikow: stala liczba widocznych wierszy + scroll kolkiem + znak
 * "v", wzorem utils/7agroff.c/7askm.c. Enter w polu wyszukiwania = Search
 * (ui_textbox_key). Brak skrotow jednoliterowych typu 'q' - pole tekstowe
 * jest tu glownym wejsciem, litera zamykalaby okno w trakcie pisania.
 *
 * Caly tekst WIDOCZNY w oknie jest po angielsku, jak w reszcie utils/
 * 7a*.c - komentarze zostaja po polsku.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "../ui.h"

#define ICON_SIZE 32
#define ROW_H 20
#define ARROW_W 20      /* waski slot na prawej krawedzi naglowka na znak "v" */
#define VISIBLE_ROWS 12 /* stala liczba widocznych wierszy - nadmiar przewijany kolkiem */
#define COL_GAP 6
#define MAX_CMD_TOKENS 24
#define MAX_RESULTS 50
#define SEARCH_COUNT 30 /* "ytsearch30:" - ile wynikow prosimy yt-dlp */
#define ID_LEN 16       /* id filmu YouTube ma 11 znakow */
#define TITLE_LEN 256
#define CHANNEL_LEN 96
#define DURATION_LEN 16
#define LINE_LEN 1024   /* jedna linia wyjscia yt-dlp; dluzsza jest przycinana */
#define QUERY_LEN 256
#define DIR_LEN 768

/* Te same wartosci, ktorych draw() uzywa do wypelnienia UiBoxStyle -
 * wydzielone, zeby ComputeContentHeight() nie wyszla z synchronizacji ze
 * stylem (ten sam wzorzec i nazwy co w utils/7agroff.c). */
#define BOX_BORDER_W 1
#define BOX_PAD_TB 6
#define BOX_MARGIN_TB 6
#define BOX_GAP 2

/* Separator pol to PRAWDZIWY znak tabulacji w tym stringu (C zamienia \t),
 * przekazany do yt-dlp jako jeden element argv - bez powloki, wiec nic go
 * nie interpretuje po drodze. "|..." to wartosc domyslna yt-dlp dla
 * pustego pola, "channel,uploader" - pierwsze niepuste z dwoch. */
#define PRINT_TEMPLATE "%(id)s\t%(title)s\t%(channel,uploader|?)s\t%(duration_string|-)s"

typedef struct {
    char id[ID_LEN];
    char title[TITLE_LEN];
    char channel[CHANNEL_LEN];
    char duration[DURATION_LEN];
} Video;

typedef struct {
    char player[128];   /* np. "mpv" */
    char ytdlp[128];    /* np. "yt-dlp" */
    char terminal[128]; /* np. "urxvt"; pusty = polecenie bez terminala */
    char download_dir[DIR_LEN];
} AppData;

static AppData app_data;

static Video g_results[MAX_RESULTS];
static int g_result_count = 0;
static int g_scroll = 0;
static UiRect g_list_r;
static char g_status[160] = "Type a query and press Enter";

static char g_query[QUERY_LEN] = "";
static int g_query_cursor = 0;

/* Trwajace wyszukiwanie: -1 = brak. g_line zbiera niepelna linie miedzy
 * kolejnymi read() - potok oddaje dane w dowolnych kawalkach. */
static pid_t g_search_pid = -1;
static int g_search_fd = -1;
static char g_line[LINE_LEN];
static size_t g_line_len = 0;

/* "~/Videos" -> "$HOME/Videos" - tylko prefiks "~/" albo samo "~", jak
 * ExpandHome w utils/7agroff.c. */
static void
ExpandHome(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, outsz, "%s%s", home, in + 1);
    else
        snprintf(out, outsz, "%s", in);
}

/* -------------------------------------------------------------------- */
/* Parsowanie wyjscia yt-dlp - niezalezne od ui.c                         */
/* -------------------------------------------------------------------- */

/* Id trafia do URL-a, ktory potem idzie m.in. przez "sh -c" (Audio/Save) -
 * akceptujemy tylko alfabet faktycznych id YouTube, zamiast ufac
 * wyjsciu zewnetrznego procesu. */
static int
ValidVideoId(const char *id)
{
    size_t len = strlen(id), i;

    if (len == 0 || len >= ID_LEN)
        return 0;
    for (i = 0; i < len; i++) {
        char c = id[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* "id\ttytul\tkanal\tczas" - id to PIERWSZE pole, czas OSTATNIE, kanal
 * przedostatnie, a tytul cala reszta posrodku (gdyby kiedys zawieral tab,
 * nie rozjedzie to pozostalych pol). Zmienia line w miejscu. */
static void
ParseResultLine(char *line)
{
    char *t1, *t3, *t2;
    Video *v;

    if (g_result_count >= MAX_RESULTS)
        return;
    t1 = strchr(line, '\t');
    t3 = strrchr(line, '\t');
    if (!t1 || t3 == t1)
        return;
    *t3 = '\0';
    t2 = strrchr(t1 + 1, '\t');
    if (!t2)
        return;
    *t1 = '\0';
    *t2 = '\0';
    if (!ValidVideoId(line))
        return;

    v = &g_results[g_result_count++];
    snprintf(v->id, sizeof(v->id), "%s", line);
    snprintf(v->title, sizeof(v->title), "%s", t1 + 1);
    snprintf(v->channel, sizeof(v->channel), "%s", t2 + 1);
    snprintf(v->duration, sizeof(v->duration), "%s", t3 + 1);
}

static void
VideoUrl(const Video *v, char *out, size_t outsz)
{
    snprintf(out, outsz, "https://www.youtube.com/watch?v=%s", v->id);
}

/* -------------------------------------------------------------------- */
/* Wyszukiwanie - proces yt-dlp z potokiem, czytany z petli zdarzen       */
/* -------------------------------------------------------------------- */

static void
AppendTokens(char *argv[], int *argc, int max, char *cmd)
{
    char *tok = strtok(cmd, " \t");

    while (tok && *argc < max - 1) {
        argv[(*argc)++] = tok;
        tok = strtok(NULL, " \t");
    }
}

static void
CancelSearch(void)
{
    if (g_search_fd >= 0) {
        close(g_search_fd);
        g_search_fd = -1;
    }
    if (g_search_pid > 0) {
        kill(g_search_pid, SIGTERM);
        g_search_pid = -1;
    }
    g_line_len = 0;
}

static void
StartSearch(const char *raw)
{
    static char ytdlp_buf[sizeof(app_data.ytdlp)];
    char search_arg[QUERY_LEN + 16];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    int fds[2];
    pid_t pid;

    while (*raw == ' ' || *raw == '\t')
        raw++;
    if (*raw == '\0') {
        snprintf(g_status, sizeof(g_status), "Enter a search query");
        return;
    }

    CancelSearch();
    g_result_count = 0;
    g_scroll = 0;

    snprintf(search_arg, sizeof(search_arg), "ytsearch%d:%s", SEARCH_COUNT, raw);
    snprintf(ytdlp_buf, sizeof(ytdlp_buf), "%s", app_data.ytdlp);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS - 5, ytdlp_buf);
    if (argc == 0) {
        snprintf(g_status, sizeof(g_status), "Error: empty 7aTube.ytdlp");
        return;
    }
    argv[argc++] = "--flat-playlist";
    argv[argc++] = "--print";
    argv[argc++] = PRINT_TEMPLATE;
    argv[argc++] = search_arg;
    argv[argc] = NULL;

    if (pipe(fds) != 0) {
        snprintf(g_status, sizeof(g_status), "Error: pipe (%s)", strerror(errno));
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        snprintf(g_status, sizeof(g_status), "Error: fork (%s)", strerror(errno));
        return;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(fds[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "7atube: could not run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); /* nie przeciekac do Play/Audio/Save */
    g_search_fd = fds[0];
    g_search_pid = pid;
    snprintf(g_status, sizeof(g_status), "Searching...");
}

static void
FinishSearch(void)
{
    /* proces juz sam sie konczy (EOF) - SIGCHLD = SIG_IGN sprzata go bez
     * wait(), wiec tylko zamykamy potok, bez kill(). */
    close(g_search_fd);
    g_search_fd = -1;
    g_search_pid = -1;
    g_line_len = 0;

    if (g_result_count == 0)
        snprintf(g_status, sizeof(g_status), "No results (is yt-dlp installed?)");
    else
        snprintf(g_status, sizeof(g_status), "%d results", g_result_count);
}

/* Wolane, gdy select() zglosi g_search_fd jako gotowy. Kompletne linie
 * trafiaja do ParseResultLine od razu, wiec wyniki pojawiaja sie w miare
 * naplywania; nadmiar ponad LINE_LEN-1 w jednej linii jest odrzucany. */
static void
PollSearch(void)
{
    char buf[4096];
    ssize_t n, i;

    n = read(g_search_fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        FinishSearch();
        return;
    }

    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            g_line[g_line_len] = '\0';
            ParseResultLine(g_line);
            g_line_len = 0;
        } else if (g_line_len < sizeof(g_line) - 1) {
            g_line[g_line_len++] = buf[i];
        }
    }
    snprintf(g_status, sizeof(g_status), "Searching... %d", g_result_count);
}

/* -------------------------------------------------------------------- */
/* Akcje na wyniku - Play bez terminala, Audio/Save w terminalu (patrz    */
/* naglowek pliku). Wzorzec AppendShellQuoted/RunInTerminal/RunDirect z   */
/* utils/7agroff.c.                                                    */
/* -------------------------------------------------------------------- */

static void
AppendShellQuoted(char *out, size_t outsz, const char *s)
{
    size_t len = strlen(out);
    char *p = out + len;
    size_t remaining = (outsz > len) ? outsz - len : 0;

    if (remaining < 3)
        return;
    *p++ = '\'';
    remaining--;
    for (; *s && remaining > 5; s++) {
        if (*s == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
            remaining -= 4;
        } else {
            *p++ = *s;
            remaining--;
        }
    }
    *p++ = '\'';
    *p = '\0';
}

/* Dopisuje do out tokeny cmd (np. "mpv --ytdl-format=best"), KAZDY osobno
 * shell-quoted i oddzielony spacja. */
static void
AppendQuotedTokens(char *out, size_t outsz, const char *cmd)
{
    char cmd_buf[256];
    char *tokens[MAX_CMD_TOKENS];
    int argc = 0, i;

    snprintf(cmd_buf, sizeof(cmd_buf), "%s", cmd);
    AppendTokens(tokens, &argc, MAX_CMD_TOKENS, cmd_buf);
    for (i = 0; i < argc; i++) {
        if (out[0] != '\0')
            strncat(out, " ", outsz - strlen(out) - 1);
        AppendShellQuoted(out, outsz, tokens[i]);
    }
}

/* inner_cmd juz zquotowane. hold_always: terminal czeka na Enter takze po
 * sukcesie (Save), inaczej tylko po bledzie (ten sam wzorzec co
 * SpawnCommand w utils/7atodo.c). */
static void
RunInTerminal(const char *inner_cmd, int hold_always)
{
    static char shell_cmd[2048];
    char terminal_buf[256];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;

    if (hold_always)
        snprintf(shell_cmd, sizeof(shell_cmd),
                 "%s; echo; echo '[7atube] finished - press Enter to close'; read -r _",
                 inner_cmd);
    else
        snprintf(shell_cmd, sizeof(shell_cmd),
                 "%s || { echo; echo '[7atube] command failed - check 7aTube.* resources;"
                 " press Enter to close'; read -r _; }", inner_cmd);

    snprintf(terminal_buf, sizeof(terminal_buf), "%s", app_data.terminal);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS - 4, terminal_buf);
    if (argc > 0)
        argv[argc++] = "-e";
    argv[argc++] = "sh";
    argv[argc++] = "-c";
    argv[argc++] = shell_cmd;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    /* SIGCHLD = SIG_IGN w main(), jadro sprzatnie proces potomny samo. */
}

static void
ActionPlay(const Video *v)
{
    char cmd_buf[256];
    char url[96];
    char *argv[MAX_CMD_TOKENS];
    int argc = 0;
    pid_t pid;

    VideoUrl(v, url, sizeof(url));
    snprintf(cmd_buf, sizeof(cmd_buf), "%s", app_data.player);
    AppendTokens(argv, &argc, MAX_CMD_TOKENS - 1, cmd_buf);
    if (argc == 0)
        return;
    argv[argc++] = url;
    argv[argc] = NULL;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "7atube: could not run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
}

static void
ActionAudio(const Video *v)
{
    char inner[768] = "";
    char url[96];

    VideoUrl(v, url, sizeof(url));
    AppendQuotedTokens(inner, sizeof(inner), app_data.player);
    strncat(inner, " --no-video ", sizeof(inner) - strlen(inner) - 1);
    AppendShellQuoted(inner, sizeof(inner), url);
    RunInTerminal(inner, 0);
}

static void
ActionSave(const Video *v)
{
    char inner[1536] = "";
    char quoted_dir[DIR_LEN + 8] = "";
    char url[96];
    size_t len;

    VideoUrl(v, url, sizeof(url));
    AppendShellQuoted(quoted_dir, sizeof(quoted_dir), app_data.download_dir);
    snprintf(inner, sizeof(inner), "mkdir -p %s && cd %s &&", quoted_dir, quoted_dir);
    /* AppendQuotedTokens dokleja spacje sam, bo inner nie jest pusty */
    AppendQuotedTokens(inner, sizeof(inner), app_data.ytdlp);
    len = strlen(inner);
    snprintf(inner + len, sizeof(inner) - len, " ");
    AppendShellQuoted(inner, sizeof(inner), url);
    RunInTerminal(inner, 1);
}

/* -------------------------------------------------------------------- */
/* Ikona okna - ekran z trojkatem "play", wprost Xlibem na 1-bitowej      */
/* Pixmapie, jak w reszcie utils/7a*.c.                                */
/* -------------------------------------------------------------------- */

static Pixmap
MakeTubeIconPixmap(Display *idpy, Window root)
{
    Pixmap icon = XCreatePixmap(idpy, root, ICON_SIZE, ICON_SIZE, 1);
    GC gc = XCreateGC(idpy, icon, 0, NULL);
    XPoint tri[3] = { { 12, 10 }, { 12, 22 }, { 22, 16 } };

    XSetForeground(idpy, gc, 0);
    XFillRectangle(idpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(idpy, gc, 1);
    XDrawRectangle(idpy, icon, gc, 2, 5, 27, 21);
    XFillPolygon(idpy, icon, gc, tri, 3, Convex, CoordModeOrigin);
    XFreeGC(idpy, gc);
    return icon;
}

/* -------------------------------------------------------------------- */
/* Warstwa UI                                                             */
/* -------------------------------------------------------------------- */

static int
BoxHeightForRows(int n_rows)
{
    return n_rows * ROW_H + (n_rows - 1) * BOX_GAP + 2 * BOX_BORDER_W + 2 * BOX_PAD_TB;
}

/* Box z jednym wierszem: "Search:" + pole tekstowe + przycisk "Search",
 * wzorem DrawNewFileBox w utils/7agroff.c. */
static int
DrawSearchBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y)
{
    static int label_w = 0, btn_w = 0;
    UiBox *box;
    UiRect row, l, m, r;
    int submit;

    if (label_w == 0) {
        label_w = ui_text_width(ctx, "Search:") + 4;
        btn_w = ui_button_width(ctx, "Search");
    }

    box = ui_box_begin(ctx, "search", 0, y, win_w, style);
    row = ui_box_next_rect(box, ROW_H);

    ui_rect_split3(row, label_w, btn_w, COL_GAP, &l, &m, &r);
    ui_label(ctx, l, "Search:");
    submit = ui_textbox_key(ctx, g_query, XK_Return) ||
             ui_textbox_key(ctx, g_query, XK_KP_Enter);
    ui_textbox(ctx, m, g_query, sizeof(g_query), &g_query_cursor);
    if (ui_button(ctx, r, "Search"))
        submit = 1;
    if (submit)
        StartSearch(g_query);

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "search") + style->margin_b;
}

/* Kolumny wiersza od prawej: Save | Audio | Play | czas | kanal, tytul
 * dostaje reszte. Szerokosci przyciskow/czasu liczone raz (font sie nie
 * zmienia w trakcie dzialania). */
typedef struct {
    UiRect title, channel, duration, play, audio, save;
} RowLayout;

static RowLayout
LayoutRow(UiCtx *ctx, UiRect row)
{
    static int play_w = 0, audio_w, save_w, dur_w;
    RowLayout lo;
    int x = row.x + row.w;
    int channel_w, title_w;

    if (play_w == 0) {
        play_w = ui_button_width(ctx, "Play");
        audio_w = ui_button_width(ctx, "Audio");
        save_w = ui_button_width(ctx, "Save");
        dur_w = ui_text_width(ctx, "00:00:00");
    }

    x -= save_w;  lo.save = (UiRect) { x, row.y, save_w, row.h };
    x -= COL_GAP + audio_w; lo.audio = (UiRect) { x, row.y, audio_w, row.h };
    x -= COL_GAP + play_w;  lo.play = (UiRect) { x, row.y, play_w, row.h };
    x -= COL_GAP + dur_w;   lo.duration = (UiRect) { x, row.y, dur_w, row.h };

    channel_w = (x - COL_GAP - row.x) / 4;
    if (channel_w < 0) channel_w = 0;
    x -= COL_GAP + channel_w;
    lo.channel = (UiRect) { x, row.y, channel_w, row.h };

    title_w = x - COL_GAP - row.x;
    if (title_w < 0) title_w = 0;
    lo.title = (UiRect) { row.x, row.y, title_w, row.h };
    return lo;
}

static int
DrawResultsBox(UiCtx *ctx, const UiBoxStyle *style, int win_w, int y, int mx, int my)
{
    UiBox *box;
    int max_scroll, i;

    max_scroll = g_result_count - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    box = ui_box_begin(ctx, "results", 0, y, win_w, style);

    {
        UiRect hdr = ui_box_next_rect(box, ROW_H);
        UiRect arrow_r = { hdr.x + hdr.w - ARROW_W, hdr.y, ARROW_W, hdr.h };
        RowLayout lo = LayoutRow(ctx, hdr);

        ui_label(ctx, lo.title, "Title");
        ui_label(ctx, lo.channel, "Channel");
        ui_label(ctx, lo.duration, "Time");
        if (g_scroll + VISIBLE_ROWS < g_result_count)
            ui_label_centered(ctx, arrow_r, "v");
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        UiRect row = ui_box_next_rect(box, ROW_H);
        int idx = g_scroll + i;
        RowLayout lo = LayoutRow(ctx, row);
        const Video *v;

        if (i == 0)
            g_list_r = (UiRect) { row.x, row.y, row.w,
                                  VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * style->gap };
        if (idx >= g_result_count)
            continue;

        v = &g_results[idx];
        if (mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h)
            ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
        ui_label_ellipsis(ctx, lo.title, v->title);
        ui_label_ellipsis(ctx, lo.channel, v->channel);
        ui_label(ctx, lo.duration, v->duration);
        if (ui_button(ctx, lo.play, "Play"))
            ActionPlay(v);
        if (ui_button(ctx, lo.audio, "Audio"))
            ActionAudio(v);
        if (ui_button(ctx, lo.save, "Save"))
            ActionSave(v);
    }

    ui_box_end(box);
    return y + style->margin_t + ui_box_height(ctx, "results") + style->margin_b;
}

static void
draw(UiCtx *ctx, int win_w)
{
    static UiBoxStyle style;
    static int ready = 0;
    int y, mx, my;

    if (!ready) {
        style = (UiBoxStyle) { 0 };
        style.margin_l = style.margin_r = ui_window_margin(ctx);
        style.margin_t = style.margin_b = BOX_MARGIN_TB;
        style.padding_l = style.padding_r = ui_box_padding(ctx);
        style.padding_t = style.padding_b = BOX_PAD_TB;
        style.border_w = BOX_BORDER_W;
        style.gap = BOX_GAP;
        style.border_color = *ui_theme_line_fg(ctx);
        style.bg_color = *ui_theme_box_bg(ctx);
        ready = 1;
    }

    ui_mouse_state(ctx, &mx, &my, NULL);

    y = DrawSearchBox(ctx, &style, win_w, 0);
    y = DrawResultsBox(ctx, &style, win_w, y, mx, my);

    {
        int wmargin = ui_window_margin(ctx);
        UiRect status_r = { wmargin, y, win_w - 2 * wmargin, ROW_H };

        ui_label_ellipsis(ctx, status_r, g_status);
    }
}

static int
ComputeContentHeight(void)
{
    int search_h = BOX_MARGIN_TB + BoxHeightForRows(1) + BOX_MARGIN_TB;
    int results_h = BOX_MARGIN_TB + BoxHeightForRows(1 + VISIBLE_ROWS) + BOX_MARGIN_TB;

    return search_h + results_h + ROW_H + BOX_MARGIN_TB;
}

static int
InRect(UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
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
    int win_w = 760, win_h;
    int win_x = 100, win_y = 100;
    int geom_x = 0, geom_y = 0, geom_mask = 0;
    unsigned int geom_w = 0, geom_h = 0;
    int i, running, redraw;
    char app_name[64] = "7aTube";
    char app_title[64] = "";
    char dir_raw[512];
    XEvent ev;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[i + 1], &geom_x, &geom_y, &geom_w, &geom_h);
            i++;
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(app_title, sizeof(app_title), "%s", argv[i + 1]);
            i++;
        }
    }

    win_h = ComputeContentHeight();

    /* yt-dlp/mpv/terminal sa fire-and-forget (albo konczone przez EOF
     * potoku, patrz FinishSearch) - jadro sprzata je bez wait(). */
    signal(SIGCHLD, SIG_IGN);

#ifdef __OpenBSD__
    /* rpath - baza zasobow X/fonty; proc exec - yt-dlp/mpv/terminal. Siec
     * i zapis na dysk robia WYLACZNIE procesy potomne po exec (pledge bez
     * execpromises ich nie ogranicza), wiec sama apka nie potrzebuje
     * inet/wpath/cpath. BEZ unveil - z tego samego powodu co 7arss.c
     * (dziedziczony po exec okaleczylby mpv/yt-dlp). */
    if (pledge("stdio rpath proc exec unix prot_exec", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7atube: brak polaczenia z X11\n");
        return 1;
    }

    ui_resource_string(dpy, "7aTube.player", "7aTube.Player", app_data.player, sizeof(app_data.player), "mpv");
    ui_resource_string(dpy, "7aTube.ytdlp", "7aTube.Ytdlp", app_data.ytdlp, sizeof(app_data.ytdlp), "yt-dlp");
    ui_resource_string(dpy, "7aTube.terminal", "7aTube.Terminal", app_data.terminal, sizeof(app_data.terminal), "urxvt");
    ui_resource_string(dpy, "7aTube.downloadDir", "7aTube.DownloadDir", dir_raw, sizeof(dir_raw), "~/Videos");
    ExpandHome(dir_raw, app_data.download_dir, sizeof(app_data.download_dir));

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
    XStoreName(dpy, win, app_title[0] ? app_title : app_name);
    XSetIconName(dpy, win, app_title[0] ? app_title : app_name);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7aTube";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    icon = MakeTubeIconPixmap(dpy, root);
    wmhints = XAllocWMHints();
    wmhints->flags = IconPixmapHint | IconMaskHint;
    wmhints->icon_pixmap = icon;
    wmhints->icon_mask = icon;
    XSetWMHints(dpy, win, wmhints);
    XFree(wmhints);

    /* Szerokosc swobodna (tytuly sa dowolnej dlugosci, ui_label_ellipsis
     * je przycina), min_height = dokladna wysokosc zawartosci - wzorem
     * utils/7agroff.c. */
    sizehints = XAllocSizeHints();
    sizehints->flags = PMinSize | PMaxSize;
    sizehints->min_width = 1;
    sizehints->min_height = ComputeContentHeight();
    sizehints->max_width = 32000;
    sizehints->max_height = 32000;
    if (geom_mask & (WidthValue | HeightValue)) {
        sizehints->flags |= USSize;
        sizehints->width = win_w;
        sizehints->height = win_h;
    }
    if (geom_mask & (XValue | YValue)) {
        sizehints->flags |= USPosition;
        sizehints->x = win_x;
        sizehints->y = win_y;
    }
    XSetWMNormalHints(dpy, win, sizehints);
    XFree(sizehints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1", win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7atube: ui_init nie powiodlo sie\n");
        XFreeGC(dpy, gc);
        XFreePixmap(dpy, icon);
        XCloseDisplay(dpy);
        return 1;
    }

    /* Dwie klatki PRZED petla glowna - zapelnienie cache wysokosci boxow,
     * patrz ten sam wzorzec w utils/7agroff.c/7aping.c. */
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);

    running = 1;
    redraw = 1;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            /* Kolko myszy (Button4/5) przechwycone PRZED ui_feed_event -
             * ten sam wzorzec co w utils/7askm.c: ui.c nie rozroznia
             * numeru przycisku, wiec kolko kliknelaby Play pod kursorem.
             * "continue" jest tu poprawne (wewnetrzna petla XPending,
             * redraw i tak wykonuje sie po jej wyjsciu). */
            if ((ev.type == ButtonPress || ev.type == ButtonRelease) &&
                (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.type == ButtonPress && InRect(g_list_r, ev.xbutton.x, ev.xbutton.y)) {
                    g_scroll += (ev.xbutton.button == Button4) ? -1 : 1;
                    redraw = 1;
                }
                continue;
            }

            ui_feed_event(ctx, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw = 1;
                break;
            case ButtonPress:
            case ButtonRelease:
            case MotionNotify:
                redraw = 1;
                break;
            case KeyPress: {
                /* tylko Escape - litery (np. 'q' z 7agroff.c) trafialyby
                 * tez do pola wyszukiwania, patrz naglowek pliku */
                KeySym ks = XLookupKeysym(&ev.xkey, 0);

                if (ks == XK_Escape) running = 0;
                redraw = 1;
                break;
            }
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
            draw(ctx, win_w);
            ui_end_frame(ctx);
            redraw = 0;
        }

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy);
            int maxfd = xfd;

            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            if (g_search_fd >= 0) {
                FD_SET(g_search_fd, &rfds);
                if (g_search_fd > maxfd) maxfd = g_search_fd;
            }
            if (select(maxfd + 1, &rfds, NULL, NULL, NULL) > 0 &&
                g_search_fd >= 0 && FD_ISSET(g_search_fd, &rfds)) {
                PollSearch();
                redraw = 1;
            }
        }
    }

    CancelSearch();
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, icon);
    XCloseDisplay(dpy);
    return 0;
}
