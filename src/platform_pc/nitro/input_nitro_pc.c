/*
 * input_nitro_pc.c - PAD (via virtual registers) and TP (touch panel).
 *
 * PAD_Read()/PAD_DetectFold() are NitroSDK inlines reading KEYINPUT and
 * HW_BUTTON_XY_BUF; pc_nitro_vblank() publishes the host input there.
 *
 * TP: the DS ARM7 samples the touch screen `frequency` times per frame into
 * a ring buffer owned by the game (src/touchpad.c). We reproduce that at
 * each VBlank. Coordinates are reported raw == screen pixels and the
 * calibration is the identity, so TP_GetCalibratedPoint() is a copy.
 */
#include "nitro_pc.h"

#include <string.h>

/* src/touchpad.c declares these itself (they are not in the SDK headers). */
void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequency, TPData *samplingBufs, u16 bufSize);
void TP_RequestAutoSamplingStopAsync(void);
void TP_WaitBusy(u32 command_flgs);
u32 TP_CheckBusy(u32 command_flgs);
u16 TP_GetLatestIndexInAuto(void);

static TPData *s_auto_buf;
static u16     s_auto_size;
static u16     s_auto_freq;
static u16     s_auto_latest;
static TPCalibrateParam s_calib = { 0, 0, 1, 1 };

static TPData current_sample(void) {
    TPData d;
    PcTouch t = pc_input_touch();
    d.x = t.x;
    d.y = t.y;
    d.touch = t.touching ? TP_TOUCH_ON : TP_TOUCH_OFF;
    d.validity = TP_VALIDITY_VALID;
    return d;
}

void TP_Init(void) {
    s_auto_buf = NULL;
    s_auto_size = s_auto_freq = s_auto_latest = 0;
}

BOOL TP_GetUserInfo(TPCalibrateParam *param) {
    *param = s_calib;
    return TRUE;
}

void TP_SetCalibrateParam(const TPCalibrateParam *param) {
    if (param != NULL) {
        s_calib = *param;
    }
}

void TP_RequestSamplingAsync(void) {
}

u32 TP_WaitRawResult(TPData *result) {
    *result = current_sample();
    return 0;
}

void TP_GetCalibratedPoint(TPData *calib, const TPData *raw) {
    *calib = *raw;
    if (raw->touch == TP_TOUCH_ON) {
        /* Clamp exactly like the SDK does to the visible area. */
        if (calib->x > 255) calib->x = 255;
        if (calib->y > 191) calib->y = 191;
    }
}

void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequency, TPData *samplingBufs, u16 bufSize) {
    (void)vcount;
    s_auto_buf = samplingBufs;
    s_auto_size = bufSize;
    s_auto_freq = frequency ? frequency : 1;
    s_auto_latest = 0;
    if (s_auto_buf != NULL && s_auto_size != 0) {
        memset(s_auto_buf, 0, sizeof(TPData) * s_auto_size);
    }
}

void TP_RequestAutoSamplingStopAsync(void) {
    s_auto_buf = NULL;
}

void TP_WaitBusy(u32 command_flgs) {
    (void)command_flgs;
}

u32 TP_CheckBusy(u32 command_flgs) {
    (void)command_flgs;
    return 0;
}

u16 TP_GetLatestIndexInAuto(void) {
    return s_auto_latest;
}

void TP_GetLatestRawPointInAuto(TPData *result) {
    if (s_auto_buf != NULL && s_auto_size != 0) {
        *result = s_auto_buf[s_auto_latest];
    } else {
        *result = current_sample();
    }
}

void pc_nitro_tp_vblank(void) {
    if (s_auto_buf == NULL || s_auto_size == 0) {
        return;
    }
    TPData d = current_sample();
    for (u16 i = 0; i < s_auto_freq; i++) {
        s_auto_latest = (u16)((s_auto_latest + 1) % s_auto_size);
        s_auto_buf[s_auto_latest] = d;
    }
}
