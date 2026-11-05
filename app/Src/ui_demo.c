/**
 * @file    ui_demo.c
 * @brief   SGL 页面 —— 板卡侧 (320 x 240 横屏 + 编码器/PB9 导航)
 *
 * ============================================================================
 *  本文件与 PC 模拟器的对应关系
 * ============================================================================
 *  设计在:  C:\Users\zed\Desktop\Github\sgl-port-windows-vscode\demo\main.c
 *
 *  那个文件被拆成两段:
 *      [A] 平台无关的页面代码  ->  ui_page_build() / ui_page_task() / 回调 / 状态
 *      [B] 平台入口 (SDL)      ->  main() + while 循环 + 键盘事件
 *
 *  本文件就是"[A] + 板卡胶水". 下面标了 [A] 的那一整段, 与模拟器 main.c
 *  里同名的两个函数**逐字相同**, 改界面只改这一段, 改完两边各粘一次即可.
 *  差异只有三处, 都是平台必需:
 *
 *      1. 文件头这个 UI_PORT_BOARD 宏  -> 打开回调里的板载 LED 指示
 *      2. 末尾 UiDemo_Init/Task       -> 初始化 + 周期任务, 替代 SDL 的 main()
 *      3. 时基与输入来源                -> BM_GetTick() / Encoder + PB9
 *                                         替代 SDL_GetTicks() / 键盘
 *
 *  所以搬运流程是:
 *      在模拟器调好 -> 复制 main.c 里 [A] 段 -> 覆盖本文件 [A] 段
 *      -> 编译 -> 烧录. 不需要改任何一行.
 *
 *  想验证两边有没有改歪, 跑:
 *      python tools/ui_port_check.py
 *
 * ============================================================================
 *  四条硬约束 (违反必然上板出问题)
 * ============================================================================
 *  1. 画布 320 x 240 (横屏). 板卡 SGL 行缓冲按 320 宽分配, x > 319 会被裁掉.
 *     切回竖屏: BSP/UI_Driver/sgl_port_at32.h 的 SGL_PORT_LANDSCAPE 改 0,
 *     同时把本文件 UI_W/UI_H 改回 240x320 并重排布局.
 *  2. 字体只有 consolas14 / consolas23 / consolas24 / consolas32 / song23(CJK).
 *  3. 会变的文字必须自己给缓冲 (static char buf[] + sgl_label_set_text_buffer +
 *     sgl_label_set_text_fmt). sgl_label_set_text() 只存指针且不设容量,
 *     直接对它 set_text_fmt 写进去得到的是空串.
 *  4. 无触摸屏: 交互必须走 sgl_key_group + sgl_event_encoder_input,
 *     不要调 sgl_event_pos_input().
 *
 *  控件可用性: 本工程跳过编译了 filebrowser / filedialog / gif / gifdec /
 *  img_ext / label_ext / avi / unzip_image / 3dvortex (都依赖文件系统或解码器),
 *  其余 61 个控件全都在.
 */
#include "ui_demo.h"
#include "sgl_port_at32.h"
#include "bm_hal.h"
#include "logger.h"
#include "encoder.h"
#include "gpio.h"

#include "at32f403a_407_board.h"    /* at32_led_toggle / LED2 */

#include <sgl.h>
#include <stdio.h>

/* 这个宏让下面 [A] 段里唯一一处板卡专属代码生效 (见 ui_btn_led_cb).
 * 模拟器上不定义它, 于是那段代码被剪掉, 两边共用同一份源码. */
#define UI_PORT_BOARD

/*==============================================================================
 * [A] 平台无关部分 —— 与模拟器 demo/main.c 的 [A] 段逐字相同
 *============================================================================*/
#define UI_W            320
#define UI_H            240
#define UI_PAD          12
#define UI_LINE_W       (UI_W - 2 * UI_PAD)         /* 296: 通栏宽度 */
#define UI_COL_RX       200                         /* 右栏起点 */
#define UI_COL_RW       (UI_W - UI_PAD - UI_COL_RX) /* 108: 右栏宽度 */
#define UI_COL_LW       (UI_COL_RX - 2 * UI_PAD)    /* 176: 左栏宽度 */

#define UI_C_BG         sgl_rgb(0x0E, 0x14, 0x1E)   /* 页面底色 (深蓝黑) */
#define UI_C_TITLE      sgl_rgb(0x4E, 0xD8, 0xFF)   /* 标题 (青) */
#define UI_C_TEXT       sgl_rgb(0xD8, 0xE0, 0xE8)   /* 正文 (浅灰) */
#define UI_C_DIM        sgl_rgb(0x7A, 0x88, 0x98)   /* 次要信息 (灰) */
#define UI_C_ACCENT     sgl_rgb(0x2E, 0xC4, 0x6B)   /* 进度条 (绿) */
#define UI_C_ACCENT2    sgl_rgb(0xFF, 0xA8, 0x2E)   /* 进度条 (橙, MODE B) */
#define UI_C_BTN        sgl_rgb(0x1F, 0x2A, 0x38)   /* 按钮底色 */
#define UI_C_BTN_TXT    sgl_rgb(0xE8, 0xF0, 0xF8)

static sgl_obj_t *s_led;
static sgl_obj_t *s_lbl_led;
static sgl_obj_t *s_lbl_bright;
static sgl_obj_t *s_slider;
static sgl_obj_t *s_btn_led;
static sgl_obj_t *s_btn_mode;
static sgl_obj_t *s_sw;
static sgl_obj_t *s_lbl_sw;
static sgl_obj_t *s_prog;
static sgl_obj_t *s_lbl_pct;
static sgl_obj_t *s_lbl_heap;
static sgl_obj_t *s_lbl_status;

/* 编码器导航组: 只有加进组的控件才会被旋转选中.
 * 顺序 = add_obj 的顺序 = 焦点移动顺序, 所以按视觉顺序加. */
static sgl_key_group_t *s_grp;

static uint8_t s_bright;
static uint8_t s_progress;
static uint8_t s_led_on;
static uint8_t s_mode;
static uint8_t s_sw_on;

/* 动态文本缓冲: 生命周期必须覆盖标签的使用期, 所以放 static */
static char s_buf_led[16];
static char s_buf_bright[24];
static char s_buf_sw[16];
static char s_buf_pct[16];
static char s_buf_heap[24];

/*------------------------------------------------------------------------------
 * 控件创建小工具
 *----------------------------------------------------------------------------*/
static sgl_obj_t *ui_label(sgl_obj_t *parent, int16_t x, int16_t y,
                           int16_t w, int16_t h, const sgl_font_t *font,
                           sgl_color_t color, sgl_align_type_t align,
                           const char *text)
{
    sgl_obj_t *obj = sgl_label_create(parent);

    if (obj == NULL) {
        return NULL;
    }
    sgl_obj_set_pos(obj, x, y);
    sgl_obj_set_size(obj, w, h);
    sgl_label_set_font(obj, font);          /* 先设字体: set_text 要用它算宽度 */
    sgl_label_set_text_color(obj, color);
    sgl_label_set_text_align(obj, align);
    sgl_label_set_text(obj, text);          /* 占位文本, 给标签一个初始脏区 */
    return obj;
}

/* 会变的标签: 建好后绑定外部缓冲, 后续用 sgl_label_set_text_fmt() 写 */
static sgl_obj_t *ui_label_dyn(sgl_obj_t *parent, int16_t x, int16_t y,
                               int16_t w, int16_t h, const sgl_font_t *font,
                               sgl_color_t color, sgl_align_type_t align,
                               char *buf, uint16_t buf_size)
{
    sgl_obj_t *obj = ui_label(parent, x, y, w, h, font, color, align, "");

    if (obj != NULL) {
        sgl_label_set_text_buffer(obj, buf, buf_size);
    }
    return obj;
}

static sgl_obj_t *ui_button(sgl_obj_t *parent, int16_t x, int16_t y,
                            int16_t w, int16_t h, const char *text,
                            void (*cb)(sgl_event_t *))
{
    sgl_obj_t *obj = sgl_button_create(parent);

    if (obj == NULL) {
        return NULL;
    }
    sgl_obj_set_pos(obj, x, y);
    sgl_obj_set_size(obj, w, h);
    sgl_button_set_radius(obj, 6);
    sgl_button_set_color(obj, UI_C_BTN);
    sgl_button_set_border_color(obj, sgl_rgb(0x3A, 0x4A, 0x60));
    sgl_button_set_border_width(obj, 1);
    sgl_button_set_text_color(obj, UI_C_BTN_TXT);
    sgl_button_set_font(obj, &consolas14);
    sgl_button_set_text(obj, text);
    sgl_obj_set_event_cb(obj, cb, NULL);
    return obj;
}

/*------------------------------------------------------------------------------
 * 局部刷新
 *----------------------------------------------------------------------------*/
static void ui_refresh_led(void)
{
    sgl_led_set_status(s_led, s_led_on ? true : false);
    sgl_label_set_text_fmt(s_lbl_led, "LED: %s", s_led_on ? "ON" : "OFF");
}

static void ui_refresh_bright(void)
{
    sgl_label_set_text_fmt(s_lbl_bright, "Brightness: %d %%", (unsigned)s_bright);
}

static void ui_refresh_sw(void)
{
    sgl_label_set_text_fmt(s_lbl_sw, "SW %s", s_sw_on ? "ON" : "OFF");
}

static void ui_refresh_prog(void)
{
    sgl_progress_set_value(s_prog, s_progress);
    sgl_label_set_text_fmt(s_lbl_pct, "PROG %d %%", (unsigned)s_progress);
}

static void ui_refresh_heap(void)
{
    sgl_mm_monitor_t mon = sgl_mm_get_monitor();

    sgl_label_set_text_fmt(s_lbl_heap, "HEAP %d/%d",
                           (unsigned)mon.used_size, (unsigned)mon.total_size);
}

/*------------------------------------------------------------------------------
 * 事件回调 —— 编码器单击 / 鼠标点击都会走到这里
 * @note  button 只对 PRESSED/RELEASED 做视觉反馈, 业务逻辑必须判 CLICKED;
 *        按住 >500ms 收到的是 LONG_CLICKED, 不是 CLICKED.
 *----------------------------------------------------------------------------*/
static void ui_btn_led_cb(sgl_event_t *e)
{
    if (e->type != SGL_EVENT_CLICKED) {
        return;
    }
    s_led_on = (uint8_t)(!s_led_on);
    ui_refresh_led();

#ifdef UI_PORT_BOARD
    at32_led_toggle(LED2);      /* 板卡专属: 点亮板载 LED2 便于确认事件到达 */
#endif
}

static void ui_btn_mode_cb(sgl_event_t *e)
{
    if (e->type != SGL_EVENT_CLICKED) {
        return;
    }
    s_mode = (uint8_t)(!s_mode);

    sgl_button_set_text(s_btn_mode, s_mode ? "MODE B" : "MODE A");
    sgl_progress_set_fill_color(s_prog, s_mode ? UI_C_ACCENT2 : UI_C_ACCENT);
    sgl_label_set_text(s_lbl_status, s_mode ? "MODE B   TURN / CLICK / HOLD"
                                            : "MODE A   TURN / CLICK / HOLD");
}

/* switch 在 PRESSED 时就已经翻转了 (控件内部行为), 这里只在 CLICKED 时同步文本 */
static void ui_sw_cb(sgl_event_t *e)
{
    if (e->type != SGL_EVENT_CLICKED || s_sw == NULL) {
        return;
    }
    s_sw_on = sgl_switch_get_status(s_sw) ? 1u : 0u;
    ui_refresh_sw();
}

/*------------------------------------------------------------------------------
 * 建页面
 * @param root 页面根对象. 模拟器传 sgl_screen_act(), 板卡同样传 sgl_screen_act()
 *
 * 布局 (320 x 240):
 *   +--------------------------------------------------+
 *   | AT32F403A SGL 320x240                            |  y=4
 *   | o LED: ON                        [   LED   ]     |  y=32
 *   | Brightness: 50 %                 [  MODE A ]     |  y=56 / 68
 *   | [========o=========]             [o ] SW ON      |  y=80 / 108
 *   | [==============                              ]   |  y=130
 *   | PROG 40 %                          HEAP 0/16384  |  y=150
 *   |              SGL FOCUS DEMO                      |  y=172
 *   |      TURN = FOCUS   CLICK = ENTER   HOLD = BACK  |  y=206
 *   +--------------------------------------------------+
 *----------------------------------------------------------------------------*/
static void ui_page_build(sgl_obj_t *root)
{
    if (root == NULL) {
        return;
    }
    sgl_page_set_color(root, UI_C_BG);

    /* 1. 标题 */
    (void)ui_label(root, UI_PAD, 4, UI_LINE_W, 22, &consolas14,
                   UI_C_TITLE, SGL_ALIGN_LEFT_MID, "AT32F403A SGL 320x240");

    /* 2. LED 指示灯 + 状态文本 */
    s_led = sgl_led_create(root);
    if (s_led != NULL) {
        sgl_obj_set_pos(s_led, UI_PAD, 34);
        sgl_obj_set_size(s_led, 14, 14);
        sgl_led_set_on_color(s_led, UI_C_ACCENT);
        sgl_led_set_off_color(s_led, sgl_rgb(0x33, 0x3C, 0x48));
        sgl_led_set_bg_color(s_led, UI_C_BG);
    }
    s_lbl_led = ui_label_dyn(root, UI_PAD + 22, 31, UI_COL_LW - 22, 20,
                             &consolas14, UI_C_TEXT, SGL_ALIGN_LEFT_MID,
                             s_buf_led, sizeof(s_buf_led));

    /* 3. 亮度: 文本 + 滑块 (滑块是 editable, 编码器要按一下才能改值) */
    s_lbl_bright = ui_label_dyn(root, UI_PAD, 56, UI_COL_LW, 18,
                                &consolas14, UI_C_DIM, SGL_ALIGN_LEFT_MID,
                                s_buf_bright, sizeof(s_buf_bright));

    s_slider = sgl_slider_create(root);
    if (s_slider != NULL) {
        sgl_obj_set_pos(s_slider, UI_PAD, 80);
        sgl_obj_set_size(s_slider, UI_COL_LW, 24);
        sgl_slider_set_fill_color(s_slider, UI_C_ACCENT);
        sgl_slider_set_track_color(s_slider, sgl_rgb(0x2A, 0x34, 0x42));
        sgl_slider_set_knob_color(s_slider, sgl_rgb(0xE8, 0xF0, 0xF8));
    }

    /* 4. 右栏: 两个按钮 */
    s_btn_led  = ui_button(root, UI_COL_RX, 32, UI_COL_RW, 30,
                           "LED",    ui_btn_led_cb);
    s_btn_mode = ui_button(root, UI_COL_RX, 68, UI_COL_RW, 30,
                           "MODE A", ui_btn_mode_cb);

    /* 5. 右栏: 开关 + 文本 (开关在 PRESSED 时翻转) */
    s_sw = sgl_switch_create(root);
    if (s_sw != NULL) {
        sgl_obj_set_pos(s_sw, UI_COL_RX, 108);
        sgl_obj_set_size(s_sw, 52, 26);
        sgl_switch_set_color(s_sw, UI_C_ACCENT);
        sgl_switch_set_bg_color(s_sw, sgl_rgb(0x2A, 0x34, 0x42));
        sgl_switch_set_knob_color(s_sw, sgl_rgb(0xE8, 0xF0, 0xF8));
        sgl_obj_set_event_cb(s_sw, ui_sw_cb, NULL);
    }
    s_lbl_sw = ui_label_dyn(root, UI_COL_RX + 60, 110, UI_COL_RW - 60, 22,
                            &consolas14, UI_C_TEXT, SGL_ALIGN_LEFT_MID,
                            s_buf_sw, sizeof(s_buf_sw));

    /* 6. 进度条 + 百分比 + 堆用量 */
    s_prog = sgl_progress_create(root);
    if (s_prog != NULL) {
        sgl_obj_set_pos(s_prog, UI_PAD, 130);
        sgl_obj_set_size(s_prog, UI_LINE_W, 14);
        sgl_progress_set_radius(s_prog, 6);
        sgl_progress_set_fill_color(s_prog, UI_C_ACCENT);
        sgl_progress_set_track_color(s_prog, sgl_rgb(0x2A, 0x34, 0x42));
    }
    s_lbl_pct = ui_label_dyn(root, UI_PAD, 150, 120, 18,
                             &consolas14, UI_C_TEXT, SGL_ALIGN_LEFT_MID,
                             s_buf_pct, sizeof(s_buf_pct));
    s_lbl_heap = ui_label_dyn(root, UI_W - UI_PAD - 140, 150, 140, 18,
                              &consolas14, UI_C_DIM, SGL_ALIGN_RIGHT_MID,
                              s_buf_heap, sizeof(s_buf_heap));

    /* 7. 中部大字 (演示 consolas24) */
    (void)ui_label(root, UI_PAD, 172, UI_LINE_W, 30, &consolas24,
                   UI_C_TITLE, SGL_ALIGN_CENTER, "SGL FOCUS DEMO");

    /* 8. 底部状态栏 */
    s_lbl_status = ui_label(root, UI_PAD, 206, UI_LINE_W, 22, &consolas14,
                            UI_C_DIM, SGL_ALIGN_CENTER,
                            "MODE A   TURN / CLICK / HOLD");

    /* 9. 状态初值 + 首次刷新 (必须在 build 末尾, 保证两边行为一致) */
    s_bright   = 50;
    s_progress = 0;
    s_led_on   = 1;
    s_mode     = 0;
    s_sw_on    = 1;

    if (s_sw != NULL) {
        sgl_switch_set_status(s_sw, true);
    }

    /* 滑块的值存在控件内部, 必须显式同步一次初值, 否则上电时停在 0 */
    if (s_slider != NULL) {
        sgl_slider_set_value(s_slider, s_bright);
    }

    ui_refresh_led();
    ui_refresh_bright();
    ui_refresh_sw();
    ui_refresh_prog();
    ui_refresh_heap();

    /* 10. 编码器导航组 —— 顺序就是旋钮的走动顺序.
     *     只把"可操作"的控件加进来; label / progress / led 是只读的, 加进来
     *     会让旋钮多停一拍, 体验变差. */
    s_grp = sgl_key_group_create(8);
    if (s_grp != NULL) {
        (void)sgl_key_group_add_obj(s_grp, s_slider);
        (void)sgl_key_group_add_obj(s_grp, s_btn_led);
        (void)sgl_key_group_add_obj(s_grp, s_btn_mode);
        (void)sgl_key_group_add_obj(s_grp, s_sw);
        sgl_key_group_load(s_grp);      /* 设为活动组; 上电后第一个被选中的是 slider */

        /* 主动点亮第一个焦点. 组刚 load 时 focused = -1, 此时第一次旋转
         * 只会"点亮焦点而不移动"(sgl_key_ensure_focus 内部逻辑), 用户会
         * 以为第一下没反应. 调一次导航: 它把第一个成员点亮后返回, 不移动. */
        sgl_key_navigate(SGL_EVENT_KEY_DOWN, true);
    }
}

/*------------------------------------------------------------------------------
 * 页面周期任务
 * @param now_ms 单调递增毫秒时基. 模拟器传 SDL_GetTicks(), 板卡传 BM_GetTick()
 * @note  用"时间戳差值"而不是 delay/定时器, 保证两边代码完全一致
 *----------------------------------------------------------------------------*/
static void ui_page_task(uint32_t now_ms)
{
    static uint32_t t_prog, t_heap, t_sync;

    /* 1. 进度条 0 -> 100 循环 */
    if ((uint32_t)(now_ms - t_prog) >= 200u) {
        t_prog = now_ms;
        s_progress = (uint8_t)((s_progress >= 100u) ? 0u : (s_progress + 2u));
        ui_refresh_prog();
    }

    /* 2. 堆用量 (低频) */
    if ((uint32_t)(now_ms - t_heap) >= 1000u) {
        t_heap = now_ms;
        ui_refresh_heap();
    }

    /* 3. 滑块的值由控件内部保存, 编码器改完不会回调业务层,
     *    所以这里周期同步一次到文本上 (100ms 足够, 不会闪) */
    if ((uint32_t)(now_ms - t_sync) >= 100u) {
        uint8_t v;

        t_sync = now_ms;
        if (s_slider != NULL) {
            v = sgl_slider_get_value(s_slider);
            if (v != s_bright) {
                s_bright = v;
                ui_refresh_bright();
            }
        }
    }
}
/*==============================================================================
 * [A] 段结束
 *============================================================================*/

/*==============================================================================
 * [B] 板卡胶水 —— 替代模拟器 main() 里的 SDL 入口与 while 循环
 *============================================================================*/

/*------------------------------------------------------------------------------
 * 编码器归一化
 *
 * Encoder_ReadDelta() 给的是**四倍频**计数: 转过一个机械定位点 (detent)
 * 通常是 ±4. 如果直接把它丢给 sgl_event_encoder_input(), 转一格会连跳 4 个
 * 控件 —— 看起来像"旋钮太灵敏/乱跳". 所以先攒够 4 个计数才算一步.
 *
 * 反过来, 如果实测转一格要拧很大角度才动, 说明你的编码器是 2 倍频或
 * 定位点更密, 把 UI_ENC_STEPS_PER_DETENT 调小 (2 或 1) 即可.
 *----------------------------------------------------------------------------*/
#define UI_ENC_STEPS_PER_DETENT     4
#define UI_ENC_STEP_MAX             8       /* 单次最多连跳 8 格, 防暴力旋转 */

/*==============================================================================
 * 输入诊断条 —— 排障专用, 定位"到底哪一环断了"
 *
 * 屏幕上会多出最底部一行:
 *      HB123 En-999 St-99 Fo-2 Ed0 K1
 *
 *      HB  UiDemo_Task 被调用的次数. 不涨 = 任务根本没跑 (或绘制链路断)
 *      En  Encoder_ReadDelta() 的原始累加 (四倍频计数). 转动时不动 = 编码器没出数
 *      St  归一化后真正送进 SGL 的步数. En 涨 St 不涨 = 归一化阈值不对
 *      Fo  s_grp->focused.  -2 = 组没建起来(堆耗尽), -1 = 还没有焦点,
 *                           0/1/2/3 = 当前焦点在组里的下标
 *      Ed  s_grp->editing.  0 = 导航态, 1 = 编辑态
 *      K   PB9 电平. 1 = 松开(上拉为高), 0 = 按下(低)
 *
 * 判读方法 (从左往右查, 第一个不动的就是断点):
 *      HB 不动            -> TASK_UI 没注册 / 调度器没跑 / 卡在刷屏
 *      HB 动 En 不动      -> 编码器硬件或 TMR4 没使能
 *      En 动 St 不动      -> UI_ENC_STEPS_PER_DETENT 太大(编码器不是 4 倍频)
 *      St 动 Fo 不变      -> s_grp 无效或 SGL 焦点逻辑异常
 *      Fo 变但屏幕无框    -> 控件不画 focus, 或脏区没刷出去
 *      K 始终 1/0 不翻转  -> 按键接线或上拉问题
 *
 * 确认无误后把 UI_DEBUG_BAR 改 0 重新编译即可去掉这一行.
 *============================================================================*/
#define UI_DEBUG_BAR                1

#if (UI_DEBUG_BAR)
static sgl_obj_t *s_dbg;
static char       s_buf_dbg[64];
static uint32_t   s_hb;          /* UiDemo_Task 心跳计数 */
static int32_t    s_enc_sum;     /* Encoder_ReadDelta() 原始累加 */
static int32_t    s_step_sum;    /* 归一化后送进 SGL 的步数累加 */
static uint8_t    s_key_lvl = 1; /* PB9 原始电平: 1 = 松开, 0 = 按下 */
#endif

/* PB9 消抖: 采样周期 10ms, 连续 3 次一致才认 (≈30ms) */
#define UI_KEY_DEBOUNCE_CNT         3

static uint8_t s_key_stable = 1;    /* 1 = 松开 (上拉为高), 0 = 按下 */
static uint8_t s_key_cnt;

static void ui_key_init(void)
{
    gpio_config_t cfg;

    cfg.pin        = GPIO_PIN_KEY;
    cfg.dir        = GPIO_DIR_INPUT;
    cfg.pull       = GPIO_PU_UP;    /* 内部上拉, 外部按键另一端接 GND */
    cfg.init_state = GPIO_HIGH;
    cfg.speed      = 0;

    (void)gpio_pin_init(&cfg);
}

/**
 * @brief 读 PB9 的消抖状态
 * @retval true = 按下 (低电平)
 */
static bool ui_key_pressed(void)
{
    uint8_t raw = (gpio_read(GPIO_PIN_KEY) == GPIO_LOW) ? 1u : 0u;

    if (raw != s_key_stable) {
        if (++s_key_cnt >= UI_KEY_DEBOUNCE_CNT) {
            s_key_stable = raw;
            s_key_cnt    = 0;
        }
    }
    else {
        s_key_cnt = 0;
    }

    return (s_key_stable != 0u) ? true : false;
}

/**
 * @brief 旋转分派: 同一个方向在"导航"和"编辑"两种上下文里, 要调不同的函数
 *
 * @details SGL 的语义天生是反的, 必须按状态分派才能两头都符合直觉:
 *
 *            | 状态 | 顺时针该发生什么 | 该调的函数 |
 *            |------|------------------|-----------|
 *            | 导航 | 焦点走到视觉上的下一个 | sgl_key_down() -> next |
 *            | 编辑 | 数值增大 (滑块右移)     | sgl_key_up()   -> +1   |
 *
 *          原因是 sgl_key_up() 在导航态走 prev(上一个), 但在编辑态把
 *          KEY_UP 发给控件, 而滑块把 KEY_UP 解释成"加一". 一个函数两种
 *          含义, 所以不能像 sgl_event_encoder_input() 那样只按 diff 正负
 *          统一调用.
 *
 * @note  编辑态从 s_grp->editing 读 —— 结构体公开定义在 sgl_event.h,
 *        且本工程只 load 了这一个组, 所以它恒等于"当前活动组".
 *        将来若加了第二个组(如弹窗), 这里要改成读活动组.
 */
static void ui_enc_dispatch(int8_t step)
{
    bool    editing = (s_grp != NULL) && (s_grp->editing != 0u);
    int8_t  n       = (step > 0) ? step : (int8_t)(-step);
    int8_t  i;

    for (i = 0; i < n; i++) {
        if (editing) {
            /* 编辑态: 顺时针 = 值 +1 */
            if (step > 0) {
                sgl_key_up();
            }
            else {
                sgl_key_down();
            }
        }
        else {
            /* 导航态: 顺时针 = 焦点下一个 */
            if (step > 0) {
                sgl_key_down();
            }
            else {
                sgl_key_up();
            }
        }
    }
}

/**
 * @brief 每个 UI 周期调用一次 (10ms): 读编码器 + 读按键, 喂给 SGL
 *
 * @note  旋转和按键分两条路走:
 *          旋转 -> ui_enc_dispatch() 自己按编辑态分派 (见上面的说明)
 *          按键 -> sgl_event_encoder_input(0, 电平) 只借用它的边沿检测
 *                  与长短按判定:
 *                    短按 (<500ms 松开) -> CLICKED / editable 控件进编辑态
 *                    长按 (>=500ms 松开) -> sgl_key_esc() 退出编辑/收起焦点
 *        diff 传 0 是因为旋转已在上面处理完, 不能让它再动一次.
 */
static void ui_input_poll(void)
{
    static int16_t acc;
    int16_t delta = Encoder_ReadDelta();    /* 带符号, 正 = 顺时针 */
    int8_t  step  = 0;

    acc += delta;

#if (UI_DEBUG_BAR)
    s_enc_sum += delta;
#endif

    while (acc >= UI_ENC_STEPS_PER_DETENT) {
        acc -= UI_ENC_STEPS_PER_DETENT;
        if (step < UI_ENC_STEP_MAX) {
            step++;
        }
    }
    while (acc <= -UI_ENC_STEPS_PER_DETENT) {
        acc += UI_ENC_STEPS_PER_DETENT;
        if (step > -UI_ENC_STEP_MAX) {
            step--;
        }
    }

    if (step != 0) {
#if (UI_DEBUG_BAR)
        s_step_sum += step;
#endif
        ui_enc_dispatch(step);
    }

    /* 按键: 只走 press/release 边沿与长短按, 旋转传 0 */
#if (UI_DEBUG_BAR)
    s_key_lvl  = (gpio_read(GPIO_PIN_KEY) == GPIO_HIGH) ? 1u : 0u;
#endif
    sgl_event_encoder_input(0, ui_key_pressed());
}

#if (UI_DEBUG_BAR)
/* 数值限幅: SGL 的 mini printf 不支持宽度修饰符 (写 "%+05d" 会原样打出来),
 * 只能靠钳位保证这一行永远不超过 320 宽 */
static int ui_dbg_clamp(int v)
{
    if (v >  9999) { return  9999; }
    if (v < -9999) { return -9999; }
    return v;
}

static void ui_dbg_create(sgl_obj_t *root)
{
    s_dbg = ui_label_dyn(root, 0, 224, UI_W, 16, &consolas14,
                         UI_C_ACCENT2, SGL_ALIGN_LEFT_MID,
                         s_buf_dbg, sizeof(s_buf_dbg));
}

static void ui_dbg_refresh(void)
{
    /* -2 是"组指针为空"的专用标记: 组建不起来时 focused 无从谈起,
     * 跑不上去的旋钮和按键就都源于这一条 */
    int fo = (s_grp != NULL) ? (int)s_grp->focused : -2;
    int ed = (s_grp != NULL) ? (int)s_grp->editing : -1;

    if (s_dbg == NULL) {
        return;
    }

    sgl_label_set_text_fmt(s_dbg, "HB%d En%d St%d Fo%d Ed%d K%d",
                           (int)(s_hb & 0x3FFu),
                           ui_dbg_clamp((int)s_enc_sum),
                           ui_dbg_clamp((int)s_step_sum),
                           fo, ed, (int)s_key_lvl);
}
#endif

int UiDemo_Init(void)
{
    int rc = sgl_port_init();       /* 里面会做 st7789_init + sgl_init + 清屏 */

    if (rc != 0) {
        LOG_ERROR("UI", "sgl_port_init failed: %d", rc);
        return rc;
    }

    /* 编码器 TMR4: 必须在第一次 ui_input_poll() 之前初始化.
     *
     * !! 这行以前是漏的 !!  origin: encoder_demo.c 被停用后 (main.c 里注释掉了
     * Encoder_Demo_Init), 全工程就再没有地方调用 Encoder_Init(), 于是 TMR4
     * 的时钟和编码器模式从来没人使能, tmr_counter_value_get(TMR4) 恒为复位值
     * 0, Encoder_ReadDelta() 永远返回 0 —— 症状是"旋钮怎么转都没反应",
     * 而且不报任何错 (读的是未使能外设的寄存器, 安静返回 0). */
    Encoder_Init();

    /* PB9 必须在页面建好之前初始化: 第一帧之后就可能开始读按键 */
    ui_key_init();

    /* sgl_init() 已经建好默认页面并置为 active, 直接在上面摆控件即可.
     * 不要用 sgl_obj_create(NULL) 再建一页: 那样会多占一份页面内存,
     * 而且模拟器那边用的是 sgl_screen_act(), 两边就对不上了. */
    ui_page_build(sgl_screen_act());

#if (UI_DEBUG_BAR)
    /* 诊断条必须建在 ui_page_build() 之后: 它要用 COVER 的方式压在最下面,
     * 建在前面会被后面创建的控件盖住 */
    ui_dbg_create(sgl_screen_act());
#endif

    /* 立刻出第一帧, 不让用户看到黑屏 */
    sgl_port_refresh();

    LOG_INFO("UI", "SGL page loaded (%dx%d)", UI_W, UI_H);
    return 0;
}

void UiDemo_Task(void)
{
    /* 1. 输入优先: 先把旋钮/按键的积压事件送进 SGL 队列,
     *    本轮 sgl_port_task() 就能画出来, 手感最跟手. */
    ui_input_poll();

#if (UI_DEBUG_BAR)
    /* 心跳 + 诊断条: 100ms 刷一次, 别让它抢 SPI 带宽.
     * s_hb 每 10ms +1, 也就是 1 秒涨 100, 不动就是任务没跑. */
    {
        static uint32_t t_dbg;
        uint32_t now = BM_GetTick();

        s_hb++;
        if ((uint32_t)(now - t_dbg) >= 100u) {
            t_dbg = now;
            ui_dbg_refresh();
        }
    }
#endif

    /* 2. 推进 SGL: 喂时基 + 事件队列 + 控件动画 + 刷屏.
     *    对应模拟器里的 sgl_task_handler() + sgl_timer_handler(). */
    sgl_port_task();

    /* 3. 页面自己的周期逻辑, 与模拟器传 SDL_GetTicks() 完全等价 */
    ui_page_task(BM_GetTick());
}
