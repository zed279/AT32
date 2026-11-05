/**
 * @file    bm_hal.c
 * @brief   BM Arch 系统抽象层实现 —— Artery AT32F403A / AT32F407
 *
 * @details 实现 bm_hal.h 的 4 个契约函数:
 *            BM_GetTick()       —— 1ms 递增计数, 允许 uint32 回绕
 *            BM_Delay()         —— 阻塞延时
 *            BM_EnterCritical() —— 关全局中断
 *            BM_ExitCritical()  —— 开全局中断
 *
 * @note    时基源为什么不用 SysTick:
 *          AT32 BSP 的 delay_us()/delay_ms() 把 SysTick 当作"一次性倒计时"
 *          使用 (改写 LOAD/VAL, 轮询 COUNTFLAG), 若再把 SysTick 配成 1ms
 *          周期中断, 两者会互相破坏。因此这里改用一个独立硬件定时器 TMR2
 *          产生 1ms 中断, 与 BSP 的延时函数完全解耦。
 *
 * @note    中断向量名由启动文件固定为 TMR2_GLOBAL_IRQHandler,
 *          换用其它定时器时需同步修改 BM_TICK_ISR 宏。
 */
#include "bm_hal.h"
#include "bm_hal_port.h"

#include "at32f403a_407.h"
#include "at32f403a_407_board.h"
#include "at32f403a_407_crm.h"
#include "at32f403a_407_tmr.h"
#include "at32f403a_407_misc.h"

/*==============================================================================
 * 配置 — 时基定时器 (默认 TMR2 / APB1)
 *============================================================================*/
#ifndef BM_TICK_TMR
#define BM_TICK_TMR            TMR2
#endif

#ifndef BM_TICK_TMR_IRQn
#define BM_TICK_TMR_IRQn       TMR2_GLOBAL_IRQn
#endif

#ifndef BM_TICK_TMR_CLK
#define BM_TICK_TMR_CLK        CRM_TMR2_PERIPH_CLOCK
#endif

#ifndef BM_TICK_ISR
#define BM_TICK_ISR            TMR2_GLOBAL_IRQHandler
#endif

/* 计数频率 1MHz, 周期 1000 计数 => 1ms */
#define BM_TICK_CNT_HZ         1000000U
#define BM_TICK_PERIOD_CNT     (BM_TICK_CNT_HZ / 1000U)

/*==============================================================================
 * 内部状态
 *============================================================================*/
static volatile uint32_t s_tick_ms   = 0;   /* 毫秒计数 (ISR 递增)      */
static volatile uint32_t s_crit_nest = 0;   /* 临界区嵌套计数           */

/*==============================================================================
 * 时基初始化
 *============================================================================*/
void BM_HAL_TickInit(void)
{
    crm_clocks_freq_type clocks;
    uint32_t tmr_clk;
    uint32_t div;

    /* 1. 取运行时总线频率 (system_clock_config 之后才准确) */
    crm_clocks_freq_get(&clocks);

    /* 2. APB1 分频 != 1 时, 定时器时钟 = APB1 x 2 */
    tmr_clk = (clocks.ahb_freq == clocks.apb1_freq)
              ? clocks.apb1_freq
              : (clocks.apb1_freq * 2U);

    /* 3. 分频到 1MHz, 保证 1 计数 = 1us */
    if (tmr_clk < BM_TICK_CNT_HZ) {
        /* 时钟过低时无法做 1ms 时基, 保持静默失败, 由上层日志暴露 */
        return;
    }
    div = (tmr_clk / BM_TICK_CNT_HZ) - 1U;

    /* 4. 配置 TMR2: 向上计数, 1000 计数溢出 = 1ms */
    crm_periph_clock_enable(BM_TICK_TMR_CLK, TRUE);

    tmr_base_init(BM_TICK_TMR, BM_TICK_PERIOD_CNT - 1U, div);
    tmr_cnt_dir_set(BM_TICK_TMR, TMR_COUNT_UP);
    tmr_flag_clear(BM_TICK_TMR, TMR_OVF_FLAG);
    tmr_interrupt_enable(BM_TICK_TMR, TMR_OVF_INT, TRUE);

    nvic_irq_enable(BM_TICK_TMR_IRQn, 0, 0);
    tmr_counter_enable(BM_TICK_TMR, TRUE);
}

/*==============================================================================
 * 时基中断 — 只做计数, 不放任何业务 (ISR 里禁止阻塞/打印)
 *============================================================================*/
void BM_TICK_ISR(void)
{
    if (tmr_flag_get(BM_TICK_TMR, TMR_OVF_FLAG) != RESET) {
        tmr_flag_clear(BM_TICK_TMR, TMR_OVF_FLAG);
        s_tick_ms++;
    }
}

/*==============================================================================
 * 契约实现
 *============================================================================*/
uint32_t BM_GetTick(void)
{
    /* Cortex-M4 的 32 位对齐读取是原子的, 无需临界区 */
    return s_tick_ms;
}

void BM_Delay(uint32_t ms)
{
    /* 复用 AT32 BSP 的 SysTick 轮询延时:
     * 与本文件使用的 TMR2 时基互不干扰, 且在 BM_HAL_TickInit() 之前也可用。 */
    while (ms > 0U) {
        uint32_t chunk = (ms > 1000U) ? 1000U : ms;
        delay_ms((uint16_t)chunk);
        ms -= chunk;
    }
}

void BM_EnterCritical(void)
{
    __disable_irq();
    s_crit_nest++;
}

void BM_ExitCritical(void)
{
    if (s_crit_nest > 0U) {
        s_crit_nest--;
    }
    if (s_crit_nest == 0U) {
        __enable_irq();
    }
}
