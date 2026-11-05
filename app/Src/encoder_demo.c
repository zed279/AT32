/**
 * @file    encoder_demo.c
 * @brief   编码器验证界面实现 —— 左右转动切换屏幕底色
 *
 * @details 界面分两块:
 *            顶部 72px 状态栏 (深蓝底): 接线说明 / 累计计数 / 颜色与转向
 *            下方 248px 色块区: 每过一个机械定位点换一种颜色
 *
 *          为什么只刷色块区:
 *            15MHz SPI 下整屏一次约 82ms, 是同步阻塞的; 只刷下方区域既省时间,
 *            又能把状态栏留作"静止参照物", 一眼看出颜色确实变了.
 *
 * @note    换色判据是"累计 4 个四倍频计数 = 一个 detent", 不是"每次采样".
 *          这样既能容忍编码器静止时的抖动, 也保证快速转动时颜色按比例跳变,
 *          不会因为轮询周期内计了几十个数就只换一次颜色.
 */
#include "encoder_demo.h"
#include "encoder.h"
#include "st7789.h"
#include "logger.h"

#include <stdio.h>

/*==============================================================================
 * 布局 / 配置
 *============================================================================*/
#define HUD_H               72u
#define AREA_Y              HUD_H
#define AREA_H              (ST7789_HEIGHT - HUD_H)

#define HUD_BG              ST7789_NAVY
#define HUD_FG              ST7789_WHITE

/*==============================================================================
 * 内部状态
 *============================================================================*/
static const st7789_color_t s_color[] = {
    ST7789_RED, ST7789_GREEN, ST7789_BLUE, ST7789_YELLOW,
    ST7789_CYAN, ST7789_MAGENTA, ST7789_ORANGE, ST7789_WHITE,
};

static const char *const s_color_name[] = {
    "RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA", "ORANGE", "WHITE",
};

#define COLOR_NUM   ((int32_t)(sizeof(s_color) / sizeof(s_color[0])))

static int32_t s_idx;      /* 当前颜色序号        */
static int32_t s_acc;      /* 未满一个 detent 的余数 */
static int8_t  s_dir;      /* 最近一次转向: +1 / -1  */

/*==============================================================================
 * 内部: 绘制
 *============================================================================*/
static void area_draw(void)
{
    (void)st7789_fill_rect(0, AREA_Y, ST7789_WIDTH, AREA_H, s_color[s_idx]);
}

static void hud_draw(void)
{
    char line[24];
    const char *dir = (s_dir > 0) ? "RIGHT" : ((s_dir < 0) ? "LEFT " : "---- ");

    (void)st7789_fill_rect(0, 0, ST7789_WIDTH, HUD_H, HUD_BG);

    (void)st7789_draw_string(2, 2, "ENC TMR4 PB6=CH1 PB7=CH2",
                             ST7789_LIGHTGRAY, HUD_BG, 1);

    (void)snprintf(line, sizeof(line), "CNT %d", (int)Encoder_GetCount());
    (void)st7789_draw_string(2, 20, line, HUD_FG, HUD_BG, 2);

    (void)snprintf(line, sizeof(line), "%s  %s", s_color_name[s_idx], dir);
    (void)st7789_draw_string(2, 54, line, HUD_FG, HUD_BG, 1);
}

/*==============================================================================
 * 公共 API
 *============================================================================*/
int EncoderDemo_Init(void)
{
    Encoder_Init();

    if (st7789_init() != ST7789_OK) {
        LOG_ERROR("ENC", "st7789_init failed (check gpio_cfg/spi_cfg map)");
        return -1;
    }

    (void)st7789_set_rotation(ST7789_ROT_0);

    s_idx = 0;
    s_acc = 0;
    s_dir = 0;

    area_draw();
    hud_draw();

    LOG_INFO("ENC", "encoder ready: TMR4 CH1=PB6 CH2=PB7, %d counts/detent",
             ENC_COUNTS_PER_DETENT);
    return 0;
}

void EncoderDemo_Task(void)
{
    int32_t steps;

    /* 1. 只取增量: 计数由 TMR4 硬件完成, 轮询丢不掉步 */
    int16_t delta = Encoder_ReadDelta();
    if (delta == 0) {
        return;
    }

    /* 2. 累计到整 detent 才动作, 余数留到下一轮 */
    s_acc += delta;
    steps  = s_acc / ENC_COUNTS_PER_DETENT;
    if (steps == 0) {
        return;
    }
    s_acc -= steps * ENC_COUNTS_PER_DETENT;

    /* 3. 换色 (steps 可能 >1, 快转时颜色按实际格数跳) */
    s_dir = (steps > 0) ? 1 : -1;

    s_idx += steps;
    s_idx %= COLOR_NUM;
    if (s_idx < 0) {
        s_idx += COLOR_NUM;
    }

    area_draw();
    hud_draw();

    LOG_INFO("ENC", "%s -> %s, cnt = %d",
             (s_dir > 0) ? "right" : "left",
             s_color_name[s_idx], (int)Encoder_GetCount());
}
