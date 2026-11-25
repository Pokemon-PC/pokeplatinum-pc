/*
 * fs_pc.c - NitroFS replacement.
 *
 * On the DS, FS_OpenFile(&f, "a/0/0/0") resolves a path inside the ROM's
 * file system. The Makefile builds that file system from the repository's
 * files/ directory (filesystem.mk: ROM path X == files/X), so on PC we simply
 * map every Nitro path onto <root>/X. Modders can therefore edit or replace
 * any file under files/ (or point --data at another directory, e.g. a tree
 * extracted from a ROM with `ndstool -x rom.nds -d <dir>`).
 *
 * The game's own NARC reader (src/filesystem.c) only needs open/seek/read,
 * which map 1:1 onto pc_fs_open/pc_fs_seek/pc_fs_read.
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#if defined(_WIN32)
#define PC_PATH_SEP '\\'
#else
#define PC_PATH_SEP '/'
#endif

#define PC_PATH_MAX 1024

/* Search path, highest priority first (e.g. mods/, pc_data/fr, files/).
 * A Nitro path resolves to the first layer that has it as a regular file,
 * so a mod only needs to contain the files it changes. */
#define PC_FS_MAX_ROOTS 12
static char s_roots[PC_FS_MAX_ROOTS][PC_PATH_MAX];
static int  s_root_count;

/* 64-bit file offsets: NARCs are small but the API must not care. */
#if defined(_WIN32)
#define pc_fseek _fseeki64
#define pc_ftell _ftelli64
#else
#define pc_fseek fseeko
#define pc_ftell ftello
#endif

bool pc_fs_add_root(const char *root_dir) {
    size_t n = strlen(root_dir);
    if (n == 0 || n >= PC_PATH_MAX - 2 || s_root_count == PC_FS_MAX_ROOTS) {
        return false;
    }
    char *r = s_roots[s_root_count];
    memcpy(r, root_dir, n + 1);
    while (n > 1 && (r[n - 1] == '/' || r[n - 1] == '\\')) {
        r[--n] = '\0';
    }
    s_root_count++;
    return true;
}

bool pc_fs_init(const char *root_dir) {
    s_root_count = 0;
    return pc_fs_add_root(root_dir);
}

void pc_fs_shutdown(void) {
    s_root_count = 0;
}

const char *pc_fs_root(void) {
    return s_root_count ? s_roots[0] : "";
}

int pc_fs_root_count(void) {
    return s_root_count;
}

const char *pc_fs_root_at(int i) {
    return (i >= 0 && i < s_root_count) ? s_roots[i] : NULL;
}

/* Builds the host path under one root; rejects absolute paths and ".." to
 * keep the game (and mods) inside the data roots. A leading '/' is
 * accepted as on NitroFS. */
static bool build_host_path(int root, const char *nitro_path, char *out, size_t out_size) {
    while (*nitro_path == '/') {
        nitro_path++;
    }
    if (*nitro_path == '\0' || strstr(nitro_path, "..") != NULL || strchr(nitro_path, ':') != NULL) {
        return false;
    }
    int w = snprintf(out, out_size, "%s/%s", s_roots[root], nitro_path);
    if (w < 0 || (size_t)w >= out_size) {
        return false;
    }
    for (char *p = out; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = PC_PATH_SEP;
        }
    }
    return true;
}

/* Only regular files count: in the source tree, a NARC that has not been
 * built yet is a *directory* of its sources, and Windows fopen() would
 * happily "open" it. */
static bool is_regular_file(const char *host) {
    struct stat st;
    return stat(host, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool pc_fs_setup_layers(const char *repo_dir, const char *lang) {
    static const char *const fmt[] = {
        "%s/" PC_MODS_DIR "/%s", /* language-specific mods  */
        "%s/" PC_MODS_DIR,       /* mods for every language */
        "%s/" PC_DATA_DIR "/%s", /* extracted / built data for the language */
        "%s/files",        /* repository sources (built in place)     */
    };
    char path[PC_PATH_MAX];
    s_root_count = 0;
    for (unsigned i = 0; i < sizeof fmt / sizeof fmt[0]; i++) {
        snprintf(path, sizeof path, fmt[i], repo_dir, lang);
        if (is_directory(path)) {
            pc_fs_add_root(path);
        }
    }
    /* Language layers built from the repository (pc_data/en) only hold what
     * differs by language: the rest of the game comes from the data of the
     * player's ROM, whatever its language. */
    static const char *const k_rom_langs[] = { "fr", "en", "de", "it", "es", "ja", "ko" };
    for (unsigned i = 0; i < sizeof k_rom_langs / sizeof k_rom_langs[0]; i++) {
        if (strcmp(k_rom_langs[i], lang) == 0) {
            continue;
        }
        snprintf(path, sizeof path, "%s/" PC_DATA_DIR "/%s", repo_dir, k_rom_langs[i]);
        if (is_directory(path)) {
            pc_fs_add_root(path);
        }
    }
    return s_root_count > 0;
}

/*
 * Localized ROMs keep some archives under resource/<lang>/<folder>/ instead
 * of the path the decomp (US ROM) uses: the French Platinum has
 * resource/fra/title/titledemo.narc where the code opens
 * demo/title/titledemo.narc. Each localized file keeps its file name, and
 * the game's own path table points there, so a missing path is looked up by
 * file name under resource/ (in the same layer order).
 */
static bool resolve_localized(const char *nitro_path, char *out, size_t out_size) {
    const char *base = strrchr(nitro_path, '/');
    base = base != NULL ? base + 1 : nitro_path;
    if (strncmp(nitro_path, "resource/", 9) == 0 || *base == '\0') {
        return false;
    }
    for (int i = 0; i < s_root_count; i++) {
        char res[PC_PATH_MAX];
        snprintf(res, sizeof res, "%s/resource", s_roots[i]);
        DIR *langs = opendir(res);
        if (langs == NULL) {
            continue;
        }
        struct dirent *l;
        bool found = false;
        while (!found && (l = readdir(langs)) != NULL) {
            if (l->d_name[0] == '.') {
                continue;
            }
            char lang_dir[PC_PATH_MAX];
            snprintf(lang_dir, sizeof lang_dir, "%s/%s", res, l->d_name);
            DIR *subs = opendir(lang_dir);
            if (subs == NULL) {
                continue;
            }
            struct dirent *s;
            while ((s = readdir(subs)) != NULL) {
                if (s->d_name[0] == '.') {
                    continue;
                }
                int w = snprintf(out, out_size, "%s/%s/%s", lang_dir, s->d_name, base);
                if (w > 0 && (size_t)w < out_size && is_regular_file(out)) {
                    found = true;
                    break;
                }
            }
            closedir(subs);
        }
        closedir(langs);
        if (found) {
            return true;
        }
    }
    return false;
}

/* First layer providing nitro_path as a regular file. */
static bool resolve(const char *nitro_path, char *out, size_t out_size) {
    for (int i = 0; i < s_root_count; i++) {
        if (build_host_path(i, nitro_path, out, out_size) && is_regular_file(out)) {
            return true;
        }
    }
    while (*nitro_path == '/') {
        nitro_path++;
    }
    return resolve_localized(nitro_path, out, out_size);
}

bool pc_fs_exists(const char *nitro_path) {
    char host[PC_PATH_MAX];
    return resolve(nitro_path, host, sizeof host);
}

bool pc_fs_open(PcFile *f, const char *nitro_path) {
    char host[PC_PATH_MAX];
    memset(f, 0, sizeof *f);
    if (!resolve(nitro_path, host, sizeof host)) {
        return false;
    }
    FILE *fp = fopen(host, "rb");
    if (fp == NULL) {
        return false;
    }
    if (pc_fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long long len = (long long)pc_ftell(fp);
    if (len < 0 || len > 0x7FFFFFFFLL) {
        fclose(fp);
        return false;
    }
    pc_fseek(fp, 0, SEEK_SET);
    f->handle = fp;
    f->start  = 0;
    f->length = (uint32_t)len;
    f->pos    = 0;
    return true;
}

void pc_fs_close(PcFile *f) {
    if (f->handle != NULL) {
        fclose((FILE *)f->handle);
    }
    memset(f, 0, sizeof *f);
}

int32_t pc_fs_read(PcFile *f, void *dst, int32_t len) {
    if (f->handle == NULL || len < 0) {
        return -1;
    }
    uint32_t avail = f->length - f->pos;
    if ((uint32_t)len > avail) {
        len = (int32_t)avail;
    }
    if (len == 0) {
        return 0;
    }
    FILE *fp = (FILE *)f->handle;
    if (pc_fseek(fp, (long long)f->start + f->pos, SEEK_SET) != 0) {
        return -1;
    }
    size_t got = fread(dst, 1, (size_t)len, fp);
    f->pos += (uint32_t)got;
    return (int32_t)got;
}

bool pc_fs_seek(PcFile *f, int32_t offset, PcSeek whence) {
    int64_t base;
    switch (whence) {
    case PC_SEEK_SET: base = 0; break;
    case PC_SEEK_CUR: base = f->pos; break;
    case PC_SEEK_END: base = f->length; break;
    default: return false;
    }
    int64_t target = base + offset;
    if (target < 0 || target > (int64_t)f->length) {
        return false;   /* NitroFS also refuses out-of-range seeks */
    }
    f->pos = (uint32_t)target;
    return true;
}

void *pc_fs_load(const char *nitro_path, uint32_t *out_size) {
    PcFile f;
    if (!pc_fs_open(&f, nitro_path)) {
        return NULL;
    }
    void *buf = pc_os_alloc(f.length ? f.length : 1, 32);
    if (buf != NULL && pc_fs_read(&f, buf, (int32_t)f.length) != (int32_t)f.length) {
        pc_os_free(buf);
        buf = NULL;
    }
    if (buf != NULL && out_size != NULL) {
        *out_size = f.length;
    }
    pc_fs_close(&f);
    return buf;
}

/* ------------------------------------------------------------------ NARC */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool pc_narc_open(PcNarc *n, const char *nitro_path) {
    uint8_t hdr[16], chunk[12];
    memset(n, 0, sizeof *n);
    if (!pc_fs_open(&n->file, nitro_path)) {
        return false;
    }
    if (pc_fs_read(&n->file, hdr, 16) != 16 || memcmp(hdr, "NARC", 4) != 0) {
        goto fail;
    }
    uint32_t btaf = rd16(hdr + 12);              /* header size */
    pc_fs_seek(&n->file, (int32_t)btaf, PC_SEEK_SET);
    if (pc_fs_read(&n->file, chunk, 12) != 12 || memcmp(chunk, "BTAF", 4) != 0) {
        goto fail;
    }
    uint32_t btaf_size = rd32(chunk + 4);
    n->count      = rd16(chunk + 8);
    n->fat_offset = btaf + 12;

    uint32_t btnf = btaf + btaf_size;
    pc_fs_seek(&n->file, (int32_t)btnf, PC_SEEK_SET);
    if (pc_fs_read(&n->file, chunk, 8) != 8 || memcmp(chunk, "BTNF", 4) != 0) {
        goto fail;
    }
    uint32_t gmif = btnf + rd32(chunk + 4);
    pc_fs_seek(&n->file, (int32_t)gmif, PC_SEEK_SET);
    if (pc_fs_read(&n->file, chunk, 8) != 8 || memcmp(chunk, "GMIF", 4) != 0) {
        goto fail;
    }
    n->img_offset = gmif + 8;
    return true;
fail:
    pc_fs_close(&n->file);
    return false;
}

void pc_narc_close(PcNarc *n) {
    pc_fs_close(&n->file);
}

bool pc_narc_member(PcNarc *n, uint32_t index, uint32_t *offset, uint32_t *size) {
    uint8_t e[8];
    if (index >= n->count) {
        return false;
    }
    pc_fs_seek(&n->file, (int32_t)(n->fat_offset + 8 * index), PC_SEEK_SET);
    if (pc_fs_read(&n->file, e, 8) != 8) {
        return false;
    }
    uint32_t start = rd32(e), end = rd32(e + 4);
    if (end < start || n->img_offset + end > n->file.length) {
        return false;
    }
    *offset = n->img_offset + start;
    *size   = end - start;
    return true;
}

void *pc_narc_load_member(PcNarc *n, uint32_t index, uint32_t *out_size) {
    uint32_t off, size;
    if (!pc_narc_member(n, index, &off, &size)) {
        return NULL;
    }
    void *buf = pc_os_alloc(size ? size : 1, 32);
    if (buf == NULL) {
        return NULL;
    }
    pc_fs_seek(&n->file, (int32_t)off, PC_SEEK_SET);
    if (pc_fs_read(&n->file, buf, (int32_t)size) != (int32_t)size) {
        pc_os_free(buf);
        return NULL;
    }
    if (out_size) {
        *out_size = size;
    }
    return buf;
}

/* ------------------------------------------------------- manifest check */

bool pc_fs_check_manifest(const char *manifest_path, PcFsReport *out, bool verbose) {
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(manifest_path, "r");
    if (fp == NULL) {
        return false;
    }
    char line[512];
    while (fgets(line, sizeof line, fp) != NULL) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (len == 0) {
            continue;
        }
        out->total++;
        if (pc_fs_exists(line)) {
            out->present++;
        } else {
            if (out->missing == 0) {
                snprintf(out->first_missing, sizeof out->first_missing, "%s", line);
            }
            out->missing++;
            if (verbose) {
                pc_log("missing: %s", line);
            }
        }
    }
    fclose(fp);
    return true;
}
