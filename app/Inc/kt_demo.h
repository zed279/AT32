/**
 * @file    kt_demo.h
 * @brief   KT06xx 双组接收芯片初始化验证界面
 *
 * @note    本模块取代 EncoderDemo 占屏，用来验证两条独立 I2C 总线上的
 *          两组 KT 芯片（各组 = KT0656M 主 + KT0650 从）是否能正常识别与初始化。
 *
 *          屏幕内容（240x320 竖屏，8x16 字体）：
 *            标题
 *            CH1 总线/使能脚说明
 *              M 0x39 ID:OK FW:11      ← 主路 KT0656M
 *                PWR:OK INIT:OK TUNE:OK
 *              S 0x3D ID:OK            ← 从路 KT0650
 *                INIT:OK
 *            CH2 ...（同上）
 *
 *          为什么从路不显示 FW：KT0650 datasheet V0.1.2 没有定义 0x010F
 *          （FIRMWARE_VERSION），读回来是未定义值，显示出来只会误导。
 *          CHIP_ID（0x0192/0x0193）两芯片都有，都是 0x4B54，可以显示。
 */
#ifndef KT_DEMO_H_
#define KT_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*==============================================================================
 * 参数
 *============================================================================*/
#define KT_DEMO_PERIOD_MS       500U    /* 轮询周期: 只在失败重试窗口内干活 */
#define KT_DEMO_RETRY_MS        5000U   /* 某组初始化失败后，多久重试一次 */

/*==============================================================================
 * API
 *============================================================================*/
/**
 * @brief 初始化 LCD + 逐组完成 KT 全流程（上电 → 挂总线 → probe → init → tune）
 * @retval 0 = 至少一组完全成功；-1 = LCD 初始化失败
 * @note   本函数会阻塞数秒（每组含 CHIP_EN 100ms 放电 + 上电等待 + PLL 锁定），
 *         属于上电自检性质，放在 Scheduler 启动前调用。
 */
int  KtDemo_Init(void);

/**
 * @brief 调度任务：失败组定期重试，状态变化才重绘
 * @note  完整成功后本任务基本空转，不会占用 SPI 带宽。
 */
void KtDemo_Task(void);

#ifdef __cplusplus
}
#endif
#endif /* KT_DEMO_H_ */
