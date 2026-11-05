/**
 * @file    lcd_demo.c
 * @brief   ST7789 LCD 流畅度演示实现
 *
 * @details 用三种负载覆盖三种典型场景, 每 6 秒轮换:
 *            FILL   全屏填色      -> 极限带宽, 反映 SPI 时钟上限
 *            BOUNCE 弹跳方块      -> 局部刷新, 反映"擦旧+画新"的实际帧率
 *            TEXT   滚动文字      -> 逐字窗口开销, 通常是最慢的一档
 *
 *          帧率统计口径: 每秒统计"渲染完成的帧数", 吞吐按"实际下发给屏的
 *          字节数 / 耗时"计算. 字节数由本文件按绘制面积累加, 不含命令字节.
 *
 * @note    帧内 SPI 传输是同步阻塞的 (见 spi_port_at32.c 的说明), 因此
 *          FILL 模式单帧最长 ~80ms, 期间其它调度任务会被推迟 —— 这是协作式
 *          调度 + 同步 DMA 的固有代价. 需要真并行请改双缓冲 + DMA 完成回调.
 */
#include "lcd_demo.h"
#include "st7789.h"
#include "bm_hal.h"
#include "soft_timer.h"
#include "logger.h"

#include <stdio.h>

/*==============================================================================
 * 配置
 *============================================================================*/
#define HUD_H               68u                       /* 顶部状态栏高度 (px)  */
#define AREA_Y              HUD_H                     /* 动画区起点            */
#define AREA_H              (ST7789_HEIGHT - HUD_H)   /* 动画区高度            */

#define MODE_PERIOD_MS      6000u                     /* 每种模式演示时长      */
#define HUD_PERIOD_MS       500u                      /* 状态栏刷新周期        */

#define BOUNCE_W            56u
#define BOUNCE_H            56u
#define BOUNCE_STEP         6
#define BOUNCE_BG           ST7789_DARKGRAY
#define BOUNCE_FG           ST7789_YELLOW

#define TEXT_Y              (AREA_Y + 40u)
#define TEXT_SCALE          3u
#define TEXT_STEP           8
#define TEXT_MSG            "BM ARCH + ST7789 + SPI1 DMA  >>>  15MHz  "

/*==============================================================================
 * 内部状态
 *============================================================================*/
typedef enum {
    DEMO_FILL = 0,
    DEMO_BOUNCE,
    DEMO_TEXT,
    DEMO_COUNT
} demo_mode_t;

static const char *const s_mode_name[DEMO_COUNT] = { "FILL", "BOUNCE", "TEXT" };

static demo_mode_t  s_mode;
static SoftTimer_t  s_mode_timer;

static int16_t      s_bx, s_by;        /* 方块左上角 */
static int16_t      s_vx, s_vy;        /* 方块速度   */
static uint8_t      s_fill_idx;        /* 填色轮转   */
static int16_t      s_text_x;          /* 文字滚动 x */

static uint32_t     s_frames;          /* 本统计窗口帧数       */
static uint32_t     s_bytes;           /* 本统计窗口下发字节数 */
static uint32_t     s_stat_tick;       /* 上次统计时刻         */
static uint32_t     s_hud_tick;        /* 上次刷 HUD 时刻      */
static uint16_t     s_fps;
static uint32_t     s_byte_rate;       /* B/s                  */

/*==============================================================================
 * 内部: 绘制
 *============================================================================*/

static void hud_draw(void)
{
    char line[24];

    /* 状态栏底色固定, 每次整块重画, 避免残留 */
    st7789_fill_rect(0, 0, ST7789_WIDTH, HUD_H, ST7789_NAVY);

    (void)snprintf(line, sizeof(line), "%s %uFPS", s_mode_name[s_mode],
                   (unsigned)s_fps);
    (void)st7789_draw_string(2, 0, line, ST7789_WHITE, ST7789_NAVY, 2);

    (void)snprintf(line, sizeof(line), "%luK/s 15MH",
                   (unsigned long)(s_byte_rate / 1024u));
    (void)st7789_draw_string(2, HUD_H / 2u, line, ST7789_CYAN, ST7789_NAVY, 2);

    s_bytes += (ST7789_WIDTH * HUD_H * 2u) + (16u * 16u * 2u) * 2u;
}

static void mode_enter(demo_mode_t mode)
{
    switch (mode) {
        case DEMO_FILL:
            s_fill_idx = 0;
            break;

        case DEMO_BOUNCE:
            s_bx = (int16_t)((ST7789_WIDTH - BOUNCE_W) / 2u);
            s_by = (int16_t)(AREA_Y + (AREA_H - BOUNCE_H) / 2u);
            s_vx = BOUNCE_STEP;
            s_vy = BOUNCE_STEP;
            st7789_fill_rect(0, AREA_Y, ST7789_WIDTH, AREA_H, BOUNCE_BG);
            s_bytes += ST7789_WIDTH * AREA_H * 2u;
            break;

        case DEMO_TEXT:
            s_text_x = (int16_t)ST7789_WIDTH;
            st7789_fill_rect(0, AREA_Y, ST7789_WIDTH, AREA_H, ST7789_BLACK);
            s_bytes += ST7789_WIDTH * AREA_H * 2u;
            break;

        default:
            break;
    }
}

/* 1. 全屏填色: 只刷 HUD 以下, 保住状态栏 */
static void frame_fill(void)
{
    static const st7789_color_t colors[] = {
        ST7789_RED, ST7789_GREEN, ST7789_BLUE, ST7789_YELLOW,
        ST7789_CYAN, ST7789_MAGENTA, ST7789_ORANGE, ST7789_WHITE,
    };

    st7789_fill_rect(0, AREA_Y, ST7789_WIDTH, AREA_H,
                     colors[s_fill_idx]);
    s_fill_idx = (uint8_t)((s_fill_idx + 1u) % (sizeof(colors) / sizeof(colors[0])));

    s_bytes += (uint32_t)ST7789_WIDTH * AREA_H * 2u;
}

/* 2. 弹跳方块: 擦旧 + 移动 + 画新, 单帧只推约 2 个方块面积 */
static void frame_bounce(void)
{
    /* 擦掉旧位置 */
    (void)st7789_fill_rect((uint16_t)s_bx, (uint16_t)s_by,
                           BOUNCE_W, BOUNCE_H, BOUNCE_BG);

    /* 位移 + 边界反弹 (先夹紧再反向, 避免高速穿墙) */
    s_bx = (int16_t)(s_bx + s_vx);
    s_by = (int16_t)(s_by + s_vy);

    if (s_bx <= 0) {
        s_bx = 0;
        s_vx = (int16_t)(-s_vx);
    } else if ((int16_t)(s_bx + BOUNCE_W) >= (int16_t)ST7789_WIDTH) {
        s_bx = (int16_t)(ST7789_WIDTH - BOUNCE_W);
        s_vx = (int16_t)(-s_vx);
    }
    if (s_by <= (int16_t)AREA_Y) {
        s_by = (int16_t)AREA_Y;
        s_vy = (int16_t)(-s_vy);
    } else if ((int16_t)(s_by + BOUNCE_H) >= (int16_t)ST7789_HEIGHT) {
        s_by = (int16_t)(ST7789_HEIGHT - BOUNCE_H);
        s_vy = (int16_t)(-s_vy);
    }

    /* 画新位置 */
    (void)st7789_fill_rect((uint16_t)s_bx, (uint16_t)s_by,
                           BOUNCE_W, BOUNCE_H, BOUNCE_FG);

    s_bytes += 2u * BOUNCE_W * BOUNCE_H * 2u;
}

/* 3. 滚动文字: 整行擦除后重画, 文字逐字建窗口, 是最慢的一档 */
static void frame_text(void)
{
    st7789_fill_rect(0, TEXT_Y, ST7789_WIDTH, 16u * TEXT_SCALE, ST7789_BLACK);

    (void)st7789_draw_string((uint16_t)s_text_x, TEXT_Y, TEXT_MSG,
                             ST7789_WHITE, ST7789_BLACK, TEXT_SCALE);

    s_text_x = (int16_t)(s_text_x - TEXT_STEP);
    if (s_text_x < -(int16_t)(sizeof(TEXT_MSG) * 8u * TEXT_SCALE)) {
        s_text_x = (int16_t)ST7789_WIDTH;
    }

    s_bytes += (uint32_t)ST7789_WIDTH * 16u * TEXT_SCALE * 2u;
    s_bytes += (uint32_t)(sizeof(TEXT_MSG) * 8u * 16u * TEXT_SCALE * TEXT_SCALE * 2u);
}

/*==============================================================================
 * 公共 API
 *============================================================================*/

int LcdDemo_Init(void)
{
    if (st7789_init() != ST7789_OK) {
        LOG_ERROR("LCD", "st7789_init failed (check gpio_cfg/spi_cfg map)");
        return -1;
    }

    (void)st7789_set_rotation(ST7789_ROT_0);
    st7789_fill(ST7789_BLACK);

    s_mode       = DEMO_FILL;
    s_fps        = 0;
    s_byte_rate  = 0;
    s_frames     = 0;
    s_bytes      = 0;
    s_stat_tick  = BM_GetTick();
    s_hud_tick   = BM_GetTick();

    SoftTimer_Init(&s_mode_timer);
    mode_enter(s_mode);
    SoftTimer_Start(&s_mode_timer, MODE_PERIOD_MS);

    hud_draw();

    LOG_INFO("LCD", "ST7789 %ux%u up, SPI1 15MHz + DMA1_CH3, demo start",
             (unsigned)ST7789_WIDTH, (unsigned)ST7789_HEIGHT);
    return 0;
}

void LcdDemo_Task(void)
{
    uint32_t now;

    /* 1. 模式轮换 */
    SoftTimer_Update(&s_mode_timer);
    if (SoftTimer_IsTimeout(&s_mode_timer)) {
        s_mode = (demo_mode_t)((s_mode + 1) % DEMO_COUNT);
        mode_enter(s_mode);
        SoftTimer_Start(&s_mode_timer, MODE_PERIOD_MS);
        LOG_INFO("LCD", "mode -> %s", s_mode_name[s_mode]);
    }

    /* 2. 渲染一帧 */
    switch (s_mode) {
        case DEMO_FILL:   frame_fill();   break;
        case DEMO_BOUNCE: frame_bounce(); break;
        case DEMO_TEXT:   frame_text();   break;
        default:                          break;
    }
    s_frames++;

    /* 3. 统计 (1s 窗口) + 状态栏 (500ms) */
    now = BM_GetTick();

    if ((uint32_t)(now - s_stat_tick) >= 1000u) {
        uint32_t dt = now - s_stat_tick;
        s_fps       = (uint16_t)((s_frames * 1000u) / dt);
        s_byte_rate = (s_bytes * 1000u) / dt;

        LOG_INFO("LCD", "%s: %u fps, %lu KB/s",
                 s_mode_name[s_mode], (unsigned)s_fps,
                 (unsigned long)(s_byte_rate / 1024u));

        s_frames    = 0;
        s_bytes     = 0;
        s_stat_tick = now;
    }

    if ((uint32_t)(now - s_hud_tick) >= HUD_PERIOD_MS) {
        hud_draw();
        s_hud_tick = now;
    }
}
