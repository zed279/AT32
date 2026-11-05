/**
 * @file    app_events.h
 * @brief   事件处理器 (消息队列消费者)
 *
 * @details 从消息队列中取出消息并分发处理。
 *          作为调度器的一个周期任务运行, 每轮最多处理若干条消息,
 *          避免长时间占用 CPU。
 *
 * @note    用户需根据自身应用修改 App_Event_Process() 中的 switch-case 分支。
 */
#ifndef __APP_EVENTS_H
#define __APP_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "msg_queue.h"

/*==============================================================================
 * API
 *============================================================================*/
void App_Event_Init(void);              /* 初始化事件模块           */
void App_Event_Process(Message_t *msg); /* 处理单条消息             */
void App_Event_ProcessAll(void);        /* 排空队列中所有消息       */

#ifdef __cplusplus
}
#endif
#endif /* __APP_EVENTS_H */
