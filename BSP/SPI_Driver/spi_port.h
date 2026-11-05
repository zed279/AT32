/**
 * @file    spi_port.h
 * @brief   可复用 SPI 驱动模板 - 硬件移植层接口
 * @note    本文件仅声明 port 层接口, 具体实现由用户在 spi_port_<mcu>.c
 *          (例如 spi_port_stm32_hal.c / spi_port_esp32_idf.c) 中覆盖弱定义桩完成.
 *
 *          驱动支持两种底层实现 (port 层二选一即可):
 *          A. 软件位时序 (bit-bang, 推荐 MCU 无硬件 SPI 或引脚不匹配时使用):
 *              port 层实现以下 5 个函数, 通用层用 GPIO 翻转模拟 SPI 时序:
 *                - spi_port_init_gpio    : 配置 SCK/MOSI/MISO/CS 为推挽/输入
 *                - spi_port_set_sck       : 设置 SCK 电平
 *                - spi_port_set_mosi      : 设置 MOSI 电平
 *                - spi_port_read_miso     : 读取 MISO 电平
 *                - spi_port_set_cs        : 设置 CS 电平
 *                - spi_port_delay_us      : 微秒延时
 *              优点: 任意引脚, 跨 MCU 一致. 缺点: 速度慢 (<1Mbps), 占 CPU.
 *
 *          B. 硬件 SPI (推荐有硬件 SPI 控制器时使用):
 *              port 层实现 spi_port_master_xfer() 原子传输函数, 通用层直接调用.
 *                - spi_port_init_hw_spi  : 配置硬件 SPI (时钟/模式/位序/速度)
 *                - spi_port_master_xfer  : 一次原子传输 (CS 控制 + 数据收发)
 *                - spi_port_deinit_hw_spi: 关闭硬件 SPI
 *              优点: 高速 (>=8Mbps), 不占 CPU, 支持 DMA. 缺点: 引脚固定.
 *
 *          通用层 spi.c 会先调用硬件 xfer, 若返回 SPI_ERR_UNSUPP 再回退到软件位时序.
 *
 *          移植到新 MCU 的步骤:
 *          1. 在 spi_cfg.h 中定义 SPI_BUS_x 宏与 g_spi_bus_map[] 表,
 *             用 HW_SPI_SOFT 或 HW_SPI_HARD 编码每条总线的硬件句柄
 *          2. (软件位时序) 实现 5 个 gpio 相关 port 函数 + 1 个 delay_us
 *          3. (硬件 SPI)   实现 init_hw_spi / master_xfer / deinit_hw_spi
 *          4. 在主程序调用 spi_init() 启动驱动
 *          5. 若 MCU 不支持某功能, 在对应 port 函数内返回 SPI_ERR_UNSUPP
 */
#ifndef SPI_PORT_H_
#define SPI_PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "spi.h"
#include "spi_cfg.h"

/* ============================ port 层接口 ============================ */

/**
 * @brief  硬件初始化 (根据 hw_handle 自动分发到软件/硬件实现)
 * @param  hw    硬件总线描述 (来自映射表, 不可为 NULL)
 * @param  cfg   用户配置 (模式/位序/速度/超时)
 * @note   实现要点:
 *         - 软件 SPI: 配置 SCK/MOSI/CS 为推挽输出, MISO 为输入, CS 初始为释放态
 *         - 硬件 SPI: 使能时钟, 调用 HAL_SPI_Init() / spi_bus_initialize(),
 *                      设置时钟频率对应到 SPI_SPEED_*
 */
spi_status_t spi_port_init(const spi_hw_bus_t *hw, const spi_config_t *cfg);

/**
 * @brief  反初始化 (关时钟, 释放引脚, 关中断)
 */
spi_status_t spi_port_deinit(const spi_hw_bus_t *hw);

/* ==================== 软件位时序层 (软件 SPI 时必须实现) ==================== */

/**
 * @brief  设置 SCK 电平
 * @param  state  1=高, 0=低
 */
spi_status_t spi_port_set_sck(const spi_hw_bus_t *hw, uint8_t state);

/**
 * @brief  设置 MOSI 电平 (主机输出从机输入)
 * @param  state  1=高, 0=低
 */
spi_status_t spi_port_set_mosi(const spi_hw_bus_t *hw, uint8_t state);

/**
 * @brief  读取 MISO 电平 (主机输入从机输出)
 * @retval 1=高, 0=低
 */
uint8_t spi_port_read_miso(const spi_hw_bus_t *hw);

/**
 * @brief  设置 CS 电平 (片选)
 * @param  active  1=选中从机 (按 cfg.cs_polarity 自动决定电平), 0=释放
 * @note   软件 SPI 必需: CS 极性由 cfg 决定, 这里统一用 active 语义
 */
spi_status_t spi_port_set_cs(const spi_hw_bus_t *hw, uint8_t active,
                              spi_cs_polarity_t cs_polarity);

/**
 * @brief  微秒级延时
 * @param  us  延时微秒数 (决定 SPI 速度: 1us 半周期 -> 500kHz SCK)
 * @note   实现:
 *         - STM32 HAL: 用 DWT 周期计数器或 TIM 定时器
 *         - ESP-IDF:    用 esp_rom_delay_us() 或 ets_delay_us()
 *         - 无定时器:   __NOP() 循环 (需根据主频校准)
 */
spi_status_t spi_port_delay_us(uint32_t us);

/* ==================== 硬件 SPI 原子传输 (硬件 SPI 时必须实现) ==================== */

/**
 * @brief  硬件 SPI 原子传输 (CS 控制 + 数据收发)
 *
 * @param  hw          硬件总线描述
 * @param  tx_buf      发送缓冲 (NULL 表示发 0xFF 哑字节)
 * @param  rx_buf      接收缓冲 (NULL 表示不接收)
 * @param  len         数据长度 (字节)
 * @param  timeout_ms  超时
 * @param  cs_control  是否在本函数内控制 CS (0=不控制, 1=自动 CS_LOW->传输->CS_HIGH)
 *
 * @note   传输行为:
 *         - cs_control=1: CS 拉低 -> 传输 -> CS 拉高 (完整的一次事务)
 *         - cs_control=0: 仅传输数据, CS 由业务层通过 spi_cs_select/deselect 控制
 *
 *         实现:
 *         - STM32 HAL: 调用 HAL_SPI_TransmitReceive() / HAL_SPI_Transmit() / HAL_SPI_Receive()
 *                       (支持 DMA: HAL_SPI_TransmitReceive_DMA)
 *         - ESP-IDF:   spi_device_transmit() (spi_master driver)
 *
 *         若 MCU 无硬件 SPI, 本函数直接返回 SPI_ERR_UNSUPP, 通用层自动回退到软件位时序
 */
spi_status_t spi_port_master_xfer(const spi_hw_bus_t *hw,
                                  const uint8_t *tx_buf, uint8_t *rx_buf,
                                  uint16_t len, uint32_t timeout_ms,
                                  uint8_t cs_control);

#ifdef __cplusplus
}
#endif
#endif /* SPI_PORT_H_ */
