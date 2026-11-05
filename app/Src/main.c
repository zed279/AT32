/**
 * @file    main.c
 * @brief   BM Arch 裸机架构入口 —— Artery AT32F403A / AT32F407
 *
 * @details 系统启动顺序 (顺序不能换):
 *            1. system_clock_config()  时钟树 (240MHz sclk / 120MHz apb1)
 *            2. at32_board_init()      板级 LED / 按键 / 延时函数
 *            3. BM_HAL_TickInit()      1ms 时基 (TMR2), 框架一切时间基准
 *            4. Scheduler_Init()       调度器
 *            5. KtDemo_Init()          两组 KT 芯片初始化 + LCD 结果界面
 *            6. 任务注册 + while(1) Scheduler_Run()
 *
 * @note  **本板不初始化调试串口**。原第 3 步的 uart_print_init() 已删除，
 *        原因：AT32 BSP 的打印串口固定是 USART1，TX = PA9，而 PA9 在本板
 *        被用作组2 KT 芯片的 SCL（KT_SCL_2）。若保留该调用，PA9 会被配成
 *        USART1 的复用推挽输出，软件 IIC 的时序会被打断。
 *        板上也没引出串口，日志已全部关闭（见 logger_cfg.h）。
 *
 * @note  main.c 只做"初始化 + 任务注册 + 主循环", 业务逻辑在 app.c,
 *        事件分发在 app_events.c。框架文件 (scheduler/msg_queue/...)
 *        与本文件无关, 换 MCU 时不需要动它们。
 */
#include "at32f403a_407.h"
#include "at32f403a_407_board.h"
#include "at32f403a_407_clock.h"

#include "bm_hal_port.h"
#include "scheduler.h"
#include "msg_queue.h"
#include "app_events.h"
#include "app.h"
#include "kt_demo.h"
#include "ui_demo.h"
/* #include "encoder_demo.h" */   /* 编码器 Demo 已停用, 让位给 KT 界面 */
/* #include "lcd_demo.h" */       /* LCD 跑分 Demo 已停用 (被 SGL 取代) */

/*==============================================================================
 * 应用参数
 *============================================================================*/
#define APP_EVENT_PERIOD_MS     10U     /* 事件队列排空周期 (ms) */
#define UI_DEMO_PERIOD_MS       10U     /* SGL 任务周期, 必须 = CONFIG_SGL_SYSTICK_MS */

/*==============================================================================
 * 主函数
 *============================================================================*/
int main(void)
{
    int ui_rc;

    /* ---- 1. 时钟 / 板级 ---- */
    system_clock_config();
    at32_board_init();

    /* NVIC 优先级分组只配置一次 (4 位抢占, 0 位子优先级) */
    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

    /* ---- 2. 框架时基: 必须早于任何 BM_GetTick() 使用者 ---- */
    BM_HAL_TickInit();

    /* ---- 3. 框架与应用初始化 ---- */
    Scheduler_Init();
    App_Init();                     /* 消息队列 + 按键 EXINT */

    /* ---- 4. 两组 KT 接收芯片初始化 + LCD 结果界面 ---- */
    /* 两组各走一遍：上电 → 挂总线 → probe → init → tune，耗时约 1~3s。
     * 失败组会在调度器启动后由 TASK_KT 每 5s 重试。 */
    if (KtDemo_Init() != 0)
    {
        /* 屏幕初始化失败时无任何可见输出（日志已关），只能用 LED 提示 */
        while (1)
        {
            at32_led_toggle(LED2);
            delay_ms(200);
        }
    }

    /* ---- 5. SGL 图形界面: LCD 的最终使用者, 必须排在 KtDemo 之后 ---- */
    /* 顺序原因: KtDemo_Init() 内部会自己 st7789_init() 并画一屏 KT 结果,
     * SGL 若先初始化会被它覆盖. 这里让 SGL 最后接管屏幕.
     * 注意: SGL 接管后, 若某组 KT 芯片仍不可用, TASK_KT 每 5s 重试时会用
     * 裸 st7789 接口重画整屏, 会把 SGL 画面盖掉. 需要 SGL 独占显示时,
     * 把下面 TASK_KT 的注册注释掉即可 (KT 芯片仍会被初始化一次). */
    ui_rc = UiDemo_Init();
    if (ui_rc != 0)
    {
        /* 本板没有可用调试串口, 失败原因只能靠 LED 闪的次数读出来:
         *   1 次 = st7789_init 失败
         *   2 次 = 屏幕方向/宽度不符 (SGL 行缓冲按 240 分配, 不许横屏 320)
         *   3 次 = sgl_fbdev_register 失败
         *   4 次 = sgl_init 失败
         *   5 次 = 页面创建失败 (通常是 SGL 堆不够)
         * 需要精确值时在 ui_rc 上打断点. */
        while (1)
        {
            int n = (ui_rc < 0) ? -ui_rc : 1;

            for (int i = 0; i < n; i++)
            {
                at32_led_toggle(LED2);
                delay_ms(120);
                at32_led_toggle(LED2);
                delay_ms(120);
            }
            delay_ms(1200);
        }
    }

    /* ---- 6. 任务注册 ---- */
    /* 注册返回值必须检查: 槽位不足或函数指针为空时它返回 0 且**不报错**,
     * 任务从此永不执行. TASK_UI 就曾因为排到第 9 个 (SCHEDULER_MAX_TASKS 是 8)
     * 而注册失败, 现象是"界面能显示(第一帧是同步刷的)但旋钮按键全无反应". */
    {
        uint8_t ok = 1;

        ok &= Scheduler_RegisterTask(TASK_LED,   App_Led_Task,         APP_LED_PERIOD_FAST);
        ok &= Scheduler_RegisterTask(TASK_EVENT, App_Event_ProcessAll, APP_EVENT_PERIOD_MS);
        /* TASK_KT 已停用: 它每 5s 会用裸 st7789 重画整屏, 把 SGL 画面盖掉 */
        /* ok &= Scheduler_RegisterTask(TASK_KT, KtDemo_Task, KT_DEMO_PERIOD_MS); */
        ok &= Scheduler_RegisterTask(TASK_UI,    UiDemo_Task,          UI_DEMO_PERIOD_MS);

        if (!ok)
        {
            /* 连闪 6 下 = 任务注册失败 (与 UiDemo_Init 的 1~5 次区分) */
            while (1)
            {
                for (int i = 0; i < 6; i++)
                {
                    at32_led_toggle(LED2);
                    delay_ms(120);
                    at32_led_toggle(LED2);
                    delay_ms(120);
                }
                delay_ms(1200);
            }
        }
    }

    /* ---- 7. 协作式主循环 ---- */
    while (1)
    {
        Scheduler_Run();
    }
}
