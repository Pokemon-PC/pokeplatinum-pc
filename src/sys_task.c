#include "sys_task.h"

#include <nitro.h>
#include <string.h>

#include "sys_task_manager.h"
#include "system.h"

SysTask *SysTask_Start(SysTaskFunc callback, void *param, u32 priority)
{
    return SysTaskManager_AddTask(gSystem.mainTaskMgr, callback, param, priority);
}

#ifdef PLATFORM_PC
// PC port: on the DS the V-Blank interrupt arrives in the middle of long
// work (loading a map takes several frames) and runs the V-Blank tasks,
// freeing their slots. The PC delivers interrupts at wait points only, so a
// long load could fill the 32 slots: when the list is full, the V-Blank that
// the DS would have had is delivered now, then the task is added.
SysTask *SysTask_ExecuteOnVBlank(SysTaskFunc callback, void *param, u32 priority)
{
    SysTask *task = SysTaskManager_AddTask(gSystem.vBlankTaskMgr, callback, param, priority);
    if (task == NULL) {
        pc_nitro_vblank_irq_now();
        task = SysTaskManager_AddTask(gSystem.vBlankTaskMgr, callback, param, priority);
    }
    return task;
}
#else
SysTask *SysTask_ExecuteOnVBlank(SysTaskFunc callback, void *param, u32 priority)
{
    return SysTaskManager_AddTask(gSystem.vBlankTaskMgr, callback, param, priority);
}
#endif

SysTask *SysTask_CreateOnPrintQueue(SysTaskFunc callback, void *param, u32 priority)
{
    return SysTaskManager_AddTask(gSystem.printTaskMgr, callback, param, priority);
}

SysTask *SysTask_ExecuteAfterVBlank(SysTaskFunc callback, void *param, u32 priority)
{
    return SysTaskManager_AddTask(gSystem.postVBlankTaskMgr, callback, param, priority);
}

void SysTask_Done(SysTask *task)
{
    SysTask_Delete(task);
}
