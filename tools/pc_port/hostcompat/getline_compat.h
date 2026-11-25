/* See memstream_compat.c. Force-included in tools/enumproc on Windows. */
#pragma once
#include <stdio.h>
long pc_compat_getline(char **line, size_t *cap, FILE *f);
#define getline pc_compat_getline
