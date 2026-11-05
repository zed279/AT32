/**
 * @file    iic_port.c
 * @brief   可复用 IIC 驱动模板 - 硬件移植层默认实现 (弱定义桩)
 * @note    本文件提供弱定义(__attribute__((weak))) 的桩函数, 默认返回 IIC_ERR_UNSUPP,
 *          保证链接通过, 工程可编译运行 (虽然 I2C 不工作).
 *
 *          用户移植时新建同名非 weak 函数即可覆盖, 不必删除本文件.
 *          常见覆盖示例:
 *            - iic_port_stm32_hal.c  (STM32 HAL 库, 软件 GPIO + 硬件 I2C)
 *            - iic_port_esp32_idf.c  (ESP-IDF, 软件 GPIO + 硬件 I2C)
 *            - iic_port_gd32.c       (GD32 标准库)
 *            - iic_port_nrf52.c       (nRF52 寄存器级)
 *
 *          典型软件 IIC 覆盖示例 (基于 GPIO 抽象层, 仅示意):
 *
 *          iic_status_t iic_port_set_sda(const iic_hw_bus_t *hw, uint8_t state)
 *          {
 *              uint32_t sda_handle = hw->hw_handle & 0xFFFF;
 *              gpio_set_dir(scl_pin, GPIO_DIR_OUTPUT);   // 推挽也行, 开漏更规范
 *              gpio_write(sda_pin, state ? GPIO_HIGH : GPIO_LOW);
 *              return IIC_OK;
 *          }
 *
 *          硬件 I2C 覆盖示例 (STM32 HAL):
 *
 *          iic_status_t iic_port_master_xfer(const iic_hw_bus_t *hw, ...)
 *          {
 *              I2C_HandleTypeDef *hi2c = lookup_i2c(hw->hw_handle);
 *              HAL_StatusTypeDef st = is_read ?
 *                  HAL_I2C_Mem_Read(hi2c, addr<<1, reg, reg_size, buf, len, timeout) :
 *                  HAL_I2C_Mem_Write(hi2c, addr<<1, reg, reg_size, buf, len, timeout);
 *              return (st == HAL_OK) ? IIC_OK : IIC_ERR_HW;
 *          }
 */
#include "iic_port.h"

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
iic_status_t iic_port_init(const iic_hw_bus_t *hw, const iic_config_t *cfg)
{
    (void)hw; (void)cfg;
    return IIC_ERR_UNSUPP;
}

__attribute__((weak))
iic_status_t iic_port_deinit(const iic_hw_bus_t *hw)
{
    (void)hw;
    return IIC_ERR_UNSUPP;
}

/* ---------- 软件位时序层 (默认不支持, 由 iic_port_<mcu>.c 覆盖) ---------- */

__attribute__((weak))
iic_status_t iic_port_set_sda_dir(const iic_hw_bus_t *hw, uint8_t is_output)
{
    (void)hw; (void)is_output;
    return IIC_ERR_UNSUPP;
}

__attribute__((weak))
iic_status_t iic_port_set_sda(const iic_hw_bus_t *hw, uint8_t state)
{
    (void)hw; (void)state;
    return IIC_ERR_UNSUPP;
}

__attribute__((weak))
iic_status_t iic_port_set_scl(const iic_hw_bus_t *hw, uint8_t state)
{
    (void)hw; (void)state;
    return IIC_ERR_UNSUPP;
}

__attribute__((weak))
uint8_t iic_port_read_sda(const iic_hw_bus_t *hw)
{
    (void)hw;
    return 1;   /* 默认高电平, 防止误判 ACK */
}

__attribute__((weak))
uint8_t iic_port_read_scl(const iic_hw_bus_t *hw)
{
    (void)hw;
    return 1;
}

__attribute__((weak))
iic_status_t iic_port_delay_us(uint32_t us)
{
    (void)us;
    return IIC_ERR_UNSUPP;
}

/* ---------- 硬件 I2C 原子传输 (默认不支持) ---------- */

__attribute__((weak))
iic_status_t iic_port_master_xfer(const iic_hw_bus_t *hw,
                                  uint16_t addr, uint8_t is_read,
                                  const uint8_t *reg_bytes, uint8_t reg_len,
                                  uint8_t *buf, uint16_t len,
                                  uint32_t timeout_ms)
{
    (void)hw; (void)addr; (void)is_read;
    (void)reg_bytes; (void)reg_len;
    (void)buf; (void)len; (void)timeout_ms;
    return IIC_ERR_UNSUPP;
}
