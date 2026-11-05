/**
 * @file    iic_port_stm32_hal.c
 * @brief   可复用 IIC 驱动模板 - STM32F103C6 HAL 库移植实现 (软件 IIC)
 * @note    本文件覆盖 iic_port.c 中的弱定义桩, 仅实现软件位时序路径
 *          (F103C6 工程未配置硬件 I2C, OLED 走软件 IIC 即可)。
 *
 *          通过 hw_handle 的最高位区分软件/硬件:
 *            bit31 = 0 : 软件 IIC, hw_handle = (scl_handle<<16) | sda_handle
 *            bit31 = 1 : 硬件 I2C (本工程未使用, 返回 UNSUPP)
 *
 *          引脚句柄编码 (与 GPIO 驱动一致):
 *            bit[15:8] = 端口 (GPIOA=0,...)
 *            bit[7:0]  = 引脚号 (0~15)
 *
 *          STM32F103C6 LQFP48 可用端口: GPIOA/B/C (C 只有 PC13~15), GPIOD (PD0/1=OSC).
 *          本工程 OLED 接 PA6(SCL) + PA7(SDA).
 *
 *          微秒延时: 用 DWT 周期计数器 (Cortex-M3 内置).
 *          上电后 DWT 默认关闭, 在 iic_port_init 内自动使能.
 *
 *          若工程加入硬件 I2C, 可在本文件追加 iic_port_master_xfer() 实现
 *          (调用 HAL_I2C_Mem_Read/Write).
 */
#include "iic_port.h"
#include "stm32f1xx_hal.h"   /* F1 系列 HAL 头 */

/* ============================ 内部: 引脚句柄解码 ============================ */

/**
 * @brief  从 16 位引脚句柄解出 GPIO 端口
 * @note   句柄编码: bit[15:8]=端口索引, bit[7:0]=引脚号
 *         STM32F103C6 LQFP48 仅 GPIOA~D 有效
 */
static GPIO_TypeDef *iic_stm32_port_from_handle(uint16_t h)
{
    switch ((h >> 8) & 0xFF) {
        case 0: return GPIOA;
        case 1: return GPIOB;
        case 2: return GPIOC;
        case 3: return GPIOD;
        default: return NULL;   /* F103C6 无 GPIOE/F/G/H */
    }
}

/**
 * @brief  从 16 位引脚句柄解出引脚位掩码 (1<<pin)
 */
static uint16_t iic_stm32_pin_mask_from_handle(uint16_t h)
{
    return (uint16_t)(1u << (h & 0xFF));
}

/**
 * @brief  使能 GPIO 端口时钟 (F1 的 GPIO 都挂 APB2)
 */
static void iic_stm32_enable_gpio_clk(GPIO_TypeDef *port)
{
         if (port == GPIOA) __HAL_RCC_GPIOA_CLK_ENABLE();
    else if (port == GPIOB) __HAL_RCC_GPIOB_CLK_ENABLE();
    else if (port == GPIOC) __HAL_RCC_GPIOC_CLK_ENABLE();
    else if (port == GPIOD) __HAL_RCC_GPIOD_CLK_ENABLE();
}

/* ============================ 内部: 微秒延时 (DWT) ============================ */

/**
 * @brief  DWT 周期计数器初始化 (在 iic_init 时调用一次)
 * @note   DWT (Data Watchpoint and Trace) 是 Cortex-M3/M4/M7 内置,
 *         提供精确到时钟周期的 CYCCNT 计数. 上电后默认关闭, 需手动使能.
 */
static void iic_stm32_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* TRCENA */
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;             /* CYCCNTENA */
}

static inline uint32_t iic_stm32_cycles_per_us(void)
{
    /* SystemCoreClock 单位是 Hz, 一微秒 = SystemCoreClock / 1000000 个周期 */
    return SystemCoreClock / 1000000u;
}

/* ============================ port 函数实现 ============================ */

iic_status_t iic_port_init(const iic_hw_bus_t *hw, const iic_config_t *cfg)
{
    if (hw == NULL || cfg == NULL) {
        return IIC_ERR_PARAM;
    }

    iic_stm32_dwt_init();   /* 一次初始化 DWT 用于延时 */

    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) {
        /* 本工程未配置硬件 I2C, 通知通用层走软件位时序 */
        return IIC_ERR_UNSUPP;
    }

    /* ---------- 软件位时序 ---------- */
    uint16_t scl_h = (uint16_t)((h >> 16) & 0xFFFFu);
    uint16_t sda_h = (uint16_t)( h        & 0xFFFFu);
    GPIO_TypeDef *scl_port = iic_stm32_port_from_handle(scl_h);
    GPIO_TypeDef *sda_port = iic_stm32_port_from_handle(sda_h);
    uint16_t      scl_mask = iic_stm32_pin_mask_from_handle(scl_h);
    uint16_t      sda_mask = iic_stm32_pin_mask_from_handle(sda_h);
    if (scl_port == NULL || sda_port == NULL) {
        return IIC_ERR_HW;
    }

    iic_stm32_enable_gpio_clk(scl_port);
    iic_stm32_enable_gpio_clk(sda_port);

    /* 配置 SCL/SDA 为开漏输出 + 上拉 + 初始高 (释放总线) */
    GPIO_InitTypeDef gi = {0};
    gi.Mode  = GPIO_MODE_OUTPUT_OD;
    gi.Pull  = GPIO_PULLUP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;

    gi.Pin = scl_mask;  HAL_GPIO_Init(scl_port, &gi);
    gi.Pin = sda_mask; HAL_GPIO_Init(sda_port, &gi);

    HAL_GPIO_WritePin(scl_port, scl_mask, GPIO_PIN_SET);
    HAL_GPIO_WritePin(sda_port, sda_mask, GPIO_PIN_SET);
    return IIC_OK;
}

iic_status_t iic_port_deinit(const iic_hw_bus_t *hw)
{
    if (hw == NULL) return IIC_ERR_PARAM;

    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) {
        return IIC_ERR_UNSUPP;
    }

    /* 软件: 把 SCL/SDA 复位为模拟模式 (省电) */
    uint16_t scl_h = (uint16_t)((h >> 16) & 0xFFFFu);
    uint16_t sda_h = (uint16_t)( h        & 0xFFFFu);
    GPIO_TypeDef *scl_port = iic_stm32_port_from_handle(scl_h);
    GPIO_TypeDef *sda_port = iic_stm32_port_from_handle(sda_h);
    uint16_t      scl_mask = iic_stm32_pin_mask_from_handle(scl_h);
    uint16_t      sda_mask = iic_stm32_pin_mask_from_handle(sda_h);
    if (scl_port) { HAL_GPIO_DeInit(scl_port, scl_mask); }
    if (sda_port) { HAL_GPIO_DeInit(sda_port, sda_mask); }
    return IIC_OK;
}

/* ---------- 软件位时序层 ---------- */

iic_status_t iic_port_set_sda_dir(const iic_hw_bus_t *hw, uint8_t is_output)
{
    if (hw == NULL) return IIC_ERR_PARAM;
    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) {
        return IIC_ERR_UNSUPP;
    }
    uint16_t sda_h = (uint16_t)(h & 0xFFFFu);
    GPIO_TypeDef *port = iic_stm32_port_from_handle(sda_h);
    uint16_t      mask = iic_stm32_pin_mask_from_handle(sda_h);
    if (port == NULL) return IIC_ERR_HW;

    /* 开漏输出: 输入=模拟模式, 输出=开漏.
     * F1 的 GPIO_MODE_OUTPUT_OD 同时支持读电平 (开漏模式下读 IDR 可读输入),
     * 因此实际上方向切换可省略. 这里仅为接口一致性, 把 SDA 重配为输入/开漏. */
    GPIO_InitTypeDef gi = {0};
    gi.Pin  = mask;
    gi.Pull = GPIO_PULLUP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Mode = is_output ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_INPUT;
    HAL_GPIO_Init(port, &gi);
    return IIC_OK;
}

iic_status_t iic_port_set_sda(const iic_hw_bus_t *hw, uint8_t state)
{
    if (hw == NULL) return IIC_ERR_PARAM;
    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) return IIC_ERR_UNSUPP;

    uint16_t sda_h = (uint16_t)(h & 0xFFFFu);
    GPIO_TypeDef *port = iic_stm32_port_from_handle(sda_h);
    uint16_t      mask = iic_stm32_pin_mask_from_handle(sda_h);
    if (port == NULL) return IIC_ERR_HW;
    HAL_GPIO_WritePin(port, mask, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return IIC_OK;
}

iic_status_t iic_port_set_scl(const iic_hw_bus_t *hw, uint8_t state)
{
    if (hw == NULL) return IIC_ERR_PARAM;
    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) return IIC_ERR_UNSUPP;

    uint16_t scl_h = (uint16_t)((h >> 16) & 0xFFFFu);
    GPIO_TypeDef *port = iic_stm32_port_from_handle(scl_h);
    uint16_t      mask = iic_stm32_pin_mask_from_handle(scl_h);
    if (port == NULL) return IIC_ERR_HW;
    HAL_GPIO_WritePin(port, mask, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return IIC_OK;
}

uint8_t iic_port_read_sda(const iic_hw_bus_t *hw)
{
    if (hw == NULL) return 1;
    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) return 1;

    uint16_t sda_h = (uint16_t)(h & 0xFFFFu);
    GPIO_TypeDef *port = iic_stm32_port_from_handle(sda_h);
    uint16_t      mask = iic_stm32_pin_mask_from_handle(sda_h);
    if (port == NULL) return 1;
    return (HAL_GPIO_ReadPin(port, mask) == GPIO_PIN_SET) ? 1 : 0;
}

uint8_t iic_port_read_scl(const iic_hw_bus_t *hw)
{
    if (hw == NULL) return 1;
    uint32_t h = hw->hw_handle;
    if (h & IIC_BUS_TYPE_HARD) return 1;

    uint16_t scl_h = (uint16_t)((h >> 16) & 0xFFFFu);
    GPIO_TypeDef *port = iic_stm32_port_from_handle(scl_h);
    uint16_t      mask = iic_stm32_pin_mask_from_handle(scl_h);
    if (port == NULL) return 1;
    return (HAL_GPIO_ReadPin(port, mask) == GPIO_PIN_SET) ? 1 : 0;
}

iic_status_t iic_port_delay_us(uint32_t us)
{
    /* DWT 微秒延时, 精确到时钟周期.
     * 差值比较 (DWT->CYCCNT - start) 自动处理 CYCCNT 回绕 (32 位) */
    uint32_t cycles = us * iic_stm32_cycles_per_us();
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
    return IIC_OK;
}

/* 硬件 I2C 原子传输: 保持弱定义桩的默认实现 (返回 UNSUPP),
 * 通用层会自动回退到软件位时序. 如需硬件 I2C, 在此实现并去掉 weak 标记. */
