/**
 * @file    st7789.h
 * @brief   ST7789 (240x320 TFT, SPI 4线) 驱动 - 公共 API
 * @note    分层说明:
 *          - 本驱动只依赖 BSP/GPIO_Driver 与 BSP/SPI_Driver 两个抽象层,
 *            不直接操作任何寄存器, 换 MCU 只要那两个驱动移植好即可复用.
 *          - 引脚: RESET/DC 走 GPIO 驱动, CS 由 SPI 驱动内部管理,
 *            SCK/MOSI 由硬件 SPI 产生.
 *
 *          典型调用顺序 (业务层, 见 APP/lcd_app.c):
 *            st7789_init();                       // 引脚+SPI+硬件复位+初始化序列+开背光
 *            st7789_fill(ST7789_BLACK);           // 清屏
 *            st7789_draw_string(8, 8, "HELLO", ST7789_WHITE, ST7789_BLACK, 1);
 *
 *          接线 (本工程默认):
 *            PA4 -> RESET, PA5 -> DC/A0, PA6 -> CS, PA7 -> BL
 *            PB10 -> SCK, PC3 -> MOSI
 */
#ifndef ST7789_H_
#define ST7789_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ============================ 屏幕参数 ============================ */
#define ST7789_WIDTH            240
#define ST7789_HEIGHT           320

/**
 * @brief 显示区偏移 (像素)
 * @note  ST7789 内置 240x320 GRAM, 240x320 屏通常无偏移 (0,0).
 *        若上电后画面整体偏移/边缘花屏, 按实际屏改这两个值
 *        (常见带偏移的型号: 240x240 圆角屏 y=80, 135x240 屏 x=52/y=40).
 */
#define ST7789_X_OFFSET         0
#define ST7789_Y_OFFSET         0

/**
 * @brief 背光有效电平
 * @note  1 = 高电平点亮 (BL 直连 LED 阳极, 最常见)
 *        0 = 低电平点亮 (模块上带三极管/MOS 反相驱动)
 */
#define ST7789_BL_ACTIVE_LEVEL  1

/* ============================ RGB565 常用颜色 ============================ */
typedef uint16_t st7789_color_t;

#define ST7789_BLACK            0x0000
#define ST7789_WHITE            0xFFFF
#define ST7789_RED              0xF800
#define ST7789_GREEN            0x07E0
#define ST7789_BLUE             0x001F
#define ST7789_YELLOW           0xFFE0
#define ST7789_CYAN             0x07FF
#define ST7789_MAGENTA          0xF81F
#define ST7789_ORANGE           0xFD20
#define ST7789_GRAY             0x8410
#define ST7789_DARKGRAY         0x4208
#define ST7789_NAVY             0x000F
#define ST7789_LIGHTGRAY        0xC618

/* ============================ 返回码 ============================ */
typedef enum {
    ST7789_OK          =  0,   /* 成功 */
    ST7789_ERR_PARAM   = -1,   /* 参数非法 (坐标越界 / 尺寸为 0 / 缓冲为空) */
    ST7789_ERR_GPIO    = -2,   /* GPIO 驱动初始化失败 (多为映射表漏条目) */
    ST7789_ERR_SPI     = -3,   /* SPI 传输失败 */
} st7789_status_t;

/* ============================ 显示方向 ============================ */
typedef enum {
    ST7789_ROT_0   = 0,   /* 竖屏, 240x320, 排线在下 */
    ST7789_ROT_90  = 1,   /* 横屏, 320x240 */
    ST7789_ROT_180 = 2,   /* 竖屏倒置 */
    ST7789_ROT_270 = 3,   /* 横屏倒置 */
} st7789_rot_t;

/* ============================ 初始化 / 控制 ============================ */

/**
 * @brief  初始化 ST7789
 * @note   内部依次完成: GPIO 引脚初始化 -> SPI 总线初始化 -> 硬件复位
 *         -> 写初始化序列 -> 开背光 -> 清屏为黑色.
 *         调用前请确认 CubeMX 已生成并调用过 MX_SPI2_Init().
 * @retval ST7789_OK / ST7789_ERR_GPIO / ST7789_ERR_SPI
 */
st7789_status_t st7789_init(void);

/**
 * @brief  设置显示方向 (会重新写 MADCTL, 之后的坐标与宽高按新方向计算)
 */
st7789_status_t st7789_set_rotation(st7789_rot_t rot);

/**
 * @brief  背光开关
 */
void st7789_backlight(bool on);

/**
 * @brief  取当前方向下的逻辑宽度 / 高度
 */
uint16_t st7789_get_width(void);
uint16_t st7789_get_height(void);

/* ============================ 绘图 API ============================ */

/**
 * @brief  用单色填满整屏
 */
st7789_status_t st7789_fill(st7789_color_t color);

/**
 * @brief  填充矩形块
 * @param  x,y      左上角坐标 (逻辑坐标)
 * @param  w,h      宽高 (像素, 不能为 0)
 * @param  color    RGB565 颜色
 * @note   超出屏幕的部分自动裁剪, 完全在屏外时返回 ST7789_OK 且不做任何事
 */
st7789_status_t st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 st7789_color_t color);

/**
 * @brief  画一个像素
 */
st7789_status_t st7789_draw_pixel(uint16_t x, uint16_t y, st7789_color_t color);

/**
 * @brief  画水平线 / 垂直线
 */
st7789_status_t st7789_draw_hline(uint16_t x, uint16_t y, uint16_t len,
                                  st7789_color_t color);
st7789_status_t st7789_draw_vline(uint16_t x, uint16_t y, uint16_t len,
                                  st7789_color_t color);

/**
 * @brief  画空心矩形边框
 * @param  thickness 线宽 (像素)
 */
st7789_status_t st7789_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 uint8_t thickness, st7789_color_t color);

/**
 * @brief  块刷新 - 直接推送一段已按大端排列的 RGB565 像素流 (给 LVGL 用)
 * @param  x,y     左上角坐标
 * @param  w,h     块宽高 (像素)
 * @param  px_be   像素缓冲, 每个像素 2 字节, 高字节在前
 *                 (即 ST7789 期望的 SDI 字节顺序)
 * @note   本函数不做颜色转换也不拷贝: 内部直接把 px_be 按行喂给 SPI,
 *         调用方负责把行宽对齐 (缓冲必须包含 w*h*2 连续字节).
 *         纯单事务完成: CS 低 -> CASET/RASET/RAMWR -> h 行数据 -> CS 高.
 */
st7789_status_t st7789_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const uint8_t *px_be);

/* ============================ 文字 API (内置 8x16 ASCII) ============================ */

/**
 * @brief  画一个 ASCII 字符
 * @param  x,y    左上角坐标
 * @param  ch     字符 (0x20~0x7E; 范围外按空格处理)
 * @param  fg     字色
 * @param  bg     底色 (画成不透明方块, 便于覆盖旧内容)
 * @param  scale  放大倍数 (1~4, 1 = 8x16 像素)
 * @note   单字占位 = (8*scale) x (16*scale) 像素
 */
st7789_status_t st7789_draw_char(uint16_t x, uint16_t y, char ch,
                                 st7789_color_t fg, st7789_color_t bg,
                                 uint8_t scale);

/**
 * @brief  画字符串
 * @param  scale  放大倍数 (1~4)
 * @note   支持 '\n' 换行 (回到 x 起点并下移一行);
 *         遇到屏右边界自动折行 (不依赖 '\n').
 * @retval 实际绘制的字符数 (负数表示错误码)
 */
int st7789_draw_string(uint16_t x, uint16_t y, const char *str,
                       st7789_color_t fg, st7789_color_t bg,
                       uint8_t scale);

/* ============================ 工具 ============================ */

/**
 * @brief  RGB888 -> RGB565 转换
 */
st7789_color_t st7789_color565(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
#endif /* ST7789_H_ */
