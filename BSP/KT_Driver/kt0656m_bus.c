/**
 * @file    kt0656m_bus.c
 * @brief   KT0656M 总线适配层 —— 全驱动唯一接触 iic 抽象层的地方
 *
 * @note    本层职责边界：
 *            做：器件实例表、CHIP_EN 上电时序、寄存器读写的错误归一化、
 *                RMW 与带超时轮询这两个原厂缺失的基础设施
 *            不做：任何射频/音效语义（那是 kt0656m.c）
 *
 *          移植到新 MCU：只改 kt0656m_cfg.h 的 KT_DELAY_MS / KT_EN_PORT_CLK_EN，
 *          以及 iic_cfg.h 的总线映射；本文件不需要动。
 */
#include "kt0656m_priv.h"

/* ============================ 器件实例表 ============================ */
/**
 * @brief 板级物理器件表：一项 = 一颗芯片
 * @note  表里不出现任何 GPIO —— CHIP_EN 是组级共享资源，见下面的"电源"段。
 *        带分集时同一逻辑通道占两项（主路 + 从路），靠 role 区分。
 *
 *        本板 2 组 × (主 + 从) = 4 项：
 *          ch=KT_CH_A  bus=KT_IIC_BUS_A (PB13/PB14)  EN=PB12
 *          ch=KT_CH_B  bus=KT_IIC_BUS_B (PA9/PA10)   EN=PA8
 */
static kt0656m_dev_t s_kt_devs[KT_CFG_PHYS_NUM] = {
    /* ---------------- 组1 (KT_CH_A) ---------------- */
    {
        .bus           = KT_IIC_BUS_A,
        .addr          = KT_ADDR_MAIN,
        .ch            = KT_CH_A,
        .role          = KT_ROLE_MAIN,
        .ready         = 0,
        .div_main_path = 1,
        .pkg_synced    = 0,
    },
#if (KT_CFG_DIVERSITY)
    {
        .bus           = KT_IIC_BUS_A,
        .addr          = KT_ADDR_SUB,
        .ch            = KT_CH_A,
        .role          = KT_ROLE_SUB,
        .ready         = 0,
        /* 对从机自己而言，0x020A/0x020C/0x020D 才是它的信号质量；
         * 0x022x 是主芯片侧的镜像寄存器，从机读没有意义。所以这里取主路。 */
        .div_main_path = 1,
        .pkg_synced    = 0,
    },
#endif
#if (KT_CFG_CHIP_NUM > 1)
    /* ---------------- 组2 (KT_CH_B) ---------------- */
    {
        .bus           = KT_IIC_BUS_B,
        .addr          = KT_ADDR_MAIN,
        .ch            = KT_CH_B,
        .role          = KT_ROLE_MAIN,
        .ready         = 0,
        .div_main_path = 1,
        .pkg_synced    = 0,
    },
#if (KT_CFG_DIVERSITY)
    {
        .bus           = KT_IIC_BUS_B,
        .addr          = KT_ADDR_SUB,
        .ch            = KT_CH_B,
        .role          = KT_ROLE_SUB,
        .ready         = 0,
        .div_main_path = 1,
        .pkg_synced    = 0,
    },
#endif
#endif /* KT_CFG_CHIP_NUM > 1 */
};

/* 已初始化过的总线位图，避免多颗芯片挂同一条总线时重复 init */
static uint32_t s_bus_inited = 0;

/**
 * @brief 按"通道 + 角色"查实例
 * @note  分集之后 ch 不再唯一（一个通道两颗芯片），必须靠 role 区分，
 *        所以不能再拿 ch 当数组下标。表项个位数，线性查找足够。
 */
static kt0656m_dev_t *dev_find(uint8_t ch, uint8_t role)
{
    unsigned i;

    for (i = 0; i < KT_CFG_PHYS_NUM; i++) {
        if (s_kt_devs[i].ch == ch && s_kt_devs[i].role == role) {
            return &s_kt_devs[i];
        }
    }
    return 0;
}

kt0656m_dev_t *kt0656m_dev(uint8_t ch)
{
    return dev_find(ch, KT_ROLE_MAIN);
}

kt0656m_dev_t *kt0656m_dev_sub(uint8_t ch)
{
    return dev_find(ch, KT_ROLE_SUB);
}

/* ============================ 电源（组级） ============================ */
/**
 * @brief 每组的 CHIP_EN 引脚
 * @note  索引 = 逻辑通道号 ch，与 s_kt_devs[].ch 对应。
 *        本板组1 = PB12，组2 = PA8；同组内主从两颗共用一根。
 */
typedef struct {
    gpio_type *port;
    uint16_t   pin;
} kt_en_pin_t;

static const kt_en_pin_t s_en_pin[KT_CFG_CHIP_NUM] = {
    { KT_EN_A_PORT, KT_EN_A_PIN },
#if (KT_CFG_CHIP_NUM > 1)
    { KT_EN_B_PORT, KT_EN_B_PIN },
#endif
};

/* 中断脚登记表：本版只留定义不启用，故仅在开启时才建表，避免未使用告警 */
#if (KT_CFG_INT_EN)
static const kt_en_pin_t s_int_pin[KT_CFG_CHIP_NUM] = {
    { KT_INT_A_PORT, KT_INT_A_PIN },
#if (KT_CFG_CHIP_NUM > 1)
    { KT_INT_B_PORT, KT_INT_B_PIN },
#endif
};
#endif

/**
 * @brief 每组 CHIP_EN 当前是否已上电
 *
 * @note  共享 EN 的组内它是【组级】状态：同组两个器件各有各的 dev，但只有一根线。
 *        早期实现把它等效地挂在 dev 上（每个 dev 自己拉低再拉高），于是
 *        第二个器件 bus_open 时会把第一个器件复位掉，而第一个器件的 ready
 *        仍是 1 —— 之后所有读写都在打一颗已掉电的芯片，现象是"莫名其妙全 NACK"。
 *
 *        改成数组后，组1 上电不会影响组2，两组可以同时工作、也可以各自省电。
 */
static uint8_t s_pwr_on[KT_CFG_CHIP_NUM] = { 0 };

/**
 * @brief 把某组的 EN 引脚配成推挽输出
 */
static void pwr_pin_config(uint8_t ch)
{
    gpio_init_type gi;

    KT_EN_PORT_CLK_EN(s_en_pin[ch].port);

    gpio_default_para_init(&gi);
    gi.gpio_pins           = s_en_pin[ch].pin;
    gi.gpio_mode           = GPIO_MODE_OUTPUT;
    gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gi.gpio_pull           = GPIO_PULL_NONE;
    gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_init(s_en_pin[ch].port, &gi);
}

/**
 * @brief 掉电后【该组】所有器件的 ready 一律作废
 * @note  只清同组，不能把另一组也打断——那会让"分组独立"名存实亡。
 */
static void pwr_invalidate_grp(uint8_t ch)
{
    unsigned i;

    for (i = 0; i < KT_CFG_PHYS_NUM; i++) {
        if (s_kt_devs[i].ch == ch) {
            s_kt_devs[i].ready = 0;
        }
    }
}

int kt0656m_power_cycle(uint8_t ch)
{
    if (ch >= KT_CFG_CHIP_NUM) {
        return KT_ERR_PARAM;
    }

    pwr_pin_config(ch);

    /* 拉低期间必须给毫秒级时间，让芯片内部电源真正放电 */
    gpio_bits_reset(s_en_pin[ch].port, s_en_pin[ch].pin);
    s_pwr_on[ch] = 0;
    pwr_invalidate_grp(ch);
    KT_DELAY_MS(KT_EN_RESET_MS);

    gpio_bits_set(s_en_pin[ch].port, s_en_pin[ch].pin);
    KT_DELAY_MS(KT_EN_SETTLE_MS);
    s_pwr_on[ch] = 1;
    return KT_OK;
}

int kt0656m_power_on(uint8_t ch)
{
    if (ch >= KT_CFG_CHIP_NUM) {
        return KT_ERR_PARAM;
    }
    if (s_pwr_on[ch] != 0u) {
        return KT_OK;       /* 幂等：同组第二个器件上电不许打断第一个器件 */
    }
    return kt0656m_power_cycle(ch);
}

void kt0656m_power_off(uint8_t ch)
{
    if (ch >= KT_CFG_CHIP_NUM) {
        return;
    }
    pwr_pin_config(ch);
    gpio_bits_reset(s_en_pin[ch].port, s_en_pin[ch].pin);
    s_pwr_on[ch] = 0;
    pwr_invalidate_grp(ch);
}

/**
 * @brief 功耗/时序测量：写电平 + 纯等待，不碰总线
 * @note  上电分支故意不置 dev->ready —— "CHIP_EN 已拉高" 与 "器件可访问"
 *        是两件事，后者要过 ACK + POWER_ON_FINISH 两道门闩（见 bus_attach）。
 *        这里提前把 ready 置 1 会让后续寄存器读写打到还没就绪的芯片上。
 */
int kt0656m_power_hold(uint8_t ch, uint8_t on, uint32_t hold_ms)
{
    if (ch >= KT_CFG_CHIP_NUM) {
        return KT_ERR_PARAM;
    }

    pwr_pin_config(ch);

    if (on != 0u) {
        gpio_bits_set(s_en_pin[ch].port, s_en_pin[ch].pin);
        /* 只记状态，不置 ready。之后 bus_open() 的 power_on() 会幂等返回，
         * 直接进 bus_attach() 等芯片真正就绪。 */
        s_pwr_on[ch] = 1;
    } else {
        gpio_bits_reset(s_en_pin[ch].port, s_en_pin[ch].pin);
        s_pwr_on[ch] = 0;
        pwr_invalidate_grp(ch);
    }

    if (hold_ms != 0u) {
        KT_DELAY_MS(hold_ms);
    }
    return KT_OK;
}

/* ============================ 上电 / 挂载 ============================ */
int kt0656m_bus_attach(kt0656m_dev_t *dev)
{
    iic_config_t cfg;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }
    if (dev->ch >= KT_CFG_CHIP_NUM || s_pwr_on[dev->ch] == 0u) {
        return KT_ERR_NOT_READY;    /* 调用顺序错：必须先 kt0656m_power_on(dev->ch) */
    }

    /* ---------- 1. 总线初始化（同一条总线只做一次） ---------- */
    if ((s_bus_inited & (1u << dev->bus)) == 0) {
        cfg.bus        = dev->bus;
        cfg.role       = IIC_ROLE_MASTER;
        cfg.addr_bits  = IIC_ADDR_7BIT;
        cfg.speed      = IIC_SPEED_STANDARD;
        cfg.timeout_ms = KT_IIC_TIMEOUT_MS;

        if (iic_init(&cfg) != IIC_OK) {
            dev->ready = 0;
            return IIC_ERR_HW;
        }
        s_bus_inited |= (1u << dev->bus);
    }

    /* ---------- 2. 等 I2C 从机就绪：反复寻址直到 ACK ---------- */
    /* 手册说 CHIP_EN 高 10ms 后就能访问，实测 20ms 仍会 NACK。固定延时靠不住，
     * 用"能寻址到"这个事实做门闩，换批次/换温度都不用重调参数。 */
    {
        iic_status_t st = IIC_ERR_NACK;
        uint32_t     waited = 0;

        for (;;) {
            st = iic_master_write(dev->bus, dev->addr, 0, 0, KT_IIC_TIMEOUT_MS);
            if (st == IIC_OK) {
                break;
            }
            if (waited >= KT_TO_BOOT_MS) {
                dev->ready = 0;
                return (int)st;
            }
            KT_DELAY_MS(KT_TO_STEP_MS);
            waited += KT_TO_STEP_MS;
        }
    }

    /* ---------- 3. 等芯片内部上电完成（POWER_ON_FINISH=1） ---------- */
    /* 寻址能通只说明 I2C 从机活了，内部模拟/数字还没配完。此时读寄存器会拿到
     * 半初始化的值，写配置也可能被内部流程覆盖，必须等这个标志。
     * 本板实测两颗芯片（0x39 / 0x3D）用的是同一位，故逻辑可复用。 */
    {
        uint8_t  sta = 0;
        uint32_t waited = 0;
        int      rc;

        dev->ready = 1;

        for (;;) {
            rc = kt_rd(dev, KT_REG_POWER_STA, &sta);
            if (rc == KT_OK && (sta & KT_PWR_ON_FINISH)) {
                break;
            }
            if (rc != KT_OK && rc != KT_ERR_TIMEOUT &&
                rc != IIC_ERR_NACK) {
                dev->ready = 0;
                return rc;          /* 真总线故障，立刻上报，别等满超时 */
            }
            if (waited >= KT_TO_POWER_ON_MS) {
                dev->ready = 0;
                return KT_ERR_NOT_READY;
            }
            KT_DELAY_MS(KT_TO_STEP_MS);
            waited += KT_TO_STEP_MS;
        }
    }

    return KT_OK;
}

int kt0656m_bus_open(kt0656m_dev_t *dev)
{
    int rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    rc = kt0656m_power_on(dev->ch); /* 幂等：只有该组第一次真的会掉电重上 */
    if (rc != KT_OK) {
        return rc;
    }
    return kt0656m_bus_attach(dev);
}

void kt0656m_bus_close(kt0656m_dev_t *dev)
{
    if (dev == 0) {
        return;
    }
    /* 同组共享 CHIP_EN，谁调用都是整组掉电；另一组不受影响 */
    kt0656m_power_off(dev->ch);
}

int kt0656m_hw_reset(kt0656m_dev_t *dev)
{
    int rc;

    if (dev == 0) {
        return KT_ERR_PARAM;
    }

    /* 整组掉电重上：该组内所有 ready 已作废 */
    rc = kt0656m_power_cycle(dev->ch);
    if (rc != KT_OK) {
        return rc;
    }

    /* 只为 dev 自己重新挂回总线；同组其他器件需调用方重新 bus_open() */
    return kt0656m_bus_attach(dev);
}

/* ============================ 寄存器读写 ============================ */
int kt_wr(kt0656m_dev_t *dev, uint16_t reg, uint8_t val)
{
    iic_status_t st;

    if (dev == 0 || !dev->ready) {
        return KT_ERR_NOT_READY;
    }

    st = iic_master_write_reg(dev->bus, dev->addr, reg, IIC_REG_16BIT,
                              &val, 1, KT_IIC_TIMEOUT_MS);
    return (st == IIC_OK) ? KT_OK : (int)st;
}

int kt_rd(kt0656m_dev_t *dev, uint16_t reg, uint8_t *val)
{
    iic_status_t st;

    if (dev == 0 || val == 0 || !dev->ready) {
        return KT_ERR_NOT_READY;
    }

    st = iic_master_read_reg(dev->bus, dev->addr, reg, IIC_REG_16BIT,
                             val, 1, KT_IIC_TIMEOUT_MS);
    return (st == IIC_OK) ? KT_OK : (int)st;
}

int kt_upd(kt0656m_dev_t *dev, uint16_t reg, uint8_t mask, uint8_t val)
{
    uint8_t regx;
    int rc;

    rc = kt_rd(dev, reg, &regx);
    if (rc != KT_OK) {
        return rc;      /* 读失败就不写，避免把垃圾值刷进芯片 */
    }

    regx = (uint8_t)((regx & (uint8_t)~mask) | (val & mask));
    return kt_wr(dev, reg, regx);
}

/* ============================ 按地址访问（无 dev 实例） ============================ */
int kt0656m_addr_probe(iic_bus_t bus, uint8_t addr)
{
    iic_status_t st;

    /* 0x00 是通用调用地址，0x78~0x7F 是 10 位地址/保留区，都不该出现器件 */
    if (addr < 0x08u || addr > 0x77u) {
        return KT_ERR_PARAM;
    }

    st = iic_master_write(bus, addr, 0, 0, KT_IIC_TIMEOUT_MS);
    return (st == IIC_OK) ? KT_OK : (int)st;
}

int kt0656m_reg_read_at(iic_bus_t bus, uint8_t addr, uint16_t reg, uint8_t *val)
{
    iic_status_t st;

    if (val == 0) {
        return KT_ERR_PARAM;
    }

    st = iic_master_read_reg(bus, addr, reg, IIC_REG_16BIT, val, 1,
                             KT_IIC_TIMEOUT_MS);
    return (st == IIC_OK) ? KT_OK : (int)st;
}

int kt_poll(kt0656m_dev_t *dev, uint16_t reg, uint8_t mask, uint8_t want,
            uint32_t timeout_ms)
{
    uint32_t waited = 0;
    uint8_t  regx;
    int      rc;

    for (;;) {
        rc = kt_rd(dev, reg, &regx);
        if (rc != KT_OK) {
            return rc;
        }
        if ((regx & mask) == want) {
            return KT_OK;
        }
        if (waited >= timeout_ms) {
            return KT_ERR_TIMEOUT;
        }
        KT_DELAY_MS(KT_TO_STEP_MS);
        waited += KT_TO_STEP_MS;
    }
}
