/**
 * @file    gpio_cfg.h
 * @brief   GPIO 驱动模板 - 本工程配置 (ST7789 240x320 LCD / AT32F403AVGT7)
 *
 * @note    本工程引脚分配 (SPI 4 线: SCK/MOSI/CS/DC + RESET + BL):
 *            PA3  -> TFT_LED   (背光, 推挽输出, 初始灭)
 *            PA4  -> TFT_CS    (片选, 由 SPI 驱动在传输时翻转)
 *            PA6  -> TFT_A0/DC (0=命令 1=数据)
 *            PB0  -> TFT_RESET (低有效复位, 初始高)
 *
 *          SCK(PA5) / MOSI(PA7) 不在这里 —— 它们是 SPI1 的复用功能,
 *          由 BSP/SPI_Driver 在 spi_port_init() 中配成 GPIO_MODE_MUX,
 *          并且这两根脚的映射登记在 spi_cfg.h 的 g_spi_hw_pins[] 里.
 *
 *          AT32 与 STM32 的差异: 端口枚举只需 A~E (EXINT 只支持 A~E).
 *          本工程只用 A/B 两个端口.
 */
#ifndef GPIO_CFG_H_
#define GPIO_CFG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 容量配置 ============================ */
#ifndef GPIO_MAX_INTERRUPT_SLOTS
#  define GPIO_MAX_INTERRUPT_SLOTS   16   /* 同时支持的最大中断引脚数 */
#endif

/* ============================ 逻辑引脚号 (工程别名) ============================ */
/* 用户可自由编号, 不要冲突即可. 推荐 0~511 之间 */
#define GPIO_PIN_LCD_RESET    0    /* PB0  复位, 低有效 */
#define GPIO_PIN_LCD_DC       1    /* PA6  数据/命令选择 (A0) */
#define GPIO_PIN_LCD_BL       2    /* PA3  背光 */
#define GPIO_PIN_KEY          3    /* PB9  用户按键, 上拉输入, 低有效 */

/* ============================ 硬件句柄编码宏 ============================ */
/**
 * @brief  将 (端口, 引脚) 编码为 32 位硬件句柄
 * @note   采用 "端口索引 << 8 | 引脚号" 的编码方案.
 *         AT32F403A 只有 GPIOA~GPIOE 五个端口 (EXINT 也只支持 A~E).
 */
#define GPIO_PORT_A   0
#define GPIO_PORT_B   1
#define GPIO_PORT_C   2
#define GPIO_PORT_D   3
#define GPIO_PORT_E   4

#ifndef HW_HANDLE
#  define HW_HANDLE(port, pin)   (((uint32_t)(port) << 8) | (uint32_t)(pin))
#endif

/* 表被多个 .c 包含时, ARMCC 会为未引用者报 #177-D, 用该属性静默 */
#ifndef GPIO_CFG_UNUSED
#  if defined(__CC_ARM) || defined(__GNUC__) || defined(__clang__)
#    define GPIO_CFG_UNUSED   __attribute__((unused))
#  else
#    define GPIO_CFG_UNUSED
#  endif
#endif

/* ============================ 引脚映射表 ============================ */
/**
 * @brief 硬件引脚描述结构体
 * @note  此类型在 gpio_port.h 中作为前向引用, 必须在此定义完整结构.
 *        gpio_port.h 不再重复 typedef (避免 typedef 重定义错误).
 */
typedef struct {
    uint16_t  user_pin;   /* 逻辑引脚号 */
    uint32_t  hw_handle;  /* 硬件句柄 (由 HW_HANDLE 编码) */
} gpio_hw_pin_t;

/**
 * @brief 逻辑引脚号 -> 硬件句柄 的映射表
 *
 * 对应板子接线:
 *   - LCD_RESET: PB0  (推挽输出, 初始高 -> 不复位)
 *   - LCD_DC:    PA6  (推挽输出, 初始高 -> 数据态)
 *   - LCD_BL:    PA3  (推挽输出, 初始灭, 由 st7789 驱动点亮)
 *   - KEY:       PB9  (上拉输入, 按下为低; 外部只需按键到 GND, 不需要上拉电阻)
 *
 * @warning 映射表漏条目会导致 gpio_init() 返回 GPIO_ERR_PIN 且引脚无动作,
 *          新增引脚时必须同时在此表登记.
 */
static const gpio_hw_pin_t g_gpio_pin_map[] GPIO_CFG_UNUSED = {
    { GPIO_PIN_LCD_RESET, HW_HANDLE(GPIO_PORT_B, 0) },   /* PB0  */
    { GPIO_PIN_LCD_DC,    HW_HANDLE(GPIO_PORT_A, 6) },   /* PA6  */
    { GPIO_PIN_LCD_BL,    HW_HANDLE(GPIO_PORT_A, 3) },   /* PA3  */
    { GPIO_PIN_KEY,       HW_HANDLE(GPIO_PORT_B, 9) },   /* PB9  */
};

#define GPIO_PIN_TABLE_SIZE   (sizeof(g_gpio_pin_map) / sizeof(g_gpio_pin_map[0]))

#ifdef __cplusplus
}
#endif
#endif /* GPIO_CFG_H_ */
