/*
 * open_memstream() for hosts without it (Windows / MinGW), used to build the
 * repository's data tools (tools/dataproc) for the PC port.
 *
 * The stream is a temporary file; pc_compat_fclose() copies its contents to
 * the caller's buffer before closing, which is what open_memstream promises
 * after fclose(). Only tools/dataproc/src/common/outputs.c uses it, and it is
 * compiled with -Dopen_memstream=pc_compat_open_memstream
 * -Dfclose=pc_compat_fclose (see cmake/PplHostTools.cmake).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef fclose

typedef struct {
    FILE *f;
    char **buf;
    size_t *size;
} MemStream;

static MemStream s_streams[64];

FILE *pc_compat_open_memstream(char **buf, size_t *size)
{
    for (int i = 0; i < 64; i++) {
        if (s_streams[i].f == NULL) {
            FILE *f = tmpfile();
            if (f == NULL) return NULL;
            s_streams[i].f = f;
            s_streams[i].buf = buf;
            s_streams[i].size = size;
            *buf = NULL;
            *size = 0;
            return f;
        }
    }
    return NULL;
}

int pc_compat_fclose(FILE *f)
{
    for (int i = 0; i < 64; i++) {
        if (s_streams[i].f == f) {
            long n;
            fflush(f);
            n = ftell(f);
            if (n < 0) n = 0;
            char *data = malloc((size_t)n + 1);
            if (data == NULL) return EOF;
            rewind(f);
            size_t got = fread(data, 1, (size_t)n, f);
            data[got] = 0;
            *s_streams[i].buf = data;
            *s_streams[i].size = got;
            s_streams[i].f = NULL;
            return fclose(f);
        }
    }
    return fclose(f);
}

/* getline() (POSIX), used by tools/enumproc. */
long pc_compat_getline(char **line, size_t *cap, FILE *f)
{
    size_t n = 0;
    int c;
    if (*line == NULL || *cap == 0) {
        *cap = 128;
        *line = malloc(*cap);
        if (*line == NULL) return -1;
    }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            char *p = realloc(*line, *cap * 2);
            if (p == NULL) return -1;
            *line = p;
            *cap *= 2;
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = 0;
    return (long)n;
}
