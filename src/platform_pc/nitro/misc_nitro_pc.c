/*
 * misc_nitro_pc.c - small NitroSDK functions the Platinum code uses that the
 * HeartGold PC layer did not need: data cache, BIOS (SVC) helpers, power
 * management callbacks, thread queues, VRAM locks, wireless power, WVR.
 *
 * Each one does on PC what the DS observably does for the game: caches and
 * amplifier do not exist (no-ops), BIOS helpers compute the same result,
 * wireless requests succeed with nobody around (see wm_pc.c).
 */
#include "nitro_pc.h"

#include <nitro/wvr.h>
#include <string.h>

/* ------------------------------------------------------------- data cache */
void DC_FlushAll(void) {}
void DC_FlushRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_StoreRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_WaitWriteBufferEmpty(void) {}

/* ------------------------------------------------------------ BIOS (SVC) */

/* CRC-16 (polynomial 0xA001, reflected), BIOS function 0x0E. */
u16 SVC_GetCRC16(u32 start, const void *datap, u32 size) {
    static const u16 k_val[8] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };
    const u8 *p = datap;
    u32 crc = start & 0xFFFF;
    for (u32 i = 0; i < size; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            int carry = crc & 1;
            crc >>= 1;
            if (carry) {
                crc ^= (u32)k_val[j] << (7 - j);
            }
        }
    }
    return (u16)crc;
}

/* Integer square root (truncated), BIOS function 0x08. */
u16 SVC_Sqrt(u32 src) {
    u32 r = 0, bit = 1u << 30;
    while (bit > src) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (src >= r + bit) {
            src -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (u16)r;
}

/* ------------------------------------------------------- power management */
/* There is no sleep mode on PC: the callbacks are kept but never called. */
void PM_Init(void) {}
void PM_AppendPostSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_DeletePostSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_PrependPreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_DeletePreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
u32 PM_SetAmp(PMAmpSwitch sw) { (void)sw; return PM_RESULT_SUCCESS; }
u32 PM_SetAmpGain(PMAmpGain gain) { (void)gain; return PM_RESULT_SUCCESS; }

/* ---------------------------------------------------------------- threads */
/* The game runs on one host thread; nothing ever waits in a queue. */
void OS_SleepThread(OSThreadQueue *queue) { (void)queue; }
void OS_WakeupThread(OSThreadQueue *queue) { (void)queue; }

/* VRAM banks shared with the ARM7 (wireless): always free on PC. */
BOOL OSi_TryLockVram(u16 bank, u16 lockId) { (void)bank; (void)lockId; return TRUE; }

/* --------------------------------------------------------- touch, files */
u32 TP_CheckError(TPRequestCommandFlag command) { (void)command; return 0; }
void FS_CancelFile(FSFile *p_file) { (void)p_file; }   /* reads complete at once */

/* -------------------------------------------------------------- GX FIFO */
void GX_SendFifo128B(const void *pSrc, void *pDest) {
    MIi_CpuSend32(pSrc, pDest, 128);
}

/* ------------------------------------------------------- wireless power */
WMErrCode WM_Init(void *wmSysBuf, u16 dmaNo) { (void)wmSysBuf; (void)dmaNo; return WM_ERRCODE_SUCCESS; }
WMErrCode WM_Finish(void) { return WM_ERRCODE_SUCCESS; }

static void wm_reply(WMCallbackFunc callback, u16 apiid) {
    if (callback != NULL) {
        WMCallback arg;
        memset(&arg, 0, sizeof arg);
        arg.apiid = apiid;
        arg.errcode = WM_ERRCODE_SUCCESS;
        callback(&arg);
    }
}

WMErrCode WM_Enable(WMCallbackFunc callback) { wm_reply(callback, WM_APIID_ENABLE); return WM_ERRCODE_OPERATING; }
WMErrCode WM_Disable(WMCallbackFunc callback) { wm_reply(callback, WM_APIID_DISABLE); return WM_ERRCODE_OPERATING; }
WMErrCode WM_PowerOn(WMCallbackFunc callback) { wm_reply(callback, WM_APIID_POWER_ON); return WM_ERRCODE_OPERATING; }
WMErrCode WM_PowerOff(WMCallbackFunc callback) { wm_reply(callback, WM_APIID_POWER_OFF); return WM_ERRCODE_OPERATING; }

/* --------------------------------------------- wireless VRAM loader (WVR) */
/* WVR loads the wireless driver into VRAM for the ARM7. Starting must
 * succeed, or the game ends with an error screen: report success at once. */
WVRResult WVR_StartUpAsync(GXVRamARM7 vram, WVRCallbackFunc callback, void *arg) {
    (void)vram;
    if (callback != NULL) {
        callback(arg, WVR_RESULT_SUCCESS);
    }
    return WVR_RESULT_OPERATING;
}

WVRResult WVR_TerminateAsync(WVRCallbackFunc callback, void *arg) {
    if (callback != NULL) {
        callback(arg, WVR_RESULT_SUCCESS);
    }
    return WVR_RESULT_OPERATING;
}
