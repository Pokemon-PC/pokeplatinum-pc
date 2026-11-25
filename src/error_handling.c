#include "error_handling.h"

#include <nitro.h>
#include <string.h>

#include "comm_manager.h"
#include "error_message_reset.h"

void ErrorHandling_AssertFail(void);

#ifdef PLATFORM_PC
// PC port: report failed game asserts with a backtrace (src/platform_pc/crash_pc.c).
void ErrorHandling_AssertFail(void)
{
    pc_log_backtrace("assert");
    if (CommManager_IsInitialized() && (OS_GetProcMode() != OS_PROCMODE_IRQ)) {
#else
void ErrorHandling_AssertFail(void)
{
    if (CommManager_IsInitialized() && (OS_GetProcMode() != OS_PROCMODE_IRQ)) {
#endif
        ErrorMessageReset_PrintErrorAndReset();
    }
}
