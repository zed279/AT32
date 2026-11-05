/**
 * @file    gpio_port.h
 * @brief   可复用 GPIO 驱动模板 - 硬件移植层接口
 * @note    本文件仅声明 port 层接口, 具体实现由用户在 gpio_port_stm32_hal.c
 *          (或其他 gpio_port_<mcu>.c) 中覆盖弱定义桩函数完成.
 *
 *          移植到新 MCU 的步骤:
 *          1. 在 gpio_cfg.h 中定义 GPIO_PIN_x 宏与 g_gpio_pin_map[] 表,
 *             将逻辑引脚号映射到本 MCU 的硬件句柄 (端口+引脚 或 GPIOx_BASE+pin)
 *          2. 实现以下全部 port 函数:
 *             - gpio_port_init        : 时钟使能 + 方向 + 上下拉 + 初始电平
 *             - gpio_port_deinit      : 恢复到复位状态 (关时钟、关中断、置模拟)
 *             - gpio_port_set_dir     : 仅修改方向
 *             - gpio_port_set_level       : 写输出寄存器 (BSRR/ODR/POROUT 等)
 *             - gpio_port_read        : 读输入寄存器 (IDR/PIN 等)
 *             - gpio_port_toggle      : 翻转 (硬件翻转指令优先, 否则读改写)
 *             - gpio_port_int_config  : 配置 EXTI/IRQ、NVIC 使能/禁止
 *          3. 在对应外部中断 ISR 中调用 gpio_irq_handler(hw_handle)
 *          4. 若 MCU 不支持某功能 (如开漏、模拟、电平触发), 在对应函数内返回 GPIO_ERR_UNSUPP
 */
#ifndef GPIO_PORT_H_
#define GPIO_PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "gpio.h"
#include "gpio_cfg.h"

/* ============================ 硬件引脚句柄 ============================ */
/**
 * @brief 硬件引脚描述结构体
 * @note  此类型定义在 gpio_cfg.h 中 (因为映射表 g_gpio_pin_map[] 需要它),
 *        此处不重复 typedef 以避免 "typedef redefinition" 编译错误.
 *        使用方通过 gpio_cfg.h -> g_gpio_pin_map[] 取得实例.
 */

/* ============================ port 层接口 ============================ */

/**
 * @brief  硬件初始化 (时钟 + 方向 + 上下拉 + 初始电平)
 * @param  hw   硬件引脚描述 (来自映射表, 不可为 NULL)
 * @param  cfg  用户配置
 */
gpio_status_t gpio_port_init(const gpio_hw_pin_t *hw, const gpio_config_t *cfg);

/**
 * @brief  反初始化 (关时钟、关中断、置复位状态)
 */
gpio_status_t gpio_port_deinit(const gpio_hw_pin_t *hw);

/**
 * @brief  仅修改方向 (不改动上下拉和当前电平)
 */
gpio_status_t gpio_port_set_dir(const gpio_hw_pin_t *hw, gpio_dir_t dir);

/**
 * @brief  写输出电平
 */
gpio_status_t gpio_port_set_level(const gpio_hw_pin_t *hw, gpio_state_t state);

/**
 * @brief  读当前电平 (输入或输出模式均可)
 */
gpio_state_t  gpio_port_read(const gpio_hw_pin_t *hw);

/**
 * @brief  翻转电平 (有硬件翻转指令优先用, 否则读改写)
 */
gpio_status_t gpio_port_toggle(const gpio_hw_pin_t *hw);

/**
 * @brief  配置外部中断
 * @param  mode  GPIO_INT_NONE 表示关闭并禁用 NVIC 线
 * @note   实现要点:
 *         - 配置触发沿 (上升/下降/双沿/电平)
 *         - 清除挂起标志
 *         - 使能/禁用 NVIC IRQ
 */
gpio_status_t gpio_port_int_config(const gpio_hw_pin_t *hw, gpio_int_mode_t mode);

#ifdef __cplusplus
}
#endif
#endif /* GPIO_PORT_H_ */
