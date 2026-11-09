/**
 * @file    kt_demo.c
 * @brief   KT06xx 双通道信号测试页
 *
 * @details 两组完全独立，各自走一遍完整链路后再持续显示信号数据：
 *            kt0656m_power_cycle(ch)   该组强制掉电重上（组1=PB12 / 组2=PA8）
 *            kt0656m_bus_attach(main)  总线 init + 等 ACK + 等 POWER_ON_FINISH
 *            kt0656m_probe(main)       读 CHIP_ID / FW / 电源状态
 *            kt0656m_init(main)        射频与音频全量配置
 *            kt0656m_tune(main, f)     设频道并等 PLL 锁定
 *            kt0656m_bus_attach(sub)   同组共用 EN，从路只需挂总线
 *
 *          **界面只显示信号测试数据，不再显示初始化过程。**
 *          初始化是否成功压缩成组头尾部一个 4 字符标记，一个词就能定位阶段：
 *            OK    正常
 *            PWR!  该组 CHIP_EN 上电失败（power_cycle 没过）
 *            ATC!  总线挂载失败（无 ACK / 芯片未就绪）
 *            INI!  init 写寄存器失败
 *            TUN!  调谐失败（PLL 不锁 / VCO 校准不收敛 / 频点越界）
 *
 * @note    整屏重绘约 82ms（15MHz SPI 全屏），只在初始化或重试时画；
 *          信号数据每 200ms 定点重画 2 行（约 0.6ms），不整屏重绘。
 */
#include "kt_demo.h"
#include "kt0656m.h"
#include "st7789.h"
#include "bm_hal.h"

#include <stdio.h>
#include <string.h>

/*==============================================================================
 * 布局（240x320 竖屏，8x16 字体，每字符 8px 宽）
 *============================================================================*/
#define X0              2u
#define TITLE_Y         2u
#define LINE_PITCH      22u     /* 信号数据行间距（行高 16 + 留白 6） */

#define KT_GRP_Y0       28u     /* 第一组组头 y */
#define KT_GRP_BLOCK_H  78u     /* 每组占高：组头 22 + 数据 22 + 数据 22 + 留白 12 */

/* 组头 y 坐标。最后一行底部 = KT_GRP_Y0 + 1*78 + 44 + 16 = 166，屏高 320 充裕 */
#define KT_GRP_HDR_Y(ch)  ((uint16_t)(KT_GRP_Y0 + (uint16_t)(ch) * KT_GRP_BLOCK_H))

/*==============================================================================
 * 初始化结果（只用于决定组头标记与是否重试，不逐项显示）
 *============================================================================*/
typedef struct {
    uint8_t addr;
    uint8_t id_ok;
    uint8_t fw;
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

/*==============================================================================
 * 信号数据（每通道一份）
 *============================================================================*/
#define KT_DEMO_SIG_PERIOD_MS   200u    /* 信号刷新周期 */
#define KT_DEMO_VOLUME          20u     /* 上电音量：KT_VOLUME_BOOT = 0，
                                         * 不拉起来 AF 恒为 0，现象和"没天线"一样 */

typedef struct {
    uint8_t rssi;       /* 0x020C 窄带信道内 RSSI，1dB/LSB */
    uint8_t fast_rssi;  /* 0x020A 宽带前端 RSSI，0.5dB/LSB，automute 判据 */
    uint8_t snr;        /* 0x020D SNR，1dB/LSB */
    uint8_t af;         /* 0x0209[3:0] 鉴频输出峰值，6dB/LSB */
    uint8_t syn;        /* 0x0209 bit6 PKG_SYNC：BPSK 辅助包同步标志。
                         * 模拟 FM 麦克风不发这种包，恒为 0 属正常，
                         * **不是"解调失败"的标志** */
    uint8_t muted;      /* 0x0088 bit0 automute 状态，0 = 已解除 */
    uint8_t ok;         /* 0 = 本次读取失败 */
} kt_sig_t;

static kt_sig_t s_sig[KT_CFG_CHIP_NUM];
static uint32_t s_last_sig;

/*==============================================================================
 * 每组的工作频点（KHz）
 *
 * 与 kt0656m_cfg.h 里的 KT_BAND_TOP_CHA/B 是两回事：那两个是**合法性上界**，
 * 这两个是**实际调谐频点**。范围改宽后若继续复用 TOP_CHx，会调到 690.000
 * （频段边界）而不是工作点，所以必须分开。
 *
 * 677.025 是实测确认值：该频点下麦克风开机后 RSSI 12→48、FAST_RSSI 100→189、
 * automute 由 1 变 0，确认为有效接收点。
 *============================================================================*/
#define KT_DEMO_FREQ_CHA        659900u
#define KT_DEMO_FREQ_CHB        656550u

static uint32_t grp_freq(uint8_t ch)
{
    return (ch == KT_CH_A) ? KT_DEMO_FREQ_CHA : KT_DEMO_FREQ_CHB;
}

/*==============================================================================
 * 内部: 跑一组
 *============================================================================*/
static void chip_run(kt0656m_dev_t *dev, kt_chip_ui_t *ui, uint32_t freq_khz,
                     uint8_t do_tune)
{
    kt0656m_info_t info;
    int rc;

    ui->addr      = dev->addr;
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
        ui->id_ok = info.id_ok;
        ui->fw    = info.fw_version;
    }

    /* 3. 初始化：驱动内部按角色过滤（KT0650 没有 DC/DC、EQ、Echo、SAI 等） */
    ui->init_rc = kt0656m_init(dev);
    if (ui->init_rc != KT_OK) {
        return;
    }

    /* 4. 设频道：从路也会走（内部转 tune_sub 精简路径） */
    if (do_tune != 0u) {
        ui->tune_rc = kt0656m_tune(dev, freq_khz);

        /* 5. 拉起音量。KT_VOLUME_BOOT = 0（上电默认静音），不设就永远
         *    听不到、AF 也一直读 0 —— 这个现象和"天线没接"完全一样，
         *    是最容易误判成射频故障的一步，所以放在 init/tune 之后就做。
         *    从路 KT0650 没有音频链路，volume_set 内部 KT_GUARD_MAIN
         *    直接返回 KT_OK，不会误写。 */
        (void)kt0656m_volume_set(dev, (uint8_t)KT_DEMO_VOLUME);
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

    (void)memset(ui, 0, sizeof(*ui));

    /* memset 之后所有 rc 都是 0，而 KT_OK 正好是 0 —— 任何"提前 return"
     * 的分支都会被误判成成功。所以先统一置 NOT_READY，再由各步骤覆盖。 */
    ui->pwr_rc           = KT_ERR_NOT_READY;
    ui->main_c.attach_rc = KT_ERR_NOT_READY;
    ui->main_c.init_rc   = KT_ERR_NOT_READY;
    ui->main_c.tune_rc   = KT_ERR_NOT_READY;
    ui->sub_c.attach_rc  = KT_ERR_NOT_READY;
    ui->sub_c.init_rc    = KT_ERR_NOT_READY;
    ui->sub_c.tune_rc    = KT_ERR_NOT_READY;

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
 * 内部: 信号读取
 *============================================================================*/
/**
 * @brief 读一次指定通道的信号质量
 * @note  用 kt0656m_reg_read(0x0209) 一次拿回 AF / PKG_SYNC / PILOT 三个字段，
 *        比分别调 af_get 与 pkg_sync_poll 少一次软 IIC 传输。
 *
 *        **不要用 kt0656m_pkg_sync_poll() 判断同步**：它是边沿触发的——
 *        只有 PKG_SYNC_INT 置位时才刷新 dev->pkg_synced；信号丢失后若中断
 *        没有再次置位，缓存值会一直停在 1，看起来"始终有信号"。直接读
 *        寄存器拿到的才是当下状态。
 *
 *        软 IIC 半周期 5us，读一个寄存器约 0.4ms，本函数 5 次传输约 2ms。
 *        两通道共约 4ms，占 10ms 事件周期的 40%，可接受。
 *        单次失败不重试：200ms 后下一轮自然重来。
 */
static void sig_read(uint8_t ch)
{
    kt0656m_dev_t *dev;
    kt_sig_t      *s;
    uint8_t        v = 0u;
    int            rc;

    if (ch >= KT_CFG_CHIP_NUM) {
        return;
    }

    s   = &s_sig[ch];
    dev = kt0656m_dev(ch);
    s->ok = 0u;

    if (dev == 0) {
        return;
    }

    rc = kt0656m_rssi_get(dev, &s->rssi);
    if (rc != KT_OK) return;
    rc = kt0656m_fast_rssi_get(dev, &s->fast_rssi);
    if (rc != KT_OK) return;
    rc = kt0656m_snr_get(dev, &s->snr);
    if (rc != KT_OK) return;
    rc = kt0656m_reg_read(dev, KT_REG_SIG_STA, &v);
    if (rc != KT_OK) return;

    s->af  = (uint8_t)(v & KT_AF_MSK);
    s->syn = ((v & KT_PKG_SYNC) != 0u) ? 1u : 0u;

    rc = kt0656m_automute_get(dev, &s->muted);
    if (rc != KT_OK) return;

    s->ok = 1u;
}

/*==============================================================================
 * 内部: 绘制
 *============================================================================*/
/**
 * @brief 组头尾部标记：一个 4 字符就定位失败的阶段
 * @note  顺序即执行顺序，先失败的先返回。依赖 grp_run() 里"先统一置
 *        NOT_READY"的初始化，否则 memset 的 0 会伪装成 KT_OK。
 */
static const char *grp_stage(const kt_grp_ui_t *ui)
{
    if (ui->pwr_rc != KT_OK)           return "PWR!";
    if (ui->main_c.attach_rc != KT_OK) return "ATC!";
    if (ui->main_c.init_rc != KT_OK)   return "INI!";
    if (ui->main_c.tune_rc != KT_OK)   return "TUN!";
    return "OK";
}

/** @brief 组头：[CH1] 677.025MHz OK   —— 只在初始化/重试后画 */
static void draw_grp_header(uint8_t ch)
{
    char           line[40];
    uint16_t       y  = KT_GRP_HDR_Y(ch);
    uint32_t       f  = grp_freq(ch);
    st7789_color_t c;

    (void)snprintf(line, sizeof(line), "[CH%u] %u.%03uMHz %s",
                   (unsigned)(ch + 1u), (unsigned)(f / 1000u),
                   (unsigned)(f % 1000u), grp_stage(&s_ui[ch]));

    c = (s_ui[ch].usable != 0u) ? ST7789_CYAN : ST7789_RED;
    (void)st7789_draw_string(X0, y, line, c, ST7789_BLACK, 1);
}

/**
 * @brief 该通道的 2 行信号数据 —— 每 200ms 重画
 * @note  只在固定 y 上重画，不清屏。每行按固定宽度格式化，保证新值
 *        完全覆盖旧值，不会残留。
 *        读失败时用 "-" 占位而不是留空格：空格依赖字库里有 0x20 字形，
 *        自定义子集字库会画出乱码；而且静默定格在旧值比没有显示更误导。
 */
static void draw_grp_sig(uint8_t ch)
{
    char            line[40];
    uint16_t        y  = (uint16_t)(KT_GRP_HDR_Y(ch) + LINE_PITCH);
    const kt_sig_t *s  = &s_sig[ch];
    st7789_color_t  c;

    if (s->ok == 0u) {
        (void)st7789_draw_string(X0, y, "R:--- FR:--- S:---",
                                 ST7789_RED, ST7789_BLACK, 1);
        y = (uint16_t)(y + LINE_PITCH);
        (void)st7789_draw_string(X0, y, "AF:-- SY:- MU:- V:--",
                                 ST7789_RED, ST7789_BLACK, 1);
        return;
    }

    /* 行1：射频能量。FR 过 automute 阈值就说明"载波够强"，用绿色 */
    (void)snprintf(line, sizeof(line), "R:%03u FR:%03u S:%03u",
                   (unsigned)s->rssi, (unsigned)s->fast_rssi,
                   (unsigned)s->snr);
    c = (s->fast_rssi >= KT_FAST_RSSI_MUTE_TH) ? ST7789_GREEN
                                               : ST7789_LIGHTGRAY;
    (void)st7789_draw_string(X0, y, line, c, ST7789_BLACK, 1);
    y = (uint16_t)(y + LINE_PITCH);

    /* 行2：音频与门控。MU=0 表示芯片已解除静音（信号判定合格），绿色 */
    (void)snprintf(line, sizeof(line), "AF:%02u SY:%u MU:%u V:%02u",
                   (unsigned)s->af, (unsigned)s->syn,
                   (unsigned)s->muted, (unsigned)KT_DEMO_VOLUME);
    c = (s->muted == 0u) ? ST7789_GREEN : ST7789_YELLOW;
    (void)st7789_draw_string(X0, y, line, c, ST7789_BLACK, 1);
}

static void redraw(void)
{
    uint8_t ch;

    (void)st7789_fill(ST7789_BLACK);
    (void)st7789_draw_string(X0, TITLE_Y, "KT06xx DUAL CH TEST",
                             ST7789_CYAN, ST7789_BLACK, 1);

    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        draw_grp_header(ch);
        draw_grp_sig(ch);
    }
}

/*==============================================================================
 * 公共 API
 *============================================================================*/
int KtDemo_Init(void)
{
    uint8_t ch;

    if (st7789_init() != ST7789_OK) {
        return -1;
    }
    (void)st7789_set_rotation(ST7789_ROT_0);

    /* 先给个提示屏：下面每组要 0.5~1.5s，黑屏容易让人以为死机 */
    (void)st7789_fill(ST7789_BLACK);
    (void)st7789_draw_string(X0, TITLE_Y, "KT06xx INIT...", ST7789_YELLOW,
                             ST7789_BLACK, 1);

    (void)memset(s_ui, 0, sizeof(s_ui));
    (void)memset(s_sig, 0, sizeof(s_sig));
    s_last_retry = BM_GetTick();

    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        grp_run(ch);
    }

    /* 先读一次，否则首屏数据行会停在 "R:---" 直到下一个刷新周期 */
    for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
        sig_read(ch);
    }
    s_last_sig = BM_GetTick();

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

    /* 有组不可用：按 KT_DEMO_RETRY_MS 整组重跑。
     * redraw() 会清屏，所以重绘后必须立刻补一次信号，否则数据行要空到
     * 下一个刷新周期才恢复。 */
    if (need_retry != 0u &&
        (uint32_t)(now - s_last_retry) >= KT_DEMO_RETRY_MS) {
        s_last_retry = now;

        for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
            if (s_ui[ch].usable == 0u) {
                grp_run(ch);
            }
        }
        for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
            sig_read(ch);
        }
        s_last_sig = now;
        redraw();
        return;
    }

    /* 信号刷新：只重画两组各 2 行，不整屏重绘（整屏 82ms 会明显闪） */
    if ((uint32_t)(now - s_last_sig) >= KT_DEMO_SIG_PERIOD_MS) {
        s_last_sig = now;

        for (ch = 0; ch < KT_CFG_CHIP_NUM; ch++) {
            sig_read(ch);
            draw_grp_sig(ch);
        }
    }
}
