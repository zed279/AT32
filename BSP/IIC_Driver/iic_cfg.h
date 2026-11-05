/**
 * @file    iic_cfg.h
 * @brief   IIC 驱动模板 - 工程配置 (Artery AT32F403A + 双组 KT 接收芯片)
 *
 * @note    本工程配置两条独立的软件 IIC 总线，各挂一组 KT 接收芯片：
 *            IIC_BUS_KT_A  SCL=PB13  SDA=PB14   组1：KT0656M(主) + KT0650(从)
 *            IIC_BUS_KT_B  SCL=PA9   SDA=PA10   组2：KT0656M(主) + KT0650(从)
 *
 *          两组各自独立总线，所以两组可以用相同的器件地址（主 0x39 / 从 0x3D，
 *          ADDR 引脚均接高），不会撞地址。
 *
 *          port 层 iic_port_at32.c 根据 hw_handle 的 bus_type 位区分:
 *            bit 31 = 0 : 软件位时序, 低 30 位为 SCL/SDA 引脚句柄编码
 *            bit 31 = 1 : 硬件 I2C, 低 30 位为硬件实例索引 (本工程未使用)
 *
 *          引脚句柄编码与 gpio_port_at32.c 一致: (port << 8) | pin
 *
 * @note    原模板里的 IIC_BUS_OLED（PA6/PA7）已删除：本板 PA6 被 LCD 的
 *          TFT_A0/DC 占用、PA7 被 TFT_SDA/MOSI 占用，那条总线不存在了。
 *          留着映射表条目不会立刻出错（iic_init 只初始化传入的那条总线），
 *          但迟早有人误用，直接清掉更安全。
 */
#ifndef IIC_CFG_H_
#define IIC_CFG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 容量配置 ============================ */
#ifndef IIC_MAX_BUSES
#  define IIC_MAX_BUSES        4    /* 同时支持的最大总线数 */
#endif

/* 软件 IIC 默认时序半周期 (us), 100kbps 周期 10us, 半周期 5us */
#ifndef IIC_SOFT_HALF_PERIOD_US
#  define IIC_SOFT_HALF_PERIOD_US   5
#endif

/* 软件 IIC 总线空闲/复位最长等待 (us), 用于检测 SCL 钳低 */
#ifndef IIC_SOFT_SCL_TIMEOUT_US
#  define IIC_SOFT_SCL_TIMEOUT_US   1000
#endif

/* ============================ 逻辑总线号 (工程别名) ============================ */
#define IIC_BUS_KT_A           0    /* 组1 KT 芯片, 软件 IIC, PB13/PB14 */
#define IIC_BUS_KT_B           1    /* 组2 KT 芯片, 软件 IIC, PA9/PA10  */

/* ============================ 硬件句柄编码宏 ============================ */
/**
 * @brief  硬件句柄编码方案
 * @note   bit31 = bus_type: 0=软件位时序, 1=硬件 I2C
 *         软件: bit[30:16] = SCL 引脚句柄, bit[15:0] = SDA 引脚句柄
 *               (引脚句柄编码与 GPIO 驱动一致: port<<8 | pin)
 *         硬件: bit[7:0]  = I2C 实例索引 (0=I2C1, 1=I2C2, 2=I2C3, ...)
 */
#define IIC_BUS_TYPE_SOFT   0x00000000u
#define IIC_BUS_TYPE_HARD   0x80000000u

/* 软件 IIC 句柄编码 (scl/sda 用 (port<<8|pin) 编码) */
#define HW_IIC_SOFT(scl_hw_handle, sda_hw_handle)   \
        (IIC_BUS_TYPE_SOFT | ((uint32_t)(scl_hw_handle) << 16) | ((uint32_t)(sda_hw_handle) & 0xFFFFu))

/* 硬件 I2C 句柄编码 (instance_index: 0=I2C1, 1=I2C2, ...) */
#define HW_IIC_HARD(instance_index)   \
        (IIC_BUS_TYPE_HARD | (uint32_t)(instance_index))

/* ============================ 硬件引脚描述结构体 ============================ */
/**
 * @brief 逻辑总线 -> 硬件句柄 的映射条目
 * @note   此类型必须在 iic_cfg.h 定义, 因为映射表 g_iic_bus_map[] 需要它,
 *         iic_port.h 仅做前向引用以避免 typedef 重定义。
 */
typedef struct {
    uint16_t  bus_id;       /* 逻辑总线号 (与 iic_bus_t 一致) */
    uint32_t  hw_handle;    /* 硬件句柄 (由 HW_IIC_SOFT/HW_IIC_HARD 编码) */
} iic_hw_bus_t;

/* ============================ 端口/引脚编码约定 ============================ */
/* 与 gpio_cfg.h 保持一致的端口索引；加 #ifndef 防止两边同时包含时重定义告警 */
#ifndef GPIO_PORT_A
#define GPIO_PORT_A 0
#endif
#ifndef GPIO_PORT_B
#define GPIO_PORT_B 1
#endif
#ifndef GPIO_PORT_C
#define GPIO_PORT_C 2
#endif
#ifndef GPIO_PORT_D
#define GPIO_PORT_D 3
#endif
#ifndef GPIO_PORT_E
#define GPIO_PORT_E 4
#endif

#ifndef HW_HANDLE_GPIO
#define HW_HANDLE_GPIO(port, pin)  (((uint32_t)(port) << 8) | (uint32_t)(pin))
#endif

/* ============================ 总线映射表 ============================ */
/**
 * @brief 逻辑总线号 -> 硬件句柄 的映射表
 *
 * 对应板子接线:
 *   组1 (KT_A):  SCL = PB13 (0x010D),  SDA = PB14 (0x010E)
 *   组2 (KT_B):  SCL = PA9  (0x0009),  SDA = PA10 (0x000A)
 */
static const iic_hw_bus_t g_iic_bus_map[] = {
    { IIC_BUS_KT_A, HW_IIC_SOFT(HW_HANDLE_GPIO(GPIO_PORT_B, 13), HW_HANDLE_GPIO(GPIO_PORT_B, 14)) },
    { IIC_BUS_KT_B, HW_IIC_SOFT(HW_HANDLE_GPIO(GPIO_PORT_A,  9), HW_HANDLE_GPIO(GPIO_PORT_A, 10)) },
};

#define IIC_BUS_TABLE_SIZE   (sizeof(g_iic_bus_map) / sizeof(g_iic_bus_map[0]))

#ifdef __cplusplus
}
#endif
#endif /* IIC_CFG_H_ */
