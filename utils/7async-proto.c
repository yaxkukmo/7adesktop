/*
 * 7async-proto.c - czytanie/pisanie protokolu 7async (specyfikacja w
 * 7async-proto.h). Bez malloc: wszystkie bufory podaje wywolujacy.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "7async-proto.h"

int
ProtoReadLine(FILE *f, char *buf, size_t size)
{
    size_t n = 0;
    int c;

    while ((c = getc(f)) != EOF) {
        if (c == '\n') {
            buf[n] = '\0';
            return 0;
        }
        if (c == '\0' || n + 1 >= size)
            return -1;
        buf[n++] = (char)c;
    }
    return -1;   /* EOF przed '\n' - urwany strumien */
}

int
ProtoParseInt(const char *s, long long min, long long max, long long *out)
{
    const char *p;
    char *end;
    long long v;

    if (*s == '\0')
        return -1;
    for (p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

int
ProtoValidUuid(const char *s)
{
    size_t n = 0;

    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-'))
            return 0;
    }
    return n >= 1 && n <= PROTO_UUID_MAX;
}

int
ProtoValidKind(const char *s)
{
    return strcmp(s, "todo") == 0 || strcmp(s, "entry") == 0;
}

/* Dzieli line na tokeny po pojedynczej spacji; puste tokeny = blad. */
static int
SplitTokens(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;

    for (;;) {
        char *sp = strchr(p, ' ');

        if (n == max || *p == '\0' || p == sp)
            return -1;
        tok[n++] = p;
        if (sp == NULL)
            return n;
        *sp = '\0';
        p = sp + 1;
    }
}

int
ProtoParseRecord(char *line, ProtoRecord *r)
{
    char *tok[6];
    long long v;

    if (SplitTokens(line, tok, 6) != 6 || strcmp(tok[0], "R") != 0)
        return -1;
    if (!ProtoValidUuid(tok[1]) || !ProtoValidKind(tok[2]))
        return -1;
    snprintf(r->uuid, sizeof r->uuid, "%s", tok[1]);
    snprintf(r->kind, sizeof r->kind, "%s", tok[2]);

    if (ProtoParseInt(tok[3], 1, 0x7fffffffffffffffLL, &r->updated_at) != 0)
        return -1;
    if (ProtoParseInt(tok[4], 0, 1, &v) != 0)
        return -1;
    r->deleted = (int)v;
    if (ProtoParseInt(tok[5], 0, PROTO_DATA_MAX, &v) != 0)
        return -1;
    r->len = (size_t)v;
    return 0;
}

int
ProtoReadData(FILE *f, const ProtoRecord *r, char *buf)
{
    if (r->len > PROTO_DATA_MAX)
        return -1;
    return fread(buf, 1, r->len, f) == r->len ? 0 : -1;
}

int
ProtoWriteRecord(FILE *f, const ProtoRecord *r, const char *data)
{
    if (fprintf(f, "R %s %s %lld %d %zu\n", r->uuid, r->kind,
                r->updated_at, r->deleted, r->len) < 0)
        return -1;
    return fwrite(data, 1, r->len, f) == r->len ? 0 : -1;
}

int
ProtoParseFields(const char *data, size_t len, ProtoField *out, int max)
{
    size_t i = 0;
    int n = 0;

    while (i < len) {
        size_t flen = 0, digits = 0;

        if (n == max)
            return -1;
        if (data[i] == '-') {
            out[n].p = NULL;
            out[n].len = 0;
            out[n].null = 1;
            n++;
            i++;
            continue;
        }
        while (i < len && data[i] >= '0' && data[i] <= '9') {
            flen = flen * 10 + (size_t)(data[i] - '0');
            if (++digits > 6)            /* > PROTO_DATA_MAX i tak */
                return -1;
            i++;
        }
        if (digits == 0 || i >= len || data[i] != ':')
            return -1;
        i++;
        if (flen > len - i)
            return -1;
        out[n].p = data + i;
        out[n].len = flen;
        out[n].null = 0;
        n++;
        i += flen;
    }
    return n;
}

int
ProtoAppendField(char *buf, size_t *used, size_t cap,
                 const char *p, size_t len, int null)
{
    char head[24];
    int hn;

    if (null) {
        if (cap - *used < 1)
            return -1;
        buf[(*used)++] = '-';
        return 0;
    }
    hn = snprintf(head, sizeof head, "%zu:", len);
    if (hn < 0 || (size_t)hn >= sizeof head)
        return -1;
    if (cap - *used < (size_t)hn || cap - *used - (size_t)hn < len)
        return -1;
    memcpy(buf + *used, head, (size_t)hn);
    *used += (size_t)hn;
    if (len > 0)
        memcpy(buf + *used, p, len);
    *used += len;
    return 0;
}
