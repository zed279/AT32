/**
 * @file    spi.c
 * @brief   可复用 SPI 驱动模板 - 通用逻辑层实现 (与 MCU 无关)
 * @note    本文件实现与硬件无关的部分:
 *          - 总线合法性校验 (查 g_spi_bus_map[] 表)
 *          - 初始化/反初始化 (调用 port 层)
 *          - 软件 SPI 位时序 (SCK 边沿 + MOSI/MISO 移位, 由 port 层提供
 *             set_sck/set_mosi/read_miso/set_cs/delay_us 5 个原语拼装)
 *          - 阻塞全双工/半双工传输 (优先硬件 xfer, 失败回退软件时序)
 *          - 寄存器读写封装 (W25Q64/SD卡/MAX31855 等通用)
 *
 *          软件 SPI 位时序参考 SPI 协议:
 *          Mode 0 (CPOL=0, CPHA=0): 空闲低, 上升沿采样, 下降沿输出
 *          Mode 1 (CPOL=0, CPHA=1): 空闲低, 下降沿采样, 上升沿输出
 *          Mode 2 (CPOL=1, CPHA=0): 空闲高, 下降沿采样, 上升沿输出
 *          Mode 3 (CPOL=1, CPHA=1): 空闲高, 上升沿采样, 下降沿输出
 *
 *          数据位: MSB 先行 (标准) 或 LSB 先行 (由 cfg.bit_order 决定)
 */
#include "spi.h"
#include "spi_port.h"
#include <stddef.h>

/* ============================ 内部: 总线查表 ============================ */
static const spi_hw_bus_t *spi_lookup(spi_bus_t bus)
{
    for (uint16_t i = 0; i < SPI_BUS_TABLE_SIZE; ++i) {
        if (g_spi_bus_map[i].bus_id == bus) {
            return &g_spi_bus_map[i];
        }
    }
    return NULL;
}

/* ============================ 内部: 配置缓存 ============================ */
/* 缓存每条总线的配置 (模式/位序/CS 极性), 软件 SPI 位时序需要 */
static spi_config_t s_cfg_cache[SPI_MAX_BUSES];

/* ============================ 内部: 软件 SPI 位时序 ============================ */

/**
 * @brief  根据 SPI 模式和 CPHA 决定采样边沿
 *
 * @note   CPHA=0: 第一个边沿 (前沿) 采样, 第二个边沿 (后沿) 输出
 *         CPHA=1: 第一个边沿 (前沿) 输出, 第二个边沿 (后沿) 采样
 *
 *  前沿 = 从空闲态跳变到活动态:
 *    CPOL=0: 前沿 = 上升沿 (0->1), 后沿 = 下降沿 (1->0)
 *    CPOL=1: 前沿 = 下降沿 (1->0), 后沿 = 上升沿 (0->1)
 */
static inline uint8_t spi_soft_cpha(const spi_hw_bus_t *hw)
{
    (void)hw;
    /* 从缓存获取 CPHA */
    uint8_t cpha = 0;
    for (uint16_t i = 0; i < SPI_BUS_TABLE_SIZE; ++i) {
        if (g_spi_bus_map[i].bus_id == hw->bus_id) {
            cpha = (s_cfg_cache[hw->bus_id].mode & 0x01);
            break;
        }
    }
    return cpha;
}

static inline uint8_t spi_soft_cpol(const spi_hw_bus_t *hw)
{
    (void)hw;
    uint8_t cpol = 0;
    for (uint16_t i = 0; i < SPI_BUS_TABLE_SIZE; ++i) {
        if (g_spi_bus_map[i].bus_id == hw->bus_id) {
            cpol = (s_cfg_cache[hw->bus_id].mode >> 1) & 0x01;
            break;
        }
    }
    return cpol;
}

/**
 * @brief  SCK 前沿 (空闲态 -> 活动态)
 *         CPOL=0: 0 -> 1 (上升沿)
 *         CPOL=1: 1 -> 0 (下降沿)
 */
static inline void spi_soft_sck_lead_edge(const spi_hw_bus_t *hw)
{
    uint8_t cpol = spi_soft_cpol(hw);
    spi_port_set_sck(hw, cpol ? 0 : 1);
}

/**
 * @brief  SCK 后沿 (活动态 -> 空闲态)
 *         CPOL=0: 1 -> 0 (下降沿)
 *         CPOL=1: 0 -> 1 (上升沿)
 */
static inline void spi_soft_sck_trail_edge(const spi_hw_bus_t *hw)
{
    uint8_t cpol = spi_soft_cpol(hw);
    spi_port_set_sck(hw, cpol ? 1 : 0);
}

/**
 * @brief  SCK 空闲电平
 */
static inline void spi_soft_sck_idle(const spi_hw_bus_t *hw)
{
    uint8_t cpol = spi_soft_cpol(hw);
    spi_port_set_sck(hw, cpol ? 1 : 0);
}

/**
 * @brief  软件 SPI 全双工字节传输 (MSB 或 LSB 先行)
 *
 * 时序 (CPHA=0, Mode 0/2):
 *   空闲 -> MOSI 输出 bit -> SCK 前沿 (采样) -> SCK 后沿 -> 下一位
 *
 * 时序 (CPHA=1, Mode 1/3):
 *   空闲 -> SCK 前沿 -> MOSI 输出 bit -> SCK 后沿 (采样) -> 下一位
 */
static uint8_t spi_soft_transfer_byte(const spi_hw_bus_t *hw, uint8_t tx_byte,
                                       spi_bit_order_t bit_order)
{
    uint8_t rx_byte = 0;

    for (int8_t i = 0; i < 8; ++i) {
        uint8_t bit;
        if (bit_order == SPI_MSB_FIRST) {
            bit = (tx_byte >> (7 - i)) & 0x01;
        } else {
            bit = (tx_byte >> i) & 0x01;
        }

        uint8_t cpha = spi_soft_cpha(hw);

        if (cpha == 0) {
            /* CPHA=0: 先输出 MOSI, 再前沿采样 */
            spi_port_set_mosi(hw, bit);
            spi_port_delay_us(SPI_SOFT_HALF_PERIOD_US);
            spi_soft_sck_lead_edge(hw);    /* 前沿: 从机采样 */
            spi_port_delay_us(SPI_SOFT_HALF_PERIOD_US);
            /* 采样 MISO */
            if (spi_port_read_miso(hw)) {
                if (bit_order == SPI_MSB_FIRST) {
                    rx_byte |= (1u << (7 - i));
                } else {
                    rx_byte |= (1u << i);
                }
            }
            spi_soft_sck_trail_edge(hw);  /* 后沿: 回到空闲 */
        } else {
            /* CPHA=1: 先前沿, 再输出 MOSI, 再后沿采样 */
            spi_soft_sck_lead_edge(hw);
            spi_port_delay_us(SPI_SOFT_HALF_PERIOD_US);
            spi_port_set_mosi(hw, bit);
            spi_port_delay_us(SPI_SOFT_HALF_PERIOD_US);
            spi_soft_sck_trail_edge(hw);   /* 后沿: 从机采样 */
            spi_port_delay_us(SPI_SOFT_HALF_PERIOD_US);
            /* 采样 MISO (在后沿之后) */
            if (spi_port_read_miso(hw)) {
                if (bit_order == SPI_MSB_FIRST) {
                    rx_byte |= (1u << (7 - i));
                } else {
                    rx_byte |= (1u << i);
                }
            }
        }
    }

    /* SCK 回到空闲态 */
    spi_soft_sck_idle(hw);
    return rx_byte;
}

/* ============================ 内部: 软件时序阻塞传输 ============================ */
/**
 * @brief  软件位时序实现的传输
 * @note   CS 由调用方控制 (spi_xfer / spi_write_reg 等)
 */
static spi_status_t spi_soft_master_xfer(const spi_hw_bus_t *hw,
                                         const uint8_t *tx_buf, uint8_t *rx_buf,
                                         uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    spi_bit_order_t order = s_cfg_cache[hw->bus_id].bit_order;

    for (uint16_t i = 0; i < len; ++i) {
        uint8_t tx = (tx_buf != NULL) ? tx_buf[i] : 0xFF;
        uint8_t rx = spi_soft_transfer_byte(hw, tx, order);
        if (rx_buf != NULL) {
            rx_buf[i] = rx;
        }
    }
    return SPI_OK;
}

/* ============================ 公共 API 实现 ============================ */

spi_status_t spi_bus_init(const spi_config_t *cfg)
{
    if (cfg == NULL) {
        return SPI_ERR_PARAM;
    }

    const spi_hw_bus_t *hw = spi_lookup(cfg->bus);
    if (hw == NULL) {
        return SPI_ERR_BUS;
    }

    /* 缓存配置 (软件 SPI 位时序需要 mode / bit_order) */
    if (cfg->bus < SPI_MAX_BUSES) {
        s_cfg_cache[cfg->bus] = *cfg;
    }

    return spi_port_init(hw, cfg);
}

spi_status_t spi_deinit(spi_bus_t bus)
{
    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) {
        return SPI_ERR_BUS;
    }
    return spi_port_deinit(hw);
}

/* ---------- CS 控制 ---------- */

spi_status_t spi_cs_select(spi_bus_t bus)
{
    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    spi_cs_polarity_t pol = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].cs_polarity : SPI_CS_ACTIVE_LOW;

    return spi_port_set_cs(hw, 1, pol);
}

spi_status_t spi_cs_deselect(spi_bus_t bus)
{
    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    spi_cs_polarity_t pol = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].cs_polarity : SPI_CS_ACTIVE_LOW;

    return spi_port_set_cs(hw, 0, pol);
}

/* ---------- 位时序 API ---------- */

uint8_t spi_transfer_byte(spi_bus_t bus, uint8_t byte)
{
    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return 0xFF;

    spi_bit_order_t order = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].bit_order : SPI_MSB_FIRST;

    return spi_soft_transfer_byte(hw, byte, order);
}

/* ---------- 阻塞主机传输 (优先硬件 xfer, 失败回退软件) ---------- */

spi_status_t spi_transfer(spi_bus_t bus,
                           const uint8_t *tx_buf, uint8_t *rx_buf,
                           uint16_t len, uint32_t timeout_ms)
{
    if (len == 0) return SPI_OK;
    if (rx_buf == NULL && tx_buf == NULL) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    /* 优先硬件传输 (不控制 CS) */
    spi_status_t st = spi_port_master_xfer(hw, tx_buf, rx_buf, len, tout, 0);
    if (st != SPI_ERR_UNSUPP) {
        return st;
    }

    /* 回退软件位时序 */
    return spi_soft_master_xfer(hw, tx_buf, rx_buf, len, tout);
}

spi_status_t spi_write(spi_bus_t bus,
                        const uint8_t *tx_buf, uint16_t len,
                        uint32_t timeout_ms)
{
    if (tx_buf == NULL || len == 0) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    spi_status_t st = spi_port_master_xfer(hw, tx_buf, NULL, len, tout, 0);
    if (st != SPI_ERR_UNSUPP) {
        return st;
    }
    return spi_soft_master_xfer(hw, tx_buf, NULL, len, tout);
}

spi_status_t spi_read(spi_bus_t bus,
                       uint8_t *rx_buf, uint16_t len,
                       uint32_t timeout_ms)
{
    if (rx_buf == NULL || len == 0) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    spi_status_t st = spi_port_master_xfer(hw, NULL, rx_buf, len, tout, 0);
    if (st != SPI_ERR_UNSUPP) {
        return st;
    }
    return spi_soft_master_xfer(hw, NULL, rx_buf, len, tout);
}

spi_status_t spi_xfer(spi_bus_t bus,
                      const uint8_t *tx_buf, uint8_t *rx_buf,
                      uint16_t len, uint32_t timeout_ms)
{
    if (len == 0) return SPI_OK;
    if (rx_buf == NULL && tx_buf == NULL) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    /* 优先硬件传输 (自动控制 CS) */
    spi_status_t st = spi_port_master_xfer(hw, tx_buf, rx_buf, len, tout, 1);
    if (st != SPI_ERR_UNSUPP) {
        return st;
    }

    /* 软件: 手动控制 CS */
    spi_cs_polarity_t pol = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].cs_polarity : SPI_CS_ACTIVE_LOW;

    spi_port_set_cs(hw, 1, pol);
    spi_port_delay_us(SPI_SOFT_CS_SETUP_US);

    st = spi_soft_master_xfer(hw, tx_buf, rx_buf, len, tout);

    spi_port_delay_us(SPI_SOFT_CS_HOLD_US);
    spi_port_set_cs(hw, 0, pol);
    return st;
}

spi_status_t spi_write_reg(spi_bus_t bus, uint8_t cmd,
                           const uint8_t *data, uint16_t len,
                           uint32_t timeout_ms)
{
    if (data == NULL && len != 0) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    spi_cs_polarity_t pol = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].cs_polarity : SPI_CS_ACTIVE_LOW;

    /* 软件: CS LOW + cmd + data + CS HIGH */
    spi_port_set_cs(hw, 1, pol);
    spi_port_delay_us(SPI_SOFT_CS_SETUP_US);

    spi_bit_order_t order = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].bit_order : SPI_MSB_FIRST;

    spi_soft_transfer_byte(hw, cmd, order);
    for (uint16_t i = 0; i < len; ++i) {
        spi_soft_transfer_byte(hw, data[i], order);
    }

    spi_port_delay_us(SPI_SOFT_CS_HOLD_US);
    spi_port_set_cs(hw, 0, pol);

    (void)tout; /* 软件时序不实现精确超时 */
    return SPI_OK;
}

spi_status_t spi_read_reg(spi_bus_t bus, uint8_t cmd,
                          uint8_t *buf, uint16_t len,
                          uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    spi_cs_polarity_t pol = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].cs_polarity : SPI_CS_ACTIVE_LOW;

    spi_bit_order_t order = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].bit_order : SPI_MSB_FIRST;

    /* 软件: CS LOW + cmd + dummy read[0..n-1] + CS HIGH */
    spi_port_set_cs(hw, 1, pol);
    spi_port_delay_us(SPI_SOFT_CS_SETUP_US);

    spi_soft_transfer_byte(hw, cmd, order);
    for (uint16_t i = 0; i < len; ++i) {
        buf[i] = spi_soft_transfer_byte(hw, 0xFF, order);
    }

    spi_port_delay_us(SPI_SOFT_CS_HOLD_US);
    spi_port_set_cs(hw, 0, pol);

    (void)tout;
    return SPI_OK;
}

spi_status_t spi_send_cmd(spi_bus_t bus,
                          const uint8_t *cmd_buf, uint8_t cmd_len,
                          uint32_t timeout_ms)
{
    if (cmd_buf == NULL || cmd_len == 0) return SPI_ERR_PARAM;

    const spi_hw_bus_t *hw = spi_lookup(bus);
    if (hw == NULL) return SPI_ERR_BUS;

    uint32_t tout = (timeout_ms == 0 && bus < SPI_MAX_BUSES) ?
                    s_cfg_cache[bus].timeout_ms : timeout_ms;

    spi_cs_polarity_t pol = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].cs_polarity : SPI_CS_ACTIVE_LOW;

    spi_bit_order_t order = (bus < SPI_MAX_BUSES) ?
        s_cfg_cache[bus].bit_order : SPI_MSB_FIRST;

    spi_port_set_cs(hw, 1, pol);
    spi_port_delay_us(SPI_SOFT_CS_SETUP_US);

    for (uint8_t i = 0; i < cmd_len; ++i) {
        spi_soft_transfer_byte(hw, cmd_buf[i], order);
    }

    spi_port_delay_us(SPI_SOFT_CS_HOLD_US);
    spi_port_set_cs(hw, 0, pol);

    (void)tout;
    return SPI_OK;
}
