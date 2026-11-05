/**
 * @file    bm_hal.h
 * @brief   裸机架构 (BM Arch) 硬件抽象接口
 *
 * @details 本文件定义了 BM Arch 框架所依赖的最小硬件接口。
 *          用户只需在目标 MCU 上实现以下 4 个函数，即可将整套框架移植到任意平台。
 *
 * @note    移植示例 (STM32 HAL):
 *            uint32_t BM_GetTick(void)        { return HAL_GetTick(); }
 *            void     BM_Delay(uint32_t ms)   { HAL_Delay(ms); }
 *            void     BM_EnterCritical(void)  { __disable_irq(); }
 *            void     BM_ExitCritical(void)   { __enable_irq(); }
 *
 *          移植示例 (ESP32):
 *            uint32_t BM_GetTick(void)        { return esp_timer_get_time() / 1000; }
 *            void     BM_Delay(uint32_t ms)   { vTaskDelay(pdMS_TO_TICKS(ms)); }
 *            void     BM_EnterCritical(void)  { portDISABLE_INTERRUPTS(); }
 *            void     BM_ExitCritical(void)   { portENABLE_INTERRUPTS(); }
 */
#ifndef __BM_HAL_H
#define __BM_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/*==============================================================================
 * 系统时基 — 返回自上电以来的毫秒计数 (需递增, 允许溢出回绕)
 *============================================================================*/
uint32_t BM_GetTick(void);

/*==============================================================================
 * 阻塞延时 (毫秒)
 *============================================================================*/
void BM_Delay(uint32_t ms);

/*==============================================================================
 * 临界区保护 — 关闭 / 恢复全局中断
 *============================================================================*/
void BM_EnterCritical(void);
void BM_ExitCritical(void);

#ifdef __cplusplus
}
#endif
#endif /* __BM_HAL_H */
