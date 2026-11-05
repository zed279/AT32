# KT06xx 芯片知识卡

> 依据：`Docs/KT/KT0656M_datasheet_V0.7.8_CN.pdf`（51 页）、`Docs/KT/KT0650_datasheet_V0.1.2_CN.pdf`（29 页）
> 用途：寄存器级速查 + 时序链记忆。对代码时优先看 `BSP/KT_Driver/kt0656m_reg.h`。

---

## 1. 定位：一颗主 + 一颗从

| | KT0656M | KT0650 |
|---|---|---|
| 角色 | 主接收机：解调 + 音频处理 + 音效 + 模拟/数字音频输出 + 分集判决 | 分集辅助机：只做 LNA/Mixer/LO/FM 解调，解调结果经 LINE_IO 送到主芯片 |
| 封装 | QFN32 5×5 | QFN24 4×4 |
| 音频输出 | AOUTP/AOUTN 差分 + SAI 数字 | **无**（不外接音频脚） |
| 音效器 | EQ/激励/Echo/啸叫抑制 | 无 |
| DCDC | 内置（DCDC_IN 3.4~4.3V → 输出一半） | 无 |
| 电池检测 | 有（VBAT_DET，0~1.2V，11bit） | 无 |
| 典型电流 | <60mA；关机 75~80µA | 同 |

链路本质：**KT0650 解调 → LINE_IO 数字流 → KT0656M 比较两路信号质量 → 选好的那路 → 统一输出**。判决在 0656M 内部，MCU 不参与（驱动只置 `DIVERSITY_EN`）。

两颗芯片频率必须一致、天线间距 ≥ λ/4。

---

## 2. 关键指标（表 4，Ta=-30~70℃）

| 项 | 值 |
|---|---|
| 频段 | 470~960 MHz |
| 灵敏度 | -108 dBm @12dB SINAD |
| 音频动态范围 | ≥106 dB |
| THD | <0.5%（最大音量） |
| 输出摆幅 | 1 Vrms（差分 AOUTP-AOUTN） |
| 音频驱动 | 1 kΩ |
| 频响 | 20Hz~18kHz（±3dB 内） |
| 频道步进 | 25 kHz |
| 最大频偏 | 120 kHz |
| 去加重 | 75 µs（固定，只可开关） |
| 扩展器时间常数 | 6~796 ms，8 档 |
| 晶体 | 24 / 24.576 MHz，±50ppm |

⚠ 手册自相矛盾：表 4 写 -108dBm@12dB SINAD，4.1 正文写 "-93dBm @80dB"。以表 4 为准。

---

## 3. 上电时序链（最容易踩的一段）

```
CHIP_EN 拉高
  └─ 等 ≥10ms（手册 4.2，2-wire 才可用）
       └─ 等 POWER_UP_FINISH(0x7F bit0)=1  或  上电中断标志(0x59 bit4)
            └─ 清上电中断（不清不能继续初始化）
                 └─ CHIP_EN 高后 10~60ms 内置 DCDC_EN(0x2D bit2)=1
                      └─ 其余寄存器初始化
                           └─ 写 CHAN<19:0> → CHAN_VALID(0x47 bit0)=1（硬件自动清 0）
                                └─ 等 PLL_READY(0x61 bit0)=1，**最迟 260ms**
```

- `CHIP_EN` 拉低 = 关机，SCL/SDA 进高阻（待机时 48kΩ 上拉仍使能）。
- 关机电流 75~80µA —— 所以"待机"必须拉低 CHIP_EN，光关功能没用。
- **CHIP_EN 拉低要保持足够久**让片内电源放电，否则芯片保留上次寄存器状态（驱动用 100ms）。

---

## 4. 晶体选择：两个寄存器必须配对

| 寄存器 | 位 | 含义 |
|---|---|---|
| `ANA_CFG` 0x0012 | bit7 `XTAL_SEL` | 0 = 晶体在 XI-XO1，1 = 在 XI-XO2 |
| `SW_CFG8` 0x0108 | bit7 `XTAL1_SEL` | XI-XO1 上的晶体：0=24M，1=24.576M |
| `SW_CFG8` 0x0108 | bit6 `XTAL2_SEL` | XI-XO2 上的晶体：0=24M，1=24.576M |

规则：**只用一颗晶体时必须接 XI-XO1**，并置 `XTAL1_SEL` = 实际频率。
配错 = 振荡器指向没焊晶体的那组脚 = 无参考时钟 = PLL 永不锁，且没有任何报错。

---

## 5. 频道设置

- `CHAN<19:0>`（0x45/0x46/0x47 bit[7:4]）单位 **1 kHz**，直接写载波频率数值，20 位。
- 写 `CHAN_VALID` 触发，硬件自动清零。
- 不需片外 VCO 电感（上一代需要）。
- 频率步进 25kHz，但产品习惯按 300kHz 排布。

---

## 6. AFC

| 寄存器 | 位 | 含义 |
|---|---|---|
| `AFC_CTRL_EN` 0x10E bit0 | | 自动控制（芯片按工作状态自己开/关 AFC），默认 1 |
| `AFC_EN` 0x218 bit1 | | 手动使能，**仅 AFC_CTRL_EN=0 时有效**，默认 1 |
| `AFC_RNG` 0x217 bit[7:6] | | 00 ±20 / 01 ±40 / 10 ±60 / 11 ±90 kHz |
| `AFC_STATUS0` 0x219 | | 累计频偏调节量，kHz |
| `AFC_STATUS1` 0x21A | | 尚未调节的残余频偏，kHz |

两个使能是**串联**关系，不是并列。想完全关掉 AFC 要先把 `AFC_CTRL_EN` 清 0，再清 `AFC_EN`。

---

## 7. Automute：四个判据 + 三档 RSSI

判据来源（`SW_CFG0` 0x100）：

| 位 | 名称 | 默认 |
|---|---|---|
| bit7 | `PILOT_MUTE_EN` 导频作为判据 | 1 |
| bit5 | `SNR_MUTE_EN` SNR 作为判据 | 1 |

执行开关：`AUTOMUTE_MUTE_EN` (0x225 bit0) = 1 才真的静音；= 0 只报告不动作。
状态：`AUTOMUTE` (0x88 bit0)，1 = 满足静音条件。

信号质量寄存器（步进见备注）：

| 地址 | 名称 | 步进 |
|---|---|---|
| 0x20A | `FAST_RSSI` | 0.5 dB，快但精度差 |
| 0x20C | `RSSI` | 1 dB |
| 0x20D | `SNR` | 1 dB |
| 0x209 bit[3:0] | `AUDIO_PEAK_OUT` | 6 dB，FM 峰值频偏 |

`AUDIO_PEAK_OUT` 的 0dB 基准 = `ADJUST_GAIN` 设定的最大频偏。

---

## 8. 数字音频接口（SAI）

### 主设备

| 晶体 | MCLK_SEL | MCLK | RATIO | LRCLK(Fs) | SCLK |
|---|---|---|---|---|---|
| 24M | 0 | 12M | 128 | 93.75k | 6M |
| 24M | 0 | 12M | 256 | 46.875k | 3M |
| 24M | 1 | 24M | 128 | 187.5k | 12M |
| 24M | 1 | 24M | 256 | 93.75k | 6M |
| 24.576M | 0 | 12.288M | 128 | 96k | 6.144M |
| 24.576M | 0 | 12.288M | 256 | 48k | 3.072M |
| 24.576M | 1 | 24.576M | 128 | 192k | 12.288M |
| 24.576M | 1 | 24.576M | 256 | 96k | 6.144M |

SCLK 固定 = LRCLK × 64。主模式**只支持 24bit**。

### 从设备

Fs 任意 32k~192k，SCLK 最高 24.576MHz，MCLK 12~24.576MHz，位宽 8/16/20/24bit 可选。

### 寄存器

| 地址 | 关键位 |
|---|---|
| 0x50 `SAI_SLAVE_CFG` | bit6 `SAI_SLAVE_SYNC_EN`（配完 MODE/DSIZE 后置 1 才生效，硬件自清）、bit5 `STEREO_MONO_SEL`、bit4 `LR_SEL`、bit[3:2] `SAI_SLAVE_MODE`、bit[1:0] `SAI_SLAVE_DSIZE` |
| 0x51 `SAI_MASTER_CFG` | bit3 `MASTER_MCLK_LRCLK_RATIO`、bit2 `MASTER_MCLK_SEL`、bit[1:0] `MASTER_MODE` |
| 0x52 `SAI_INTERFACE_EN` | bit1 `SAI_MASTER_EN`、bit0 `SAI_SLAVE_EN` |

- 从模式 MODE：00 保留 / 01 I2S / 10 左对齐 / 11 右对齐。
- 从模式 DSIZE：00 16bit / 01 20bit / 10 24bit / **11 8bit**（不是线性递增，易错）。
- **单颗芯片**（只出一个声道）必须先清 `STEREO_MONO_SEL`=0，此时 `LR_SEL` 无意义，SDOUT 左右声道输出同一份数据。
- **两颗芯片**共用一条 SAI：两颗都置 `STEREO_MONO_SEL`=1，一颗 `LR_SEL`=1 作左、另一颗 =0 作右；左右切换时非本声道的 SDOUT 进高阻。

---

## 9. 音频处理链路

```
FM 解调
  → ADJUST_GAIN<2:0>    把不同标准的最大频偏信号归一到固定幅度（保住动态范围、不饱和）
  → 扩展器（1:2）+ 去加重（75µs），顺序由 DE_BEHIND 决定
  → 音效器（EQ / 激励 / Echo / 啸叫抑制，4 个模块顺序由 AUDIO_ORDER<4:0> 定，24 种排列）
  → AUDIO_GAIN<1:0>     0 / 3.5 / 6 / 9.5 dB
  → RX_VOL<4:0>         0=Mute，1=-60dB … 30=-2dB，31=0dB（每档 2dB）
  → AOUTP/AOUTN + SDOUT
```

### ADJUST_GAIN 编码（按寄存器表，别按正文枚举顺序）

| 值 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 频偏 | 75k | 50k | 37.5k | 25k | 20k | 120k | 100k | 保留 |

⚠ 4.10 正文的枚举顺序（120/100/75/50/37.5/25/20）与寄存器表**不一样**，以表为准。

### 扩展器 / 去加重（0x256 `DE_EXP_CFG`）

| 位 | 名称 | 说明 |
|---|---|---|
| bit[6:4] | `EXP_TC` | 000=6ms 001=12ms 010=24ms 011=48ms 100=93ms 101=199ms 110=398ms 111=796ms |
| bit3 | `DEEMPHASIS_EN` | 默认 1 |
| bit2 | `EXP_EN` | 默认 1 |
| bit0 | `DE_BEHIND` | 0=先去加重再扩展，1=先扩展再去加重（默认） |

### EQ：15 段，0x257~0x265

25 / 40 / 63 / 100 / 160 / 250 / 400 / 630 / 1k / 1.6k / 2.5k / 4k / 6.3k / 10k / 16k Hz

每段 bit[4:0]：`00000`=-12dB … `01100`=0dB … `11000`=+12dB（1dB 步进，`01100` 是 0dB 不是 `10000`）。
`EQ_EN` 在 **0x257 bit5**（和第一段增益同字节）。

### 激励器

- `EXCITER_EN` 0x269 bit7。
- 奇/偶次谐波独立可调，0x26A：bit[6:4] ODD、bit[2:0] EVEN。
- 幅度编码：000=-∞dB，001=-30，010=-24，011=-18，100=-12，101=-6，110=0。

### Echo

- `ECHO_EN` 0x266 bit7。
- `ECHO_RATIO` 0x267 bit[4:0]：0 ~ 25/32。
- `ECHO_DELAY` 0x268 bit[4:0]：0~23 → **22ms 起步，非线性递增**（22/24/27/29/32/35/39/43/47/52/57/63/69/76/84/92/101/111/122/135/148/163/179/197）。

### 啸叫抑制（0x26F `SQUEAL_CFG0`）

| 位 | 名称 |
|---|---|
| bit5 | `SQUEAL_ELIM_EN` 抑制总开关 |
| bit[4:3] | `SQUEAL_DET_EN` 检测开关：11=开，00=关，01/10 保留 |

- `ELIM=1` + `DET=0` → 手动模式（参数要自己给）
- `ELIM=1` + `DET=11` → 自动模式（芯片自己算参数）
- 具体参数算法在 APP NOTE，datasheet 没写。

---

## 10. 辅助数字信道（要配 KT0646M 发射端）

- 载波固定 30kHz 或 30.72kHz，同时充当**导频**。
- 使能：`AUXDATA_EN` (0x241 bit7)=1；`PILOT_DETECT` (0x209 bit7) 指示是否检测到导频。
- 两种模式：
  - **循环模式**：发射端按 A→B→C→D→A 反复发 4 个 16bit 数据；接收端 `PKG_SYNC`=1 后从 `AUX_DATA_A/B/C/D` (0x248~0x24F) 读。
  - **突发模式**：只发一次；靠 `BURST_INT` 中断 + `BURST_DATA` (0x246/0x247) 读，读后清标志。
- **读 16bit 寄存器的顺序是强制的**：先读 H，芯片自动锁存 L，再读 L。反过来读到的是拼接垃圾。
  适用：`BATTERY_CODE`、`BURST_DATA`、`AUX_DATA_A~D`。

---

## 11. 电池检测

- `VBAT_DET` 引脚，量程 0~1.2V，**11bit 无符号**，0~2048，参考 1.2V。
- 外部分压 R1/R2（典型 22k / 15k）保证 VBAT_DET ≤ 1.2V。
- 读数 `BATTERY_CODE<10:8>` (0xC0 bit[2:0]) + `<7:0>` (0xC1)，**必须先读 0xC0**。
- 换算：`VBAT = code / 2048 × 1.2 × (R1+R2)/R2`。

---

## 12. 中断

三个源，`INT_FLAG` (0x59) 对应位，**写 0 清**（不是写 1 清）：

| 位 | 标志 | 使能 |
|---|---|---|
| bit4 | `POWER_UP_FINISH_INT_FLAG` | 0x55 bit4 |
| bit5 | `AUTOMUTE_INT_FLAG` | 0x55 bit5 |
| bit6 | `BURST_INT_FLAG` | 0x55 bit6 |

- `INT_LEVEL` (0x55 bit7)：0 = 高有效，1 = 低有效。
- **所有标志清完，INT 引脚才恢复**。automute 中断还要回读 `AUTOMUTE` (0x88 bit0) 才能判断是"进入静音"还是"退出静音"。

---

## 13. 2-wire 接口

- 设备地址 7 位：`7'b011x001`
  - KT0656M：ADDR=0 → `0x31`，ADDR=1 → `0x39`
  - KT0650 ：`7'b011x101`：ADDR=0 → `0x35`，ADDR=1 → `0x3D`
  - 同一条总线上最多 2 主 + 2 从 = 4 颗，靠 ADDR + 型号位天然错开。
- 寄存器地址 **16 位**（先高后低），数据 **8 位**，MSB first。
- 内部地址计数器自增 → 支持连续读写。
- SCL 最高 400kHz；tLOW ≥1.3µs、tHIGH ≥0.6µs。
- 写：START → 地址+W → 地址H → 地址L → data → … → STOP
- 读：START → 地址+W → 地址H → 地址L → **Sr** → 地址+R → data → … → NACK → STOP

---

## 14. 寄存器速查（手册定义的）

| 地址 | 名称 | 关键位 |
|---|---|---|
| 0x0012 | ANA_CFG | bit7 XTAL_SEL |
| 0x002D | ANA_CFG38 | bit2 DCDC_EN |
| 0x0045/46/47 | CHAN_CFG0/1/2 | CHAN<19:0>；0x47 bit0 CHAN_VALID |
| 0x0050 | SAI_SLAVE_CFG | 见 §8 |
| 0x0051 | SAI_MASTER_CFG | 见 §8 |
| 0x0052 | SAI_INTERFACE_EN | bit1 MASTER_EN / bit0 SLAVE_EN |
| 0x0055 | INT_EN | bit7 LEVEL, bit6 BURST, bit5 AUTOMUTE, bit4 POWER_UP |
| 0x0059 | INT_FLAG | 同上，写 0 清 |
| 0x0061 | PLL_FSM_CTRL5 | bit0 PLL_READY（只读） |
| 0x007F | POWER_STA_CTRL1 | bit0 POWER_ON_FINISH（只读） |
| 0x0088 | AUTOMUTE_FSM_STA | bit0 AUTOMUTE（只读，默认 1） |
| 0x00C0 | ADC_CHAN0_HB | BATTERY_CODE<10:8>（只读，**先读**） |
| 0x00C1 | ADC_CHAN0_LB | BATTERY_CODE<7:0>（只读） |
| 0x0100 | SW_CFG0 | bit7 PILOT_MUTE_EN, bit5 SNR_MUTE_EN |
| 0x0108 | SW_CFG8 | bit7 XTAL1_SEL, bit6 XTAL2_SEL |
| 0x010E | SW_CFG14 | bit0 AFC_CTRL_EN |
| 0x0192/0x0193 | MANUFACTURER_ID | 0x4B / 0x54（只读；**两颗芯片 ID 相同，不能靠它区分型号**） |
| 0x0200 | TOP_CFG0 | bit[6:4] ADJUST_GAIN |
| 0x0201 | TOP_CFG1 | bit[4:0] RX_VOL |
| 0x0209 | TOP_STATUS0 | bit7 PILOT_DETECT, bit6 PKG_SYNC, bit[3:0] AUDIO_PEAK_OUT |
| 0x020A | TOP_STATUS1 | FAST_RSSI（0.5dB） |
| 0x020C | TOP_STATUS3 | RSSI（1dB） |
| 0x020D | TOP_STATUS4 | SNR（1dB） |
| 0x0217 | AFC_CFG0 | bit[7:6] AFC_RNG |
| 0x0218 | AFC_CFG1 | bit1 AFC_EN |
| 0x0219 | AFC_STATUS0 | AFC_DF_ACC（累计调节量） |
| 0x021A | AFC_STATUS1 | AFC_CURRENT（残余偏差） |
| 0x021C | ANTDIV_CFG1 | bit6 DIVERSITY_EN |
| 0x0224 | AUDIOLINK_CFG | bit[6:5] AUDIO_GAIN, bit[4:0] AUDIO_ORDER |
| 0x0225 | AUTOMUTE_CFG0 | bit0 AUTOMUTE_MUTE_EN |
| 0x0241 | BPSK_CFG0 | bit7 AUXDATA_EN, bit6 BPSK_NEW_MODE |
| 0x0246/47 | BURST_DATAH/L | 先 H 后 L |
| 0x0248~0x24F | AUX_DATA_A~D H/L | 先 H 后 L |
| 0x0256 | DE_EXP_CFG | 见 §9 |
| 0x0257~0x0265 | EQ_CFG0~14 | bit5 EQ_EN（仅 0x257）, bit[4:0] 增益 |
| 0x0266/67/68 | ECHO_CFG0/1/2 | EN / RATIO / DELAY |
| 0x0269/0x026A | EXCITER_CFG0/1 | EN / ODD+EVEN |
| 0x026F | SQUEAL_CFG0 | bit5 ELIM_EN, bit[4:3] DET_EN |

---

## 15. 同址不同义：分集双芯片最容易翻车的地方

| 地址 | KT0656M | KT0650 | 后果 |
|---|---|---|---|
| 0x0108 | bit7/6/5 有定义，bit[5:0] 保留 | 只有 bit7/bit6，bit[5:0] 保留 | 拿 bit4 当校准使能 → 0650 上无对应硬件 → 轮询永远等不到清 0 → 300ms 超时 |
| 0x0225 | `AUTOMUTE_CFG0` | `AUTOMUTE_CFG0` | 同义，但手册都标 bit[7:1] 保留 |
| 0x021C | bit6 DIVERSITY_EN（bit[5:0] 保留默认 `00_1001`） | 同 | 用主路 mask 清 bit[4:3] 会改掉从路默认值 |
| 0x0100 | bit7 PILOT_MUTE_EN / bit5 SNR_MUTE_EN | 同义（名字叫 SNR_MUTE_EN 等） | 可共用 |
| 0x0217 / 0x0218 | AFC_RNG / AFC_EN | 同 | 可共用 |
| 0x0209 | PILOT_DETECT / PKG_SYNC / AF | 同 | 可共用 |
| 0x00C0 / 0x00C1 | 电池检测 | **不存在** | 0650 上读电池 = 未定义 |
| 0x026F 及音效器 | 有 | **不存在** | 音效配置必须只下发给主路 |

---

## 16. 手册未公开、但原厂参考代码在用的寄存器

这些 datasheet **查不到**，只能从原厂驱动继承。改它们时没有文档兜底，务必一次只改一个位并实测：

`0x000D` LDO 电流、`0x0010` VTR_CFG、`0x0016/17/18/1A/1C/1E` PLL/VCO 相关、`0x001F` BIAS_CFG、
`0x0030` LO 锁定、`0x0042` SDM 复位、`0x004D` I2S unlock、`0x0053` RFAMP 中断、`0x0057/0x005B` 中断状态、
`0x005C` 频段校准门控、`0x0064~0x0069` VCOCNT 结果、`0x0079/0x007A` SNR 门限、`0x0081/0x0082` automute 门限、
`0x0087` 软静音、`0x0109` PLL 噪声、`0x010F` FW_VERSION、`0x0133` PLL 复位、
从路 0x0221~0x0223（RSSI/FAST_RSSI/SNR）、`0x0243/0x0245` BPSK 包参数。

> 经验：手册覆盖的只是"客户需要配的那一层"，射频校准、VCO 频段表这类出厂参数层没有公开。所以 KT 驱动的价值主要在"原厂参考序列"，不在 datasheet。

---

## 17. 手册与现有驱动的三处不一致（已核对，待上板确认）

| # | 位置 | 手册怎么说 | 驱动怎么做 | 判断 |
|---|---|---|---|---|
| 1 | 0x0225 | `AUTOMUTE_CFG0`，bit0 才是 `AUTOMUTE_MUTE_EN`，bit[7:1] 标 Reserved（默认 `0000_111`） | `kt0656m.c:596` 注释为 `DC_NOTCH_MUTE_EN`，写 mask `0x0E` 把 bit[3:1] 从 `111` 清成 `000` | **操作的是手册标注的保留位**。两种解释：① 原厂内部这些位确为 DC notch 静音控制（默认 111 = 三档静音开，清 0 = 关），则这是有意配置；② 纯保留位则无影响。bit0 未动、静音总闸仍为默认开，两种情况都不至于坏事，但**没有文档依据**，改动时只能靠实测 |
| 2 | 0x0256 bit[6:4] | `EXP_TC` = 扩展器时间常数，`001`=12ms | 命名 `KT_COMP_TC`，注释写"温漂补偿档位" | **命名/注释错**，取值 1 实际就是 12ms 扩展器时间常数。无功能影响，但会误导后来人 |
| 3 | 0x00C0/0xC1 | 11bit **无符号**，0~2048，参考 1.2V | 注释"有符号 16 位"，代码 `(int16_t)` 后取绝对值 | 高字节只有 bit[2:0] 有效，拼接后恒 ≥0，`raw<0` 是**死分支**，结果不会错。概念错、代码无害 |

另外一处功能缺口：**`SQUEAL_CFG0` (0x026F) 啸叫抑制驱动完全没实现**——手册 4.13.4 明确有手动/自动两种模式，datasheet 说参数算法见 APP NOTE。

---

## 18. 记忆锚点

1. **10ms / 260ms**：CHIP_EN 后 10ms 可访问总线；CHAN_VALID 后 PLL 最迟 260ms 锁。
2. **0x0108 与 0x0012 必须配对**，单晶体接 XI-XO1。
3. **16bit 寄存器先读高字节**，芯片靠这个动作锁存低字节。
4. **中断标志写 0 清**，不是写 1 清。
5. **ADJUST_GAIN 的 0~4 是 75k→20k 递减，5/6 才跳回 120k/100k**，不连续。
6. **EQ 的 0dB 是 `01100`**，不是 `10000`。
7. **SAI 从模式 DSIZE 的 `11` 是 8bit**，不是 32bit。
8. 两颗芯片 **CHIP_ID 相同（0x4B54）**，区分型号只能看 I2C 地址那位。
