/**
 * @file    encoder.c
 * @brief   增量式旋转编码器驱动实现 (AT32F403A TMR4 硬件正交解码)
 *
 * @details 配置要点:
 *            1. PB6/PB7 配成 GPIO_MODE_INPUT + 上拉. 编码器是"开关对地"型时
 *               必须有上拉; 若是推挽输出的光电/霍尔编码器, 上拉也无害.
 *               注意: 输入捕获只需要 GPIO 处于输入模式, 不需要 GPIO_MODE_MUX
 *               (TMR4 的 CH1/CH2 是这两个脚的默认复用通路).
 *            2. TMR4 计数器 16 位, PR 设 0xFFFF, 自由回绕, 不做中断.
 *            3. 编码器模式 C (SMS=011): TI1 与 TI2 的上升沿+下降沿都计数,
 *               即四倍频, 分辨率最高, 也不会因单边沿丢失方向信息.
 *            4. 输入滤波 0x0F: 机械触点抖动用硬件滤掉, 不占 CPU.
 *
 * @note    为什么不用中断: 编码器只是人机输入, 10ms 轮询一次足够; 中断反而
 *          在快速转动时频繁打断, 收益为负. 计数由硬件累加, 不存在"漏采".
 */
#include "encoder.h"

#include "at32f403a_407.h"
#include "at32f403a_407_crm.h"
#include "at32f403a_407_gpio.h"
#include "at32f403a_407_tmr.h"

/*==============================================================================
 * 硬件映射 — 换定时器/引脚只改这一段
 *============================================================================*/
#define ENC_TMR                 TMR4
#define ENC_TMR_CLK             CRM_TMR4_PERIPH_CLOCK

#define ENC_PORT                GPIOB
#define ENC_PORT_CLK            CRM_GPIOB_PERIPH_CLOCK
#define ENC_PINS                (GPIO_PINS_6 | GPIO_PINS_7)   /* PB6=CH1 PB7=CH2 */

#define ENC_CNT_PERIOD          0xFFFFU     /* TMR4 为 16 位计数器 */

/*==============================================================================
 * 内部状态
 *============================================================================*/
static uint16_t s_last_raw = 0;    /* 上次采样到的硬件计数值 */
static int32_t  s_count    = 0;    /* 累计计数 (带符号)     */

/*==============================================================================
 * 初始化
 *============================================================================*/
void Encoder_Init(void)
{
    gpio_init_type gi;

    /* 1. 时钟: GPIOB + TMR4 (TMR4 挂 APB1, 时钟由 CRM 使能) */
    crm_periph_clock_enable(ENC_PORT_CLK, TRUE);
    crm_periph_clock_enable(ENC_TMR_CLK, TRUE);

    /* 2. 两路输入: 输入模式 + 内部上拉 (编码器公共端接地, 靠上拉拉高) */
    gpio_default_para_init(&gi);
    gi.gpio_pins           = ENC_PINS;
    gi.gpio_mode           = GPIO_MODE_INPUT;
    gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gi.gpio_pull           = GPIO_PULL_UP;
    gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_init(ENC_PORT, &gi);

    /* 3. 计数器时基: 周期 0xFFFF (满量程), 不分频 */
    tmr_base_init(ENC_TMR, ENC_CNT_PERIOD, 0);
    tmr_cnt_dir_set(ENC_TMR, TMR_COUNT_UP);

    /* 4. 编码器模式 C = TI1/TI2 双边沿四倍频.
     *    第一路极性决定计数方向; ENC_DIR_INVERT=1 时把 CH1 反相即可反向. */
#if (ENC_DIR_INVERT)
    tmr_encoder_mode_config(ENC_TMR, TMR_ENCODER_MODE_C,
                            TMR_INPUT_FALLING_EDGE, TMR_INPUT_RISING_EDGE);
#else
    tmr_encoder_mode_config(ENC_TMR, TMR_ENCODER_MODE_C,
                            TMR_INPUT_RISING_EDGE, TMR_INPUT_RISING_EDGE);
#endif

    /* 5. 输入数字滤波 (库函数未覆盖这两个位, 直接写寄存器) */
    ENC_TMR->cm1_input_bit.c1df = ENC_IC_FILTER;
    ENC_TMR->cm1_input_bit.c2df = ENC_IC_FILTER;

    /* 6. 启动计数器, 并以当前值为增量基准 */
    tmr_counter_enable(ENC_TMR, TRUE);

    s_last_raw = (uint16_t)tmr_counter_value_get(ENC_TMR);
    s_count    = 0;
}

/*==============================================================================
 * 采样
 *============================================================================*/
int16_t Encoder_ReadDelta(void)
{
    uint16_t raw = (uint16_t)tmr_counter_value_get(ENC_TMR);

    /* 16 位回绕: 无符号相减后强转 int16, 跨 0x0000/0xFFFF 也正确 */
    int16_t delta = (int16_t)(raw - s_last_raw);

    s_last_raw = raw;
    s_count   += delta;

    return delta;
}

int32_t Encoder_GetCount(void)
{
    return s_count;
}

void Encoder_ResetCount(void)
{
    /* 只清软件侧基准: 硬件计数器继续自由跑, 避免写 CNT 与计数边沿竞争 */
    s_last_raw = (uint16_t)tmr_counter_value_get(ENC_TMR);
    s_count    = 0;
}

uint16_t Encoder_ReadRaw(void)
{
    return (uint16_t)tmr_counter_value_get(ENC_TMR);
}
