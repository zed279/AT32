//********************************************************************
//* SGL Configuration File - AT32F403A + ST7789 (240x320, RGB565)     *
//*                                                                   *
//* 本文件是针对 AT32F403A_Project 的工程配置, 与上游 sgl_config.h    *
//* 的差异只有两类: "资源裁剪" 和 "字节序".                           *
//*   - MCU 内存有限, 堆按实际页面规模给, 不照抄 PC 版的 100KB        *
//*   - ST7789 走 SPI, 期望像素高字节先出, 故打开 COLOR16_SWAP        *
//* 修改这里任何一项都可能影响 sgl_port_at32.c 的缓冲, 两处要同步.     *
//********************************************************************

#ifndef  __SGL_CONFIG_H__
#define  __SGL_CONFIG_H__


/* ============================ 显示设备 ============================ */
/* 像素深度: ST7789 配 COLMOD=0x55 -> RGB565 */
#define  CONFIG_SGL_FBDEV_PIXEL_DEPTH                      16
/* 屏幕旋转: 0 = 240(宽) x 320(高) 竖屏, 与 st7789_set_rotation(ROT_0) 对应.
 * 若改成 90/270, 必须同步把 st7789 也切到对应方向, 否则坐标全错位. */
#define  CONFIG_SGL_FBDEV_ROTATION                         0
/* 不用整屏 VRAM (240*320*2 = 150KB, 放不下), 走"脏区 + 行缓冲"分段刷新 */
#define  CONFIG_SGL_USE_FBDEV_VRAM                         0
/* ST7789 经 SPI 接收像素时高字节先出; SGL 内部是 uint16 小端.
 * 打开后 flush 回调拿到的 buffer 已是可直接丢给 SPI 的字节流. */
#define  CONFIG_SGL_COLOR16_SWAP                           1

/* ============================ 系统时基 ============================ */
/* 与 sgl_port_task() 的调用周期一致 (BM Arch 的 TASK_UI = 10ms) */
#define  CONFIG_SGL_SYSTICK_MS                             10

/* ============================ 事件与脏区 ============================ */
/* 本工程暂无触摸屏, 事件队列只用于程序内部投递, 8 条足够 */
#define  CONFIG_SGL_EVENT_QUEUE_SIZE                       8
/* 脏区最多 16 块: 控件多的页面一次刷新会拆成多块, 太少会反复合并 */
#define  CONFIG_SGL_DIRTY_AREA_NUM_MAX                     16
#define  CONFIG_SGL_DIRTY_AREA_TRACE                       0

/* ============================ 调试 ============================ */
/* 本板没有可用调试串口 (PA9 被 KT 组2 的 I2C 占用, 见 main.c 注释),
 * 打开 DEBUG 只会白吃 flash, 故关闭. 需要查错时临时改 1 并调用
 * sgl_logdev_register() 注册到任意可输出的通道. */
#define  CONFIG_SGL_DEBUG                                  0
#define  CONFIG_SGL_LOG_LEVEL                              0
#define  CONFIG_SGL_LOG_COLOR                              0

/* ============================ 对象与字体 ============================ */
/* 对象名是 PC 端调试用的, MCU 上关掉 (每个对象省 4 字节指针 + 字符串) */
#define  CONFIG_SGL_OBJ_USE_NAME                           0
#define  CONFIG_SGL_FONT_COMPRESSED                        0
/* 开机 logo 是 SGL 自带动画, 占启动时间且额外链接一批绘制代码 */
#define  CONFIG_SGL_BOOT_LOGO                              0
#define  CONFIG_SGL_THEME_DARK                             0

/* ============================ 内存管理 ============================ */
/* 所有控件都从这块静态池分配. 一个典型页面(10~20 个控件)约 2~4KB,
 * 16KB 留了余量; 页面更多时先看 sgl_mm_get_monitor() 再调大. */
#define  CONFIG_SGL_HEAP_ALGO                              lwmem
#define  CONFIG_SGL_HEAP_MEMORY_SIZE                       16384

/* ============================ 字体 (每开一个都吃 flash) ============================ */
/* 【与 PC 模拟器 demo/sgl_config.h 逐项一致】这样在模拟器里随意用哪个字体,
 * 搬到板卡都不会链接失败. font 是 const, 只占 flash 不占 RAM, 本板 1MB flash
 * 目前才用 ~44KB, 全开这 5 个也就 ~130KB, 没必要为省 flash 给自己挖坑. */
/* consolas14: 小号 ASCII, 状态栏/说明文字 (~13KB) */
#define  CONFIG_SGL_FONT_CONSOLAS14                        1
/* consolas23: 中号 ASCII, SGL 官方示例用得最多的就是它 (~30KB) */
#define  CONFIG_SGL_FONT_CONSOLAS23                        1
/* consolas24: 大号 ASCII, 标题 (~40KB) */
#define  CONFIG_SGL_FONT_CONSOLAS24                        1
/* consolas32: 特大号, 数字大屏 (~55KB) */
#define  CONFIG_SGL_FONT_CONSOLAS32                        1
/* consolas24 压缩版: 需同时换字体源文件, 默认不用 */
#define  CONFIG_SGL_FONT_CONSOLAS24_COMPRESS               0
/* kai33: 本工程没有对应字体源文件, 必须 0 */
#define  CONFIG_SGL_FONT_KAI33                             0
/* song23: 23px 宋体, 工程里唯一的 CJK 字体, 中文界面必须开 (~15KB) */
#define  CONFIG_SGL_FONT_SONG23                            1

/* ============================ 绘制细节 ============================ */
/* 焦点框宽度: 后续用编码器导航时的高亮边框 */
#define  CONFIG_SGL_FOCUSED_WIDTH                          2
/* 图片双线性插值: 放大更平滑但每像素多几次乘加, MCU 上关掉 */
#define  CONFIG_SGL_PIXMAP_BILINEAR_INTERP                 0
/* 帧率/脏区监控叠层: 调试用, 量产关掉 */
#define  CONFIG_SGL_MONITOR_TRACE                          0

#endif  //!__SGL_CONFIG_H__
