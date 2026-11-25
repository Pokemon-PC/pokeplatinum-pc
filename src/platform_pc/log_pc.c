#include "platform_pc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>

static void pc_vlog(const char *tag, const char *fmt, va_list ap) {
    Uint32 ms = SDL_GetTicks();
    fprintf(stderr, "[%5u.%03u %s] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000), tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void pc_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pc_vlog("ppl", fmt, ap);
    va_end(ap);
}

void pc_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pc_vlog("ppl:warn", fmt, ap);
    va_end(ap);
}

void pc_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ppl:fatal] %s\n", buf);
    if (getenv("PPL_EXIT_AFTER") == NULL) {   /* scripted test runs must not block on a dialog */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, PC_GAME_TITLE, buf, NULL);
    }
    exit(1);
}
