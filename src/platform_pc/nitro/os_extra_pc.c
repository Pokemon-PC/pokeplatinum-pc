/*
 * os_extra_pc.c - remaining NitroSDK OS services: alarms, threads, mutexes,
 * message queues, lock IDs, printf.
 *
 * Execution model: the game runs on a single host thread. Alarms fire from
 * the VBlank hook (pc_nitro_os_alarm_tick). Threads are only created by
 * online / special-hardware features (Pokewalker IR, GTS sockets, Wi-Fi
 * setup, card task thread): they are recorded and reported as finished
 * without running, which keeps those features inert instead of hanging.
 * A cooperative scheduler can replace this if one of them becomes needed.
 */
#include "nitro_pc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- alarms */

#define MAX_ALARMS 64
static OSAlarm *s_alarms[MAX_ALARMS];
static BOOL s_alarm_ready;

void OS_InitAlarm(void) {
    memset(s_alarms, 0, sizeof s_alarms);
    s_alarm_ready = TRUE;
}

BOOL OS_IsAlarmAvailable(void) {
    return s_alarm_ready;
}

void OS_CreateAlarm(OSAlarm *alarm) {
    memset(alarm, 0, sizeof *alarm);
}

static void alarm_unlink(OSAlarm *alarm) {
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (s_alarms[i] == alarm) {
            s_alarms[i] = NULL;
        }
    }
}

static void alarm_link(OSAlarm *alarm) {
    alarm_unlink(alarm);
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (s_alarms[i] == NULL) {
            s_alarms[i] = alarm;
            return;
        }
    }
    pc_warn("OS: too many alarms");
}

void OS_SetAlarm(OSAlarm *alarm, OSTick tick, OSAlarmHandler handler, void *arg) {
    alarm->handler = handler;
    alarm->arg = arg;
    alarm->fire = OS_GetTick() + tick;
    alarm->period = 0;
    alarm_link(alarm);
}

void OS_SetPeriodicAlarm(OSAlarm *alarm, OSTick start, OSTick period, OSAlarmHandler handler, void *arg) {
    alarm->handler = handler;
    alarm->arg = arg;
    alarm->start = start;
    alarm->period = period;
    alarm->fire = start;
    alarm_link(alarm);
}

void OS_CancelAlarm(OSAlarm *alarm) {
    alarm_unlink(alarm);
    alarm->handler = NULL;
}

/* Called once per frame from pc_nitro_vblank(). */
void pc_nitro_os_alarm_tick(void) {
    OSTick now = OS_GetTick();
    for (int i = 0; i < MAX_ALARMS; i++) {
        OSAlarm *a = s_alarms[i];
        if (a == NULL || a->handler == NULL || now < a->fire) {
            continue;
        }
        OSAlarmHandler h = a->handler;
        void *arg = a->arg;
        if (a->period != 0) {
            while (a->fire <= now) {
                a->fire += a->period;
            }
        } else {
            s_alarms[i] = NULL;
            a->handler = NULL;
        }
        h(arg);
    }
}

/* ------------------------------------------------------------- threads */

static BOOL s_thread_ready = TRUE;

BOOL OS_IsThreadAvailable(void) {
    return s_thread_ready;
}

void OS_CreateThread(OSThread *thread, void (*func)(void *), void *arg, void *stack, u32 stackSize, u32 prio) {
    (void)func; (void)arg; (void)stack; (void)stackSize; (void)prio;
    memset(thread, 0, sizeof *thread);
    thread->state = OS_THREAD_STATE_TERMINATED;
    pc_warn("OS_CreateThread: secondary threads are not run on PC (online/IR feature)");
}

BOOL OS_IsThreadTerminated(const OSThread *thread) {
    return thread->state == OS_THREAD_STATE_TERMINATED;
}

void OS_JoinThread(OSThread *thread) { (void)thread; }
void OS_KillThread(OSThread *thread, void *arg) { (void)arg; thread->state = OS_THREAD_STATE_TERMINATED; }
void OS_WakeupThreadDirect(OSThread *thread) { (void)thread; }

/* ------------------------------------------------------------- mutexes */

void OS_InitMutex(OSMutex *mutex) { memset(mutex, 0, sizeof *mutex); }
void OS_LockMutex(OSMutex *mutex) { (void)mutex; }
void OS_UnlockMutex(OSMutex *mutex) { (void)mutex; }

/* ------------------------------------------------------- message queues */

void OS_InitMessageQueue(OSMessageQueue *mq, OSMessage *msgArray, s32 msgCount) {
    memset(mq, 0, sizeof *mq);
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
}

/* Non-blocking on PC: with a single thread nobody else could fill or drain
 * the queue while we wait, so a would-block call fails instead of hanging. */
BOOL OS_SendMessage(OSMessageQueue *mq, OSMessage msg, s32 flags) {
    (void)flags;
    if (mq->usedCount >= mq->msgCount) {
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
    return TRUE;
}

BOOL OS_ReceiveMessage(OSMessageQueue *mq, OSMessage *msg, s32 flags) {
    (void)flags;
    if (mq->usedCount == 0) {
        return FALSE;
    }
    if (msg != NULL) {
        *msg = mq->msgArray[mq->firstIndex];
    }
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    return TRUE;
}

/* ------------------------------------------------------------ misc */

static s32 s_next_lock_id = 0x40;

s32 OS_GetLockID(void) {
    return s_next_lock_id < 0x7F ? s_next_lock_id++ : 0x40;
}

void OS_ReleaseLockID(u16 lockID) { (void)lockID; }
void OSi_UnlockVram(u16 bank, u16 lockId) { (void)bank; (void)lockId; }

u16 OS_GetTickLo(void) {
    return (u16)OS_GetTick();
}

static s64 s_tick_offset;

void OS_SetTick(OSTick tick) {
    s_tick_offset = (s64)tick - (s64)pc_os_ticks();   /* informational only */
}

/* Waits for an interrupt: on PC the only one is the next VBlank. */
void OS_Halt(void) {
    pc_os_wait_vblank();
    pc_nitro_vblank();
}

int OS_VSNPrintf(char *dest, size_t len, const char *format, va_list va) {
    return vsnprintf(dest, len, format, va);
}

int OS_SPrintf(char *dest, const char *format, ...) {
    va_list va;
    va_start(va, format);
    int n = vsprintf(dest, format, va);
    va_end(va);
    return n;
}

/* --------------------------------------------- C library bits (MSL) */

void __msl_assertion_failed(const char *condition, const char *file, const char *function, int line) {
    pc_warn("assertion failed: %s (%s:%d, %s)", condition, file, line, function ? function : "?");
}

/* MSL's C-locale lower-case map (used by the Wi-Fi chat/lobby code). */
const unsigned char __lower_mapC[256] = {
#define L(c) (unsigned char)(((c) >= 'A' && (c) <= 'Z') ? (c) + 32 : (c))
#define R16(b) L(b+0),L(b+1),L(b+2),L(b+3),L(b+4),L(b+5),L(b+6),L(b+7),L(b+8),L(b+9),L(b+10),L(b+11),L(b+12),L(b+13),L(b+14),L(b+15)
    R16(0x00), R16(0x10), R16(0x20), R16(0x30), R16(0x40), R16(0x50), R16(0x60), R16(0x70),
    R16(0x80), R16(0x90), R16(0xA0), R16(0xB0), R16(0xC0), R16(0xD0), R16(0xE0), R16(0xF0),
#undef R16
#undef L
};

/* DWC's own case-insensitive compare (lib/include/dwc/chat_internal.h). */
int dwc_strnicmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        ca = __lower_mapC[ca];
        cb = __lower_mapC[cb];
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
    }
}

/* -------------------------------------------- generated-stub reporting */

void pc_stub_called(const char *name) {
    static const char *seen[512];
    static int count;
    for (int i = 0; i < count; i++) {
        if (seen[i] == name) {
            return;
        }
    }
    if (count < 512) {
        seen[count++] = name;
    }
    pc_log("stub: %s() (offline / not emulated on PC)", name);
}
