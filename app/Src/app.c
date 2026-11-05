/**
 * @file    app.c
 * @brief   应用层实现 —— LED 流水灯 + 按键调速 (BM Arch on AT32F403A)
 *
 * @details 演示 BM Arch 的两条主路径:
 *            1. 周期任务:  Scheduler 按 APP_LED_PERIOD_* 调 App_Led_Task()
 *            2. 异步事件:  EXINT0 ISR 只投递 MSG_BUTTON_PRESSED,
 *                          由 App_Event_Process() 在任务上下文调用
 *                          App_OnButtonPressed() 做业务处理
 */
#include "app.h"
#include "app_events.h"
#include "bm_hal.h"
#include "scheduler.h"
#include "msg_queue.h"
#include "logger.h"

#include "at32f403a_407.h"
#include "at32f403a_407_board.h"

/*==============================================================================
 * 内部状态
 *============================================================================*/
static uint8_t  s_led_idx    = 0;       /* 当前点亮的 LED 序号         */
static uint8_t  s_speed      = 1;       /* 1=快档 0=慢档               */
static uint32_t s_last_press = 0;       /* 上次有效按下的 Tick (消抖)  */

/*==============================================================================
 * 按键 EXINT 初始化
 *============================================================================*/
static void App_ButtonExtiInit(void)
{
    exint_init_type exint_init_struct;

    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    gpio_exint_line_config(GPIO_PORT_SOURCE_GPIOA, GPIO_PINS_SOURCE0);

    exint_default_para_init(&exint_init_struct);
    exint_init_struct.line_enable   = TRUE;
    exint_init_struct.line_mode     = EXINT_LINE_INTERRUPT;
    exint_init_struct.line_select   = EXINT_LINE_0;
    exint_init_struct.line_polarity = EXINT_TRIGGER_RISING_EDGE;
    exint_init(&exint_init_struct);

    nvic_irq_enable(EXINT0_IRQn, 1, 0);
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void App_Init(void)
{
    App_Event_Init();       /* 消息队列初始化 (框架 app_events.c) */
    App_ButtonExtiInit();
}

/*==============================================================================
 * 周期任务 — LED 流水灯
 *============================================================================*/
void App_Led_Task(void)
{
    at32_led_toggle((led_type)s_led_idx);

    s_led_idx++;
    if (s_led_idx >= LED_NUM) {
        s_led_idx = 0;
    }
}

/*==============================================================================
 * 异步事件 — 按键按下 (任务上下文, 允许阻塞/打印)
 *============================================================================*/
void App_OnButtonPressed(void)
{
    uint32_t now    = BM_GetTick();
    uint32_t period;

    /* 软件消抖: 机械抖动会在 EXINT 上打出多个上升沿 */
    if ((s_last_press != 0U) && ((now - s_last_press) < APP_BTN_DEBOUNCE_MS)) {
        return;
    }
    s_last_press = (now != 0U) ? now : 1U;

    s_speed = (uint8_t)(s_speed ? 0U : 1U);
    period  = s_speed ? APP_LED_PERIOD_FAST : APP_LED_PERIOD_SLOW;

    Scheduler_SetPeriod(TASK_LED, period);

    LOG_INFO("APP", "button pressed -> %s, period = %u ms",
             s_speed ? "FAST" : "SLOW", (unsigned)period);
}

/*==============================================================================
 * EXINT0 中断 — 只清标志 + 投递消息, 不写业务
 *============================================================================*/
void EXINT0_IRQHandler(void)
{
    exint_flag_clear(EXINT_LINE_0);
    MsgQueue_SendSimple(MSG_BUTTON_PRESSED);
}
