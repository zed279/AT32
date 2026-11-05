/**
 * @file    app.h
 * @brief   应用层接口 (BM Arch on AT32F403A)
 *
 * @details 应用层只通过调度任务与消息事件与框架交互:
 *            - 周期任务  App_Led_Task()         ← Scheduler 轮询
 *            - 异步事件  App_OnButtonPressed()  ← App_Event_Process() 分发
 *          中断里绝不写业务, 只投递消息。
 */
#ifndef __APP_H
#define __APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*==============================================================================
 * 应用参数
 *============================================================================*/
#define APP_LED_PERIOD_FAST     100U    /* 快档: LED 切换周期 (ms) */
#define APP_LED_PERIOD_SLOW     400U    /* 慢档: LED 切换周期 (ms) */
#define APP_BTN_DEBOUNCE_MS     200U    /* 按键软件消抖窗口 (ms)   */

/*==============================================================================
 * API
 *============================================================================*/
void App_Init(void);             /* 事件模块 + 按键 EXINT 初始化 */
void App_Led_Task(void);         /* 调度任务: LED 流水灯        */
void App_OnButtonPressed(void);  /* 事件处理: 按键切换速度档     */

#ifdef __cplusplus
}
#endif
#endif /* __APP_H */
