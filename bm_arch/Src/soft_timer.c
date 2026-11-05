/**
 * @file    soft_timer.c
 * @brief   软件定时器实现
 */
#include "soft_timer.h"
#include "bm_hal.h"

void SoftTimer_Init(SoftTimer_t *timer)
{
    timer->start_time  = 0;
    timer->duration_ms = 0;
    timer->running     = false;
    timer->timeout     = false;
}

void SoftTimer_Start(SoftTimer_t *timer, uint32_t duration_ms)
{
    timer->start_time  = BM_GetTick();
    timer->duration_ms = duration_ms;
    timer->running     = true;
    timer->timeout     = false;
}

void SoftTimer_Stop(SoftTimer_t *timer)
{
    timer->running = false;
    timer->timeout = false;
}

void SoftTimer_Update(SoftTimer_t *timer)
{
    if (!timer->running) return;

    if (BM_GetTick() - timer->start_time >= timer->duration_ms) {
        timer->running = false;
        timer->timeout = true;
    }
}

bool SoftTimer_IsRunning(SoftTimer_t *timer)
{
    return timer->running;
}

bool SoftTimer_IsTimeout(SoftTimer_t *timer)
{
    return timer->timeout;
}
