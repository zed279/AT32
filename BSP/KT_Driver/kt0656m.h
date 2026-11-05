/**
 * @file    kt0656m.h
 * @brief   KT0656M 无线麦克风接收芯片驱动 —— 公共 API
 *
 * @note    分层：
 *            kt0656m_bus.c   总线层：dev 句柄 → iic 抽象，唯一的寄存器读写出口
 *            kt0656m.c       控制层：上电时序、初始化、调谐、音效、状态读取
 *
 *          设计要点（相对原厂 KT_WirelessMicRxdrv.c 的改动）：
 *            1. 消灭了原厂的 chipSel 全局游标。原厂每个函数隐式读全局变量决定
 *               "现在操作哪颗芯片"，不可重入、多通道时极易串台。本驱动所有 API
 *               都显式接收 kt0656m_dev_t*，多颗芯片可安全交替调用。
 *            2. 消灭了原厂的 I2C_Byte_Write/Read 外部符号。总线走 BSP/IIC_Driver，
 *               换 MCU 只改 iic_cfg.h，本驱动不动。
 *            3. 消灭了原厂 20+ 处 while 死等。全部改为带超时的轮询，失败返回
 *               KT_ERR_TIMEOUT，不再让 MCU 挂在 IIC 掉线现场。
 *            4. 消灭了原厂的满篇寄存器魔数。全部具名宏，见 kt0656m_reg.h。
 *            5. 音效/射频参数从 .h 的宏搬迁到 kt0656m_cfg.h，驱动代码里不出现
 *               产品级数值。
 *
 *          典型用法：
 *            kt0656m_dev_t *dev = kt0656m_dev(KT_CH_A);
 *            if (kt0656m_init(dev) != 0) { ... }
 *            kt0656m_tune(dev, 750000);         // 750.000 MHz
 *            kt0656m_volume_set(dev, 20);
 *            // 主循环里定期:
 *            kt0656m_patch(dev);                // 原厂 bug 修复 + 中断控制
 *            kt0656m_pkg_sync_poll(dev, &synced);
 */
#ifndef KT0656M_H_
#define KT0656M_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "kt0656m_cfg.h"
#include "kt0656m_reg.h"
#include "iic.h"

/* ============================ 返回码 ============================ */
/* 0 = 成功；-1 ~ -8 复用 iic_status_t；-20 起为本驱动私有 */
#define KT_OK                    0
#define KT_ERR_NOT_READY       (-20)   /* 未上电 / 未初始化 / CHIP_EN 未拉高 */
#define KT_ERR_ID_MISMATCH     (-21)   /* CHIP_ID 不是 0x4B54 */
#define KT_ERR_FW_MISMATCH     (-22)   /* 固件版本不是 0x11，后续寄存器表不匹配 */
#define KT_ERR_PARAM           (-23)   /* 入参越界（音量/频点/EQ 段号等） */
#define KT_ERR_CHIP_STUCK      (-24)   /* 芯片 I2C ACK 但读回恒定垃圾值：多半是
                                        * CHIP_EN 没真的掉电，芯片卡在上一次的坏状态 */

/* 寄存器轮询超时直接复用 iic 层错误码，业务层只需判负值 */
#define KT_ERR_TIMEOUT          IIC_ERR_TIMEOUT

/* -30 起：分步失败码。把"tune 失败"细化到具体环节，产线日志能直接定位。
 * 与 iic 错误码的区别：这些码说明"总线没事，是芯片状态没达到预期"。 */
#define KT_ERR_TUNE_CHAN        (-30)   /* 写频道字 / chan_valid 阶段失败 */
#define KT_ERR_TUNE_PLL_LOCK    (-31)   /* PLL_DONE 超时：PLL 没锁到目标频点 */
#define KT_ERR_TUNE_VCO_CALI    (-32)   /* VCOCNT_RDY 超时：VCO 未起振或 band 校准不收敛 */
#define KT_ERR_INIT_LOBAND      (-34)   /* init: LOBAND 校准扫描超时 */

/**
 * @brief  错误码转可读字符串（覆盖 iic_status_t 与 KT 私有码）
 * @param  rc 任意驱动/总线返回码
 * @retval 常量字符串，未知码返回 "ERR_UNKNOWN"；不可释放
 */
const char *kt0656m_strerror(int rc);

/* ============================ 器件句柄 ============================ */
/**
 * @brief 单颗 KT0656M 的运行时上下文
 * @note  由 kt0656m_dev() 返回板级静态实例，业务层不要自己 malloc
 */
typedef struct kt0656m_dev {
    iic_bus_t       bus;            /* 逻辑总线号（iic_cfg.h） */
    uint8_t         addr;           /* 7 位从地址 */
    uint8_t         ch;             /* 逻辑通道号 KT_CH_A / KT_CH_B */
    uint8_t         role;           /* KT_ROLE_MAIN(主路 KT0656M) / KT_ROLE_SUB(从路 KT0650) */
    uint8_t         ready;          /* 1 = 所在电源组已上电，且本器件已过 ACK + POWER_ON 两道门闩 */
    /* 1 = 分集判决当前选中【主路】天线，0 = 选中【从路】天线。纯结果字段，
     * 给 UI 显示"现在用的是哪根天线"用，**不参与任何寄存器选择**。
     * 注意：这里的"主/从"指天线路由，与 I2C 的"主机/从机"毫无关系——
     *       I2C 上 KT0656M 与 KT0650 都是从机，MCU 才是主机。
     * 历史坑：这个字段曾经同时被用来决定 RSSI/SNR 读 0x020A 组还是 0x0221 组，
     *         导致"判决选了从路以后 snr_get() 读到未公开镜像"的隐蔽故障，
     *         已拆开：读自己用 kt0656m_*_get()，读另一路用 kt0656m_alt_*_get()。 */
    uint8_t         div_main_path;
    uint8_t         pkg_synced;     /* 最近一次包同步检测结果 */
} kt0656m_dev_t;

/**
 * @brief 探测结果
 */
typedef struct kt0656m_info {
    uint8_t scan_hit;       /* 1 = 总线上扫到了目标地址 */
    uint8_t chip_id_h;      /* 0x0192 */
    uint8_t chip_id_l;      /* 0x0193 */
    uint8_t fw_version;     /* 0x010F */
    uint8_t power_sta;      /* 0x007F */
    uint8_t id_ok;          /* 1 = CHIP_ID == 0x4B54 */
    uint8_t boot_ok;        /* 1 = POWER_ON_FINISH == 1 */
    uint8_t stuck;          /* 1 = 四个寄存器读回同一个字节：I2C 在 ACK 但数据是假的，
                             *     说明芯片内部时钟没起来（典型是没真正掉电复位） */
} kt0656m_info_t;

/* ============================ 实例获取 ============================ */
/**
 * @brief  取主路器件实例（KT0656M，音频输出、负责分集判决）
 * @param  ch  KT_CH_A / KT_CH_B（不存在返回 NULL）
 * @retval 静态实例指针，不要 free
 */
kt0656m_dev_t *kt0656m_dev(uint8_t ch);

/**
 * @brief  取从路器件实例（KT0650，分集辅助）
 * @param  ch  KT_CH_A / KT_CH_B
 * @retval 无分集或该通道没有从机时返回 NULL
 * @note   它与主路共用同一根 CHIP_EN，上下电见 kt0656m_power_on(ch)
 */
kt0656m_dev_t *kt0656m_dev_sub(uint8_t ch);

/**
 * @brief  调试用寄存器直读/直写（绕过一切语义，仅建议日志与产线自检使用）
 * @note   公有是因为排查现场必须能 dump 芯片内部状态；正常业务不要拿它配芯片
 * @retval KT_OK / 负的 iic_status_t / KT_ERR_NOT_READY
 */
int kt0656m_reg_read(kt0656m_dev_t *dev, uint16_t reg, uint8_t *val);
int kt0656m_reg_write(kt0656m_dev_t *dev, uint16_t reg, uint8_t val);

/**
 * @brief  探测某个 7 位地址上是否有器件应答
 * @note   只发 START + 地址字节 + STOP，不碰任何寄存器，对总线上其他器件
 *         零副作用。分集从机（KT0650）在建实例之前先用它确认"在不在"。
 * @retval KT_OK = 有应答；负的 iic_status_t = 无应答 / 总线故障；KT_ERR_PARAM
 */
int kt0656m_addr_probe(iic_bus_t bus, uint8_t addr);

/**
 * @brief  按地址直接读一个寄存器，不需要 kt0656m_dev_t 实例
 * @note   用于"器件还没进实例表、但想先看看它长什么样"的场合（如分集从机
 *         摸底）。只读，不会改动器件状态。业务层不必接触 iic 抽象。
 * @retval KT_OK / 负的 iic_status_t / KT_ERR_PARAM
 */
int kt0656m_reg_read_at(iic_bus_t bus, uint8_t addr, uint16_t reg, uint8_t *val);


/* ============================ 生命周期 ============================ */
/* 分两层：CHIP_EN 是【组级】资源（同组两颗芯片共用一根，组与组之间独立），
 * 所以"上下电"归组级，"能不能访问"归器件级。别把两件事混在一个函数里，
 * 否则第二个器件初始化时必然把第一个器件悄悄复位掉。
 *
 * 本板两根 EN：组1 = PB12（KT_CH_A），组2 = PA8（KT_CH_B）。
 * 所有上下电 API 都带 ch 参数，指定操作哪一组。
 *
 * 推荐的调用顺序（每组各做一遍）：
 *     kt0656m_power_on(KT_CH_A);          // 组1上电（幂等）
 *     kt0656m_bus_attach(main);           // 主路挂上总线
 *     kt0656m_init(main);  kt0656m_tune(main, 750000);
 *     kt0656m_bus_attach(sub);            // 从路挂上总线（主路不受影响）
 *     // 从路的射频前段配置尚未实现：不能复用 kt0656m_init()，它含 DC/DC、EQ、
 *     // Echo、Exciter、SAI、电池检测等 KT0650 根本没有的寄存器
 */

/**
 * @brief  指定组上电（幂等）：该组已上电则立即返回，绝不再拉低 CHIP_EN
 * @param  ch  KT_CH_A / KT_CH_B
 * @note   幂等是分集能工作的前提：同组多颗芯片分别调用也不会互相打断。
 *         不同组之间互不影响，可以同时上电。
 * @retval KT_OK / KT_ERR_PARAM（ch 越界）
 */
int kt0656m_power_on(uint8_t ch);

/**
 * @brief  指定组强制掉电重上：拉低 KT_EN_RESET_MS → 拉高 → 等 KT_EN_SETTLE_MS
 * @param  ch  KT_CH_A / KT_CH_B
 * @note   该组内所有器件的 ready 一律清零，另一组不受影响。
 *         调用方必须重新 attach 同组用到的每一颗，不能只 attach 自己那颗。
 * @retval KT_OK / KT_ERR_PARAM
 */
int kt0656m_power_cycle(uint8_t ch);

/**
 * @brief  指定组掉电（该组所有器件 ready 清零）
 * @param  ch  KT_CH_A / KT_CH_B
 */
void kt0656m_power_off(uint8_t ch);

/**
 * @brief  功耗/时序测量专用：把某组 CHIP_EN 直接拉到指定电平并保持 hold_ms
 *
 * @param  ch      操作哪一组（KT_CH_A / KT_CH_B）
 * @param  on      1 = 上电（CHIP_EN 高），0 = 掉电（CHIP_EN 低）
 * @param  hold_ms 电平建立后保持的毫秒数；0 = 不等待立即返回
 * @retval KT_OK / KT_ERR_PARAM
 *
 * @note   本函数**只写电平 + 等待**，不发起任何 I2C 访问、不轮询
 *         POWER_ON_FINISH、不打印。存在的理由：
 *           power_cycle() 的 100ms/20ms 写死在 cfg 里，测启动 inrush 时
 *         20ms 窗口看不全曲线；
 *           power_on() 成功后调用方往往立刻 bus_attach()，I2C 通信电流会
 *         叠在启动曲线上，分不清"芯片启动"和"总线活动"。
 *
 * @note   与正常上电流程共用同一份组级状态（s_pwr_on），两条路径不会打架：
 *           on=1 → s_pwr_on=1，但 dev->ready 保持 0（上电 != 已挂总线）。
 *                  之后调 kt0656m_bus_open() 时 power_on() 幂等直接跳过掉电，
 *                  直接进 bus_attach() 轮询 —— 正是想要的行为。
 *           on=0 → s_pwr_on=0 且整组 ready 清零。
 *
 * @note   **不做幂等判断，这是特性不是缺陷**：本函数只写电平，不检查"当前
 *         是否已经是该状态"。要测冷启动 inrush，调用方必须自己先 on=0 保持
 *         至少 KT_EN_RESET_MS，让芯片内部电源真正放电；否则测到的是
 *         "从上一次残留状态恢复"，峰值会明显偏小。
 *
 * @note   测量窗口内禁止调用 printf 或任何寄存器读写。115200 下每行约 3~4ms
 *         的 UART 发送电流、每次 I2C 传输的几 mA 都会污染读数。
 */
int kt0656m_power_hold(uint8_t ch, uint8_t on, uint32_t hold_ms);

/**
 * @brief  器件挂上总线：总线初始化 + 轮询寻址 ACK + 轮询 POWER_ON_FINISH
 * @note   不碰 CHIP_EN。要求先 kt0656m_power_on(dev->ch)，否则返回 KT_ERR_NOT_READY。
 * @retval KT_OK / KT_ERR_NOT_READY / 负的 iic_status_t
 */
int kt0656m_bus_attach(kt0656m_dev_t *dev);

/**
 * @brief  上电组 + 器件挂上总线（= power_on() + bus_attach()，幂等）
 * @note   单颗芯片的简单用例调它就行；多颗芯片交错调用也安全
 * @retval KT_OK / 负的 iic_status_t
 */
int kt0656m_bus_open(kt0656m_dev_t *dev);

/**
 * @brief  整组掉电（共享 CHIP_EN 的板上无法只关一颗）
 * @note   参数保留是为了调用习惯统一；实现上等价 kt0656m_power_off()
 */
void kt0656m_bus_close(kt0656m_dev_t *dev);

/**
 * @brief  硬复位：整组掉电重上，并把 dev 自己重新挂回总线
 * @note   故意打断芯片内部电源，清掉一切残留寄存器状态。业务层的"芯片状态异常"
 *         恢复手段，比重新跑 init 更彻底（init 不会复位那些被写坏、且手册标
 *         Reserved 的位）。复位后需重新 init + tune。
 *
 *         **副作用**：共享 CHIP_EN 时同组其他器件也一起掉电，它们的 ready 被清零，
 *         调用方必须重新 kt0656m_bus_open() 才能继续用。
 * @retval KT_OK / 负的 iic_status_t
 */
int kt0656m_hw_reset(kt0656m_dev_t *dev);

/**
 * @brief  连通性探测：扫地址 → 读 CHIP_ID / 固件版本 / 上电完成标志
 * @retval KT_OK / 负的 iic_status_t（总线层失败）
 * @note   返回 KT_OK 只代表"I2C 传输成功"，芯片身份要看 info->id_ok。
 *         info->stuck = 1 时是芯片没复位干净的典型信号，应先 kt0656m_hw_reset()。
 */
int kt0656m_probe(kt0656m_dev_t *dev, kt0656m_info_t *info);

/**
 * @brief  完整初始化：探测 → DCDC → 静音 → 射频/音效/BPSK 全量配置
 * @note   成功后 dev->ready = 1，可调用 tune/volume 等
 * @retval KT_OK / KT_ERR_ID_MISMATCH / KT_ERR_FW_MISMATCH / KT_ERR_TIMEOUT / iic 错误
 */
int kt0656m_init(kt0656m_dev_t *dev);


/* ============================ 调谐 ============================ */
/**
 * @brief  完整调谐（含 VCO band 温漂校准 + PLL 复位）
 * @param  freq_khz 接收频率，单位 kHz，例如 750000
 * @retval KT_OK / iic 错误 / KT_ERR_TUNE_PLL_LOCK / KT_ERR_TUNE_VCO_CALI
 * @note   耗时不定：手册 4.8 允许 PLL 最迟 260ms 才锁定，最坏情况接近 1s，
 *         不要在高频中断/音频回调里调用。
 */
int kt0656m_tune(kt0656m_dev_t *dev, uint32_t freq_khz);

/**
 * @brief  快速调谐（跳过校准），约 2~3ms，用于波段内小步进切换
 */
int kt0656m_tune_fast(kt0656m_dev_t *dev, uint32_t freq_khz);

/**
 * @brief  扫频模式调谐（只写频道字，不等待 PLL），用于自动搜台
 */
int kt0656m_tune_seek(kt0656m_dev_t *dev, uint32_t freq_khz);


/* ============================ 音量与音效 ============================ */
int kt0656m_volume_set(kt0656m_dev_t *dev, uint8_t vol);        /* 0~31，0=静音 */

int kt0656m_echo_enable(kt0656m_dev_t *dev, uint8_t en);
int kt0656m_echo_set(kt0656m_dev_t *dev, uint8_t ratio, uint8_t delay);

int kt0656m_exciter_enable(kt0656m_dev_t *dev, uint8_t en);
int kt0656m_exciter_set(kt0656m_dev_t *dev, uint8_t odd, uint8_t even);

int kt0656m_eq_enable(kt0656m_dev_t *dev, uint8_t en);
int kt0656m_eq_set_band(kt0656m_dev_t *dev, uint8_t band, uint8_t gain); /* band 0~14 */

int kt0656m_diversity_enable(kt0656m_dev_t *dev, uint8_t en);


/* ============================ 状态读取 ============================ */
/**
 * @brief  读信号类指标，失败时 *out 保持原值不变
 * @note   rssi/fast_rssi/snr 读的是**这颗芯片自己的**前端质量
 *         （主路 0x020A/0x020C/0x020D，从路 KT0650 同地址）。
 *         与 div_main_path 无关 —— 判决选了哪一路不影响"我自己的信号多强"。
 */
int kt0656m_af_get(kt0656m_dev_t *dev, uint8_t *out);        /* 音频幅度 0~15 */
int kt0656m_rssi_get(kt0656m_dev_t *dev, uint8_t *out);      /* 射频场强 */
int kt0656m_fast_rssi_get(kt0656m_dev_t *dev, uint8_t *out); /* 快速场强 */
int kt0656m_snr_get(kt0656m_dev_t *dev, uint8_t *out);       /* 信噪比 */
int kt0656m_automute_get(kt0656m_dev_t *dev, uint8_t *muted);/* 1 = 芯片在静音 */
int kt0656m_pilot_get(kt0656m_dev_t *dev, uint8_t *ok);      /* 1 = 检测到导频 */
int kt0656m_battery_read(kt0656m_dev_t *dev, uint16_t *code);/* 0~0x7FF，0x7FF≈1.2V */

/**
 * @brief  读【另一路天线】的质量，仅主芯片有效（对 KT0650 返回 KT_ERR_PARAM）
 * @note  对应 0x0221/0x0222/0x0223 —— 这三个地址两份 datasheet 都没有定义，
 *        是主芯片把从路经 LINE_IO 送来的信息映射出来的未公开镜像。
 *        语义未经手册确认，只可用于调试显示；做产品判据前先按文档 6.11 实测。
 */
int kt0656m_alt_rssi_get(kt0656m_dev_t *dev, uint8_t *out);
int kt0656m_alt_fast_rssi_get(kt0656m_dev_t *dev, uint8_t *out);
int kt0656m_alt_snr_get(kt0656m_dev_t *dev, uint8_t *out);

/**
 * @brief  分集【主路/从路】判决：比较两路快速 RSSI，把结果写回 dev->div_main_path
 * @note   与 I2C 主机/从机无关。只写结果字段，**不改变任何后续读取行为** ——
 *         RSSI/SNR 想读哪一路由调用方自己选（*_get 读自己，alt_*_get 读另一路）。
 *         无分集时恒为 1（主路），可安全调用。对从路调用也恒为 1。
 */
int kt0656m_select_antenna(kt0656m_dev_t *dev);

/**
 * @brief  包同步轮询：有同步中断时读状态并更新 dev->pkg_synced
 * @param  synced 输出 1=包同步 0=未同步，可为 NULL
 */
int kt0656m_pkg_sync_poll(kt0656m_dev_t *dev, uint8_t *synced);


/* ============================ 周期维护 ============================ */
/**
 * @brief  原厂 Patch 的等价实现：射频中断使能控制 + 导频 mute 修复 + SNR mute 修复
 * @note   建议业务主循环 10~50ms 调一次
 */
int kt0656m_patch(kt0656m_dev_t *dev);


/* ============================ 数字音频接口 ============================ */
/**
 * @brief  配置 I2S 输出（tune 会自动调用，一般不需要手工调用）
 */
int kt0656m_audio_if_init(kt0656m_dev_t *dev);

#ifdef __cplusplus
}
#endif
#endif /* KT0656M_H_ */
