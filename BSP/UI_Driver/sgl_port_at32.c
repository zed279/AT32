/**
 * @file    sgl_port_at32.c
 * @brief   SGL -> ST7789 (AT32F403A) 移植实现
 *
 * @details 一次刷屏的完整链路:
 *            sgl_task_handler()
 *              -> 收集脏区 -> draw_obj_slice() 画进 s_fb_buf
 *              -> sgl_fbdev_flush_area()  (CONFIG_SGL_COLOR16_SWAP 在此就地换字节序)
 *              -> 本文件的 sgl_port_flush_area()
 *              -> st7789_blit()  (BSP/LCD_Driver: CASET/RASET/RAMWR + SPI DMA)
 *
 * @note    为什么用单缓冲:
 *          SGL 的双缓冲是为"DMA 后台搬运 + CPU 同时画下一块"设计的. 本工程
 *          st7789_blit() 是同步阻塞的 (spi_write 内部等 DMA 完成才返回),
 *          双缓冲拿不到任何并行收益, 只会多占 19KB SRAM. 所以 buffer[1] = NULL.
 *          将来若把 st7789_blit 改成异步 DMA + 完成回调, 再在这里换双缓冲,
 *          并把 sgl_fbdev_flush_ready() 从 flush 回调里挪到 DMA 完成中断.
 *
 * @note    缓冲区行数 (SGL_PORT_BUF_LINES) 的取舍:
 *          行数越多 -> 一次窗口传输越大, 命令开销占比越低, 刷屏越快;
 *          行数越少 -> RAM 越省, 脏区很小时也不会白传.
 *          40 行 = 屏宽 * 40 * 2, 是速度/内存的折中:
 *            竖屏 240 宽 -> 19.2KB, 全屏重绘约 6 次窗口
 *            横屏 320 宽 -> 25.6KB, 全屏重绘同样约 6 次窗口
 *          横屏嫌 RAM 紧可把行数降到 32 (20.5KB), 代价是全屏多 1~2 次窗口.
 */
#include "sgl_port_at32.h"
#include "st7789.h"
#include "bm_hal.h"

#include <sgl.h>
#include <sgl_mm.h>

/* SGL 的帧缓冲行数与宽度 —— 宽度必须等于当前方向的屏幕宽度,
 * 因为 SGL 按 "xres 像素一行" 切块, 宽度不匹配就会错行或越界.
 * 方向由 sgl_port_at32.h 的 SGL_PORT_LANDSCAPE 决定. */
#define SGL_PORT_BUF_LINES      40
#define SGL_PORT_BUF_WIDTH      SGL_PORT_WIDTH

/* 屏的两个物理边长: 240 和 320. 无论竖屏还是横屏, 行宽都不会超过长边.
 * (不能用 ST7789_WIDTH —— 它等于 240, 是"竖屏基准宽", 横屏 320 会被它误判) */
#if (SGL_PORT_BUF_WIDTH > ST7789_HEIGHT)
#error "SGL 缓冲宽度不能大于屏幕物理长边"
#endif

#if (CONFIG_SGL_FBDEV_PIXEL_DEPTH != 16)
#error "本移植层按 RGB565 实现, CONFIG_SGL_FBDEV_PIXEL_DEPTH 必须是 16"
#endif

#if (CONFIG_SGL_FBDEV_ROTATION != 0)
#error "本移植层固定竖屏(ROT_0), 若改 CONFIG_SGL_FBDEV_ROTATION 请同步改下方 st7789_set_rotation()"
#endif

/* 单块行缓冲: SGL 只在这里逐条脏区绘制/刷屏, 不占整屏 VRAM */
static sgl_color_t s_fb_buf[SGL_PORT_BUF_WIDTH * SGL_PORT_BUF_LINES];

/* 上一次推进 tick 的时刻, 用于把 BM_GetTick() 的毫秒差喂给 SGL */
static uint32_t s_last_tick;

/**
 * @brief SGL 刷屏回调 —— 由 sgl_fbdev_flush_area() 在每块脏区画完后调用
 * @param area 脏区 (左闭右闭)
 * @param src  像素缓冲, 每行 (x2-x1+1) 个像素, 已按 ST7789 需要的高字节在前排列
 * @note  必须调用 sgl_fbdev_flush_ready(), 否则绘制循环会永远等在
 *        sgl_fbdev_flush_wait_ready() 上 (屏幕静止不动, 表现为"卡死").
 */
static void sgl_port_flush_area(sgl_area_t *area, sgl_color_t *src)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    (void)st7789_blit((uint16_t)area->x1, (uint16_t)area->y1, w, h,
                      (const uint8_t *)src);

    /* 同步刷屏: 本函数返回时数据已经进屏, 直接标记缓冲可再用 */
    sgl_fbdev_flush_ready();
}

int sgl_port_init(void)
{
    sgl_fbinfo_t fbinfo;

    /* 1. LCD: 引脚 + SPI1 + 复位 + 初始化序列 + 开背光 */
    if (st7789_init() != ST7789_OK) {
        return -1;
    }

    /* 2. 方向必须与 CONFIG_SGL_FBDEV_ROTATION 保持一致 (后者恒为 0)
     *
     *   坑: ST7789_ROT_90 在本 BSP 里是 **横屏 320x240** (s_width=320),
     *   不是"顺时针转 90 度变竖屏". 传 ROT_90 会让下面的宽度检查 (320 > 240)
     *   直接 return -2, UiDemo_Init() 失败, main() 进 LED 闪烁死循环,
     *   表现为"屏幕全黑什么都不画". 竖屏请用 ROT_0.
     *
     *   方向在 sgl_port_at32.h 里用 SGL_PORT_LANDSCAPE 一个宏切换,
     *   下面的宽度检查会自动适配, 不需要改动这里. */
    (void)st7789_set_rotation(SGL_PORT_LCD_ROT);
    (void)st7789_fill(ST7789_BLACK);

    /* 行缓冲按 SGL_PORT_WIDTH 分配, 必须和实际方向严丝合缝:
     *   窄了 -> 越界写; 宽了 -> SGL 按错误的行宽切块, 画面错行 */
    if (st7789_get_width() != SGL_PORT_BUF_WIDTH) {
        return -2;
    }

    /* 3. 注册帧缓冲设备 */
    fbinfo.buffer[0]   = s_fb_buf;
    fbinfo.buffer[1]   = NULL;                       /* 单缓冲, 见文件头说明 */
    fbinfo.buffer_size = SGL_PORT_BUF_WIDTH * SGL_PORT_BUF_LINES;   /* 单位: 像素 */
    fbinfo.xres        = (int16_t)st7789_get_width();
    fbinfo.yres        = (int16_t)st7789_get_height();
    fbinfo.flush_area  = sgl_port_flush_area;

    if (sgl_fbdev_register(&fbinfo) != 0) {
        return -3;
    }

    /* 4. SGL 全局初始化 (内部会建堆 + 建默认页面对象) */
    if (sgl_init() != 0) {
        return -4;
    }

    s_last_tick = BM_GetTick();
    return 0;
}

void sgl_port_task(void)
{
    uint32_t now = BM_GetTick();
    uint32_t delta = now - s_last_tick;

    /* SGL 只认自己的 tick_ms, 这里用"毫秒差"喂它, 不依赖任何定时器中断.
     * delta 为 0 时不要调 sgl_tick_inc, 否则白跑一次 tick 判断. */
    if (delta != 0u) {
        sgl_tick_inc(delta);
        s_last_tick = now;
    }

    sgl_task_handler();     /* 内部按 SGL_SYSTEM_TICK_MS 限流, 调用频率高也无害 */
    sgl_timer_handler();    /* 控件自带的软件定时器 (文本滚动/闪烁等) */
}

void sgl_port_refresh(void)
{
    uint32_t now = BM_GetTick();
    uint32_t delta = now - s_last_tick;

    if (delta != 0u) {
        sgl_tick_inc(delta);
        s_last_tick = now;
    }

    sgl_task_handler_sync();
    sgl_timer_handler();
}

void sgl_port_heap_info(uint32_t *used, uint32_t *total)
{
    sgl_mm_monitor_t mon = sgl_mm_get_monitor();

    if (used  != NULL) { *used  = (uint32_t)mon.used_size;  }
    if (total != NULL) { *total = (uint32_t)mon.total_size; }
}
