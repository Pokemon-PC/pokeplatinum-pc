/*
 * fs_nitro_pc.c - NitroFS API (FS_*) on top of fs_pc.c.
 *
 * On the DS an "open" FSFile owns no resource: it is a file ID plus
 * top/bottom/pos offsets into the ROM, and game code routinely lets FSFile
 * structs on the stack go out of scope without FS_CloseFile(). We keep the
 * same model:
 *   - prop.file.own_id = 1 + index of the path in an interning table,
 *   - prop.file.top/bottom/pos = byte range and position, as on the DS,
 *     so the SDK inlines (FS_GetLength, FS_GetFileImageTop) work unchanged,
 *   - host FILE handles live in a small LRU cache keyed by path id and are
 *     reopened transparently when evicted.
 * Overlays are linked statically on PC; loading one resets its variables.
 */
#include "nitro_pc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATHS      1024   /* distinct files the ROM can contain: ~400 */
#define HANDLE_CACHE   16

static char  *s_paths[MAX_PATHS];
static u32    s_path_count;

typedef struct {
    u32      path_id;   /* 0 = empty */
    u32      last_use;
    PcFile   file;
} CachedHandle;

static CachedHandle s_cache[HANDLE_CACHE];
static u32    s_use_clock;
static BOOL   s_fs_ready;
static char   s_cwd[256];   /* FS_ChangeDir prefix, without trailing '/' */

static void overlay_init(void);

void FS_Init(u32 default_dma_no) {
    (void)default_dma_no;
    s_fs_ready = TRUE;
    overlay_init();
}

BOOL FS_IsAvailable(void) {
    return s_fs_ready;
}

u32 FS_SetDefaultDMA(u32 dma_no) {
    (void)dma_no;
    return FS_DMA_NOT_USE;
}

u32 FS_TryLoadTable(void *p_mem, u32 maxsize) {
    (void)p_mem;
    (void)maxsize;
    return 0;   /* no FAT/FNT cache needed: paths resolve on the host FS */
}

void FS_InitFile(FSFile *p_file) {
    memset(p_file, 0, sizeof *p_file);
    p_file->error = FS_RESULT_SUCCESS;
}

/* "dir//x" -> "dir/x", strips leading '/', applies FS_ChangeDir. */
static void build_path(char *out, size_t size, const char *path) {
    if (path[0] == '/' || s_cwd[0] == '\0') {
        snprintf(out, size, "%s", path[0] == '/' ? path + 1 : path);
    } else {
        snprintf(out, size, "%s/%s", s_cwd, path);
    }
}

BOOL FS_ChangeDir(const char *path) {
    char full[256];
    build_path(full, sizeof full, path);
    snprintf(s_cwd, sizeof s_cwd, "%s", full);
    size_t n = strlen(s_cwd);
    while (n > 0 && s_cwd[n - 1] == '/') {
        s_cwd[--n] = '\0';
    }
    return TRUE;
}

static u32 intern_path(const char *path) {
    for (u32 i = 0; i < s_path_count; i++) {
        if (strcmp(s_paths[i], path) == 0) {
            return i + 1;
        }
    }
    if (s_path_count == MAX_PATHS) {
        pc_fatal("FS: more than %d distinct files opened", MAX_PATHS);
    }
    size_t n = strlen(path) + 1;
    s_paths[s_path_count] = malloc(n);
    memcpy(s_paths[s_path_count], path, n);
    return ++s_path_count;
}

/* Returns an open host file for path_id, opening (and evicting) as needed. */
static PcFile *acquire(u32 path_id) {
    CachedHandle *victim = &s_cache[0];
    for (int i = 0; i < HANDLE_CACHE; i++) {
        CachedHandle *c = &s_cache[i];
        if (c->path_id == path_id) {
            c->last_use = ++s_use_clock;
            return &c->file;
        }
        if (c->path_id == 0 || c->last_use < victim->last_use) {
            victim = c;
        }
    }
    if (victim->path_id != 0) {
        pc_fs_close(&victim->file);
        victim->path_id = 0;
    }
    if (!pc_fs_open(&victim->file, s_paths[path_id - 1])) {
        return NULL;
    }
    victim->path_id = path_id;
    victim->last_use = ++s_use_clock;
    return &victim->file;
}

static BOOL valid(const FSFile *p_file) {
    return (p_file->stat & FS_FILE_STATUS_IS_FILE) && p_file->prop.file.own_id != 0 &&
           p_file->prop.file.own_id <= s_path_count;
}

BOOL FS_OpenFile(FSFile *p_file, const char *path) {
    char full[512];
    build_path(full, sizeof full, path);
    u32 id = intern_path(full);
    PcFile *f = acquire(id);
    if (f == NULL) {
        p_file->error = FS_RESULT_ERROR;
        return FALSE;
    }
    p_file->stat = FS_FILE_STATUS_IS_FILE;
    p_file->error = FS_RESULT_SUCCESS;
    p_file->prop.file.own_id = id;
    p_file->prop.file.top = 0;
    p_file->prop.file.bottom = f->length;
    p_file->prop.file.pos = 0;
    return TRUE;
}

BOOL FS_CloseFile(FSFile *p_file) {
    if (!valid(p_file)) {
        return FALSE;
    }
    p_file->stat = 0;
    p_file->prop.file.own_id = 0;
    return TRUE;
}

int FS_ReadFile(FSFile *p_file, void *dst, s32 len) {
    PcFile *f = valid(p_file) ? acquire(p_file->prop.file.own_id) : NULL;
    if (f == NULL || len < 0) {
        p_file->error = FS_RESULT_FAILURE;
        return -1;
    }
    u32 pos = p_file->prop.file.pos, bottom = p_file->prop.file.bottom;
    if ((u32)len > bottom - pos) {
        len = (s32)(bottom - pos);
    }
    if (!pc_fs_seek(f, (int32_t)pos, PC_SEEK_SET)) {
        p_file->error = FS_RESULT_ERROR;
        return -1;
    }
    int32_t got = pc_fs_read(f, dst, len);
    if (got < 0) {
        p_file->error = FS_RESULT_ERROR;
        return -1;
    }
    p_file->prop.file.pos = pos + (u32)got;
    p_file->error = FS_RESULT_SUCCESS;
    return got;
}

int FS_ReadFileAsync(FSFile *p_file, void *dst, s32 len) {
    return FS_ReadFile(p_file, dst, len);   /* completes immediately */
}

BOOL FS_WaitAsync(FSFile *p_file) {
    return p_file->error == FS_RESULT_SUCCESS;
}

/* Same rules as the SDK: the target is clamped to [top, bottom]. */
BOOL FS_SeekFile(FSFile *p_file, s32 offset, FSSeekFileMode origin) {
    if (!valid(p_file)) {
        return FALSE;
    }
    s64 base;
    switch (origin) {
    case FS_SEEK_SET: base = p_file->prop.file.top; break;
    case FS_SEEK_CUR: base = p_file->prop.file.pos; break;
    case FS_SEEK_END: base = p_file->prop.file.bottom; break;
    default: return FALSE;
    }
    s64 target = base + offset;
    if (target < (s64)p_file->prop.file.top) {
        target = p_file->prop.file.top;
    }
    if (target > (s64)p_file->prop.file.bottom) {
        target = p_file->prop.file.bottom;
    }
    p_file->prop.file.pos = (u32)target;
    return TRUE;
}

/* ------------------------------------------------------- file IDs */

/* Used by NitroSystem's sound archive (snd_arc.c): resolve once, reopen
 * cheaply later. The interned path index plays the role of the FAT index. */
BOOL FS_ConvertPathToFileID(FSFileID *p_file_id, const char *path) {
    char full[512];
    build_path(full, sizeof full, path);
    if (!pc_fs_exists(full)) {
        return FALSE;
    }
    p_file_id->arc = NULL;
    p_file_id->file_id = intern_path(full);
    return TRUE;
}

BOOL FS_OpenFileFast(FSFile *p_file, FSFileID file_id) {
    if (file_id.file_id == 0 || file_id.file_id > s_path_count) {
        return FALSE;
    }
    PcFile *f = acquire(file_id.file_id);
    if (f == NULL) {
        return FALSE;
    }
    p_file->stat = FS_FILE_STATUS_IS_FILE;
    p_file->error = FS_RESULT_SUCCESS;
    p_file->prop.file.own_id = file_id.file_id;
    p_file->prop.file.top = 0;
    p_file->prop.file.bottom = f->length;
    p_file->prop.file.pos = 0;
    return TRUE;
}

/* ------------------------------------------------- custom archives */

/* Only the Wi-Fi Connection setup screens mount a custom archive
 * ("rom:/dwc/utility.bin"); online services do not exist on PC. */
void FS_InitArchive(FSArchive *p_arc) { memset(p_arc, 0, sizeof *p_arc); }
BOOL FS_RegisterArchiveName(FSArchive *p_arc, const char *name, u32 name_len) { (void)p_arc; (void)name; (void)name_len; return FALSE; }
void FS_ReleaseArchiveName(FSArchive *p_arc) { (void)p_arc; }
BOOL FS_LoadArchive(FSArchive *p_arc, u32 base, u32 fat, u32 fat_size, u32 fnt, u32 fnt_size,
                    FS_ARCHIVE_READ_FUNC read_func, FS_ARCHIVE_WRITE_FUNC write_func) {
    (void)p_arc; (void)base; (void)fat; (void)fat_size; (void)fnt; (void)fnt_size; (void)read_func; (void)write_func;
    return FALSE;
}
BOOL FS_UnloadArchive(FSArchive *p_arc) { (void)p_arc; return TRUE; }
u32 FS_LoadArchiveTables(FSArchive *p_arc, void *p_mem, u32 max_size) { (void)p_arc; (void)p_mem; (void)max_size; return 0; }
void *FS_UnloadArchiveTables(FSArchive *p_arc) { (void)p_arc; return NULL; }
/* The DS boot code registers the cartridge's own file system as archive
 * "rom"; the game looks it up to refresh its FAT/FNT location
 * (src/system_card.c). On PC that archive is the host file system. */
static FSArchive s_rom_archive;

FSArchive *FS_FindArchive(const char *path, int offset) {
    if (offset == 3 && strncmp(path, "rom", 3) == 0) {
        return &s_rom_archive;
    }
    return NULL;
}
void FS_SetArchiveProc(FSArchive *p_arc, FS_ARCHIVE_PROC_FUNC proc, u32 flags) { (void)p_arc; (void)proc; (void)flags; }
void FS_NotifyArchiveAsyncEnd(FSArchive *p_arc, FSResult ret) { (void)p_arc; (void)ret; }

/* ------------------------------------------------------------ overlays */

/*
 * All overlays are linked into the executable. What the DS does on a load
 * that matters to game code is reloading the overlay's .data and zeroing its
 * .bss: its variables start fresh every time. Each overlay's variables sit in
 * their own section (tools/pc_port/gen_overlay_sections.py); FS_Init takes a
 * snapshot of them before any game code runs, and every load restores it.
 *
 * FS_LoadOverlayInfo reports a distinct fake RAM range per overlay so that
 * poke_overlay.c's overlap check (CanOverlayBeLoaded) only rejects loading an
 * overlay that is already loaded. The range is never dereferenced.
 */
#include "pc_overlay_sections.h"

#define PC_OVERLAY_FAKE_BASE  0x30000000u   /* outside ITCM/DTCM: main region */
#define PC_OVERLAY_FAKE_SPAN  0x00100000u

#ifdef PPL_OVERLAY_RESET
static char *s_ovl_snapshot[PC_OVERLAY_COUNT];

static void overlay_snapshot_all(void) {
    static BOOL done;
    if (done) {
        return;
    }
    done = TRUE;
    size_t total = 0;
    for (u32 i = 0; i < PC_OVERLAY_COUNT; i++) {
        size_t n = (size_t)(gPcOverlayData[i].end - gPcOverlayData[i].begin);
        s_ovl_snapshot[i] = malloc(n);
        if (s_ovl_snapshot[i] == NULL) {
            pc_fatal("overlay: out of memory for snapshots");
        }
        memcpy(s_ovl_snapshot[i], gPcOverlayData[i].begin, n);
        total += n;
    }
    pc_log("overlay: %u snapshots, %u bytes of variables", (unsigned)PC_OVERLAY_COUNT, (unsigned)total);
}

static void overlay_reset(FSOverlayID id) {
    if (id < PC_OVERLAY_COUNT && s_ovl_snapshot[id] != NULL) {
        memcpy(gPcOverlayData[id].begin, s_ovl_snapshot[id],
               (size_t)(gPcOverlayData[id].end - gPcOverlayData[id].begin));
    }
}
#else
static void overlay_snapshot_all(void) {}
static void overlay_reset(FSOverlayID id) { (void)id; }
#endif

/* Soft reset: nothing is loaded any more, everything starts fresh. The host
 * handle cache and interned paths stay valid (the ROM did not change). */
void pc_nitro_fs_soft_reset(void) {
    for (u32 i = 0; i < PC_OVERLAY_COUNT; i++) {
        overlay_reset(i);
    }
    s_cwd[0] = '\0';
    memset(&s_rom_archive, 0, sizeof s_rom_archive);
}

/* Called by FS_Init (see the top of this file): before NitroMain. */
static void overlay_init(void) {
    overlay_snapshot_all();
}

static BOOL overlay_load(FSOverlayID id) {
    if (id >= PC_OVERLAY_COUNT) {
        return FALSE;
    }
    pc_log("overlay: load %s (%u)", gPcOverlayNames[id], (unsigned)id);
    overlay_reset(id);
    return TRUE;
}

BOOL FS_LoadOverlayInfo(FSOverlayInfo *p_ovi, MIProcessor target, FSOverlayID id) {
    memset(p_ovi, 0, sizeof *p_ovi);
    if (id >= PC_OVERLAY_COUNT) {
        return FALSE;
    }
    p_ovi->header.id = id;
    p_ovi->header.ram_address = (u8 *)(uintptr_t)(PC_OVERLAY_FAKE_BASE + id * PC_OVERLAY_FAKE_SPAN);
    p_ovi->header.ram_size = PC_OVERLAY_FAKE_SPAN / 2;
    p_ovi->header.bss_size = PC_OVERLAY_FAKE_SPAN / 4;
    p_ovi->target = target;
    return TRUE;
}

BOOL FS_LoadOverlayImage(FSOverlayInfo *p_ovi) {
    return overlay_load(p_ovi->header.id);
}

BOOL FS_LoadOverlayImageAsync(FSOverlayInfo *p_ovi, FSFile *p_file) {
    if (p_file != NULL) {
        FS_InitFile(p_file);
    }
    return overlay_load(p_ovi->header.id);
}

/*
 * Static initializers (NitroStaticInit, nitro/sinit.h): on the DS the overlay
 * loader runs the .sinit table of an overlay when it starts it. On PC each one
 * registers itself with its overlay at program start-up (a constructor in the
 * PC branch of sinit.h); starting the overlay runs them.
 */
#define PC_MAX_STATIC_INITS 128
static struct {
    int overlay;
    void (*fn)(void);
} s_static_inits[PC_MAX_STATIC_INITS];
static int s_static_init_count;

void pc_register_static_init(int overlay, void (*fn)(void)) {
    if (s_static_init_count < PC_MAX_STATIC_INITS) {
        s_static_inits[s_static_init_count].overlay = overlay;
        s_static_inits[s_static_init_count].fn = fn;
        s_static_init_count++;
    }
}

static void overlay_run_static_inits(FSOverlayID id) {
    for (int i = 0; i < s_static_init_count; i++) {
        if (s_static_inits[i].overlay == (int)id) {
            s_static_inits[i].fn();
        }
    }
}

void FS_StartOverlay(FSOverlayInfo *p_ovi) {
    overlay_run_static_inits(p_ovi->header.id);
}

BOOL FS_LoadOverlay(MIProcessor target, FSOverlayID id) {
    (void)target;
    if (!overlay_load(id)) {
        return FALSE;
    }
    overlay_run_static_inits(id);
    return TRUE;
}

BOOL FS_UnloadOverlay(MIProcessor target, FSOverlayID id) {
    (void)target;
    if (id < PC_OVERLAY_COUNT) {
        pc_log("overlay: unload %s (%u)", gPcOverlayNames[id], (unsigned)id);
    }
    return TRUE;
}
