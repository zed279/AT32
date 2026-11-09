/**
 * @file    scheduler.h
 * @brief   协作式任务调度器 (无 RTOS)
 *
 * @details 基于时间片轮询的非抢占式调度器。
 *          通过 BM_GetTick() 获取系统时基，按注册周期定时触发任务。
 *          适用于裸机 (Bare-Metal) 系统，无需操作系统支持。
 */
#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*==============================================================================
 * 配置 — 最大任务数 (按需修改)
 *
 * @warning 这个值必须 >= 下面 TaskHandle_t 枚举的成员个数 (TASK_MAX).
 *          一旦小于, Scheduler_RegisterTask() 对超界的 id 直接 return 0,
 *          **不报错、不打印、不留痕迹**, 表现为"任务函数写了但从来不执行",
 *          极易误判成外设/驱动问题. scheduler.c 末尾有一处编译期断言兜底.
 *============================================================================*/
/* 留 4 个余量: 原来的 8 个槽位被 TaskHandle_t 枚举 (TASK_0..TASK_KT = 8 个)
 * 刚好占满, 再加一个任务时 Scheduler_RegisterTask() 会因 id >= 容量而
 * 静默返回 0 —— 现象是"任务注册了但永远不跑", 极难定位。 */
#ifndef SCHEDULER_MAX_TASKS



#define SCHEDULER_MAX_TASKS  12

#endif

/*==============================================================================
 * 任务句柄 (用户自定义枚举, 最后一个成员作为 TASK_MAX)
 *============================================================================*/
typedef enum {
    TASK_0 = 0,
    TASK_1,
    TASK_2,
    TASK_EVENT,             /* 框架: 消息队列排空 (App_Event_ProcessAll) */
    TASK_LCD,               /* 应用: LCD 动画 (每帧渲染一次, 已停用) */
    TASK_ENCODER,           /* 应用: 编码器采样 + LCD 换色验证 (已停用) */
    TASK_KT,                /* 应用: KT06xx 初始化结果刷新 + 失败重试 */
    TASK_UI,                /* 应用: SGL 图形界面 (周期 = CONFIG_SGL_SYSTICK_MS 10ms) */
    /* ... 在此添加更多任务 ... */
    TASK_MAX                /* 必须放最后, 自动表示任务总数 */
} TaskHandle_t;

/* 编译期容量检查 —— 枚举常量在预处理阶段不可见, 所以用"负长度数组"报错:
 * 一旦 TASK_MAX > SCHEDULER_MAX_TASKS, 这里编译失败, 而不是等到运行期
 * Scheduler_RegisterTask() 静默返回 0。 */
extern char scheduler_task_count_check_t[(TASK_MAX <= SCHEDULER_MAX_TASKS) ? 1 : -1];

/*==============================================================================
 * 任务函数指针
 *============================================================================*/
typedef void (*TaskFunc_t)(void);

/*==============================================================================
 * 任务控制块
 *============================================================================*/
typedef struct {
    TaskFunc_t func;          /* 任务函数指针       */
    uint32_t   period_ms;     /* 执行周期 (ms)     */
    uint32_t   last_run;      /* 上次执行的 Tick    */
    uint8_t    enable;        /* 1=启用, 0=禁用    */
    uint8_t    registered;    /* 1=已注册           */
} TaskCtrlBlock_t;

/*==============================================================================
 * API
 *============================================================================*/
void     Scheduler_Init(void);
uint8_t  Scheduler_RegisterTask(TaskHandle_t id, TaskFunc_t func, uint32_t period_ms);
void     Scheduler_Run(void);
uint8_t  Scheduler_SetPeriod(TaskHandle_t id, uint32_t new_period_ms);
uint32_t Scheduler_GetPeriod(TaskHandle_t id);
uint8_t  Scheduler_EnableTask(TaskHandle_t id, uint8_t enable);
uint8_t  Scheduler_IsTaskRunning(TaskHandle_t id);

#ifdef __cplusplus
}
#endif
#endif /* __SCHEDULER_H */
