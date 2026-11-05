/**
 * @file    iic.h
 * @brief   可复用 IIC(I2C) 驱动模板 - 公共 API
 * @note    本驱动采用"硬件抽象层 + 通用逻辑层"分层设计:
 *          - iic.c      : 通用逻辑、参数校验、软件位时序、阻塞读写、寄存器读写 (与 MCU 无关)
 *          - iic_port   : 硬件相关操作 (引脚电平控制 / 硬件 I2C 控制器寄存器读写)
 *
 *          支持两种底层实现:
 *          1) 软件 IIC (bit-bang): port 层实现 set_sda / set_scl / read_sda / delay_us 4 个函数
 *                                    通用层 iic.c 用 GPIO 翻转模拟 I2C 时序, 适合任意 MCU
 *          2) 硬件 IIC:            port 层实现 iic_port_master_xfer() 原子传输函数
 *                                    通用层优先调用硬件传输, 失败再回退软件位时序
 *
 *          使用流程:
 *          1. 在 iic_cfg.h 中定义总线别名与硬件句柄映射表
 *          2. 实现 iic_port.c 中的硬件相关函数 (或新建 iic_port_<mcu>.c 覆盖弱定义)
 *          3. 调用 iic_init() 初始化总线
 *          4. 调用 iic_master_write()/iic_master_read() 收发数据
 *          5. 寄存器型从机用 iic_master_write_reg()/iic_master_read_reg()
 *          6. 调试用 iic_scan() 扫描总线上 0x01~0x7F 范围内的所有从机
 *
 *          与 OLED/SSD1315/EEPROM/MPU6050/BMP280 等从机配合时, 业务层
 *          只需调用本头文件中声明的 API, 不需要关心 MCU 类型。
 */
#ifndef IIC_H_
#define IIC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "iic_cfg.h"

/* ============================ 返回码 ============================ */
typedef enum {
    IIC_OK            =  0,   /* 成功 */
    IIC_ERR_PARAM     = -1,   /* 参数非法 (NULL / 长度=0 / 总线号越界) */
    IIC_ERR_BUS       = -2,   /* 逻辑总线未定义 / 越界 */
    IIC_ERR_BUSY      = -3,   /* 总线忙 (硬件占用中) */
    IIC_ERR_TIMEOUT   = -4,   /* 超时 (ACK 等待 / SCL 钳低 / 字节传输) */
    IIC_ERR_NACK      = -5,   /* 从机未应答 (地址 NACK 或数据 NACK) */
    IIC_ERR_HW        = -6,   /* 硬件操作失败 (HAL 错误 / IDF 错误) */
    IIC_ERR_UNSUPP    = -7,   /* 当前硬件不支持该功能 */
    IIC_ERR_ARB       = -8,   /* 仲裁失败 (多主模式, 本方丢失总线) */
} iic_status_t;

/* ============================ 逻辑总线号 ============================ */
/**
 * @brief 逻辑总线编号
 * @note  用户在 iic_cfg.h 中通过 IIC_BUS_x 宏定义,
 *        并在 g_iic_bus_map[] 中映射到实际硬件句柄 (软件 GPIO 引脚或硬件 I2C 实例)
 */
typedef uint16_t iic_bus_t;

/* ============================ 主从角色 ============================ */
typedef enum {
    IIC_ROLE_MASTER = 0,   /* 主机 */
    IIC_ROLE_SLAVE  = 1,   /* 从机 (本驱动主要面向主机, 从机可选) */
} iic_role_t;

/* ============================ 地址位宽 ============================ */
typedef enum {
    IIC_ADDR_7BIT  = 7,   /* 7 位地址 (标准模式, 99% 设备) */
    IIC_ADDR_10BIT = 10,  /* 10 位地址 (罕见, 部分大容量 EEPROM) */
} iic_addr_bits_t;

/* ============================ 寄存器地址尺寸 ============================ */
typedef enum {
    IIC_REG_8BIT  = 1,    /* 8 位寄存器地址 (1 字节, 90% 设备如 BMP280/MPU6050) */
    IIC_REG_16BIT = 2,    /* 16 位寄存器地址 (2 字节, 大容量 EEPROM 如 24C32/24C256) */
    IIC_REG_24BIT = 3,    /* 24 位寄存器地址 (3 字节, 极少数) */
    IIC_REG_NONE  = 0,    /* 无寄存器地址 (直接读写, 如 SSD1315 命令流) */
} iic_reg_size_t;

/* ============================ 速度等级 ============================ */
typedef enum {
    IIC_SPEED_STANDARD  = 0,   /* 标准模式 ~100 kbps */
    IIC_SPEED_FAST      = 1,   /* 快速模式 ~400 kbps */
    IIC_SPEED_FAST_PLUS = 2,   /* Fast+ 1 Mbps (硬件 I2C 才支持) */
    IIC_SPEED_HIGH      = 3,   /* 高速模式 3.4 Mbps (罕见, 需硬件) */
} iic_speed_t;

/* ============================ ACK 状态 (软件位时序层) ============================ */
typedef enum {
    IIC_ACK  = 0,   /* 应答 (SDA 低) */
    IIC_NACK = 1,   /* 非应答 (SDA 高) */
} iic_ack_t;

/* ============================ 初始化配置 ============================ */
typedef struct {
    iic_bus_t       bus;          /* 逻辑总线号 */
    iic_role_t      role;         /* 主/从 (本驱动以主机为主) */
    iic_addr_bits_t addr_bits;    /* 地址位宽 */
    iic_speed_t     speed;        /* 速度等级 */
    uint32_t        timeout_ms;   /* 阻塞操作默认超时 (ms) */
} iic_config_t;

/* ============================ 公共 API ============================ */

/**
 * @brief  初始化指定总线
 * @param  cfg  初始化配置 (不能为 NULL)
 * @retval IIC_OK / IIC_ERR_PARAM / IIC_ERR_BUS / IIC_ERR_HW
 */
iic_status_t iic_init(const iic_config_t *cfg);

/**
 * @brief  反初始化总线 (释放 GPIO / 关闭硬件 I2C)
 */
iic_status_t iic_deinit(iic_bus_t bus);

/**
 * @brief  生成 START 条件 (SCL 高时 SDA 由高变低)
 * @note   仅软件位时序模式需要直接调用; 阻塞 API 内部已封装
 */
iic_status_t iic_start(iic_bus_t bus);

/**
 * @brief  生成 STOP 条件 (SCL 高时 SDA 由低变高)
 */
iic_status_t iic_stop(iic_bus_t bus);

/**
 * @brief  发送一个字节
 * @param  byte  要发送的字节
 * @retval IIC_OK=收到ACK, IIC_ERR_NACK=未应答, IIC_ERR_TIMEOUT=超时
 */
iic_status_t iic_send_byte(iic_bus_t bus, uint8_t byte);

/**
 * @brief  读取一个字节
 * @param  ack  读取后是否给从机应答 (1B 末尾 NACK, 中间 ACK)
 * @retval 读取到的字节 (0~255)
 */
uint8_t iic_read_byte(iic_bus_t bus, iic_ack_t ack);

/* ==================== 阻塞主机读写 ==================== */

/**
 * @brief  主机发送数据 (写从机)
 * @param  bus      逻辑总线号
 * @param  addr     7 位从机地址 (未左移, 范围 0x01~0x7F)
 * @param  data     待发送数据缓冲 (可 NULL 时 len=0 仅寻址)
 * @param  len      数据长度 (字节)
 * @param  timeout_ms 超时 (软件位时序模式下无字节级超时, 仅由 SCL 钳低检测兜底)
 * @retval IIC_OK / IIC_ERR_NACK / IIC_ERR_TIMEOUT / ...
 */
iic_status_t iic_master_write(iic_bus_t bus, uint16_t addr,
                              const uint8_t *data, uint16_t len,
                              uint32_t timeout_ms);

/**
 * @brief  主机接收数据 (读从机)
 * @param  bus      逻辑总线号
 * @param  addr     7 位从机地址
 * @param  buf      接收缓冲
 * @param  len      要读取的字节数
 * @param  timeout_ms 超时
 * @retval IIC_OK / IIC_ERR_NACK / IIC_ERR_TIMEOUT
 */
iic_status_t iic_master_read(iic_bus_t bus, uint16_t addr,
                             uint8_t *buf, uint16_t len,
                             uint32_t timeout_ms);

/**
 * @brief  向从机寄存器写数据 (8/16/24 位寄存器地址)
 * @param  bus       逻辑总线号
 * @param  addr      7 位从机地址
 * @param  reg       寄存器地址 (按 reg_size 解释为 8/16/24 位)
 * @param  reg_size  寄存器地址尺寸 (IIC_REG_8BIT/16BIT/24BIT)
 * @param  data      数据缓冲
 * @param  len       数据长度
 * @param  timeout_ms 超时
 * @note   传输序列: START + addr+W + regH + regL + data[0..n] + STOP
 */
iic_status_t iic_master_write_reg(iic_bus_t bus, uint16_t addr,
                                  uint32_t reg, iic_reg_size_t reg_size,
                                  const uint8_t *data, uint16_t len,
                                  uint32_t timeout_ms);

/**
 * @brief  从从机寄存器读数据
 * @note   传输序列: START + addr+W + regH + regL
 *                    RESTART + addr+R + data[0..n-1] + NACK + STOP
 */
iic_status_t iic_master_read_reg(iic_bus_t bus, uint16_t addr,
                                 uint32_t reg, iic_reg_size_t reg_size,
                                 uint8_t *buf, uint16_t len,
                                 uint32_t timeout_ms);

/**
 * @brief  扫描总线上所有应答的从机地址
 * @param  bus   逻辑总线号
 * @param  cb    回调, 每命中一个从机调用一次 cb(addr)
 * @note   扫描范围 0x01~0x7F (0x00 是通用调用地址, 0x7F 保留)
 */
iic_status_t iic_scan(iic_bus_t bus, void (*cb)(uint8_t addr));

#ifdef __cplusplus
}
#endif
#endif /* IIC_H_ */
