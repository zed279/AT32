/**
 * @file    encoder_demo.h
 * @brief   编码器验证界面 —— 左右转动切换屏幕颜色
 *
 * @note    本模块取代原 lcd_demo (动画跑分), 目的是验证硬件正交解码是否可用:
 *            - 每累计 4 个计数 (一个机械定位点) 换一种底色
 *            - 顶部状态栏显示累计计数、当前颜色、最后一次转向
 *          静止时完全不刷屏, 不会抢占 SPI 带宽.
 */
#ifndef ENCODER_DEMO_H_
#define ENCODER_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*==============================================================================
 * 参数
 *============================================================================*/
#define ENC_DEMO_PERIOD_MS      5U     /* 采样周期: 5ms, 人手转动远慢于此 */
#define ENC_COUNTS_PER_DETENT   4      /* 一个机械定位点的四倍频计数        */

/*==============================================================================
 * API
 *============================================================================*/
int  EncoderDemo_Init(void);       /* 编码器 + LCD 初始化, 画首屏 */
void EncoderDemo_Task(void);       /* 调度任务: 采样 + 换色        */

#ifdef __cplusplus
}
#endif
#endif /* ENCODER_DEMO_H_ */
