/**
 * @file    spi_port_stm32_hal.c
 * @brief   可复用 SPI 驱动模板 - STM32 HAL 库移植实现
 * @note    本文件覆盖 spi_port.c 中的弱定义桩, 同时支持:
 *            A. 软件 SPI (bit-bang): 通过 HAL_GPIO_* 操作 SCK/MOSI/MISO/CS 引脚
 *            B. 硬件 SPI            : 通过 HAL_SPI_TransmitReceive / HAL_SPI_Transmit 等
 *
 *          适用 STM32F1/F4/F7/H7 系列 (代码以 STM32F407 为例, F1 仅 RCC 时钟源不同).
 *          通过 hw_handle 的最高位区分软件/硬件:
 *            bit31 = 0 : 软件 SPI, hw_handle = (sck<<24)|(mosi<<16)|(miso<<8)|cs
 *            bit31 = 1 : 硬件 SPI, hw_handle 低 8 位 = SPI 实例索引 (0=SPI1,...)
 *                        bit[15:8] = CS 引脚句柄
 *
 *          引脚句柄编码 (与 GPIO 驱动一致):
 *            bit[15:8] = 端口 (GPIOA=0,...,GPIOE=4)
 *            bit[7:0]  = 引脚号 (0~15)
 *
 *          STM32 SPI 时钟源:
 *            F1: APB2 (SPI1), APB1 (SPI2)
 *            F4: APB2 (SPI1/SPI4/SPI5/SPI6), APB1 (SPI2/SPI3)
 *            H7: SPI1/2/3 可选多个时钟域
 *
 *          CubeMX 配置 (本工程 = SPI_Driver_W25Q16, STM32F407VETx):
 *            1. SPI1 -> Mode: Full-Duplex Master
 *            2. PB3 -> SPI1_SCK, PB4 -> SPI1_MISO, PB5 -> SPI1_MOSI (AF5)
 *            3. PB0 -> GPIO Output (CS, 软件控制, 由 BSP/GPIO_Driver 初始化)
 *            4. Parameter Settings: Prescaler=8 (APB2 84MHz/8 = 10.5MHz)
 *               CPOL=Low, CPHA=1 Edge (Mode 0), MSB First, 8-bit
 *            注: PB3/PB4 复位后默认是 JTDO/NJTRST, 本工程用 SWD 调试,
 *                配成 AF5 后即可作为 SPI1 使用, 无需额外 remap (F4 无 AFIO).
 */
#include "spi_port.h"
#include "stm32f4xx_hal.h"   /* 若工程是 F1, 改为 stm32f1xx_hal.h */

/* ============================ 内部: 引脚句柄解码 ============================ */

static GPIO_TypeDef *spi_stm32_port_from_handle(uint16_t h)
{
    switch ((h >> 8) & 0xFF) {
        case 0: return GPIOA;
        case 1: return GPIOB;
        case 2: return GPIOC;
        case 3: return GPIOD;
        case 4: return GPIOE;
#ifdef GPIOF
        case 5: return GPIOF;
#endif
#ifdef GPIOG
        case 6: return GPIOG;
#endif
#ifdef GPIOH
        case 7: return GPIOH;
#endif
        default: return NULL;
    }
}

static uint16_t spi_stm32_pin_mask_from_handle(uint16_t h)
{
    return (uint16_t)(1u << (h & 0xFF));
}

static void spi_stm32_enable_gpio_clk(GPIO_TypeDef *port)
{
         if (port == GPIOA) __HAL_RCC_GPIOA_CLK_ENABLE();
    else if (port == GPIOB) __HAL_RCC_GPIOB_CLK_ENABLE();
    else if (port == GPIOC) __HAL_RCC_GPIOC_CLK_ENABLE();
    else if (port == GPIOD) __HAL_RCC_GPIOD_CLK_ENABLE();
    else if (port == GPIOE) __HAL_RCC_GPIOE_CLK_ENABLE();
#ifdef __HAL_RCC_GPIOF_CLK_ENABLE
    else if (port == GPIOF) __HAL_RCC_GPIOF_CLK_ENABLE();
#endif
#ifdef __HAL_RCC_GPIOG_CLK_ENABLE
    else if (port == GPIOG) __HAL_RCC_GPIOG_CLK_ENABLE();
#endif
#ifdef __HAL_RCC_GPIOH_CLK_ENABLE
    else if (port == GPIOH) __HAL_RCC_GPIOH_CLK_ENABLE();
#endif
}

/* ============================ 内部: SPI 硬件实例查找 ============================ */

/* 用户工程需要提供 SPI 句柄 (CubeMX 生成, 通常在 spi.c 中定义)
 * 本工程只启用了 SPI1 (见 spi_cfg.h 的 SPI_HW_SPIx_ENABLE).
 * 未启用的实例不做 extern / 查表引用, 否则链接期会报 "undefined hspi2/hspi3".
 */
#if SPI_HW_SPI1_ENABLE
extern SPI_HandleTypeDef hspi1;
#endif
#if SPI_HW_SPI2_ENABLE
extern SPI_HandleTypeDef hspi2;
#endif
#if SPI_HW_SPI3_ENABLE
extern SPI_HandleTypeDef hspi3;
#endif

static SPI_HandleTypeDef *spi_stm32_lookup_spi(uint32_t instance_idx)
{
    switch (instance_idx) {
#if SPI_HW_SPI1_ENABLE
        case 0: return &hspi1;
#endif
#if SPI_HW_SPI2_ENABLE
        case 1: return &hspi2;
#endif
#if SPI_HW_SPI3_ENABLE
        case 2: return &hspi3;
#endif
        default: return NULL;
    }
}

/**
 * @brief  速度等级映射到 HAL SPI 分频系数
 * @note   STM32F407 SPI1 挂 APB2 = 84MHz
 *         SPI_SPEED_LOW      -> ~1 MHz  (Prescaler 64: 84M/64 = 1.3MHz)
 *         SPI_SPEED_STANDARD -> ~4 MHz  (Prescaler 16: 84M/16 = 5.25MHz)
 *         SPI_SPEED_FAST     -> ~8 MHz  (Prescaler 8:  84M/8 = 10.5MHz)
 *         SPI_SPEED_VERY_FAST-> ~20 MHz (Prescaler 4:  84M/4 = 21MHz)
 *         SPI_SPEED_ULTRA    -> ~40 MHz (Prescaler 2:  84M/2 = 42MHz)
 */
static uint32_t spi_stm32_speed_to_baudrate(spi_speed_t s)
{
    switch (s) {
        case SPI_SPEED_LOW:       return SPI_BAUDRATEPRESCALER_64;
        case SPI_SPEED_STANDARD:  return SPI_BAUDRATEPRESCALER_16;
        case SPI_SPEED_FAST:      return SPI_BAUDRATEPRESCALER_8;
        case SPI_SPEED_VERY_FAST: return SPI_BAUDRATEPRESCALER_4;
        case SPI_SPEED_ULTRA:     return SPI_BAUDRATEPRESCALER_2;
        default:                  return SPI_BAUDRATEPRESCALER_16;
    }
}

/**
 * @brief  SPI 模式映射到 HAL CPOL/CPHA
 *         Mode 0: CPOL=Low,  CPHA=1 Edge
 *         Mode 1: CPOL=Low,  CPHA=2 Edge
 *         Mode 2: CPOL=High, CPHA=1 Edge
 *         Mode 3: CPOL=High, CPHA=2 Edge
 */
static void spi_stm32_mode_to_hal(spi_mode_t mode,
                                   uint32_t *cpol, uint32_t *cpha)
{
    *cpol = (mode & 0x02) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
    *cpha = (mode & 0x01) ? SPI_PHASE_2EDGE   : SPI_PHASE_1EDGE;
}

/* ============================ 内部: DWT 微秒延时 ============================ */

static void spi_stm32_dwt_init(void)
{
    CoreDebug->DEMCR |= (1u << 24);
    DWT->CYCCNT = 0;
    DWT->CTRL |= (1u << 0);
}

static inline uint32_t spi_stm32_cycles_per_us(void)
{
    return SystemCoreClock / 1000000u;
}

/* ============================ 内部: CS 引脚操作 ============================ */

static spi_status_t spi_stm32_init_cs_pin(uint32_t cs_handle)
{
    GPIO_TypeDef *port = spi_stm32_port_from_handle((uint16_t)cs_handle);
    uint16_t mask = spi_stm32_pin_mask_from_handle((uint16_t)cs_handle);
    if (port == NULL) return SPI_ERR_HW;

    spi_stm32_enable_gpio_clk(port);

    GPIO_InitTypeDef gi = {0};
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Pin = mask;
    HAL_GPIO_Init(port, &gi);

    /* CS 初始释放 (高电平 = 不选中) */
    HAL_GPIO_WritePin(port, mask, GPIO_PIN_SET);
    return SPI_OK;
}

/* ============================ port 函数实现 ============================ */

spi_status_t spi_port_init(const spi_hw_bus_t *hw, const spi_config_t *cfg)
{
    if (hw == NULL || cfg == NULL) {
        return SPI_ERR_PARAM;
    }

    spi_stm32_dwt_init();

    uint32_t h = hw->hw_handle;

    if (h & SPI_BUS_TYPE_HARD) {
        /* ---------- 硬件 SPI ---------- */
        uint32_t idx = h & 0x7Fu;
        SPI_HandleTypeDef *hspi = spi_stm32_lookup_spi(idx);
        if (hspi == NULL) {
            return SPI_ERR_HW;
        }

        /* 获取 CS 引脚并配置 */
        uint32_t cs_handle = (h >> 8) & 0xFFFFu;
        if (cs_handle != 0) {
            spi_status_t st = spi_stm32_init_cs_pin(cs_handle);
            if (st != SPI_OK) return st;
        }

        /* 配置 SPI 参数 (若 CubeMX 已配置则覆盖) */
        uint32_t cpol, cpha;
        spi_stm32_mode_to_hal(cfg->mode, &cpol, &cpha);

        hspi->Init.Mode = SPI_MODE_MASTER;
        hspi->Init.NSS = SPI_NSS_SOFT;
        hspi->Init.BaudRatePrescaler = spi_stm32_speed_to_baudrate(cfg->speed);
        hspi->Init.CLKPolarity = cpol;
        hspi->Init.CLKPhase = cpha;
        hspi->Init.FirstBit = (cfg->bit_order == SPI_LSB_FIRST) ?
                               SPI_FIRSTBIT_LSB : SPI_FIRSTBIT_MSB;
        hspi->Init.DataSize = (cfg->data_size == SPI_DATA_16BIT) ?
                               SPI_DATASIZE_16BIT : SPI_DATASIZE_8BIT;
        hspi->Init.CRCPolynomial = 7;
        hspi->Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;

        if (HAL_SPI_Init(hspi) != HAL_OK) {
            return SPI_ERR_HW;
        }
        return SPI_OK;
    }

    /* ---------- 软件位时序 ---------- */
    /* 解码 4 个引脚: SCK, MOSI, MISO, CS */
    uint16_t sck_h  = (uint16_t)((h >> 24) & 0xFFFFu);
    uint16_t mosi_h = (uint16_t)((h >> 16) & 0xFFFFu);
    uint16_t miso_h = (uint16_t)((h >> 8)  & 0xFFFFu);
    uint16_t cs_h   = (uint16_t)( h        & 0xFFFFu);

    GPIO_TypeDef *sck_port  = spi_stm32_port_from_handle(sck_h);
    GPIO_TypeDef *mosi_port = spi_stm32_port_from_handle(mosi_h);
    GPIO_TypeDef *miso_port = spi_stm32_port_from_handle(miso_h);
    GPIO_TypeDef *cs_port   = spi_stm32_port_from_handle(cs_h);

    if (sck_port == NULL || mosi_port == NULL || cs_port == NULL) {
        return SPI_ERR_HW;
    }

    /* 使能 GPIO 时钟 */
    spi_stm32_enable_gpio_clk(sck_port);
    spi_stm32_enable_gpio_clk(mosi_port);
    if (miso_port) spi_stm32_enable_gpio_clk(miso_port);
    spi_stm32_enable_gpio_clk(cs_port);

    GPIO_InitTypeDef gi = {0};
    gi.Speed = GPIO_SPEED_FREQ_HIGH;

    /* SCK: 推挽输出 */
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Pin = spi_stm32_pin_mask_from_handle(sck_h);
    HAL_GPIO_Init(sck_port, &gi);
    /* SCK 空闲电平 (CPOL=0 -> 低, CPOL=1 -> 高) */
    HAL_GPIO_WritePin(sck_port, gi.Pin,
        (cfg->mode & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* MOSI: 推挽输出 */
    gi.Pin = spi_stm32_pin_mask_from_handle(mosi_h);
    HAL_GPIO_Init(mosi_port, &gi);
    HAL_GPIO_WritePin(mosi_port, gi.Pin, GPIO_PIN_RESET);

    /* MISO: 输入 (上拉) */
    if (miso_port) {
        gi.Mode = GPIO_MODE_INPUT;
        gi.Pull = GPIO_PULLUP;
        gi.Pin = spi_stm32_pin_mask_from_handle(miso_h);
        HAL_GPIO_Init(miso_port, &gi);
    }

    /* CS: 推挽输出, 初始释放 (高) */
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Pin = spi_stm32_pin_mask_from_handle(cs_h);
    HAL_GPIO_Init(cs_port, &gi);
    HAL_GPIO_WritePin(cs_port, gi.Pin, GPIO_PIN_SET);

    return SPI_OK;
}

spi_status_t spi_port_deinit(const spi_hw_bus_t *hw)
{
    if (hw == NULL) return SPI_ERR_PARAM;
    uint32_t h = hw->hw_handle;

    if (h & SPI_BUS_TYPE_HARD) {
        uint32_t idx = h & 0x7Fu;
        SPI_HandleTypeDef *hspi = spi_stm32_lookup_spi(idx);
        if (hspi == NULL) return SPI_ERR_HW;
        HAL_SPI_DeInit(hspi);
        return SPI_OK;
    }

    /* 软件: 释放所有 GPIO */
    uint16_t pins[] = {
        (uint16_t)((h >> 24) & 0xFFFFu),
        (uint16_t)((h >> 16) & 0xFFFFu),
        (uint16_t)((h >> 8)  & 0xFFFFu),
        (uint16_t)( h        & 0xFFFFu),
    };
    for (int i = 0; i < 4; ++i) {
        GPIO_TypeDef *port = spi_stm32_port_from_handle(pins[i]);
        if (port) {
            HAL_GPIO_DeInit(port, spi_stm32_pin_mask_from_handle(pins[i]));
        }
    }
    return SPI_OK;
}

/* ---------- 软件位时序层 ---------- */

spi_status_t spi_port_set_sck(const spi_hw_bus_t *hw, uint8_t state)
{
    if (hw == NULL) return SPI_ERR_PARAM;
    uint32_t h = hw->hw_handle;
    if (h & SPI_BUS_TYPE_HARD) return SPI_ERR_UNSUPP;

    uint16_t sck_h = (uint16_t)((h >> 24) & 0xFFFFu);
    GPIO_TypeDef *port = spi_stm32_port_from_handle(sck_h);
    if (port == NULL) return SPI_ERR_HW;
    HAL_GPIO_WritePin(port, spi_stm32_pin_mask_from_handle(sck_h),
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SPI_OK;
}

spi_status_t spi_port_set_mosi(const spi_hw_bus_t *hw, uint8_t state)
{
    if (hw == NULL) return SPI_ERR_PARAM;
    uint32_t h = hw->hw_handle;
    if (h & SPI_BUS_TYPE_HARD) return SPI_ERR_UNSUPP;

    uint16_t mosi_h = (uint16_t)((h >> 16) & 0xFFFFu);
    GPIO_TypeDef *port = spi_stm32_port_from_handle(mosi_h);
    if (port == NULL) return SPI_ERR_HW;
    HAL_GPIO_WritePin(port, spi_stm32_pin_mask_from_handle(mosi_h),
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SPI_OK;
}

uint8_t spi_port_read_miso(const spi_hw_bus_t *hw)
{
    if (hw == NULL) return 1;
    uint32_t h = hw->hw_handle;
    if (h & SPI_BUS_TYPE_HARD) return 1;

    uint16_t miso_h = (uint16_t)((h >> 8) & 0xFFFFu);
    GPIO_TypeDef *port = spi_stm32_port_from_handle(miso_h);
    if (port == NULL) return 1;
    return (HAL_GPIO_ReadPin(port, spi_stm32_pin_mask_from_handle(miso_h))
            == GPIO_PIN_SET) ? 1 : 0;
}

spi_status_t spi_port_set_cs(const spi_hw_bus_t *hw, uint8_t active,
                              spi_cs_polarity_t cs_polarity)
{
    if (hw == NULL) return SPI_ERR_PARAM;
    uint32_t h = hw->hw_handle;

    /* CS 引脚位置: 硬件 SPI 在 bit[15:8], 软件 SPI 在 bit[7:0] */
    uint16_t cs_h = (h & SPI_BUS_TYPE_HARD) ?
                    (uint16_t)((h >> 8) & 0xFFFFu) :
                    (uint16_t)(h & 0xFFFFu);

    GPIO_TypeDef *port = spi_stm32_port_from_handle(cs_h);
    if (port == NULL) return SPI_ERR_HW;

    /* active=1 选中: 低有效时拉低, 高有效时拉高 */
    GPIO_PinState pin_state;
    if (cs_polarity == SPI_CS_ACTIVE_LOW) {
        pin_state = active ? GPIO_PIN_RESET : GPIO_PIN_SET;
    } else {
        pin_state = active ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }
    HAL_GPIO_WritePin(port, spi_stm32_pin_mask_from_handle(cs_h), pin_state);
    return SPI_OK;
}

spi_status_t spi_port_delay_us(uint32_t us)
{
    uint32_t cycles = us * spi_stm32_cycles_per_us();
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
    return SPI_OK;
}

/* ---------- 硬件 SPI 原子传输 ---------- */

spi_status_t spi_port_master_xfer(const spi_hw_bus_t *hw,
                                  const uint8_t *tx_buf, uint8_t *rx_buf,
                                  uint16_t len, uint32_t timeout_ms,
                                  uint8_t cs_control)
{
    if (hw == NULL) return SPI_ERR_PARAM;
    uint32_t h = hw->hw_handle;
    if ((h & SPI_BUS_TYPE_HARD) == 0) {
        return SPI_ERR_UNSUPP;   /* 软件总线走通用层 */
    }

    SPI_HandleTypeDef *hspi = spi_stm32_lookup_spi(h & 0x7Fu);
    if (hspi == NULL) return SPI_ERR_HW;

    uint32_t tout = (timeout_ms == 0) ? HAL_MAX_DELAY : timeout_ms;

    /* CS 控制 */
    if (cs_control) {
        uint16_t cs_h = (uint16_t)((h >> 8) & 0xFFFFu);
        GPIO_TypeDef *cs_port = spi_stm32_port_from_handle(cs_h);
        if (cs_port) {
            HAL_GPIO_WritePin(cs_port, spi_stm32_pin_mask_from_handle(cs_h),
                              GPIO_PIN_RESET);
        }
    }

    HAL_StatusTypeDef st;

    if (tx_buf != NULL && rx_buf != NULL) {
        /* 全双工 */
        st = HAL_SPI_TransmitReceive(hspi, (uint8_t *)tx_buf, rx_buf, len, tout);
    } else if (tx_buf != NULL) {
        /* 仅写 */
        st = HAL_SPI_Transmit(hspi, (uint8_t *)tx_buf, len, tout);
    } else if (rx_buf != NULL) {
        /* 仅读: 发送 0xFF 哑字节同时接收 */
        /* STM32 SPI 是全双工硬件, 读时必须同时写 */
        /* 用 HAL_SPI_Receive 内部会自动发 0xFFFF... */
        st = HAL_SPI_Receive(hspi, rx_buf, len, tout);
    } else {
        st = HAL_OK;   /* len=0 无操作 */
    }

    /* CS 释放 */
    if (cs_control) {
        uint16_t cs_h = (uint16_t)((h >> 8) & 0xFFFFu);
        GPIO_TypeDef *cs_port = spi_stm32_port_from_handle(cs_h);
        if (cs_port) {
            HAL_GPIO_WritePin(cs_port, spi_stm32_pin_mask_from_handle(cs_h),
                              GPIO_PIN_SET);
        }
    }

    return (st == HAL_OK) ? SPI_OK : SPI_ERR_HW;
}
