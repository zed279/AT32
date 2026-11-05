/**
 * @file    gpio_port_at32.c
 * @brief   可复用 GPIO 驱动模板 - Artery AT32F403A/407 移植实现
 *
 * @note    本文件实现 gpio_port.h 声明的 7 个 port 函数, 覆盖 gpio_port.c 的
 *          弱定义桩. 与 gpio.c (通用层) 的分工不变: 本文件只管寄存器.
 *
 *          与 STM32 版 (gpio_port_stm32_hal.c) 的差异:
 *            - 时钟: crm_periph_clock_enable(CRM_GPIOx_PERIPH_CLOCK, TRUE)
 *            - 配置: gpio_init(GPIOx, &gpio_init_type)  (复数 gpio_pins 字段)
 *            - 输出: gpio_bits_set / gpio_bits_reset  (BSRE/BCRE, 原子操作)
 *            - 翻转: gpio_bits_toggle
 *            - 中断: exint + gpio_exint_line_config(端口源, 引脚源)
 *
 * @note    端口范围: AT32F403A/407 只有 GPIOA~GPIOE.
 *          中断分发需由用户在 EXINTx_IRQHandler 中调用
 *          gpio_irq_handler(hw_handle), 本工程按键走 app.c 的 EXINT0.
 */
#include "gpio_port.h"
#include <stddef.h>
#include "at32f403a_407.h"

/* ============================ 内部: 句柄解码 ============================ */

static gpio_type *hw_to_port(uint32_t h)
{
    switch ((h >> 8) & 0xFF) {
        case GPIO_PORT_A: return GPIOA;
        case GPIO_PORT_B: return GPIOB;
        case GPIO_PORT_C: return GPIOC;
        case GPIO_PORT_D: return GPIOD;
        case GPIO_PORT_E: return GPIOE;
        default:          return NULL;
    }
}

static uint16_t hw_to_pin_mask(uint32_t h)
{
    return (uint16_t)(1u << (h & 0xFF));
}

static gpio_status_t enable_port_clock(gpio_type *port)
{
         if (port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    else if (port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    else if (port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    else if (port == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    else if (port == GPIOE) crm_periph_clock_enable(CRM_GPIOE_PERIPH_CLOCK, TRUE);
    else return GPIO_ERR_HW;
    return GPIO_OK;
}

static gpio_pull_type to_pull(gpio_pull_t pull)
{
    switch (pull) {
        case GPIO_PU_UP:   return GPIO_PULL_UP;
        case GPIO_PU_DOWN: return GPIO_PULL_DOWN;
        default:           return GPIO_PULL_NONE;
    }
}

/* ============================ port 函数实现 ============================ */

gpio_status_t gpio_port_init(const gpio_hw_pin_t *hw, const gpio_config_t *cfg)
{
    gpio_init_type gi;

    if (hw == NULL || cfg == NULL) {
        return GPIO_ERR_PARAM;
    }

    gpio_type *port = hw_to_port(hw->hw_handle);
    uint16_t   mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }

    gpio_status_t st = enable_port_clock(port);
    if (st != GPIO_OK) {
        return st;
    }

    gpio_default_para_init(&gi);
    gi.gpio_pins           = mask;
    gi.gpio_pull           = to_pull(cfg->pull);
    gi.gpio_drive_strength = (cfg->speed == 0) ? GPIO_DRIVE_STRENGTH_MODERATE
                                               : GPIO_DRIVE_STRENGTH_STRONGER;

    switch (cfg->dir) {
        case GPIO_DIR_INPUT:
            gi.gpio_mode = GPIO_MODE_INPUT;
            gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
            break;
        case GPIO_DIR_OUTPUT:
            gi.gpio_mode = GPIO_MODE_OUTPUT;
            gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
            break;
        case GPIO_DIR_OD:
            gi.gpio_mode = GPIO_MODE_OUTPUT;
            gi.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
            break;
        case GPIO_DIR_ANALOG:
            gi.gpio_mode = GPIO_MODE_ANALOG;
            gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
            break;
        default:
            return GPIO_ERR_PARAM;
    }

    gpio_init(port, &gi);

    /* 输出模式: 设定初始电平 (先配好模式再写电平, 避免上电毛刺) */
    if (cfg->dir == GPIO_DIR_OUTPUT || cfg->dir == GPIO_DIR_OD) {
        if (cfg->init_state == GPIO_HIGH) {
            gpio_bits_set(port, mask);
        } else {
            gpio_bits_reset(port, mask);
        }
    }
    return GPIO_OK;
}

gpio_status_t gpio_port_deinit(const gpio_hw_pin_t *hw)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    gpio_type *port = hw_to_port(hw->hw_handle);
    uint16_t   mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }
    gpio_reset(port);
    (void)mask;
    return GPIO_OK;
}

gpio_status_t gpio_port_set_dir(const gpio_hw_pin_t *hw, gpio_dir_t dir)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    /* 复用 init 路径重配方向, 上下拉保持 NONE, 不动当前电平 */
    gpio_config_t cfg;
    cfg.pin        = hw->user_pin;
    cfg.dir        = dir;
    cfg.pull       = GPIO_PU_NONE;
    cfg.init_state = GPIO_LOW;
    cfg.speed      = 0;
    return gpio_port_init(hw, &cfg);
}

gpio_status_t gpio_port_set_level(const gpio_hw_pin_t *hw, gpio_state_t state)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    gpio_type *port = hw_to_port(hw->hw_handle);
    uint16_t   mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }

    if (state == GPIO_HIGH) {
        gpio_bits_set(port, mask);
    } else {
        gpio_bits_reset(port, mask);
    }
    return GPIO_OK;
}

gpio_state_t gpio_port_read(const gpio_hw_pin_t *hw)
{
    if (hw == NULL) {
        return GPIO_LOW;
    }
    gpio_type *port = hw_to_port(hw->hw_handle);
    uint16_t   mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_LOW;
    }
    return (gpio_input_data_bit_read(port, mask) != RESET) ? GPIO_HIGH : GPIO_LOW;
}

gpio_status_t gpio_port_toggle(const gpio_hw_pin_t *hw)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    gpio_type *port = hw_to_port(hw->hw_handle);
    uint16_t   mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }
    /* AT32 有专门的翻转寄存器 (odt ^= pins), 单条指令原子完成 */
    gpio_bits_toggle(port, mask);
    return GPIO_OK;
}

gpio_status_t gpio_port_int_config(const gpio_hw_pin_t *hw, gpio_int_mode_t mode)
{
    exint_init_type exint_struct;
    uint32_t pin_pos;

    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    gpio_type *port = hw_to_port(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }

    pin_pos = hw->hw_handle & 0xFFu;

    if (mode == GPIO_INT_NONE) {
        /* 关闭 EXINT 线并禁用 NVIC */
        exint_default_para_init(&exint_struct);
        exint_struct.line_select   = (EXINT_LINE_0 << pin_pos);
        exint_struct.line_enable   = FALSE;
        exint_init(&exint_struct);
        return GPIO_OK;
    }

    /* 端口源只支持 A~E (AT32F403A 限制) */
    gpio_port_source_type port_src;
    switch ((hw->hw_handle >> 8) & 0xFF) {
        case GPIO_PORT_A: port_src = GPIO_PORT_SOURCE_GPIOA; break;
        case GPIO_PORT_B: port_src = GPIO_PORT_SOURCE_GPIOB; break;
        case GPIO_PORT_C: port_src = GPIO_PORT_SOURCE_GPIOC; break;
        case GPIO_PORT_D: port_src = GPIO_PORT_SOURCE_GPIOD; break;
        case GPIO_PORT_E: port_src = GPIO_PORT_SOURCE_GPIOE; break;
        default:          return GPIO_ERR_HW;
    }
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    gpio_exint_line_config(port_src, (gpio_pins_source_type)pin_pos);

    exint_default_para_init(&exint_struct);
    exint_struct.line_select = (EXINT_LINE_0 << pin_pos);
    exint_struct.line_enable = TRUE;
    exint_struct.line_mode   = EXINT_LINE_INTERRUPT;

    switch (mode) {
        case GPIO_INT_RISING:  exint_struct.line_polarity = EXINT_TRIGGER_RISING_EDGE;  break;
        case GPIO_INT_FALLING: exint_struct.line_polarity = EXINT_TRIGGER_FALLING_EDGE; break;
        case GPIO_INT_BOTH:    exint_struct.line_polarity = EXINT_TRIGGER_BOTH_EDGE;    break;
        case GPIO_INT_LOW:
        case GPIO_INT_HIGH:
            return GPIO_ERR_UNSUPP;   /* AT32 EXINT 只支持边沿触发 */
        default:
            return GPIO_ERR_PARAM;
    }
    exint_init(&exint_struct);

    /* NVIC: pin0~4 独立向量, pin5~9 共享, pin10~15 共享 */
    IRQn_Type irqn;
    if (pin_pos <= 4u) {
        irqn = (IRQn_Type)(EXINT0_IRQn + pin_pos);
    } else if (pin_pos <= 9u) {
        irqn = EXINT9_5_IRQn;
    } else if (pin_pos <= 15u) {
        irqn = EXINT15_10_IRQn;
    } else {
        return GPIO_ERR_PARAM;
    }
    nvic_irq_enable(irqn, 1, 0);
    return GPIO_OK;
}
