/*
 * card_nitro_pc.c - cartridge backup (save) API on top of save_pc.c.
 *
 * Every backup access of the SDK funnels through CARDi_RequestStreamCommand
 * (see nitro/card/backup.h inlines). Requests complete synchronously; the
 * async variants then invoke their callback, as the DS does on completion.
 * The game's own Pokéwalker SPI helpers (CARD_Spi*) talk to the IR chip on
 * the cartridge and are stubbed as "no device".
 */
#include "nitro_pc.h"

#include <string.h>

static CARDResult     s_result = CARD_RESULT_SUCCESS;
static CARDBackupType s_type = CARD_BACKUP_TYPE_NOT_USE;

void CARD_Init(void) {
}

BOOL CARD_IsPulledOut(void) {
    return FALSE;
}

void CARD_LockRom(u16 lock_id) { (void)lock_id; }
void CARD_UnlockRom(u16 lock_id) { (void)lock_id; }
void CARD_LockBackup(u16 lock_id) { (void)lock_id; }
void CARD_UnlockBackup(u16 lock_id) { (void)lock_id; }
void CARD_SetCacheFlushThreshold(u32 icache, u32 dcache) { (void)icache; (void)dcache; }

BOOL CARD_IdentifyBackup(CARDBackupType type) {
    s_type = type;
    s_result = CARD_RESULT_SUCCESS;
    return TRUE;
}

CARDResult CARD_GetResultCode(void) {
    return s_result;
}

BOOL CARD_WaitBackupAsync(void) {
    return s_result == CARD_RESULT_SUCCESS;
}

BOOL CARD_TryWaitBackupAsync(void) {
    return TRUE;   /* nothing is ever in flight */
}

void CARD_CancelBackupAsync(void) {
}

static BOOL finish(BOOL ok, MIDmaCallback callback, void *arg, BOOL is_async) {
    s_result = ok ? CARD_RESULT_SUCCESS : CARD_RESULT_FAILURE;
    if (is_async && callback != NULL) {
        callback(arg);
    }
    return ok;
}

BOOL CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async,
                                CARDRequest req_type, int req_retry, CARDRequestMode req_mode) {
    (void)req_retry;
    if (s_type == CARD_BACKUP_TYPE_NOT_USE) {
        s_result = CARD_RESULT_NO_RESPONSE;
        return FALSE;
    }
    BOOL ok;
    switch (req_type) {
    case CARD_REQ_READ_BACKUP:
        ok = pc_save_read(src, (void *)(uintptr_t)dst, len);
        break;
    case CARD_REQ_WRITE_BACKUP:
        ok = pc_save_write(dst, (const void *)(uintptr_t)src, len);
        if (ok && req_mode == CARD_REQUEST_MODE_SEND_VERIFY) {
            /* Verify against what we just wrote, like the chip would. */
            u8 tmp[256];
            for (u32 done = 0; ok && done < len; done += sizeof tmp) {
                u32 n = len - done < sizeof tmp ? len - done : (u32)sizeof tmp;
                ok = pc_save_read(dst + done, tmp, n) &&
                     memcmp(tmp, (const u8 *)(uintptr_t)src + done, n) == 0;
            }
        }
        if (ok) {
            ok = pc_save_flush();
        }
        break;
    case CARD_REQ_ERASE_SECTOR_BACKUP:
    case CARD_REQ_ERASE_PAGE_BACKUP:
        ok = pc_save_erase(dst, len) && pc_save_flush();
        break;
    default:
        pc_warn("CARD: unsupported backup request %d", (int)req_type);
        ok = FALSE;
        break;
    }
    return finish(ok, callback, arg, is_async);
}

/* Raw ROM reads (outside NitroFS): only the Wi-Fi setup screens use them;
 * there is no ROM image on PC, so the buffer is cleared. */
void CARDi_ReadRom(u32 dma, const void *src, void *dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async) {
    (void)dma;
    (void)src;
    memset(dst, 0, len);
    s_result = CARD_RESULT_FAILURE;
    if (is_async && callback != NULL) {
        callback(arg);
    }
}

/* --- Pokéwalker IR (cartridge SPI) : no device attached ----------------- */

void CARD_SetSpiReadWaitCycles(u32 cycles) { (void)cycles; }
void CARD_SetSpiWriteWaitCycles(u32 cycles) { (void)cycles; }

void CARD_SpiWaitInit(void) {
}

int CARD_SpiWaitGetStatus(void) {
    return 0;
}

int CARD_SpiWaitReadRange(u8 *buffer) {
    (void)buffer;
    return 0;
}

void CARD_SpiWaitWriteRange(const void *data, int size) {
    (void)data;
    (void)size;
}
