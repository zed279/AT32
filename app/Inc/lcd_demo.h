/**
 * @file    lcd_demo.h
 * @brief   ST7789 LCD 流畅度演示 (BM Arch 应用层)
 *
 * @details 三种负载轮流跑, 用来判断这条 SPI 链路到底能吃多快:
 *            1. FILL   全屏刷色   —— 极限带宽 (150KB/帧)
 *            2. BOUNCE 弹跳方块   —— 典型 UI 局部刷新 (每帧约 12KB)
 *            3. TEXT   滚动文字   —— 文本渲染开销
 *          顶部 HUD 实时显示 fps 与吞吐 (KB/s), 同时每秒从串口打一行日志.
 */
#ifndef __LCD_DEMO_H
#define __LCD_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * API
 *============================================================================*/

/**
 * @brief  初始化 LCD 并进入第一种演示模式
 * @retval 0 = 成功, -1 = ST7789 初始化失败 (多为 gpio/spi 映射表漏条目)
 * @note   内部会阻塞约 300ms (硬件复位 + 初始化序列 + 清屏), 请在启动阶段调用
 */
int  LcdDemo_Init(void);

/**
 * @brief  演示任务 (注册到调度器, 周期建议 1ms)
 * @note   每调用一次渲染一帧, 帧内的 SPI 传输是阻塞的, 帧耗时 2~80ms 不等
 */
void LcdDemo_Task(void);

#ifdef __cplusplus
}
#endif
#endif /* __LCD_DEMO_H */
