/*
 * save_pc.c - cartridge backup memory (CARD_*Backup*) -> save.bin.
 *
 * HGSS uses a 512 KiB SPI flash. The whole image is kept in RAM, mirrors the
 * chip semantics (erased = 0xFF) and is written back to disk atomically
 * (temp file + rename) on every flush, so a crash never corrupts save.bin.
 * The file is byte-compatible with DeSmuME/melonDS ".sav" raw dumps
 * (512 KiB), so existing saves can be dropped in.
 *
 * Milestone 2 maps CARD_IdentifyBackup / CARD_ReadBackupAsync /
 * CARD_WriteAndVerifyBackupAsync / CARD_EraseBackupSectorAsync onto this API
 * (completing synchronously, then invoking the callback).
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static uint8_t *s_image;
static char     s_path[1024];
static bool     s_dirty;

bool pc_save_init(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    s_image = malloc(PC_SAVE_SIZE);
    if (s_image == NULL) {
        return false;
    }
    memset(s_image, 0xFF, PC_SAVE_SIZE);
    s_dirty = false;

    FILE *fp = fopen(s_path, "rb");
    if (fp == NULL) {
        pc_log("save: %s not found, starting with a blank (erased) chip", s_path);
        return true;
    }
    size_t got = fread(s_image, 1, PC_SAVE_SIZE, fp);
    fclose(fp);
    if (got != PC_SAVE_SIZE) {
        pc_warn("save: %s is %u bytes (expected %u); remaining bytes left erased",
                s_path, (unsigned)got, (unsigned)PC_SAVE_SIZE);
    }
    pc_log("save: loaded %s", s_path);
    return true;
}

void pc_save_shutdown(void) {
    pc_save_flush();
    free(s_image);
    s_image = NULL;
}

static bool in_range(uint32_t offset, uint32_t len) {
    return s_image != NULL && offset <= PC_SAVE_SIZE && len <= PC_SAVE_SIZE - offset;
}

bool pc_save_read(uint32_t offset, void *dst, uint32_t len) {
    if (!in_range(offset, len)) {
        return false;
    }
    memcpy(dst, s_image + offset, len);
    return true;
}

bool pc_save_write(uint32_t offset, const void *src, uint32_t len) {
    if (!in_range(offset, len)) {
        return false;
    }
    memcpy(s_image + offset, src, len);
    s_dirty = true;
    return true;
}

bool pc_save_erase(uint32_t offset, uint32_t len) {
    if (!in_range(offset, len)) {
        return false;
    }
    memset(s_image + offset, 0xFF, len);
    s_dirty = true;
    return true;
}

bool pc_save_flush(void) {
    if (s_image == NULL || !s_dirty) {
        return true;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE *fp = fopen(tmp, "wb");
    if (fp == NULL) {
        pc_warn("save: cannot write %s", tmp);
        return false;
    }
    bool ok = fwrite(s_image, 1, PC_SAVE_SIZE, fp) == PC_SAVE_SIZE;
    ok = (fclose(fp) == 0) && ok;
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(tmp, s_path) == 0;
#endif
    }
    if (!ok) {
        pc_warn("save: failed to commit %s", s_path);
        return false;
    }
    s_dirty = false;
    return true;
}
