/**
 * @file    ui_rx.h
 * @brief   UHF 无线麦克风接收机 —— 主屏 (320 x 240 横屏)
 *
 * @details 一屏两个通道卡片 (CH-A / CH-B) + 顶部状态栏 + 底部操作提示栏,
 *          数据全部来自 KT0656M / KT0650 寄存器快照。
 *
 *          页面实现见 app/Src/ui_rx.c。main.c 里这样用:
 *              UiRx_Init();                                  // 建页面
 *              Scheduler_RegisterTask(TASK_UI, UiRx_Task, 10); // 10ms 周期
 *
 * @note    本模块不依赖 SGL 之外的任何东西, 只对外暴露下面这几个函数。
 *          业务层要让界面知道"频率改了 / 切通道了", 调对应 setter 即可,
 *          不需要碰内部控件。
 */
#ifndef UI_RX_H_
#define UI_RX_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 通道号, 与 kt0656m 的 KT_CH_A / KT_CH_B 语义一致, 但这里独立定义,
 * 免得 UI 层被驱动头文件绑架 */
#define UI_RX_CH_A          0
#define UI_RX_CH_B          1
#define UI_RX_CH_NUM        2

/**
 * @brief  建立接收机主屏
 * @retval 0 = 成功; 非 0 = sgl_port_init() 的错误码 (见 main.c 的 LED 闪码)
 * @note   内部会调 sgl_port_init() / Encoder_Init() / PB9 初始化,
 *         所以**不要**再在外面重复初始化屏幕和编码器
 */
int  UiRx_Init(void);

/**
 * @brief  周期任务, 10ms 调一次 (= CONFIG_SGL_SYSTICK_MS)
 * @note   顺序: 输入 -> 数据快照 -> 条幅动画 -> SGL 刷屏
 */
void UiRx_Task(void);

/**
 * @brief  告诉界面某个通道的接收频率 (业务层 tune 成功后调用)
 * @param  ch       UI_RX_CH_A / UI_RX_CH_B
 * @param  freq_khz 频率, 单位 kHz (步进 25kHz)
 * @note   界面只负责显示, 不会反过来去调 kt0656m_tune()
 */
void UiRx_SetFreq(uint8_t ch, uint32_t freq_khz);

/**
 * @brief  选中某个通道 (蓝色高亮框)
 */
void UiRx_SetSelected(uint8_t ch);

/**
 * @brief  读当前选中通道
 */
uint8_t UiRx_GetSelected(void);

/**
 * @brief  注册"短按"回调 (对应底部提示 PRESS MENU)
 * @note   传 NULL 取消注册。未注册时短按只在提示栏闪一下文字。
 */
void UiRx_OnMenu(void (*cb)(void));

/**
 * @brief  注册"长按 >=500ms"回调 (对应底部提示 HOLD PAIR)
 */
void UiRx_OnPair(void (*cb)(void));

/**
 * @brief  让提示栏闪一条自定义文字 (1.2s 后自动恢复)
 * @param  txt 要显示的文字, 只存指针, 必须是常量串或静态缓冲
 * @note   给对频/菜单这些还没实现的动作做即时反馈用
 */
void UiRx_Toast(const char *txt);

#ifdef __cplusplus
}
#endif
#endif /* UI_RX_H_ */
