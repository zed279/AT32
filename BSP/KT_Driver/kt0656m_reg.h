/**
 * @file    kt0656m_reg.h
 * @brief   KT0656M 寄存器地图 —— 纯芯片知识层（不含任何逻辑与工程参数）
 *
 * @note    本文件只做一件事：把 datasheet 里的寄存器地址和位域，变成可读的具名宏。
 *          任何"产品要配什么值"都不属于这里，那是 kt0656m_cfg.h 的事。
 *
 *          命名约定：KT_REG_xxx  = 寄存器地址
 *                    _SHIFT       = 位域起始位
 *                    _MSK         = 位域掩码（已左移到位）
 *
 *          手册依据：KT0656M_datasheet_V0.7.8_CN.pdf (Docs/)
 *            4.17  2-wire: 设备地址 7'b011x001，寄存器地址 16 位，数据 8 位
 *            4.18  MANUFACTURER_ID 0x192/0x193 = 0x4B/0x54
 */
#ifndef KT0656M_REG_H_
#define KT0656M_REG_H_

/* ============================ 标识 ============================ */
#define KT_REG_CHIP_ID_H            0x0192u  /* MANUFACTURER_ID<15:8> 固定 0x4B */
#define KT_REG_CHIP_ID_L            0x0193u  /* MANUFACTURER_ID<7:0>  固定 0x54 */
#define KT_REG_FW_VERSION           0x010Fu  /* FIRMWARE_VERSION，0x11 才继续初始化 */
#define KT_CHIP_ID_H_VAL            0x4Bu
#define KT_CHIP_ID_L_VAL            0x54u
#define KT_FW_VERSION_VAL           0x11u

/* ============================ 电源 / 时钟 / 偏置 ============================ */
#define KT_REG_POWER_STA            0x007Fu  /* bit0 POWER_ON_FINISH */
#define KT_PWR_ON_FINISH            (1u << 0)

#define KT_REG_DCDC_CFG             0x002Du  /* bit2 DCDC_EN, bit3 VTR_MONITOR_EN */
#define KT_DCDC_EN                  (1u << 2)
#define KT_VTR_MONITOR_EN           (1u << 3)

#define KT_REG_VTR_CFG              0x0010u  /* bit7 REF_VTR_VTH_SEL */
#define KT_REF_VTR_VTH_SEL          (1u << 7)

#define KT_REG_BIAS_CFG             0x001Fu  /* bit[6:5] 偏置电流档 */
#define KT_BIAS_0P4V                0x40u    /* 0.4V */

#define KT_REG_LDO_CURRENT          0x000Du  /* 0xCF = 6mA */
#define KT_LDO_CURRENT_6MA          0xCFu

#define KT_REG_XTAL_CFG             0x0108u  /* 手册 4.18.17 SW_CFG8
                                              * bit3 SCAN_MODE, bit4 LOBAND_CALI_SCAN_EN
                                              * bit5 HLSI_INV, bit6 XTAL2_SEL, bit7 XTAL1_SEL */
#define KT_SCAN_MODE                (1u << 3)
#define KT_LOBAND_CALI_SCAN_EN      (1u << 4)
#define KT_HLSI_INV_SHIFT           5
#define KT_XTAL_SEL2_SHIFT          6
#define KT_XTAL_SEL1_SHIFT          7

/* 手册 4.18.1 ANA_CFG：决定片外晶体接在 XI-XO1 还是 XI-XO2。
 * 必须与上面 0x0108 的 XTAL1_SEL/XTAL2_SEL 配对，配错 = 振荡器接空 = PLL 永远不锁。 */
#define KT_REG_ANA_CFG              0x0012u
#define KT_XTAL_SEL_BIT             (1u << 7)   /* 0: XI-XO1, 1: XI-XO2 */

/* ============================ PLL ============================ */
#define KT_REG_PLL_NOISE            0x0109u  /* 0x10 = PLL noise +3dB */
#define KT_PLL_NOISE_3DB            0x10u

#define KT_REG_CHAN_H               0x0045u  /* 频道字<19:12> */
#define KT_REG_CHAN_M               0x0046u  /* 频道字<11:4>  */
#define KT_REG_CHAN_L               0x0047u  /* bit[7:4] 频道字<3:0>, bit0 CHAN_VALID */
#define KT_CHAN_VALID               (1u << 0)

#define KT_REG_PLL_STA              0x0061u  /* bit0 PLL_DONE, bit1 LOMC_SEL,
                                              * bit[5:4] O_VCOCNT_WIN, bit6 S_VCOCNT_START */
#define KT_PLL_DONE                 (1u << 0)
#define KT_LOMC_SEL                 (1u << 1)
#define KT_VCOCNT_START             (1u << 6)
#define KT_VCOCNT_WIN_5             0x28u    /* 保留 bit[3:0]，写 0x8 */

#define KT_REG_RF_CFG               0x0017u  /* bit[2:0] LOCOARSE/LOFINE_VAR_SEL */
#define KT_REG_SDM_RST              0x0042u  /* bit2 DSP_RST, bit4 PLL_SDM_RST, bit5 VCOCNT_RST */
#define KT_DSP_RST                  (1u << 2)
#define KT_PLL_SDM_RST              (1u << 4)
#define KT_VCOCNT_RST               (1u << 5)

#define KT_REG_LO_LOCK              0x0030u  /* bit7 LO_LOCK_DET_PD */
#define KT_LO_LOCK_DET_PD           (1u << 7)

#define KT_REG_VCO_CALI_EN          0x001Cu  /* bit6 LO_VCO_BAND_CALI_EN */
#define KT_LO_VCO_BAND_CALI_EN      (1u << 6)

#define KT_REG_LO_DIV128            0x0016u  /* bit7 LO_DIV128_EN, bit6 LO_KVCO_CALI_EN,
                                              * bit[2:0] I_LO_KVCO_COARSE_VREF_SEL */
#define KT_LO_DIV128_EN             (1u << 7)
#define KT_LO_KVCO_CALI_EN          (1u << 6)
#define KT_KVCO_VREF_SEL_MSK        0x07u

#define KT_REG_LO_MMD               0x001Au  /* bit[5:0] O_LO_MMD_MC */
#define KT_LO_MMD_MC_32             0x20u

#define KT_REG_VCO_BAND_SEL         0x001Eu  /* R_LO_VCO_BAND_SEL，8 位 */
#define KT_REG_KVCO_VREF            0x0018u  /* bit[2:0] O_LO_KVCO_COARSE_VREF_SEL */
#define KT_REG_VCOCNT_STA           0x0064u  /* bit7 I_VCOCNT_RDY, bit[3:0] result<19:16> */
#define KT_VCOCNT_RDY               (1u << 7)
#define KT_REG_VCOCNT_RES_M         0x0065u
#define KT_REG_VCOCNT_RES_L         0x0066u
#define KT_REG_VCOFREQ_REF_H        0x0067u  /* bit[1:0] */
#define KT_REG_VCOFREQ_REF_M        0x0068u
#define KT_REG_VCOFREQ_REF_L        0x0069u
#define KT_REG_BAND_CALI_EN         0x005Cu  /* bit6 校准前置条件 */
#define KT_BAND_CALI_GATE           (1u << 6)

#define KT_REG_PLL_RESET_EN         0x0133u  /* bit3 I_PLL_RESET_EN, bit6 DLL_RST_EN */
#define KT_PLL_RESET_EN             (1u << 3)
#define KT_DLL_RST_EN               (1u << 6)

/* ============================ 射频 / AFC ============================ */
#define KT_REG_RFAMP_INT            0x0053u  /* bit6 RFAMP_INT_EN */
#define KT_RFAMP_INT_EN             (1u << 6)

#define KT_REG_DSP_CFG              0x010Eu  /* bit0 AFC_CTRL_FSM_EN, bit3 DIVERSITY_SW_FSM */
#define KT_AFC_CTRL_FSM_EN          (1u << 0)
#define KT_DIVERSITY_SW_FSM         (1u << 3)

#define KT_REG_AFC_RNG              0x0217u  /* bit[7:6] AFC_RNG */
#define KT_AFC_RNG_SHIFT            6
#define KT_AFC_RNG_60K              0x02u    /* 00:±20 01:±40 10:±60 11:±90 kHz */

#define KT_REG_AFC_EN               0x0218u  /* bit1 AFC_EN = 1, AFC_FROZEN = 0 */
#define KT_AFC_EN_VAL               0x02u

#define KT_REG_COMP_TC              0x0256u  /* bit[6:4] COMP_TC */
#define KT_COMP_TC_SHIFT            4

#define KT_REG_AUDIO_SEL            0x021Cu  /* bit6 DIVERSITY_EN, bit[5:4] AUDIO_SEL */
#define KT_DIVERSITY_EN             (1u << 6)

/* ============================ 自动静音 / 信号质量 ============================ */
#define KT_REG_AUTOMUTE_CTRL        0x0081u  /* bit6 FAST_RSSI_MUTE, bit5 FAST_RSSI_PD_MUTE,
                                              * bit4 SUPER_RSSI_MUTE */
#define KT_FAST_RSSI_MUTE_EN        (1u << 6)
#define KT_FAST_RSSI_PD_MUTE_EN     (1u << 5)
#define KT_SUPER_RSSI_MUTE_EN       (1u << 4)
#define KT_REG_FAST_RSSI_TH         0x0082u
#define KT_REG_SNR_TH_LOW           0x0079u
#define KT_REG_SNR_TH_HIGH          0x007Au

#define KT_REG_RX_CTRL1             0x0100u  /* bit5 AUTOMUTE_SNR_EN, bit7 PILOT_DET_EN */
#define KT_AUTOMUTE_SNR_EN          (1u << 5)
#define KT_PILOT_DET_EN             (1u << 7)

#define KT_REG_SOFT_MUTE            0x0087u  /* bit1 SNR_MUTE, bit2 PILOT_MUTE, bit3 SOFT_AFC_MUTE */
#define KT_SOFT_SNR_MUTE            (1u << 1)
#define KT_SOFT_PILOT_MUTE          (1u << 2)
#define KT_SOFT_AFC_MUTE            (1u << 3)

#define KT_REG_AUTOMUTE_STA         0x0088u  /* bit0 MUTE_FLAG */
#define KT_AUTOMUTE_FLAG            (1u << 0)

#define KT_REG_SIG_STA              0x0209u  /* bit[3:0] AF, bit6 PKG_SYNC, bit7 PILOT */
#define KT_AF_MSK                   0x0Fu
#define KT_PKG_SYNC                 (1u << 6)
#define KT_PILOT_OK                 (1u << 7)

#define KT_REG_PKG_SYNC_INT         0x0057u  /* bit4 PKG_SYNC_INT，写 1 清除 */
#define KT_PKG_SYNC_INT             (1u << 4)
#define KT_REG_RF_INT_STA           0x005Bu  /* bit[6:0] 非 0 表示有中断待处理 */

#define KT_REG_RSSI_M               0x020Cu  /* 主路 RSSI */
#define KT_REG_FAST_RSSI_M          0x020Au  /* 主路快速 RSSI */
#define KT_REG_SNR_M                0x020Du  /* 主路 SNR */
#define KT_REG_RSSI_S               0x0221u  /* 从路 RSSI */
#define KT_REG_FAST_RSSI_S          0x0222u  /* 从路快速 RSSI */
#define KT_REG_SNR_S                0x0223u  /* 从路 SNR */

#define KT_REG_VOLUME               0x0201u  /* bit[4:0] 音量, bit7 NBW */
#define KT_VOLUME_MSK               0x1Fu

#define KT_REG_ADJ_GAIN             0x0200u  /* bit[6:4] ADJUST_GAIN */
#define KT_ADJ_GAIN_SHIFT           4
#define KT_REG_DC_NOTCH             0x0225u  /* bit[3:1] DC_NOTCH_MUTE */
#define KT_REG_AUDIO_GAIN           0x0224u  /* bit[6:5] AUDIO_GAIN（当前未启用） */

#define KT_REG_BPSK_CFG             0x0241u  /* bit7 AUXDATA_EN, bit6 BPSK_NEW_MODE */
#define KT_AUXDATA_EN               (1u << 7)
#define KT_BPSK_NEW_MODE            (1u << 6)
#define KT_REG_BPSK_CARRY           0x0243u  /* bit[6:4] AUX_CARRY_NUM, bit[2:0] CARRY_NUM_TH */
#define KT_AUX_CARRY_SHIFT          4
#define KT_REG_BPSK_PKT             0x0245u  /* 0x22: 连续正确/错误各 2 包 */

/* ============================ 音效：混响 / 激励 / EQ ============================ */
#define KT_REG_ECHO_CTRL            0x0266u  /* bit7 EN, bit6 STRU, bit[5:4] GAIN_DOWN,
                                              * bit[2:0] GAIN_UP */
#define KT_ECHO_EN                  (1u << 7)
#define KT_ECHO_STRU_SHIFT          6
#define KT_ECHO_GAIN_DOWN_SHIFT     4
#define KT_ECHO_GAIN_UP_SHIFT       0
#define KT_REG_ECHO_RATIO           0x0267u  /* bit[4:0] */
#define KT_REG_ECHO_DELAY           0x0268u  /* bit[4:0]，0~23 → 22~197ms */
#define KT_ECHO_PARAM_MSK           0x1Fu

#define KT_REG_EXCITER_CTRL         0x0269u  /* bit7 EN, bit[6:5] TUNE, bit[2:0] DRIVE */
#define KT_EXCITER_EN               (1u << 7)
#define KT_EXCITER_TUNE_SHIFT       5
#define KT_EXCITER_DRIVE_SHIFT      0
#define KT_REG_EXCITER_ODD_EVEN     0x026Au  /* bit[6:4] ODD, bit[2:0] EVEN */
#define KT_EXCITER_ODD_SHIFT        4
#define KT_EXCITER_EVEN_SHIFT       0
#define KT_EXCITER_PARAM_MSK        0x07u

#define KT_REG_EQ_BASE              0x0257u  /* 0x0257~0x0265 共 15 段，每段 bit[4:0] = 增益 */
#define KT_REG_EQ_CTRL              0x0257u  /* 同一地址 bit5 = EQ_EN */
#define KT_EQ_EN                    (1u << 5)
#define KT_EQ_GAIN_MSK              0x1Fu
#define KT_EQ_BAND_NUM              15u

/* ============================ 电池 ============================ */
#define KT_REG_BATTERY_H            0x00C0u  /* 有符号 16 位，1.2V → 0x07FF */
#define KT_REG_BATTERY_L            0x00C1u
#define KT_BATTERY_FULL_SCALE       0x07FFu

/* ============================ 数字音频接口 (I2S) ============================ */
#define KT_REG_I2S_UNLOCK           0x004Du  /* bit7 UNLOCK_TW_CFG_RDY */
#define KT_I2S_UNLOCK_RDY           (1u << 7)
#define KT_REG_I2S_FMT              0x0050u  /* bit5 STEREO_MONO_SEL, bit4 LR_SEL,
                                              * bit6 I2S_SLAVE_SYNC_EN, bit[3:0] 格式/位宽 */
#define KT_I2S_MONO                 (1u << 5)
#define KT_I2S_LR_SEL               (1u << 4)
#define KT_I2S_SLAVE_SYNC_EN        (1u << 6)
#define KT_REG_I2S_CLK              0x0051u  /* bit[3:0] 对齐方式 / mclk 比 */
#define KT_REG_I2S_ROLE             0x0052u  /* bit0 I2S_SLAVE_EN, bit1 I2S_MASTER_EN,
                                              * bit3 I2S_PAD_SDOUT_OEN */
#define KT_I2S_SLAVE_EN             (1u << 0)
#define KT_I2S_MASTER_EN            (1u << 1)
#define KT_I2S_SDOUT_OEN            (1u << 3)

#endif /* KT0656M_REG_H_ */
