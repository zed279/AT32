/**
 * @file    kt_demo.c
 * @brief   KT06xx 双组接收芯片初始化验证界面实现
 *
 * @details 两组完全独立，各自走一遍完整链路：
 *            kt0656m_power_on(ch)      拉该组 CHIP_EN（组1=PB12 / 组2=PA8）
 *            kt0656m_bus_attach(main)  该组总线 init + 等 ACK + 等 POWER_ON_FINISH
 *            kt0656m_probe(main)       读 CHIP_ID / FW / 电源状态
 *            kt0656m_init(main)        射频与音频全量配置
 *            kt0656m_tune(main, f)     设频道并等 PLL 锁定（最迟 260ms）
 *            kt0656m_bus_attach(sub)   同一根 EN 已上电，从路只需挂总线
 *
 *          失效顺序很重要：probe 必须在 init 之前，因为 init 会写一堆寄存器，
 *          芯片若根本没应答，init 只会连着报 NACK，看不出根因。
 *
 * @note    整屏重绘约 82ms（15MHz SPI 全屏），所以只在状态变化时画。
 *          失败组每 5s 重试一次，重试期间界面不闪（结果没变就不重绘）。
 */
#include "kt_demo.h"
#include "kt0656m.h"
#include "st7789.h"
#include "bm_hal.h"

#include <stdio.h>
#include <string.h>

/*==============================================================================
 * 布局
 *============================================================================*/
#define X0              2u
#define LINE_H          18u
#define TITLE_GAP       22u

/*==============================================================================
 * 界面状态
 *============================================================================*/
typedef struct {
    uint8_t addr;       /* I2C 7 位地址        */
    uint8_t scan_hit;   /* 1 = 地址有应答      */
    uint8_t id_ok;      /* 1 = CHIP_ID == 0x4B54 */
    uint8_t fw;         /* 0x010F（仅主路有效） */
    uint8_t pwr_ok;     /* 1 = POWER_ON_FINISH */
    int     attach_rc;
    int     init_rc;
    int     tune_rc;
} kt_chip_ui_t;

typedef struct {
    kt_chip_ui_t main_c;
    kt_chip_ui_t sub_c;
    int          pwr_rc;
    uint8_t      usable;    /* 1 = 主路 init + tune 都成功，不需要重试 */
} kt_grp_ui_t;

static kt_grp_ui_t s_ui[KT_CFG_CHIP_NUM];
static uint32_t    s_last_retry;

/* 每组的总线/使能脚说明（与 kt0656m_cfg.h 第 3 节对应） */
static const char *const s_grp_desc[KT_CFG_CHIP_NUM] = {
    "CH1 PB13/PB14 EN PB12",
#if (KT_CFG_CHIP_NUM > 1)
    "CH2 PA9/PA10 EN PA8",
#endif
};

/* 每组的默认工作频点（KHz） */
static uint32_t grp_freq(uint8_t ch)
{
    return (ch == KT_CH_A) ? KT_BAND_TOP_CHA : KT_BAND_TOP_CHB;
}

/*==============================================================================
 * 内部: 跑一组
 *============================================================================*/
static void chip_run(kt0656m_dev_t *dev, kt_chip_ui_t *ui, uint32_t freq_khz,
                     uint8_t do_tune)
{
    kt0656m_info_t info;
    int rc;

    ui->addr     = dev->addr;
    ui->attach_rc = KT_ERR_NOT_READY;
    ui->init_rc   = KT_ERR_NOT_READY;
    ui->tune_rc   = KT_ERR_NOT_READY;

    /* 1. 挂总线：总线 init + 等 ACK + 等芯片内部上电完成 */
    rc = kt0656m_bus_attach(dev);
    ui->attach_rc = rc;
    if (rc != KT_OK) {
        return;
    }

    /* 2. 探身份：先确认是谁，再谈配置 */
    rc = kt0656m_probe(dev, &info);
    if (rc == KT_OK) {
        ui->scan_hit = info.scan_hit;
        ui->id_ok    = info.id_ok;
        ui->fw       = info.fw_version;
        ui->pwr_ok   = info.boot_ok;
    }

    /* 3. 初始化：驱动内部按角色过滤（KT0650 没有 DC/DC、EQ、Echo、SAI 等） */
    ui->init_rc = kt0656m_init(dev);
    if (ui->init_rc != KT_OK) {
        return;
    }

    /* 4. 设频道：从路也会走（内部转 tune_sub 精简路径） */
    if (do_tune != 0u) {
        ui->tune_rc = kt0656m_tune(dev, freq_khz);
    }
}

static void grp_run(uint8_t ch)
{
    kt0656m_dev_t *main_dev;
    kt0656m_dev_t *sub_dev;
    kt_grp_ui_t   *ui;
    uint32_t       freq;

    if (ch >= KT_CFG_CHIP_NUM) {
        return;
    }

    ui       = &s_ui[ch];
    main_dev = kt0656m_dev(ch);
    sub_dev  = kt0656m_dev_sub(ch);
    freq     = grp_freq(ch);

    /* 清状态（重试时也必须从干净状态开始，否则会显示上一轮的 OK） */
    (void)memset(ui, 0, sizeof(*ui));

    /* 1. 该组强制掉电重上。
     * 用 power_cycle 而不是 power_on：power_on 是幂等的，重试时若第一次上电
     * 本身就是坏的（CHIP_EN 毛刺、芯片内部状态残留），幂等会直接返回 OK，
     * 永远修不好。这里每次都是一次干净的上电时序，代价只有 120ms。 */
    ui->pwr_rc = kt0656m_power_cycle(ch);
    if (ui->pwr_rc != KT_OK) {
        return;
    }

    /* 2. 主路 KT0656M：attach → probe → init → tune */
    if (main_dev != 0) {
        chip_run(main_dev, &ui->main_c, freq, 1u);
    }

    /* 3. 从路 KT0650：同组共用 CHIP_EN，已在上面拉高，这里只挂总线 */
    if (sub_dev != 0) {
        chip_run(sub_dev, &ui->sub_c, freq, 1u);
    }

    /* 主路能出声（init + tune 都过）就算这组可用；从路失败只影响分集效果 */
    if (ui->main_c.init_rc == KT_OK && ui->main_c.tune_rc == KT_OK) {
        ui->usable = 1u;
    }
}

/*==============================================================================
 * 内部: 绘制
 *============================================================================*/
static st7789_color_t rc_color(int rc)
{
    return (rc == KT_OK) ? ST7789_GREEN : ST7789_RED;
}

static const char *rc_brief(int rc)
{
    return (rc == KT_OK) ? "OK" : kt0656m_strerror(rc);
}

/**
 * @brief 一行三态：标签 + attach/init/tune 结果
 * @note  用固定宽度 "PWR:OK" 这种短词，避免 strerror 的长字符串把行撑出屏幕
 */
static void draw_status_line(uint16_t y, const char *name, int rc)
{
    char line[40];

    if (rc == KT_OK) {
        (void)snprintf(line, sizeof(line), "  %s:OK", name);
    } else {
        (void)snprintf(line, sizeof(line), "  %s:%s", name, rc_brief(rc));
    }
    (void)st7789_draw_string(X0, y, line, rc_color(rc), ST7789_BLACK, 1);
}

/**
 * @brief 单颗芯片的两段状态挤一行（ATC / INI），避免行数超出 320px 屏高
 * @note  失败时把错误码原样带上，方便直接看出是 NACK 还是 TIMEOUT；
 *        长度按最坏情况 "ERR_TIMEOUT" 算过，30 字符内放得下。
 */
static void draw_pair_line(uint16_t y, const char *n1, int rc1,
                           const char *n2, int rc2)
{
    char line[40];

    if (rc1 == KT_OK && rc2 == KT_OK) {
        (void)snprintf(line, sizeof(line), "  %s:OK  %s:OK", n1, n2);
        (void)st7789_draw_string(X0, y, line, ST7789_GREEN, ST7789_BLACK, 1);
        return;
    }

    /* 至少一段失败：整行标红，失败的那段带错误码，另一段照实写 */
    if (rc1 == KT_OK) {
        (void)snprintf(line, sizeof(line), "  %s:OK  %s:%s",
                       n1, n2, rc_brief(rc2));
    } else if (rc2 == KT_OK) {
        (void)snprintf(line, sizeof(line), "  %s:%s  %s:OK",
                       n1, rc_brief(rc1), n2);
    } else {
        (void)snprintf(line, sizeof(line), "  %s:%s  %s:%s",
                       n1, rc_brief(rc1), n2, rc_brief(rc2));
    }
    (void)st7789_draw_string(X0, y, line, ST7789_RED, ST7789_BLACK, 1);
}

static void draw_main_block(uint16_t *py, const kt_chip_ui_t *ui)
{
    char line[40];
    uint16_t y = *py;

    /* 身份行：地址 + CHIP_ID + 固件版本 */
    if (ui->id_ok != 0u) {
        (void)snprintf(line, sizeof(line), "M 0x%02X ID:OK FW:%02X",
                       (unsigned)ui->addr, (unsigned)ui->fw);
    } else {
        (void)snprintf(line, sizeof(line), "M 0x%02X ID:FAIL",
                       (unsigned)ui->addr);
    }
    (void)st7789_draw_string(X0, y, line,
                             (ui->id_ok != 0u) ? ST7789_WHITE : ST7789_RED,
                             ST7789_BLACK, 1);
    y += LINE_H;

    draw_pair_line(y, "ATC", ui->attach_rc, "INI", ui->init_rc);
    y += LINE_H;

    draw_status_line(y, "TUN", ui->tune_rc);
    y += LINE_H;

    *py = y;
}

static void draw_sub_block(uint16_t *py, const kt_chip_ui_t *ui)
{
    char line[40];
    uint16_t y = *py;

    /* 从路不显示 FW：KT0650 手册没有 0x010F 定义，读回来是未定义值 */
    if (ui->id_ok != 0u) {
        (void)snprintf(line, sizeof(line), "S 0x%02X ID:OK",
                       (unsigned)ui->addr);
    } else {
        (void)snprintf(line, sizeof(line), "S 0x%02X ID:FAIL",
                       (unsigned)ui->addr);
    }
    (void)st7789_draw_string(X0, y, line,
                             (ui->id_ok != 0u) ? ST7789_WHITE : ST7789_RED,
                             ST7789_BLACK, 1);
    y += LINE_H;

    draw_pair_line(y, "ATC", ui->attach_rc, "INI", ui->init_rc);
    y += LINE_H;

    *py = y;
}

static void redraw(void)
{
    uint16_t y;
    uint8_t  ch;

    (void)st7789_fill(ST7789_BLACK);

    y = 2u;
    (void)st7789_draw_string(X0, y, "KT06xx DUAL GROUP", ST7789_CYAN,
                             ST7789_BLACK, 1);
    y += TITLE_GAP;

    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        (void)st7789_draw_string(X0, y, s_grp_desc[ch], ST7789_LIGHTGRAY,
                                 ST7789_BLACK, 1);
        y += LINE_H;

        draw_main_block(&y, &s_ui[ch].main_c);
        draw_sub_block(&y, &s_ui[ch].sub_c);

        y += 4u;    /* 组间留白 */
    }

    /* 底部汇总：一眼看出还有没有需要重试的组 */
    {
        char line[40];
        uint8_t bad = 0;

        for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
            if (s_ui[ch].usable == 0u) {
                bad++;
            }
        }
        if (bad == 0u) {
            (void)snprintf(line, sizeof(line), "ALL GROUPS READY");
            (void)st7789_draw_string(X0, y, line, ST7789_GREEN,
                                     ST7789_BLACK, 1);
        } else {
            (void)snprintf(line, sizeof(line), "%u GROUP(S) RETRYING",
                           (unsigned)bad);
            (void)st7789_draw_string(X0, y, line, ST7789_YELLOW,
                                     ST7789_BLACK, 1);
        }
    }
}

/*==============================================================================
 * 公共 API
 *============================================================================*/
int KtDemo_Init(void)
{
    uint8_t  ch;

    if (st7789_init() != ST7789_OK) {
        return -1;
    }
    (void)st7789_set_rotation(ST7789_ROT_0);

    /* 先给个提示屏：下面每组要 0.5~1.5s，黑屏容易让人以为死机 */
    (void)st7789_fill(ST7789_BLACK);
    (void)st7789_draw_string(X0, 2u, "KT06xx INIT...", ST7789_YELLOW,
                             ST7789_BLACK, 1);

    (void)memset(s_ui, 0, sizeof(s_ui));
    s_last_retry = BM_GetTick();

    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        grp_run(ch);
    }

    redraw();
    return 0;
}

void KtDemo_Task(void)
{
    uint32_t now = BM_GetTick();
    uint8_t  ch;
    uint8_t  need_retry = 0;

    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        if (s_ui[ch].usable == 0u) {
            need_retry = 1u;
            break;
        }
    }
    if (need_retry == 0u) {
        return;     /* 全部就绪：本任务空转，不刷屏 */
    }

    if ((uint32_t)(now - s_last_retry) < KT_DEMO_RETRY_MS) {
        return;
    }
    s_last_retry = now;

    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        if (s_ui[ch].usable == 0u) {
            grp_run(ch);
        }
    }
    redraw();
}
