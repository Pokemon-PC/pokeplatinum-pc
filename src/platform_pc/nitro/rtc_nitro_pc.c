/*
 * rtc_nitro_pc.c - real-time clock from the host's local time.
 * (HGSS uses it for day/night, the day of the week and timed events.)
 * The date/second conversion helpers are the SDK's own C code
 * (lib/NitroSDK/src/rtc/rtc_convert_*.c), compiled unchanged.
 */
#include "nitro_pc.h"

#include <stdlib.h>
#include <time.h>

static void now(RTCDate *date, RTCTime *t) {
    time_t raw = time(NULL);
    {
        /* Test hook PPL_TEST_DAY_OFFSET=N: N days later (weekly events). */
        static long day_offset = -1000000;
        if (day_offset == -1000000) {
            const char *d = getenv("PPL_TEST_DAY_OFFSET");
            day_offset = d != NULL ? atol(d) : 0;
        }
        raw += (time_t)day_offset * 86400;
    }
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &raw);
#else
    localtime_r(&raw, &lt);
#endif
    if (date != NULL) {
        date->year = (u32)(lt.tm_year % 100);   /* DS RTC: 2000-2099 */
        date->month = (u32)lt.tm_mon + 1;
        date->day = (u32)lt.tm_mday;
        date->week = (RTCWeek)lt.tm_wday;       /* 0 = Sunday, like RTC_WEEK_SUNDAY */
    }
    if (t != NULL) {
        /* Test hook PPL_TEST_HOUR=H: fixed hour (day/night checks). */
        static int test_hour = -2;
        if (test_hour == -2) {
            const char *h = getenv("PPL_TEST_HOUR");
            test_hour = h != NULL ? atoi(h) % 24 : -1;
        }
        if (test_hour >= 0) {
            lt.tm_hour = test_hour;
        }
        t->hour = (u32)lt.tm_hour;
        t->minute = (u32)lt.tm_min;
        t->second = (u32)(lt.tm_sec > 59 ? 59 : lt.tm_sec);
    }
}

void RTC_Init(void) {
}

RTCResult RTC_GetDate(RTCDate *date) {
    now(date, NULL);
    return RTC_RESULT_SUCCESS;
}

RTCResult RTC_GetTime(RTCTime *time_) {
    now(NULL, time_);
    return RTC_RESULT_SUCCESS;
}

RTCResult RTC_GetDateTime(RTCDate *date, RTCTime *time_) {
    now(date, time_);
    return RTC_RESULT_SUCCESS;
}

RTCResult RTC_GetDateTimeAsync(RTCDate *date, RTCTime *time_, RTCCallback callback, void *arg) {
    now(date, time_);
    if (callback != NULL) {
        callback(RTC_RESULT_SUCCESS, arg);
    }
    return RTC_RESULT_SUCCESS;
}
