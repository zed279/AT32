/**
 * @file    gpio.h
 * @brief   可复用 GPIO 驱动模板 - 公共 API
 * @note    本驱动采用"硬件抽象层 + 通用逻辑层"分层设计:
 *          - gpio.c   : 通用逻辑、参数校验、回调管理 (与 MCU 无关)
 *          - gpio_port: 硬件相关操作 (寄存器读写、时钟使能、中断配置)
 *          移植时只需实现 gpio_port.h 中声明的接口即可。
 *
 *          使用流程:
 *          1. 在 gpio_cfg.h 中定义引脚编号与硬件端口/引脚的映射表
 *          2. 实现 gpio_port.c 中的硬件相关函数
 *          3. 调用 gpio_pin_init() 初始化引脚
 *          4. 调用 gpio_write()/gpio_read()/gpio_toggle() 进行操作
 *          5. 如需中断, 调用 gpio_attach_interrupt() 注册回调
 */
#ifndef GPIO_H_
#define GPIO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "gpio_cfg.h"

/* ============================ 返回码 ============================ */
typedef enum {
    GPIO_OK            =  0,   /* 成功 */
    GPIO_ERR_PARAM    = -1,    /* 参数非法 */
    GPIO_ERR_PIN      = -2,    /* 引脚未定义 / 越界 */
    GPIO_ERR_BUSY     = -3,    /* 引脚已被占用 (如已使能中断) */
    GPIO_ERR_HW       = -4,    /* 硬件操作失败 */
    GPIO_ERR_UNSUPP   = -5,    /* 当前硬件不支持该功能 */
} gpio_status_t;

/* ============================ 引脚编号 ============================ */
/**
 * @brief 逻辑引脚编号
 * @note  用户在 gpio_cfg.h 中通过 GPIO_PIN_x 宏定义,
 *        并在 g_gpio_pin_map[] 中映射到实际硬件端口/引脚
 */
typedef uint16_t gpio_pin_t;

/* ============================ 方向 ============================ */
typedef enum {
    GPIO_DIR_INPUT  = 0,   /* 输入 */
    GPIO_DIR_OUTPUT = 1,   /* 推挽输出 */
    GPIO_DIR_OD     = 2,   /* 开漏输出 */
    GPIO_DIR_ANALOG = 3,   /* 模拟功能 (ADC/DAC/COMP 等) */
} gpio_dir_t;

/* ============================ 上下拉 ============================ */
typedef enum {
    GPIO_PU_NONE = 0,    /* 无上下拉 (悬浮) */
    GPIO_PU_UP   = 1,    /* 上拉 */
    GPIO_PU_DOWN = 2,    /* 下拉 */
} gpio_pull_t;

/* ============================ 电平状态 ============================ */
typedef enum {
    GPIO_LOW  = 0,         /* 低电平 */
    GPIO_HIGH = 1,         /* 高电平 */
} gpio_state_t;

/* ============================ 中断触发方式 ============================ */
typedef enum {
    GPIO_INT_NONE    = 0,  /* 无中断 (用于 detach) */
    GPIO_INT_RISING  = 1,  /* 上升沿触发 */
    GPIO_INT_FALLING = 2,  /* 下降沿触发 */
    GPIO_INT_BOTH    = 3,  /* 双沿触发 */
    GPIO_INT_LOW     = 4,  /* 低电平触发 (部分 MCU 支持) */
    GPIO_INT_HIGH    = 5,  /* 高电平触发 (部分 MCU 支持) */
} gpio_int_mode_t;

/* ============================ 初始化配置 ============================ */
typedef struct {
    gpio_pin_t    pin;          /* 逻辑引脚编号 */
    gpio_dir_t    dir;          /* 方向 */
    gpio_pull_t   pull;         /* 上下拉 */
    gpio_state_t  init_state;   /* 输出模式下的初始电平 (输入模式忽略) */
    uint8_t       speed;        /* 驱动速度等级 (0=低速, MCU 相关, 可忽略) */
} gpio_config_t;

/* ============================ 回调函数类型 ============================ */
typedef void (*gpio_callback_t)(gpio_pin_t pin);

/* ============================ 公共 API ============================ */

/**
 * @brief  初始化指定引脚
 * @param  cfg  初始化配置 (不能为 NULL)
 * @retval GPIO_OK / GPIO_ERR_PARAM / GPIO_ERR_PIN / GPIO_ERR_HW
 */
gpio_status_t gpio_pin_init(const gpio_config_t *cfg);

/**
 * @brief  反初始化引脚 (恢复到默认/复位状态, 释放占用的中断)
 */
gpio_status_t gpio_deinit(gpio_pin_t pin);

/**
 * @brief  设置引脚方向
 */
gpio_status_t gpio_set_dir(gpio_pin_t pin, gpio_dir_t dir);

/**
 * @brief  写电平 (仅输出模式)
 */
gpio_status_t gpio_write(gpio_pin_t pin, gpio_state_t state);

/**
 * @brief  读电平 (输入或输出模式均可)
 */
gpio_state_t  gpio_read(gpio_pin_t pin);

/**
 * @brief  翻转电平
 */
gpio_status_t gpio_toggle(gpio_pin_t pin);

/**
 * @brief  注册中断回调
 * @param  pin   引脚
 * @param  mode  触发方式 (GPIO_INT_NONE 表示注销)
 * @param  cb    回调函数 (NULL 表示注销)
 * @note   同一引脚只能注册一个回调; 重复注册会覆盖前一次
 */
gpio_status_t gpio_attach_interrupt(gpio_pin_t pin,
                                    gpio_int_mode_t mode,
                                    gpio_callback_t cb);

/**
 * @brief  注销中断回调 (内部等价于 gpio_attach_interrupt(pin, NONE, NULL))
 */
gpio_status_t gpio_detach_interrupt(gpio_pin_t pin);

/**
 * @brief  中断服务例程入口 (由硬件 ISR 调用)
 * @param  hw_pin  触发中断的硬件引脚句柄 (由 port 层提供)
 * @note   port 层在 ISR 中应调用本函数分发到用户回调
 */
void gpio_irq_handler(uint32_t hw_pin);

#ifdef __cplusplus
}
#endif
#endif /* GPIO_H_ */
