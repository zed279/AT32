/**
 * @file    spi_cfg.h
 * @brief   可复用 SPI 驱动 - 本工程配置 (ST7789 LCD / AT32F403ACGT7)
 *
 * @note    本工程只有一条 SPI 总线, 接 ST7789 240x320 TFT (只写, 无 MISO):
 *
 *            SPI1 (硬件 SPI, Master, 单线半双工发送, Mode 0, 8bit, MSB First)
 *              PA5 -----> SPI1_SCK
 *              PA7 -----> SPI1_MOSI/SDA
 *              PA4 -----> CS   (普通 GPIO, 软件控制, 由本驱动在传输时翻转)
 *              DMA1 通道 3 -> SPI1_TX (见 AT32F403A 手册 表9-3)
 *
 *          PA5/PA7 是 SPI1 的默认复用功能 (不做重映射), 在
 *          spi_port_at32.c 的 spi_port_init() 里用 GPIO_MODE_MUX 配置.
 *          PA4/PA6/PB0 属于 GPIO 域, 见 gpio_cfg.h.
 *
 *          SPI1 挂 APB2 = 120MHz (本工程 240MHz 主频 / APB2 分频 2):
 *            SPI_SPEED_LOW       -> /64 = 1.875MHz  (上电保守)
 *            SPI_SPEED_STANDARD  -> /32 = 3.75MHz
 *            SPI_SPEED_FAST      -> /16 = 7.5MHz
 *            SPI_SPEED_VERY_FAST -> /8  = 15MHz     (本工程默认, ST7789 规格内)
 *            SPI_SPEED_ULTRA     -> /4  = 30MHz     (超 ST7789 66ns 规格, 花屏就降档)
 */
#ifndef SPI_CFG_H_
#define SPI_CFG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 容量配置 ============================ */
#ifndef SPI_MAX_BUSES
#  define SPI_MAX_BUSES        1    /* 本工程只用 1 条总线 (SPI1) */
#endif

/* 单次传输长度 >= 该阈值走 DMA, 小于则轮询 (短命令用 DMA 反而慢) */
#ifndef SPI_DMA_MIN_LEN
#  define SPI_DMA_MIN_LEN      16
#endif

/* 软件 SPI 默认半周期延时 (us) —— 本工程走硬件 SPI, 不使用这些参数 */
#ifndef SPI_SOFT_HALF_PERIOD_US
#  define SPI_SOFT_HALF_PERIOD_US   1
#endif
#ifndef SPI_SOFT_CS_SETUP_US
#  define SPI_SOFT_CS_SETUP_US      1
#endif
#ifndef SPI_SOFT_CS_HOLD_US
#  define SPI_SOFT_CS_HOLD_US       1
#endif

/* ============================ 逻辑总线号 (工程别名) ============================ */
#define SPI_BUS_LCD            0    /* ST7789 LCD, 硬件 SPI1, CS=PA4 */

/* ============================ 硬件句柄编码宏 ============================ */
/**
 * @brief 硬件句柄编码方案
 * @note   bit31 = bus_type: 0=软件位时序, 1=硬件 SPI
 *
 *         软件: bit[30:24]=SCK 引脚句柄, bit[23:16]=MOSI, bit[15:8]=MISO, bit[7:0]=CS
 *         硬件: bit[7:0]  = SPI 实例索引 (0=SPI1, 1=SPI2, 2=SPI3)
 *               bit[23:8] = CS 引脚句柄 (硬件 SPI 仍需软件控制 CS)
 *
 *         引脚句柄编码与 GPIO 驱动一致: port<<8 | pin
 */
#define SPI_BUS_TYPE_SOFT   0x00000000u
#define SPI_BUS_TYPE_HARD   0x80000000u

/* 软件 SPI 句柄编码 (4 个引脚, 用与 GPIO 驱动一致的编码) */
#define HW_SPI_SOFT(sck_hw, mosi_hw, miso_hw, cs_hw)   \
        (SPI_BUS_TYPE_SOFT | ((uint32_t)(sck_hw)  << 24) \
                           | ((uint32_t)(mosi_hw) << 16) \
                           | ((uint32_t)(miso_hw)  << 8)  \
                           | ((uint32_t)(cs_hw)          & 0xFFu))

/* 硬件 SPI 句柄编码 (instance_index: 0=SPI1, 1=SPI2, 2=SPI3; cs_hw: CS 引脚句柄) */
#define HW_SPI_HARD(instance_index, cs_hw)   \
        (SPI_BUS_TYPE_HARD | ((uint32_t)(cs_hw) << 8) | (uint32_t)(instance_index))

/* ============================ 引脚/端口编码 (与 GPIO 驱动一致) ============================ */
#ifndef HW_HANDLE_GPIO
#  define HW_HANDLE_GPIO(port, pin)  (((uint32_t)(port) << 8) | (uint32_t)(pin))
#endif
/* 与 gpio_cfg.h 中的 GPIO_PORT_x 取值保持一致 (同值重定义是合法的) */
#ifndef GPIO_PORT_A
#  define GPIO_PORT_A 0
#endif
#ifndef GPIO_PORT_B
#  define GPIO_PORT_B 1
#endif

/* 表被多个 .c 包含时, ARMCC 会为未引用者报 #177-D, 用该属性静默 */
#ifndef SPI_CFG_UNUSED
#  if defined(__CC_ARM) || defined(__GNUC__) || defined(__clang__)
#    define SPI_CFG_UNUSED   __attribute__((unused))
#  else
#    define SPI_CFG_UNUSED
#  endif
#endif

/* ============================ 硬件引脚描述结构体 ============================ */
/**
 * @brief 逻辑总线 -> 硬件句柄 的映射条目
 * @note   此类型必须在 spi_cfg.h 定义 (映射表需要它),
 *         spi_port.h 仅前向引用以避免 typedef 重定义.
 */
typedef struct {
    uint16_t  bus_id;       /* 逻辑总线号 (与 spi_bus_t 一致) */
    uint32_t  hw_handle;    /* 硬件句柄 (由 HW_SPI_SOFT/HW_SPI_HARD 编码) */
} spi_hw_bus_t;

/**
 * @brief 硬件 SPI 实例 -> SCK/MOSI 引脚 的映射条目
 * @note  硬件 SPI 的 CS 走总线句柄 (软件控制), 但 SCK/MOSI 必须由 port 层
 *        自己配成复用功能 —— 没有 CubeMX 帮我们做 MspInit, 所以显式登记.
 */
typedef struct {
    uint8_t   instance;     /* SPI 实例索引 (0=SPI1, 1=SPI2, 2=SPI3) */
    uint32_t  sck_hw;       /* SCK  引脚句柄 */
    uint32_t  mosi_hw;      /* MOSI 引脚句柄 */
} spi_hw_pins_t;

/* ============================ 总线映射表 ============================ */
/**
 * @brief 逻辑总线号 -> 硬件句柄 的映射表
 *
 * @note  HW_SPI_HARD(0, ...) 的 0 = SPI1, 与 spi_port_at32.c 的
 *        spi_at32_instance() 一致.
 *        CS 引脚 = PA4 -> HW_HANDLE_GPIO(GPIO_PORT_A, 4) = 0x0004
 */
static const spi_hw_bus_t g_spi_bus_map[] SPI_CFG_UNUSED = {
    /* ST7789 LCD: 硬件 SPI1, CS=PA4 */
    { SPI_BUS_LCD, HW_SPI_HARD(0, HW_HANDLE_GPIO(GPIO_PORT_A, 4)) },
};

#define SPI_BUS_TABLE_SIZE   (sizeof(g_spi_bus_map) / sizeof(g_spi_bus_map[0]))

/* ============================ SCK/MOSI 引脚映射表 ============================ */
/**
 * @brief 硬件 SPI 实例 -> SCK/MOSI 引脚
 * @note  SPI1 默认映射 (不重映射): SCK=PA5, MISO=PA6, MOSI=PA7.
 *        本工程 MISO(PA6) 被借用成 LCD 的 DC/A0, 所以 SPI1 配成
 *        "单线半双工发送", 不占用 MISO.
 * @warning 漏登记本表 -> spi_port_init() 返回 SPI_ERR_HW, 屏幕点不亮.
 */
static const spi_hw_pins_t g_spi_hw_pins[] SPI_CFG_UNUSED = {
    { 0u, HW_HANDLE_GPIO(GPIO_PORT_A, 5), HW_HANDLE_GPIO(GPIO_PORT_A, 7) },
};

#define SPI_HW_PIN_TABLE_SIZE   (sizeof(g_spi_hw_pins) / sizeof(g_spi_hw_pins[0]))

#ifdef __cplusplus
}
#endif
#endif /* SPI_CFG_H_ */
