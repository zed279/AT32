/**
 * @file    bm_hal_port.h
 * @brief   BM Arch 移植层补充接口 (不进入 bm_hal.h 契约)
 *
 * @details bm_hal.h 只定义 4 个跨平台函数, 保持契约纯净;
 *          目标平台特有的初始化入口 (如时基源启动) 放在本文件中。
 *
 * @note    使用顺序:
 *            1. system_clock_config()      系统时钟就绪
 *            2. BM_HAL_TickInit()          启动 1ms 时基源 (TMR2)
 *            3. Scheduler_Init() / Logger_Init() 等框架模块
 *          必须在 BM_HAL_TickInit() 之后调用任何依赖 BM_GetTick() 的模块。
 */
#ifndef __BM_HAL_PORT_H
#define __BM_HAL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 启动 1ms 系统时基 (AT32F403A: TMR2 溢出中断)
 *   - 依赖 system_clock_config() 已完成, 内部按运行时 APB1 频率计算分频
 *   - 可重复调用 (第二次会重新装载计数)
 *============================================================================*/
void BM_HAL_TickInit(void);

#ifdef __cplusplus
}
#endif
#endif /* __BM_HAL_PORT_H */
