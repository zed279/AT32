/**
 * @file    kt0656m.c
 * @brief   KT0656M 控制层 —— 上电初始化、调谐、音效、状态读取
 *
 * @note    本文件是原厂 KT_WirelessMicRxdrv.c 的规整版。算法逻辑（尤其是
 *          Pll_Band_Cali 的 VCO band 温漂校准、PLL_Reset、tune 的复位时序）
 *          与原厂逐条对应，未做任何"优化"，因为那些时序是原厂实测调过的。
 *
 *          与原厂的差异只有四类，全部是为了可复用和可靠性：
 *            1. chipSel 全局游标 → 显式 dev 参数
 *            2. I2C_Byte_Read/Write → kt_rd/kt_wr（走 BSP/IIC_Driver）
 *            3. while 死等 → kt_poll 带超时
 *            4. 寄存器魔数 → kt0656m_reg.h 具名宏
 *
 *          删掉的原厂内容：
 *            - KT0655M 分支（0x0331~0x033A），那是另一颗芯片
 *            - XTAL_DUAL 的双晶振自动选择（caclXtal/oldCaclXtal/use24M[] 表，
 *              共约 260 行），本板单晶振。需要时在 cfg 里改 KT_XTAL_DUAL
 *            - SQUEAL_EN 分支，当前产品不用啸叫抑制
 *            - 5 份注释掉的 SAIInit 变体，改成 cfg 里的 KT_I2S_MODE_SELECT
 */
#include "kt0656m_priv.h"

#define KT_ABS32(v)     (((v) < 0) ? -(v) : (v))

/* 芯片角色：MAIN = KT0656M（解调 + 音频输出 + 内部分集判决）
 *           SUB  = KT0650（只解调，把结果经 LINE_IO 交给主芯片）
 * 注意这里的"主/从"是芯片职责，与 I2C 主机/从机无关——总线上的两颗都是从机。 */
#define KT_IS_MAIN(dev)     ((dev)->role == KT_ROLE_MAIN)

/* 主路专属 API 的角色守卫。
 * 从路没有音频链路（无音量/EQ/Echo/Exciter/SAI/电池检测），调用这些接口
 * 对它是"不适用"而不是"失败"，所以静默返回 KT_OK 而不是错误码 —— 业务层
 * 遍历两颗芯片时不必为角色写两套分支，也不会被一个必然失败的错误码打断。
 * 真正的错误（参数越界、总线故障）仍照常返回。 */
#define KT_GUARD_MAIN(dev)  do { if (!KT_IS_MAIN(dev)) { return KT_OK; } } while (0)

/* ============================ 内部：调谐子过程 ============================ */

/**
 * @brief VCO band 校准（解决温漂）
 * @param cl_low   起始 band
 * @param cl_high  最大 band（原厂传 255）
 * @note  逐条对应原厂 Pll_Band_Cali()。原厂里两处 while 死等，这里都给了上界：
 *        VCOCNT_RDY 用 KT_TO_VCOCNT_MS，band 扫描用 cl_high 迭代上限。
 */
static int pll_band_cali(kt0656m_dev_t *dev, uint8_t cl_low, uint8_t cl_high)
{
    int32_t vco_cnt_res;
    int32_t vco_freq_ref;
    int32_t delta_now;
    int32_t delta_prev;
    uint8_t regx;
    uint8_t band;
    int     rc;

    /* 校准前置条件，不满足直接跳过（原厂行为） */
    rc = kt_rd(dev, KT_REG_BAND_CALI_EN, &regx);
    if (rc != KT_OK) {
        return rc;
    }
    if ((regx & KT_BAND_CALI_GATE) == 0) {
        return KT_OK;
    }

    /* O_VCOCNT_WIN(5)，保留 bit[3:0] 原值 */
    rc = kt_upd(dev, KT_REG_PLL_STA, 0x38u, KT_VCOCNT_WIN_5);
    if (rc != KT_OK) return rc;

    /* S_LO_VCO_BAND_CALI_EN */
    rc = kt_upd(dev, KT_REG_VCO_CALI_EN, KT_LO_VCO_BAND_CALI_EN, KT_LO_VCO_BAND_CALI_EN);
    if (rc != KT_OK) return rc;

    /* O_LO_MMD_MC(32) */
    rc = kt_upd(dev, KT_REG_LO_MMD, 0x3Fu, KT_LO_MMD_MC_32);
    if (rc != KT_OK) return rc;

    /* S_LOMC_SEL */
    rc = kt_upd(dev, KT_REG_PLL_STA, KT_LOMC_SEL, KT_LOMC_SEL);
    if (rc != KT_OK) return rc;

    /* S_LO_DIV128_EN */
    rc = kt_upd(dev, KT_REG_LO_DIV128, KT_LO_DIV128_EN, KT_LO_DIV128_EN);
    if (rc != KT_OK) return rc;

    /* 起始 band：原厂规则是读到的值 <5 就归零，否则回退 5 档 */
    rc = kt_rd(dev, KT_REG_VCO_BAND_SEL, &band);
    if (rc != KT_OK) return rc;
    if (band < 5u) {
        rc = kt_wr(dev, KT_REG_VCO_BAND_SEL, cl_low);
    } else {
        rc = kt_wr(dev, KT_REG_VCO_BAND_SEL, (uint8_t)(band - 5u));
    }
    if (rc != KT_OK) return rc;

    /* ---------- 第一次采样，作为比较基准 ---------- */
    rc = kt_upd(dev, KT_REG_SDM_RST, KT_VCOCNT_RST, 0);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_PLL_STA, KT_VCOCNT_START, KT_VCOCNT_START);
    if (rc != KT_OK) return rc;
    rc = kt_poll(dev, KT_REG_VCOCNT_STA, KT_VCOCNT_RDY, KT_VCOCNT_RDY, KT_TO_VCOCNT_MS);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_VCO_CALI;
    if (rc != KT_OK) return rc;

    rc = kt_rd(dev, KT_REG_VCOCNT_STA, &regx);
    if (rc != KT_OK) return rc;
    vco_cnt_res = (int32_t)(regx & 0x0Fu) << 8;
    rc = kt_rd(dev, KT_REG_VCOCNT_RES_M, &regx);
    if (rc != KT_OK) return rc;
    vco_cnt_res = (vco_cnt_res | regx) << 8;
    rc = kt_rd(dev, KT_REG_VCOCNT_RES_L, &regx);
    if (rc != KT_OK) return rc;
    vco_cnt_res = (vco_cnt_res | regx) >> 2;

    rc = kt_rd(dev, KT_REG_VCOFREQ_REF_H, &regx);
    if (rc != KT_OK) return rc;
    vco_freq_ref = (int32_t)(regx & 0x03u) << 8;
    rc = kt_rd(dev, KT_REG_VCOFREQ_REF_M, &regx);
    if (rc != KT_OK) return rc;
    vco_freq_ref = (vco_freq_ref | regx) << 8;
    rc = kt_rd(dev, KT_REG_VCOFREQ_REF_L, &regx);
    if (rc != KT_OK) return rc;
    vco_freq_ref |= regx;

    delta_prev = vco_cnt_res - vco_freq_ref;

    rc = kt_upd(dev, KT_REG_SDM_RST, KT_VCOCNT_RST, KT_VCOCNT_RST);
    if (rc != KT_OK) return rc;

    /* ---------- 逐档递增，找 |delta| 开始变大的拐点 ---------- */
    for (;;) {
        rc = kt_rd(dev, KT_REG_VCO_BAND_SEL, &band);
        if (rc != KT_OK) return rc;
        band++;
        rc = kt_wr(dev, KT_REG_VCO_BAND_SEL, band);
        if (rc != KT_OK) return rc;

        rc = kt_upd(dev, KT_REG_SDM_RST, KT_VCOCNT_RST, 0);
        if (rc != KT_OK) return rc;
        rc = kt_upd(dev, KT_REG_PLL_STA, KT_VCOCNT_START, KT_VCOCNT_START);
        if (rc != KT_OK) return rc;
        rc = kt_poll(dev, KT_REG_VCOCNT_STA, KT_VCOCNT_RDY, KT_VCOCNT_RDY, KT_TO_VCOCNT_MS);
        if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_VCO_CALI;
        if (rc != KT_OK) return rc;

        /* 注意：Tmp[0] 取的是"上一轮 delta 的绝对值"，与原厂一致 */
        {
            int32_t delta_abs_prev = KT_ABS32(delta_prev);

            rc = kt_rd(dev, KT_REG_VCOCNT_STA, &regx);
            if (rc != KT_OK) return rc;
            vco_cnt_res = (int32_t)(regx & 0x0Fu) << 8;
            rc = kt_rd(dev, KT_REG_VCOCNT_RES_M, &regx);
            if (rc != KT_OK) return rc;
            vco_cnt_res = (vco_cnt_res | regx) << 8;
            rc = kt_rd(dev, KT_REG_VCOCNT_RES_L, &regx);
            if (rc != KT_OK) return rc;
            vco_cnt_res = (vco_cnt_res | regx) >> 2;

            rc = kt_rd(dev, KT_REG_VCOFREQ_REF_H, &regx);
            if (rc != KT_OK) return rc;
            vco_freq_ref = (int32_t)(regx & 0x03u) << 8;
            rc = kt_rd(dev, KT_REG_VCOFREQ_REF_M, &regx);
            if (rc != KT_OK) return rc;
            vco_freq_ref = (vco_freq_ref | regx) << 8;
            rc = kt_rd(dev, KT_REG_VCOFREQ_REF_L, &regx);
            if (rc != KT_OK) return rc;
            vco_freq_ref |= regx;

            delta_now = vco_cnt_res - vco_freq_ref;

            rc = kt_upd(dev, KT_REG_SDM_RST, KT_VCOCNT_RST, KT_VCOCNT_RST);
            if (rc != KT_OK) return rc;

            if ((KT_ABS32(delta_now) >= delta_abs_prev) && (delta_now <= 0)) {
                /* 拐点已过，退回上一档 */
                rc = kt_rd(dev, KT_REG_VCO_BAND_SEL, &band);
                if (rc != KT_OK) return rc;
                band--;
                rc = kt_wr(dev, KT_REG_VCO_BAND_SEL, band);
                if (rc != KT_OK) return rc;
                break;
            }

            delta_prev = delta_now;
        }

        rc = kt_rd(dev, KT_REG_VCO_BAND_SEL, &band);
        if (rc != KT_OK) return rc;
        if (band == cl_high) {
            break;      /* 到顶强制退出，原厂同行为 */
        }
    }

    /* 收尾：关闭校准相关的三个开关 */
    rc = kt_upd(dev, KT_REG_VCO_CALI_EN, KT_LO_VCO_BAND_CALI_EN, 0);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_LO_DIV128, KT_LO_DIV128_EN, 0);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_PLL_STA, KT_LOMC_SEL, 0);

    return rc;
}

/**
 * @brief PLL 复位 / KVCO 校准，原厂为解决温漂加入
 */
static int pll_reset(kt0656m_dev_t *dev)
{
    uint8_t  regx;
    uint8_t  kvco_cali_was_on;
    uint8_t  vref_sel;
    uint8_t  vref_rst;
    int      rc;

    rc = kt_rd(dev, KT_REG_PLL_RESET_EN, &regx);
    if (rc != KT_OK) return rc;
    if ((regx & KT_PLL_RESET_EN) == 0) {
        return KT_OK;
    }

    rc = kt_rd(dev, KT_REG_LO_DIV128, &regx);
    if (rc != KT_OK) return rc;
    kvco_cali_was_on = (uint8_t)((regx >> 6) & 0x01u);
    vref_sel         = (uint8_t)(regx & KT_KVCO_VREF_SEL_MSK);

    rc = kt_rd(dev, KT_REG_PLL_RESET_EN, &regx);
    if (rc != KT_OK) return rc;
    vref_rst = (uint8_t)(regx & 0x07u);

    /* 把复位值写进 O_LO_KVCO_COARSE_VREF_SEL */
    rc = kt_upd(dev, KT_REG_KVCO_VREF, 0x07u, vref_rst);
    if (rc != KT_OK) return rc;

    /* S_LO_KVCO_CALI_EN */
    rc = kt_upd(dev, KT_REG_LO_DIV128, KT_LO_KVCO_CALI_EN, KT_LO_KVCO_CALI_EN);
    if (rc != KT_OK) return rc;

    KT_DELAY_MS(10);

    if (kvco_cali_was_on) {
        rc = kt_upd(dev, KT_REG_LO_DIV128, KT_LO_KVCO_CALI_EN, KT_LO_KVCO_CALI_EN);
    } else {
        rc = kt_upd(dev, KT_REG_LO_DIV128, KT_LO_KVCO_CALI_EN, 0);
    }
    if (rc != KT_OK) return rc;

    /* 恢复原始 Vref */
    rc = kt_upd(dev, KT_REG_KVCO_VREF, 0x07u, vref_sel);

    return rc;
}

/**
 * @brief 写频道字并等 PLL 锁定
 * @param scan_mode 1 = 扫频模式（只写不等待）
 */
static int write_channel(kt0656m_dev_t *dev, uint32_t freq_khz, uint8_t scan_mode)
{
    uint32_t freq = freq_khz & 0x000FFFFFu;
    uint8_t  freq_h = (uint8_t)(freq >> 12);
    uint8_t  freq_m = (uint8_t)((freq & 0x00000FFFu) >> 4);
    uint8_t  freq_l = (uint8_t)((freq & 0x0000000Fu) << 4);
    int      rc;

    /* SCAN_MODE 在 0x0108 bit3：只有 KT0656M 定义，KT0650 那里是保留位
     * （默认 6'b10_0000 的 bit3 恰为 0，但我们不靠"恰好"）。而且搜索模式
     * 对从路本身没有意义 —— 从路永远跟随主路的频点，不单独搜台。 */
    if (KT_IS_MAIN(dev)) {
        rc = kt_upd(dev, KT_REG_XTAL_CFG, KT_SCAN_MODE,
                    scan_mode ? KT_SCAN_MODE : 0);
        if (rc != KT_OK) return rc;
    }

    rc = kt_wr(dev, KT_REG_CHAN_H, freq_h);
    if (rc != KT_OK) return rc;
    rc = kt_wr(dev, KT_REG_CHAN_M, freq_m);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_CHAN_L, 0xF0u, freq_l);
    if (rc != KT_OK) return rc;

    /* 切频期间关掉射频幅度中断，避免通道字写入过程误触发。
     * 0x0053 在 KT0650 手册未定义，从路跳过。 */
    if (KT_IS_MAIN(dev)) {
        rc = kt_upd(dev, KT_REG_RFAMP_INT, KT_RFAMP_INT_EN, 0);
        if (rc != KT_OK) return rc;
    }

    /* CHAN_VALID 拉高触发调谐 */
    rc = kt_upd(dev, KT_REG_CHAN_L, KT_CHAN_VALID, KT_CHAN_VALID);
    return rc;
}

/* ============================ 探测 ============================ */

int kt0656m_probe(kt0656m_dev_t *dev, kt0656m_info_t *info)
{
    int rc;

    if (dev == 0 || info == 0) {
        return KT_ERR_PARAM;
    }

    info->scan_hit   = 0;
    info->chip_id_h  = 0xFF;
    info->chip_id_l  = 0xFF;
    info->fw_version = 0xFF;
    info->power_sta  = 0xFF;
    info->id_ok      = 0;
    info->boot_ok    = 0;
    info->stuck      = 0;

    if (!dev->ready) {
        return KT_ERR_NOT_READY;
    }

    /* 地址探测：直接发一次寻址，比全总线扫描快得多 */
    rc = iic_master_write(dev->bus, dev->addr, 0, 0, KT_IIC_TIMEOUT_MS);
    info->scan_hit = (rc == IIC_OK) ? 1 : 0;

    rc = kt_rd(dev, KT_REG_CHIP_ID_H, &info->chip_id_h);
    if (rc != KT_OK) return rc;
    rc = kt_rd(dev, KT_REG_CHIP_ID_L, &info->chip_id_l);
    if (rc != KT_OK) return rc;
    info->id_ok = (info->chip_id_h == KT_CHIP_ID_H_VAL &&
                   info->chip_id_l == KT_CHIP_ID_L_VAL) ? 1 : 0;

    rc = kt_rd(dev, KT_REG_FW_VERSION, &info->fw_version);
    if (rc != KT_OK) return rc;

    rc = kt_rd(dev, KT_REG_POWER_STA, &info->power_sta);
    if (rc != KT_OK) return rc;
    info->boot_ok = (info->power_sta & KT_PWR_ON_FINISH) ? 1 : 0;

    /*
     * 四个寄存器（CHIP_ID_H/L、固件版本、电源状态）内容完全不同，不可能相等。
     * 全等说明 I2C 从机在 ACK，但返回的是同一个常量垃圾字节 —— 芯片内部时钟
     * 没起来（典型于 CHIP_EN 没真正掉电、或振荡器被配到没焊晶体的脚）。
     */
    if (info->chip_id_h == info->chip_id_l &&
        info->chip_id_l == info->fw_version &&
        info->fw_version == info->power_sta) {
        info->stuck = 1;
    }

    return KT_OK;
}

/* ============================ 初始化 ============================ */

/*
 * ============================ 初始化：按角色分段 ============================
 *
 * init 拆成 11 段，每段内部自己决定"当前这颗芯片该不该写"。
 * 为什么不写成"主路一套流程 + 从路一套流程"：
 *   1. 主路的寄存器写入顺序必须与改动前逐行一致（DCDC 要先于等上电、音量
 *      静音要先于开输出……这些次序是上板调出来的）。拆段之后主 init 只是
 *      按原顺序调用，顺序差异一眼可审，不必担心"重构时手滑动了次序"。
 *   2. KT0650 的执行集是"原序列的子集"，不是另一套序列。用角色分支表达
 *      子集，将来主路新增寄存器时，改动点必然落在某一段里，"从路要不要跟"
 *      这个问题会自然浮现；而两套并列流程会让人只改主路、忘了从路。
 *
 * KT0650（手册 V0.1.2 第 4.10 节）只定义约 30 个寄存器，与 KT0656M 不重叠
 * 或语义不同的地方必须显式过滤。三个最容易出事的点：
 *
 *   0x0108 —— KT0650 只有 bit7 XTAL1_SEL / bit6 XTAL2_SEL，bit5:0 保留
 *             （默认 6'b10_0000）。所以
 *               a) 写 XTAL_CFG 只能用 0xC0 mask，不能把 HLSI_INV(bit5) 写进去；
 *               b) 绝不能拿 bit4 当 LOBAND_CALI_SCAN_EN —— KT0650 没有这个
 *                  硬件，置 1 之后 kt_poll 等它清 0 会一直等到 300ms 超时，
 *                  表现为"从机 init 必报低段校准失败"。
 *   0x0225 —— KT0656M = DC_NOTCH，KT0650 = AUTOMUTE_CFG0（bit0 才是功能位，
 *             bit7:1 保留）。同址不同义，从路必须整段跳过。
 *   0x021C —— KT0650 的 bit5:0 保留（默认 6'b00_1001）。主路那步
 *             "kt_upd(0x30, 0)"清的是 AUDIO_SEL，从路不能套用同一个 mask，
 *             从路只允许单独动 bit6 DIVERSITY_EN。
 *
 * 反过来也有"同址同义、可以共用"的，别多写分支：
 *   0x0100  KT0656M RX_CTRL1 = AUTOMUTE_SNR_EN(bit5)/PILOT_DET_EN(bit7)
 *           KT0650  SW_CFG0   = SNR_MUTE_EN    (bit5)/PILOT_MUTE_EN(bit7)
 *   0x0217  两芯片都是 AFC_RNG 的高两位；0x0218 都是 AFC_EN(bit1)
 *   0x0209  两芯片都是 PILOT_DETECT(bit7)/PKG_SYNC(bit6)/AF(bit3:0)
 */

/**
 * @brief 段 1：识别芯片（MANUFACTURER_ID 高字节，重试 3 次）
 */
static int in_ident(kt0656m_dev_t *dev)
{
    uint8_t regx;
    int     rc;
    uint8_t i;

    for (i = 0; i < 3u; i++) {
        rc = kt_rd(dev, KT_REG_CHIP_ID_H, &regx);
        if (rc == KT_OK && regx == KT_CHIP_ID_H_VAL) {
            return KT_OK;
        }
        if (i == 2u) {
            break;
        }
        KT_DELAY_MS(10);
    }
    return KT_ERR_ID_MISMATCH;
}

/**
 * @brief 段 2：DCDC 上电 + 先静音（仅主路）
 * @note  KT0650 无 DCDC、无音量寄存器，整段跳过。它上电即工作，不需要
 *        "先开 DC-DC 再静音"这个动作。
 */
static int in_power(kt0656m_dev_t *dev)
{
    int rc;

    if (!KT_IS_MAIN(dev)) {
        return KT_OK;
    }

    rc = kt_upd(dev, KT_REG_DCDC_CFG, KT_DCDC_EN, KT_CFG_DCDC_EN ? KT_DCDC_EN : 0);
    if (rc != KT_OK) return rc;

    return kt0656m_volume_set(dev, KT_VOLUME_BOOT);
}

/**
 * @brief 段 3：等内部上电完成（两颗芯片同一位：0x7F bit0）
 */
static int in_wait_ready(kt0656m_dev_t *dev)
{
    return kt_poll(dev, KT_REG_POWER_STA, KT_PWR_ON_FINISH, KT_PWR_ON_FINISH,
                   KT_TO_POWER_ON_MS);
}

/**
 * @brief 段 4：固件版本门禁（版本不符说明寄存器表对不上，不能盲配）
 * @note  KT0650 手册未列出 0x010F，但实测读回同为 0x11（阶段 0 扫描），
 *        说明该地址真实存在且同义，因此两路共用这道门禁。
 */
static int in_fw_check(kt0656m_dev_t *dev)
{
    uint8_t regx;
    int     rc;

    /* KT0650 datasheet V0.1.2 的寄存器表里没有 0x010F（FIRMWARE_VERSION），
     * 拿 KT0656M 的版本号去校验从路没有依据。读回来若是未定义值，
     * 会让整段初始化在这里中断，现象是"从机 init 必报 FW_MISMATCH"。
     * 从路的固件版本本驱动也不依赖，直接跳过。 */
    if (!KT_IS_MAIN(dev)) {
        return KT_OK;
    }

    rc = kt_rd(dev, KT_REG_FW_VERSION, &regx);
    if (rc != KT_OK) return rc;
    if (regx != KT_FW_VERSION_VAL) {
        return KT_ERR_FW_MISMATCH;
    }
    return KT_OK;
}

/**
 * @brief 段 5：晶振配置（两颗芯片都必须做，但 mask 不同）
 */
static int in_xtal(kt0656m_dev_t *dev)
{
    uint8_t mask;
    uint8_t val;
    int     rc;

    /* 5.1 先选片外晶体接在哪组脚（0x0012 bit7），再告诉芯片它的频率 */
    rc = kt_upd(dev, KT_REG_ANA_CFG, KT_XTAL_SEL_BIT,
                KT_XTAL_ON_XO2 ? KT_XTAL_SEL_BIT : 0);
    if (rc != KT_OK) return rc;

    /* 5.2 XTAL1_SEL / XTAL2_SEL 两芯片同位同义，可共用；
     *     HLSI_INV(bit5) 只有 KT0656M 有，KT0650 那里是保留位且默认 1，
     *     套用主路的 0xE0 mask 会把它误清成 0。所以 mask 按角色取。 */
    val = (uint8_t)((KT_XTAL_SEL1 << KT_XTAL_SEL1_SHIFT) |
                    (KT_XTAL_SEL2 << KT_XTAL_SEL2_SHIFT));

    if (KT_IS_MAIN(dev)) {
        mask = 0xE0u;
        val |= (uint8_t)(KT_HLSI_INV << KT_HLSI_INV_SHIFT);
    } else {
        mask = 0xC0u;
    }

    return kt_upd(dev, KT_REG_XTAL_CFG, mask, val);
}

/**
 * @brief 段 6：模拟前端偏置（仅主路）
 * @note  BIAS_CFG(0x001F) / LDO_CURRENT(0x000D) / PLL_NOISE(0x0109) 在
 *        KT0650 手册里全部未定义，从路跳过并沿用上电默认值。
 */
static int in_frontend(kt0656m_dev_t *dev)
{
    int rc;

    if (!KT_IS_MAIN(dev)) {
        return KT_OK;
    }

    rc = kt_upd(dev, KT_REG_BIAS_CFG, 0xE0u, KT_BIAS_0P4V);     /* 0.4V */
    if (rc != KT_OK) return rc;

    rc = kt_wr(dev, KT_REG_LDO_CURRENT, KT_LDO_CURRENT_6MA);     /* 6mA */
    if (rc != KT_OK) return rc;

    return kt_wr(dev, KT_REG_PLL_NOISE, KT_PLL_NOISE_3DB);       /* PLL noise +3dB */
}

/**
 * @brief 段 7：自动静音
 * @note  阈值类寄存器（0x82 FAST_RSSI_TH、0x79/0x7A SNR_TH、0x81 AUTOMUTE_CTRL）
 *        在 KT0650 手册未定义，从路只能用上电默认阈值 —— 这也符合分工：
 *        从路的质量由主芯片读取，从路自己静不静音不影响输出。
 *        0x0100 两芯片同位同义，所以"用哪些条件做判断"这一组两路都写。
 */
static int in_automute(kt0656m_dev_t *dev)
{
    int rc;

#if (KT_CFG_AUTOMUTE_EN)
    if (KT_IS_MAIN(dev)) {
        rc = kt_wr(dev, KT_REG_FAST_RSSI_TH, KT_FAST_RSSI_MUTE_TH);
        if (rc != KT_OK) return rc;

        rc = kt_upd(dev, KT_REG_AUTOMUTE_CTRL,
                    (uint8_t)(KT_FAST_RSSI_MUTE_EN | KT_FAST_RSSI_PD_MUTE_EN | KT_SUPER_RSSI_MUTE_EN),
                    (uint8_t)((KT_CFG_FAST_RSSI_MUTE_EN    ? KT_FAST_RSSI_MUTE_EN    : 0) |
                              (KT_CFG_FAST_RSSI_PD_MUTE_EN ? KT_FAST_RSSI_PD_MUTE_EN : 0) |
                              (KT_CFG_SUPER_RSSI_MUTE_EN   ? KT_SUPER_RSSI_MUTE_EN   : 0)));
        if (rc != KT_OK) return rc;

        rc = kt_wr(dev, KT_REG_SNR_TH_LOW,  KT_AUTOMUTE_SNR_LOW);
        if (rc != KT_OK) return rc;
        rc = kt_wr(dev, KT_REG_SNR_TH_HIGH, KT_AUTOMUTE_SNR_HIGH);
        if (rc != KT_OK) return rc;

        /* 窄带 NBW=1（在主路的 VOLUME 里） */
        rc = kt_upd(dev, KT_REG_VOLUME, 0x80u, 0x80u);
        if (rc != KT_OK) return rc;
    }

    /* SNR 自动静音使能 */
    rc = kt_upd(dev, KT_REG_RX_CTRL1, KT_AUTOMUTE_SNR_EN, KT_AUTOMUTE_SNR_EN);
    if (rc != KT_OK) return rc;

    /* 导频检测按产品需要开关 */
    rc = kt_upd(dev, KT_REG_RX_CTRL1, KT_PILOT_DET_EN,
                KT_CFG_PILOT_DETECT ? KT_PILOT_DET_EN : 0);
    return rc;
#else
    (void)dev;
    return KT_OK;
#endif
}

/**
 * @brief 段 8：BPSK 辅助数据
 * @note  0x0241 两芯片同义（bit7 AUXDATA_EN / bit6 BPSK_NEW_MODE）；
 *        0x0243 BPSK_CARRY 与 0x0245 BPSK_PKT 在 KT0650 手册未定义，
 *        从路只能吃默认值。数字辅助信道的"发送侧节奏"本来就该由主路决定。
 */
static int in_bpsk(kt0656m_dev_t *dev)
{
    int rc;

    rc = kt_upd(dev, KT_REG_BPSK_CFG, (uint8_t)(KT_AUXDATA_EN | KT_BPSK_NEW_MODE),
                (uint8_t)((KT_CFG_AUXDATA_EN    ? KT_AUXDATA_EN    : 0) |
                          (KT_BPSK_NEW_MODE_EN  ? KT_BPSK_NEW_MODE : 0)));
    if (rc != KT_OK) return rc;

    if (!KT_IS_MAIN(dev)) {
        return KT_OK;
    }

    rc = kt_upd(dev, KT_REG_BPSK_CARRY, 0x77u,
                (uint8_t)((KT_AUX_CARRY_NUM << KT_AUX_CARRY_SHIFT) | KT_CARRY_NUM_TH));
    if (rc != KT_OK) return rc;

    return kt_wr(dev, KT_REG_BPSK_PKT, 0x22u);   /* 连续正确/错误各 2 包 */
}

/**
 * @brief 段 9：解调与 AFC
 * @note  AFC(0x217/0x218) 两颗芯片都要：从路也工作在 UHF，收的也是同一个
 *        发射机，晶体偏差一样存在。其余（增益、DC notch、DSP、软静音、
 *        温补、VTR 监测）都只有主路有。
 *        顺序保持原样：AFC 两个寄存器夹在主路专属块中间，所以这里用两段
 *        if 把它夹住，而不是把 AFC 提到前面 —— 主路的写入次序不重排。
 */
static int in_demod(kt0656m_dev_t *dev)
{
    int rc;

    if (KT_IS_MAIN(dev)) {
        rc = kt_upd(dev, KT_REG_ADJ_GAIN, 0x70u,
                    (uint8_t)(KT_ADJ_GAIN_SEL << KT_ADJ_GAIN_SHIFT));
        if (rc != KT_OK) return rc;

        /* 0x0225 在 KT0650 上是 AUTOMUTE_CFG0，执行这句会改坏它的保留位 */
        rc = kt_upd(dev, KT_REG_DC_NOTCH, 0x0Eu, 0);            /* DC_NOTCH_MUTE_EN = 0 */
        if (rc != KT_OK) return rc;

        rc = kt_upd(dev, KT_REG_DSP_CFG, KT_AFC_CTRL_FSM_EN, 0); /* AFC CTRL FSM 关闭 */
        if (rc != KT_OK) return rc;

        rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_AFC_MUTE, 0);
        if (rc != KT_OK) return rc;
    }

    rc = kt_upd(dev, KT_REG_AFC_RNG, 0xC1u,
                (uint8_t)((KT_AFC_RNG_SEL << KT_AFC_RNG_SHIFT) | 0x01u));
    if (rc != KT_OK) return rc;

    rc = kt_wr(dev, KT_REG_AFC_EN, KT_AFC_EN_VAL);           /* AFC 使能，不冻结 */
    if (rc != KT_OK) return rc;

    if (KT_IS_MAIN(dev)) {
        rc = kt_upd(dev, KT_REG_COMP_TC, 0x70u,
                    (uint8_t)(KT_COMP_TC_VAL << KT_COMP_TC_SHIFT));
        if (rc != KT_OK) return rc;

        rc = kt_upd(dev, KT_REG_DCDC_CFG, KT_VTR_MONITOR_EN, KT_VTR_MONITOR_EN);
        if (rc != KT_OK) return rc;

        rc = kt_upd(dev, KT_REG_VTR_CFG, KT_REF_VTR_VTH_SEL, KT_REF_VTR_VTH_SEL);
        if (rc != KT_OK) return rc;
    }

    return KT_OK;
}

/**
 * @brief 段 10：LO / 分集 / 输出路由（仅主路）
 * @note  LO_LOCK(0x0030)、PLL_RESET_EN(0x0133)、DSP_CFG(0x010E) 在 KT0650
 *        上不存在；0x021C 从路虽有，但只有 bit6 DIVERSITY_EN 是非保留位，
 *        主路那句 mask=0x30 的 AUDIO_SEL 写入对从路是"改保留位"，不能执行。
 *        从路的 0x021C 由阶段 3 用 kt0656m_diversity_enable() 单独置 bit6。
 */
static int in_lo_div(kt0656m_dev_t *dev)
{
    int rc;

    if (!KT_IS_MAIN(dev)) {
        return KT_OK;
    }

    rc = kt_upd(dev, KT_REG_LO_LOCK, KT_LO_LOCK_DET_PD, 0);
    if (rc != KT_OK) return rc;

    rc = kt_upd(dev, KT_REG_PLL_RESET_EN, KT_DLL_RST_EN, KT_DLL_RST_EN);
    if (rc != KT_OK) return rc;

    /* 天线分集用硬件的，软件状态机关掉 */
    rc = kt_upd(dev, KT_REG_DSP_CFG, KT_DIVERSITY_SW_FSM, 0);
    if (rc != KT_OK) return rc;

    /* 输出合并信号：必须在软件分集状态机之后写，否则会被它改写 */
    return kt_upd(dev, KT_REG_AUDIO_SEL, 0x30u, 0);
}

/**
 * @brief 段 11：低段校准扫描（仅主路）
 * @note  这一步是给 fast tune 铺路的。KT0650 的 0x0108 bit4 是保留位，
 *        置 1 后没有硬件会清它 —— 若照抄主路流程，从路 init 会在这里
 *        死等 KT_TO_LOBAND_CALI_MS 并必报 KT_ERR_INIT_LOBAND。
 */
static int in_loband(kt0656m_dev_t *dev)
{
    int rc;

    if (!KT_IS_MAIN(dev)) {
        return KT_OK;
    }

    rc = kt_upd(dev, KT_REG_XTAL_CFG, KT_LOBAND_CALI_SCAN_EN, KT_LOBAND_CALI_SCAN_EN);
    if (rc != KT_OK) return rc;

    rc = kt_poll(dev, KT_REG_XTAL_CFG, KT_LOBAND_CALI_SCAN_EN, 0, KT_TO_LOBAND_CALI_MS);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_INIT_LOBAND;
    return rc;
}

/* ============================ 初始化：对外入口 ============================ */

int kt0656m_init(kt0656m_dev_t *dev)
{
    int rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    if (!dev->ready) {
        rc = kt0656m_bus_open(dev);
        if (rc != KT_OK) {
            return rc;
        }
    }

    /* 调用次序 = 改造前函数体的段落次序，未做任何重排 */
    rc = in_ident(dev);      if (rc != KT_OK) return rc;
    rc = in_power(dev);      if (rc != KT_OK) return rc;
    rc = in_wait_ready(dev); if (rc != KT_OK) return rc;
    rc = in_fw_check(dev);   if (rc != KT_OK) return rc;
    rc = in_xtal(dev);       if (rc != KT_OK) return rc;
    rc = in_frontend(dev);   if (rc != KT_OK) return rc;
    rc = in_automute(dev);   if (rc != KT_OK) return rc;
    rc = in_bpsk(dev);       if (rc != KT_OK) return rc;
    rc = in_demod(dev);      if (rc != KT_OK) return rc;
    rc = in_lo_div(dev);     if (rc != KT_OK) return rc;
    return in_loband(dev);
}

/* ============================ 调谐 ============================ */

/**
 * @brief 从路（KT0650）调谐：写频率 + 等 PLL_READY，没有别的步骤
 * @note  手册 4.5 原文："将以 KHz 为单位的载波频率写入 CHAN<19:0>，再将
 *        CHAN_VALID 置 1 完成频率设置。此后 CHAN_VALID 将被硬件自动清 0。
 *        在 260ms 之内 PLL_READY 将变高。"
 *        也就是说从路的调谐流程到这里就结束了 —— 它没有 DSP 复位、没有
 *        LO 锁定检测、没有 VCO band 温漂校准、没有音频接口初始化。
 *        照抄主路流程只会往不存在的寄存器里写数据，还会卡在等一个永远
 *        不来的标志上。
 */
static int tune_sub(kt0656m_dev_t *dev, uint32_t freq_khz)
{
    int rc;

    rc = write_channel(dev, freq_khz, 0);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_CHAN;
    if (rc != KT_OK) return rc;

    /* CHAN_VALID 写 1 后芯片内部会把 PLL_DONE 拉低再拉高。立即读可能读到
     * 上一轮残留的 1（尤其从路刚上电、默认频点还没稳定时），所以等 1ms。 */
    KT_DELAY_MS(1);
    rc = kt_poll(dev, KT_REG_PLL_STA, KT_PLL_DONE, KT_PLL_DONE, KT_TO_PLL_DONE_MS);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_PLL_LOCK;
    return rc;
}

int kt0656m_tune(kt0656m_dev_t *dev, uint32_t freq_khz)
{
    uint8_t regx;
    uint8_t lo_lock_saved;
    uint8_t state;
    int     rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }
    if (freq_khz < KT_BAND_BOTTOM_CHA || freq_khz > KT_BAND_TOP_CHB) {
        return KT_ERR_PARAM;
    }

    /* 从路（KT0650）走精简路径：它没有音频链路、没有 band 校准可做。
     * 对外接口保持同一个，业务层切台时无脑对两颗都调 tune 即可 ——
     * "哪颗需要做什么"由驱动按角色决定，不该由业务层记。 */
    if (!KT_IS_MAIN(dev)) {
        return tune_sub(dev, freq_khz);
    }

    rc = write_channel(dev, freq_khz, 0);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_CHAN;
    if (rc != KT_OK) return rc;

    /* 原厂注释：chan_valid 后芯片内部会写 PLL_DONE=0，必须等 1ms 再查，
     * 否则可能读到上一轮的 1，内外同时操作 DSP 会死机 */
    KT_DELAY_MS(1);
    rc = kt_poll(dev, KT_REG_PLL_STA, KT_PLL_DONE, KT_PLL_DONE, KT_TO_PLL_DONE_MS);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_PLL_LOCK;
    if (rc != KT_OK) return rc;

    /* ---------- 复位 DSP 与 PLL SDM ---------- */
    rc = kt_upd(dev, KT_REG_SDM_RST, KT_DSP_RST, KT_DSP_RST);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_SDM_RST, KT_PLL_SDM_RST, KT_PLL_SDM_RST);
    if (rc != KT_OK) return rc;

    /* ---------- 锁定检测上电，处理完再恢复 ---------- */
    rc = kt_rd(dev, KT_REG_LO_LOCK, &regx);
    if (rc != KT_OK) return rc;
    lo_lock_saved = (uint8_t)(regx & KT_LO_LOCK_DET_PD);
    rc = kt_upd(dev, KT_REG_LO_LOCK, KT_LO_LOCK_DET_PD, KT_LO_LOCK_DET_PD);
    if (rc != KT_OK) return rc;

    /* ---------- 粗调档位补偿：原厂 double+16MHz/V 的映射 ---------- */
    rc = kt_rd(dev, KT_REG_RF_CFG, &regx);
    if (rc != KT_OK) return rc;
    state = (uint8_t)(regx & 0x07u);
    state = (state >= 3u) ? 7u : (uint8_t)((state << 1) + 3u);
    rc = kt_upd(dev, KT_REG_RF_CFG, 0x07u, state);
    if (rc != KT_OK) return rc;

    /* ---------- 温漂校准 ---------- */
    rc = pll_band_cali(dev, 0, 255);
    if (rc != KT_OK) return rc;
    rc = pll_reset(dev);
    if (rc != KT_OK) return rc;

    /* ---------- 释放复位 ---------- */
    rc = kt_upd(dev, KT_REG_SDM_RST, KT_PLL_SDM_RST, 0);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_SDM_RST, KT_DSP_RST, 0);
    if (rc != KT_OK) return rc;

    KT_DELAY_MS(10);

    rc = kt_upd(dev, KT_REG_LO_LOCK, KT_LO_LOCK_DET_PD, lo_lock_saved);
    if (rc != KT_OK) return rc;

    rc = kt0656m_audio_if_init(dev);
    if (rc != KT_OK) return rc;

    /* 信号够强就先解除 SNR 静音，避免切台后要等一个 patch 周期才出声 */
    {
        uint8_t snr = 0;
        if (kt0656m_snr_get(dev, &snr) == KT_OK && snr > KT_AUTOMUTE_SNR_LOW) {
            rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_SNR_MUTE, 0);
        } else {
            rc = KT_OK;
        }
    }

    return rc;
}

int kt0656m_tune_fast(kt0656m_dev_t *dev, uint32_t freq_khz)
{
    uint8_t snr = 0;
    int     rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }
    if (freq_khz < KT_BAND_BOTTOM_CHA || freq_khz > KT_BAND_TOP_CHB) {
        return KT_ERR_PARAM;
    }

    /* 从路的"快调"就是普通调谐：它本来就没有音频接口要重新初始化，
     * 也没有 SOFT_MUTE 要解。 */
    if (!KT_IS_MAIN(dev)) {
        return tune_sub(dev, freq_khz);
    }

    rc = write_channel(dev, freq_khz, 1);
    if (rc != KT_OK) return rc;

    KT_DELAY_MS(1);
    rc = kt_poll(dev, KT_REG_PLL_STA, KT_PLL_DONE, KT_PLL_DONE, KT_TO_PLL_DONE_MS);
    if (rc == KT_ERR_TIMEOUT) return KT_ERR_TUNE_PLL_LOCK;
    if (rc != KT_OK) return rc;

    rc = kt0656m_audio_if_init(dev);
    if (rc != KT_OK) return rc;

    if (kt0656m_snr_get(dev, &snr) == KT_OK && snr > KT_AUTOMUTE_SNR_LOW) {
        rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_SNR_MUTE, 0);
    }

    return rc;
}

int kt0656m_tune_seek(kt0656m_dev_t *dev, uint32_t freq_khz)
{
    int rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }
    if (freq_khz < KT_BAND_BOTTOM_CHA || freq_khz > KT_BAND_TOP_CHB) {
        return KT_ERR_PARAM;
    }

    rc = write_channel(dev, freq_khz, 1);
    if (rc != KT_OK) return rc;

    KT_DELAY_MS(1);     /* 只触发调谐，不等 PLL，搜台靠后续逐点判 RSSI */
    return KT_OK;
}

/* ============================ 音量与音效 ============================ */

int kt0656m_volume_set(kt0656m_dev_t *dev, uint8_t vol)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    if (vol > KT_VOLUME_MAX) {
        return KT_ERR_PARAM;
    }
    return kt_upd(dev, KT_REG_VOLUME, KT_VOLUME_MSK, vol);
}

int kt0656m_echo_enable(kt0656m_dev_t *dev, uint8_t en)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    /* 原厂 mask 是 (regx & 0x08)，即只有 bit3 来自寄存器，其余全部由参数覆盖。
     * 所以这里 mask 必须是 0xF7 而不是 0xF8 —— 用 0xF8 会把 GAIN_UP(bit[2:0])
     * 当成"保留位"，导致混响增益写不进去。 */
    return kt_upd(dev, KT_REG_ECHO_CTRL, 0xF7u,
                  (uint8_t)((en ? KT_ECHO_EN : 0) |
                            (KT_ECHO_STRU      << KT_ECHO_STRU_SHIFT) |
                            (KT_ECHO_GAIN_DOWN << KT_ECHO_GAIN_DOWN_SHIFT) |
                            (KT_ECHO_GAIN_UP   << KT_ECHO_GAIN_UP_SHIFT)));
}

int kt0656m_echo_set(kt0656m_dev_t *dev, uint8_t ratio, uint8_t delay)
{
    int rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    if (ratio > 25u || delay > 23u) {
        return KT_ERR_PARAM;
    }

    rc = kt_upd(dev, KT_REG_ECHO_RATIO, KT_ECHO_PARAM_MSK, ratio);
    if (rc != KT_OK) return rc;
    return kt_upd(dev, KT_REG_ECHO_DELAY, KT_ECHO_PARAM_MSK, delay);
}

int kt0656m_exciter_enable(kt0656m_dev_t *dev, uint8_t en)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    return kt_upd(dev, KT_REG_EXCITER_CTRL, 0xE7u,
                  (uint8_t)((en ? KT_EXCITER_EN : 0) |
                            (KT_EXCITER_TUNE_DEF  << KT_EXCITER_TUNE_SHIFT) |
                            (KT_EXCITER_DRIVE_DEF << KT_EXCITER_DRIVE_SHIFT)));
}

int kt0656m_exciter_set(kt0656m_dev_t *dev, uint8_t odd, uint8_t even)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    if (odd > 6u || even > 6u) {
        return KT_ERR_PARAM;
    }
    return kt_upd(dev, KT_REG_EXCITER_ODD_EVEN, 0x77u,
                  (uint8_t)((odd << KT_EXCITER_ODD_SHIFT) |
                            (even << KT_EXCITER_EVEN_SHIFT)));
}

int kt0656m_eq_enable(kt0656m_dev_t *dev, uint8_t en)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    return kt_upd(dev, KT_REG_EQ_CTRL, KT_EQ_EN, en ? KT_EQ_EN : 0);
}

int kt0656m_eq_set_band(kt0656m_dev_t *dev, uint8_t band, uint8_t gain)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    KT_GUARD_MAIN(dev);
    if (band >= KT_EQ_BAND_NUM || gain > 24u) {
        return KT_ERR_PARAM;
    }

    if (band == 0u) {
        /* 第 0 段与 EQ_EN 同地址，只能动 bit[4:0] */
        return kt_upd(dev, KT_REG_EQ_BASE, KT_EQ_GAIN_MSK, gain);
    }
    return kt_wr(dev, (uint16_t)(KT_REG_EQ_BASE + band), gain);
}

int kt0656m_diversity_enable(kt0656m_dev_t *dev, uint8_t en)
{
    if (dev == 0) {
        return KT_ERR_PARAM;
    }
    return kt_upd(dev, KT_REG_AUDIO_SEL, KT_DIVERSITY_EN, en ? KT_DIVERSITY_EN : 0);
}

/* ============================ 状态读取 ============================ */

int kt0656m_af_get(kt0656m_dev_t *dev, uint8_t *out)
{
    uint8_t regx;
    int     rc;

    if (out == 0) return KT_ERR_PARAM;
    rc = kt_rd(dev, KT_REG_SIG_STA, &regx);
    if (rc != KT_OK) return rc;
    *out = (uint8_t)(regx & KT_AF_MSK);
    return KT_OK;
}

/*
 * 下面三个 get 读的是【这颗芯片自己的】信号质量：主路读 0x20A/0x20C/0x20D，
 * 从路 KT0650 也读同样的地址 —— 它有自己独立的前端，报告的就是它自己的接收质量。
 *
 * 这里刻意**不再看 dev->div_main_path**。那个字段表示"芯片内部分集判决当前
 * 选中了哪一路天线"，是给人看的结果，和"这颗芯片自己的信号有多强"是两件事。
 * 两者混用会产生一个很隐蔽的故障链：
 *     select_antenna() 判定从路更好 → div_main_path=0
 *     → snr_get() 转去读 0x0223（未公开镜像）
 *     → tune() 里"信号够强就解 SNR 静音"这一步拿到垃圾值，判为信号弱
 *     → 声音一直静音，而日志里 SNR 数字看着还挺正常
 *
 * 要在主芯片侧读从路的信号质量，用 kt0656m_alt_*_get()。
 */
int kt0656m_rssi_get(kt0656m_dev_t *dev, uint8_t *out)
{
    if (out == 0) return KT_ERR_PARAM;
    return kt_rd(dev, KT_REG_RSSI_M, out);
}

int kt0656m_fast_rssi_get(kt0656m_dev_t *dev, uint8_t *out)
{
    if (out == 0) return KT_ERR_PARAM;
    return kt_rd(dev, KT_REG_FAST_RSSI_M, out);
}

int kt0656m_snr_get(kt0656m_dev_t *dev, uint8_t *out)
{
    if (out == 0) return KT_ERR_PARAM;
    return kt_rd(dev, KT_REG_SNR_M, out);
}

/**
 * @brief 读"另一路天线"的信号质量（仅主芯片有效）
 * @note  0x0221/0x0222/0x0223 在两份 datasheet 里**都没有定义**，是未公开的
 *        镜像寄存器：主芯片把从路经 LINE_IO 送上来的解调信息映射到这三个地址。
 *        因为手册没有定义，语义只能靠实测确认 —— 判断方法见
 *        Docs/KT0656M_Driver_Guide.md 6.11。在实测确认之前，不要拿它做
 *        产品判据，只用于调试显示。
 *        KT0650 侧这三个地址无意义，调用返回 KT_ERR_PARAM。
 */
static int alt_quality_get(kt0656m_dev_t *dev, uint16_t reg, uint8_t *out)
{
    if (dev == 0 || out == 0) return KT_ERR_PARAM;
    if (!KT_IS_MAIN(dev))  return KT_ERR_PARAM;   /* 镜像只存在于主芯片侧 */
    return kt_rd(dev, reg, out);
}

int kt0656m_alt_rssi_get(kt0656m_dev_t *dev, uint8_t *out)
{
    return alt_quality_get(dev, KT_REG_RSSI_S, out);
}

int kt0656m_alt_fast_rssi_get(kt0656m_dev_t *dev, uint8_t *out)
{
    return alt_quality_get(dev, KT_REG_FAST_RSSI_S, out);
}

int kt0656m_alt_snr_get(kt0656m_dev_t *dev, uint8_t *out)
{
    return alt_quality_get(dev, KT_REG_SNR_S, out);
}

int kt0656m_automute_get(kt0656m_dev_t *dev, uint8_t *muted)
{
    uint8_t regx;
    int     rc;

    if (muted == 0) return KT_ERR_PARAM;
    rc = kt_rd(dev, KT_REG_AUTOMUTE_STA, &regx);
    if (rc != KT_OK) return rc;
    *muted = (regx & KT_AUTOMUTE_FLAG) ? 1u : 0u;
    return KT_OK;
}

int kt0656m_pilot_get(kt0656m_dev_t *dev, uint8_t *ok)
{
    uint8_t regx;
    int     rc;

    if (ok == 0) return KT_ERR_PARAM;
    rc = kt_rd(dev, KT_REG_SIG_STA, &regx);
    if (rc != KT_OK) return rc;
    *ok = (regx & KT_PILOT_OK) ? 1u : 0u;
    return KT_OK;
}

int kt0656m_battery_read(kt0656m_dev_t *dev, uint16_t *code)
{
    uint8_t hi;
    uint8_t lo;
    int16_t raw;
    int     rc;

    if (dev == 0 || code == 0) return KT_ERR_PARAM;

    /* 电池检测在 KT0650 上不存在（手册无 0x00C0/0x00C1） */
    KT_GUARD_MAIN(dev);

    rc = kt_rd(dev, KT_REG_BATTERY_H, &hi);
    if (rc != KT_OK) return rc;
    rc = kt_rd(dev, KT_REG_BATTERY_L, &lo);
    if (rc != KT_OK) return rc;

    /* 芯片给的是有符号值，取绝对值（与原厂一致） */
    raw = (int16_t)(((uint16_t)hi << 8) | lo);
    *code = (raw < 0) ? (uint16_t)(-raw) : (uint16_t)raw;
    return KT_OK;
}

int kt0656m_select_antenna(kt0656m_dev_t *dev)
{
    if (dev == 0) return KT_ERR_PARAM;

    /* 用 #if 而不是 #ifdef：cfg.h 里这个宏是 `#define ... 1` 形式，改成 0 表示
     * 关闭。若写成 #ifdef，把 1 改成 0 时这里仍然会编进去，和全项目其他
     * 地方的 #if (KT_CFG_DIVERSITY) 判断不一致 —— 那种不一致最难查。 */
#if (KT_CFG_DIVERSITY)
    {
        uint8_t m;
        uint8_t s;
        int     rc;

        /* 只对主芯片有意义：0x020A 是自己的快速 RSSI，0x0222 是主芯片侧
         * 看到的从路镜像。给 KT0650 调这个函数无意义，直接返回。 */
        if (!KT_IS_MAIN(dev)) {
            dev->div_main_path = 1u;
            return KT_OK;
        }

        rc = kt_rd(dev, KT_REG_FAST_RSSI_M, &m);
        if (rc != KT_OK) return rc;
        rc = kt_rd(dev, KT_REG_FAST_RSSI_S, &s);
        if (rc != KT_OK) return rc;
        dev->div_main_path = (m >= s) ? 1u : 0u;
    }
#else
    /* 无分集：恒用主路，占位保证业务层调用方式统一 */
    dev->div_main_path = 1u;
#endif
    return KT_OK;
}

int kt0656m_pkg_sync_poll(kt0656m_dev_t *dev, uint8_t *synced)
{
    uint8_t regx;
    int     rc;

    if (dev == 0) return KT_ERR_PARAM;

    rc = kt_rd(dev, KT_REG_PKG_SYNC_INT, &regx);
    if (rc != KT_OK) return rc;

    if (regx & KT_PKG_SYNC_INT) {
        /* 写 1 清中断 */
        rc = kt_wr(dev, KT_REG_PKG_SYNC_INT, (uint8_t)(regx | KT_PKG_SYNC_INT));
        if (rc != KT_OK) return rc;

        rc = kt_rd(dev, KT_REG_SIG_STA, &regx);
        if (rc != KT_OK) return rc;
        dev->pkg_synced = (regx & KT_PKG_SYNC) ? 1u : 0u;
    }

    if (synced) {
        *synced = dev->pkg_synced;
    }
    return KT_OK;
}

/* ============================ 周期维护 ============================ */

int kt0656m_patch(kt0656m_dev_t *dev)
{
    uint8_t sta;
    uint8_t rx_ctrl;
    uint8_t sig;
    uint8_t snr;
    int     rc;

    if (dev == 0) return KT_ERR_PARAM;

    KT_GUARD_MAIN(dev);

    /* ---------- 1. 射频中断使能跟随中断状态寄存器 ---------- */
    rc = kt_rd(dev, KT_REG_RF_INT_STA, &sta);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_RFAMP_INT, KT_RFAMP_INT_EN,
                ((sta & 0x7Fu) != 0u) ? KT_RFAMP_INT_EN : 0);
    if (rc != KT_OK) return rc;

    /* ---------- 2. 导频 mute 修复 ----------
     * 开了导频检测但芯片没读到导频时，手动补一个软件 mute */
    rc = kt_rd(dev, KT_REG_RX_CTRL1, &rx_ctrl);
    if (rc != KT_OK) return rc;
    rc = kt_rd(dev, KT_REG_SIG_STA, &sig);
    if (rc != KT_OK) return rc;

    if ((rx_ctrl & KT_PILOT_DET_EN) && (sig & KT_PILOT_OK) == 0u) {
        rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_PILOT_MUTE, KT_SOFT_PILOT_MUTE);
    } else {
        rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_PILOT_MUTE, 0);
    }
    if (rc != KT_OK) return rc;

    /* ---------- 3. SNR mute 修复 ----------
     * 用高低两个门限做迟滞，避免临界点上反复 mute/unmute */
    rc = kt_rd(dev, KT_REG_SNR_M, &snr);
    if (rc != KT_OK) return rc;

    if ((rx_ctrl & KT_AUTOMUTE_SNR_EN) && (snr <= KT_AUTOMUTE_SNR_LOW)) {
        rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_SNR_MUTE, KT_SOFT_SNR_MUTE);
    } else if ((snr >= KT_AUTOMUTE_SNR_HIGH) || ((rx_ctrl & KT_AUTOMUTE_SNR_EN) == 0u)) {
        rc = kt_upd(dev, KT_REG_SOFT_MUTE, KT_SOFT_SNR_MUTE, 0);
    } else {
        rc = KT_OK;   /* 迟滞区间内保持原状态 */
    }
    if (rc != KT_OK) return rc;

    return KT_OK;
}

/* ============================ 数字音频接口 ============================ */

int kt0656m_audio_if_init(kt0656m_dev_t *dev)
{
    uint8_t regx;
    int     rc;

    if (dev == 0) return KT_ERR_PARAM;

    KT_GUARD_MAIN(dev);

#if (KT_CFG_I2S_EN)
    if (dev->ch == KT_CH_B) {
        /* ---------- B 通道做主：输出时钟，A 通道跟随 ---------- */
        rc = kt_wr(dev, KT_REG_I2S_FMT, KT_I2S_FMT_MASTER);
        if (rc != KT_OK) return rc;
        rc = kt_wr(dev, KT_REG_I2S_CLK, KT_I2S_CLK_MASTER);
        if (rc != KT_OK) return rc;
        return kt_wr(dev, KT_REG_I2S_ROLE, KT_I2S_MASTER_EN);
    }

    /* ---------- A 通道做从 ---------- */
    rc = kt_wr(dev, KT_REG_I2S_ROLE, KT_I2S_SLAVE_EN);
    if (rc != KT_OK) return rc;

    /* 自动估算时序，快速模式，phase=0，unlock_tw=0 */
    rc = kt_wr(dev, KT_REG_I2S_UNLOCK, 0x01u);
    if (rc != KT_OK) return rc;
    rc = kt_upd(dev, KT_REG_I2S_UNLOCK, KT_I2S_UNLOCK_RDY, KT_I2S_UNLOCK_RDY);
    if (rc != KT_OK) return rc;

    rc = kt_wr(dev, KT_REG_I2S_FMT, KT_I2S_FMT_SLAVE);
    if (rc != KT_OK) return rc;

    rc = kt_upd(dev, KT_REG_I2S_FMT, KT_I2S_SLAVE_SYNC_EN, KT_I2S_SLAVE_SYNC_EN);
    if (rc != KT_OK) return rc;

    rc = kt_rd(dev, KT_REG_I2S_ROLE, &regx);
    if (rc != KT_OK) return rc;
    return kt_wr(dev, KT_REG_I2S_ROLE, (uint8_t)(regx | KT_I2S_SDOUT_OEN));
#else
    (void)regx;
    (void)rc;
    return KT_OK;
#endif
}

/* ============================ 错误码翻译 ============================ */
/**
 * @brief 错误码转可读字符串
 * @note  业务层不必再自己写 switch。两段码空间在这里合并：
 *          -1 ~ -8   iic_status_t（总线层）
 *          -20 ~ -23 KT 私有码（语义层）
 *        注意 KT_ERR_TIMEOUT 就是 IIC_ERR_TIMEOUT 的别名，不能重复 case。
 */
const char *kt0656m_strerror(int rc)
{
    switch (rc) {
    case KT_OK:                     return "OK";
    case IIC_ERR_PARAM:             return "ERR_PARAM";
    case IIC_ERR_BUS:               return "ERR_BUS";
    case IIC_ERR_BUSY:              return "ERR_BUSY";
    case IIC_ERR_TIMEOUT:           return "ERR_TIMEOUT(bus stuck or reg poll timeout)";
    case IIC_ERR_NACK:              return "ERR_NACK(no ack, check ADDR strap)";
    case IIC_ERR_HW:                return "ERR_HW";
    case IIC_ERR_UNSUPP:            return "ERR_UNSUPP";
    case IIC_ERR_ARB:               return "ERR_ARB";
    case KT_ERR_NOT_READY:          return "ERR_NOT_READY(CHIP_EN low or bus closed)";
    case KT_ERR_ID_MISMATCH:        return "ERR_ID_MISMATCH(not 0x4B54)";
    case KT_ERR_FW_MISMATCH:        return "ERR_FW_MISMATCH(expect 0x11)";
    case KT_ERR_PARAM:              return "ERR_PARAM(out of range)";
    case KT_ERR_CHIP_STUCK:         return "ERR_CHIP_STUCK(ack but constant data, chip not reset)";
    case KT_ERR_TUNE_CHAN:          return "ERR_TUNE_CHAN(write channel word failed)";
    case KT_ERR_TUNE_PLL_LOCK:      return "ERR_TUNE_PLL_LOCK(PLL_DONE never set, LO not locked)";
    case KT_ERR_TUNE_VCO_CALI:      return "ERR_TUNE_VCO_CALI(VCOCNT_RDY timeout, VCO dead)";
    case KT_ERR_INIT_LOBAND:        return "ERR_INIT_LOBAND(loband cali scan timeout)";
    default:                        return "ERR_UNKNOWN";
    }
}

/* ============================ 调试寄存器访问 ============================ */
int kt0656m_reg_read(kt0656m_dev_t *dev, uint16_t reg, uint8_t *val)
{
    return kt_rd(dev, reg, val);
}

int kt0656m_reg_write(kt0656m_dev_t *dev, uint16_t reg, uint8_t val)
{
    return kt_wr(dev, reg, val);
}
