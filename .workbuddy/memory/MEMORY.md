# AT32F403A_Project 长期笔记

## 工程结构
- `bm_arch/` 裸机框架（时间片调度 + ISR 安全消息队列 + 软定时器 + 分级日志）
- `BSP/` 驱动层，按外设分目录：`GPIO_Driver` `SPI_Driver` `IIC_Driver` `KT_Driver` `Encoder_Driver` `LCD_Driver`
- `app/` 应用与 Demo，`mdk_v5/` Keil 工程（ARMCC v5，需 `uC99`=1）
- MCU：AT32F403A（LQFP100），与 STM32 有差异（DMA 请求固定映射、GPIO 枚举名不同）

## 分层约定（复用即分层边界）
- `*_reg.h` 芯片知识层（手册位域 → 宏，跨产品不变）
- `*_cfg.h` 产品参数层（换产品只改这里）
- `*.c/.h` 控制逻辑层（跨产品不变）
- `*_priv.h` 模块私有，不外暴露

## KT 双组接收方案（分集）
- 主 `KT0656M`（QFN32，音频输出 + 音效 + 分集判决）+ 从 `KT0650`（QFN24，只解调，经 LINE_IO 上报）
- **两组完全独立**，各一条软件 IIC 总线 + 各一根 CHIP_EN（同组主从共用使能）

| | 组1 (KT_CH_A) | 组2 (KT_CH_B) |
|---|---|---|
| SCL | PB13 | PA9 |
| SDA | PB14 | PA10 |
| CHIP_EN | PB12 | PA8 |
| INT | PB11（本版未启用） | PB15（本版未启用） |
| 主 0656M 地址 | 0x39（ADDR 高） | 0x39 |
| 从 0650 地址 | 0x3D（ADDR 高） | 0x3D |

- 四颗芯片 ADDR 全接高，两组独立总线所以同地址不冲突
- 驱动分层：`kt0656m_reg.h` 芯片知识 / `kt0656m_cfg.h` 产品参数 / `kt0656m_bus.c` 总线+组级电源 / `kt0656m.c` 控制逻辑
- IIC 移植层：`iic_port_at32.c`（DWT 延时 + AT32 gpio_init/gpio_bits_set），句柄编码 `(port<<8)|pin`
- 应用层：`app/Src/kt_demo.c`，LCD 显示两组主/从的地址、CHIP_ID、FW、ATC/INI/TUN 结果
- **详细知识卡：`Docs/KT/KT06xx_芯片知识卡.md`**
- 手册：`Docs/KT/KT0656M_datasheet_V0.7.8_CN.pdf`、`KT0650_datasheet_V0.1.2_CN.pdf`

### 引脚硬冲突（已处理，勿回退）
- **PA9/PA10 = USART1（AT32 BSP 的 `PRINT_UART_TX_PIN` = GPIO_PINS_9）**
- 组2 的 SCL/SDA 正好占这两个脚 → `main.c` 已删除 `uart_print_init()` 调用，
  `logger_cfg.h` 全部日志级别与 `LOG_USE_PRINTF` 置 0
- **以后要恢复串口打印，必须换 USART 或改 SWO，不能再用 PA9**
- `iic_cfg.h` 里原来的 `IIC_BUS_OLED`(PA6/PA7) 已删除：PA6=TFT_A0/DC、PA7=TFT_SDA 被 LCD 占用

### KT 关键时序（勿改小）
- CHIP_EN 拉高 → ≥10ms 才能访问 2-wire；拉低要 ≥100ms 让片内放电（否则寄存器状态残留）
- 等 `POWER_ON_FINISH`(0x7F bit0)，实测 20ms 时仍 NACK，需按状态等到 ~300ms
- `CHAN_VALID` 置 1 后 PLL 最迟 **260ms** 锁；超时阈值须 >260ms（曾配 20ms 导致 tune 必失败）
- DCDC_EN(0x2D bit2) 必须在 CHIP_EN 高后 10~60ms 内置 1

### 双组改造中的两个驱动修正
- `in_fw_check` 加角色过滤：KT0650 手册无 0x010F，用 0656M 版本号校验它会让从路 init 必报 FW_MISMATCH
- `kt0656m_power_*` 全部加 `ch` 参数；`s_pwr_on` 改为按 ch 索引的数组，掉电只清同组 ready

### KT 手册坑（已记录在知识卡 §17-18）
- `0x0012 bit7 XTAL_SEL` 与 `0x0108 bit7/6` 必须配对，单晶体接 XI-XO1
- 16bit 寄存器先读高字节（芯片靠此锁存低字节）
- 中断标志写 0 清
- `ADJUST_GAIN` 编码不连续（0~4 递减到 20k，5/6 跳回 120k/100k）
- EQ 0dB = `01100`；SAI 从模式 DSIZE `11` = 8bit
- 两颗芯片 CHIP_ID 同为 0x4B54，只能靠 I2C 地址型号位区分
- **datasheet 只公开配置层**；射频校准/VCO 频段/LDO/BIAS 约 20 个地址只在原厂参考代码里，改它们无文档兜底

## 已解决的历史坑
- HC-SR04 供电不足（5V 供 3.3V 模块）
- OLED SDA/SCL 接反
- `gpio_cfg.h` 映射表漏条目 → `gpio_init()` 静默失败
- ST7789 CS 提前拉高截断末字节 → DMA 完成后须等 `SPI_I2S_BF_FLAG` 清零
- TMR4 编码器库函数不设输入滤波，需直写 `c1df/c2df`

## 当前状态
- `mdk_v5` 最近一次编译：0 Error / 0 Warning（Code≈19382）
- LCD 驱动 + 编码器正交解码（TMR4 / PB6+PB7）已验证通过
