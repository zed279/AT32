/**
 * @file    ui_demo.h
 * @brief   SGL 图形界面移植验证 Demo (AT32F403A + ST7789)
 *
 * @note    本文件只声明两个入口, 与 bm_arch 的调度器对接方式见 main.c:
 *            Scheduler_RegisterTask(TASK_UI, UiDemo_Task, 10);
 */
#ifndef UI_DEMO_H_
#define UI_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 LCD + SGL 并构建演示页面
 * @retval 0 成功, 其它失败 (LCD 或 SGL 初始化错误)
 */
int UiDemo_Init(void);

/**
 * @brief  周期任务, 周期固定 10ms (与 CONFIG_SGL_SYSTICK_MS 一致)
 */
void UiDemo_Task(void);

#ifdef __cplusplus
}
#endif
#endif /* UI_DEMO_H_ */
