/**
 * @file    spi_port.c
 * @brief   可复用 SPI 驱动模板 - 硬件移植层默认实现 (弱定义桩)
 * @note    本文件提供弱定义(__attribute__((weak))) 的桩函数, 默认返回 SPI_ERR_UNSUPP,
 *          保证链接通过, 工程可编译运行 (虽然 SPI 不工作).
 *
 *          用户移植时新建同名非 weak 函数即可覆盖, 不必删除本文件.
 *          常见覆盖示例:
 *            - spi_port_stm32_hal.c  (STM32 HAL 库, 软件 GPIO + 硬件 SPI)
 *            - spi_port_esp32_idf.c  (ESP-IDF, 软件 GPIO + 硬件 SPI)
 *            - spi_port_gd32.c       (GD32 标准库)
 *            - spi_port_nrf52.c      (nRF52 寄存器级)
 *
 *          典型软件 SPI 覆盖示例 (基于 GPIO 抽象层, 仅示意):
 *
 *          spi_status_t spi_port_set_sck(const spi_hw_bus_t *hw, uint8_t state)
 *          {
 *              uint32_t sck_handle = (hw->hw_handle >> 24) & 0xFF;
 *              gpio_write(sck_pin, state ? GPIO_HIGH : GPIO_LOW);
 *              return SPI_OK;
 *          }
 *
 *          硬件 SPI 覆盖示例 (STM32 HAL):
 *
 *          spi_status_t spi_port_master_xfer(const spi_hw_bus_t *hw, ...)
 *          {
 *              SPI_HandleTypeDef *hspi = lookup_spi(hw->hw_handle);
 *              HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(hspi, tx, rx, len, timeout);
 *              return (st == HAL_OK) ? SPI_OK : SPI_ERR_HW;
 *          }
 */
#include "spi_port.h"

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
spi_status_t spi_port_init(const spi_hw_bus_t *hw, const spi_config_t *cfg)
{
    (void)hw; (void)cfg;
    return SPI_ERR_UNSUPP;
}

__attribute__((weak))
spi_status_t spi_port_deinit(const spi_hw_bus_t *hw)
{
    (void)hw;
    return SPI_ERR_UNSUPP;
}

/* ---------- 软件位时序层 (默认不支持, 由 spi_port_<mcu>.c 覆盖) ---------- */

__attribute__((weak))
spi_status_t spi_port_set_sck(const spi_hw_bus_t *hw, uint8_t state)
{
    (void)hw; (void)state;
    return SPI_ERR_UNSUPP;
}

__attribute__((weak))
spi_status_t spi_port_set_mosi(const spi_hw_bus_t *hw, uint8_t state)
{
    (void)hw; (void)state;
    return SPI_ERR_UNSUPP;
}

__attribute__((weak))
uint8_t spi_port_read_miso(const spi_hw_bus_t *hw)
{
    (void)hw;
    return 1;   /* 默认高电平 */
}

__attribute__((weak))
spi_status_t spi_port_set_cs(const spi_hw_bus_t *hw, uint8_t active,
                              spi_cs_polarity_t cs_polarity)
{
    (void)hw; (void)active; (void)cs_polarity;
    return SPI_ERR_UNSUPP;
}

__attribute__((weak))
spi_status_t spi_port_delay_us(uint32_t us)
{
    (void)us;
    return SPI_ERR_UNSUPP;
}

/* ---------- 硬件 SPI 原子传输 (默认不支持) ---------- */

__attribute__((weak))
spi_status_t spi_port_master_xfer(const spi_hw_bus_t *hw,
                                  const uint8_t *tx_buf, uint8_t *rx_buf,
                                  uint16_t len, uint32_t timeout_ms,
                                  uint8_t cs_control)
{
    (void)hw; (void)tx_buf; (void)rx_buf;
    (void)len; (void)timeout_ms; (void)cs_control;
    return SPI_ERR_UNSUPP;
}
