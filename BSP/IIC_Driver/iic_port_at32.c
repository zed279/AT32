/**
 * @file    iic_port_at32.c
 * @brief   可复用 IIC 驱动模板 - Artery AT32F403A/407 移植实现（软件 IIC）
 *
 * @note    覆盖 iic_port.c 的弱定义桩，只实现软件位时序路径。
 *          通用层 iic.c 不解析句柄，本文件负责把 hw_handle 解成 GPIO 端口 + 引脚。
 *
 *          句柄编码（与 gpio_port_at32.c 完全一致）：
 *            bit[15:8] = 端口索引 0=A 1=B 2=C 3=D 4=E
 *            bit[7:0]  = 引脚号 0~15
 *
 *          与 STM32 版的差异：
 *            - 配置用 gpio_init() + gpio_init_type（字段是复数 gpio_pins）
 *            - 电平用 gpio_bits_set/reset（BSRE/BCRE 原子操作，不用读改写）
 *            - 读取用 gpio_input_data_bit_read()，返回 flag（SET/RESET）
 *            - 开漏是独立字段 gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN，
 *              不像 STM32 那样揉进 MODE 枚举里
 *
 *          微秒延时用 Cortex-M4 内置 DWT 周期计数器，不碰 AT32 BSP 的
 *          delay_us()（那套用 SysTick 做一次性倒计时，与 BM_HAL 的 TMR2
 *          时基虽不冲突，但 IIC 要的是亚微秒精度，DWT 更合适）。
 */
#include "iic_port.h"
#include "at32f403a_407.h"

/* ============================ 内部: 句柄解码 ============================ */

/**
 * @brief 从 16 位引脚句柄解出 GPIO 端口
 * @note  AT32F403A/407 只有 GPIOA~GPIOE
 */
static gpio_type *iic_port_from_handle(uint16_t h)
{
    switch ((h >> 8) & 0xFF) {
        case 0: return GPIOA;
        case 1: return GPIOB;
        case 2: return GPIOC;
        case 3: return GPIOD;
        case 4: return GPIOE;
        default: return NULL;
    }
}

/**
 * @brief 从 16 位引脚句柄解出引脚位掩码（1 << pin）
 */
static uint16_t iic_pin_mask_from_handle(uint16_t h)
{
    return (uint16_t)(1u << (h & 0xFF));
}

/**
 * @brief 按端口使能 GPIO 时钟
 */
static void iic_enable_gpio_clk(gpio_type *port)
{
         if (port == GPIOA) crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    else if (port == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    else if (port == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    else if (port == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    else if (port == GPIOE) crm_periph_clock_enable(CRM_GPIOE_PERIPH_CLOCK, TRUE);
}

/* ============================ 内部: 微秒延时（DWT） ============================ */

/**
 * @brief DWT 周期计数器初始化
 * @note  上电后 DWT 默认关闭，必须先使能 TRCENA 才能写 CYCCNTENA。
 *        重复调用无害（幂等），iic_init 每条总线都会进来一次。
 */
static void iic_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
 * @brief 一个微秒对应多少个内核周期
 * @note  AT32 SDK 用 system_core_clock（HCLK），不是 CMSIS 的 SystemCoreClock
 */
static uint32_t iic_cycles_per_us(void)
{
    uint32_t mhz = system_core_clock / 1000000u;
    return (mhz == 0u) ? 1u : mhz;
}

/* ============================ 内部: 引脚模式配置 ============================ */

/**
 * @brief 把一个引脚配成开漏输出 + 内部上拉 + 高电平（释放总线）
 */
static void iic_pin_config_od(gpio_type *port, uint16_t mask)
{
    gpio_init_type gi;

    gpio_default_para_init(&gi);
    gi.gpio_pins           = mask;
    gi.gpio_mode           = GPIO_MODE_OUTPUT;
    gi.gpio_out_type       = GPIO_OUTPUT_OPEN_DRAIN;
    gi.gpio_pull           = GPIO_PULL_UP;
    gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(port, &gi);

    gpio_bits_set(port, mask);      /* 开漏写 1 = 释放，由上拉拉高 */
}

/**
 * @brief 把一个引脚配成输入 + 内部上拉（用于读从机应答）
 */
static void iic_pin_config_in(gpio_type *port, uint16_t mask)
{
    gpio_init_type gi;

    gpio_default_para_init(&gi);
    gi.gpio_pins           = mask;
    gi.gpio_mode           = GPIO_MODE_INPUT;
    gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gi.gpio_pull           = GPIO_PULL_UP;
    gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(port, &gi);
}

/* ============================ port 函数实现 ============================ */

iic_status_t iic_port_init(const iic_hw_bus_t *hw, const iic_config_t *cfg)
{
    uint32_t      h;
    uint16_t      scl_h;
    uint16_t      sda_h;
    gpio_type    *scl_port;
    gpio_type    *sda_port;
    uint16_t      scl_mask;
    uint16_t      sda_mask;

    if (hw == NULL || cfg == NULL) {
        return IIC_ERR_PARAM;
    }

    iic_dwt_init();     /* 延时基准，幂等 */

    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return IIC_ERR_UNSUPP;      /* 本工程不配硬件 I2C，通知通用层走软件位时序 */
    }

    scl_h   = (uint16_t)((h >> 16) & 0xFFFFu);
    sda_h   = (uint16_t)( h        & 0xFFFFu);
    scl_port = iic_port_from_handle(scl_h);
    sda_port = iic_port_from_handle(sda_h);
    if (scl_port == NULL || sda_port == NULL) {
        return IIC_ERR_HW;
    }
    scl_mask = iic_pin_mask_from_handle(scl_h);
    sda_mask = iic_pin_mask_from_handle(sda_h);

    iic_enable_gpio_clk(scl_port);
    iic_enable_gpio_clk(sda_port);

    /* 上电即释放总线：两颗芯片的 SCL/SDA 都是开漏 + 内部 48k 上拉 */
    iic_pin_config_od(scl_port, scl_mask);
    iic_pin_config_od(sda_port, sda_mask);

    return IIC_OK;
}

iic_status_t iic_port_deinit(const iic_hw_bus_t *hw)
{
    uint32_t   h;
    uint16_t   scl_h;
    uint16_t   sda_h;
    gpio_type *scl_port;
    gpio_type *sda_port;

    if (hw == NULL) {
        return IIC_ERR_PARAM;
    }

    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return IIC_ERR_UNSUPP;
    }

    /* 释放引脚：配成模拟输入（最低功耗，且不会对外驱动任何电平） */
    scl_h    = (uint16_t)((h >> 16) & 0xFFFFu);
    sda_h    = (uint16_t)( h        & 0xFFFFu);
    scl_port = iic_port_from_handle(scl_h);
    sda_port = iic_port_from_handle(sda_h);

    if (scl_port != NULL) {
        gpio_init_type gi;
        gpio_default_para_init(&gi);
        gi.gpio_pins = iic_pin_mask_from_handle(scl_h);
        gi.gpio_mode = GPIO_MODE_ANALOG;
        gi.gpio_pull = GPIO_PULL_NONE;
        gpio_init(scl_port, &gi);
    }
    if (sda_port != NULL) {
        gpio_init_type gi;
        gpio_default_para_init(&gi);
        gi.gpio_pins = iic_pin_mask_from_handle(sda_h);
        gi.gpio_mode = GPIO_MODE_ANALOG;
        gi.gpio_pull = GPIO_PULL_NONE;
        gpio_init(sda_port, &gi);
    }
    return IIC_OK;
}

/* ---------------------------- 软件位时序层 ---------------------------- */

iic_status_t iic_port_set_sda_dir(const iic_hw_bus_t *hw, uint8_t is_output)
{
    uint32_t   h;
    uint16_t   sda_h;
    gpio_type *port;
    uint16_t   mask;

    if (hw == NULL) {
        return IIC_ERR_PARAM;
    }
    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return IIC_ERR_UNSUPP;
    }

    sda_h = (uint16_t)(h & 0xFFFFu);
    port  = iic_port_from_handle(sda_h);
    mask  = iic_pin_mask_from_handle(sda_h);
    if (port == NULL) {
        return IIC_ERR_HW;
    }

    /* AT32 的输出寄存器（ODT）在切输入时不会被清掉，切回输出会把上次的电平
     * 带出来。调用方（iic.c）在切回输出后总会显式写一次电平，所以这里不用管。 */
    if (is_output != 0u) {
        iic_pin_config_od(port, mask);
    } else {
        iic_pin_config_in(port, mask);
    }
    return IIC_OK;
}

iic_status_t iic_port_set_sda(const iic_hw_bus_t *hw, uint8_t state)
{
    uint32_t   h;
    uint16_t   sda_h;
    gpio_type *port;
    uint16_t   mask;

    if (hw == NULL) {
        return IIC_ERR_PARAM;
    }
    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return IIC_ERR_UNSUPP;
    }

    sda_h = (uint16_t)(h & 0xFFFFu);
    port  = iic_port_from_handle(sda_h);
    mask  = iic_pin_mask_from_handle(sda_h);
    if (port == NULL) {
        return IIC_ERR_HW;
    }

    if (state != 0u) {
        gpio_bits_set(port, mask);
    } else {
        gpio_bits_reset(port, mask);
    }
    return IIC_OK;
}

iic_status_t iic_port_set_scl(const iic_hw_bus_t *hw, uint8_t state)
{
    uint32_t   h;
    uint16_t   scl_h;
    gpio_type *port;
    uint16_t   mask;

    if (hw == NULL) {
        return IIC_ERR_PARAM;
    }
    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return IIC_ERR_UNSUPP;
    }

    scl_h = (uint16_t)((h >> 16) & 0xFFFFu);
    port  = iic_port_from_handle(scl_h);
    mask  = iic_pin_mask_from_handle(scl_h);
    if (port == NULL) {
        return IIC_ERR_HW;
    }

    if (state != 0u) {
        gpio_bits_set(port, mask);
    } else {
        gpio_bits_reset(port, mask);
    }
    return IIC_OK;
}

uint8_t iic_port_read_sda(const iic_hw_bus_t *hw)
{
    uint32_t   h;
    uint16_t   sda_h;
    gpio_type *port;
    uint16_t   mask;

    if (hw == NULL) {
        return 1;
    }
    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return 1;
    }

    sda_h = (uint16_t)(h & 0xFFFFu);
    port  = iic_port_from_handle(sda_h);
    mask  = iic_pin_mask_from_handle(sda_h);
    if (port == NULL) {
        return 1;
    }
    return (gpio_input_data_bit_read(port, mask) != RESET) ? 1u : 0u;
}

uint8_t iic_port_read_scl(const iic_hw_bus_t *hw)
{
    uint32_t   h;
    uint16_t   scl_h;
    gpio_type *port;
    uint16_t   mask;

    if (hw == NULL) {
        return 1;
    }
    h = hw->hw_handle;
    if ((h & IIC_BUS_TYPE_HARD) != 0u) {
        return 1;
    }

    scl_h = (uint16_t)((h >> 16) & 0xFFFFu);
    port  = iic_port_from_handle(scl_h);
    mask  = iic_pin_mask_from_handle(scl_h);
    if (port == NULL) {
        return 1;
    }
    return (gpio_input_data_bit_read(port, mask) != RESET) ? 1u : 0u;
}

iic_status_t iic_port_delay_us(uint32_t us)
{
    uint32_t cycles;
    uint32_t start;

    /* 防御：DWT 没使能时 CYCCNT 恒为 0，下面的 while 会永久卡死。
     * 正常路径 iic_port_init() 已经初始化过 DWT，这里只是兜住
     * "有人在 init 之前就调了位时序"的错误用法（软件 IIC 死循环最难查）。 */
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0u) {
        iic_dwt_init();
    }

    /* 差值比较自动处理 CYCCNT 32 位回绕 */
    cycles = us * iic_cycles_per_us();
    start  = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
    return IIC_OK;
}

/* 硬件 I2C 原子传输：保留 iic_port.c 的弱定义桩（返回 UNSUPP），
 * 通用层 iic_soft_master_xfer() 会自动接管。 */
