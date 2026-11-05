# SGL 界面开发手册 —— 横竖屏 / 页面 / 控件 / 编码器与按键

> 适用工程：`C:\Users\zed\Desktop\AT32F403A_Project`
> 硬件：AT32F403A + ST7789（240×320，SPI1 @15MHz）
> 输入：**1 个旋转编码器**（TMR4 硬件正交解码，PB6=B 相 / PB7=A 相）+ **1 个按键**（PB9，上拉输入，低有效）
> 无触摸屏
> SGL 版本：`third_party/sgl`（与 PC 模拟器 `sgl-port-windows-vscode` 源码逐字节相同）

---

## 0. 快速索引

| 想做的事 | 看哪里 | 只改这里就够 |
|---|---|---|
| 竖屏改横屏（或改回来） | §1.2 | `BSP/UI_Driver/sgl_port_at32.h` → `SGL_PORT_LANDSCAPE` |
| 页面换个背景色 / 加控件 | §2、§3 | `app/Src/ui_demo.c` 的 `ui_page_build()` |
| 新增一个可被旋钮选中的控件 | §4.2 | `ui_page_build()` 末尾 `sgl_key_group_add_obj()` |
| 旋钮转一格跳 4 格 | §4.1 | `ui_demo.c` → `UI_ENC_STEPS_PER_DETENT` |
| 旋钮方向反了 | §4.6 | `step` 取反，或反序 add_obj |
| 按键按下没反应 | §4.4 | PB9 是否在 `gpio_cfg.h` 登记 + `ui_key_init()` |
| 文字显示成 `%u %` | §3.2 | 只能用 `%d`，不支持 `%u` |
| 界面和模拟器不一致 | §5 | `python tools/ui_port_check.py` |

当前工程状态：**已经是横屏 320×240，页面 14 个控件，编码器导航已接好**。模拟器同款页面可运行验证。

---

## 1. 横竖屏切换

### 1.1 旋转发生在哪一层

ST7789 支持 4 个显示方向，靠 `MADCTL (0x36)` 寄存器控制。**旋转在屏幕硬件内部完成**，MCU 送进去的像素坐标始终是"当前方向的逻辑坐标"。

```
业务代码 (ui_demo.c)          SGL (third_party/sgl)         BSP (st7789.c)
     x=0..319  ──────────►   surf 坐标 = 屏幕坐标   ──────►  CASET/RASET + RAMWR
     y=0..239                不感知方向                      MADCTL 决定物理扫描方向
```

本工程选择**硬件旋转**：SGL 侧 `CONFIG_SGL_FBDEV_ROTATION` 恒为 0，方向只在 `st7789_set_rotation()` 里体现。

本 BSP 的四个方向（`BSP/LCD_Driver/st7789.c`）：

| 枚举 | MADCTL | 逻辑分辨率 | 说明 |
|---|---|---|---|
| `ST7789_ROT_0` | 0x00 | 240 × 320 | 竖屏，排线在下 |
| `ST7789_ROT_90` | 0x60 | **320 × 240** | 横屏（本工程当前） |
| `ST7789_ROT_180` | 0xC0 | 240 × 320 | 竖屏倒置 |
| `ST7789_ROT_270` | 0xA0 | 320 × 240 | 横屏倒置 |

> **最容易踩的坑**：`ST7789_ROT_90` 不是"转 90° 变竖屏"，它在本 BSP 里就是横屏 320×240。想要竖屏请用 `ROT_0`。上一版就是因为传了 `ROT_90` 而代码里按 240 分配缓冲，宽度检查 `320 > 240` 直接返回 -2，屏幕全黑、LED 闪 2 次。

### 1.2 只改一个宏

`BSP/UI_Driver/sgl_port_at32.h`：

```c
/* 0 = 竖屏 240x320, 1 = 横屏 320x240 */
#define SGL_PORT_LANDSCAPE      1
```

文件里已经把它展开成三个量，`sgl_port_init()` 和页面代码都用这一份：

```c
#if (SGL_PORT_LANDSCAPE != 0)
#  define SGL_PORT_WIDTH        320
#  define SGL_PORT_HEIGHT       240
#  define SGL_PORT_LCD_ROT      ST7789_ROT_90
#else
#  define SGL_PORT_WIDTH        240
#  define SGL_PORT_HEIGHT       320
#  define SGL_PORT_LCD_ROT      ST7789_ROT_0
#endif
```

改完 **必须全量重编**（`UV4 -r`），增量编译偶尔不会重新链接 SGL 分组：

```bash
cd mdk_v5
"C:/Keil_v5/UV4/UV4.exe" -r AT32F403A_Project.uvprojx -j0 -o build.log
```

### 1.3 为什么不能改 `CONFIG_SGL_FBDEV_ROTATION`

SGL 自己也能软件旋转，但代价是三重浪费，且会和硬件旋转叠加成"转两次"：

| 代码位置 | 行为 | 代价 |
|---|---|---|
| `core/sgl_core.c` `sgl_fbdev_register()` | 90/270 时交换 `xres/yres` | 你传的 320×240 会变成 240×320 |
| `core/sgl_core.c` `sgl_core_ctx_init()` | `sgl_malloc(buffer_size)` 分配旋转缓冲 | **多占 25.6KB SRAM** |
| `include/sgl_core.h` `sgl_fbdev_flush_area()` | 每个脏区逐像素搬运 + 旋转 | 每帧多一次全量内存搬运 |
| `core/sgl_event.c` `sgl_event_pos_input()` | 触摸坐标再变换一次 | 坐标错位 |

**结论：`CONFIG_SGL_FBDEV_ROTATION` 永远保持 0**，`sgl_port_at32.c` 里还有 `#error` 守着这一条。

### 1.4 横屏后必须同步的三处

| # | 位置 | 竖屏 | 横屏 | 谁负责 |
|---|---|---|---|---|
| 1 | 行缓冲宽度 | 240 | 320 | 自动（`SGL_PORT_BUF_WIDTH = SGL_PORT_WIDTH`） |
| 2 | 页面布局 | 240×320 | 320×240 | **手工重排**（`UI_W/UI_H` + 每个坐标） |
| 3 | 模拟器窗口 | 240×320 | 320×240 | `demo/sgl_port_sdl2.c` |

第 3 处是模拟器的，容易忘。你这次就是只改了模拟器 `demo/main.c` 的 `UI_W/UI_H`，窗口还是 240×320，所以页面被裁成左上角一块。三处对照：

```c
/* 板卡 */ BSP/UI_Driver/sgl_port_at32.h   #define SGL_PORT_LANDSCAPE 1
/* 模拟器 */ demo/sgl_port_sdl2.c           CONFIG_SGL_PANEL_WIDTH  320
                                            CONFIG_SGL_PANEL_HEIGHT 240
/* 两边页面 */ ui_demo.c / demo/main.c      #define UI_W 320 / UI_H 240
```

`tools/ui_port_check.py` 会自己读 `sgl_port_at32.h` 的方向来决定校验画布，不用手改脚本。

### 1.5 横屏布局怎么写

横屏的黄金尺寸是 **320×240**，减掉左右各 12 的边距，有效宽度 296。推荐做成"左栏 + 右栏"：

```c
#define UI_W            320
#define UI_H            240
#define UI_PAD          12
#define UI_LINE_W       (UI_W - 2 * UI_PAD)         /* 296 通栏 */
#define UI_COL_RX       200                         /* 右栏起点 */
#define UI_COL_RW       (UI_W - UI_PAD - UI_COL_RX) /* 108 右栏宽 */
#define UI_COL_LW       (UI_COL_RX - 2 * UI_PAD)    /* 176 左栏宽 */
```

本工程页面实测布局（`ui_demo.c`）：

```
┌──────────────────────────────────────────────────┐ 320×240
│ AT32F403A SGL 320x240                            │ y=4   h=22
│ ● LED: ON                        ┌────────────┐  │ y=32
│ Brightness: 53 %                 │    LED     │  │ y=56 / 32
│ ┌───────────o──────────┐         ├────────────┤  │ y=80
│ │                      │         │   MODE A   │  │ y=68
│ └──────────────────────┘         └────────────┘  │
│                                  ┌────┐ SW ON    │ y=108
│ ████████████████████████░░░░░░░  └────┘          │ y=130
│ PROG 28 %                      HEAP 1832/16384   │ y=150
│                SGL FOCUS DEMO                    │ y=172 (consolas24)
│      MODE A   TURN / CLICK / HOLD                │ y=206
└──────────────────────────────────────────────────┘
```

**布局不用写死两套**。如果只是竖屏改横屏，把宏换掉、把纵向排列改成左右分栏即可；要同时支持两种方向，就在 `ui_page_build()` 里用 `#if` 分两段坐标（不推荐，维护成本翻倍）。

### 1.6 改横屏后的资源账

| 项 | 竖屏 240×320 | 横屏 320×240 | 说明 |
|---|---|---|---|
| 行缓冲 | 240×40×2 = **19.2KB** | 320×40×2 = **25.6KB** | `SGL_PORT_BUF_LINES = 40` |
| ZI-data（实测） | 38860 | **45276** | 差 6416 B，正好是缓冲差 |
| 全屏像素 | 76800 | 76800 | 像素数不变 |
| 全屏刷屏 | ≈82ms | ≈82ms | 15MHz SPI，153.6KB/1.875MB/s |

横屏嫌 RAM 紧，把 `SGL_PORT_BUF_LINES` 从 40 降到 32（20.5KB），代价是全屏多 1~2 次窗口命令，实测差别很小。

---

## 2. 建立页面

### 2.1 单页面（推荐）：直接用 `sgl_screen_act()`

`sgl_init()` 内部已经建好一个根页面并置为 active。绝大多数产品只有一屏，直接在上面摆控件：

```c
int UiDemo_Init(void)
{
    int rc = sgl_port_init();          /* st7789_init + sgl_init，内部已建好根页面 */
    if (rc != 0) return rc;

    ui_page_build(sgl_screen_act());   /* 往根页面上挂控件 */
    sgl_port_refresh();                /* 立刻出第一帧，别让用户看黑屏 */
    return 0;
}
```

**不要**用 `sgl_obj_create(NULL)` 再建一页——那是第二个页面对象，多占一份内存，而且和模拟器写法不一致。

### 2.2 多页面：创建 → 加载 → 删除

```c
static sgl_obj_t *s_page_main;
static sgl_obj_t *s_page_setting;

/* 新建一页（parent = NULL 表示这是页面级对象） */
s_page_setting = sgl_obj_create(NULL);
sgl_page_set_color(s_page_setting, sgl_rgb(0x0E, 0x14, 0x1E));
build_setting_page(s_page_setting);

/* 切换：active 指针一变，下一帧整屏重绘 */
sgl_screen_load(s_page_setting);

/* 不再用的页面必须删，否则 heap 只出不进 */
sgl_obj_delete(s_page_main);
```

`sgl_obj_delete()` 的三种行为（`core/sgl_core.c`）：

| 传入 | 行为 |
|---|---|
| `NULL` 或当前 active 页 | 保留页面本身，**清空所有子对象** |
| 普通子对象 | 标记 `DESTROYED`，在事件循环里释放 |
| 非 active 的页面 | 立即 `sgl_free()` |

> 切页面前先删旧页；否则 16KB 堆建两三个页面就满了。

### 2.3 堆预算

所有控件都从 `CONFIG_SGL_HEAP_MEMORY_SIZE = 16384` 的静态池分配（lwmem）。本工程 14 个控件的实测占用：

```
HEAP 1832 / 16384     ← 页面右下角实时显示
```

经验值：

| 控件 | 约占用 |
|---|---|
| label / led / progress | 60 ~ 100 B |
| button / switch / checkbox | 90 ~ 140 B |
| slider / gauge | 120 ~ 180 B |
| roller / dropdown | 200 ~ 400 B（选项文本另算） |
| keyboard / textlist | 400 ~ 900 B |

一屏 20 个普通控件约 2~3KB，16KB 够用；要放键盘或两三个列表，建议加到 24~32KB（RAM 224KB，不心疼）。

实时查看：页面上的 `HEAP x/16384` 标签，或代码里 `sgl_mm_get_monitor()`。

### 2.4 页面底色

```c
sgl_page_set_color(root, sgl_rgb(0x0E, 0x14, 0x1E));   /* 深蓝黑 */
```

也可以贴图：`sgl_page_set_pixmap(root, &my_pixmap)`（本工程没编 img_ext，只支持内置 pixmap 数组）。

---

## 3. 控件

### 3.1 所有控件通用的对象 API

| 函数 | 作用 | 备注 |
|---|---|---|
| `sgl_obj_set_pos(obj, x, y)` | 相对父对象定位 | 左上角 |
| `sgl_obj_set_size(obj, w, h)` | 尺寸 | 必须在 set_pos 之后 |
| `sgl_obj_set_pos_align(obj, SGL_ALIGN_...)` | 按父对象对齐 | 布局居中时省算坐标 |
| `sgl_obj_set_radius(obj, r)` | 圆角 | 各控件也有自己的 set_radius |
| `sgl_obj_set_border_width/color` | 边框 | |
| `sgl_obj_set_alpha(obj, a)` | 透明度 | `SGL_ALPHA_MAX` = 不透明 |
| `sgl_obj_set_event_cb(obj, fn, data)` | 事件回调 | `fn(sgl_event_t *e)` |
| `sgl_obj_set_clickable(obj)` | 可点击 | 多数控件 create 时自带 |
| `sgl_obj_set_editable(obj)` | 可编辑 | 决定编码器按一下是"进编辑"还是"点击" |
| `sgl_obj_set_dirty(obj)` | 手动标脏重绘 | 改了控件内部状态后调 |

对齐枚举：`SGL_ALIGN_LEFT_TOP` / `LEFT_MID` / `LEFT_BOTTOM` / `CENTER` / `RIGHT_MID` …（`include/sgl_core.h`）。

### 3.2 label —— 文字

```c
sgl_obj_t *lbl = sgl_label_create(root);
sgl_obj_set_pos(lbl, 12, 4);
sgl_obj_set_size(lbl, 296, 22);
sgl_label_set_font(lbl, &consolas14);        /* 先设字体，后面算宽度要用 */
sgl_label_set_text_color(lbl, sgl_rgb(0x4E,0xD8,0xFF));
sgl_label_set_text_align(lbl, SGL_ALIGN_LEFT_MID);
sgl_label_set_text(lbl, "AT32F403A");
```

**会变的文字必须绑定缓冲**，否则写进去是空串：

```c
static char buf[24];                                  /* 生命周期要覆盖标签 */

sgl_label_set_text_buffer(lbl, buf, sizeof(buf));
sgl_label_set_text_fmt(lbl, "Brightness: %d %%", 53); /* 之后随便刷新 */
```

原因：`sgl_label_set_text()` 只存指针、把 `text_capacity` 留在 0；`set_text_fmt` 发现容量为 0 就直接返回，于是永远显示空。

**格式化只支持这几种**（`core/sgl_snprintf.c`）：

| 支持 | 不支持 |
|---|---|
| `%d` `%s` `%c` `%x` `%X` `%f` `%%` | **`%u`** `%ld` `%02d` `%p` 等 |

写成 `%u` 的后果不是报错，是**原样打印 `%u`**——本工程第一版就踩了，屏上显示 `Brightness: %u %`。无符号数一律用 `%d`（值都小于 2^31，不会出错）。

中文用 `song23`：

```c
sgl_label_set_font(lbl, &song23);
sgl_label_set_text(lbl, "音量");     /* UTF-8 由 SGL 内部解析 */
```

### 3.3 button —— 按钮

```c
static void on_btn(sgl_event_t *e)
{
    if (e->type != SGL_EVENT_CLICKED) return;   /* 关键：必须过滤 */
    /* 业务逻辑 */
}

sgl_obj_t *btn = sgl_button_create(root);
sgl_obj_set_pos(btn, 200, 32);
sgl_obj_set_size(btn, 108, 30);
sgl_button_set_radius(btn, 6);
sgl_button_set_color(btn, sgl_rgb(0x1F,0x2A,0x38));
sgl_button_set_border_color(btn, sgl_rgb(0x3A,0x4A,0x60));
sgl_button_set_border_width(btn, 1);
sgl_button_set_text_color(btn, sgl_rgb(0xE8,0xF0,0xF8));
sgl_button_set_font(btn, &consolas14);
sgl_button_set_text(btn, "MODE A");
sgl_obj_set_event_cb(btn, on_btn, NULL);
```

必读：button 控件自己**只处理 PRESSED / RELEASED**（做按下缩放效果），业务回调会依次收到：

| 动作 | 回调收到的 type 序列 |
|---|---|
| 编码器/鼠标短按（<500ms） | `PRESSED` → `CLICKED` → `RELEASED` |
| 按住 ≥500ms 再松 | `PRESSED` → **`LONG_CLICKED`** → `RELEASED` |

所以回调第一行必须 `if (e->type != SGL_EVENT_CLICKED) return;`，否则一次点击会触发三次业务。

### 3.4 slider —— 滑块（可编辑控件）

```c
sgl_obj_t *sld = sgl_slider_create(root);
sgl_obj_set_pos(sld, 12, 80);
sgl_obj_set_size(sld, 176, 24);
sgl_slider_set_value(sld, 50);                       /* 0~100，必须显式设初值 */
sgl_slider_set_fill_color(sld, sgl_rgb(0x2E,0xC4,0x6B));   /* 已填充段 */
sgl_slider_set_track_color(sld, sgl_rgb(0x2A,0x34,0x42));  /* 未填充段 */
sgl_slider_set_knob_color(sld, sgl_rgb(0xE8,0xF0,0xF8));   /* 圆钮 */
```

| 特性 | 说明 |
|---|---|
| 范围 | 固定 0~100（`uint8_t`），要别的量程自己映射 |
| 可编辑 | create 时自带 `editable`，**编码器必须先按一下进编辑态**，再转才改值 |
| 方向 | `KEY_UP`/`KEY_RIGHT` → **+1**；`KEY_DOWN`/`KEY_LEFT` → **-1** |
| 取值 | `sgl_slider_get_value()`；值变化**不会**回调业务层，要自己周期同步 |

```c
/* 在 ui_page_task() 里每 100ms 同步一次到文本 */
if (sgl_slider_get_value(sld) != s_bright) {
    s_bright = sgl_slider_get_value(sld);
    ui_refresh_bright();
}
```

`set_direct(SGL_DIRECT_VERTICAL)` 可做竖向滑块。

### 3.5 switch / checkbox —— 开关、复选框

```c
sgl_obj_t *sw = sgl_switch_create(root);
sgl_obj_set_pos(sw, 200, 108);
sgl_obj_set_size(sw, 52, 26);
sgl_switch_set_color(sw, sgl_rgb(0x2E,0xC4,0x6B));       /* ON 底色 */
sgl_switch_set_bg_color(sw, sgl_rgb(0x2A,0x34,0x42));    /* OFF 底色 */
sgl_switch_set_knob_color(sw, sgl_rgb(0xE8,0xF0,0xF8));
sgl_switch_set_status(sw, true);
sgl_obj_set_event_cb(sw, on_sw, NULL);

bool on = sgl_switch_get_status(sw);
```

**时序坑**：switch / checkbox 在 **`PRESSED`** 时就翻转状态，不是 CLICKED。所以回调里读到的是"已经翻转后"的值：

```c
static void on_sw(sgl_event_t *e)
{
    if (e->type != SGL_EVENT_CLICKED) return;
    s_sw_on = sgl_switch_get_status(s_sw) ? 1u : 0u;   /* 这里读到的已是新值 */
}
```

checkbox 用法一样：`sgl_checkbox_create` + `set_text` + `get_status`。

### 3.6 progress / led —— 只读指示

```c
sgl_obj_t *pg = sgl_progress_create(root);
sgl_obj_set_pos(pg, 12, 130);
sgl_obj_set_size(pg, 296, 14);
sgl_progress_set_radius(pg, 6);
sgl_progress_set_fill_color(pg, sgl_rgb(0x2E,0xC4,0x6B));
sgl_progress_set_track_color(pg, sgl_rgb(0x2A,0x34,0x42));
sgl_progress_set_value(pg, 40);          /* 0~100 */

sgl_obj_t *led = sgl_led_create(root);
sgl_obj_set_pos(led, 12, 34);
sgl_obj_set_size(led, 14, 14);
sgl_led_set_on_color(led, sgl_rgb(0x2E,0xC4,0x6B));
sgl_led_set_off_color(led, sgl_rgb(0x33,0x3C,0x48));
sgl_led_set_status(led, true);
```

这两个不加进按键组（只读），否则旋钮会多停一拍。

### 3.7 列表类：roller / textlist / dropdown

编码器场景下**最好用**的一类控件，天生适合"转着选"。

```c
/* 滚筒选择器：一列选项，上下滚动 */
sgl_obj_t *rl = sgl_roller_create(root);
sgl_obj_set_pos(rl, 12, 100);
sgl_obj_set_size(rl, 176, 54);
sgl_roller_set_option_static(rl, "LOW\nMID\nHIGH\nMAX");  /* \n 分隔 */
sgl_roller_set_visible_rows(rl, 3);
sgl_roller_set_text_font(rl, &consolas14);
int idx = sgl_roller_get_selected_index(rl);

/* 文本列表 */
sgl_obj_t *tl = sgl_textlist_create(root);
sgl_textlist_add_item(tl, "Item 1");
sgl_textlist_add_item(tl, "Item 2");

/* 下拉框：按一下展开，转着选 */
sgl_obj_t *dd = sgl_dropdown_create(root);
sgl_dropdown_set_option_static(dd, "A\nB\nC");
sgl_dropdown_add_option(dd, "D");
```

| 控件 | editable | 编码器行为 |
|---|---|---|
| roller | 是 | 按一下进编辑 → 转着换选项 → 长按退出 |
| textlist | 是 | 同上 |
| dropdown | 是 | 按一下展开 → 转着选 → 按一下确认 |

### 3.8 textbox / msgbox —— 文本展示、对话框

```c
/* 只读/多行文本展示 */
sgl_obj_t *tb = sgl_textbox_create(root);
sgl_obj_set_pos(tb, 12, 100);
sgl_obj_set_size(tb, 200, 80);
sgl_textbox_set_text(tb, "line1\nline2");
sgl_textbox_set_font(tb, &consolas14);
sgl_textbox_set_bg_color(tb, sgl_rgb(0x1A,0x22,0x2E));

/* 消息框：带左右两个按钮，自己就是一个模态层 */
sgl_obj_t *mb = sgl_msgbox_create(root);
sgl_obj_set_pos(mb, 60, 70);
sgl_obj_set_size(mb, 200, 100);
sgl_msgbox_set_title_text(mb, "CONFIRM");
sgl_msgbox_set_msg_text(mb, "Save settings?");
sgl_msgbox_set_left_btn_text(mb, "NO");
sgl_msgbox_set_right_btn_text(mb, "YES");
```

msgbox 弹出后要把**它的两个按钮**加进当前按键组，否则旋钮选不到（见 §4.2 的组切换）。

### 3.9 编码器场景的控件选型

| 需求 | 首选 | 别用 |
|---|---|---|
| 连续调节（音量/亮度） | `slider` / `gauge` | textbox 输入数字 |
| 离散档位（低/中/高） | `roller` / `dropdown` | 一排 button |
| 开关量 | `switch` / `checkbox` | button + 自己记状态 |
| 触发动作 | `button` | — |
| 只读数值 | `label`（`consolas24/32`） | — |
| 确认/提示 | `msgbox` | 自己搭一层 rect |

**只把可操作的控件加进按键组**，标签、进度条、LED 不要加。

---

## 4. 编码器 + PB9 输入

### 4.1 编码器归一化（必须做）

硬件：TMR4 正交解码，PB6 = B 相，PB7 = A 相，四倍频。驱动在 `BSP/Encoder_Driver/`。

```c
int16_t Encoder_ReadDelta(void);   /* 自上次调用以来的增量，正 = 顺时针 */
```

**一个机械定位点（detent）通常产生 ±4 个计数**。直接把增量丢给 SGL，转一格会连跳 4 个控件——表现为"旋钮太灵敏、乱跳"。必须攒够 4 个才算一步：

```c
#define UI_ENC_STEPS_PER_DETENT     4
#define UI_ENC_STEP_MAX             8

static void ui_input_poll(void)
{
    static int16_t acc;
    int16_t delta = Encoder_ReadDelta();
    int8_t  step  = 0;

    acc += delta;
    while (acc >= UI_ENC_STEPS_PER_DETENT)  { acc -= UI_ENC_STEPS_PER_DETENT; if (step <  UI_ENC_STEP_MAX) step++; }
    while (acc <= -UI_ENC_STEPS_PER_DETENT) { acc += UI_ENC_STEPS_PER_DETENT; if (step > -UI_ENC_STEP_MAX) step--; }

    sgl_event_encoder_input(step, ui_key_pressed());
}
```

手感调整：

| 现象 | 改法 |
|---|---|
| 转一格跳 2~4 个控件 | `UI_ENC_STEPS_PER_DETENT` 改 4（默认） |
| 转好几格才动一下 | 改 2 或 1 |
| 快速转时跳太多 | 减小 `UI_ENC_STEP_MAX` |
| 正反颠倒 | `step` 取反 |

### 4.2 SGL 的按键组模型（核心）

SGL 为"无触摸、只有旋钮和键"的设备设计了一套 **key group**：一个页面里所有可被旋钮操作的控件排成一个环，旋钮在环上走。

```
        ┌──────────────────────────────────────────┐
        │  key group: [slider] [btnLED] [btnMODE] [sw] │
        └──────────────────────────────────────────┘
                    ▲  焦点(FOCUSED)高亮边框
                    │
  未编辑态: 旋转 = 在环上移动焦点, 短按 = 点击
  editable 控件: 短按 = 进入编辑态
  编辑态:   旋转 = 把 KEY_UP/KEY_DOWN 发给该控件改值
            长按(≥500ms) = ESC, 退出编辑态
```

建组与加入（放在 `ui_page_build()` 最后）：

```c
s_grp = sgl_key_group_create(8);            /* 最多 8 个成员 */
sgl_key_group_add_obj(s_grp, s_slider);     /* 顺序 = 旋钮走动顺序 */
sgl_key_group_add_obj(s_grp, s_btn_led);
sgl_key_group_add_obj(s_grp, s_btn_mode);
sgl_key_group_add_obj(s_grp, s_sw);
sgl_key_group_load(s_grp);                  /* 设为当前活动组 */
```

规则（`core/sgl_event.c` 实现）：

| 规则 | 说明 |
|---|---|
| **导航顺序 = add_obj 顺序** | 不是屏幕上的几何位置，按视觉顺序加 |
| 上电第一次转，只点亮第一个焦点 | `sgl_key_ensure_focus()` 第一次返回 false，不移动 |
| 焦点环是闭环 | 最后一个再转会回到第一个 |
| 只有 `editable` 控件有"编辑态" | slider / roller / dropdown / textlist 是；button / switch / checkbox 不是 |
| 多页面各建一组 | 切页面时 `sgl_key_group_load(新组)`，旧组 `sgl_key_group_delete()` |

焦点框画在对象外圈：

```c
/* sgl_config.h */
#define CONFIG_SGL_FOCUSED_WIDTH   2
/* sgl_cfgfix.h 默认颜色：纯绿 */
#define CONFIG_SGL_FOCUSED_COLOR   sgl_rgb(0x00, 0xFF, 0x00)
```

> 颜色撞坑：本工程开关也是绿色，焦点落在开关上时几乎看不出。要么把 `CONFIG_SGL_FOCUSED_COLOR` 改成橙色/白色，要么把开关配色换成蓝色系。

### 4.3 事件类型速查

| 常量 | 值 | 什么时候来 |
|---|---|---|
| `SGL_EVENT_PRESSED` | 2 | 按下（button 用它做视觉按下） |
| `SGL_EVENT_RELEASED` | 3 | 松开 |
| `SGL_EVENT_CLICKED` | 4 | 短按松开（<500ms）—— **业务写这里** |
| `SGL_EVENT_LONG_CLICKED` | 5 | 长按松开（≥500ms） |
| `SGL_EVENT_FOCUSED` | 14 | 焦点进入 |
| `SGL_EVENT_UNFOCUSED` | 15 | 焦点离开 |
| `SGL_EVENT_KEY_UP/DOWN/LEFT/RIGHT` | 17~20 | 编辑态下旋钮发给控件 |
| `SGL_EVENT_KEY_ESC` | 21 | 长按退出编辑 |
| `SGL_EVENT_DESTROYED` | 16 | 对象被删除 |

程序主动投递事件（跨任务、定时触发）：

```c
sgl_event_send_obj(btn, SGL_EVENT_CLICKED);   /* 走真实队列，下一轮 handler 分发 */
```

### 4.4 PB9 按键

PB9 已经登记进 GPIO 表（`BSP/GPIO_Driver/gpio_cfg.h`）：

```c
#define GPIO_PIN_KEY          3    /* PB9 用户按键, 上拉输入, 低有效 */

static const gpio_hw_pin_t g_gpio_pin_map[] = {
    ...
    { GPIO_PIN_KEY, HW_HANDLE(GPIO_PORT_B, 9) },   /* PB9 */
};
```

初始化 + 10ms 消抖：

```c
static uint8_t s_key_stable = 1;    /* 1 = 松开（上拉为高） */
static uint8_t s_key_cnt;

static void ui_key_init(void)
{
    gpio_config_t cfg;

    cfg.pin        = GPIO_PIN_KEY;
    cfg.dir        = GPIO_DIR_INPUT;
    cfg.pull       = GPIO_PU_UP;     /* 内部上拉，按键另一端接 GND */
    cfg.init_state = GPIO_HIGH;
    cfg.speed      = 0;

    (void)gpio_pin_init(&cfg);
}

/* 每 10ms 调一次，连续 3 次一致才认（≈30ms 消抖） */
static bool ui_key_pressed(void)
{
    uint8_t raw = (gpio_read(GPIO_PIN_KEY) == GPIO_LOW) ? 1u : 0u;

    if (raw != s_key_stable) {
        if (++s_key_cnt >= 3u) { s_key_stable = raw; s_key_cnt = 0; }
    } else {
        s_key_cnt = 0;
    }
    return (s_key_stable != 0u) ? true : false;
}
```

**不需要自己写长短按判断**——把消抖后的电平交给 `sgl_event_encoder_input()`，它内部按 500ms 分短按/长按：

```c
sgl_event_encoder_input(step, ui_key_pressed());
```

> 如果 PB9 和编码器自带按键是**两个不同的键**，就分开用：编码器按键走 `sgl_event_encoder_input(step, enc_pressed)`，PB9 直接调 `sgl_key_enter_pressed()` / `sgl_key_enter_released()` / `sgl_key_esc()`（后者很适合做"返回"）。

### 4.5 主循环里的调用顺序

```c
void UiDemo_Task(void)          /* 调度器 10ms 一次，= CONFIG_SGL_SYSTICK_MS */
{
    ui_input_poll();            /* 1. 先喂输入，本轮就能画出来，最跟手 */
    sgl_port_task();            /* 2. 喂时基 + 事件队列 + 动画 + 脏区刷屏 */
    ui_page_task(BM_GetTick()); /* 3. 页面自己的周期逻辑 */
}
```

任务注册（`main.c` 已注册）：

```c
Scheduler_RegisterTask(TASK_UI, UiDemo_Task, 10U);
```

### 4.6 方向与手感

SGL 的方向语义有点绕：

| 调用 | 焦点行为 | 对 slider/roller |
|---|---|---|
| `sgl_key_up()` | 走**上一个**（prev） | `KEY_UP` → **+1** |
| `sgl_key_down()` | 走**下一个**（next） | `KEY_DOWN` → **-1** |

`sgl_event_encoder_input(diff > 0)` 内部调的是 `sgl_key_up()`，所以**顺时针 = 焦点上一个 + 数值增大**。数值方向符合直觉，焦点方向是"倒着走"。

三种调整方式，按需选一种：

```c
/* A. 只想让数值方向反过来 */
sgl_event_encoder_input(-step, pressed);

/* B. 想让顺时针走视觉上的下一个: 反序 add_obj */
sgl_key_group_add_obj(s_grp, s_sw);
sgl_key_group_add_obj(s_grp, s_btn_mode);
sgl_key_group_add_obj(s_grp, s_btn_led);
sgl_key_group_add_obj(s_grp, s_slider);

/* C. 完全自定义: 不用 sgl_event_encoder_input, 自己调 */
if (step > 0) for (int i = 0; i < step; i++) sgl_key_down();
else          for (int i = 0; i < -step; i++) sgl_key_up();
sgl_key_enter_pressed() / sgl_key_enter_released() / sgl_key_esc();
```

### 4.7 模拟器上的对应按键

模拟器 `demo/main.c` 已经接好同一套逻辑（边沿式调用，比电平式更适合键盘）：

| 键 | 等价动作 |
|---|---|
| → / ↓ | `sgl_key_up()`（顺时针） |
| ← / ↑ | `sgl_key_down()`（逆时针） |
| 空格 / 回车 | `sgl_key_enter_pressed()`，松开时按 500ms 分 `enter_released()` / `esc()` |
| Q | 退出 |

模拟器里验证过的流程：转一格 → 第一个控件出现绿框；按空格 → 进编辑态；再转 → 数值变化；长按空格 → 退出编辑。

---

## 5. 坑清单

按"现象 → 原因 → 解法"排，都是本工程实际踩过的。

| # | 现象 | 原因 | 解法 |
|---|---|---|---|
| 1 | 屏幕全黑，LED 快闪 2 次 | `sgl_port_init()` 返回 -2：`st7789_get_width() != SGL_PORT_BUF_WIDTH` | 方向宏与页面 `UI_W` 不一致，见 §1.2 |
| 2 | 画面出来一帧后静止 | flush 回调漏了 `sgl_fbdev_flush_ready()` | `sgl_port_flush_area()` 末尾必须调 |
| 3 | 显示 `Brightness: %u %` | SGL 精简 printf 不支持 `%u` | 全部改 `%d` |
| 4 | 动态文本是空串 | 只调了 `set_text`，没绑缓冲 | `sgl_label_set_text_buffer()` + static 缓冲 |
| 5 | 转一格跳 4 个控件 | 四倍频增量没归一化 | `UI_ENC_STEPS_PER_DETENT = 4` |
| 6 | 旋钮转了没反应 | 控件没加进 key group，或组没 `load` | `sgl_key_group_add_obj()` + `load()` |
| 7 | 旋钮一动，数值乱跳 | 编辑态判定 | 先短按进编辑态，再转；长按退出 |
| 8 | 焦点框看不见 | 焦点色和控件底色撞了 | 改 `CONFIG_SGL_FOCUSED_COLOR` |
| 9 | 按键回调执行三次 | 没过滤事件类型 | `if (e->type != SGL_EVENT_CLICKED) return;` |
| 10 | 长按不触发 | 用的是 `CLICKED` 判断 | 长按是 `LONG_CLICKED` |
| 11 | 切页面后堆耗尽 | 旧页面没删 | `sgl_obj_delete(旧页)` |
| 12 | 改了控件状态不刷新 | 内部状态改了但没标脏 | `sgl_obj_set_dirty(obj)`（多数 setter 自带） |
| 13 | 控件跑到屏幕外 | 横竖屏切换后布局没改 | §1.5，或跑校验脚本 |
| 14 | 汉字显示方块 | 用了 consolas 系列 | 中文必须 `song23` |
| 15 | 链接报 undefined | 用了未编译的字体/控件 | 字号只用 14/23/24/32；控件避开 gif/avi/filebrowser 等 |
| 16 | Keil 编出的镜像不含 SGL | 只跑了增量 `-b` | 改 `.uvprojx` 后用 `-r` 全量 |
| 17 | 模拟器好好的，上板错位 | 两边画布尺寸不同 | 三处一起改：§1.4 |
| 18 | 模拟器里鼠标一划控件乱跳 | 鼠标停在控件上被当作拖动 | 用键盘测试时把鼠标移出窗口 |
| 19 | 上电滑块停在 0 | 没调 `sgl_slider_set_value()` | 显式设初值 |
| 20 | 旋转方向反了 | SGL 的 UP=prev | §4.6 三种改法 |

---

## 6. 上板检查清单

```bash
# 1. 两边 [A] 段是否一致（画布方向自动取 port 层配置）
python tools/ui_port_check.py
# 期望：全部通过 (0 error, 0 warning)

# 2. 板卡全量编译
cd mdk_v5
"C:/Keil_v5/UV4/UV4.exe" -r AT32F403A_Project.uvprojx -j0 -o build.log
# 期望：0 Error(s), 0 Warning(s)
```

上电应看到：

1. 屏亮，深蓝黑底，顶部 `AT32F403A SGL 320x240`
2. 左侧 LED 绿点 + `LED: ON`，`Brightness: 50 %`，滑块在中间
3. 右侧 `LED` / `MODE A` 两个按钮，下面开关 ON + `SW ON`
4. 进度条每 200ms 加 2%，`PROG n %`；右下 `HEAP x/16384`
5. **转编码器**：滑块外圈出现绿色框（焦点）
6. **再转**：焦点在 滑块 → 开关 → MODE A → LED 之间循环（顺序是 add_obj 的逆序）
7. **按 PB9**：焦点在按钮上 → 按钮动作（LED 切换 / MODE 切换）；焦点在滑块上 → 进入编辑态
8. **编辑态转编码器**：`Brightness` 数值增减，滑块跟着动
9. **长按 PB9 ≥500ms**：退出编辑态，焦点清空
10. 焦点在 LED 按钮上按 PB9，板载 LED2 翻转（`at32_led_toggle` 验证事件链路）

失败时的错误码（`main.c` 里 LED 闪次数）：

| 闪 | 含义 |
|---|---|
| 1 | `st7789_init()` 失败 |
| 2 | 方向/缓冲宽度不符（§1.2） |
| 3 | `sgl_fbdev_register()` 失败 |
| 4 | `sgl_init()` 失败 |
| 5 | 页面创建失败（多为 SGL 堆不够） |

---

## 7. 相关文件

| 文件 | 职责 |
|---|---|
| `BSP/UI_Driver/sgl_port_at32.h` | 方向宏 `SGL_PORT_LANDSCAPE`、移植层接口 |
| `BSP/UI_Driver/sgl_port_at32.c` | 刷屏回调、时基、`sgl_port_init/task` |
| `app/Src/ui_demo.c` | 页面 + 编码器/PB9 输入（`[A]` 段与模拟器逐字相同） |
| `BSP/Encoder_Driver/encoder.c` | TMR4 正交解码，`Encoder_ReadDelta()` |
| `BSP/GPIO_Driver/gpio_cfg.h` | PB9 登记（`GPIO_PIN_KEY`） |
| `third_party/sgl/sgl_config.h` | SGL 配置（堆、字体、脏区、焦点框） |
| `tools/ui_port_check.py` | 搬运校验（画布方向自动跟随 port 层） |
| `Docs/UI/模拟器到板卡移植规范.md` | 搬运规范（`[A]/[B]` 分段、配置对照） |
| `../sgl-port-windows-vscode/demo/main.c` | PC 模拟器页面（同一份 `[A]` 段） |
