/*
 * wm_pc.c - the DS wireless manager (WM) as an empty radio.
 *
 * There is no local wireless on PC. The game's wireless layer
 * (src/wireless_manager.c) and its users (Union Room, Wireless Colosseum,
 * Pokéathlon / mini-games in link mode...) only need the radio to behave as
 * on a DS with nobody around: every request succeeds, a parent beacons for
 * nobody, a scan never finds a parent. Requests answer through their callback
 * asynchronously, like the ARM7 does: callbacks are queued and run at the
 * next V-Blank (pc_wm_vblank, called by pc_nitro_vblank).
 *
 * Callback argument layouts follow lib/include/nitro/wm/wm_internal.h and the
 * event structs read by src/wireless_manager.c (state at +4 for MP events,
 * +8 for parent / scan / connect events).
 */
#include "nitro_pc.h"

#include <string.h>

typedef WMCallbackFunc WmCallback;   /* void (*)(void *arg) */

#define WM_SUCCESS   WM_ERRCODE_SUCCESS
#define WM_OPERATING WM_ERRCODE_OPERATING

/* WM API ids (WM_APIID_*), only reported back in the callback argument. */
enum {
    APIID_INITIALIZE = 0, APIID_RESET = 1, APIID_END = 2, APIID_SET_P_PARAM = 7,
    APIID_START_PARENT = 8, APIID_END_PARENT = 9, APIID_START_SCAN = 10, APIID_END_SCAN = 11,
    APIID_START_CONNECT = 12, APIID_DISCONNECT = 13, APIID_START_MP = 14, APIID_SET_MP_DATA = 15,
    APIID_END_MP = 16, APIID_SET_ENTRY = 21, APIID_SET_LIFETIME = 23, APIID_MEASURE_CHANNEL = 24,
    APIID_SET_GAMEINFO = 27,
};

#define WM_QUEUE 32
typedef struct {
    WmCallback cb;
    u8 arg[0x100];
} WmPending;

static WmPending s_queue[WM_QUEUE];
static int s_count;
static u16 s_tgid = 1;

static u8 *queue(WmCallback cb, u16 apiid) {
    if (cb == NULL || s_count >= WM_QUEUE) {
        return NULL;
    }
    WmPending *p = &s_queue[s_count++];
    memset(p, 0, sizeof *p);
    p->cb = cb;
    *(u16 *)(p->arg + 0) = apiid;
    *(u16 *)(p->arg + 2) = WM_SUCCESS;
    return p->arg;
}

void pc_wm_vblank(void) {
    /* Callbacks may queue new requests (e.g. the next scan): run only the
     * ones pending now, the new ones answer at the next V-Blank. */
    int n = s_count;
    WmPending now[WM_QUEUE];
    memcpy(now, s_queue, sizeof(WmPending) * (size_t)n);
    memmove(s_queue, s_queue + n, sizeof(WmPending) * (size_t)(s_count - n));
    s_count -= n;
    for (int i = 0; i < n; i++) {
        now[i].cb(now[i].arg);
    }
}

void pc_wm_reset(void) {
    s_count = 0;
}

static WMErrCode async(WmCallback cb, u16 apiid) {
    queue(cb, apiid);
    return WM_OPERATING;
}

WMErrCode WM_Initialize(void *buffer, WMCallbackFunc callback, u16 dma) {
    (void)buffer, (void)dma;
    return async(callback, APIID_INITIALIZE);
}

WMErrCode WM_InitializeForListening(void *buffer, WMCallbackFunc callback, u16 dma, BOOL unk) {
    (void)buffer, (void)dma, (void)unk;
    return async(callback, APIID_INITIALIZE);
}

WMErrCode WM_SetIndCallback(WMCallbackFunc callback) {
    (void)callback;
    return WM_SUCCESS;
}

WMErrCode WM_SetPortCallback(u16 port, WMCallbackFunc callback, void *arg) {
    (void)port, (void)callback, (void)arg;
    return WM_SUCCESS;
}

WMErrCode WM_SetLifeTime(WMCallbackFunc callback, u16 a, u16 b, u16 c, u16 d) {
    (void)a, (void)b, (void)c, (void)d;
    return async(callback, APIID_SET_LIFETIME);
}

WMErrCode WM_SetParentParameter(WMCallbackFunc callback, const WMParentParam *param) {
    (void)param;
    return async(callback, APIID_SET_P_PARAM);
}

WMErrCode WM_StartParent(WMCallbackFunc callback) {
    u8 *a = queue(callback, APIID_START_PARENT);
    if (a != NULL) {
        *(u16 *)(a + 8) = 0;   /* WM_STATECODE_PARENT_START */
    }
    return WM_OPERATING;
}

WMErrCode WM_EndParent(WMCallbackFunc callback) {
    return async(callback, APIID_END_PARENT);
}

WMErrCode WM_StartMP(WMCallbackFunc callback, u16 *recvBuf, u16 recvSize, u16 *sendBuf, u16 sendSize, u16 frequency) {
    (void)recvBuf, (void)recvSize, (void)sendBuf, (void)sendSize, (void)frequency;
    u8 *a = queue(callback, APIID_START_MP);
    if (a != NULL) {
        *(u16 *)(a + 4) = 10;  /* WM_STATECODE_MP_START */
    }
    return WM_OPERATING;
}

WMErrCode WM_EndMP(WMCallbackFunc callback) {
    return async(callback, APIID_END_MP);
}

WMErrCode WM_StartScan(WMCallbackFunc callback, const WMScanParam *param) {
    (void)param;
    u8 *a = queue(callback, APIID_START_SCAN);
    if (a != NULL) {
        *(u16 *)(a + 8) = 4;   /* channel scanned, no parent found */
    }
    return WM_OPERATING;
}

WMErrCode WM_EndScan(WMCallbackFunc callback) {
    return async(callback, APIID_END_SCAN);
}

WMErrCode WM_StartConnectEx(WMCallbackFunc callback, const WMBssDesc *bssDesc, const u8 *data, BOOL unk, u16 unk2) {
    (void)bssDesc, (void)data, (void)unk, (void)unk2;
    u8 *a = queue(callback, APIID_START_CONNECT);
    if (a != NULL) {
        *(u16 *)(a + 2) = 1;   /* WM_ERRCODE_FAILED: there is nobody to connect to */
    }
    return WM_OPERATING;
}

WMErrCode WM_Disconnect(WMCallbackFunc callback, u16 aid) {
    (void)aid;
    return async(callback, APIID_DISCONNECT);
}

WMErrCode WM_Reset(WMCallbackFunc callback) {
    s_count = 0;
    return async(callback, APIID_RESET);
}

WMErrCode WM_End(WMCallbackFunc callback) {
    return async(callback, APIID_END);
}

WMErrCode WM_SetMPDataToPortEx(WMCallbackFunc callback, void *arg, const u16 *data, u16 length, u16 aidBitmap, u16 port,
                               u16 priority) {
    (void)data, (void)length, (void)aidBitmap, (void)port, (void)priority;
    u8 *a = queue(callback, APIID_SET_MP_DATA);
    if (a != NULL) {
        memcpy(a + 0x20, &arg, sizeof arg);   /* WirelessSendEvent.callback */
    }
    return WM_OPERATING;
}

WMErrCode WM_MeasureChannel(WMCallbackFunc callback, u16 a, u16 b, u16 channel, u16 d) {
    (void)a, (void)b, (void)d;
    u8 *e = queue(callback, APIID_MEASURE_CHANNEL);
    if (e != NULL) {
        *(u16 *)(e + 8) = channel;
        *(u16 *)(e + 10) = 0;  /* ccaBusyRatio: a quiet channel */
    }
    return WM_OPERATING;
}

WMErrCode WM_SetGameInfo(WMCallbackFunc callback, const u16 *gameInfo, u16 size, u32 ggid, u16 tgid, u8 attr) {
    (void)gameInfo, (void)size, (void)ggid, (void)tgid, (void)attr;
    return async(callback, APIID_SET_GAMEINFO);
}

WMErrCode WM_SetEntry(WMCallbackFunc callback, BOOL flag) {
    (void)flag;
    return async(callback, APIID_SET_ENTRY);
}

u16 WM_GetAllowedChannel(void) {
    return (1u << 0) | (1u << 6) | (1u << 12);   /* channels 1, 7, 13 */
}

u16 WM_GetDispersionScanPeriod(void) {
    return 30;
}

u16 WM_GetDispersionBeaconPeriod(void) {
    return 200;
}

u16 WM_GetNextTgid(void) {
    return s_tgid++;
}

/* WM status block shared with the ARM7 (the game clears a word at +0x198). */
const WMStatus *WMi_GetStatusAddress(void) {
    static u8 status[0x800] __attribute__((aligned(32)));
    return (const WMStatus *)status;
}

WMLinkLevel WM_GetLinkLevel(void) {
    return (WMLinkLevel)0;
}
