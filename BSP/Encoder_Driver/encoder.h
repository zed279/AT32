/**
 * @file    encoder.h
 * @brief   增量式旋转编码器驱动 (AT32F403A 硬件正交解码 / TMR4)
 *
 * @note    硬件连接 (本工程):
 *            PB6 -> TMR4_CH1  (编码器 B 相)
 *            PB7 -> TMR4_CH2  (编码器 A 相)
 *
 *          依据: AT32F403A 数据手册 DS V2.04 第 29 页 (LQFP100 引脚 42/43),
 *                PB6 默认可作 TMR4_CH1, PB7 默认可作 TMR4_CH2, 无需 IOMUX 重映射.
 *
 *          解码方式: TMR4 从模式 = 编码器模式 C (SMS=011), 即 TI1/TI2 双边沿
 *                    四倍频, 一个电气周期计 4 个数. 全程无 CPU 参与, 无中断,
 *                    计数精度不受轮询周期影响 —— 上层只需周期读增量.
 *
 *          设计取舍: 不注册 TMR4 溢出中断. 计数器 16 位自由回绕, 增量按
 *                    (int16_t)(now - last) 计算, 天然安全, 只要两次采样之间
 *                    的绝对变化 < 32767 个计数即不会算错 (人手不可能做到).
 *
 * @note    移植到其它 MCU: 只需改 encoder.c 顶部的 ENC_TMR / ENC_PINS 等宏,
 *          并把库函数换成对应的定时器编码器模式配置.
 */
#ifndef ENCODER_H_
#define ENCODER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 配置 ============================ */
/**
 * @brief 计数方向取反
 * @note  0 = A 相超前 B 相时计数增加 (右旋为正)
 *        1 = 反向. 若实测转向与预期相反, 改这里即可, 不必动接线.
 */
#ifndef ENC_DIR_INVERT
#  define ENC_DIR_INVERT      0
#endif

/**
 * @brief 输入数字滤波深度 (0~15)
 * @note  TMR 输入滤波器: fSAMPLING = fDTS / 2^(N+1) 档位, N = 可选 1/2/4/8 次采样.
 *        本工程取 15 = fDTS/32 + 连续 8 次一致才认, 用来吃掉机械触点的抖动.
 *        TMR4 时钟 240MHz 时采样时钟仍有 7.5MHz, 远高于人手转动频率,
 *        不会丢步; 若换成高速光电编码器 (数十 kHz 以上) 需降低该值.
 */
#ifndef ENC_IC_FILTER
#  define ENC_IC_FILTER       0x0F
#endif

/* ============================ API ============================ */

/**
 * @brief  初始化编码器 (GPIO 输入 + TMR4 编码器模式)
 * @note   调用前 system_clock_config() 必须已执行 (TMR4 依赖 APB1 时钟).
 */
void    Encoder_Init(void);

/**
 * @brief  读取自上次调用以来的计数增量
 * @retval 带符号增量: 正 = 右旋 (A 相超前), 负 = 左旋
 * @note   四倍频: 每经过一个机械定位点 (detent) 通常为 +/-4.
 *         必须在单任务上下文内调用 (内部有 last_raw 状态).
 */
int16_t Encoder_ReadDelta(void);

/**
 * @brief  累计计数 (带符号, 上电以来)
 */
int32_t Encoder_GetCount(void);

/**
 * @brief  清零累计计数 (硬件计数器同时归零)
 */
void    Encoder_ResetCount(void);

/**
 * @brief  直接读硬件 16 位计数器 (调试用, 不影响内部增量状态)
 */
uint16_t Encoder_ReadRaw(void);

#ifdef __cplusplus
}
#endif
#endif /* ENCODER_H_ */
