/**
 * @file    spi_port_at32.c
 * @brief   可复用 SPI 驱动模板 - Artery AT32F403A/407 移植实现 (硬件 SPI + DMA)
 *
 * @note    本文件实现 spi_port.h 声明的 port 函数, 只支持"硬件 SPI"路线:
 *            - SPI1/SPI2/SPI3 主机, 单线半双工发送 (SPI_TRANSMIT_HALF_DUPLEX_TX)
 *            - TX 走 DMA: SPI1->DMA1_CH3, SPI2->DMA1_CH5, SPI3->DMA2_CH2
 *              (AT32F403A 手册 表9-3/表9-4 固定请求映射, 与 STM32 不完全相同)
 *            - 短传输 (< SPI_DMA_MIN_LEN) 自动走轮询, 省掉 DMA 启动开销
 *
 *          软件位时序路线未实现 (set_sck/set_mosi/read_miso 返回 UNSUPP),
 *          需要时按 gpio_port_at32.c 的写法补 3 个函数即可.
 *
 * @note    为什么用"单线半双工": 本工程 PA6(MISO) 被借去当 LCD 的 DC/A0,
 *          SPI1 不能占 MISO. 半双工发送只用 MOSI(PA7) 一根线, 推屏足够.
 *
 * @note    传输语义是"启动 DMA -> 等完成 -> 等 SPI 空闲"的同步返回:
 *          spi.h 的 API 约定返回即代表数据已发完, 否则 st7789 会在数据
 *          还没移完时拉高 CS, 屏幕最后一点会花. 想真正并行要改 st7789/spi.h
 *          做双缓冲, 不在本文件范围内.
 */
#include "spi_port.h"
#include "at32f403a_407.h"
#include "at32f403a_407_board.h"   /* delay_us() */

/* ============================ 内部: 实例与 DMA 表 ============================ */

typedef struct {
    dma_channel_type     *ch;        /* TX 通道          */
    uint32_t              fdt_flag;  /* 传输完成标志      */
    crm_periph_clock_type dma_clk;   /* DMA 控制器时钟    */
} spi_at32_dma_t;

/* 索引与 g_spi_bus_map[] 的 HW_SPI_HARD(instance, ...) 一致: 0=SPI1 */
static const spi_at32_dma_t s_dma_tx[3] = {
    { DMA1_CHANNEL3, DMA1_FDT3_FLAG, CRM_DMA1_PERIPH_CLOCK },  /* SPI1_TX */
    { DMA1_CHANNEL5, DMA1_FDT5_FLAG, CRM_DMA1_PERIPH_CLOCK },  /* SPI2_TX */
    { DMA2_CHANNEL2, DMA2_FDT2_FLAG, CRM_DMA2_PERIPH_CLOCK },  /* SPI3_TX */
};

static spi_type *spi_at32_instance(uint32_t idx)
{
    switch (idx) {
        case 0u: return SPI1;
        case 1u: return SPI2;
        case 2u: return SPI3;
        default: return NULL;
    }
}

static crm_periph_clock_type spi_at32_clk(uint32_t idx)
{
    return (idx == 0u) ? CRM_SPI1_PERIPH_CLOCK
         : (idx == 1u) ? CRM_SPI2_PERIPH_CLOCK
                       : CRM_SPI3_PERIPH_CLOCK;
}

/* ============================ 内部: 引脚句柄解码 ============================ */

static gpio_type *hw_to_port(uint32_t h)
{
    switch ((h >> 8) & 0xFF) {
        case 0u: return GPIOA;
        case 1u: return GPIOB;
        case 2u: return GPIOC;
        case 3u: return GPIOD;
        case 4u: return GPIOE;
        default: return NULL;
    }
}

static uint16_t hw_to_mask(uint32_t h)
{
    return (uint16_t)(1u << (h & 0xFFu));
}

static void port_clock_enable(gpio_type *port)
{
         if (port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    else if (port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    else if (port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    else if (port == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    else if (port == GPIOE) crm_periph_clock_enable(CRM_GPIOE_PERIPH_CLOCK, TRUE);
}

/* ============================ 内部: 速度 / 模式映射 ============================ */

/**
 * @brief SPI 速度等级 -> 分频系数
 * @note  按"外设时钟 / 分频 <= 目标波特率"取最小分频, 时钟树改了也不用动表.
 *        当前 APB2 = APB1 = 120MHz:
 *          LOW 2M -> /64 = 1.875M   STD 4M  -> /32 = 3.75M
 *          FAST 8M -> /16 = 7.5M    VERY_FAST 16M -> /8 = 15M
 *          ULTRA 32M -> /4 = 30M
 */
static spi_mclk_freq_div_type spi_at32_speed_to_div(spi_speed_t speed, uint32_t pclk)
{
    static const struct {
        spi_mclk_freq_div_type div_enum;
        uint32_t               div_val;
    } table[] = {
        { SPI_MCLK_DIV_2,   2u   }, { SPI_MCLK_DIV_4,   4u   },
        { SPI_MCLK_DIV_8,   8u   }, { SPI_MCLK_DIV_16,  16u  },
        { SPI_MCLK_DIV_32,  32u  }, { SPI_MCLK_DIV_64,  64u  },
        { SPI_MCLK_DIV_128, 128u }, { SPI_MCLK_DIV_256, 256u },
    };
    uint32_t target;

    switch (speed) {
        case SPI_SPEED_LOW:      target = 2000000u;  break;
        case SPI_SPEED_STANDARD: target = 4000000u;  break;
        case SPI_SPEED_FAST:     target = 8000000u;  break;
        case SPI_SPEED_ULTRA:    target = 32000000u; break;
        case SPI_SPEED_VERY_FAST:
        default:                 target = 16000000u; break;
    }

    for (uint8_t i = 0u; i < (uint8_t)(sizeof(table) / sizeof(table[0])); ++i) {
        if ((pclk / table[i].div_val) <= target) {
            return table[i].div_enum;
        }
    }
    return SPI_MCLK_DIV_256;   /* 时钟过高时兜底降到最慢 */
}

/* ============================ 内部: 引脚初始化 ============================ */

/* CS: 推挽输出, 初始高 (未选中) */
static spi_status_t spi_at32_init_cs(uint32_t cs_handle)
{
    gpio_init_type gi;
    gpio_type *port = hw_to_port(cs_handle);
    uint16_t   mask = hw_to_mask(cs_handle);

    if (port == NULL) {
        return SPI_ERR_HW;
    }
    port_clock_enable(port);

    gpio_default_para_init(&gi);
    gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gi.gpio_pull           = GPIO_PULL_NONE;
    gi.gpio_mode           = GPIO_MODE_OUTPUT;
    gi.gpio_pins           = mask;
    gpio_init(port, &gi);

    gpio_bits_set(port, mask);
    return SPI_OK;
}

/* SCK / MOSI: 复用功能 (SPI1 默认映射, 无需重映射) */
static spi_status_t spi_at32_init_bus_pins(uint32_t idx)
{
    const spi_hw_pins_t *pins = NULL;
    gpio_init_type gi;

    for (uint8_t i = 0u; i < (uint8_t)SPI_HW_PIN_TABLE_SIZE; ++i) {
        if (g_spi_hw_pins[i].instance == (uint8_t)idx) {
            pins = &g_spi_hw_pins[i];
            break;
        }
    }
    if (pins == NULL) {
        return SPI_ERR_HW;   /* spi_cfg.h 的 g_spi_hw_pins[] 漏登记 */
    }

    gpio_type *sck_port  = hw_to_port(pins->sck_hw);
    gpio_type *mosi_port = hw_to_port(pins->mosi_hw);
    if (sck_port == NULL || mosi_port == NULL) {
        return SPI_ERR_HW;
    }

    port_clock_enable(sck_port);
    port_clock_enable(mosi_port);

    gpio_default_para_init(&gi);
    gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gi.gpio_pull           = GPIO_PULL_NONE;
    gi.gpio_mode           = GPIO_MODE_MUX;

    gi.gpio_pins = hw_to_mask(pins->sck_hw);
    gpio_init(sck_port, &gi);

    gi.gpio_pins = hw_to_mask(pins->mosi_hw);
    gpio_init(mosi_port, &gi);

    return SPI_OK;
}

/* ============================ 内部: DMA ============================ */

static spi_status_t spi_at32_dma_init(uint32_t idx, spi_type *spi)
{
    dma_init_type dma_cfg;
    const spi_at32_dma_t *d = &s_dma_tx[idx];

    crm_periph_clock_enable(d->dma_clk, TRUE);

    dma_reset(d->ch);
    dma_default_para_init(&dma_cfg);
    dma_cfg.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
    dma_cfg.buffer_size           = 1u;
    dma_cfg.memory_base_addr      = 0u;
    dma_cfg.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
    dma_cfg.memory_inc_enable     = TRUE;
    dma_cfg.peripheral_base_addr  = (uint32_t)&(spi->dt);
    dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
    dma_cfg.peripheral_inc_enable = FALSE;
    dma_cfg.loop_mode_enable      = FALSE;
    dma_cfg.priority              = DMA_PRIORITY_HIGH;
    dma_init(d->ch, &dma_cfg);

    dma_channel_enable(d->ch, FALSE);
    dma_flag_clear(d->fdt_flag);
    return SPI_OK;
}

/**
 * @brief  DMA 推送一段数据 (返回时数据已全部移出且 SPI 空闲)
 */
static spi_status_t spi_at32_dma_write(uint32_t idx, spi_type *spi,
                                        const uint8_t *buf, uint16_t len,
                                        uint32_t timeout_ms)
{
    const spi_at32_dma_t *d = &s_dma_tx[idx];
    uint32_t guard = (timeout_ms ? timeout_ms : 100u) * 20000u;

    dma_channel_enable(d->ch, FALSE);
    dma_flag_clear(d->fdt_flag);

    /* 每次传输只改内存地址与长度, 方向/宽度/外设地址沿用 init 时的配置 */
    d->ch->maddr = (uint32_t)buf;
    dma_data_number_set(d->ch, len);
    dma_channel_enable(d->ch, TRUE);

    while (dma_flag_get(d->fdt_flag) == RESET) {
        if (--guard == 0u) {
            dma_channel_enable(d->ch, FALSE);
            return SPI_ERR_TIMEOUT;
        }
    }
    dma_flag_clear(d->fdt_flag);
    dma_channel_enable(d->ch, FALSE);

    /* DMA 传完最后一个字节后, 移位寄存器里可能还有 1 字节没出去.
     * 必须等 BUSY 清零再返回, 否则调用方拉高 CS 会截断最后一位 (屏幕边缘花点). */
    guard = (timeout_ms ? timeout_ms : 100u) * 20000u;
    while (spi_i2s_flag_get(spi, SPI_I2S_BF_FLAG) != RESET) {
        if (--guard == 0u) {
            return SPI_ERR_TIMEOUT;
        }
    }
    return SPI_OK;
}

/* ============================ 内部: 轮询发送 ============================ */

static spi_status_t spi_at32_poll_write(spi_type *spi, const uint8_t *buf,
                                         uint16_t len, uint32_t timeout_ms)
{
    uint32_t guard;

    for (uint16_t i = 0u; i < len; ++i) {
        guard = (timeout_ms ? timeout_ms : 100u) * 20000u;
        while (spi_i2s_flag_get(spi, SPI_I2S_TDBE_FLAG) == RESET) {
            if (--guard == 0u) {
                return SPI_ERR_TIMEOUT;
            }
        }
        spi_i2s_data_transmit(spi, buf[i]);
    }

    guard = (timeout_ms ? timeout_ms : 100u) * 20000u;
    while (spi_i2s_flag_get(spi, SPI_I2S_BF_FLAG) != RESET) {
        if (--guard == 0u) {
            return SPI_ERR_TIMEOUT;
        }
    }
    return SPI_OK;
}

/* ============================ port 函数实现 ============================ */

spi_status_t spi_port_init(const spi_hw_bus_t *hw, const spi_config_t *cfg)
{
    spi_init_type spi_cfg;
    crm_clocks_freq_type clocks;
    uint32_t pclk;
    uint32_t idx;
    uint32_t cs_handle;

    if (hw == NULL || cfg == NULL) {
        return SPI_ERR_PARAM;
    }
    if ((hw->hw_handle & SPI_BUS_TYPE_HARD) == 0u) {
        return SPI_ERR_UNSUPP;   /* 本移植只实现硬件 SPI */
    }

    idx = hw->hw_handle & 0x7Fu;
    if (idx >= (uint32_t)(sizeof(s_dma_tx) / sizeof(s_dma_tx[0]))) {
        return SPI_ERR_HW;
    }

    spi_type *spi = spi_at32_instance(idx);
    if (spi == NULL) {
        return SPI_ERR_HW;
    }

    /* 1. SCK / MOSI 复用脚 */
    if (spi_at32_init_bus_pins(idx) != SPI_OK) {
        return SPI_ERR_HW;
    }

    /* 2. CS 脚 (软件控制) */
    cs_handle = (hw->hw_handle >> 8) & 0xFFFFu;
    if (cs_handle != 0u) {
        if (spi_at32_init_cs(cs_handle) != SPI_OK) {
            return SPI_ERR_HW;
        }
    }

    /* 3. 取总线的外设时钟 (SPI1 走 APB2, SPI2/3 走 APB1) */
    crm_clocks_freq_get(&clocks);
    pclk = (idx == 0u) ? clocks.apb2_freq : clocks.apb1_freq;

    /* 4. SPI 参数 */
    crm_periph_clock_enable(spi_at32_clk(idx), TRUE);

    spi_default_para_init(&spi_cfg);
    spi_cfg.transmission_mode    = SPI_TRANSMIT_HALF_DUPLEX_TX;   /* 只用 MOSI */
    spi_cfg.master_slave_mode    = SPI_MODE_MASTER;
    spi_cfg.mclk_freq_division   = spi_at32_speed_to_div(cfg->speed, pclk);
    spi_cfg.first_bit_transmission = (cfg->bit_order == SPI_LSB_FIRST)
                                     ? SPI_FIRST_BIT_LSB : SPI_FIRST_BIT_MSB;
    spi_cfg.frame_bit_num        = (cfg->data_size == SPI_DATA_16BIT)
                                   ? SPI_FRAME_16BIT : SPI_FRAME_8BIT;
    spi_cfg.clock_polarity       = (cfg->mode & 0x02)
                                   ? SPI_CLOCK_POLARITY_HIGH : SPI_CLOCK_POLARITY_LOW;
    spi_cfg.clock_phase          = (cfg->mode & 0x01)
                                   ? SPI_CLOCK_PHASE_2EDGE : SPI_CLOCK_PHASE_1EDGE;
    spi_cfg.cs_mode_selection    = SPI_CS_SOFTWARE_MODE;
    spi_init(spi, &spi_cfg);

    /* 5. DMA */
    if (spi_at32_dma_init(idx, spi) != SPI_OK) {
        return SPI_ERR_HW;
    }
    spi_i2s_dma_transmitter_enable(spi, TRUE);

    spi_enable(spi, TRUE);
    return SPI_OK;
}

spi_status_t spi_port_deinit(const spi_hw_bus_t *hw)
{
    uint32_t idx;

    if (hw == NULL) {
        return SPI_ERR_PARAM;
    }
    if ((hw->hw_handle & SPI_BUS_TYPE_HARD) == 0u) {
        return SPI_ERR_UNSUPP;
    }

    idx = hw->hw_handle & 0x7Fu;
    if (idx >= (uint32_t)(sizeof(s_dma_tx) / sizeof(s_dma_tx[0]))) {
        return SPI_ERR_HW;
    }

    spi_i2s_dma_transmitter_enable(spi_at32_instance(idx), FALSE);
    spi_enable(spi_at32_instance(idx), FALSE);
    dma_channel_enable(s_dma_tx[idx].ch, FALSE);
    return SPI_OK;
}

/* ---------- CS 控制 ---------- */

spi_status_t spi_port_set_cs(const spi_hw_bus_t *hw, uint8_t active,
                              spi_cs_polarity_t cs_polarity)
{
    uint32_t cs_handle;
    gpio_type *port;
    uint16_t   mask;

    if (hw == NULL) {
        return SPI_ERR_PARAM;
    }
    if ((hw->hw_handle & SPI_BUS_TYPE_HARD) == 0u) {
        return SPI_ERR_UNSUPP;
    }

    /* 硬件 SPI: CS 句柄在 bit[23:8] */
    cs_handle = (hw->hw_handle >> 8) & 0xFFFFu;
    port = hw_to_port(cs_handle);
    if (port == NULL) {
        return SPI_ERR_HW;
    }
    mask = hw_to_mask(cs_handle);

    /* active=1 选中: 低有效时拉低, 高有效时拉高 */
    if (cs_polarity == SPI_CS_ACTIVE_LOW) {
        if (active) {
            gpio_bits_reset(port, mask);
        } else {
            gpio_bits_set(port, mask);
        }
    } else {
        if (active) {
            gpio_bits_set(port, mask);
        } else {
            gpio_bits_reset(port, mask);
        }
    }
    return SPI_OK;
}

/* ---------- 软件位时序层 (硬件 SPI 不支持) ---------- */

spi_status_t spi_port_set_sck(const spi_hw_bus_t *hw, uint8_t state)
{
    (void)hw;
    (void)state;
    return SPI_ERR_UNSUPP;
}

spi_status_t spi_port_set_mosi(const spi_hw_bus_t *hw, uint8_t state)
{
    (void)hw;
    (void)state;
    return SPI_ERR_UNSUPP;
}

uint8_t spi_port_read_miso(const spi_hw_bus_t *hw)
{
    (void)hw;
    return 1u;   /* 单线半双工无 MISO, 返回空闲高 */
}

spi_status_t spi_port_delay_us(uint32_t us)
{
    delay_us(us);   /* 复用 AT32 BSP 的 SysTick 轮询延时 */
    return SPI_OK;
}

/* ---------- 硬件 SPI 原子传输 ---------- */

spi_status_t spi_port_master_xfer(const spi_hw_bus_t *hw,
                                   const uint8_t *tx_buf, uint8_t *rx_buf,
                                   uint16_t len, uint32_t timeout_ms,
                                   uint8_t cs_control)
{
    spi_status_t st;
    uint32_t idx;

    if (hw == NULL) {
        return SPI_ERR_PARAM;
    }
    if ((hw->hw_handle & SPI_BUS_TYPE_HARD) == 0u) {
        return SPI_ERR_UNSUPP;   /* 软件总线交给通用层 */
    }

    idx = hw->hw_handle & 0x7Fu;
    if (idx >= (uint32_t)(sizeof(s_dma_tx) / sizeof(s_dma_tx[0]))) {
        return SPI_ERR_HW;
    }

    spi_type *spi = spi_at32_instance(idx);
    if (spi == NULL) {
        return SPI_ERR_HW;
    }

    /* 单线半双工发送: 不支持接收方向 (本项目 LCD 只写) */
    if (rx_buf != NULL) {
        return SPI_ERR_UNSUPP;
    }
    if (tx_buf == NULL || len == 0u) {
        return SPI_OK;
    }

    if (cs_control) {
        (void)spi_port_set_cs(hw, 1u, SPI_CS_ACTIVE_LOW);
    }

    if (len >= SPI_DMA_MIN_LEN) {
        st = spi_at32_dma_write(idx, spi, tx_buf, len, timeout_ms);
    } else {
        st = spi_at32_poll_write(spi, tx_buf, len, timeout_ms);
    }

    if (cs_control) {
        (void)spi_port_set_cs(hw, 0u, SPI_CS_ACTIVE_LOW);
    }
    return st;
}
