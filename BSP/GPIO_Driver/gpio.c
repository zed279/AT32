/**
 * @file    gpio.c
 * @brief   可复用 GPIO 驱动模板 - 通用逻辑层实现 (与 MCU 无关)
 * @note    本文件实现与硬件无关的:
 *          - 引脚合法性校验
 *          - 初始化/方向/读写/翻转
 *          - 中断回调注册表管理
 *          - 中断事件分发 (由 port 层 ISR 调用 gpio_irq_handler)
 *          真正的寄存器操作在 gpio_port.c 中实现。
 */
#include "gpio.h"
#include <stddef.h>
#include "gpio_port.h"

/* ============================ 内部: 引脚合法性校验 ============================ */
/**
 * @brief  通过逻辑引脚号查表得到硬件端口/引脚句柄
 * @retval !=NULL  成功, *out 为硬件句柄
 * @retval NULL    引脚未定义
 */
static const gpio_hw_pin_t *gpio_lookup(gpio_pin_t pin, uint16_t *out_idx)
{
    for (uint16_t i = 0; i < GPIO_PIN_TABLE_SIZE; ++i) {
        if (g_gpio_pin_map[i].user_pin == pin) {
            if (out_idx) *out_idx = i;
            return &g_gpio_pin_map[i];
        }
    }
    return NULL;
}

/* ============================ 内部: 中断回调表 ============================ */
typedef struct {
    gpio_pin_t      pin;
    gpio_callback_t cb;
} gpio_int_slot_t;

static gpio_int_slot_t s_int_slots[GPIO_MAX_INTERRUPT_SLOTS];

static gpio_int_slot_t *gpio_int_slot_find(gpio_pin_t pin)
{
    for (uint16_t i = 0; i < GPIO_MAX_INTERRUPT_SLOTS; ++i) {
        if (s_int_slots[i].cb != NULL && s_int_slots[i].pin == pin) {
            return &s_int_slots[i];
        }
    }
    return NULL;
}

static gpio_int_slot_t *gpio_int_slot_alloc(void)
{
    for (uint16_t i = 0; i < GPIO_MAX_INTERRUPT_SLOTS; ++i) {
        if (s_int_slots[i].cb == NULL) {
            return &s_int_slots[i];
        }
    }
    return NULL;
}

/* ============================ 公共 API 实现 ============================ */

gpio_status_t gpio_pin_init(const gpio_config_t *cfg)
{
    if (cfg == NULL) {
        return GPIO_ERR_PARAM;
    }

    uint16_t idx = 0;
    const gpio_hw_pin_t *hw = gpio_lookup(cfg->pin, &idx);
    if (hw == NULL) {
        return GPIO_ERR_PIN;
    }

    /* 调用 port 层完成硬件初始化 (时钟使能 + 方向 + 上下拉 + 初始电平) */
    gpio_status_t st = gpio_port_init(hw, cfg);
    if (st != GPIO_OK) {
        return st;
    }

    return GPIO_OK;
}

gpio_status_t gpio_deinit(gpio_pin_t pin)
{
    const gpio_hw_pin_t *hw = gpio_lookup(pin, NULL);
    if (hw == NULL) {
        return GPIO_ERR_PIN;
    }

    /* 先注销可能存在的中断回调 */
    (void)gpio_detach_interrupt(pin);

    return gpio_port_deinit(hw);
}

gpio_status_t gpio_set_dir(gpio_pin_t pin, gpio_dir_t dir)
{
    const gpio_hw_pin_t *hw = gpio_lookup(pin, NULL);
    if (hw == NULL) {
        return GPIO_ERR_PIN;
    }
    return gpio_port_set_dir(hw, dir);
}

gpio_status_t gpio_write(gpio_pin_t pin, gpio_state_t state)
{
    const gpio_hw_pin_t *hw = gpio_lookup(pin, NULL);
    if (hw == NULL) {
        return GPIO_ERR_PIN;
    }
    return gpio_port_set_level(hw, state);
}

gpio_state_t gpio_read(gpio_pin_t pin)
{
    const gpio_hw_pin_t *hw = gpio_lookup(pin, NULL);
    if (hw == NULL) {
        return GPIO_LOW;   /* 引脚无效时默认返回低, 由调用方判断 errno */
    }
    return gpio_port_read(hw);
}

gpio_status_t gpio_toggle(gpio_pin_t pin)
{
    const gpio_hw_pin_t *hw = gpio_lookup(pin, NULL);
    if (hw == NULL) {
        return GPIO_ERR_PIN;
    }
    return gpio_port_toggle(hw);
}

gpio_status_t gpio_attach_interrupt(gpio_pin_t pin,
                                    gpio_int_mode_t mode,
                                    gpio_callback_t cb)
{
    const gpio_hw_pin_t *hw = gpio_lookup(pin, NULL);
    if (hw == NULL) {
        return GPIO_ERR_PIN;
    }

    /* 注销分支 */
    if (mode == GPIO_INT_NONE || cb == NULL) {
        gpio_int_slot_t *slot = gpio_int_slot_find(pin);
        if (slot != NULL) {
            slot->cb  = NULL;
            slot->pin = 0;
        }
        return gpio_port_int_config(hw, GPIO_INT_NONE);
    }

    /* 已存在则覆盖, 否则分配新槽 */
    gpio_int_slot_t *slot = gpio_int_slot_find(pin);
    if (slot == NULL) {
        slot = gpio_int_slot_alloc();
        if (slot == NULL) {
            return GPIO_ERR_BUSY;
        }
    }
    slot->pin = pin;
    slot->cb  = cb;

    /* 配置硬件中断 */
    gpio_status_t st = gpio_port_int_config(hw, mode);
    if (st != GPIO_OK) {
        /* 失败时回滚 */
        slot->cb  = NULL;
        slot->pin = 0;
        return st;
    }
    return GPIO_OK;
}

gpio_status_t gpio_detach_interrupt(gpio_pin_t pin)
{
    return gpio_attach_interrupt(pin, GPIO_INT_NONE, NULL);
}

void gpio_irq_handler(uint32_t hw_pin)
{
    /* port 层 ISR 中调用本函数, hw_pin 为硬件引脚句柄 (例如端口/引脚编码) */
    for (uint16_t i = 0; i < GPIO_MAX_INTERRUPT_SLOTS; ++i) {
        if (s_int_slots[i].cb != NULL) {
            const gpio_hw_pin_t *hw = gpio_lookup(s_int_slots[i].pin, NULL);
            if (hw != NULL && hw->hw_handle == hw_pin) {
                s_int_slots[i].cb(s_int_slots[i].pin);
                break;
            }
        }
    }
}

/* ============================ 模块初始化 ============================ */
/**
 * @brief  可选: 驱动模块级初始化 (清空中断表)
 * @note   若编译器支持, 可在主程序启动时调用一次
 */
void gpio_driver_init(void)
{
    for (uint16_t i = 0; i < GPIO_MAX_INTERRUPT_SLOTS; ++i) {
        s_int_slots[i].cb  = NULL;
        s_int_slots[i].pin = 0;
    }
}
