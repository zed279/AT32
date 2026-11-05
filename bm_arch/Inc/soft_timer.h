/**
 * @file    soft_timer.h
 * @brief   软件定时器 (非阻塞)
 *
 * @details 基于 BM_GetTick() 的轻量级软件定时器。
 *          为状态机提供非阻塞的延时等待能力，替代阻塞式 HAL_Delay()。
 *          纯软件实现, 不占用任何硬件定时器资源。
 *
 * @note    典型用法:
 *            SoftTimer_t timer;
 *            SoftTimer_Init(&timer);
 *            SoftTimer_Start(&timer, 50);    // 启动 50ms 计时
 *            // ... 在轮询循环中 ...
 *            SoftTimer_Update(&timer);
 *            if (SoftTimer_IsTimeout(&timer)) { ... }
 */
#ifndef __SOFT_TIMER_H
#define __SOFT_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/*==============================================================================
 * 定时器结构
 *============================================================================*/
typedef struct {
    uint32_t start_time;    /* 启动时的 Tick       */
    uint32_t duration_ms;   /* 定时时长 (ms)      */
    bool     running;       /* 运行中标志          */
    bool     timeout;       /* 超时标志            */
} SoftTimer_t;

/*==============================================================================
 * API
 *============================================================================*/
void SoftTimer_Init(SoftTimer_t *timer);
void SoftTimer_Start(SoftTimer_t *timer, uint32_t duration_ms);
void SoftTimer_Stop(SoftTimer_t *timer);
void SoftTimer_Update(SoftTimer_t *timer);
bool SoftTimer_IsRunning(SoftTimer_t *timer);
bool SoftTimer_IsTimeout(SoftTimer_t *timer);

#ifdef __cplusplus
}
#endif
#endif /* __SOFT_TIMER_H */
