/**
 * @file    gpio_port.c
 * @brief   可复用 GPIO 驱动模板 - 硬件移植层默认实现 (弱定义桩)
 * @note    本文件提供弱定义(__attribute__((weak)) 或 #pragma weak)的桩函数,
 *          - 默认返回 GPIO_ERR_UNSUPP, 保证链接通过, 工程可编译运行
 *          - 用户移植时新建同名函数 (去掉 weak) 即可覆盖
 *
 *          典型覆盖示例 (以 STM32 HAL 为例, 仅示意, 不强制使用 HAL):
 *
 *          gpio_status_t gpio_port_init(const gpio_hw_pin_t *hw, const gpio_config_t *cfg)
 *          {
 *              GPIO_TypeDef *port = hw_decode_port(hw->hw_handle);
 *              uint16_t      msk  = hw_decode_pin_mask(hw->hw_handle);
 *              GPIO_InitTypeDef gi = {0};
 *              __HAL_RCC_GPIOx_CLK_ENABLE(port);
 *              gi.Pin   = msk;
 *              gi.Mode  = (cfg->dir == GPIO_DIR_OUTPUT) ? GPIO_MODE_OUTPUT_PP :
 *                       (cfg->dir == GPIO_DIR_OD)     ? GPIO_MODE_OUTPUT_OD :
 *                       (cfg->dir == GPIO_DIR_ANALOG)? GPIO_MODE_ANALOG :
 *                                                        GPIO_MODE_INPUT;
 *              gi.Pull  = (cfg->pull == GPIO_PULL_UP)  ? GPIO_PULLUP :
 *                       (cfg->pull == GPIO_PULL_DOWN)? GPIO_PULLDOWN :
 *                                                      GPIO_NOPULL;
 *              gi.Speed = (cfg->speed == 0) ? GPIO_SPEED_FREQ_LOW :
 *                                             GPIO_SPEED_FREQ_HIGH;
 *              HAL_GPIO_Init(port, &gi);
 *              if (cfg->dir == GPIO_DIR_OUTPUT || cfg->dir == GPIO_DIR_OD) {
 *                  HAL_GPIO_WritePin(port, msk, cfg->init_state);
 *              }
 *              return GPIO_OK;
 *          }
 *
 *          ISR 入口示例 (STM32 EXTI):
 *          void EXTI0_IRQHandler(void) { __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_0);
 *                                        gpio_irq_handler(HW_HANDLE(GPIOA, 0)); }
 */
#include "gpio_port.h"

/* 若编译器不支持 __attribute__((weak)) (如 MSVC), 用宏兼容 */
#ifndef WEAK
#  if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
#    define WEAK __attribute__((weak))
#  elif defined(__arm__)
#    define WEAK __weak
#  else
#    define WEAK
#  endif
#endif

/* ============================ 默认桩实现 ============================ */

__attribute__((weak))
gpio_status_t gpio_port_init(const gpio_hw_pin_t *hw, const gpio_config_t *cfg)
{
    (void)hw; (void)cfg;
    return GPIO_ERR_UNSUPP;
}

__attribute__((weak))
gpio_status_t gpio_port_deinit(const gpio_hw_pin_t *hw)
{
    (void)hw;
    return GPIO_ERR_UNSUPP;
}

__attribute__((weak))
gpio_status_t gpio_port_set_dir(const gpio_hw_pin_t *hw, gpio_dir_t dir)
{
    (void)hw; (void)dir;
    return GPIO_ERR_UNSUPP;
}

__attribute__((weak))
gpio_status_t gpio_port_set_level(const gpio_hw_pin_t *hw, gpio_state_t state)
{
    (void)hw; (void)state;
    return GPIO_ERR_UNSUPP;
}

__attribute__((weak))
gpio_state_t gpio_port_read(const gpio_hw_pin_t *hw)
{
    (void)hw;
    return GPIO_LOW;
}

__attribute__((weak))
gpio_status_t gpio_port_toggle(const gpio_hw_pin_t *hw)
{
    (void)hw;
    return GPIO_ERR_UNSUPP;
}

__attribute__((weak))
gpio_status_t gpio_port_int_config(const gpio_hw_pin_t *hw, gpio_int_mode_t mode)
{
    (void)hw; (void)mode;
    return GPIO_ERR_UNSUPP;
}
