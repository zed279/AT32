/**
 * @file    sgl_port_at32.h
 * @brief   SGL 图形库在 AT32F403A + ST7789 上的移植适配层
 *
 * @note    分层关系:
 *            app/ui_demo.c   (业务界面)
 *              -> sgl.h      (图形库, third_party/sgl)
 *              -> 本文件      (移植层: 刷屏回调 / 时基 / 初始化)
 *                -> st7789.h (BSP/LCD_Driver, 只依赖 GPIO + SPI 抽象层)
 *
 *          SGL 官方移植只需要三件事:
 *            1. 注册帧缓冲 (sgl_fbdev_register) + 一个 flush 回调
 *            2. 每 1ms (或 CONFIG_SGL_SYSTICK_MS) 推进 tick
 *            3. 主循环里周期调用 sgl_task_handler()
 *          本层把 1 和 3 包成 sgl_port_init() / sgl_port_task().
 *
 * @warning 本层与 third_party/sgl/sgl_config.h 强耦合:
 *            CONFIG_SGL_FBDEV_PIXEL_DEPTH 必须 = 16
 *            CONFIG_SGL_FBDEV_ROTATION    必须与 st7789 的旋转方向一致
 *            CONFIG_SGL_COLOR16_SWAP      必须 = 1 (ST7789 要高字节先出)
 *          改任何一项都要同时改另一处, 否则花屏或红蓝互换.
 */
#ifndef __SGL_PORT_AT32_H
#define __SGL_PORT_AT32_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*==============================================================================
 * 显示方向 —— 全工程只需要改这一处
 *============================================================================*/
/**
 * @brief 0 = 竖屏 240x320, 1 = 横屏 320x240
 *
 * @note  旋转由 ST7789 的 MADCTL 寄存器在硬件层完成, SGL 侧始终
 *        "逻辑坐标 == 屏幕坐标". 所以 third_party/sgl/sgl_config.h 里的
 *        CONFIG_SGL_FBDEV_ROTATION 必须保持 0 —— 改成 90 会让 SGL 自己
 *        再转一次(多分配一整块 25KB 旋转缓冲 + 逐像素搬运), 结果转两次.
 */
#define SGL_PORT_LANDSCAPE      1

#if (SGL_PORT_LANDSCAPE != 0)
#  define SGL_PORT_WIDTH        320
#  define SGL_PORT_HEIGHT       240
#  define SGL_PORT_LCD_ROT      ST7789_ROT_90      /* 本 BSP: ROT_90 = 横屏 320x240 */
#else
#  define SGL_PORT_WIDTH        240
#  define SGL_PORT_HEIGHT       320
#  define SGL_PORT_LCD_ROT      ST7789_ROT_0       /* 本 BSP: ROT_0 = 竖屏 240x320 */
#endif

/**
 * @brief  初始化 LCD + SGL (内含 st7789_init, 不要在外面重复调用)
 * @retval 0 成功, -1 LCD 初始化失败, -2 方向/缓冲宽度不符,
 *         -3 SGL 注册失败, -4 sgl_init 失败
 * @note   调用后屏幕会被清成页面底色, 但还没有任何控件 ——
 *         "建页面 + sgl_screen_load()" 由业务层负责 (见 ui_demo.c).
 */
int sgl_port_init(void);

/**
 * @brief  SGL 周期任务: 推进时基 + 事件队列 + 动画 + 脏区绘制刷屏
 * @note   放到 BM Arch 的调度器里, 周期 = CONFIG_SGL_SYSTICK_MS (10ms).
 *         内部刷屏是同步阻塞的 (SPI 15MHz, 240x40 像素约 13ms),
 *         所以这个任务的周期不宜小于 10ms, 否则会挤占其它任务.
 */
void sgl_port_task(void);

/**
 * @brief  强制同步刷新一帧 (调试/切页面后立即出图时用)
 */
void sgl_port_refresh(void);

/**
 * @brief  取 SGL 堆的使用情况, 用于页面开发阶段评估内存在哪
 * @param  used   已用字节数 (可为 NULL)
 * @param  total  总字节数   (可为 NULL)
 */
void sgl_port_heap_info(uint32_t *used, uint32_t *total);

#ifdef __cplusplus
}
#endif
#endif /* __SGL_PORT_AT32_H */
