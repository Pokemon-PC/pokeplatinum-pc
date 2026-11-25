/* See memstream_compat.c. Force-included (-include) in outputs.c on Windows,
 * after <stdio.h> so the C library's own declarations stay untouched. */
#pragma once
#include <stdio.h>
FILE *pc_compat_open_memstream(char **buf, size_t *size);
int pc_compat_fclose(FILE *f);
#define open_memstream pc_compat_open_memstream
#define fclose pc_compat_fclose
