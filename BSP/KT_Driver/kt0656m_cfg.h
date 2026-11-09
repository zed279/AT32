/**
 * @file    kt0656m_cfg.h
 * @brief   KT0656M 产品参数与板级配置 —— 换一个产品只改这个文件
 *
 * @note    分层职责：
 *            kt0656m_reg.h  寄存器地图（芯片知识，跨产品不变）
 *            kt0656m_cfg.h  产品参数（本文件，换产品改这里）
 *            kt0656m.h/.c   控制逻辑（跨产品不变）
 *
 *          移植到新板子需要改三处：
 *            1. 平台宏 KT_DELAY_MS / KT_EN_PORT_CLK_EN
 *            2. KT_CFG_CHIP_NUM / KT_CFG_PHYS_NUM 与 s_kt_devs[] 实例表
 *            3. 下面的射频/音效参数
 *
 *          ===================== 当前板子：双组 KT =====================
 *          两组完全独立：各自一条软件 IIC 总线、各自一根 CHIP_EN。
 *
 *            组1 (KT_CH_A)                        组2 (KT_CH_B)
 *            --------------------------------     --------------------------------
 *            SCL      PB13                        SCL      PA9
 *            SDA      PB14                        SDA      PA10
 *            CHIP_EN  PB12  高有效                CHIP_EN  PA8   高有效
 *            INT      PB11  (本版不启用)          INT      PB15  (本版不启用)
 *            KT0656M  ADDR 接高 -> 0x39           KT0656M  ADDR 接高 -> 0x39
 *            KT0650   ADDR 接高 -> 0x3D           KT0650   ADDR 接高 -> 0x3D
 *
 *          同组内 0656M 与 0650 共用一根 CHIP_EN（"从机使能脚与主机一致"），
 *          所以上下电只能按组做，见 kt0656m_bus.c 的组级电源状态。
 *          跨组地址重复是安全的：两条独立总线，物理上不在一根线上。
 *          LINE_IO 组内连接两颗芯片，分集判决在 KT0656M 内部完成，MCU 不参与。
 *
 * @note    组2 的 SCL/SDA 用 PA9/PA10，这两个脚正好是 AT32 BSP 的 USART1 打印脚
 *          （at32f403a_407_board.h: PRINT_UART_TX_PIN = GPIO_PINS_9）。
 *          本板没引出串口，main.c 已删除 uart_print_init() 调用，日志也全关，
 *          所以 USART1 不会被使能。**如果以后要恢复串口打印，必须换到别的
 *          USART 或改走 SWO，不能再用 PA9。**
 */
#ifndef KT0656M_CFG_H_
#define KT0656M_CFG_H_

#include <stdint.h>
#include "at32f403a_407.h"
#include "at32f403a_407_board.h"    /* delay_ms() */
#include "iic_cfg.h"

/* ============================ 0. 平台钩子 ============================ */
/* 移植到别的 MCU 时只改这两处：把阻塞延时和 GPIO 时钟换成对应实现 */
#define KT_DELAY_MS(ms)         delay_ms((uint16_t)(ms))

#define KT_EN_PORT_CLK_EN(port) do {                                          \
        if ((port) == GPIOA)      crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE); \
        else if ((port) == GPIOB) crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE); \
        else if ((port) == GPIOC) crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE); \
        else if ((port) == GPIOD) crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE); \
    } while (0)

/* ============================ 1. 芯片数量与拓扑 ============================ */
/**
 * @brief 通道/分集拓扑
 * @note  两个数字含义不同，别混用：
 *          KT_CFG_CHIP_NUM  逻辑通道数（业务层 kt0656m_dev(ch) 的 ch 取值 0..N-1）
 *          KT_CFG_PHYS_NUM  物理芯片数（= 实例表 s_kt_devs[] 的项数）
 *        带天线分集时一个逻辑通道对应两颗物理芯片（主路 KT0656M + 从路 KT0650），
 *        所以本板 CHIP_NUM=2 而 PHYS_NUM=4。把 CHIP_NUM 直接改成 4 是错的：
 *        那会让 KT_CH_A/KT_CH_B 变成"四颗芯片"，与"两个通道"的语义冲突。
 *
 *        地址规则（两份 datasheet 4.10 / 4.17）。按【8 位地址字节】编号：
 *            bit4 = ADDR 引脚电平（高 1 / 低 0）
 *            bit3 = 芯片型号（KT0656M 恒 0 / KT0650 恒 1）
 *        换算成 7 位地址就是 b3 = ADDR、b2 = 型号。
 *        本板四颗芯片 ADDR 全部接高，故主路恒 0x39、从路恒 0x3D；
 *        两组走不同总线，同地址不构成冲突。
 */
#define KT_CFG_CHIP_NUM         2       /* 逻辑通道数：A / B 两组 */
#define KT_CFG_DIVERSITY        1       /* 1 = 每组带分集从机 KT0650（真分集，判决在芯片内） */
#if (KT_CFG_DIVERSITY)
#define KT_CFG_PHYS_NUM         4       /* 物理芯片：2 × (KT0656M + KT0650) */
#else
#define KT_CFG_PHYS_NUM         2
#endif

/* 器件角色：区分同一个通道下的两颗芯片 */
#define KT_ROLE_MAIN            0u      /* KT0656M：音频输出、内部分集判决 */
#define KT_ROLE_SUB             1u      /* KT0650：只解调 + 经 LINE_IO 上报，无音频输出 */

/* 7 位地址（原厂 8 位地址 >>1），四颗芯片 ADDR 全部接高 */
#define KT_ADDR_MAIN            0x39u   /* 7'b0111001  KT0656M ADDR 高  原厂 0x72 */
#define KT_ADDR_SUB             0x3Du   /* 7'b0111101  KT0650  ADDR 高  原厂 0x7A */

/* ============================ 2. 逻辑通道号 ============================ */
/**
 * @brief 业务层用 KT_CH_A / KT_CH_B 索引通道，不直接碰地址与引脚
 */
#define KT_CH_A                 0
#define KT_CH_B                 1

/* ============================ 3. 电源（CHIP_EN，按组） ============================ */
/**
 * @brief CHIP_EN 是【组级】资源，不是器件私有引脚
 * @note  同组内 KT0656M 与 KT0650 共用一根 EN：拉低一次同组两颗一起掉电，
 *        但组与组之间完全独立（组1 = PB12，组2 = PA8）。
 *        "这组当前是否已上电"是每组一个全局状态（见 kt0656m_bus.c 的 s_pwr_on[]）。
 *
 *        早期实现把 EN 挂在 dev 上：第二个器件 bus_open 时会再拉低 100ms，
 *        把第一个器件悄悄复位掉，而第一个器件的 ready 仍为 1。现象是
 *        "驱动以为在用，芯片其实已掉电"，之后所有读写都是 NACK 或垃圾值。
 *        现在由 kt0656m_power_on(ch) 的幂等 + 组级状态解决。
 *
 *        若将来某块板同组两颗芯片各有独立 EN，这里就要扩成器件级表，
 *        并把 dev 上的 pwr 编号加回来——那时"组"才成为多值。
 */
/* 组1 */
#define KT_EN_A_PORT            GPIOB
#define KT_EN_A_PIN             GPIO_PINS_12
/* 组2 */
#define KT_EN_B_PORT            GPIOA
#define KT_EN_B_PIN             GPIO_PINS_8

#define KT_EN_SETTLE_MS         20u     /* 手册 4.2：拉高后 >=10ms 才可访问 2-wire */
/* CHIP_EN 拉低保持时间。必须足够长，让芯片内部电源真正放电：
 * 只给几微秒等于没掉电，芯片会保留上一次的寄存器状态（见过被留在
 * "振荡器切到没焊晶体的 XO2" 而彻底失锁的现场）。 */
#define KT_EN_RESET_MS          100u

/* 兼容旧命名（单组时代调用点留下的宏），新代码请用上面的 A/B 版本 */
#define KT_EN1_PORT             KT_EN_A_PORT
#define KT_EN1_PIN              KT_EN_A_PIN
#define KT_EN1_SETTLE_MS        KT_EN_SETTLE_MS
#define KT_EN1_RESET_MS         KT_EN_RESET_MS

/* ============================ 3.1 中断脚（本版只留定义，不启用） ============================ */
/**
 * @brief KT_INT 引脚定义
 * @note  本版按需求"暂不接，先留定义"处理：只在这里登记引脚宏，
 *        **不配置 GPIO、不配置 EXINT、不参与任何流程**（芯片的 INT 输出
 *        自己会驱动这根线，MCU 侧保持复位后的浮空输入即可）。
 *        将来启用时注意：两组 INT 在不同引脚（PB11 / PB15），都落在
 *        EXINT15_10 共享向量上，中断里要靠 INT_FLAG 寄存器区分来源。
 */
#define KT_INT_A_PORT           GPIOB
#define KT_INT_A_PIN            GPIO_PINS_11
#define KT_INT_B_PORT           GPIOB
#define KT_INT_B_PIN            GPIO_PINS_15
#define KT_CFG_INT_EN           0       /* 0 = 本版不启用中断（只留定义） */

/* ============================ 3.2 总线 ============================ */
#define KT_IIC_BUS_A            IIC_BUS_KT_A    /* 组1：PB13/PB14 */
#define KT_IIC_BUS_B            IIC_BUS_KT_B    /* 组2：PA9/PA10  */

#define KT_IIC_TIMEOUT_MS       20u     /* 单次 2-wire 传输超时 */

/* 寄存器轮询超时（原厂全是 while 死等，这里给上界，超时返回错误码） */
/* 上电完成等待。手册 4.2 只保证"CHIP_EN 高 10ms 后可以访问 2-wire"，
 * 但实测芯片真正就绪比这晚：20ms 时寻址还 NACK，POWER_ON_FINISH 也是 0。
 * 所以这里不用固定延时，按状态等。 */
#define KT_TO_BOOT_MS           300u    /* 等 I2C 从机开始 ACK */
#define KT_TO_POWER_ON_MS       300u    /* 等 POWER_ON_FINISH */
/* 手册 4.8：CHAN_VALID 置 1 后，PLL_READY 最迟 260ms 才置高。
 * 低于这个值必然误报 KT_ERR_TUNE_PLL_LOCK —— 曾按 20ms 配，tune 每次必失败。
 * 放宽到 500ms 不影响正常情况：锁上就立刻返回，只有真失败时才多等。 */
#define KT_TO_PLL_DONE_MS       500u
#define KT_TO_LOBAND_CALI_MS    300u    /* 等 LOBAND 校准扫描完成 */
#define KT_TO_VCOCNT_MS         50u     /* 等 VCOCNT_RDY */
#define KT_TO_STEP_MS           10u     /* 轮询步进 */

/* ============================ 4. 晶振 ============================ */
/* 手册 4.3 原文要点：只焊一颗晶体时**必须接 XI-XO1**，并把 XTAL1_SEL 设成实际频率。
 *
 * 手册 4.18.1 ANA_CFG (0x0012)
 *   bit7 XTAL_SEL    0 = 晶体接 XI-XO1（默认）  1 = 接 XI-XO2
 *
 * 手册 4.18.17 SW_CFG8 (0x0108)
 *   bit7 XTAL1_SEL   XI-XO1 上的晶体频率：0 = 24MHz  1 = 24.576MHz
 *   bit6 XTAL2_SEL   XI-XO2 上的晶体频率：0 = 24MHz  1 = 24.576MHz（默认 1）
 *   bit5            手册标 Reserved，原厂内部命名 HLSI_INV
 *
 * 本板（原理图 + 示波器双重确认）：晶体接 XI-XO1，24.576MHz。
 * 取值与原厂 XTAL_24P576M_ONLY 分支一致 → 0x0108 = 0x80，0x0012 = 0x00。
 * 三个宏必须配对，配错等于把振荡器接到没焊晶体的那组脚 → 无参考时钟 → PLL 永不锁。
 */
#define KT_XTAL_ON_XO2          0       /* 0 = 晶体接 XI-XO1   1 = 接 XI-XO2 */
#define KT_XTAL_SEL1            1       /* XO1 上晶体：0=24MHz  1=24.576MHz */
#define KT_XTAL_SEL2            0       /* XO2 上晶体：0=24MHz  1=24.576MHz（XO2 未用） */

#define KT_HLSI_INV             0       /* 0: HLSI  1: inv(HLSI) */

/* ============================ 5. 射频参数 ============================ */
/* 注意：本文件所有"产品开关"一律带 KT_CFG_ 前缀，
 * 与 kt0656m_reg.h 里的位掩码（KT_DCDC_EN / KT_AUXDATA_EN 等）严格区分，
 * 否则同名宏会在预处理阶段互相覆盖。 */
#define KT_CFG_DCDC_EN          1       /* 芯片内部 DCDC：0 关 / 1 开 */

/* 频段范围（单位 KHz）
 *
 * 芯片 RF 能力是 470~960MHz（KT0656M datasheet），下面的上下界**不是芯片限制**，
 * 而是软件挡板：kt0656m_tune() / tune_fast() / scan() 用它挡住非法频率。
 *
 * 本板麦克风工作在 640~690MHz，所以挡板按它设。
 * 注意检查用的是 [BOTTOM_CHA, TOP_CHB] 这一整个连续区间，
 * 所以 A/B 两组共用同一范围，不再像原 WS19 那样切成两个子频段。
 *
 * 改成别的频段只需要动这四个数，驱动内部无需修改：
 * 频道字就是 freq_khz 的 20 位整数（见 write_channel），470000~960000 都能表示。 */
#define KT_BAND_BOTTOM_CHA      640000u
#define KT_BAND_TOP_CHA         690000u
#define KT_BAND_BOTTOM_CHB      640000u
#define KT_BAND_TOP_CHB         690000u

/* 频道网格步进。**当前代码里没有任何地方引用**，只作为频道规划参考保留。
 * 原厂驱动用的是 50KHz（BAND_STEP_RX），芯片本身不限制步进，
 * 实际网格取决于发射机的频道表。 */
#define KT_BAND_STEP            300u

/* AFC：2'b00 ±20 / 01 ±40 / 10 ±60 / 11 ±90 kHz */
#define KT_AFC_RNG_SEL          2u

/* 鉴频带宽：0:75K 1:50K 2:37.5K 3:25K 4:20K 5:120K 6:100K */
#define KT_ADJ_GAIN_SEL         1u

/* 自动静音 */
#define KT_CFG_AUTOMUTE_EN          1
#define KT_CFG_FAST_RSSI_MUTE_EN    1
#define KT_CFG_FAST_RSSI_PD_MUTE_EN 1
#define KT_CFG_SUPER_RSSI_MUTE_EN   1
#define KT_FAST_RSSI_MUTE_TH        0x89u
#define KT_AUTOMUTE_SNR_LOW         0x78u   /* 低于此值静音 */
#define KT_AUTOMUTE_SNR_HIGH        0x80u   /* 高于此值解除 */

/* 导频检测：产品需要靠导频判断发射端是否开机时打开 */
#define KT_CFG_PILOT_DETECT     0

/* BPSK 辅助数据 */
#define KT_BPSK_NEW_MODE_EN     1
#define KT_CFG_AUXDATA_EN       1
#define KT_CARRY_NUM_TH         4       /* 3'b000:10 ... 3'b111:24 */
#define KT_AUX_CARRY_NUM        3       /* 3'b000:32 ... 3'b111:12 */

/* ============================ 6. 音效默认值 ============================ */
/* 手册 4.13.3 DE_EXP_CFG (0x256) bit[6:4] = EXP_TC 扩展器时间常数
 *   000:6ms 001:12ms 010:24ms 011:48ms 100:93ms 101:199ms 110:398ms 111:796ms
 * 注意：这不是"温漂补偿档位"，旧注释写错了，含义是扩展器（expander）的
 *       检波时间常数，直接影响小信号时的增益爬升速度。 */
#define KT_EXP_TC_VAL           1       /* 001 = 12ms */
/* 旧名字保留，避免调用点全改；语义见上面 */
#define KT_COMP_TC_VAL          KT_EXP_TC_VAL

#define KT_ECHO_STRU            1       /* 0 全通 / 1 梳状 */
#define KT_ECHO_GAIN_DOWN       0       /* 0:-13dB 1:-10dB 2:-7dB */
#define KT_ECHO_GAIN_UP         7       /* 7:13.1dB */
#define KT_ECHO_RATIO_DEF       10      /* 0~25 → 0~25/32 */
#define KT_ECHO_DELAY_DEF       23      /* 0~23 → 22~197ms */

#define KT_EXCITER_TUNE_DEF     2       /* 0:600Hz 1:1K 2:2K 3:3.8K */
#define KT_EXCITER_DRIVE_DEF    0       /* 0:0dB ... 5:15dB */
#define KT_EXCITER_ODD_DEF      1
#define KT_EXCITER_EVEN_DEF     1

#define KT_VOLUME_MAX           31u     /* 0 = 静音 */
#define KT_VOLUME_BOOT          0u      /* 上电默认静音，等业务层拉起来 */

/* ============================ 7. 数字音频接口 ============================ */
/**
 * @brief I2S/SAI 输出配置
 * @note  本板两组都做主（各组独立输出音频），格式按 24bit I2S 标准。
 */
#define KT_CFG_I2S_EN           1

/* 从模式格式字：0x36 = stereo + LR_SEL=1 + i2s 标准 24bit */
#define KT_I2S_FMT_SLAVE        0x36u
/* 主模式格式字与 mclk 比：0x20 = LR_SEL=0，0x10 = mclk 12M / lrclk 128 */
#define KT_I2S_FMT_MASTER       0x20u
#define KT_I2S_CLK_MASTER       0x10u

#endif /* KT0656M_CFG_H_ */
