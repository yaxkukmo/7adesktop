/*
 * 7amount.c - zamontowane systemy plikow + montowanie wymiennych dyskow
 * (pendrive, karty, CD) na OpenBSD, na bibliotece ui.c/ui.h z tego
 * katalogu.
 *
 * Nowa apka (nie port Xt/Xaw), TYLKO OpenBSD (sysctl hw.disknames,
 * DIOCGDINFO, getmntinfo) - na innych systemach kompiluje sie do
 * komunikatu bledu w main().
 *
 * Uklad: po lewej box z lista (naglowek kolumn Device/Mount point/Type/
 * Size jak w 7atube.c), po prawej kolumna: box informacyjny
 * (boxInfoBackground) z ikona dysku, pod nim szczegoly zaznaczonej
 * pozycji (bez boxa) i przycisk Mount/Unmount rowno z dolna linia boxa
 * listy; pod boxami wiersz "Mount point: [pole] [Disklabel]", na dole
 * linia statusu.
 * Klik w wiersz zaznacza, kolko myszy i scrollbar (strzalki + kropki
 * stron, wzorem 7atube.c) przewijaja liste.
 *
 * Lista (co SCAN_MS, bez forkow):
 *  - wszystko co pokazuje mount(8): getmntinfo(MNT_NOWAIT), zrodlo
 *    "<duid>.i" pokazywane jako sd0i;
 *  - pod spodem niezamontowane partycje wymiennych dyskow ze znanym
 *    systemem plikow: dyski z sysctl hw.disknames, partycje i typ z
 *    ioctl(DIOCGDINFO) na /dev/rsdNc (wystarczy grupa operator).
 * Pozycje systemowe (punkt montowania z /etc/fstab, "/" albo dowolna
 * partycja dysku, ktory ma wpis w fstab lub "/") nie maja przycisku
 * Unmount. Zaznaczenie trzymane po nazwie urzadzenia, wiec przezywa
 * odswiezenie listy.
 *
 * Montowanie: punkt z pola "Mount point" pod boxami (przy zmianie
 * zaznaczenia domyslnie ~/mnt/<partycja>, np. ~/mnt/sd1i). Istniejacy
 * katalog uzywany jak jest; nowy tworzony mkdir() jako uzytkownik (razem
 * z ~/mnt), bez doas - wlasciciel katalogu = uzytkownik, a
 * mount_msdos/mount_ntfs bez -u/-g przejmuja wlasciciela plikow
 * wlasnie z katalogu montowania, wiec FAT jest od razu zapisywalny bez
 * wolania mount_msdos wprost (mount(8) nie przyjmuje -u/-g). Zawsze
 * nodev,nosuid; ntfs/cd9660/udf tylko do odczytu. Odmontowanie:
 * "doas -n umount PUNKT" (katalog zostaje). Disklabel odpala
 * "7aMount.terminal -e doas disklabel -E sdN" (edytor interaktywny, nic
 * nie zapisuje bez 'w'; doas pyta o haslo w terminalu) podwojnym
 * forkiem, jak przegladarka w 7ascan.c.
 *
 * doas -n nigdy nie pyta o haslo (apka nie ma tty) - potrzebne reguly w
 * /etc/doas.conf, dopisane PO ogolnym "permit persist :wheel" (wygrywa
 * ostatnia pasujaca regula):
 *     permit nopass :wheel cmd mount
 *     permit nopass :wheel cmd umount
 * (CELOWO bez "cmd install" - doas install bez hasla to w praktyce root:
 * mozna nim polozyc dowolny plik z setuid; dlatego punkt montowania jest
 * w katalogu domowym, gdzie wystarcza zwykly mkdir.)
 * Brak reguly -> komunikat w linii statusu.
 *
 * mount/umount ida asynchronicznie (fork+execvp z argv, bez powloki,
 * stdout+stderr w potoku O_NONBLOCK czytanym w select() razem z fd X,
 * wzorem 7ascan.c) - umount pendrive'a po duzym zapisie potrafi trwac
 * dlugo, a okno ma w tym czasie reagowac. Naraz co najwyzej jedna
 * operacja.
 * Ostatnia linia wyjscia polecenia trafia do linii statusu.
 *
 * Ikona w boxie: utils/icons/hd_mount_48.xpm (wkompilowana, libXpm),
 * przezroczystosc wypieczona kolorem tla boxu informacyjnego, jak w
 * 7abubbles.c (ui_draw_pixmap nie zna maski).
 *
 * Wszystkie dane w stalych tablicach, bez malloc.
 *
 * Klawiatura: q/Esc = wyjscie (Esc w polu punktu montowania tylko zdejmuje
 * z niego fokus).
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __OpenBSD__
#include <fstab.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include "../ui.h"

#define ROW_H        22
#define COL_GAP       6
#define BOX_GAP       6        /* odstep miedzy boxem listy a info */
#define SCAN_MS    2000
#define MAX_DISKS     8
#define MAX_ENTRIES  48
#define MAX_ARGS     16
#define LINE_LEN    256
#define VISIBLE_ROWS  8
#define ICON_SIZE    48
#define SCROLLBAR_W  ROW_H     /* strzalki ROW_H x ROW_H, jak w 7atube.c */
#define SCROLL_DOT_PITCH 14
#define SCROLL_DOT_R  4

#ifdef __OpenBSD__

#include "icons/usb_48.xbm"
#include "icons/hd_mount_48.xpm"

typedef struct {
    char letter;               /* 'i' */
    int  fs;                   /* FS_* z disklabel.h */
} Part;

typedef struct {
    char name[8];              /* "sd1" */
    char duid[17];             /* "" gdy dysk nie ma DUID */
    int  system;
    int  nparts;
    Part parts[MAXPARTITIONS];
} Disk;

/* wiersz listy: zamontowany system plikow albo partycja do zamontowania */
typedef struct {
    char     dev[32];          /* "sd1i", "tmpfs", ... */
    char     mnt[MNAMELEN];    /* "" = niezamontowana */
    char     type[MFSNAMELEN];
    uint64_t total, used;      /* used tylko dla zamontowanych */
    int      fs;               /* FS_* dla niezamontowanych */
    int      disk;             /* indeks w g_disks, -1 = nie z dysku */
    int      system;
} Entry;

static Disk  g_disks[MAX_DISKS];
static int   g_ndisk;
static Entry g_ent[MAX_ENTRIES];
static int   g_nent;
static int   g_sel = -1;
static char  g_sel_dev[32];
static int   g_scroll;
/* punkt montowania z pola tekstowego; wypelniany domyslnie przy zmianie
 * zaznaczenia (g_mnt_for = dla ktorego urzadzenia) */
static char  g_mnt_buf[MNAMELEN];
static int   g_mnt_cur;
static char  g_mnt_for[32];
/* domyslny katalog na punkty montowania: $HOME/mnt (main) */
static char  g_mount_dir[MNAMELEN];
static UiRect g_list_r;        /* obszar wierszy - dla kolka myszy */

/* operacja w tle (mount/umount), co najwyzej jedna */
static pid_t g_job_pid = -1;
static int   g_job_fd  = -1;
static char  g_job_ok[LINE_LEN];     /* status po sukcesie */
static char  g_job_fail[64];         /* prefiks statusu po bledzie */
static char  g_job_cur[LINE_LEN];    /* biezaca (niedokonczona) linia */
static char  g_job_last[LINE_LEN];   /* ostatnia pelna linia wyjscia */

static char g_status[LINE_LEN] = "Ready";
static char g_terminal[LINE_LEN];

/* ikona boxu info; wlasciciel: main (FreeIcon przed XCloseDisplay) */
static Pixmap g_icon = None;

static UiBoxStyle g_list_style, g_info_style;
static int g_dev_w, g_type_w, g_size_w, g_info_w;

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                            */
/* ------------------------------------------------------------------ */

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
format_size(uint64_t bytes, char *out, size_t cap)
{
    const char *units = "KMGTP";
    double v = (double)bytes / 1024.0;
    int u = 0;

    while (v >= 1000.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, cap, "%.1f%c", v, units[u]);
}

/* "3.2G/14.9G" albo samo "14.9G" dla niezamontowanych */
static void
format_entry_size(const Entry *e, char *out, size_t cap)
{
    char used[16], total[16];

    format_size(e->total, total, sizeof(total));
    if (!e->mnt[0]) {
        snprintf(out, cap, "%s", total);
        return;
    }
    format_size(e->used, used, sizeof(used));
    snprintf(out, cap, "%s/%s", used, total);
}

/* Nazwa dysku z jadra, ale i tak sprawdzana, bo trafia do sciezek i argv
 * doas: male litery, potem cyfry ("sd1", "cd0"). */
static int
valid_disk_name(const char *s)
{
    const char *p = s;

    while (*p >= 'a' && *p <= 'z')
        p++;
    if (p == s || *p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    return *p == '\0' && p - s < (long)sizeof(g_disks[0].name);
}

/* Litera partycji dysku d w zrodle z fstab/getmntinfo ("/dev/sd1i",
 * "sd1i" albo "<duid>.i"), 0 gdy to nie ten dysk. */
static char
part_letter(const char *spec, const Disk *d)
{
    const char *p = spec;
    size_t n = strlen(d->name);

    if (strncmp(p, "/dev/", 5) == 0)
        p += 5;
    if (strncmp(p, d->name, n) == 0 && p[n] >= 'a' && p[n] <= 'p'
        && p[n + 1] == '\0')
        return p[n];
    if (d->duid[0] && strncmp(spec, d->duid, 16) == 0 && spec[16] == '.'
        && spec[17] >= 'a' && spec[17] <= 'p' && spec[18] == '\0')
        return spec[17];
    return 0;
}

/* Typ dla mount -t, NULL = nieobslugiwany (swap, raid, ...). */
static const char *
fs_mount_type(int fs)
{
    switch (fs) {
    case FS_BSDFFS:  return "ffs";
    case FS_MSDOS:   return "msdos";
    case FS_EXT2FS:  return "ext2fs";
    case FS_NTFS:    return "ntfs";
    case FS_ISO9660: return "cd9660";
    case FS_UDF:     return "udf";
    default:         return NULL;
    }
}

static int
fs_read_only(int fs)
{
    return fs == FS_NTFS || fs == FS_ISO9660 || fs == FS_UDF;
}

/* ------------------------------------------------------------------ */
/* Wykrywanie                                                            */
/* ------------------------------------------------------------------ */

/* Partycje z disklabel; bez nosnika (pusty czytnik, cd bez plyty)
 * nparts zostaje 0. Rozmiar partycji nie jest tu trzymany - liczy go
 * add_unmounted z tej samej etykiety. */
static int
read_label(const Disk *d, struct disklabel *dl)
{
    char path[32];
    int fd, ok;

    snprintf(path, sizeof(path), "/dev/r%sc", d->name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ok = ioctl(fd, DIOCGDINFO, dl) == 0;
    close(fd);
    return ok;
}

static int
in_fstab(const Disk *d)
{
    struct fstab *fs;
    int found = 0;

    if (setfsent()) {
        while (!found && (fs = getfsent()) != NULL)
            found = part_letter(fs->fs_spec, d) != 0;
        endfsent();
    }
    return found;
}

static int
mnt_in_fstab(const char *mnt)
{
    return strcmp(mnt, "/") == 0 || getfsfile(mnt) != NULL;
}

/* Jeden wpis "nazwa:duid" z hw.disknames do *d; 0 = pominac. */
static int
parse_disk_entry(char *entry, Disk *d)
{
    char *colon = strchr(entry, ':');

    memset(d, 0, sizeof(*d));
    if (colon)
        *colon = '\0';
    if (!valid_disk_name(entry))
        return 0;
    snprintf(d->name, sizeof(d->name), "%s", entry);
    if (colon && strlen(colon + 1) == 16)
        snprintf(d->duid, sizeof(d->duid), "%s", colon + 1);
    return 1;
}

static void
scan_disk_names(void)
{
    int mib[2] = { CTL_HW, HW_DISKNAMES };
    char names[1024], *entry, *next;
    size_t len = sizeof(names) - 1;

    g_ndisk = 0;
    if (sysctl(mib, 2, names, &len, NULL, 0) < 0) {
        snprintf(g_status, sizeof(g_status), "sysctl hw.disknames: %s",
                 strerror(errno));
        return;
    }
    names[len < sizeof(names) ? len : sizeof(names) - 1] = '\0';
    for (entry = names; entry && *entry && g_ndisk < MAX_DISKS; entry = next) {
        next = strchr(entry, ',');
        if (next)
            *next++ = '\0';
        if (parse_disk_entry(entry, &g_disks[g_ndisk])) {
            g_disks[g_ndisk].system = in_fstab(&g_disks[g_ndisk]);
            g_ndisk++;
        }
    }
}

static void
add_mounted(const struct statfs *m)
{
    Entry *e = &g_ent[g_nent++];
    char letter;
    int i;

    memset(e, 0, sizeof(*e));
    e->disk = -1;
    snprintf(e->mnt, sizeof(e->mnt), "%s", m->f_mntonname);
    snprintf(e->type, sizeof(e->type), "%s", m->f_fstypename);
    e->total = (uint64_t)m->f_blocks * m->f_bsize;
    e->used  = (uint64_t)(m->f_blocks - m->f_bfree) * m->f_bsize;
    for (i = 0; i < g_ndisk; i++) {
        letter = part_letter(m->f_mntfromname, &g_disks[i]);
        if (letter) {
            e->disk = i;
            snprintf(e->dev, sizeof(e->dev), "%s%c", g_disks[i].name, letter);
            if (strcmp(e->mnt, "/") == 0)
                g_disks[i].system = 1;
            break;
        }
    }
    if (e->disk < 0)
        snprintf(e->dev, sizeof(e->dev), "%s",
                 strncmp(m->f_mntfromname, "/dev/", 5) == 0
                 ? m->f_mntfromname + 5 : m->f_mntfromname);
}

static int
is_mounted(const char *dev)
{
    int i;

    for (i = 0; i < g_nent; i++)
        if (g_ent[i].mnt[0] && strcmp(g_ent[i].dev, dev) == 0)
            return 1;
    return 0;
}

static void
add_unmounted(int di, const struct disklabel *dl, int pi)
{
    const struct partition *pp = &dl->d_partitions[pi];
    Entry *e;
    char dev[32];

    snprintf(dev, sizeof(dev), "%s%c", g_disks[di].name, 'a' + pi);
    if (is_mounted(dev) || g_nent >= MAX_ENTRIES)
        return;
    e = &g_ent[g_nent++];
    memset(e, 0, sizeof(*e));
    snprintf(e->dev, sizeof(e->dev), "%s", dev);
    snprintf(e->type, sizeof(e->type), "%s", fs_mount_type(pp->p_fstype));
    e->fs    = pp->p_fstype;
    e->total = DL_GETPSIZE(pp) * dl->d_secsize;
    e->disk  = di;
}

/* Niezamontowane partycje wymiennego dysku; plyta CD/czesc nosnikow ma
 * system plikow tylko na partycji 'c', wtedy ona. */
static void
add_disk_parts(int di)
{
    struct disklabel dl;
    int i, n, any = 0;

    if (!read_label(&g_disks[di], &dl))
        return;
    n = dl.d_npartitions < MAXPARTITIONS ? dl.d_npartitions : MAXPARTITIONS;
    for (i = 0; i < n; i++) {
        if (i == RAW_PART || DL_GETPSIZE(&dl.d_partitions[i]) == 0
            || !fs_mount_type(dl.d_partitions[i].p_fstype))
            continue;
        add_unmounted(di, &dl, i);
        any = 1;
    }
    if (!any && n > RAW_PART && fs_mount_type(dl.d_partitions[RAW_PART].p_fstype))
        add_unmounted(di, &dl, RAW_PART);
}

static void
restore_selection(void)
{
    int i;

    g_sel = -1;
    for (i = 0; g_sel_dev[0] && i < g_nent; i++)
        if (strcmp(g_ent[i].dev, g_sel_dev) == 0)
            g_sel = i;
    if (g_sel < 0)
        g_sel_dev[0] = '\0';
}

static void
scan(void)
{
    struct statfs *mnts;
    int nmnt, i;

    scan_disk_names();
    g_nent = 0;
    nmnt = getmntinfo(&mnts, MNT_NOWAIT);
    for (i = 0; i < nmnt && g_nent < MAX_ENTRIES; i++)
        add_mounted(&mnts[i]);
    /* systemowosc po add_mounted - "/" ustawia g_disks[].system */
    for (i = 0; i < g_nent; i++)
        g_ent[i].system = mnt_in_fstab(g_ent[i].mnt)
            || (g_ent[i].disk >= 0 && g_disks[g_ent[i].disk].system);
    for (i = 0; i < g_ndisk; i++)
        if (!g_disks[i].system)
            add_disk_parts(i);
    restore_selection();
}

/* ------------------------------------------------------------------ */
/* Procesy                                                               */
/* ------------------------------------------------------------------ */

/* fork+execvp, stdout+stderr dziecka w potoku *out_fd, stdin z /dev/null
 * (doas -n i tak nie pyta). -1 = blad (opis w g_status). */
static pid_t
spawn(char *const argv[], int *out_fd)
{
    int fds[2], nul;
    pid_t pid;

    if (pipe(fds) < 0) {
        snprintf(g_status, sizeof(g_status), "pipe: %s", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(g_status, sizeof(g_status), "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    *out_fd = fds[0];
    return pid;
}

/* Dokleja kawalek wyjscia do biezacej linii; pelne niepuste linie
 * zapamietywane w g_job_last. */
static void
feed_output(const char *buf, ssize_t n)
{
    size_t cur = strlen(g_job_cur);
    ssize_t i;

    for (i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            if (cur > 0)
                snprintf(g_job_last, sizeof(g_job_last), "%s", g_job_cur);
            cur = 0;
        } else if (cur + 1 < sizeof(g_job_cur)) {
            g_job_cur[cur++] = buf[i];
        }
        g_job_cur[cur] = '\0';
    }
}

/* Komunikat bledu doas/polecenia do statusu, z podpowiedzia gdy brakuje
 * reguly nopass w doas.conf. */
static void
set_fail_status(const char *what)
{
    if (strstr(g_job_last, "Authentication required")
        || strstr(g_job_last, "Authorization") || strstr(g_job_last, "not permitted"))
        snprintf(g_status, sizeof(g_status),
                 "%s: add doas nopass rules (see 7amount.c header)", what);
    else
        snprintf(g_status, sizeof(g_status), "%s: %s", what,
                 g_job_last[0] ? g_job_last : "failed");
}

static int
exit_ok(int st)
{
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void
start_job(char *const argv[], const char *running, const char *ok, const char *fail)
{
    int fd;
    pid_t pid;

    g_job_cur[0] = g_job_last[0] = '\0';
    pid = spawn(argv, &fd);
    if (pid < 0)
        return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_job_pid = pid;
    g_job_fd  = fd;
    snprintf(g_job_ok, sizeof(g_job_ok), "%s", ok);
    snprintf(g_job_fail, sizeof(g_job_fail), "%s", fail);
    snprintf(g_status, sizeof(g_status), "%s", running);
}

static void
finish_job(void)
{
    int st = 0;

    feed_output("\n", 1);
    close(g_job_fd);
    g_job_fd = -1;
    while (waitpid(g_job_pid, &st, 0) < 0 && errno == EINTR)
        ;
    g_job_pid = -1;
    if (exit_ok(st))
        snprintf(g_status, sizeof(g_status), "%s", g_job_ok);
    else
        set_fail_status(g_job_fail);
}

/* Czyta wszystko co jest w potoku; 1 = operacja sie skonczyla. */
static int
read_job(void)
{
    char buf[256];
    ssize_t n;

    for (;;) {
        n = read(g_job_fd, buf, sizeof(buf));
        if (n > 0) {
            feed_output(buf, n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return 0;
        finish_job();
        return 1;
    }
}

/* Podwojny fork: program odlaczony od apki, bez zombie i bez kolizji z
 * waitpid operacji w tle. cmd dzielona spacjami, potem extra (NULL-
 * terminated, moze byc NULL); dir = katalog roboczy albo NULL. */
static void
launch(const char *cmd, char *const extra[], const char *dir)
{
    char buf[LINE_LEN], *argv[MAX_ARGS], *tok;
    int argc = 0;
    pid_t pid;

    snprintf(buf, sizeof(buf), "%s", cmd);
    for (tok = strtok(buf, " "); tok && argc < MAX_ARGS - 1; tok = strtok(NULL, " "))
        argv[argc++] = tok;
    while (extra && *extra && argc < MAX_ARGS - 1)
        argv[argc++] = *extra++;
    argv[argc] = NULL;
    if (argc == 0)
        return;

    pid = fork();
    if (pid < 0) {
        snprintf(g_status, sizeof(g_status), "fork: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        if (fork() == 0) {
            if (!dir || chdir(dir) == 0)
                execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

/* ------------------------------------------------------------------ */
/* Akcje                                                                 */
/* ------------------------------------------------------------------ */

/* Istniejacy katalog jest uzywany jak jest (wlasciciel bez zmian). Nowy:
 * mkdir jako uzytkownik, najpierw domyslny g_mount_dir (gdy go nie ma),
 * bez doas - patrz naglowek. */
static int
prepare_mount_point(const char *mnt)
{
    struct stat st;

    if (stat(mnt, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 1;
        snprintf(g_status, sizeof(g_status), "%s is not a directory", mnt);
        return 0;
    }
    if (mkdir(g_mount_dir, 0755) < 0 && errno != EEXIST) {
        snprintf(g_status, sizeof(g_status), "mkdir %s: %s", g_mount_dir, strerror(errno));
        return 0;
    }
    if (mkdir(mnt, 0755) < 0) {
        snprintf(g_status, sizeof(g_status), "mkdir %s: %s", mnt, strerror(errno));
        return 0;
    }
    return 1;
}

static void
do_mount(const Entry *e)
{
    char dev[48], mnt[MNAMELEN], ok[LINE_LEN], running[64], fail[64];
    const char *opts = fs_read_only(e->fs) ? "nodev,nosuid,ro" : "nodev,nosuid";
    size_t n;

    snprintf(dev, sizeof(dev), "/dev/%s", e->dev);
    snprintf(mnt, sizeof(mnt), "%s", g_mnt_buf);
    n = strlen(mnt);
    while (n > 1 && mnt[n - 1] == '/')
        mnt[--n] = '\0';
    if (mnt[0] != '/') {
        snprintf(g_status, sizeof(g_status), "Mount point must be an absolute path");
        return;
    }
    if (!prepare_mount_point(mnt))
        return;
    snprintf(running, sizeof(running), "Mounting %s...", e->dev);
    snprintf(ok, sizeof(ok), "%s mounted on %s", e->dev, mnt);
    snprintf(fail, sizeof(fail), "Mount %s", e->dev);
    {
        char *argv[] = { "doas", "-n", "mount", "-t",
                         (char *)fs_mount_type(e->fs), "-o", (char *)opts,
                         dev, mnt, NULL };
        start_job(argv, running, ok, fail);
    }
}

static void
do_unmount(const Entry *e)
{
    char mnt[MNAMELEN], ok[LINE_LEN], running[64], fail[64];

    snprintf(mnt, sizeof(mnt), "%s", e->mnt);
    snprintf(running, sizeof(running), "Unmounting %s...", e->dev);
    snprintf(ok, sizeof(ok), "%s unmounted", e->dev);
    snprintf(fail, sizeof(fail), "Unmount %s", e->dev);
    {
        char *argv[] = { "doas", "-n", "umount", mnt, NULL };
        start_job(argv, running, ok, fail);
    }
}

static void
do_disklabel(const Entry *e)
{
    char disk[16];
    char *extra[] = { "-e", "doas", "disklabel", "-E", disk, NULL };

    snprintf(disk, sizeof(disk), "%s", g_disks[e->disk].name);
    launch(g_terminal, extra, NULL);
}

static void
mount_or_unmount(const Entry *e)
{
    if (g_job_pid > 0)
        snprintf(g_status, sizeof(g_status), "Busy - wait for the current operation");
    else if (e->mnt[0])
        do_unmount(e);
    else
        do_mount(e);
}

/* ------------------------------------------------------------------ */
/* Ikona                                                                 */
/* ------------------------------------------------------------------ */

/* hd_mount_48_xpm -> Pixmapa z przezroczystoscia wypieczona tlem boxu
 * info (maska jako clip GC). Przy bledzie g_icon zostaje None. */
static void
load_icon(UiCtx *ctx, Display *dpy, Window win, unsigned depth)
{
    Pixmap src = None, mask = None;
    XpmAttributes attrs;
    GC gc;

    memset(&attrs, 0, sizeof(attrs));
    if (XpmCreatePixmapFromData(dpy, win, hd_mount_48_xpm, &src, &mask, &attrs) != XpmSuccess)
        return;
    if (attrs.width == ICON_SIZE && attrs.height == ICON_SIZE) {
        gc = XCreateGC(dpy, win, 0, NULL);
        g_icon = XCreatePixmap(dpy, win, ICON_SIZE, ICON_SIZE, depth);
        XSetForeground(dpy, gc, ui_theme_box_info_bg(ctx)->pixel);
        XFillRectangle(dpy, g_icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
        XSetClipMask(dpy, gc, mask);
        XCopyArea(dpy, src, g_icon, gc, 0, 0, ICON_SIZE, ICON_SIZE, 0, 0);
        XFreeGC(dpy, gc);
    }
    XFreePixmap(dpy, src);
    if (mask != None)
        XFreePixmap(dpy, mask);
    XpmFreeAttributes(&attrs);
}

/* ------------------------------------------------------------------ */
/* Rysowanie                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    UiRect dev, mnt, type, size;
} RowLayout;

static void
init_style(UiCtx *ctx)
{
    UiBoxStyle s = {0};
    int w;

    s.margin_t   = 4;
    s.margin_b   = 4;
    s.padding_l  = ui_box_padding(ctx);
    s.padding_r  = ui_box_padding(ctx);
    s.padding_t  = 2;
    s.padding_b  = 2;
    s.gap        = 2;
    s.border_w   = 1;
    s.border_color = *ui_theme_line_fg(ctx);

    g_list_style = s;
    g_list_style.margin_l = ui_window_margin(ctx);
    g_list_style.margin_r = BOX_GAP / 2;
    g_list_style.bg_color = *ui_theme_box_bg(ctx);

    g_info_style = s;
    g_info_style.margin_l = BOX_GAP - BOX_GAP / 2;
    g_info_style.margin_r = ui_window_margin(ctx);
    g_info_style.bg_color = *ui_theme_box_info_bg(ctx);

    g_dev_w  = ui_text_width(ctx, "mfs:99999") + 2 * UI_CELL_PAD;
    g_type_w = ui_text_width(ctx, "cd9660") + 2 * UI_CELL_PAD;
    g_size_w = ui_text_width(ctx, "999.9G/999.9G") + 2 * UI_CELL_PAD;

    w = ui_text_width(ctx, "999.9G/999.9G");
    if (ui_button_width(ctx, "Disklabel") > w)
        w = ui_button_width(ctx, "Disklabel");
    if (ui_button_width(ctx, "Unmount") > w)
        w = ui_button_width(ctx, "Unmount");
    g_info_w = w + 2 * ui_box_padding(ctx) + 2 + g_info_style.margin_l
             + g_info_style.margin_r;
}

static RowLayout
layout_row(UiRect row)
{
    RowLayout lo;
    int mnt_w = row.w - g_dev_w - g_type_w - g_size_w;

    if (mnt_w < 0)
        mnt_w = 0;
    lo.dev  = (UiRect) { row.x, row.y, g_dev_w, row.h };
    lo.mnt  = (UiRect) { lo.dev.x + lo.dev.w, row.y, mnt_w, row.h };
    lo.type = (UiRect) { lo.mnt.x + lo.mnt.w, row.y, g_type_w, row.h };
    lo.size = (UiRect) { lo.type.x + lo.type.w, row.y, g_size_w, row.h };
    return lo;
}

static UiRect
cell_text(UiRect c)
{
    c.x += UI_CELL_PAD;
    c.w -= 2 * UI_CELL_PAD;
    return c;
}

/* hdr = pierwszy wiersz boxa (pelna szerokosc), lo_w = szerokosc kolumn
 * (bez scrollbara); ostatnia komorka siega do ramki, takze nad
 * scrollbarem, jak w 7atube.c */
static void
draw_header(UiCtx *ctx, UiBox *box, UiRect hdr, int lo_w)
{
    UiRect strip = ui_box_header_strip(box, hdr);
    RowLayout lo;
    int right = strip.x + strip.w;

    hdr.w = lo_w;
    lo = layout_row(hdr);
    ui_header_cell(ctx, (UiRect) { strip.x, strip.y, lo.mnt.x - strip.x, strip.h },
                   lo.dev.x + UI_CELL_PAD, "Device");
    ui_header_cell(ctx, (UiRect) { lo.mnt.x - 1, strip.y, lo.mnt.w + 1, strip.h },
                   lo.mnt.x + UI_CELL_PAD, "Mount point");
    ui_header_cell(ctx, (UiRect) { lo.type.x - 1, strip.y, lo.type.w + 1, strip.h },
                   lo.type.x + UI_CELL_PAD, "Type");
    ui_header_cell(ctx, (UiRect) { lo.size.x - 1, strip.y, right - lo.size.x + 1, strip.h },
                   lo.size.x + UI_CELL_PAD, "Size");
}

static void
draw_entry(UiCtx *ctx, UiRect row, int idx, int mx, int my)
{
    const Entry *e = &g_ent[idx];
    RowLayout lo = layout_row(row);
    char size[32];

    if (ui_hit_test(ctx, row)) {
        g_sel = idx;
        snprintf(g_sel_dev, sizeof(g_sel_dev), "%s", e->dev);
    }
    if (idx == g_sel ||
        (mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h))
        ui_fill_round_rect(ctx, row, ui_theme_accent(ctx));
    format_entry_size(e, size, sizeof(size));
    ui_label_ellipsis(ctx, cell_text(lo.dev), e->dev);
    ui_label_ellipsis(ctx, cell_text(lo.mnt), e->mnt[0] ? e->mnt : "-");
    ui_label_ellipsis(ctx, cell_text(lo.type), e->type);
    ui_label_ellipsis(ctx, cell_text(lo.size), size);
}

static int
max_scroll(void)
{
    return g_nent > VISIBLE_ROWS ? g_nent - VISIBLE_ROWS : 0;
}

static void
scroll_list(int delta)
{
    g_scroll += delta;
    if (g_scroll > max_scroll())
        g_scroll = max_scroll();
    if (g_scroll < 0)
        g_scroll = 0;
}

/* Kropki stron na torze scrollbara - przeniesione z DrawScrollDots w
 * utils/7atube.c. Strona = VISIBLE_ROWS wierszy; ostatnia kropka =
 * koniec listy. */
static int
dots_current(int pages)
{
    if (g_scroll >= max_scroll())
        return pages - 1;
    return (g_scroll + VISIBLE_ROWS / 2) / VISIBLE_ROWS;
}

static int
dots_start(int visible, int pages, int current)
{
    int start = current - visible / 2;

    if (start > pages - visible)
        start = pages - visible;
    if (start < 0)
        start = 0;
    return start;
}

static void
draw_scroll_dots(UiCtx *ctx, UiRect track)
{
    int pages = 1 + (max_scroll() + VISIBLE_ROWS - 1) / VISIBLE_ROWS;
    int visible = track.h / SCROLL_DOT_PITCH;
    int current, start, y0, i;

    if (visible < 1)
        return;
    if (visible > pages)
        visible = pages;
    y0 = track.y + (track.h - visible * SCROLL_DOT_PITCH) / 2;

    start = dots_start(visible, pages, dots_current(pages));
    for (i = 0; i < visible; i++) {
        UiRect cell = { track.x, y0 + i * SCROLL_DOT_PITCH, track.w, SCROLL_DOT_PITCH };
        if (ui_hit_test(ctx, cell)) {
            g_scroll = (start + i) * VISIBLE_ROWS;
            scroll_list(0);
            break;
        }
    }

    current = dots_current(pages);
    start = dots_start(visible, pages, current);
    for (i = 0; i < visible; i++) {
        int cx = track.x + track.w / 2;
        int cy = y0 + i * SCROLL_DOT_PITCH + SCROLL_DOT_PITCH / 2;
        int more = (i == 0 && start > 0)
                || (i == visible - 1 && start + visible < pages);
        int r = more ? SCROLL_DOT_R / 2 : SCROLL_DOT_R;

        if (start + i == current)
            ui_fill_circle(ctx, cx, cy, r, ui_theme_fg(ctx));
        else
            ui_draw_circle(ctx, cx, cy, r, 1, ui_theme_fg(ctx));
    }
}

/* strzalka w gore / kropki / strzalka w dol; strzalki PO ui_button
 * (inaczej tlo przycisku je zakryje), g_scroll zmieniany PRZED
 * rysowaniem wierszy */
static void
draw_scrollbar(UiCtx *ctx, UiRect sb)
{
    UiRect up_r    = { sb.x, sb.y, sb.w, ROW_H };
    UiRect down_r  = { sb.x, sb.y + sb.h - ROW_H, sb.w, ROW_H };
    UiRect track_r = { sb.x, sb.y + ROW_H, sb.w, sb.h - 2 * ROW_H };
    int up = ui_button(ctx, up_r, "");
    int down = ui_button(ctx, down_r, "");

    ui_draw_arrow(ctx, up_r, UI_ARROW_UP);
    ui_draw_arrow(ctx, down_r, UI_ARROW_DOWN);
    scroll_list(up ? -1 : down ? 1 : 0);
    if (track_r.h > 0)
        draw_scroll_dots(ctx, track_r);
}

static void
draw_list(UiCtx *ctx, int w)
{
    UiBox *box;
    UiRect hdr, row;
    int i, mx, my, rows_w, list_h;

    ui_mouse_state(ctx, &mx, &my, NULL);
    box = ui_box_begin(ctx, "list", 0, 0, w, &g_list_style);
    hdr = ui_box_next_rect(box, ROW_H);
    rows_w = hdr.w - SCROLLBAR_W - COL_GAP;
    list_h = VISIBLE_ROWS * ROW_H + (VISIBLE_ROWS - 1) * g_list_style.gap;
    g_list_r = (UiRect) { hdr.x, hdr.y + ROW_H + g_list_style.gap, hdr.w, list_h };
    draw_header(ctx, box, hdr, rows_w);
    draw_scrollbar(ctx, (UiRect) { hdr.x + hdr.w - SCROLLBAR_W, g_list_r.y,
                                   SCROLLBAR_W, list_h });
    for (i = 0; i < VISIBLE_ROWS; i++) {
        row = ui_box_next_rect(box, ROW_H);
        row.w = rows_w;
        if (g_scroll + i < g_nent)
            draw_entry(ctx, row, g_scroll + i, mx, my);
    }
    ui_box_end(box);
}

/* y pod boxem o danym id, ktory zaczynal sie na y */
static int
below_box(UiCtx *ctx, const char *id, int y)
{
    return y + g_info_style.margin_t + ui_box_height(ctx, id) + g_info_style.margin_b;
}

/* prawa kolumna: box z ikona, pod nim szczegoly bez boxa, przycisk
 * Mount/Unmount dolna krawedzia rowno z dolna linia boxa listy */
static void
draw_info(UiCtx *ctx, int x, int w)
{
    UiBox *box;
    UiRect r;
    const Entry *e = g_sel >= 0 ? &g_ent[g_sel] : NULL;
    char size[32];
    int bold;

    box = ui_box_begin(ctx, "icon", x, 0, w, &g_info_style);
    r = ui_box_next_rect(box, ICON_SIZE + 4);
    if (g_icon != None)
        ui_draw_pixmap(ctx, (UiRect) { r.x + (r.w - ICON_SIZE) / 2, r.y + 2,
                                       ICON_SIZE, ICON_SIZE }, g_icon);
    ui_box_end(box);

    r = (UiRect) { x + g_info_style.margin_l, below_box(ctx, "icon", 0),
                   w - g_info_style.margin_l - g_info_style.margin_r, ROW_H };
    bold = ui_set_bold(ctx, 1);
    ui_label_centered(ctx, r, e ? e->dev : "No selection");
    ui_set_bold(ctx, bold);
    if (e) {
        format_entry_size(e, size, sizeof(size));
        r.y += ROW_H;
        ui_label_centered(ctx, r, e->system ? "system" : e->type);
        r.y += ROW_H;
        ui_label_centered(ctx, r, size);
    }

    r.y = g_list_style.margin_t + ui_box_height(ctx, "list") - ROW_H;
    if (e && (!e->mnt[0] || !e->system)
        && ui_button(ctx, r, e->mnt[0] ? "Unmount" : "Mount"))
        mount_or_unmount(e);
}

/* "Mount point: [pole] [Disklabel]" pod boxami (pole na polowe
 * dostepnej szerokosci); przy zmianie
 * zaznaczenia pole dostaje punkt montowania pozycji albo domyslny
 * g_mount_dir/<urzadzenie>. Zwraca y pod wierszem. */
static int
draw_mount_point(UiCtx *ctx, int x, int y, int w)
{
    const Entry *e = g_sel >= 0 ? &g_ent[g_sel] : NULL;
    const char *label = "Mount point:";
    int bold, lw, tw;

    if (e && strcmp(g_mnt_for, e->dev) != 0) {
        if (e->mnt[0])
            snprintf(g_mnt_buf, sizeof(g_mnt_buf), "%s", e->mnt);
        else
            snprintf(g_mnt_buf, sizeof(g_mnt_buf), "%s/%s", g_mount_dir, e->dev);
        g_mnt_cur = (int)strlen(g_mnt_buf);
        snprintf(g_mnt_for, sizeof(g_mnt_for), "%s", e->dev);
    }
    bold = ui_set_bold(ctx, 1);
    lw = ui_text_width(ctx, label);
    ui_label(ctx, (UiRect) { x, y, lw, ROW_H }, label);
    ui_set_bold(ctx, bold);
    tw = (w - lw - COL_GAP) / 2;
    if (tw > 0)
        ui_textbox(ctx, (UiRect) { x + lw + COL_GAP, y, tw, ROW_H },
                   g_mnt_buf, (int)sizeof(g_mnt_buf), &g_mnt_cur);
    if (e && e->disk >= 0
        && ui_button(ctx, (UiRect) { x + lw + COL_GAP + tw + 2 * COL_GAP, y,
                                     ui_button_width(ctx, "Disklabel"), ROW_H },
                     "Disklabel"))
        do_disklabel(e);
    return y + ROW_H;
}

static void
draw(UiCtx *ctx, int win_w)
{
    UiRect status;
    int m = ui_window_margin(ctx), list_w = win_w - g_info_w, h;

    draw_list(ctx, list_w);
    draw_info(ctx, list_w, g_info_w);
    h = below_box(ctx, "list", 0);
    h = draw_mount_point(ctx, m, h, win_w - 2 * m) + 4;
    status = (UiRect) { m, h, win_w - 2 * m, ROW_H };
    if (status.w > 0)
        ui_label_ellipsis(ctx, status, g_status);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

static void
frame(UiCtx *ctx, int win_w)
{
    ui_begin_frame(ctx);
    draw(ctx, win_w);
    ui_end_frame(ctx);
}

/* box listy: border + padding + naglowek i VISIBLE_ROWS wierszy z gap,
 * plus marginesy i linia statusu */
static int
window_height(void)
{
    return 4 + 2 + 4 + (VISIBLE_ROWS + 1) * ROW_H + VISIBLE_ROWS * 2 + 4
         + ROW_H + 4 + ROW_H + 4;
}

int
main(int argc, char **argv)
{
    Display *dpy;
    Window   win, root;
    GC       gc;
    UiCtx   *ctx;
    XEvent   ev;
    int      screen, win_w, win_h, win_x = 0, win_y = 0, min_w, min_h;
    int      geom_x = 0, geom_y = 0, geom_mask = 0, running = 1, redraw = 1, i;
    unsigned geom_w = 0, geom_h = 0;
    long     next_scan = 0;
    char     title[128]   = "7amount";
    char     app_name[64] = "7amount";

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-geometry") == 0 || strcmp(argv[i], "-geom") == 0)
            && i + 1 < argc) {
            geom_mask = XParseGeometry(argv[++i], &geom_x, &geom_y, &geom_w, &geom_h);
        } else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            snprintf(title, sizeof(title), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            snprintf(app_name, sizeof(app_name), "%s", argv[++i]);
        } else {
            fprintf(stderr, "Usage: 7amount [-geometry GEOM] [-title TEXT] [-name NAME]\n");
            return 1;
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "7amount: XOpenDisplay failed\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);
    {
        const char *home = getenv("HOME");
        snprintf(g_mount_dir, sizeof(g_mount_dir), "%s/mnt",
                 home && home[0] == '/' ? home : "/tmp");
    }
    ui_resource_string(dpy, "7aMount.terminal", "7aMount.Terminal",
                       g_terminal, sizeof(g_terminal), "xterm");

    win_w = 640;
    win_h = window_height();
    win = XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)win_w, (unsigned)win_h,
                              0, BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XStoreName(dpy, win, title);
    XSetIconName(dpy, win, title);
    {
        XClassHint *ch = XAllocClassHint();
        ch->res_name  = app_name;
        ch->res_class = "7amount";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    gc  = XCreateGC(dpy, win, 0, NULL);  /* zwalniany XFreeGC na koncu main */
    ctx = ui_init(dpy, win, gc, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1",
                  win_w, win_h);
    if (!ctx) {
        fprintf(stderr, "7amount: ui_init failed\n");
        XFreeGC(dpy, gc);
        XCloseDisplay(dpy);
        return 1;
    }
    init_style(ctx);
    load_icon(ctx, dpy, win, (unsigned)DefaultDepth(dpy, screen));

    min_w = 480;
    min_h = win_h;
    if (geom_mask & WidthValue)
        win_w = (int)geom_w < min_w ? min_w : (int)geom_w;
    if (geom_mask & XValue)
        win_x = (geom_mask & XNegative) ? DisplayWidth(dpy, screen) - win_w + geom_x : geom_x;
    if (geom_mask & YValue)
        win_y = (geom_mask & YNegative) ? DisplayHeight(dpy, screen) - win_h + geom_y : geom_y;
    XMoveResizeWindow(dpy, win, win_x, win_y, (unsigned)win_w, (unsigned)win_h);
    ui_resize(ctx, win_w, win_h);
    {
        /* wysokosc stala - liczba wierszy listy nie zalezy od okna */
        XSizeHints *sh = XAllocSizeHints();
        sh->flags = PMinSize | PMaxSize;
        if (geom_mask & (XValue | YValue))
            sh->flags |= USPosition;
        if (geom_mask & WidthValue)
            sh->flags |= USSize;
        sh->x = win_x;
        sh->y = win_y;
        sh->width  = win_w;
        sh->height = win_h;
        sh->min_width  = min_w;
        sh->min_height = min_h;
        sh->max_width  = 32000;
        sh->max_height = min_h;
        XSetWMNormalHints(dpy, win, sh);
        XFree(sh);
    }
    XMapWindow(dpy, win);
    ui_set_window_icon(ctx, usb_48_width, usb_48_height, usb_48_bits);

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            /* kolko myszy nad lista przechwycone PRZED ui_feed_event,
             * jak w 7atube.c */
            if (ev.type == ButtonPress
                && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
                if (ev.xbutton.x >= g_list_r.x && ev.xbutton.x < g_list_r.x + g_list_r.w
                    && ev.xbutton.y >= g_list_r.y && ev.xbutton.y < g_list_r.y + g_list_r.h)
                    scroll_list(ev.xbutton.button == Button4 ? -1 : 1);
                redraw = 1;
                continue;
            }
            if (ev.type == ButtonRelease
                && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5))
                continue;
            ui_feed_event(ctx, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0)
                    redraw = 1;
                break;
            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (ui_has_focus(ctx, g_mnt_buf)) {
                    if (sym == XK_Escape)
                        ui_set_focus(ctx, NULL);
                } else if (sym == XK_q || sym == XK_Escape) {
                    running = 0;
                }
                break;
            }
            case ButtonPress:
            case ButtonRelease:
            case MotionNotify:
                redraw = 1;
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
        if (!running)
            break;

        if (now_ms() >= next_scan) {
            scan();
            scroll_list(0);
            next_scan = now_ms() + SCAN_MS;
            redraw = 1;
        }
        if (redraw) {
            /* druga klatka pokazuje stan zmieniony klikiem w pierwszej
             * (zaznaczenie, status) - klik zerowany w ui_end_frame */
            frame(ctx, win_w);
            frame(ctx, win_w);
            redraw = 0;
        }

        {
            fd_set rfds;
            int xfd = ConnectionNumber(dpy), maxfd = xfd;
            long left = next_scan - now_ms();
            struct timeval tv;

            if (left < 0)
                left = 0;
            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            if (g_job_fd >= 0) {
                FD_SET(g_job_fd, &rfds);
                if (g_job_fd > maxfd)
                    maxfd = g_job_fd;
            }
            tv.tv_sec  = left / 1000;
            tv.tv_usec = (left % 1000) * 1000;
            if (select(maxfd + 1, &rfds, NULL, NULL, &tv) > 0
                && g_job_fd >= 0 && FD_ISSET(g_job_fd, &rfds)) {
                if (read_job())
                    next_scan = 0;   /* stan po mount/umount od razu */
                redraw = 1;
            }
        }
    }

    if (g_job_pid > 0) {
        close(g_job_fd);
        waitpid(g_job_pid, NULL, 0);
    }
    if (g_icon != None)
        XFreePixmap(dpy, g_icon);
    ui_destroy(ctx);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}

#else /* !__OpenBSD__ */

int
main(void)
{
    fprintf(stderr, "7amount: OpenBSD only\n");
    return 1;
}

#endif
