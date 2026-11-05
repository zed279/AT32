/**
 * @file    scheduler.c
 * @brief   协作式任务调度器实现
 */
#include "scheduler.h"
#include "bm_hal.h"
#include <string.h>

/*==============================================================================
 * 编译期保险 —— 枚举个数不能超过任务槽位数
 *
 * @details Scheduler_RegisterTask() 对 id >= SCHEDULER_MAX_TASKS 的调用是
 *          **静默 return 0**. 一旦有人往 TaskHandle_t 里加成员却忘了调大
 *          SCHEDULER_MAX_TASKS, 新任务就永远不执行, 而且没有任何报错, 现象
 *          跟"外设坏了"一模一样 (本工程 TASK_UI 就因为排到第 9 个而整整
 *          一个下午没跑起来).
 *
 *          这里用 typedef 反向下标做静态断言: 条件成立时数组长度为 1,
 *          不成立时长度为 -1, 编译直接报错. 比运行时检查可靠得多.
 *============================================================================*/
#if defined(__CC_ARM)
typedef char scheduler_slot_check_t[(TASK_MAX <= SCHEDULER_MAX_TASKS) ? 1 : -1];
#else
_Static_assert(TASK_MAX <= SCHEDULER_MAX_TASKS,
               "TASK_MAX exceeds SCHEDULER_MAX_TASKS: enlarge it in scheduler.h");
#endif

/*==============================================================================
 * 任务表 — 大小由 SCHEDULER_MAX_TASKS 控制
 *============================================================================*/
static TaskCtrlBlock_t s_tasks[SCHEDULER_MAX_TASKS];

/*==============================================================================
 * 初始化
 *============================================================================*/
void Scheduler_Init(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));
}

/*==============================================================================
 * 注册任务
 *============================================================================*/
uint8_t Scheduler_RegisterTask(TaskHandle_t id, TaskFunc_t func, uint32_t period_ms)
{
    if ((uint32_t)id >= SCHEDULER_MAX_TASKS || func == NULL) {
        return 0;
    }

    s_tasks[id].func       = func;
    s_tasks[id].period_ms  = period_ms;
    s_tasks[id].last_run   = BM_GetTick();
    s_tasks[id].enable     = 1;
    s_tasks[id].registered = 1;

    return 1;
}

/*==============================================================================
 * 主循环 — 轮询所有到期任务
 *============================================================================*/
void Scheduler_Run(void)
{
    uint32_t current_tick = BM_GetTick();

    for (uint32_t i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if (!s_tasks[i].registered || !s_tasks[i].enable || s_tasks[i].func == NULL) {
            continue;
        }

        uint32_t elapsed = current_tick - s_tasks[i].last_run;

        if (elapsed >= s_tasks[i].period_ms) {
            s_tasks[i].last_run += s_tasks[i].period_ms;
            s_tasks[i].func();
        }
    }
}

/*==============================================================================
 * 动态修改任务周期
 *============================================================================*/
uint8_t Scheduler_SetPeriod(TaskHandle_t id, uint32_t new_period_ms)
{
    if ((uint32_t)id >= SCHEDULER_MAX_TASKS || !s_tasks[id].registered) {
        return 0;
    }

    if (new_period_ms < 1) {
        new_period_ms = 1;
    }

    s_tasks[id].period_ms = new_period_ms;
    s_tasks[id].last_run  = BM_GetTick();   /* 重置计时, 防止立即触发 */

    return 1;
}

/*==============================================================================
 * 获取任务周期
 *============================================================================*/
uint32_t Scheduler_GetPeriod(TaskHandle_t id)
{
    if ((uint32_t)id >= SCHEDULER_MAX_TASKS || !s_tasks[id].registered) {
        return 0;
    }
    return s_tasks[id].period_ms;
}

/*==============================================================================
 * 启用 / 禁用任务
 *============================================================================*/
uint8_t Scheduler_EnableTask(TaskHandle_t id, uint8_t enable)
{
    if ((uint32_t)id >= SCHEDULER_MAX_TASKS || !s_tasks[id].registered) {
        return 0;
    }
    s_tasks[id].enable = enable ? 1 : 0;
    return 1;
}

/*==============================================================================
 * 查询任务是否在运行
 *============================================================================*/
uint8_t Scheduler_IsTaskRunning(TaskHandle_t id)
{
    if ((uint32_t)id >= SCHEDULER_MAX_TASKS || !s_tasks[id].registered) {
        return 0;
    }
    return s_tasks[id].enable;
}
