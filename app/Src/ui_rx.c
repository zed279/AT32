/**
 * @file    ui_rx.c
 * @brief   UHF 无线麦克风接收机 —— 主屏 (320 x 240 横屏, 深色工业风)
 *
 * ============================================================================
 *  一屏有什么
 * ============================================================================
 *   ┌────────────────────────────────────────────────────────────┐ 320 x 240
 *   │ (天线) UHF RX 470-960MHz  (锁) PLL LOCKED (电池)3.7V (齿轮) │ 状态栏 28px
 *   ├───────────────────────────┬────────────────────────────────┤
 *   │ CH-A            ● LINK ● PILOT │ CH-B         ● LINK ● NO PILOT │
 *   │ 520.000 MHz               │ 568.125 MHz                    │
 *   │ ─────────────────────────  │ ─────────────────────────      │
 *   │ RSSI               -62dBm │ RSSI                  -85dBm    │
 *   │ ████████████████████      │ ████████                       │
 *   │ AUDIO              -12dB  │ AUDIO                  --      │
 *   │  ▂▄▆█▇▆▅▃▂ (包络)          │  ▁▁▁▁▁▁ (无导频=无音频)          │
 *   │ ─────────────────────────  │ ─────────────────────────      │
 *   │ AFC        DIV            │ AFC        DIV                 │
 *   │ [▮]85%     ())UNMUTE      │ [▮]42%     ())MUTE             │
 *   │                           │                                │
 *   ├───────────────────────────┴────────────────────────────────┤
 *   │ ◀▶ ENCODER      PRESS MENU          HOLD PAIR              │ 提示栏 20px
 *   └────────────────────────────────────────────────────────────┘
 *
 * ============================================================================
 *  三条设计约束 (改界面之前先看)
 * ============================================================================
 *  1. **音频只在导频同步后才有显示**。没对上频 = 没有声音, 这是物理事实,
 *     所以 rx_derive() 里只要 pilot == 0 就把 audio 归一化为静音, 界面上
 *     显示 "--"、包络只剩一条基线。不要为了"好看"绕过这条。
 *
 *  2. **RSSI / AUDIO 不是 progress 控件**, 是自己画的 (rx_meter_t):
 *     - RSSI  = 8 格分段, 每格高度随机减 0~2px, 亮度从左到右递增;
 *     - AUDIO = 29 根 4px 竖条组成抛物线包络, 中间高/亮、两端矮/暗。
 *     都带"快起慢落"的包络跟随, 看起来才像示波器而不是整齐方块。
 *
 *  3. **本页没有可编辑控件, 因此不建 sgl_key_group**。旋钮直接切通道,
 *     短按/长按自己判边沿 (见 rx_input_poll), 不走 sgl_event_encoder_input。
 *     原因: 那套 API 的语义是"移动焦点 + 控件编辑", 而这里旋钮只做一件事。
 *
 * ============================================================================
 *  数据从哪来
 * ============================================================================
 *  UI_RX_SIM_DATA = 1 : 内置演示数据 (CH-A 强信号 / CH-B 弱信号无导频)。
 *                      芯片还没对频时先用它把视觉定下来, 条幅会自己动。
 *  UI_RX_SIM_DATA = 0 : 真读 KT 寄存器。每个字段读的是哪个寄存器,
 *                      见 rx_hw_read_ch() 里的注释, 与 KT06xx 知识卡一致。
 *
 *  真实数据路径的刷新率是 200ms (软件 IIC 上 8 个寄存器 x 2 通道),
 *  条幅动画独立跑 50ms, 用插值补出中间帧, 所以看着是连续的。
 */
#include "ui_rx.h"
#include "sgl_port_at32.h"
#include "bm_hal.h"
#include "logger.h"
#include "encoder.h"
#include "gpio.h"
#include "kt0656m.h"

#include <sgl.h>
#include <string.h>

/*==============================================================================
 * 0. 配置
 *============================================================================*/
/* 1 = 内置演示数据 (定视觉用);   0 = 读真实寄存器
 * !! 接上真芯片、对频成功后改成 0 !! */
#define UI_RX_SIM_DATA              0

/* CH-B 的数据从哪颗芯片来。本板有两组独立芯片 (组1/组2), 每组的
 * "通道 B" 到底映射哪一颗, 取决于硬件走线, 只有你知道:
 *   1 = 组2 的 KT0656M 主芯片 (kt0656m_dev(KT_CH_B))  <- 双路真分集: 两个独立通道
 *   0 = 组1 的 KT0650 从芯片 (kt0656m_dev_sub(KT_CH_A)) <- 单通道双天线
 * 改这一行即可, 其余代码不用动。 */
#define RX_CHB_FROM_GROUP2          1

/* RSSI 原始码 -> dBm 的显示换算。两份 datasheet 都没给绝对 dBm 标定,
 * 这里按线性映射 (0..RX_RSSI_RAW_MAX -> -100..-40dBm), 只用于条幅与读数。
 * 做完灵敏度实测后改这两个宏即可, 不要散落在代码里。 */
#define RX_RSSI_RAW_MAX             255
#define RX_RSSI_DBM_MIN            (-100)
#define RX_RSSI_DBM_MAX             (-40)

/* 包络条参数 */
#define RX_ENV_COLS                 29
#define RX_ENV_BAR_W                4
#define RX_ENV_GAP                  1

/* 时序 (ms) */
#define RX_T_READ_MS                200u    /* 读寄存器快照 */
#define RX_T_METER_MS                50u    /* 条幅动画 */
#define RX_T_TOAST_MS              1200u    /* 提示栏临时文字持续时间 */

/* 旋钮 / 按键 */
#define RX_ENC_STEPS_PER_DETENT       4
#define RX_ENC_STEP_MAX               4
#define RX_KEY_DEBOUNCE_CNT           3
#define RX_KEY_LONG_MS              500u

/*==============================================================================
 * 1. 配色 (深色工业风)
 *============================================================================*/
#define RX_C_BG       sgl_rgb(0x1A, 0x1A, 0x1A)   /* 页面底: 纯黑偏亮一点 */
#define RX_C_BAR      sgl_rgb(0x12, 0x12, 0x12)   /* 状态栏 / 提示栏底 */
#define RX_C_CARD     sgl_rgb(0x20, 0x20, 0x20)   /* 卡片底 */
#define RX_C_LINE     sgl_rgb(0x2C, 0x2C, 0x2C)   /* 分隔线 / 未选中边框 */
#define RX_C_TITLE    sgl_rgb(0x17, 0x3E, 0x6E)   /* 卡片标题栏: 深蓝 */
#define RX_C_BLUE     sgl_rgb(0x2E, 0x8B, 0xFF)   /* 选中高亮 / 强调 */
#define RX_C_TEXT     sgl_rgb(0xEA, 0xEA, 0xEA)   /* 主文字 */
#define RX_C_DIM      sgl_rgb(0x8C, 0x8C, 0x8C)   /* 次要文字 */
#define RX_C_TRACK    sgl_rgb(0x2A, 0x2A, 0x2A)   /* 条幅未点亮格 */
#define RX_C_GREEN    sgl_rgb(0x2E, 0xC4, 0x6B)
#define RX_C_YELLOW   sgl_rgb(0xEA, 0xB3, 0x08)
#define RX_C_ORANGE   sgl_rgb(0xF9, 0x73, 0x16)
#define RX_C_RED      sgl_rgb(0xEF, 0x44, 0x44)
#define RX_C_GREY     sgl_rgb(0x50, 0x50, 0x50)   /* 未激活 / 无信号 */

/*==============================================================================
 * 2. 数据快照层
 *============================================================================*/
typedef struct {
    uint32_t freq_khz;
    int16_t  rssi_dbm;
    int16_t  audio_db;
    uint8_t  rssi_pct;      /* 0..100 条幅用 */
    uint8_t  audio_pct;     /* 0..100 条幅用 */
    uint8_t  link;          /* 1 = 有载波 (RSSI 超过门限) */
    uint8_t  pilot;         /* 1 = 导频同步 (0x209 bit7) */
    uint8_t  afc;           /* 1 = AFC 在工作 (0x218 bit1) */
    uint8_t  div;           /* 1 = 真分集打开 (0x21C bit6) */
    uint8_t  muted;         /* 1 = 芯片自动静音 (0x88 bit0) */
    uint8_t  tx_batt;       /* 0..100, 0xFF = 未知 (BPSK 辅助信道) */
} rx_ch_t;

typedef struct {
    rx_ch_t  ch[UI_RX_CH_NUM];
    uint8_t  pll_locked;    /* 0x61 bit0 */
    uint16_t batt_mv;       /* 整机电池电压, mV */
} rx_state_t;

static rx_state_t s_st;
static uint32_t   s_freq_khz[UI_RX_CH_NUM] = { 520000u, 568125u };

/* 超过这个场强就算"有载波", 用来点 LINK 灯 */
#define RX_LINK_THRESHOLD_DBM      (-95)

/**
 * @brief 8 格 RSSI 的判格门限 (dBm, 从低到高)
 *
 * 分段条本身就是离散的, 用门限表比线性映射更贴近调机习惯:
 *   -97 -93 -89 -85 -80 -75 -69 -62
 *   ->  -62dBm 满 8 格, -85dBm 正好 4 格 (与需求样例一致)
 * 做完实际灵敏度标定后只改这一行, 别的地方不用动。
 */
static const int8_t s_rssi_th[8] = { -97, -93, -89, -85, -80, -75, -69, -62 };

static uint8_t rx_rssi_cells(int16_t dbm)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0u; i < 8u; i++) {
        if (dbm >= s_rssi_th[i]) {
            n = (uint8_t)(i + 1u);
        }
    }
    return n;
}

static uint8_t rx_pct(int32_t v, int32_t lo, int32_t hi)
{
    if (v <= lo) {
        return 0u;
    }
    if (v >= hi) {
        return 100u;
    }
    return (uint8_t)(((v - lo) * 100) / (hi - lo));
}

/**
 * @brief  由原始量推出条幅百分比 + 应用"没导频就没音频"规则
 * @note   规则集中在这一处, 界面其它地方不再各自判断, 免得出现
 *         "NO PILOT 但音频条还在跳"这种自相矛盾的画面
 */
static void rx_derive(rx_ch_t *c)
{
    c->rssi_pct = (uint8_t)(rx_rssi_cells(c->rssi_dbm) * 100u / 8u);

    if (c->rssi_dbm > RX_LINK_THRESHOLD_DBM) {
        c->link = 1u;
    }

    if ((c->pilot == 0u) || (c->muted != 0u)) {
        /* 没有导频 = 麦克风没对上频 = 没有声音; 被静音同理 */
        c->audio_db  = -90;
        c->audio_pct = 0u;
    }
    else {
        c->audio_pct = rx_pct(c->audio_db, -60, 0);
    }
}

/*------------------------------------------------------------------------------
 * 演示数据 —— 数值按需求文档给的样例: CH-A -62dBm / -12dB / 85%,
 * CH-B -85dBm / 无导频 / 42% / 静音。
 * 加了一点慢速漂移, 让条幅在板子上看着是活的。
 *----------------------------------------------------------------------------*/
#if (UI_RX_SIM_DATA)
static void rx_sim_read(rx_state_t *st, uint32_t now_ms)
{
    int16_t off_a = (int16_t)((now_ms / 400u) % 5u) - 2;   /* -2..+2 */
    int16_t off_b = (int16_t)((now_ms / 700u) % 3u) - 1;   /* -1..+1 */
    rx_ch_t *a = &st->ch[UI_RX_CH_A];
    rx_ch_t *b = &st->ch[UI_RX_CH_B];

    st->pll_locked = 1u;
    st->batt_mv    = 3700u;

    /* CH-A: 强信号, 导频有效, 发射机电量 85% */
    a->freq_khz = s_freq_khz[UI_RX_CH_A];
    a->rssi_dbm = (int16_t)(-62 + off_a);
    a->audio_db = (int16_t)(-12 + off_a);
    a->pilot    = 1u;
    a->afc      = 1u;
    a->div      = 1u;
    a->muted    = 0u;
    a->tx_batt  = 85u;

    /* CH-B: 弱信号, 没有导频 -> 自动静音, 音频必须为空 */
    b->freq_khz = s_freq_khz[UI_RX_CH_B];
    b->rssi_dbm = (int16_t)(-85 + off_b);
    b->audio_db = -28;              /* 有信号时的电平, 但下面 pilot=0 会归零 */
    b->pilot    = 0u;
    b->afc      = 1u;
    b->div      = 1u;
    b->muted    = 1u;
    b->tx_batt  = 42u;

    rx_derive(a);
    rx_derive(b);
}
#endif /* UI_RX_SIM_DATA */

/*------------------------------------------------------------------------------
 * 真实寄存器
 *----------------------------------------------------------------------------*/
#if (UI_RX_SIM_DATA == 0)
/* BPSK 辅助信道解出的发射机电量。驱动 reg 表里还没有这一项 (0x246/0x247
 * 只在原厂参考代码里出现), 这里就近定义, 将来并入 kt0656m_reg.h */
#define RX_REG_BURST_DATA           0x0246u

static int rx_rd(kt0656m_dev_t *d, uint16_t reg, uint8_t *out)
{
    uint8_t v;

    if ((d == NULL) || (d->ready == 0u)) {
        return -1;
    }
    if (kt0656m_reg_read(d, reg, &v) != KT_OK) {
        return -1;
    }
    *out = v;
    return 0;
}

static void rx_hw_read_ch(kt0656m_dev_t *d, rx_ch_t *c)
{
    uint8_t v;

    c->link  = 0u;
    c->pilot = 0u;
    c->afc   = 0u;
    c->div   = 0u;
    c->muted = 1u;
    c->tx_batt = 0xFFu;

    if ((d == NULL) || (d->ready == 0u)) {
        c->rssi_dbm  = RX_RSSI_DBM_MIN;
        c->audio_db  = -90;
        rx_derive(c);
        return;
    }

    /* 场强: 0x20A (主路快速 RSSI), 驱动已封装 */
    v = 0u;
    if (kt0656m_fast_rssi_get(d, &v) == KT_OK) {
        c->rssi_dbm = (int16_t)(RX_RSSI_DBM_MIN +
                                ((int32_t)v * (RX_RSSI_DBM_MAX - RX_RSSI_DBM_MIN)) / RX_RSSI_RAW_MAX);
    }
    else {
        c->rssi_dbm = RX_RSSI_DBM_MIN;
    }

    /* 导频: 0x209 bit7 */
    v = 0u;
    if (kt0656m_pilot_get(d, &v) == KT_OK) {
        c->pilot = (v != 0u) ? 1u : 0u;
    }

    /* 音频幅度: 0x209 bit3:0, 每级 6dB -> 0..-90dB */
    v = 0u;
    if (kt0656m_af_get(d, &v) == KT_OK) {
        c->audio_db = (int16_t)(((int16_t)(v & KT_AF_MSK) - 15) * 6);
    }
    else {
        c->audio_db = -90;
    }

    /* AFC 激活: 0x218 bit1 */
    if (rx_rd(d, KT_REG_AFC_EN, &v) == 0) {
        c->afc = ((v & KT_AFC_EN_VAL) != 0u) ? 1u : 0u;
    }

    /* 真分集: 0x21C bit6 */
    if (rx_rd(d, KT_REG_AUDIO_SEL, &v) == 0) {
        c->div = ((v & KT_DIVERSITY_EN) != 0u) ? 1u : 0u;
    }

    /* 自动静音: 0x88 bit0 */
    v = 1u;
    if (kt0656m_automute_get(d, &v) == KT_OK) {
        c->muted = (v != 0u) ? 1u : 0u;
    }

    /* 发射机电量: BPSK burst data, 直接给百分比 */
    if (rx_rd(d, RX_REG_BURST_DATA, &v) == 0) {
        c->tx_batt = (v <= 100u) ? v : 0xFFu;
    }

    rx_derive(c);
}

static void rx_hw_read(rx_state_t *st)
{
    kt0656m_dev_t *dev_a = kt0656m_dev(KT_CH_A);
    kt0656m_dev_t *dev_b;
    uint8_t v;

#if (RX_CHB_FROM_GROUP2)
    dev_b = kt0656m_dev(KT_CH_B);
#else
    dev_b = kt0656m_dev_sub(KT_CH_A);
#endif

    st->batt_mv = 3700u;            /* TODO: 接整机 ADC 后改成实测值 */

    st->pll_locked = 0u;
    if (rx_rd(dev_a, KT_REG_PLL_STA, &v) == 0) {
        st->pll_locked = ((v & KT_PLL_DONE) != 0u) ? 1u : 0u;
    }

    rx_hw_read_ch(dev_a, &st->ch[UI_RX_CH_A]);
    rx_hw_read_ch(dev_b, &st->ch[UI_RX_CH_B]);

    st->ch[UI_RX_CH_A].freq_khz = s_freq_khz[UI_RX_CH_A];
    st->ch[UI_RX_CH_B].freq_khz = s_freq_khz[UI_RX_CH_B];
}
#endif /* UI_RX_SIM_DATA == 0 */

/*==============================================================================
 * 3. 小工具
 *============================================================================*/
/** 按百分比压暗一个颜色 (100 = 原色) —— 比 alpha 混合快, 且不受背景色影响 */
static sgl_color_t rx_dim(sgl_color_t c, uint8_t pct)
{
    c.ch.red   = (uint8_t)((c.ch.red   * pct) / 100u);
    c.ch.green = (uint8_t)((c.ch.green * pct) / 100u);
    c.ch.blue  = (uint8_t)((c.ch.blue  * pct) / 100u);
    return c;
}

/** 确定性伪随机: 同样的 phase/i 两边画出来一样, 不依赖 rand() */
static uint8_t rx_noise(uint16_t phase, uint8_t i)
{
    uint16_t v = (uint16_t)(phase + (uint16_t)(i * 37u));

    v = (uint16_t)(v * 1103u + 12345u);
    v ^= (uint16_t)(v >> 8);
    return (uint8_t)v;
}

/** 整数转十进制, 返回写入结束位置 (SGL 的 snprintf 不支持 %03d, 只能手写) */
static char *rx_fmt_int(char *p, int32_t v)
{
    char     tmp[8];
    uint8_t  n = 0u;
    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;

    if (v < 0) {
        *p++ = '-';
    }
    do {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while ((u != 0u) && (n < 8u));
    while (n != 0u) {
        *p++ = tmp[--n];
    }
    *p = '\0';
    return p;
}

/** "520.000" —— 固定 3 位小数 */
static void rx_fmt_freq(char *buf, uint32_t khz)
{
    uint32_t mhz = khz / 1000u;
    uint32_t k   = khz % 1000u;

    if (mhz > 999u) {
        mhz = 999u;
    }
    buf[0] = (char)('0' + (mhz / 100u) % 10u);
    buf[1] = (char)('0' + (mhz / 10u) % 10u);
    buf[2] = (char)('0' + (mhz % 10u));
    buf[3] = '.';
    buf[4] = (char)('0' + (k / 100u));
    buf[5] = (char)('0' + ((k / 10u) % 10u));
    buf[6] = (char)('0' + (k % 10u));
    buf[7] = '\0';
}

/** "-62dBm" / "--" */
static void rx_fmt_value(char *buf, int16_t v, uint8_t valid, const char *unit)
{
    char *p = buf;

    if (valid) {
        p = rx_fmt_int(p, v);
        while (*unit != '\0') {
            *p++ = *unit++;
        }
        *p = '\0';
    }
    else {
        buf[0] = '-';
        buf[1] = '-';
        buf[2] = '\0';
    }
}

/** 场强/电平分级取色: 绿 -> 黄 -> 橙 -> 红 */
static sgl_color_t rx_level_color(int16_t db, int16_t weak, int16_t bad)
{
    if (db <= bad) {
        return RX_C_RED;
    }
    if (db <= weak) {
        return RX_C_ORANGE;
    }
    if (db <= (int16_t)(weak + 15)) {
        return RX_C_YELLOW;
    }
    return RX_C_GREEN;
}

/**
 * @brief 往绑定了缓冲的 label 里写一段固定文字
 * @note  直接用 sgl_label_set_text() 也能显示, 但它会把 text 指针换成
 *        调用者的常量串, 之后若有人再对这个 label 调 set_text_fmt,
 *        就会往只读段里写。统一走这里, 指针始终指向自己的缓冲。
 */
static void rx_text_copy(sgl_obj_t *lbl, char *buf, uint16_t buf_size, const char *txt)
{
    uint16_t i = 0u;

    if ((lbl == NULL) || (buf == NULL) || (buf_size == 0u)) {
        return;
    }
    while ((txt[i] != '\0') && (i < (uint16_t)(buf_size - 1u))) {
        buf[i] = txt[i];
        i++;
    }
    buf[i] = '\0';
    sgl_label_set_text(lbl, buf);
}

/*==============================================================================
 * 4. 自定义控件 A: 图标 (rx_icon)
 *
 *    SGL 自带的 icon 控件要配 fontawesome 字体, 本工程没开; led 控件只有
 *    一个圆点。这几个天线/锁/电池/齿轮/喇叭在 12px 尺度下列不出第二种画法,
 *    干脆用绘图原语自己画, 一次成型, 也不占 flash。
 *============================================================================*/
enum {
    RX_ICON_DOT = 0,
    RX_ICON_ANT,
    RX_ICON_LOCK,
    RX_ICON_BATT,
    RX_ICON_GEAR,
    RX_ICON_SPK,
    RX_ICON_TRI_L,
    RX_ICON_TRI_R
};

typedef struct {
    sgl_obj_t    obj;
    uint8_t      kind;
    uint8_t      level;     /* 电池: 0..100 */
    uint8_t      slashed;   /* 喇叭: 1 = 画一道斜杠 (静音) */
    uint8_t      alpha;
    sgl_color_t  color;
} rx_icon_t;

/** sgl_obj_t* -> rx_icon_t* (obj 是结构体首成员, 可安全反查) */
static rx_icon_t *rx_icon_of(sgl_obj_t *obj)
{
    return (obj != NULL) ? sgl_container_of(obj, rx_icon_t, obj) : NULL;
}

/**
 * @brief 画一个"基边在 xbase、尖端在 xapex"的实心三角
 * @note  逐行算宽度, 比写一个通用多边形填充省事, 也不会溢出
 */
static void rx_fill_tri(sgl_surf_t *surf, sgl_area_t *clip,
                        int16_t xbase, int16_t xapex, int16_t yt, int16_t yb,
                        sgl_color_t c)
{
    int16_t hgt = (int16_t)(yb - yt);
    int16_t ymid;
    int16_t reach;
    int16_t y;

    if (hgt < 2) {
        return;
    }
    ymid  = (int16_t)((yt + yb) / 2);
    reach = (int16_t)(hgt / 2);

    for (y = yt; y <= yb; y++) {
        int16_t d  = (y > ymid) ? (int16_t)(y - ymid) : (int16_t)(ymid - y);
        int16_t xe = (int16_t)(xbase + ((xapex - xbase) * (reach - d)) / reach);

        if (xe > xbase) {
            sgl_draw_fill_hline(surf, clip, y, xbase, xe, 1, c, SGL_ALPHA_MAX);
        }
        else {
            sgl_draw_fill_hline(surf, clip, y, xe, xbase, 1, c, SGL_ALPHA_MAX);
        }
    }
}

static void rx_icon_construct_cb(sgl_surf_t *surf, sgl_obj_t *obj, sgl_event_t *evt)
{
    rx_icon_t  *ic;
    sgl_area_t *clip;
    int16_t     x1, y1, x2, y2, w, h, cx, cy;
    sgl_color_t c;
    uint8_t     a;

    if (evt->type != SGL_EVENT_DRAW_MAIN) {
        return;
    }

    ic   = sgl_container_of(obj, rx_icon_t, obj);
    clip = &obj->area;
    x1   = obj->coords.x1;
    y1   = obj->coords.y1;
    x2   = obj->coords.x2;
    y2   = obj->coords.y2;
    w    = (int16_t)(x2 - x1 + 1);
    h    = (int16_t)(y2 - y1 + 1);
    cx   = (int16_t)(x1 + w / 2);
    cy   = (int16_t)(y1 + h / 2);
    c    = ic->color;
    a    = ic->alpha;

    if ((w <= 0) || (h <= 0)) {
        return;
    }

    switch (ic->kind) {
    case RX_ICON_DOT:
        sgl_draw_fill_circle(surf, clip, cx, cy, (int16_t)((sgl_min(w, h) - 1) / 2), c, a);
        break;

    case RX_ICON_ANT:
        /* 竖杆 + 两根斜臂 + 底座, 12x12 内是个清楚的"天线" */
        sgl_draw_fill_vline(surf, clip, cx, (int16_t)(y1 + h / 2), (int16_t)(y2 - 1), 1, c, a);
        sgl_draw_line_fill_slanted(surf, clip, (int16_t)(cx - w / 2 + 1), y1,
                                   cx, (int16_t)(y1 + h / 2), 1, c, a);
        sgl_draw_line_fill_slanted(surf, clip, (int16_t)(cx + w / 2 - 1), y1,
                                   cx, (int16_t)(y1 + h / 2), 1, c, a);
        sgl_draw_fill_hline(surf, clip, y2, (int16_t)(cx - 1), (int16_t)(cx + 1), 1, c, a);
        break;

    case RX_ICON_LOCK:
        /* 锁梁(∩ 形) + 锁体(实心圆角块) */
        sgl_draw_fill_vline(surf, clip, (int16_t)(cx - 2), (int16_t)(y1 + 1), (int16_t)(y1 + 4), 1, c, a);
        sgl_draw_fill_vline(surf, clip, (int16_t)(cx + 2), (int16_t)(y1 + 1), (int16_t)(y1 + 4), 1, c, a);
        sgl_draw_fill_hline(surf, clip, (int16_t)(y1 + 1), (int16_t)(cx - 2), (int16_t)(cx + 2), 1, c, a);
        {
            sgl_area_t body;
            body.x1 = (int16_t)(x1 + 1);
            body.y1 = (int16_t)(y1 + 5);
            body.x2 = (int16_t)(x2 - 1);
            body.y2 = y2;
            sgl_draw_fill_rect(surf, clip, &body, 1, c, a);
        }
        break;

    case RX_ICON_BATT:
        {
            sgl_area_t shell;
            sgl_area_t cap;
            sgl_area_t fill;
            int16_t    inner_w;
            int16_t    fill_w;

            shell.x1 = x1;
            shell.y1 = y1;
            shell.x2 = (int16_t)(x2 - 2);
            shell.y2 = y2;
            sgl_draw_fill_rect_border(surf, clip, &shell, 1, c, 1, a);

            cap.x1 = (int16_t)(x2 - 1);
            cap.y1 = (int16_t)(y1 + 2);
            cap.x2 = x2;
            cap.y2 = (int16_t)(y2 - 2);
            sgl_draw_fill_rect(surf, clip, &cap, 0, c, a);

            inner_w = (int16_t)(shell.x2 - shell.x1 - 3);
            fill_w  = (int16_t)((inner_w * (int16_t)ic->level) / 100);
            if (fill_w > 0) {
                fill.x1 = (int16_t)(shell.x1 + 2);
                fill.y1 = (int16_t)(y1 + 2);
                fill.x2 = (int16_t)(fill.x1 + fill_w - 1);
                fill.y2 = (int16_t)(y2 - 2);
                sgl_draw_fill_rect(surf, clip, &fill, 0, c, a);
            }
        }
        break;

    case RX_ICON_GEAR:
        /* 圆环 (中间自然露底) + 四个连到边框的齿 */
        sgl_draw_fill_ring(surf, clip, cx, cy, 2, 4, c, a);
        sgl_draw_fill_vline(surf, clip, cx, y1, (int16_t)(y1 + 3), 1, c, a);
        sgl_draw_fill_vline(surf, clip, cx, (int16_t)(y2 - 3), y2, 1, c, a);
        sgl_draw_fill_hline(surf, clip, cy, x1, (int16_t)(x1 + 3), 1, c, a);
        sgl_draw_fill_hline(surf, clip, cy, (int16_t)(x2 - 3), x2, 1, c, a);
        break;

    case RX_ICON_SPK:
        {
            sgl_area_t body;

            body.x1 = x1;
            body.y1 = (int16_t)(y1 + h / 3);
            body.x2 = (int16_t)(x1 + w / 4);
            body.y2 = (int16_t)(y2 - h / 3);
            sgl_draw_fill_rect(surf, clip, &body, 0, c, a);
            /* 喇叭口: 右边是宽边, 尖端接在 body 上 */
            rx_fill_tri(surf, clip, x2, body.x2, y1, y2, c);
            if (ic->slashed != 0u) {
                sgl_draw_line_fill_slanted(surf, clip, x1, y1, x2, y2, 1, RX_C_RED, SGL_ALPHA_MAX);
            }
        }
        break;

    case RX_ICON_TRI_L:
        rx_fill_tri(surf, clip, x2, x1, y1, y2, c);
        break;

    case RX_ICON_TRI_R:
        rx_fill_tri(surf, clip, x1, x2, y1, y2, c);
        break;

    default:
        break;
    }
}

/*==============================================================================
 * 5. 自定义控件 B: 电平条 (rx_meter)
 *
 *    两种风格:
 *      RX_MTR_SEG8 —— 分段条, 格数固定, 每格高度抖动 + 亮度递增。
 *                     RSSI 用它 (8 格 = 需求里的"8格满格")。
 *      RX_MTR_ENV  —— 抛物线包络, 由 29 根竖条组成, 中间高而亮、两端矮而暗。
 *                     AUDIO 用它, 看起来像实时音频包络。
 *
 *    pct 是目标值, disp 是显示值, 由 rx_meter_tick() 做"快起慢落"跟随,
 *    所以 50ms 的动画周期也能看出连续的跳动。
 *============================================================================*/
enum {
    RX_MTR_SEG8 = 0,
    RX_MTR_ENV
};

typedef struct {
    sgl_obj_t    obj;
    uint8_t      style;
    uint8_t      cells;     /* SEG8 的格数, 0 = 8 */
    uint8_t      pct;       /* 目标 0..100 */
    uint8_t      disp;      /* 显示 0..100 (插值) */
    uint8_t      live;      /* 1 = 加抖动 */
    uint16_t     phase;     /* 动画相位, 外部每帧 +1 */
    sgl_color_t  color;
    sgl_color_t  track;
} rx_meter_t;

static void rx_meter_draw_seg(sgl_surf_t *surf, sgl_obj_t *obj, rx_meter_t *m,
                              int16_t w, int16_t h)
{
    const uint8_t n = (m->cells != 0u) ? m->cells : 8u;
    const int16_t gap = 2;
    int16_t cw;
    int16_t xoff;
    uint8_t lit;
    uint8_t i;

    cw = (int16_t)((w - gap * (int16_t)(n - 1u)) / (int16_t)n);
    if (cw < 2) {
        return;
    }
    xoff = (int16_t)(obj->coords.x1 + (w - (cw * (int16_t)n + gap * (int16_t)(n - 1u))) / 2);
    lit  = (uint8_t)(((uint16_t)m->disp * n + 50u) / 100u);

    for (i = 0u; i < n; i++) {
        sgl_area_t r;
        int16_t    cx = (int16_t)(xoff + (int16_t)i * (cw + gap));

        r.x1 = cx;
        r.x2 = (int16_t)(cx + cw - 1);
        r.y1 = obj->coords.y1;
        r.y2 = obj->coords.y2;

        if (i < lit) {
            /* 顶端随机削掉 0~2px: 八格不再是一条齐边, 而是有起伏 */
            if (m->live != 0u) {
                r.y1 = (int16_t)(r.y1 + (int16_t)(rx_noise(m->phase, i) % 3u));
            }
            /* 亮度从左到右 62% -> 100% */
            sgl_draw_fill_rect(surf, &obj->area, &r, 1,
                               rx_dim(m->color, (uint8_t)(62u + ((uint16_t)i * 38u) / (n - 1u))),
                               SGL_ALPHA_MAX);
        }
        else {
            sgl_draw_fill_rect(surf, &obj->area, &r, 1, m->track, SGL_ALPHA_MAX);
        }
    }
}

static void rx_meter_draw_env(sgl_surf_t *surf, sgl_obj_t *obj, rx_meter_t *m,
                              int16_t w, int16_t h)
{
    const int16_t n    = RX_ENV_COLS;
    const int16_t bw   = RX_ENV_BAR_W;
    const int16_t gap  = RX_ENV_GAP;
    const int16_t span = (int16_t)(n * bw + (n - 1) * gap);
    const int16_t base = obj->coords.y2;
    int16_t x0;
    int16_t i;

    /* 基线: 静音时只剩这一条, 不留一块大黑 */
    sgl_draw_fill_hline(surf, &obj->area, base, obj->coords.x1, obj->coords.x2, 1,
                        m->track, SGL_ALPHA_MAX);

    if (m->disp == 0u) {
        /* 没有导频就没有音频, 这块区域是空的。空着一片黑会让人以为界面坏了,
         * 所以居中给一行 NO SIGNAL —— 既是状态也是说明。 */
        const char *txt = "NO SIGNAL";
        int16_t     tw  = sgl_font_get_string_width(txt, &consolas14);
        int16_t     ty  = (int16_t)(obj->coords.y1 + (h - sgl_font_get_height(&consolas14)) / 2);

        sgl_draw_string(surf, &obj->area,
                        (int16_t)(obj->coords.x1 + (w - tw) / 2), ty,
                        txt, RX_C_GREY, SGL_ALPHA_MAX, &consolas14);
        return;
    }

    if (h < 4) {
        return;
    }

    x0 = (int16_t)(obj->coords.x1 + (w - span) / 2);

    for (i = 0; i < n; i++) {
        int16_t mid;
        int32_t win;
        int32_t amp;
        int16_t jit;
        int16_t amp_h;
        sgl_area_t r;

        /* 抛物线窗口: 中间 1.0, 两端 0.40 */
        mid = (int16_t)(n - 1);
        win = ((int32_t)4 * i * (mid - i) * 100) / ((int32_t)mid * mid);
        win = 40 + (win * 60) / 100;

        amp = ((int32_t)m->disp * h * win) / 10000;

        if (m->live != 0u) {
            jit = (int16_t)(rx_noise((uint16_t)(m->phase + i * 3), (uint8_t)i) % 21u) - 10;
            amp += (amp * jit) / 100;
        }
        if (amp < 2) {
            amp = 2;
        }
        if (amp > (int32_t)(h - 1)) {
            amp = (int32_t)(h - 1);
        }
        amp_h = (int16_t)amp;

        r.x1 = (int16_t)(x0 + i * (bw + gap));
        r.x2 = (int16_t)(r.x1 + bw - 1);
        r.y1 = (int16_t)(base - amp_h + 1);
        r.y2 = base;

        /* 亮度跟着窗口走: 中间亮(100%)、两端暗(10%) */
        sgl_draw_fill_rect(surf, &obj->area, &r, 1,
                           rx_dim(m->color, (uint8_t)(10u + (uint16_t)(win * 90) / 100u)),
                           SGL_ALPHA_MAX);
    }
}

static void rx_meter_construct_cb(sgl_surf_t *surf, sgl_obj_t *obj, sgl_event_t *evt)
{
    rx_meter_t *m;
    int16_t     w;
    int16_t     h;

    if (evt->type != SGL_EVENT_DRAW_MAIN) {
        return;
    }

    m = sgl_container_of(obj, rx_meter_t, obj);
    w = (int16_t)(obj->coords.x2 - obj->coords.x1 + 1);
    h = (int16_t)(obj->coords.y2 - obj->coords.y1 + 1);
    if ((w <= 2) || (h <= 2)) {
        return;
    }

    if (m->style == RX_MTR_SEG8) {
        rx_meter_draw_seg(surf, obj, m, w, h);
    }
    else {
        rx_meter_draw_env(surf, obj, m, w, h);
    }
}

/*==============================================================================
 * 6. 控件创建小工具
 *============================================================================*/
static sgl_obj_t *rx_label(sgl_obj_t *parent, int16_t x, int16_t y,
                           int16_t w, int16_t h, const sgl_font_t *font,
                           sgl_color_t color, sgl_align_type_t align,
                           const char *text, char *buf, uint16_t buf_size)
{
    sgl_obj_t *obj = sgl_label_create(parent);

    if (obj == NULL) {
        return NULL;
    }
    sgl_obj_set_pos(obj, x, y);
    sgl_obj_set_size(obj, w, h);
    sgl_label_set_font(obj, font);          /* 先设字体: 后面算宽度要用 */
    sgl_label_set_text_color(obj, color);
    sgl_label_set_text_align(obj, align);
    sgl_label_set_text(obj, text);

    if (buf != NULL) {
        sgl_label_set_text_buffer(obj, buf, buf_size);
    }
    return obj;
}

static sgl_obj_t *rx_rect(sgl_obj_t *parent, int16_t x, int16_t y,
                          int16_t w, int16_t h, sgl_color_t color, uint16_t radius)
{
    sgl_obj_t *obj = sgl_rect_create(parent);

    if (obj == NULL) {
        return NULL;
    }
    sgl_obj_set_pos(obj, x, y);
    sgl_obj_set_size(obj, w, h);
    sgl_rect_set_color(obj, color);
    sgl_rect_set_radius(obj, radius);
    sgl_rect_set_border_width(obj, 0);      /* 默认是 2, 不关掉会把细线吃掉 */
    return obj;
}

static sgl_obj_t *rx_icon(sgl_obj_t *parent, int16_t x, int16_t y,
                          int16_t w, int16_t h, uint8_t kind, sgl_color_t color)
{
    rx_icon_t *ic = (rx_icon_t *)sgl_malloc(sizeof(rx_icon_t));

    if (ic == NULL) {
        return NULL;
    }
    memset(ic, 0, sizeof(rx_icon_t));
    sgl_obj_init(&ic->obj, parent);
    sgl_obj_set_unflexible(&ic->obj);
    sgl_obj_set_pos(&ic->obj, x, y);
    sgl_obj_set_size(&ic->obj, w, h);

    ic->obj.construct_fn = rx_icon_construct_cb;
    ic->kind  = kind;
    ic->level = 100u;
    ic->alpha = SGL_ALPHA_MAX;
    ic->color = color;
    return &ic->obj;
}

static sgl_obj_t *rx_meter(sgl_obj_t *parent, int16_t x, int16_t y,
                           int16_t w, int16_t h, uint8_t style, sgl_color_t color)
{
    rx_meter_t *m = (rx_meter_t *)sgl_malloc(sizeof(rx_meter_t));

    if (m == NULL) {
        return NULL;
    }
    memset(m, 0, sizeof(rx_meter_t));
    sgl_obj_init(&m->obj, parent);
    sgl_obj_set_unflexible(&m->obj);
    sgl_obj_set_pos(&m->obj, x, y);
    sgl_obj_set_size(&m->obj, w, h);

    m->obj.construct_fn = rx_meter_construct_cb;
    m->style = style;
    m->cells = (style == RX_MTR_SEG8) ? 8u : 0u;
    m->live  = 1u;
    m->color = color;
    m->track = RX_C_TRACK;
    return &m->obj;
}

/*==============================================================================
 * 7. 布局常量 (全部相对卡片左上角, 单位 px)
 *============================================================================*/
#define RX_W                320
#define RX_H                240

#define RX_STATUS_H         28
#define RX_HINT_Y           220

#define RX_CARD_Y           30
#define RX_CARD_H           190
#define RX_CARD_W           157
#define RX_CARD_AX          2
#define RX_CARD_BX          161

#define RX_TITLE_Y          2       /* 卡片边框 2px, 标题栏贴在里面 */
#define RX_TITLE_H          18
#define RX_CONTENT_PAD      5       /* 内容左右边距 */
#define RX_CONTENT_W        (RX_CARD_W - 2 * RX_CONTENT_PAD)   /* 147 */

#define RX_FREQ_Y           22
#define RX_FREQ_H           26
#define RX_UNIT_X           92

#define RX_DIV1_Y           50
#define RX_RSSI_TXT_Y       53
#define RX_RSSI_BAR_Y       68
#define RX_RSSI_BAR_H       14
#define RX_AUD_TXT_Y        87
#define RX_AUD_BAR_Y        102
#define RX_AUD_BAR_H        34
#define RX_DIV2_Y           140
#define RX_ST1_Y            146
#define RX_ST2_Y            164

#define RX_TXT_H            14
#define RX_VAL_W            56      /* 右侧数值列宽 ("-100dBm" 7 字符 = 46px) */

/*==============================================================================
 * 8. 控件句柄
 *============================================================================*/
typedef struct {
    sgl_obj_t *card;
    sgl_obj_t *lbl_name;
    sgl_obj_t *dot_link;
    sgl_obj_t *lbl_link;
    sgl_obj_t *dot_pilot;
    sgl_obj_t *lbl_pilot;
    sgl_obj_t *lbl_freq;
    sgl_obj_t *lbl_unit;
    sgl_obj_t *lbl_rssi;
    sgl_obj_t *lbl_rssi_v;
    sgl_obj_t *mtr_rssi;
    sgl_obj_t *lbl_aud;
    sgl_obj_t *lbl_aud_v;
    sgl_obj_t *mtr_aud;
    sgl_obj_t *lbl_afc;
    sgl_obj_t *lbl_div;
    sgl_obj_t *ico_batt;
    sgl_obj_t *lbl_batt;
    sgl_obj_t *ico_spk;
    sgl_obj_t *lbl_mute;
    char       buf_freq[10];
    char       buf_rssi[12];
    char       buf_aud[10];
    char       buf_batt[8];
    char       buf_pilot[10];
    char       buf_mute[10];
    uint8_t    disp_rssi;       /* 条幅显示值 (插值) */
    uint8_t    disp_aud;
} rx_card_t;

static rx_card_t s_card[UI_RX_CH_NUM];

/* 状态栏 / 提示栏 */
static sgl_obj_t *s_lbl_pll;
static sgl_obj_t *s_lbl_batt;
static sgl_obj_t *s_lbl_hint_mid;
static char       s_buf_pll[8];
static char       s_buf_batt[10];
static const char *s_hint_mid_txt = "PRESS MENU";       /* 提示栏中段的默认文字 */
static uint32_t    s_toast_until;

/* 交互状态 */
static uint8_t  s_sel = UI_RX_CH_A;
static void   (*s_cb_menu)(void);
static void   (*s_cb_pair)(void);
static uint32_t s_t_read;
static uint32_t s_t_meter;

/*==============================================================================
 * 9. 建页面
 *============================================================================*/
static void rx_build_status_bar(sgl_obj_t *root)
{
    (void)rx_rect(root, 0, 0, RX_W, RX_STATUS_H, RX_C_BAR, 0);
    (void)rx_rect(root, 0, RX_STATUS_H - 1, RX_W, 1, RX_C_LINE, 0);

    /* 左: 天线 + 波段 */
    (void)rx_icon(root, 8, 8, 12, 12, RX_ICON_ANT, RX_C_DIM);
    (void)rx_label(root, 24, 4, 122, 20, &consolas14, RX_C_DIM,
                   SGL_ALIGN_LEFT_MID, "UHF RX 470-960MHz", NULL, 0);

    /* 中: 锁 + PLL 状态 */
    (void)rx_icon(root, 152, 9, 10, 11, RX_ICON_LOCK, RX_C_GREEN);
    s_lbl_pll = rx_label(root, 165, 4, 68, 20, &consolas14, RX_C_GREEN,
                         SGL_ALIGN_LEFT_MID, "PLL LOCKED", s_buf_pll, sizeof(s_buf_pll));

    /* 右: 电池 + 电压 + 齿轮 */
    (void)rx_icon(root, 238, 10, 16, 9, RX_ICON_BATT, RX_C_GREEN);
    s_lbl_batt = rx_label(root, 257, 4, 32, 20, &consolas14, RX_C_TEXT,
                          SGL_ALIGN_LEFT_MID, "3.7V", s_buf_batt, sizeof(s_buf_batt));
    (void)rx_icon(root, 294, 8, 13, 13, RX_ICON_GEAR, RX_C_DIM);
}

static void rx_build_hint_bar(sgl_obj_t *root)
{
    (void)rx_rect(root, 0, RX_HINT_Y, RX_W, 1, RX_C_LINE, 0);

    /* 左: ◀ ▶ + ENCODER */
    (void)rx_icon(root, 6, 226, 6, 8, RX_ICON_TRI_L, RX_C_DIM);
    (void)rx_icon(root, 13, 226, 6, 8, RX_ICON_TRI_R, RX_C_DIM);
    (void)rx_label(root, 22, RX_HINT_Y, 60, 20, &consolas14, RX_C_DIM,
                   SGL_ALIGN_LEFT_MID, "ENCODER", NULL, 0);

    /* 中: 会变, 用缓冲 */
    s_lbl_hint_mid = rx_label(root, 100, RX_HINT_Y, 120, 20, &consolas14, RX_C_DIM,
                              SGL_ALIGN_CENTER, s_hint_mid_txt, NULL, 0);

    /* 右: HOLD PAIR */
    (void)rx_label(root, 240, RX_HINT_Y, 74, 20, &consolas14, RX_C_DIM,
                   SGL_ALIGN_RIGHT_MID, "HOLD PAIR", NULL, 0);
}

static void rx_build_card(sgl_obj_t *root, uint8_t idx, int16_t cx)
{
    rx_card_t *k = &s_card[idx];
    sgl_obj_t *card;
    char       *name = (idx == UI_RX_CH_A) ? "CH-A" : "CH-B";
    int16_t     vx;                     /* 数值列 x */
    int16_t     dot_ax, txt_lx, dot_px, txt_px;

    vx = (int16_t)(RX_CONTENT_PAD + RX_CONTENT_W - RX_VAL_W);

    /* --- 卡片底板: 边框 2px 常驻, 选中时只换颜色, 布局不跳 --- */
    card = sgl_rect_create(root);
    k->card = card;
    if (card != NULL) {
        sgl_obj_set_pos(card, cx, RX_CARD_Y);
        sgl_obj_set_size(card, RX_CARD_W, RX_CARD_H);
        sgl_rect_set_color(card, RX_C_CARD);
        sgl_rect_set_radius(card, 4);
        sgl_rect_set_border_width(card, 2);
        sgl_rect_set_border_color(card, RX_C_LINE);
    }
    if (card == NULL) {
        return;
    }

    /* --- 标题栏 --- */
    (void)rx_rect(card, 2, RX_TITLE_Y, RX_CARD_W - 4, RX_TITLE_H, RX_C_TITLE, 3);
    k->lbl_name = rx_label(card, 7, RX_TITLE_Y, 40, RX_TITLE_H, &consolas14,
                           RX_C_TEXT, SGL_ALIGN_LEFT_MID, name, NULL, 0);

    /* 右侧两颗状态灯: 位置固定, 只换颜色/文字, 两张卡对齐 */
    dot_ax = (int16_t)(RX_CARD_W - 6 - 101);   /* LINK 灯 */
    txt_lx = (int16_t)(dot_ax + 8);
    dot_px = (int16_t)(RX_CARD_W - 6 - 62);    /* PILOT 灯 ("NO PILOT" 最宽 53px) */
    txt_px = (int16_t)(dot_px + 8);

    k->dot_link = rx_icon(card, dot_ax, (int16_t)(RX_TITLE_Y + 6), 6, 6,
                          RX_ICON_DOT, RX_C_GREY);
    k->lbl_link = rx_label(card, txt_lx, RX_TITLE_Y, 30, RX_TITLE_H, &consolas14,
                           RX_C_DIM, SGL_ALIGN_LEFT_MID, "LINK", NULL, 0);
    k->dot_pilot = rx_icon(card, dot_px, (int16_t)(RX_TITLE_Y + 6), 6, 6,
                           RX_ICON_DOT, RX_C_GREY);
    k->lbl_pilot = rx_label(card, txt_px, RX_TITLE_Y, 56, RX_TITLE_H, &consolas14,
                            RX_C_DIM, SGL_ALIGN_LEFT_MID, "NO PILOT",
                            k->buf_pilot, sizeof(k->buf_pilot));

    /* --- 频点 --- */
    k->lbl_freq = rx_label(card, RX_CONTENT_PAD, RX_FREQ_Y, 96, RX_FREQ_H, &consolas24,
                           RX_C_TEXT, SGL_ALIGN_LEFT_MID, "000.000",
                           k->buf_freq, sizeof(k->buf_freq));
    k->lbl_unit = rx_label(card, RX_UNIT_X, 30, 36, 16, &consolas14, RX_C_DIM,
                           SGL_ALIGN_LEFT_MID, "MHz", NULL, 0);

    (void)rx_rect(card, RX_CONTENT_PAD, RX_DIV1_Y, RX_CONTENT_W, 1, RX_C_LINE, 0);

    /* --- RSSI --- */
    k->lbl_rssi = rx_label(card, RX_CONTENT_PAD, RX_RSSI_TXT_Y, 40, RX_TXT_H,
                           &consolas14, RX_C_DIM, SGL_ALIGN_LEFT_MID, "RSSI", NULL, 0);
    k->lbl_rssi_v = rx_label(card, vx, RX_RSSI_TXT_Y, RX_VAL_W, RX_TXT_H,
                             &consolas14, RX_C_TEXT, SGL_ALIGN_RIGHT_MID, "-62dBm",
                             k->buf_rssi, sizeof(k->buf_rssi));
    k->mtr_rssi = rx_meter(card, RX_CONTENT_PAD, RX_RSSI_BAR_Y, RX_CONTENT_W,
                           RX_RSSI_BAR_H, RX_MTR_SEG8, RX_C_GREEN);

    /* --- AUDIO --- */
    k->lbl_aud = rx_label(card, RX_CONTENT_PAD, RX_AUD_TXT_Y, 60, RX_TXT_H,
                          &consolas14, RX_C_DIM, SGL_ALIGN_LEFT_MID, "AUDIO", NULL, 0);
    k->lbl_aud_v = rx_label(card, vx, RX_AUD_TXT_Y, RX_VAL_W, RX_TXT_H,
                            &consolas14, RX_C_TEXT, SGL_ALIGN_RIGHT_MID, "-12dB",
                            k->buf_aud, sizeof(k->buf_aud));
    k->mtr_aud = rx_meter(card, RX_CONTENT_PAD, RX_AUD_BAR_Y, RX_CONTENT_W,
                          RX_AUD_BAR_H, RX_MTR_ENV, RX_C_GREEN);

    (void)rx_rect(card, RX_CONTENT_PAD, RX_DIV2_Y, RX_CONTENT_W, 1, RX_C_LINE, 0);

    /* --- 状态行 1: AFC / DIV --- */
    k->lbl_afc = rx_label(card, RX_CONTENT_PAD, RX_ST1_Y, 40, 16, &consolas14,
                          RX_C_GREEN, SGL_ALIGN_LEFT_MID, "AFC", NULL, 0);
    k->lbl_div = rx_label(card, 80, RX_ST1_Y, 40, 16, &consolas14,
                          RX_C_GREEN, SGL_ALIGN_LEFT_MID, "DIV", NULL, 0);

    /* --- 状态行 2: 发射机电量 + 静音状态 --- */
    k->ico_batt = rx_icon(card, RX_CONTENT_PAD, (int16_t)(RX_ST2_Y + 4), 16, 9,
                          RX_ICON_BATT, RX_C_GREEN);
    k->lbl_batt = rx_label(card, 24, RX_ST2_Y, 48, 16, &consolas14, RX_C_GREEN,
                           SGL_ALIGN_LEFT_MID, "85%", k->buf_batt, sizeof(k->buf_batt));
    k->ico_spk = rx_icon(card, 80, (int16_t)(RX_ST2_Y + 3), 11, 10,
                         RX_ICON_SPK, RX_C_GREEN);
    k->lbl_mute = rx_label(card, 96, RX_ST2_Y, 54, 16, &consolas14, RX_C_GREEN,
                           SGL_ALIGN_LEFT_MID, "UNMUTE",
                           k->buf_mute, sizeof(k->buf_mute));
}

static void rx_page_build(sgl_obj_t *root)
{
    if (root == NULL) {
        return;
    }
    sgl_page_set_color(root, RX_C_BG);

    rx_build_status_bar(root);
    rx_build_card(root, UI_RX_CH_A, RX_CARD_AX);
    rx_build_card(root, UI_RX_CH_B, RX_CARD_BX);
    rx_build_hint_bar(root);
}

/*==============================================================================
 * 10. 局部刷新
 *============================================================================*/
static void rx_refresh_selected(void)
{
    uint8_t i;

    for (i = 0u; i < UI_RX_CH_NUM; i++) {
        if (s_card[i].card != NULL) {
            sgl_rect_set_border_color(s_card[i].card,
                                      (i == s_sel) ? RX_C_BLUE : RX_C_LINE);
        }
    }
}

static void rx_refresh_pll(uint8_t locked)
{
    sgl_label_set_text(s_lbl_pll, locked ? "PLL LOCKED" : "PLL UNLOCK");
    sgl_label_set_text_color(s_lbl_pll, locked ? RX_C_GREEN : RX_C_RED);
}

static void rx_refresh_batt(uint16_t mv)
{
    s_buf_batt[0] = (char)('0' + (mv / 1000u) % 10u);
    s_buf_batt[1] = '.';
    s_buf_batt[2] = (char)('0' + (mv / 100u) % 10u);
    s_buf_batt[3] = 'V';
    s_buf_batt[4] = '\0';
    sgl_label_set_text(s_lbl_batt, s_buf_batt);
}

/**
 * @brief  卡片上的文字 / 状态灯颜色 (200ms 一次)
 * @note   条幅不在这里更新, 它自己 50ms 跑动画 (见 rx_meters_tick)
 */
static void rx_refresh_card(uint8_t idx, const rx_ch_t *c)
{
    rx_card_t      *k = &s_card[idx];
    rx_icon_t      *ico;
    sgl_color_t     c_link, c_pilot, c_rssi, c_aud, c_batt;
    uint8_t         valid_aud = 0u;

    if (k->card == NULL) {
        return;
    }

    /* 频点 */
    rx_fmt_freq(k->buf_freq, c->freq_khz);
    sgl_label_set_text(k->lbl_freq, k->buf_freq);

    /* LINK 灯: 绿灯=有载波且导频正常, 黄灯=有载波但导频没同步, 灰=无载波 */
    if (c->link == 0u) {
        c_link = RX_C_GREY;
    }
    else if (c->pilot != 0u) {
        c_link = RX_C_GREEN;
    }
    else {
        c_link = RX_C_YELLOW;
    }
    ico = rx_icon_of(k->dot_link);
    if (ico != NULL) {
        ico->color = c_link;
        sgl_obj_set_dirty(k->dot_link);
    }
    sgl_label_set_text_color(k->lbl_link, (c->link != 0u) ? c_link : RX_C_DIM);

    /* PILOT */
    rx_text_copy(k->lbl_pilot, k->buf_pilot, sizeof(k->buf_pilot),
                 (c->pilot != 0u) ? "PILOT" : "NO PILOT");
    c_pilot = (c->pilot != 0u) ? RX_C_GREEN : RX_C_GREY;
    ico = rx_icon_of(k->dot_pilot);
    if (ico != NULL) {
        ico->color = c_pilot;
        sgl_obj_set_dirty(k->dot_pilot);
    }
    sgl_label_set_text_color(k->lbl_pilot, (c->pilot != 0u) ? RX_C_GREEN : RX_C_DIM);

    /* RSSI 数值 */
    c_rssi = rx_level_color(c->rssi_dbm, -80, -92);
    rx_fmt_value(k->buf_rssi, c->rssi_dbm, 1u, "dBm");
    sgl_label_set_text(k->lbl_rssi_v, k->buf_rssi);
    sgl_label_set_text_color(k->lbl_rssi_v, c_rssi);

    /* AUDIO 数值: 没导频时显示 "--", 这是物理事实不是占位符 */
    valid_aud = (c->audio_pct > 0u) ? 1u : 0u;
    c_aud = valid_aud ? rx_level_color(c->audio_db, -30, -45) : RX_C_GREY;
    rx_fmt_value(k->buf_aud, c->audio_db, valid_aud, "dB");
    sgl_label_set_text(k->lbl_aud_v, k->buf_aud);
    sgl_label_set_text_color(k->lbl_aud_v, c_aud);

    /* AFC / DIV */
    sgl_label_set_text_color(k->lbl_afc, (c->afc != 0u) ? RX_C_GREEN : RX_C_GREY);
    sgl_label_set_text_color(k->lbl_div, (c->div != 0u) ? RX_C_GREEN : RX_C_GREY);

    /* 发射机电量 */
    if (c->tx_batt == 0xFFu) {
        rx_text_copy(k->lbl_batt, k->buf_batt, sizeof(k->buf_batt), "--");
        c_batt = RX_C_GREY;
    }
    else {
        char *p = rx_fmt_int(k->buf_batt, (int32_t)c->tx_batt);

        *p++ = '%';
        *p   = '\0';
        sgl_label_set_text(k->lbl_batt, k->buf_batt);
        c_batt = (c->tx_batt >= 50u) ? RX_C_GREEN
                                     : ((c->tx_batt >= 20u) ? RX_C_YELLOW : RX_C_RED);
    }
    sgl_label_set_text_color(k->lbl_batt, c_batt);
    ico = rx_icon_of(k->ico_batt);
    if (ico != NULL) {
        ico->level = (c->tx_batt == 0xFFu) ? 0u : c->tx_batt;
        ico->color = c_batt;
        sgl_obj_set_dirty(k->ico_batt);
    }

    /* 静音状态: 喇叭图标 + 文字同步 */
    rx_text_copy(k->lbl_mute, k->buf_mute, sizeof(k->buf_mute),
                 (c->muted != 0u) ? "MUTE" : "UNMUTE");
    sgl_label_set_text_color(k->lbl_mute, (c->muted != 0u) ? RX_C_RED : RX_C_GREEN);
    ico = rx_icon_of(k->ico_spk);
    if (ico != NULL) {
        ico->color   = (c->muted != 0u) ? RX_C_RED : RX_C_GREEN;
        ico->slashed = (c->muted != 0u) ? 1u : 0u;
        sgl_obj_set_dirty(k->ico_spk);
    }
}

/**
 * @brief  条幅动画 (50ms): 目标值 -> 显示值, 快起慢落
 * @note   快起慢落是电平表的物理特性: 上升要立刻跟上, 回落有阻尼,
 *         否则柱子会"闪"得很难看
 */
static void rx_meters_tick(void)
{
    uint8_t i;

    for (i = 0u; i < UI_RX_CH_NUM; i++) {
        rx_card_t  *k = &s_card[i];
        const rx_ch_t *c = &s_st.ch[i];
        rx_meter_t *mr = (rx_meter_t *)(void *)k->mtr_rssi;
        rx_meter_t *ma = (rx_meter_t *)(void *)k->mtr_aud;

        if (mr != NULL) {
            if ((uint16_t)mr->phase > 0xF000u) {
                mr->phase = 0u;
            }
            mr->phase++;
            mr->pct = c->rssi_pct;
            mr->color = rx_level_color(c->rssi_dbm, -80, -92);
            if (mr->pct > mr->disp) {
                mr->disp = mr->pct;
            }
            else if (mr->disp > mr->pct) {
                mr->disp = (uint8_t)(mr->disp - ((mr->disp - mr->pct + 3u) / 4u));
            }
            sgl_obj_set_dirty(k->mtr_rssi);
        }

        if (ma != NULL) {
            if ((uint16_t)ma->phase > 0xF000u) {
                ma->phase = 0u;
            }
            ma->phase++;
            ma->pct = c->audio_pct;
            ma->color = rx_level_color(c->audio_db, -30, -45);
            if (ma->pct > ma->disp) {
                ma->disp = ma->pct;
            }
            else if (ma->disp > ma->pct) {
                ma->disp = (uint8_t)(ma->disp - ((ma->disp - ma->pct + 2u) / 3u));
            }
            sgl_obj_set_dirty(k->mtr_aud);
        }
    }
}

/*==============================================================================
 * 11. 输入
 *============================================================================*/
static void rx_select(uint8_t ch)
{
    if (ch >= UI_RX_CH_NUM) {
        return;
    }
    if (ch != s_sel) {
        s_sel = ch;
        rx_refresh_selected();
    }
}

static void rx_toast_set(const char *txt, sgl_color_t color)
{
    if (s_lbl_hint_mid == NULL) {
        return;
    }
    sgl_label_set_text(s_lbl_hint_mid, txt);
    sgl_label_set_text_color(s_lbl_hint_mid, color);
    s_toast_until = BM_GetTick() + RX_T_TOAST_MS;
}

static void rx_hint_restore(void)
{
    if (s_lbl_hint_mid == NULL) {
        return;
    }
    sgl_label_set_text(s_lbl_hint_mid, s_hint_mid_txt);
    sgl_label_set_text_color(s_lbl_hint_mid, RX_C_DIM);
}

/* PB9: 上拉输入, 低有效, 10ms 采样 3 次消抖 */
static uint8_t s_key_stable = 1u;
static uint8_t s_key_cnt;
static uint32_t s_key_down_ms;

static void rx_key_init(void)
{
    gpio_config_t cfg;

    cfg.pin        = GPIO_PIN_KEY;
    cfg.dir        = GPIO_DIR_INPUT;
    cfg.pull       = GPIO_PU_UP;
    cfg.init_state = GPIO_HIGH;
    cfg.speed      = 0;

    (void)gpio_pin_init(&cfg);
}

static void rx_do_menu(void)
{
    if (s_cb_menu != NULL) {
        s_cb_menu();
    }
    rx_toast_set("MENU  --", RX_C_BLUE);
}

static void rx_do_pair(void)
{
    if (s_cb_pair != NULL) {
        s_cb_pair();
    }
    rx_toast_set("PAIR  --", RX_C_ORANGE);
}

/**
 * @brief 每个 UI 周期读旋钮 + 按键
 * @note  这里**不用** sgl_event_encoder_input: 本页没有焦点组,
 *        旋钮只干一件事 (切通道), 按键的长短按自己计时更直接
 */
static void rx_input_poll(uint32_t now_ms)
{
    static int16_t acc;
    int16_t delta = Encoder_ReadDelta();
    int8_t  step  = 0;
    uint8_t raw;

    acc = (int16_t)(acc + delta);
    while (acc >= RX_ENC_STEPS_PER_DETENT) {
        acc = (int16_t)(acc - RX_ENC_STEPS_PER_DETENT);
        if (step < RX_ENC_STEP_MAX) {
            step++;
        }
    }
    while (acc <= -RX_ENC_STEPS_PER_DETENT) {
        acc = (int16_t)(acc + RX_ENC_STEPS_PER_DETENT);
        if (step > -RX_ENC_STEP_MAX) {
            step--;
        }
    }
    if (step != 0) {
        /* 只有两个通道: 任意方向的转动都是在 A/B 之间切 */
        rx_select((uint8_t)(s_sel == UI_RX_CH_A ? UI_RX_CH_B : UI_RX_CH_A));
    }

    /* 按键消抖 + 长短按 */
    raw = (gpio_read(GPIO_PIN_KEY) == GPIO_LOW) ? 1u : 0u;
    if (raw != s_key_stable) {
        if (++s_key_cnt >= RX_KEY_DEBOUNCE_CNT) {
            s_key_cnt    = 0u;
            s_key_stable = raw;
            if (raw != 0u) {
                /* 按下: 记时刻 */
                s_key_down_ms = now_ms;
            }
            else {
                /* 松开: 按住了多久决定是短按还是长按 */
                uint32_t dur = (uint32_t)(now_ms - s_key_down_ms);

                if (dur >= RX_KEY_LONG_MS) {
                    rx_do_pair();
                }
                else {
                    rx_do_menu();
                }
            }
        }
    }
    else {
        s_key_cnt = 0u;
    }
}

/*==============================================================================
 * 12. 对外接口
 *============================================================================*/
static void rx_state_poll(uint32_t now_ms)
{
#if (UI_RX_SIM_DATA)
    rx_sim_read(&s_st, now_ms);
#else
    (void)now_ms;
    rx_hw_read(&s_st);
#endif

    rx_refresh_pll(s_st.pll_locked);
    rx_refresh_batt(s_st.batt_mv);
    rx_refresh_card(UI_RX_CH_A, &s_st.ch[UI_RX_CH_A]);
    rx_refresh_card(UI_RX_CH_B, &s_st.ch[UI_RX_CH_B]);
}

int UiRx_Init(void)
{
    int rc;

    rc = sgl_port_init();
    if (rc != 0) {
        LOG_ERROR("UI", "sgl_port_init failed: %d", rc);
        return rc;
    }

    /* 编码器 TMR4 必须显式初始化: 漏了它 Encoder_ReadDelta() 恒返 0,
     * 症状是"旋钮怎么转都没反应"且不报错 */
    Encoder_Init();
    rx_key_init();

    rx_page_build(sgl_screen_act());

    /* 先取一次数据, 再建第一帧, 避免上电瞬间出现 "--" 空屏 */
    s_t_read  = BM_GetTick();
    s_t_meter = s_t_read;
    rx_state_poll(s_t_read);

    /* 条幅内部状态与快照对齐, 免得第一帧从 0 慢慢爬上来 */
    {
        uint8_t i;

        for (i = 0u; i < UI_RX_CH_NUM; i++) {
            if (s_card[i].mtr_rssi != NULL) {
                ((rx_meter_t *)(void *)s_card[i].mtr_rssi)->disp = s_st.ch[i].rssi_pct;
            }
            if (s_card[i].mtr_aud != NULL) {
                ((rx_meter_t *)(void *)s_card[i].mtr_aud)->disp = s_st.ch[i].audio_pct;
            }
        }
    }

    rx_refresh_selected();
    sgl_port_refresh();

    LOG_INFO("UI", "RX main page loaded (%dx%d)", RX_W, RX_H);
    return 0;
}

void UiRx_Task(void)
{
    uint32_t now = BM_GetTick();

    rx_input_poll(now);

    if ((uint32_t)(now - s_t_read) >= RX_T_READ_MS) {
        s_t_read = now;
        rx_state_poll(now);
    }

    if ((uint32_t)(now - s_t_meter) >= RX_T_METER_MS) {
        s_t_meter = now;
        rx_meters_tick();
    }

    if ((s_toast_until != 0u) && ((int32_t)(now - s_toast_until) >= 0)) {
        s_toast_until = 0u;
        rx_hint_restore();
    }

    sgl_port_task();
}

void UiRx_SetFreq(uint8_t ch, uint32_t freq_khz)
{
    if (ch >= UI_RX_CH_NUM) {
        return;
    }
    s_freq_khz[ch] = freq_khz;
    s_st.ch[ch].freq_khz = freq_khz;

    if (s_card[ch].lbl_freq != NULL) {
        rx_fmt_freq(s_card[ch].buf_freq, freq_khz);
        sgl_label_set_text(s_card[ch].lbl_freq, s_card[ch].buf_freq);
    }
}

void UiRx_SetSelected(uint8_t ch)
{
    rx_select(ch);
}

uint8_t UiRx_GetSelected(void)
{
    return s_sel;
}

void UiRx_OnMenu(void (*cb)(void))
{
    s_cb_menu = cb;
}

void UiRx_OnPair(void (*cb)(void))
{
    s_cb_pair = cb;
}

void UiRx_Toast(const char *txt)
{
    if (txt == NULL) {
        rx_hint_restore();
        s_toast_until = 0u;
        return;
    }
    rx_toast_set(txt, RX_C_BLUE);
}
