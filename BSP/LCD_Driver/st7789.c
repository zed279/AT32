/**
 * @file    st7789.c
 * @brief   ST7789 (240x320 TFT, SPI 4线) 驱动实现
 * @note    分层关系:
 *            st7789.c  ->  gpio.h (RESET / DC / BL 电平控制)
 *                      ->  spi.h  (CS / SCK / MOSI, 硬件 SPI2)
 *          本文件不含任何 STM32 寄存器操作, 只有 delay_ms() 一处平台依赖.
 *
 *          SPI 事务模型 (关键, 别改坏):
 *            ST7789 的 4 线串行协议要求 "一条命令 + 它的参数/像素数据"
 *            在同一个 CS 低电平周期内完成. 因此所有操作都按
 *              lcd_begin() -> 写命令 -> 写数据 -> lcd_end()
 *            的完整事务组织, 不允许多次 CS 翻转拼接同一命令.
 *
 *          写速度 (SPI2 = 42MHz / 4 = 10.5MHz):
 *            单色全屏 (240*320*2 = 150KB) 约 115ms.
 *            LVGL 场景下逐块刷新, 实际每次只推脏区, 体感远比全屏刷新快.
 *            若屏走线较长出现花屏, 把 ST7789_SPI_SPEED 降回 SPI_SPEED_FAST(5.25MHz).
 */
#include "st7789.h"
/* 注意: 这里必须用相对路径. 工程里 Core/Inc 下也有 gpio.h / spi.h (CubeMX 生成的
 * 外设初始化声明), 若写成 "gpio.h" 会因 -I 搜索顺序命中 Core 版本, 导致
 * gpio_config_t / spi_config_t / GPIO_PIN_x 全部未定义. */
#include "../GPIO_Driver/gpio.h"
#include "../SPI_Driver/spi.h"
#include "st7789_font.h"

/* AT32 平台延时: 用 BSP 的 delay_ms() (SysTick 轮询, 与本工程 TMR2 时基互不干扰) */
#include "at32f403a_407_board.h"

/* ============================ 内部配置 ============================ */
#define ST7789_SPI_BUS              SPI_BUS_LCD
#define ST7789_SPI_TIMEOUT_MS       100u
/**
 * @brief SPI 速度等级
 * @note  换算依据见 spi_port_at32.c 的 spi_at32_speed_to_div():
 *          分频 = 满足 APBx/div <= 档位目标值 的最小值.
 *        AT32F403A 主频 240MHz -> APB2 = 120MHz, 故本档实际取值:
 *          SPI_SPEED_FAST      -> 120/16 =  7.5MHz (全屏约 164ms, 静态画面足够)
 *          SPI_SPEED_VERY_FAST -> 120/8  = 15.0MHz (全屏约  82ms, 本档)
 *        ST7789 写周期上限 tSCYCW=66ns 约合 15.15MHz, 15MHz 卡在规格线上,
 *        无余量. 若上板出现随机花点/错位, 优先降级为 SPI_SPEED_FAST.
 */
#define ST7789_SPI_SPEED            SPI_SPEED_VERY_FAST

/**
 * @brief MADCTL 的 RGB/BGR 位
 * @note  0x00 = 正常 RGB 顺序; 若屏上红蓝互换 (红色显示成蓝色),
 *        把本宏改成 0x08 即可.
 */
#define ST7789_MADCTL_COLOR_ORDER   0x00

/* 行缓冲像素数: 取横竖两个方向的最大值, 保证横屏也能一次刷一整行 */
#define ST7789_LINE_BUF_PX          320

/* ============================ 内部状态 ============================ */
static uint16_t s_width  = ST7789_WIDTH;
static uint16_t s_height = ST7789_HEIGHT;
static uint8_t  s_madctl = ST7789_MADCTL_COLOR_ORDER;

/* 行缓冲: 存放一整行 RGB565 像素 (最多 320 px = 640 字节) */
static uint8_t  s_line_buf[ST7789_LINE_BUF_PX * 2];

/* ============================ 低层: 引脚与事务 ============================ */

static inline void lcd_dc(uint8_t level)
{
    gpio_write(GPIO_PIN_LCD_DC, level ? GPIO_HIGH : GPIO_LOW);
}

static inline void lcd_rst(uint8_t level)
{
    gpio_write(GPIO_PIN_LCD_RESET, level ? GPIO_HIGH : GPIO_LOW);
}

/* CS 低, DC 切到命令态: 开启一次完整事务 */
static void lcd_begin(void)
{
    spi_cs_select(ST7789_SPI_BUS);
    lcd_dc(0);
}

/* 结束事务: CS 拉高, 控制器锁存并执行 */
static void lcd_end(void)
{
    spi_cs_deselect(ST7789_SPI_BUS);
}

/* 事务内: 写 1 字节命令 (DC=0) */
static st7789_status_t lcd_cmd(uint8_t cmd)
{
    lcd_dc(0);
    return (spi_write(ST7789_SPI_BUS, &cmd, 1, ST7789_SPI_TIMEOUT_MS) == SPI_OK)
           ? ST7789_OK : ST7789_ERR_SPI;
}

/* 事务内: 写 n 字节数据 (DC=1) */
static st7789_status_t lcd_dat(const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return ST7789_OK;
    }
    lcd_dc(1);
    return (spi_write(ST7789_SPI_BUS, data, len, ST7789_SPI_TIMEOUT_MS) == SPI_OK)
           ? ST7789_OK : ST7789_ERR_SPI;
}

/* 独立事务: 发一条带参数的命令 */
static st7789_status_t lcd_cmd_data(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    st7789_status_t st;

    lcd_begin();
    st = lcd_cmd(cmd);
    if (st == ST7789_OK) {
        st = lcd_dat(data, len);
    }
    lcd_end();
    return st;
}

/* ============================ 低层: 窗口 ============================ */

/* 事务内: 设置绘图窗口 (CASET + RASET + RAMWR), 之后可直接连续写像素 */
static st7789_status_t lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x0 = (uint16_t)(x + ST7789_X_OFFSET);
    uint16_t y0 = (uint16_t)(y + ST7789_Y_OFFSET);
    uint16_t x1 = (uint16_t)(x0 + w - 1);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    uint8_t  buf[4];
    st7789_status_t st;

    /* CASET: 列地址范围 */
    buf[0] = (uint8_t)(x0 >> 8); buf[1] = (uint8_t)(x0 & 0xFF);
    buf[2] = (uint8_t)(x1 >> 8); buf[3] = (uint8_t)(x1 & 0xFF);
    st = lcd_cmd(0x2A);
    if (st != ST7789_OK) return st;
    st = lcd_dat(buf, 4);
    if (st != ST7789_OK) return st;

    /* RASET: 行地址范围 */
    buf[0] = (uint8_t)(y0 >> 8); buf[1] = (uint8_t)(y0 & 0xFF);
    buf[2] = (uint8_t)(y1 >> 8); buf[3] = (uint8_t)(y1 & 0xFF);
    st = lcd_cmd(0x2B);
    if (st != ST7789_OK) return st;
    st = lcd_dat(buf, 4);
    if (st != ST7789_OK) return st;

    /* RAMWR: 进入写显存状态, 后续数据自动按窗口填充 */
    return lcd_cmd(0x2C);
}

/* ============================ 初始化序列 ============================ */

typedef struct {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[14];
    uint16_t delay_ms;
} st7789_init_cmd_t;

/**
 * @brief ST7789 初始化序列 (240x320 IPS, 厂家通用配方)
 * @note  INVON(0x21) 对 IPS 屏是必须的, 漏了会整屏反色;
 *        若你的屏是 TN 且颜色发白, 把 0x21 换成 0x20 (INVOFF).
 */
static const st7789_init_cmd_t s_init_seq[] = {
    { 0x01, 0, { 0 },                                              150 }, /* SWRESET  软件复位 */
    { 0x11, 0, { 0 },                                              120 }, /* SLPOUT   退出睡眠 */
    { 0x36, 1, { 0x00 },                                             0 }, /* MADCTL   (set_rotation 会重写) */
    { 0x3A, 1, { 0x55 },                                             0 }, /* COLMOD   16bit/px RGB565 */
    { 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 },                      0 }, /* PORCTRL  门廊 */
    { 0xB7, 1, { 0x35 },                                             0 }, /* GCTRL    VGH/VGL */
    { 0xBB, 1, { 0x19 },                                             0 }, /* VCOMS    VCOM */
    { 0xC0, 1, { 0x2C },                                             0 }, /* LCMCTRL */
    { 0xC2, 1, { 0x01 },                                             0 }, /* VDVVRHEN */
    { 0xC3, 1, { 0x12 },                                             0 }, /* VRHS     VAP */
    { 0xC4, 1, { 0x20 },                                             0 }, /* VDVS     VDV */
    { 0xC6, 1, { 0x0F },                                             0 }, /* FRCTRL2  60Hz */
    { 0xD0, 2, { 0xA4, 0xA1 },                                       0 }, /* PWCTRL1  电源控制 */
    { 0xE0, 14, { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                  0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 },        0 }, /* PVGAMCTRL 正极性伽马 */
    { 0xE1, 14, { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                  0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 },        0 }, /* NVGAMCTRL 负极性伽马 */
    { 0x21, 0, { 0 },                                              10 }, /* INVON    IPS 屏必须反显 */
    { 0x13, 0, { 0 },                                              10 }, /* NORON    普通显示模式 */
    { 0x29, 0, { 0 },                                              20 }, /* DISPON   开显示 */
};

/* ============================ 公共 API ============================ */

static st7789_status_t lcd_gpio_init(void)
{
    gpio_config_t cfg;
    gpio_status_t gst;

    /* RESET: 推挽输出, 初始高 (不复位) */
    cfg.pin        = GPIO_PIN_LCD_RESET;
    cfg.dir        = GPIO_DIR_OUTPUT;
    cfg.pull       = GPIO_PU_NONE;
    cfg.init_state = GPIO_HIGH;
    cfg.speed      = 1;
    gst = gpio_pin_init(&cfg);
    if (gst != GPIO_OK) return ST7789_ERR_GPIO;

    /* DC/A0: 推挽输出, 初始高 (数据态, 空闲态无所谓但要确定) */
    cfg.pin        = GPIO_PIN_LCD_DC;
    cfg.init_state = GPIO_HIGH;
    gst = gpio_pin_init(&cfg);
    if (gst != GPIO_OK) return ST7789_ERR_GPIO;

    /* BL: 推挽输出, 初始灭 (避免上电瞬间花屏白闪) */
    cfg.pin        = GPIO_PIN_LCD_BL;
    cfg.init_state = ST7789_BL_ACTIVE_LEVEL ? GPIO_LOW : GPIO_HIGH;
    gst = gpio_pin_init(&cfg);
    if (gst != GPIO_OK) return ST7789_ERR_GPIO;

    return ST7789_OK;
}

static st7789_status_t lcd_spi_init(void)
{
    spi_config_t cfg;

    cfg.bus         = ST7789_SPI_BUS;
    cfg.mode        = SPI_MODE_0;              /* CPOL=0, CPHA=0 */
    cfg.bit_order   = SPI_MSB_FIRST;
    cfg.speed       = ST7789_SPI_SPEED;
    cfg.cs_polarity = SPI_CS_ACTIVE_LOW;
    cfg.data_size   = SPI_DATA_8BIT;
    cfg.timeout_ms  = ST7789_SPI_TIMEOUT_MS;

    return (spi_bus_init(&cfg) == SPI_OK) ? ST7789_OK : ST7789_ERR_SPI;
}

/* 硬件复位: RST 低 >10us, 高后等 120ms 才能发命令 */
static void lcd_hw_reset(void)
{
    lcd_rst(1);
    delay_ms(1);
    lcd_rst(0);
    delay_ms(20);
    lcd_rst(1);
    delay_ms(120);
}

st7789_status_t st7789_init(void)
{
    st7789_status_t st;

    /* 1. 先配 GPIO, 保证 RST/DC/BL 处于确定电平 */
    st = lcd_gpio_init();
    if (st != ST7789_OK) return st;

    /* 2. 配 SPI (内部会把 CS=PA6 配成推挽输出并拉高) */
    st = lcd_spi_init();
    if (st != ST7789_OK) return st;

    /* 3. 硬件复位 */
    lcd_hw_reset();

    /* 4. 写初始化序列 */
    for (uint16_t i = 0; i < (uint16_t)(sizeof(s_init_seq) / sizeof(s_init_seq[0])); ++i) {
        const st7789_init_cmd_t *c = &s_init_seq[i];
        st = lcd_cmd_data(c->cmd, c->data, c->len);
        if (st != ST7789_OK) return st;
        if (c->delay_ms) {
            delay_ms(c->delay_ms);
        }
    }

    /* 5. 默认竖屏 + 清屏为黑 + 开背光 */
    (void)st7789_set_rotation(ST7789_ROT_0);
    (void)st7789_fill(ST7789_BLACK);
    st7789_backlight(true);

    return ST7789_OK;
}

st7789_status_t st7789_set_rotation(st7789_rot_t rot)
{
    uint8_t val;

    switch (rot) {
        case ST7789_ROT_0:   val = 0x00; s_width = 240; s_height = 320; break;
        case ST7789_ROT_90:  val = 0x60; s_width = 320; s_height = 240; break;
        case ST7789_ROT_180: val = 0xC0; s_width = 240; s_height = 320; break;
        case ST7789_ROT_270: val = 0xA0; s_width = 320; s_height = 240; break;
        default: return ST7789_ERR_PARAM;
    }
    s_madctl = (uint8_t)(val | ST7789_MADCTL_COLOR_ORDER);

    /* 只写 MADCTL 即可; 下一次 fill/draw 会重新 CASET/RASET, 无窗口残留问题.
     * 注意: 若 ST7789_X_OFFSET/Y_OFFSET 不为 0, 横竖屏切换时这两个偏移需要互换,
     *       本工程 240x320 屏偏移为 0, 无需处理. */
    return lcd_cmd_data(0x36, &s_madctl, 1);
}

void st7789_backlight(bool on)
{
    uint8_t level = on ? ST7789_BL_ACTIVE_LEVEL : !ST7789_BL_ACTIVE_LEVEL;
    gpio_write(GPIO_PIN_LCD_BL, level ? GPIO_HIGH : GPIO_LOW);
}

uint16_t st7789_get_width(void)  { return s_width;  }
uint16_t st7789_get_height(void) { return s_height; }

/* ============================ 绘图 ============================ */

st7789_status_t st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 st7789_color_t color)
{
    uint16_t i;
    st7789_status_t st;

    if (w == 0 || h == 0) {
        return ST7789_ERR_PARAM;
    }
    /* 裁剪到屏幕内 */
    if (x >= s_width || y >= s_height) {
        return ST7789_OK;
    }
    if ((uint32_t)x + w > s_width)  w = (uint16_t)(s_width - x);
    if ((uint32_t)y + h > s_height) h = (uint16_t)(s_height - y);

    /* 预先填充一整行像素到行缓冲 (RGB565 大端序) */
    for (i = 0; i < w; ++i) {
        s_line_buf[2 * i]     = (uint8_t)(color >> 8);
        s_line_buf[2 * i + 1] = (uint8_t)(color & 0xFF);
    }

    /* 单个事务: 窗口 + 连续 h 行像素 */
    lcd_begin();
    st = lcd_set_window(x, y, w, h);
    if (st == ST7789_OK) {
        lcd_dc(1);
        for (i = 0; i < h; ++i) {
            if (spi_write(ST7789_SPI_BUS, s_line_buf, (uint16_t)(w * 2),
                          ST7789_SPI_TIMEOUT_MS) != SPI_OK) {
                st = ST7789_ERR_SPI;
                break;
            }
        }
    }
    lcd_end();
    return st;
}

st7789_status_t st7789_fill(st7789_color_t color)
{
    return st7789_fill_rect(0, 0, s_width, s_height, color);
}

st7789_status_t st7789_draw_pixel(uint16_t x, uint16_t y, st7789_color_t color)
{
    return st7789_fill_rect(x, y, 1, 1, color);
}

st7789_status_t st7789_draw_hline(uint16_t x, uint16_t y, uint16_t len,
                                  st7789_color_t color)
{
    return st7789_fill_rect(x, y, len, 1, color);
}

st7789_status_t st7789_draw_vline(uint16_t x, uint16_t y, uint16_t len,
                                  st7789_color_t color)
{
    return st7789_fill_rect(x, y, 1, len, color);
}

st7789_status_t st7789_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 uint8_t thickness, st7789_color_t color)
{
    st7789_status_t st;

    if (w == 0 || h == 0 || thickness == 0) {
        return ST7789_ERR_PARAM;
    }
    if ((uint16_t)(thickness * 2) >= h || (uint16_t)(thickness * 2) >= w) {
        /* 线宽超过一半时退化为实心矩形 */
        return st7789_fill_rect(x, y, w, h, color);
    }

    /* 上下两条横边 */
    st = st7789_fill_rect(x, y, w, thickness, color);
    if (st != ST7789_OK) return st;
    st = st7789_fill_rect(x, (uint16_t)(y + h - thickness), w, thickness, color);
    if (st != ST7789_OK) return st;
    /* 左右两条竖边 (避开已画的角) */
    st = st7789_fill_rect(x, (uint16_t)(y + thickness), thickness,
                          (uint16_t)(h - 2 * thickness), color);
    if (st != ST7789_OK) return st;
    return st7789_fill_rect((uint16_t)(x + w - thickness),
                            (uint16_t)(y + thickness), thickness,
                            (uint16_t)(h - 2 * thickness), color);
}

/* ============================ 块刷新 (LVGL 用) ============================ */

/**
 * @brief 块推送: 单个事务写完整窗口, 零拷贝
 * @note  px_be 已是 ST7789 需要的字节序 (每像素 高字节在前),
 *        因此这里按行直接透传, 不再做 uint16 拆分.
 */
st7789_status_t st7789_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const uint8_t *px_be)
{
    uint16_t row;
    uint16_t row_bytes;
    st7789_status_t st;

    if (px_be == NULL || w == 0 || h == 0) {
        return ST7789_ERR_PARAM;
    }
    if (x >= s_width || y >= s_height) {
        return ST7789_OK;
    }
    /* 裁剪到屏幕内 (LVGL 的脏区本就不会越界, 这里兜底) */
    if ((uint32_t)x + w > s_width)  w = (uint16_t)(s_width - x);
    if ((uint32_t)y + h > s_height) h = (uint16_t)(s_height - y);

    row_bytes = (uint16_t)(w * 2);

    lcd_begin();
    st = lcd_set_window(x, y, w, h);
    if (st == ST7789_OK) {
        lcd_dc(1);
        for (row = 0; row < h; ++row) {
            if (spi_write(ST7789_SPI_BUS, px_be + (uint32_t)row * row_bytes,
                          row_bytes, ST7789_SPI_TIMEOUT_MS) != SPI_OK) {
                st = ST7789_ERR_SPI;
                break;
            }
        }
    }
    lcd_end();
    return st;
}

/* ============================ 文字 ============================ */
st7789_status_t st7789_draw_char(uint16_t x, uint16_t y, char ch,
                                 st7789_color_t fg, st7789_color_t bg,
                                 uint8_t scale)
{
    const uint8_t *glyph;
    uint16_t cw, chh;
    uint16_t n;
    st7789_status_t st;

    if (scale == 0) { scale = 1; }
    if (scale > 4)  { scale = 4; }

    /* 字符码范围外一律按空格 (空白块) 处理 */
    if ((uint8_t)ch < ST7789_FONT_FIRST || (uint8_t)ch > ST7789_FONT_LAST) {
        ch = ' ';
    }

    cw  = (uint16_t)(ST7789_FONT_W * scale);
    chh = (uint16_t)(ST7789_FONT_H * scale);

    /* 完整落在屏内才绘制; 部分越界交给调用者裁剪 (draw_string 会自动折行) */
    if (x >= s_width || y >= s_height) {
        return ST7789_OK;
    }
    if ((uint32_t)x + cw > s_width || (uint32_t)y + chh > s_height) {
        return ST7789_OK;
    }

    glyph = g_st7789_font8x16[(uint8_t)ch - ST7789_FONT_FIRST];

    lcd_begin();
    st = lcd_set_window(x, y, cw, chh);
    if (st == ST7789_OK) {
        lcd_dc(1);
        for (uint8_t gy = 0; gy < ST7789_FONT_H; ++gy) {
            uint8_t bits = glyph[gy];
            uint8_t sx = 0;

            /* 横向展开: 每个点阵位 -> scale 个像素 */
            for (uint8_t gx = 0; gx < ST7789_FONT_W; ++gx) {
                st7789_color_t c = (bits & (uint8_t)(0x80u >> gx)) ? fg : bg;
                for (uint8_t s = 0; s < scale; ++s) {
                    s_line_buf[2 * sx]     = (uint8_t)(c >> 8);
                    s_line_buf[2 * sx + 1] = (uint8_t)(c & 0xFF);
                    ++sx;
                }
            }
            n = (uint16_t)(cw * 2);

            /* 纵向重复: scale 次同样的行 */
            for (uint8_t s = 0; s < scale; ++s) {
                if (spi_write(ST7789_SPI_BUS, s_line_buf, n,
                              ST7789_SPI_TIMEOUT_MS) != SPI_OK) {
                    st = ST7789_ERR_SPI;
                    break;
                }
            }
            if (st != ST7789_OK) break;
        }
    }
    lcd_end();
    return st;
}

int st7789_draw_string(uint16_t x, uint16_t y, const char *str,
                       st7789_color_t fg, st7789_color_t bg,
                       uint8_t scale)
{
    uint16_t cx = x;
    uint16_t cy = y;
    uint16_t cw, chh;
    int drawn = 0;

    if (str == NULL) {
        return ST7789_ERR_PARAM;
    }
    if (scale == 0) { scale = 1; }
    if (scale > 4)  { scale = 4; }

    cw  = (uint16_t)(ST7789_FONT_W * scale);
    chh = (uint16_t)(ST7789_FONT_H * scale);

    while (*str != '\0') {
        if (*str == '\r') {
            ++str;
            continue;
        }
        if (*str == '\n') {
            cx = x;
            cy = (uint16_t)(cy + chh);
            ++str;
            continue;
        }
        /* 右边界自动折行 */
        if ((uint32_t)cx + cw > s_width) {
            cx = x;
            cy = (uint16_t)(cy + chh);
        }
        /* 到底了就不再画, 避免无意义的总线开销 */
        if ((uint32_t)cy + chh > s_height) {
            break;
        }

        st7789_status_t st = st7789_draw_char(cx, cy, *str, fg, bg, scale);
        if (st != ST7789_OK) {
            return (int)st;
        }
        cx = (uint16_t)(cx + cw);
        ++drawn;
        ++str;
    }
    return drawn;
}

/* ============================ 工具 ============================ */

st7789_color_t st7789_color565(uint8_t r, uint8_t g, uint8_t b)
{
    return (st7789_color_t)(((uint16_t)(r & 0xF8) << 8) |
                            ((uint16_t)(g & 0xFC) << 3) |
                            ((uint16_t)(b) >> 3));
}
