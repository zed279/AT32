/**
 * @file    app_events.c
 * @brief   事件处理器实现
 *
 * @details 从消息队列消费消息并分发处理。
 *          每轮最多处理 APP_EVENT_BATCH_SIZE 条消息, 避免长时间占用调度器。
 *
 * @note    用户请根据自身应用修改 App_Event_Process() 中的 switch-case。
 */
#include "app_events.h"
#include "app.h"
#include "bm_hal.h"
#include "logger.h"
#include <stdio.h>
#include <stdbool.h>

/*==============================================================================
 * 配置 — 每轮最多处理的消息条数
 *============================================================================*/
#ifndef APP_EVENT_BATCH_SIZE
#define APP_EVENT_BATCH_SIZE  5
#endif

/*==============================================================================
 * 初始化
 *============================================================================*/
void App_Event_Init(void)
{
    MsgQueue_Init();
    LOG_INFO("EVT", "Event module initialized");
}

/*==============================================================================
 * 处理单条消息
 *
 * @note 用户根据应用需求修改此函数的 switch-case 分支
 *============================================================================*/
void App_Event_Process(Message_t *msg)
{
    if (msg == NULL) return;

    switch(msg->type) {

        /* ---- 示例: 频率变化 ---- */
        case MSG_FREQ_CHANGED: {
            /* uint32_t freq = *(uint32_t*)msg->data; */
            LOG_INFO("APP", "Frequency changed");
            break;
        }

        /* ---- 示例: 搜台开始 ---- */
        case MSG_SEEK_START:
            LOG_INFO("APP", "Seek started...");
            break;

        /* ---- 示例: 搜台完成 ---- */
        case MSG_SEEK_DONE: {
            /* uint32_t freq = *(uint32_t*)msg->data; */
            LOG_INFO("APP", "Seek done!");
            break;
        }

        /* ---- 示例: 信号更新 (节流打印) ---- */
        case MSG_RSSI_UPDATE: {
            static uint32_t last_print_time = 0;
            uint32_t now = BM_GetTick();

            if (now - last_print_time >= 5000) {
                last_print_time = now;
                /* int16_t rssi = *(int16_t*)msg->data; */
                LOG_INFO("APP", "RSSI updated");
            }
            break;
        }

        /* ---- 示例: 立体声变化 ---- */
        case MSG_STEREO_CHANGED: {
            /* bool stereo = *(bool*)msg->data; */
            LOG_INFO("APP", "Stereo changed");
            break;
        }

        /* ---- 示例: 静音 ---- */
        case MSG_MUTE:
            LOG_INFO("APP", "Muted");
            break;

        /* ---- 示例: 取消静音 ---- */
        case MSG_UNMUTE:
            LOG_INFO("APP", "Unmuted");
            break;

        /* ---- 应用: 按键按下 (业务实现在 app.c) ---- */
        case MSG_BUTTON_PRESSED:
            App_OnButtonPressed();
            break;

        /* ---- 未知消息 ---- */
        default:
            LOG_WARN("APP", "Unknown msg: %d", msg->type);
            break;
    }
}

/*==============================================================================
 * 排空队列 — 作为调度器周期任务调用
 *============================================================================*/
void App_Event_ProcessAll(void)
{
    Message_t msg;
    uint8_t processed = 0;

    while (MsgQueue_Recv(&msg) && processed < APP_EVENT_BATCH_SIZE) {
        App_Event_Process(&msg);
        processed++;
    }
}
