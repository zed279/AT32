/**
 * @file    iic.c
 * @brief   可复用 IIC 驱动模板 - 通用逻辑层实现 (与 MCU 无关)
 * @note    本文件实现与硬件无关的部分:
 *          - 总线合法性校验 (查 g_iic_bus_map[] 表)
 *          - 初始化/反初始化 (调用 port 层)
 *          - 软件 I2C 位时序 (START/STOP/字节发送/字节读取, 由 port 层提供
 *             set_sda/set_scl/read_sda/delay_us 4 个原语拼装)
 *          - 阻塞主机读写 (优先硬件 xfer, 失败回退软件时序)
 *          - 寄存器读写封装 (MPU6050/BMP280/EEPROM 等通用)
 *          - 总线扫描 (0x01~0x7F)
 *
 *          软件位时序参考 I2C 协议:
 *          START: SCL 高时 SDA 由高变低
 *          STOP : SCL 高时 SDA 由低变高
 *          数据位: SCL 低时改 SDA, SCL 高时采样 (MSB 先行)
 *          ACK:   第 9 个 SCL 周期, 主机释放 SDA 读从机应答
 */
#include "iic.h"
#include "iic_port.h"
#include <stddef.h>

/* ============================ 内部: 总线查表 ============================ */
/**
 * @brief  通过逻辑总线号查表得到硬件总线描述
 * @retval !=NULL 成功
 * @retval NULL   总线未定义
 */
static const iic_hw_bus_t *iic_lookup(iic_bus_t bus)
{
    for (uint16_t i = 0; i < IIC_BUS_TABLE_SIZE; ++i) {
        if (g_iic_bus_map[i].bus_id == bus) {
            return &g_iic_bus_map[i];
        }
    }
    return NULL;
}

/* ============================ 内部: 软件位时序 ============================ */
/* 半周期延时 (SCL 高/低各占一个半周期) */
static inline void iic_soft_half_period(const iic_hw_bus_t *hw)
{
    iic_port_delay_us(IIC_SOFT_HALF_PERIOD_US);
    (void)hw;
}

/**
 * @brief  等待 SCL 升高, 同时检测从机是否钳低 (clock stretching)
 * @note   标准 I2C 允许从机在 ACK 前拉低 SCL 暂停主机, 但总时间超
 *         IIC_SOFT_SCL_TIMEOUT_US 视为总线故障
 */
static iic_status_t iic_soft_wait_scl_high(const iic_hw_bus_t *hw)
{
    /* port 层 set_scl 后通常立即升高 (开漏+上拉), 但从机可能拉低 */
    uint32_t waited = 0;
    while (iic_port_read_scl(hw) == 0) {
        iic_port_delay_us(1);
        if (++waited > IIC_SOFT_SCL_TIMEOUT_US) {
            return IIC_ERR_TIMEOUT;
        }
    }
    return IIC_OK;
}

/**
 * @brief  生成 START 条件
 * @note   时序: SDA=1, SCL=1 -> SDA=0(START) -> SCL=0
 *         若 SCL 或 SDA 初始不是高, 说明总线被占用, 先发 STOP 复位一次
 */
static iic_status_t iic_soft_start(const iic_hw_bus_t *hw)
{
    iic_port_set_sda_dir(hw, 1);
    iic_port_set_sda(hw, 1);
    iic_port_set_scl(hw, 1);
    iic_soft_half_period(hw);

    /* 检测总线空闲 (SDA 与 SCL 均高) */
    if (iic_port_read_sda(hw) == 0 || iic_port_read_scl(hw) == 0) {
        /* 总线未空闲, 发 STOP 复位 */
        iic_port_set_sda(hw, 0);
        iic_soft_half_period(hw);
        iic_port_set_scl(hw, 0);
        iic_soft_half_period(hw);
        /* 再尝试 START */
        iic_port_set_sda(hw, 1);
        iic_port_set_scl(hw, 1);
        iic_soft_half_period(hw);
    }

    iic_port_set_sda(hw, 0);   /* SCL 高时 SDA 由高变低 = START */
    iic_soft_half_period(hw);
    iic_port_set_scl(hw, 0);
    iic_soft_half_period(hw);
    return IIC_OK;
}

/**
 * @brief  生成 STOP 条件
 * @note   时序: SDA=0, SCL=1 -> SDA=1(STOP) -> SCL=0 (保持总线空闲)
 */
static iic_status_t iic_soft_stop(const iic_hw_bus_t *hw)
{
    iic_port_set_sda_dir(hw, 1);
    iic_port_set_sda(hw, 0);
    iic_port_set_scl(hw, 1);
    iic_soft_half_period(hw);
    /* 等待 SCL 真正升高 (从机 clock stretching) */
    iic_status_t st = iic_soft_wait_scl_high(hw);
    if (st != IIC_OK) {
        return st;
    }
    iic_port_set_sda(hw, 1);   /* SCL 高时 SDA 由低变高 = STOP */
    iic_soft_half_period(hw);
    return IIC_OK;
}

/**
 * @brief  发送一个字节 (MSB 先行), 返回从机 ACK 状态
 * @retval IIC_OK = ACK, IIC_ERR_NACK = NACK, IIC_ERR_TIMEOUT = SCL 钳低超时
 */
static iic_status_t iic_soft_send_byte(const iic_hw_bus_t *hw, uint8_t byte)
{
    iic_port_set_sda_dir(hw, 1);

    /* 发送 8 位数据 (MSB 先) */
    for (int8_t i = 7; i >= 0; --i) {
        iic_port_set_sda(hw, (byte >> i) & 0x01);
        iic_soft_half_period(hw);                       /* SCL 低期间数据稳定 */
        iic_port_set_scl(hw, 1);                        /* SCL 升高, 从机采样 */
        iic_status_t st = iic_soft_wait_scl_high(hw);
        if (st != IIC_OK) return st;
        iic_soft_half_period(hw);
        iic_port_set_scl(hw, 0);
        iic_soft_half_period(hw);
    }

    /* 第 9 周期: 读从机 ACK */
    iic_port_set_sda_dir(hw, 0);   /* 释放 SDA 切换为输入 */
    iic_port_set_scl(hw, 1);
    iic_status_t st = iic_soft_wait_scl_high(hw);
    if (st != IIC_OK) return st;
    iic_soft_half_period(hw);
    uint8_t ack = iic_port_read_sda(hw);   /* 0=ACK, 1=NACK */
    iic_port_set_scl(hw, 0);
    iic_soft_half_period(hw);
    iic_port_set_sda_dir(hw, 1);   /* 恢复输出, 准备下一字节 */
    iic_port_set_sda(hw, 1);        /* 释放 SDA */

    return (ack == 0) ? IIC_OK : IIC_ERR_NACK;
}

/**
 * @brief  读取一个字节 (MSB 先行)
 * @param  send_ack  1=读后发 ACK, 0=读后发 NACK (用于末字节)
 */
static uint8_t iic_soft_read_byte(const iic_hw_bus_t *hw, iic_ack_t send_ack)
{
    uint8_t byte = 0;

    iic_port_set_sda_dir(hw, 0);   /* 主机释放 SDA, 由从机驱动 */

    for (int8_t i = 7; i >= 0; --i) {
        iic_port_set_scl(hw, 1);
        (void)iic_soft_wait_scl_high(hw);
        iic_soft_half_period(hw);
        if (iic_port_read_sda(hw)) {
            byte |= (uint8_t)(1u << i);
        }
        iic_port_set_scl(hw, 0);
        iic_soft_half_period(hw);
    }

    /* 主机发送 ACK/NACK */
    iic_port_set_sda_dir(hw, 1);
    iic_port_set_sda(hw, (send_ack == IIC_ACK) ? 0 : 1);
    iic_soft_half_period(hw);
    iic_port_set_scl(hw, 1);
    iic_soft_half_period(hw);
    iic_port_set_scl(hw, 0);
    iic_soft_half_period(hw);

    iic_port_set_sda_dir(hw, 0);   /* 释放 SDA */
    iic_port_set_sda(hw, 1);
    return byte;
}

/* ============================ 内部: 软件时序阻塞主机传输 ============================ */
/**
 * @brief  软件位时序实现的主机传输
 * @note   传输序列 (reg_len > 0 时):
 *           START + (addr<<1 | 0) + reg_bytes[0..reg_len-1]
 *           [RESTART] + (addr<<1 | is_read) + data[0..len-1] + STOP
 *         (reg_len = 0 时跳过寄存器地址段)
 */
static iic_status_t iic_soft_master_xfer(const iic_hw_bus_t *hw,
                                         uint16_t addr, uint8_t is_read,
                                         const uint8_t *reg_bytes, uint8_t reg_len,
                                         uint8_t *buf, uint16_t len,
                                         uint32_t timeout_ms)
{
    (void)timeout_ms;   /* 软件时序不实现精确超时, 依赖 SCL 钳低检测 */

    /* ----- 第一段: 写寄存器地址 (若有) ----- */
    iic_status_t st = iic_soft_start(hw);
    if (st != IIC_OK) return st;

    /* 写地址+W */
    st = iic_soft_send_byte(hw, (uint8_t)(addr << 1));
    if (st != IIC_OK) { iic_soft_stop(hw); return IIC_ERR_NACK; }

    /* 写寄存器地址 */
    for (uint8_t i = 0; i < reg_len; ++i) {
        st = iic_soft_send_byte(hw, reg_bytes[i]);
        if (st != IIC_OK) { iic_soft_stop(hw); return IIC_ERR_NACK; }
    }

    if (!is_read) {
        /* 写模式: 继续写数据 */
        for (uint16_t i = 0; i < len; ++i) {
            st = iic_soft_send_byte(hw, buf[i]);
            if (st != IIC_OK) { iic_soft_stop(hw); return IIC_ERR_NACK; }
        }
        iic_soft_stop(hw);
        return IIC_OK;
    }

    /* 读模式: RESTART + addr+R + 数据读取 */
    st = iic_soft_start(hw);   /* RESTART = 在 SCL 低时再来一个 START */
    if (st != IIC_OK) return st;

    st = iic_soft_send_byte(hw, (uint8_t)((addr << 1) | 0x01));
    if (st != IIC_OK) { iic_soft_stop(hw); return IIC_ERR_NACK; }

    for (uint16_t i = 0; i < len; ++i) {
        /* 末字节 NACK, 其余 ACK (符合 I2C 协议: 末字节 NACK 通知从机停止发送) */
        buf[i] = iic_soft_read_byte(hw, (i == len - 1) ? IIC_NACK : IIC_ACK);
    }
    iic_soft_stop(hw);
    return IIC_OK;
}

/* ============================ 公共 API 实现 ============================ */

iic_status_t iic_init(const iic_config_t *cfg)
{
    if (cfg == NULL) {
        return IIC_ERR_PARAM;
    }

    const iic_hw_bus_t *hw = iic_lookup(cfg->bus);
    if (hw == NULL) {
        return IIC_ERR_BUS;
    }

    return iic_port_init(hw, cfg);
}

iic_status_t iic_deinit(iic_bus_t bus)
{
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) {
        return IIC_ERR_BUS;
    }
    return iic_port_deinit(hw);
}

/* ---------- 位时序 API (兼容旧 OLED/EEPROM 写法) ---------- */

iic_status_t iic_start(iic_bus_t bus)
{
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;
    return iic_soft_start(hw);
}

iic_status_t iic_stop(iic_bus_t bus)
{
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;
    return iic_soft_stop(hw);
}

iic_status_t iic_send_byte(iic_bus_t bus, uint8_t byte)
{
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;
    return iic_soft_send_byte(hw, byte);
}

uint8_t iic_read_byte(iic_bus_t bus, iic_ack_t ack)
{
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return 0xFF;
    return iic_soft_read_byte(hw, ack);
}

/* ---------- 阻塞主机读写 (优先硬件 xfer, 失败回退软件) ---------- */

iic_status_t iic_master_write(iic_bus_t bus, uint16_t addr,
                               const uint8_t *data, uint16_t len,
                               uint32_t timeout_ms)
{
    if ((data == NULL && len != 0) || addr > 0x7F) {
        return IIC_ERR_PARAM;
    }
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;

    /* 优先硬件传输 */
    iic_status_t st = iic_port_master_xfer(hw, addr, 0,
                                          NULL, 0,
                                          (uint8_t *)data, len, timeout_ms);
    if (st != IIC_ERR_UNSUPP) {
        return st;
    }

    /* 回退软件位时序 */
    return iic_soft_master_xfer(hw, addr, 0, NULL, 0,
                                 (uint8_t *)data, len, timeout_ms);
}

iic_status_t iic_master_read(iic_bus_t bus, uint16_t addr,
                             uint8_t *buf, uint16_t len,
                             uint32_t timeout_ms)
{
    if (buf == NULL || addr > 0x7F) {
        return IIC_ERR_PARAM;
    }
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;

    iic_status_t st = iic_port_master_xfer(hw, addr, 1,
                                          NULL, 0,
                                          buf, len, timeout_ms);
    if (st != IIC_ERR_UNSUPP) {
        return st;
    }
    return iic_soft_master_xfer(hw, addr, 1, NULL, 0,
                                 buf, len, timeout_ms);
}

iic_status_t iic_master_write_reg(iic_bus_t bus, uint16_t addr,
                                  uint32_t reg, iic_reg_size_t reg_size,
                                  const uint8_t *data, uint16_t len,
                                  uint32_t timeout_ms)
{
    if ((data == NULL && len != 0) || addr > 0x7F ||
        reg_size > IIC_REG_24BIT || reg_size == IIC_REG_NONE) {
        return IIC_ERR_PARAM;
    }

    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;

    /* 将 32 位寄存器地址序列化为字节流 (大端, 高字节在前) */
    uint8_t reg_bytes[3];
    uint8_t reg_len = (uint8_t)reg_size;
    for (uint8_t i = 0; i < reg_len; ++i) {
        reg_bytes[i] = (uint8_t)(reg >> (8 * (reg_len - 1 - i)));
    }

    /* 硬件优先 */
    iic_status_t st = iic_port_master_xfer(hw, addr, 0,
                                          reg_bytes, reg_len,
                                          (uint8_t *)data, len, timeout_ms);
    if (st != IIC_ERR_UNSUPP) {
        return st;
    }
    /* 软件 */
    return iic_soft_master_xfer(hw, addr, 0, reg_bytes, reg_len,
                                 (uint8_t *)data, len, timeout_ms);
}

iic_status_t iic_master_read_reg(iic_bus_t bus, uint16_t addr,
                                 uint32_t reg, iic_reg_size_t reg_size,
                                 uint8_t *buf, uint16_t len,
                                 uint32_t timeout_ms)
{
    if (buf == NULL || addr > 0x7F ||
        reg_size > IIC_REG_24BIT || reg_size == IIC_REG_NONE) {
        return IIC_ERR_PARAM;
    }

    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;

    uint8_t reg_bytes[3];
    uint8_t reg_len = (uint8_t)reg_size;
    for (uint8_t i = 0; i < reg_len; ++i) {
        reg_bytes[i] = (uint8_t)(reg >> (8 * (reg_len - 1 - i)));
    }

    iic_status_t st = iic_port_master_xfer(hw, addr, 1,
                                          reg_bytes, reg_len,
                                          buf, len, timeout_ms);
    if (st != IIC_ERR_UNSUPP) {
        return st;
    }
    return iic_soft_master_xfer(hw, addr, 1, reg_bytes, reg_len,
                                 buf, len, timeout_ms);
}

iic_status_t iic_scan(iic_bus_t bus, void (*cb)(uint8_t addr))
{
    const iic_hw_bus_t *hw = iic_lookup(bus);
    if (hw == NULL) return IIC_ERR_BUS;

    /* 对 0x01~0x7F 每个地址发 START + addr+W, 收到 ACK 说明设备存在 */
    for (uint8_t addr = 1; addr <= 0x7F; ++addr) {
        iic_status_t st = iic_port_master_xfer(hw, addr, 0,
                                               NULL, 0, NULL, 0, 10);
        if (st == IIC_ERR_UNSUPP) {
            /* 软件回退 */
            st = iic_soft_master_xfer(hw, addr, 0, NULL, 0, NULL, 0, 10);
        }
        if (st == IIC_OK && cb != NULL) {
            cb(addr);
        }
        /* NACK/TIMEOUT 是正常的, 继续扫描下一个地址 */
    }
    return IIC_OK;
}
