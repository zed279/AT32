/**
 * @file    spi.h
 * @brief   可复用 SPI 驱动模板 - 公共 API
 * @note    本驱动采用"硬件抽象层 + 通用逻辑层"分层设计:
 *          - spi.c      : 通用逻辑、参数校验、软件位时序、阻塞全双工/半双工传输 (与 MCU 无关)
 *          - spi_port   : 硬件相关操作 (引脚电平控制 / 硬件 SPI 控制器寄存器读写)
 *
 *          支持两种底层实现:
 *          1) 软件 SPI (bit-bang): port 层实现 set_sck / set_mosi / read_miso / set_cs / delay_us
 *                                    通用层 spi.c 用 GPIO 翻转模拟 SPI 时序, 适合任意 MCU
 *          2) 硬件 SPI:            port 层实现 spi_port_master_xfer() 原子传输函数
 *                                    通用层优先调用硬件传输, 失败再回退软件位时序
 *
 *          使用流程:
 *          1. 在 spi_cfg.h 中定义总线别名与硬件句柄映射表
 *          2. 实现 spi_port.c 中的硬件相关函数 (或新建 spi_port_<mcu>.c 覆盖弱定义)
 *          3. 调用 spi_bus_init() 初始化总线
 *          4. 调用 spi_transfer() 全双工收发 / spi_write() / spi_read()
 *          5. 寄存器型从机用 spi_write_reg() / spi_read_reg()
 *          6. 调试用 spi_scan() 检测 CS 线上是否有从机应答
 *
 *          与 W25Q64 Flash/SD卡/MAX31855/ILI9341/ADS1256 等从机配合时, 业务层
 *          只需调用本头文件中声明的 API, 不需要关心 MCU 类型。
 */
#ifndef SPI_H_
#define SPI_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spi_cfg.h"

/* ============================ 返回码 ============================ */
typedef enum {
    SPI_OK            =  0,   /* 成功 */
    SPI_ERR_PARAM     = -1,   /* 参数非法 (NULL / 长度=0 / 总线号越界) */
    SPI_ERR_BUS       = -2,   /* 逻辑总线未定义 / 越界 */
    SPI_ERR_BUSY      = -3,   /* 总线忙 (硬件占用中) */
    SPI_ERR_TIMEOUT   = -4,   /* 超时 (传输未完成) */
    SPI_ERR_NACK      = -5,   /* 从机无响应 (CS 拉低后 MISO 无变化) */
    SPI_ERR_HW        = -6,   /* 硬件操作失败 (HAL 错误 / IDF 错误) */
    SPI_ERR_UNSUPP    = -7,   /* 当前硬件不支持该功能 */
    SPI_ERR_CS         = -8,   /* CS 控制失败 */
} spi_status_t;

/* ============================ 逻辑总线号 ============================ */
/**
 * @brief 逻辑总线编号
 * @note  用户在 spi_cfg.h 中通过 SPI_BUS_x 宏定义,
 *        并在 g_spi_bus_map[] 中映射到实际硬件句柄 (软件 GPIO 引脚或硬件 SPI 实例)
 */
typedef uint16_t spi_bus_t;

/* ============================ SPI 模式 (CPOL/CPHA) ============================ */
/**
 * @brief SPI 四种时钟模式
 * @note  CPOL: 时钟极性 (0=空闲低, 1=空闲高)
 *        CPHA: 时钟相位 (0=第一个边沿采样, 1=第二个边沿采样)
 *
 *        Mode 0 (CPOL=0, CPHA=0): 空闲低, 上升沿采样, 下降沿输出 (最常用, 90% 设备)
 *        Mode 1 (CPOL=0, CPHA=1): 空闲低, 下降沿采样, 上升沿输出
 *        Mode 2 (CPOL=1, CPHA=0): 空闲高, 下降沿采样, 上升沿输出
 *        Mode 3 (CPOL=1, CPHA=1): 空闲高, 上升沿采样, 下降沿输出
 */
typedef enum {
    SPI_MODE_0 = 0,   /* CPOL=0, CPHA=0 (最常用) */
    SPI_MODE_1 = 1,   /* CPOL=0, CPHA=1 */
    SPI_MODE_2 = 2,   /* CPOL=1, CPHA=0 */
    SPI_MODE_3 = 3,   /* CPOL=1, CPHA=1 */
} spi_mode_t;

/* ============================ 位序 ============================ */
typedef enum {
    SPI_MSB_FIRST = 0,   /* MSB 先行 (标准 SPI, 99% 设备) */
    SPI_LSB_FIRST = 1,   /* LSB 先行 (罕见, 部分 LCD) */
} spi_bit_order_t;

/* ============================ 速度等级 ============================ */
typedef enum {
    SPI_SPEED_LOW      = 0,   /* ~1 MHz   (慢速 Flash/SD 初始化) */
    SPI_SPEED_STANDARD = 1,   /* ~4 MHz   (多数传感器) */
    SPI_SPEED_FAST     = 2,   /* ~8 MHz   (高速 Flash) */
    SPI_SPEED_VERY_FAST = 3,  /* ~20 MHz  (DMA 传输, ILI9341 LCD) */
    SPI_SPEED_ULTRA    = 4,   /* ~40 MHz+ (仅硬件 SPI, STM32H7/ESP32) */
} spi_speed_t;

/* ============================ CS 极性 ============================ */
typedef enum {
    SPI_CS_ACTIVE_LOW  = 0,   /* 低有效 (标准, 99% 设备) */
    SPI_CS_ACTIVE_HIGH = 1,   /* 高有效 (罕见) */
} spi_cs_polarity_t;

/* ============================ 数据宽度 ============================ */
typedef enum {
    SPI_DATA_8BIT  = 8,    /* 8 位数据帧 (标准) */
    SPI_DATA_16BIT = 16,   /* 16 位数据帧 (部分 ADC/DAC) */
} spi_data_size_t;

/* ============================ 初始化配置 ============================ */
typedef struct {
    spi_bus_t          bus;           /* 逻辑总线号 */
    spi_mode_t         mode;          /* CPOL/CPHA 模式 */
    spi_bit_order_t    bit_order;     /* MSB/LSB 先行 */
    spi_speed_t        speed;         /* 速度等级 */
    spi_cs_polarity_t  cs_polarity;   /* CS 极性 */
    spi_data_size_t    data_size;     /* 数据帧宽度 */
    uint32_t           timeout_ms;    /* 阻塞操作默认超时 (ms) */
} spi_config_t;

/* ============================ 公共 API ============================ */

/**
 * @brief  初始化指定总线
 * @param  cfg  初始化配置 (不能为 NULL)
 * @retval SPI_OK / SPI_ERR_PARAM / SPI_ERR_BUS / SPI_ERR_HW
 */
spi_status_t spi_bus_init(const spi_config_t *cfg);

/**
 * @brief  反初始化总线 (释放 GPIO / 关闭硬件 SPI)
 */
spi_status_t spi_deinit(spi_bus_t bus);

/* ==================== CS 控制 ==================== */

/**
 * @brief  拉低 (或拉高) CS, 选中从机开始传输
 * @note   CS 极性由 cfg.cs_polarity 决定, 默认低有效
 */
spi_status_t spi_cs_select(spi_bus_t bus);

/**
 * @brief  释放 CS, 结束传输
 */
spi_status_t spi_cs_deselect(spi_bus_t bus);

/* ==================== 软件 SPI 位时序 (兼容旧写法) ==================== */

/**
 * @brief  软件 SPI 发送并接收一个字节 (全双工)
 * @param  bus  逻辑总线号
 * @param  byte 要发送的字节
 * @retval 读取到的字节 (0~255)
 * @note   仅软件位时序模式需要直接调用; 阻塞 API 内部已封装
 */
uint8_t spi_transfer_byte(spi_bus_t bus, uint8_t byte);

/* ==================== 阻塞主机传输 ==================== */

/**
 * @brief  全双工传输 (同时发送和接收)
 * @param  bus      逻辑总线号
 * @param  tx_buf   待发送数据缓冲 (NULL 表示发 0xFF 哑字节)
 * @param  rx_buf   接收缓冲 (NULL 表示不接收)
 * @param  len      数据长度 (字节)
 * @param  timeout_ms 超时 (0 表示使用 cfg.timeout_ms)
 * @note   本函数不控制 CS, 需在调用前 spi_cs_select(), 调用后 spi_cs_deselect()
 *         若需要 CS 自动控制, 用 spi_xfer() 代替
 */
spi_status_t spi_transfer(spi_bus_t bus,
                          const uint8_t *tx_buf, uint8_t *rx_buf,
                          uint16_t len, uint32_t timeout_ms);

/**
 * @brief  半双工写 (仅发送)
 * @param  bus      逻辑总线号
 * @param  tx_buf   待发送数据缓冲
 * @param  len      数据长度
 * @param  timeout_ms 超时
 * @note   本函数不控制 CS
 */
spi_status_t spi_write(spi_bus_t bus,
                       const uint8_t *tx_buf, uint16_t len,
                       uint32_t timeout_ms);

/**
 * @brief  半双工读 (仅接收, 发送哑字节 0xFF)
 * @param  bus      逻辑总线号
 * @param  rx_buf   接收缓冲
 * @param  len      要读取的字节数
 * @param  timeout_ms 超时
 * @note   本函数不控制 CS
 */
spi_status_t spi_read(spi_bus_t bus,
                      uint8_t *rx_buf, uint16_t len,
                      uint32_t timeout_ms);

/**
 * @brief  完整的 CS 自动控制传输 (选中从机 -> 传输 -> 释放 CS)
 * @param  bus      逻辑总线号
 * @param  tx_buf   发送缓冲 (可 NULL, 发哑字节)
 * @param  rx_buf   接收缓冲 (可 NULL, 不接收)
 * @param  len      数据长度
 * @param  timeout_ms 超时
 * @note   本函数自动控制 CS, 适合简单单次传输
 */
spi_status_t spi_xfer(spi_bus_t bus,
                      const uint8_t *tx_buf, uint8_t *rx_buf,
                      uint16_t len, uint32_t timeout_ms);

/**
 * @brief  向从机寄存器写数据
 * @param  bus       逻辑总线号
 * @param  cmd       命令字节 / 寄存器地址 (1 字节)
 * @param  data      数据缓冲
 * @param  len       数据长度
 * @param  timeout_ms 超时
 * @note   传输序列: CS_LOW + cmd + data[0..n-1] + CS_HIGH
 *         适合 W25Q64 Flash / SD 卡 / 各种 SPI 传感器
 */
spi_status_t spi_write_reg(spi_bus_t bus, uint8_t cmd,
                           const uint8_t *data, uint16_t len,
                           uint32_t timeout_ms);

/**
 * @brief  从从机寄存器读数据
 * @param  bus       逻辑总线号
 * @param  cmd       命令字节 / 寄存器地址 (1 字节)
 * @param  buf       接收缓冲
 * @param  len       要读取的字节数
 * @param  timeout_ms 超时
 * @note   传输序列: CS_LOW + cmd + [dummy/addr] + read[0..n-1] + CS_HIGH
 *         对于 W25Q64: cmd=读命令, 后续可直接读数据
 *         对于 MAX31855: cmd 无效, CS 拉低即开始输出数据
 */
spi_status_t spi_read_reg(spi_bus_t bus, uint8_t cmd,
                          uint8_t *buf, uint16_t len,
                          uint32_t timeout_ms);

/**
 * @brief  向从机发送多字节命令 (无数据阶段)
 * @param  bus       逻辑总线号
 * @param  cmd_buf   命令字节缓冲
 * @param  cmd_len   命令字节数
 * @param  timeout_ms 超时
 * @note   传输序列: CS_LOW + cmd_buf[0..n-1] + CS_HIGH
 *         适合 Flash 的写使能/擦除命令 / SD 卡的 CMD0/CMD8
 */
spi_status_t spi_send_cmd(spi_bus_t bus,
                          const uint8_t *cmd_buf, uint8_t cmd_len,
                          uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* SPI_H_ */
