/*
 * install_pc.c - where the game data lives, and first-run setup.
 *
 * The PC port never ships Nintendo data: the player provides their own
 * cartridge dump. Layout of a release folder (all next to the executable):
 *
 *   pokeplatinum.exe
 *
 *   config.ini            (written on first run)
 *   save.bin              (the 512 KiB backup memory, same format as the DS)
 *   <anything>.nds        (the player's Platinum ROM, only needed once)
 *   pc_data/<lang>/       (files extracted from that ROM on first run)
 *   mods/, mods/<lang>/   (optional: files that override pc_data)
 *
 * Base directory: the executable's folder when it holds pc_data/ or a ROM,
 * otherwise the source tree (developer builds, PPL_REPO_DIR).
 *
 * The extractor reads the ROM's NitroFS (FNT + FAT) exactly like
 * tools/pc_port/nitrofs.py. Language: 4th letter of the game code
 * (CPUF = French Platinum, CPUE = English...).
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define pc_mkdir(p) _mkdir(p)
#define SEP "\\"
#else
#include <dirent.h>
#define pc_mkdir(p) mkdir((p), 0755)
#define SEP "/"
#endif

#ifndef PPL_REPO_DIR
#define PPL_REPO_DIR "."
#endif

static char s_base[1024];

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/* Game code of a ROM file (4 chars at 0x0C), "" if unreadable. */
static void rom_game_code(const char *path, char code[5]) {
    code[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        if (fseek(f, 0x0C, SEEK_SET) == 0 && fread(code, 1, 4, f) == 4) {
            code[4] = '\0';
        } else {
            code[0] = '\0';
        }
        fclose(f);
    }
}

/* A *.nds file in dir (out: full path) whose header has this version's game
 * code (PC_GAME_CODE3*). File names are never trusted: the folder may hold
 * the ROMs of other games. */
static bool find_rom(const char *dir, char *out, size_t n) {
    bool found = false;
    char path[1100], code[5];
#if defined(_WIN32)
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s" SEP "*.nds", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        snprintf(path, sizeof path, "%s" SEP "%s", dir, fd.cFileName);
#else
    DIR *d = opendir(dir);
    if (d == NULL) {
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l <= 4 || (strcmp(e->d_name + l - 4, ".nds") != 0 && strcmp(e->d_name + l - 4, ".NDS") != 0)) {
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
#endif
        rom_game_code(path, code);
        if (memcmp(code, PC_GAME_CODE3, 3) == 0) {
            snprintf(out, n, "%s", path);
            found = true;
            break;
        }
#if defined(_WIN32)
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    return found;
}

static void exe_dir(char *out, size_t n) {
    char *b = SDL_GetBasePath();
    if (b == NULL) {
        snprintf(out, n, ".");
        return;
    }
    snprintf(out, n, "%s", b);
    SDL_free(b);
    size_t l = strlen(out);
    while (l > 1 && (out[l - 1] == '/' || out[l - 1] == '\\')) {
        out[--l] = '\0';
    }
}

const char *pc_install_base_dir(void) {
    if (s_base[0] != '\0') {
        return s_base;
    }
    char exe[1024], probe[1100], rom[1100];
    exe_dir(exe, sizeof exe);
    snprintf(probe, sizeof probe, "%s" SEP PC_DATA_DIR, exe);
    if (dir_exists(probe) || find_rom(exe, rom, sizeof rom)) {
        snprintf(s_base, sizeof s_base, "%s", exe);
    } else {
        snprintf(probe, sizeof probe, "%s/" PC_DATA_DIR, PPL_REPO_DIR);
        snprintf(s_base, sizeof s_base, "%s", dir_exists(probe) ? PPL_REPO_DIR : exe);
    }
    return s_base;
}

static const char *const k_langs[] = { "fr", "en", "de", "it", "es", "ja", "ko" };

static const char *lang_from_code(char c) {
    switch (c) {
    case 'F': return "fr";
    case 'E': return "en";
    case 'D': return "de";
    case 'I': return "it";
    case 'S': return "es";
    case 'J': return "ja";
    case 'K': return "ko";
    default:  return NULL;
    }
}

/* mkdir -p for the parent directories of path. */
static void make_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            pc_mkdir(path);
            *p = c;
        }
    }
}

typedef struct {
    const uint8_t *rom;
    size_t size;
    uint32_t fnt, fat;
    const char *out;
    int count;
    bool ok;
} Extract;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void walk(Extract *x, uint32_t dir_id, const char *prefix, int depth) {
    if (depth > 32 || !x->ok) {
        return;
    }
    const uint8_t *entry = x->rom + x->fnt + 8 * (dir_id & 0xFFF);
    uint32_t pos = x->fnt + rd32(entry);
    uint16_t fid = rd16(entry + 4);
    while (pos < x->size) {
        uint8_t t = x->rom[pos++];
        if (t == 0) {
            break;
        }
        int n = t & 0x7F;
        char name[260];
        memcpy(name, x->rom + pos, (size_t)n);
        name[n] = '\0';
        pos += (uint32_t)n;
        char path[1100];
        snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", name);
        if (t & 0x80) {
            uint16_t sub = rd16(x->rom + pos);
            pos += 2;
            walk(x, sub, path, depth + 1);
        } else {
            uint32_t start = rd32(x->rom + x->fat + 8u * fid), end = rd32(x->rom + x->fat + 8u * fid + 4);
            fid++;
            if (end < start || end > x->size) {
                x->ok = false;
                return;
            }
            char host[1400];
            snprintf(host, sizeof host, "%s/%s", x->out, path);
            make_parents(host);
            FILE *f = fopen(host, "wb");
            if (f == NULL || fwrite(x->rom + start, 1, end - start, f) != end - start) {
                x->ok = false;
            }
            if (f != NULL) {
                fclose(f);
            }
            x->count++;
        }
    }
}

static void message(const char *title, const char *text) {
    pc_warn("%s", text);
    if (getenv("PPL_EXIT_AFTER") == NULL) {   /* automated runs: no dialog to wait on */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text, NULL);
    }
}

/* Extracts rom_path into <base>/pc_data/<lang>. Returns the language. */
static const char *extract_rom(const char *base, const char *rom_path) {
    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x200 || size > 512L * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    uint8_t *rom = malloc((size_t)size);
    if (rom == NULL || fread(rom, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(rom);
        return NULL;
    }
    fclose(f);
    char code[5];
    memcpy(code, rom + 12, 4);
    code[4] = '\0';
    const char *lang = lang_from_code(code[3]);
    if (memcmp(code, PC_GAME_CODE3, 3) != 0 || lang == NULL) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "%s n'est pas une ROM de " PC_GAME_TITLE_FR " (code %s).\n"
                 "%s is not a " PC_GAME_TITLE_EN " ROM (game code %s).",
                 rom_path, code, rom_path, code);
        message(PC_GAME_TITLE, msg);
        free(rom);
        return NULL;
    }
    char out[1200];
    snprintf(out, sizeof out, "%s" SEP PC_DATA_DIR, base);
    pc_mkdir(out);
    snprintf(out, sizeof out, "%s" SEP PC_DATA_DIR SEP "%s", base, lang);
    pc_mkdir(out);
    pc_log("install: extracting %s (%s) to %s", rom_path, code, out);
    Extract x = { rom, (size_t)size, rd32(rom + 0x40), rd32(rom + 0x48), out, 0, true };
    walk(&x, 0xF000, "", 0);
    free(rom);
    if (!x.ok || x.count == 0) {
        message(PC_GAME_TITLE, "L'extraction de la ROM a \xc3\xa9\x63hou\xc3\xa9. / ROM extraction failed.");
        return NULL;
    }
    pc_log("install: %d files extracted", x.count);
    return lang;
}


/* pc_data/<lang> holds a whole extracted ROM (not just a language layer
 * built from the repository, like pc_data/en). */
static char s_rom_explicit[1100];

void pc_install_set_rom(const char *path) {
    snprintf(s_rom_explicit, sizeof s_rom_explicit, "%s", path);
}

/* The player's ROM, in this order: the explicit path (--rom / PPL_ROM, a
 * file or a folder), the game's folder, then config.ini [options] rom_dir. */
static bool locate_rom(const char *base, char *out, size_t n) {
    char code[5];
    if (s_rom_explicit[0] != '\0') {
        if (file_exists(s_rom_explicit)) {
            rom_game_code(s_rom_explicit, code);
            if (memcmp(code, PC_GAME_CODE3, 3) == 0) {
                snprintf(out, n, "%s", s_rom_explicit);
                return true;
            }
            pc_warn("install: %s is not a " PC_GAME_TITLE_EN " ROM (game code '%s')", s_rom_explicit, code);
        } else if (find_rom(s_rom_explicit, out, n)) {
            return true;
        }
    }
    if (find_rom(base, out, n)) {
        return true;
    }
    const PcConfig *cfg = pc_config();
    if (cfg->romDir[0] != '\0' && find_rom(cfg->romDir, out, n)) {
        return true;
    }
    return false;
}

static bool has_rom_data(const char *base, const char *lang) {
    char probe[1200];
    snprintf(probe, sizeof probe, "%s" SEP PC_DATA_DIR SEP "%s" SEP "poketool" SEP "personal" SEP "pl_personal.narc", base, lang);
    return file_exists(probe);
}

static bool any_rom_data(const char *base) {
    for (unsigned i = 0; i < sizeof k_langs / sizeof k_langs[0]; i++) {
        if (has_rom_data(base, k_langs[i])) {
            return true;
        }
    }
    return false;
}

const char *pc_install_prepare(const char *requested_lang) {
    const char *base = pc_install_base_dir();
    char probe[1200], rom[1100];
    if (requested_lang != NULL) {
        snprintf(probe, sizeof probe, "%s" SEP PC_DATA_DIR SEP "%s", base, requested_lang);
        if (dir_exists(probe) && any_rom_data(base)) {
            return requested_lang;           /* the ROM data fills in the rest */
        }
        if (dir_exists(probe) && locate_rom(base, rom, sizeof rom) && extract_rom(base, rom) != NULL) {
            return requested_lang;
        }
    } else {
        /* auto: the language of the player's ROM */
        for (unsigned i = 0; i < sizeof k_langs / sizeof k_langs[0]; i++) {
            if (has_rom_data(base, k_langs[i])) {
                return k_langs[i];
            }
        }
    }
    if (locate_rom(base, rom, sizeof rom)) {
        const char *lang = extract_rom(base, rom);
        if (lang != NULL && (requested_lang == NULL || strcmp(lang, requested_lang) == 0)) {
            return lang;
        }
        if (lang != NULL) {
            pc_warn("install: the ROM is in %s, not %s", lang, requested_lang);
            return lang;
        }
    }
    /* Developer tree: files/ alone (no ROM data) still boots far enough to
     * be useful for M1-M3 tests. */
    snprintf(probe, sizeof probe, "%s" SEP "files", base);
    if (dir_exists(probe) && requested_lang != NULL) {
        return requested_lang;
    }
    char msg[1600];
    snprintf(msg, sizeof msg,
             "Donn\xc3\xa9\x65s du jeu introuvables.\n"
             "Copiez la ROM de VOTRE cartouche " PC_GAME_TITLE_FR " (fichier .nds)\n"
             "dans ce dossier (ou indiquez son dossier dans config.ini, rom_dir),\n"
             "puis relancez le jeu :\n%s\n\n"
             "Game data not found. Copy the ROM dumped from YOUR OWN\n"
             "" PC_GAME_TITLE_EN " cartridge (.nds file) into this folder (or set\n"
             "its folder in config.ini, rom_dir) and restart.",
             base);
    message(PC_GAME_TITLE, msg);
    return NULL;
}

void pc_install_save_path(const char *name, char *out, size_t n) {
    if (name == NULL) {
        name = PC_SAVE_DEFAULT;
    }
    bool absolute = name[0] == '/' || name[0] == '\\' || (name[0] != '\0' && name[1] == ':');
    if (absolute || strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        snprintf(out, n, "%s", name);
    } else {
        char exe[1024];   /* next to the executable, even in a developer build */
        exe_dir(exe, sizeof exe);
        snprintf(out, n, "%s" SEP "%s", exe, name);
    }
}
