/**
 * @file    iic_port.h
 * @brief   可复用 IIC 驱动模板 - 硬件移植层接口
 * @note    本文件仅声明 port 层接口, 具体实现由用户在 iic_port_<mcu>.c
 *          (例如 iic_port_stm32_hal.c / iic_port_esp32_idf.c) 中覆盖弱定义桩完成.
 *
 *          驱动支持两种底层实现 (port 层二选一即可):
 *          A. 软件位时序 (bit-bang, 推荐 MCU 无硬件 I2C 时使用):
 *              port 层实现以下 4 个函数, 通用层用 GPIO 翻转模拟 I2C 时序:
 *                - iic_port_init_gpio     : 配置 SCL/SDA 为开漏输出 + 上拉
 *                - iic_port_set_sda        : 设置 SDA 电平
 *                - iic_port_set_scl        : 设置 SCL 电平
 *                - iic_port_read_sda       : 读取 SDA 电平
 *                - iic_port_set_sda_dir    : 切换 SDA 方向 (输出数据/输入读应答)
 *                - iic_port_delay_us      : 微秒延时 (周期决定速度)
 *              优点: 任意引脚, 跨 MCU 一致. 缺点: 速度慢 (<400kbps), 占 CPU.
 *
 *          B. 硬件 I2C (推荐有硬件 I2C 控制器时使用):
 *              port 层实现 iic_port_master_xfer() 原子传输函数, 通用层直接调用.
 *                - iic_port_init_hw_i2c   : 配置硬件 I2C (时钟/地址模式/速度)
 *                - iic_port_master_xfer   : 一次原子传输 (START/地址/数据/RESTART/STOP)
 *                - iic_port_deinit_hw_i2c : 关闭硬件 I2C
 *              优点: 高速 (>=1Mbps), 不占 CPU, 支持 DMA. 缺点: 引脚固定.
 *
 *          通用层 iic.c 会先调用硬件 xfer, 若返回 IIC_ERR_UNSUPP 再回退到软件位时序.
 *
 *          移植到新 MCU 的步骤:
 *          1. 在 iic_cfg.h 中定义 IIC_BUS_x 宏与 g_iic_bus_map[] 表,
 *             用 HW_IIC_SOFT 或 HW_IIC_HARD 编码每条总线的硬件句柄
 *          2. (软件位时序) 实现 6 个 gpio 相关 port 函数 + 1 个 delay_us
 *          3. (硬件 I2C)   实现 init_hw_i2c / master_xfer / deinit_hw_i2c
 *          4. 在主程序调用 iic_init() 启动驱动
 *          5. 若 MCU 不支持某功能, 在对应 port 函数内返回 IIC_ERR_UNSUPP
 */
#ifndef IIC_PORT_H_
#define IIC_PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "iic.h"
#include "iic_cfg.h"

/* ============================ port 层接口 ============================ */

/**
 * @brief  硬件初始化 (根据 hw_handle 自动分发到软件/硬件实现)
 * @param  hw    硬件总线描述 (来自映射表, 不可为 NULL)
 * @param  cfg   用户配置 (角色/地址位宽/速度/超时)
 * @note   实现要点:
 *         - 软件 IIC: 配置 SCL/SDA 为开漏输出, 内部上拉, 初始电平为高
 *         - 硬件 I2C: 使能时钟, 调用 HAL_I2C_Init() / i2c_param_config(),
 *                      设置时钟频率对应到 IIC_SPEED_*
 */
iic_status_t iic_port_init(const iic_hw_bus_t *hw, const iic_config_t *cfg);

/**
 * @brief  反初始化 (关时钟, 释放引脚, 关中断)
 */
iic_status_t iic_port_deinit(const iic_hw_bus_t *hw);

/* ==================== 软件位时序层 (软件 IIC 时必须实现) ==================== */

/**
 * @brief  设置 SDA 方向 (输出推数据 / 输入读 ACK)
 * @param  is_output  1=输出, 0=输入(开漏+上拉, 等价于"释放SDA")
 * @note   软件 IIC 必需: 主机发送数据时输出, 等待从机 ACK 时切换为输入
 */
iic_status_t iic_port_set_sda_dir(const iic_hw_bus_t *hw, uint8_t is_output);

/**
 * @brief  设置 SDA 电平 (SDA 必须为输出方向)
 * @param  state  1=高(释放, 经上拉电阻拉高), 0=低(下拉)
 * @note   软件 IIC: SDA 是开漏, 写"高"实际是释放引脚, 由上拉拉高
 */
iic_status_t iic_port_set_sda(const iic_hw_bus_t *hw, uint8_t state);

/**
 * @brief  设置 SCL 电平
 */
iic_status_t iic_port_set_scl(const iic_hw_bus_t *hw, uint8_t state);

/**
 * @brief  读取 SDA 当前电平
 * @retval 1=高, 0=低
 * @note   读取前 SDA 必须切换为输入方向 (iic_port_set_sda_dir(hw, 0))
 */
uint8_t iic_port_read_sda(const iic_hw_bus_t *hw);

/**
 * @brief  读取 SCL 当前电平 (用于检测总线是否被从机钳低)
 */
uint8_t iic_port_read_scl(const iic_hw_bus_t *hw);

/**
 * @brief  微秒级延时
 * @param  us  延时微秒数 (决定 I2C 速度: 5us 半周期 -> 100kbps)
 * @note   实现:
 *         - STM32 HAL: 用 DWT 周期计数器或 TIM 定时器
 *         - ESP-IDF:    用 esp_rom_delay_us() 或 ets_delay_us()
 *         - 无定时器:   __NOP() 循环 (需根据主频校准)
 */
iic_status_t iic_port_delay_us(uint32_t us);

/* ==================== 硬件 I2C 原子传输 (硬件 I2C 时必须实现) ==================== */

/**
 * @brief  硬件 I2C 原子传输 (单次 START + 数据 + STOP, 或带 RESTART)
 *
 * @param  hw          硬件总线描述
 * @param  addr        7 位从机地址 (未左移, 范围 0x01~0x7F)
 * @param  is_read     0=写, 1=读
 * @param  reg_bytes   寄存器地址字节流 (可 NULL 表示不发送寄存器地址, reg_len=0)
 * @param  reg_len      寄存器地址字节数 (0=不发, 1/2/3=8/16/24 位地址)
 * @param  buf          数据缓冲 (写时为源, 读时为目)
 * @param  len          数据长度 (字节)
 * @param  timeout_ms  超时
 *
 * @note   传输序列:
 *         - reg_len > 0 时: START + (addr<<1 | W) + reg_bytes[] + [RESTART] + (addr<<1 | R/W) + buf[] + STOP
 *         - reg_len = 0 时: START + (addr<<1 | R/W) + buf[] + STOP
 *
 *         实现:
 *         - STM32 HAL: 调用 HAL_I2C_Master_Seq_Transmit_IT/Mem_Read
 *                       (F7/H7 推荐; F1/F4 用 HAL_I2C_Master_Transmit/Mem_Read 阻塞)
 *         - ESP-IDF:   i2c_master_write_read_device()
 *
 *         若 MCU 无硬件 I2C, 本函数直接返回 IIC_ERR_UNSUPP, 通用层自动回退到软件位时序
 */
iic_status_t iic_port_master_xfer(const iic_hw_bus_t *hw,
                                  uint16_t addr, uint8_t is_read,
                                  const uint8_t *reg_bytes, uint8_t reg_len,
                                  uint8_t *buf, uint16_t len,
                                  uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* IIC_PORT_H_ */
