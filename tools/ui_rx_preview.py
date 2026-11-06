#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ui_rx.c 的离线预览渲染器 —— 只为"看图检查布局", 不参与编译。

它用的是 ui_rx.c 里那一套坐标常量与配色, 所以能提前发现:
  * 文字超出卡片 / 压到分隔线
  * 图标与文字错位
  * 条幅长度不够或溢出
改布局时先改这里看效果, 再同步到 C 代码, 免得反复烧板。

    python tools/ui_rx_preview.py
    -> tools/ui_rx_preview.png  (320 x 240)
"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

SS = 4  # 超采样倍数, 让圆角和斜线不那么锯齿

W, H = 320, 240

# ---- 配色 (与 ui_rx.c 一致) ----
BG      = (0x1A, 0x1A, 0x1A)
BAR     = (0x12, 0x12, 0x12)
CARD    = (0x20, 0x20, 0x20)
LINE    = (0x2C, 0x2C, 0x2C)
TITLE   = (0x17, 0x3E, 0x6E)
BLUE    = (0x2E, 0x8B, 0xFF)
TEXT    = (0xEA, 0xEA, 0xEA)
DIM     = (0x8C, 0x8C, 0x8C)
TRACK   = (0x2A, 0x2A, 0x2A)
GREEN   = (0x2E, 0xC4, 0x6B)
YELLOW  = (0xEA, 0xB3, 0x08)
ORANGE  = (0xF9, 0x73, 0x16)
RED     = (0xEF, 0x44, 0x44)
GREY    = (0x50, 0x50, 0x50)

# ---- 布局 (与 ui_rx.c 一致) ----
STATUS_H = 28
HINT_Y = 220
CARD_Y, CARD_H, CARD_W = 30, 190, 157
CARD_AX, CARD_BX = 2, 161

TITLE_Y, TITLE_H = 2, 18
PAD = 5
CONTENT_W = CARD_W - 2 * PAD

FREQ_Y, FREQ_H, UNIT_X = 22, 26, 92
DIV1_Y = 50
RSSI_TXT_Y, RSSI_BAR_Y, RSSI_BAR_H = 53, 68, 14
AUD_TXT_Y, AUD_BAR_Y, AUD_BAR_H = 87, 102, 34
DIV2_Y = 140
ST1_Y, ST2_Y = 146, 164
TXT_H = 14
VAL_W = 56

ENV_COLS, ENV_BW, ENV_GAP = 29, 4, 1

FONT_DIR = "C:/Windows/Fonts"


def font(size):
    for name in ("consola.ttf", "consolab.ttf", "cour.ttf"):
        p = os.path.join(FONT_DIR, name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


F14 = font(12)
F24 = font(23)


def dim(c, pct):
    return tuple(int(v * pct / 100) for v in c)


def noise(phase, i):
    v = (phase + i * 37) & 0xFFFF
    v = (v * 1103 + 12345) & 0xFFFF
    v ^= v >> 8
    return v & 0xFF


RSSI_TH = [-97, -93, -89, -85, -80, -75, -69, -62]


def rssi_pct(dbm):
    n = 0
    for i, th in enumerate(RSSI_TH):
        if dbm >= th:
            n = i + 1
    return n * 100 // 8


def level_color(db, weak, bad):
    if db <= bad:
        return RED
    if db <= weak:
        return ORANGE
    if db <= weak + 15:
        return YELLOW
    return GREEN


class Canvas:
    def __init__(self):
        self.img = Image.new("RGB", (W * SS, H * SS), BG)
        self.d = ImageDraw.Draw(self.img)

    def rect(self, x, y, w, h, color, radius=0, outline=None, border=0):
        box = [x * SS, y * SS, (x + w - 1) * SS, (y + h - 1) * SS]
        self.d.rounded_rectangle(box, radius=radius * SS, fill=color,
                                 outline=outline, width=border * SS)

    def hline(self, y, x1, x2, color, w=1):
        self.d.rectangle([x1 * SS, y * SS, x2 * SS, (y + w - 1) * SS + SS - 1], fill=color)

    def vline(self, x, y1, y2, color, w=1):
        self.d.rectangle([x * SS, y1 * SS, (x + w - 1) * SS + SS - 1, y2 * SS], fill=color)

    def line(self, x1, y1, x2, y2, color, w=1):
        self.d.line([x1 * SS, y1 * SS, x2 * SS, y2 * SS], fill=color, width=w * SS)

    def circle(self, cx, cy, r, color):
        self.d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=color)

    def ring(self, cx, cy, r_in, r_out, color):
        self.d.ellipse([(cx - r_out) * SS, (cy - r_out) * SS,
                        (cx + r_out) * SS, (cy + r_out) * SS], fill=color)
        self.d.ellipse([(cx - r_in) * SS, (cy - r_in) * SS,
                        (cx + r_in) * SS, (cy + r_in) * SS], fill=BG)

    def tri(self, xbase, xapex, yt, yb, color):
        # 与 C 里 rx_fill_tri 同一套逐行算法
        hgt = yb - yt
        if hgt < 2:
            return
        ymid = (yt + yb) // 2
        reach = hgt // 2
        for y in range(yt, yb + 1):
            dd = y - ymid if y > ymid else ymid - y
            xe = xbase + int((xapex - xbase) * (reach - dd) / reach)
            self.hline(y, min(xbase, xe), max(xbase, xe), color)

    def text(self, x, y, s, color, f=F14, align="left"):
        tw = self.d.textlength(s, font=f)
        if align == "right":
            x = x - tw / SS
        elif align == "center":
            x = x - tw / (2 * SS)
        self.d.text((x * SS, y * SS), s, font=f, fill=color)
        return tw / SS


def draw_icon(c, kind, x, y, w, h, color, level=100, slashed=False):
    if kind == "dot":
        c.circle(x + w // 2, y + h // 2, max(1, (min(w, h) - 1) // 2), color)
    elif kind == "ant":
        cx = x + w // 2
        c.vline(cx, y + h // 2, y + h - 1, color)
        c.line(cx - w // 2 + 1, y, cx, y + h // 2, color)
        c.line(cx + w // 2 - 1, y, cx, y + h // 2, color)
        c.hline(y + h, cx - 1, cx + 1, color)
    elif kind == "lock":
        cx = x + w // 2
        c.vline(cx - 2, y + 1, y + 4, color)
        c.vline(cx + 2, y + 1, y + 4, color)
        c.hline(y + 1, cx - 2, cx + 2, color)
        c.rect(x + 1, y + 5, w - 2, h - 5, color, radius=1)
    elif kind == "batt":
        c.rect(x, y, w - 1, h, None, radius=1, outline=color, border=1)
        c.rect(x + w - 2, y + 2, 2, h - 4, color)
        inner = w - 1 - 3
        fw = int(inner * level / 100)
        if fw > 0:
            c.rect(x + 2, y + 2, fw, h - 4, color)
    elif kind == "gear":
        cx, cy = x + w // 2, y + h // 2
        c.ring(cx, cy, 2, 4, color)
        c.vline(cx, y, y + 3, color)
        c.vline(cx, y + h - 3, y + h, color)
        c.hline(cy, x, x + 3, color)
        c.hline(cy, x + w - 3, x + w, color)
    elif kind == "spk":
        c.rect(x, y + h // 3, w // 4, h - 2 * (h // 3), color)
        c.tri(x + w, x + w // 4, y, y + h, color)
        if slashed:
            c.line(x, y, x + w, y + h, RED)
    elif kind == "tri_l":
        c.tri(x + w, x, y, y + h, color)
    elif kind == "tri_r":
        c.tri(x, x + w, y, y + h, color)


def draw_meter_seg(c, x, y, w, h, pct, phase, color, cells=8):
    gap = 2
    cw = (w - gap * (cells - 1)) // cells
    xoff = x + (w - (cw * cells + gap * (cells - 1))) // 2
    lit = (pct * cells + 50) // 100
    for i in range(cells):
        cx = xoff + i * (cw + gap)
        if i < lit:
            top = y + noise(phase, i) % 3
            c.rect(cx, top, cw, y + h - top, dim(color, 62 + i * 38 // (cells - 1)), radius=1)
        else:
            c.rect(cx, y, cw, h, TRACK, radius=1)


def draw_meter_env(c, x, y, w, h, pct, phase, color, n=ENV_COLS, bw=ENV_BW, gap=ENV_GAP):
    base = y + h - 1
    c.hline(base, x, x + w - 1, TRACK)
    if pct == 0:
        c.text(x + w / 2, y + h / 2 - 6, "NO SIGNAL", GREY, align="center")
        return
    span = n * bw + (n - 1) * gap
    x0 = x + (w - span) // 2
    mid = n - 1
    for i in range(n):
        win = (4 * i * (mid - i) * 100) // (mid * mid)
        win = 40 + win * 60 // 100
        amp = pct * h * win // 10000
        jit = noise(phase + i * 3, i) % 21 - 10
        amp += amp * jit // 100
        amp = max(2, min(amp, h - 1))
        bx = x0 + i * (bw + gap)
        c.rect(bx, base - amp + 1, bw, amp, dim(color, 10 + win * 90 // 100), radius=1)


def draw_card(c, cx, name, d):
    x = cx
    c.rect(x, CARD_Y, CARD_W, CARD_H, CARD, radius=4,
           outline=(BLUE if d["selected"] else LINE), border=2)

    # 标题栏
    c.rect(x + 2, CARD_Y + TITLE_Y, CARD_W - 4, TITLE_H, TITLE, radius=3)
    ty = CARD_Y + TITLE_Y
    c.text(x + 7, ty + 2, name, TEXT)

    dax = x + CARD_W - 6 - 101
    draw_icon(c, "dot", dax, ty + 6, 6, 6, d["link_color"])
    c.text(dax + 8, ty + 2, "LINK", d["link_color"] if d["link"] else DIM)

    dpx = x + CARD_W - 6 - 62
    draw_icon(c, "dot", dpx, ty + 6, 6, 6, d["pilot_color"])
    c.text(dpx + 8, ty + 2, d["pilot_txt"], d["pilot_color"] if d["pilot"] else DIM)

    # 频点
    fy = CARD_Y + FREQ_Y
    tw = c.text(x + PAD, fy + 1, d["freq"], TEXT, F24)
    c.text(x + PAD + tw + 3, fy + 8, "MHz", DIM)

    c.hline(CARD_Y + DIV1_Y, x + PAD, x + PAD + CONTENT_W - 1, LINE)

    vx = x + PAD + CONTENT_W - VAL_W
    # RSSI
    ry = CARD_Y + RSSI_TXT_Y
    c.text(x + PAD, ry + 1, "RSSI", DIM)
    c.text(vx + VAL_W, ry + 1, d["rssi_txt"], d["rssi_color"], align="right")
    draw_meter_seg(c, x + PAD, CARD_Y + RSSI_BAR_Y, CONTENT_W, RSSI_BAR_H,
                   d["rssi_pct"], d.get("phase", 7), d["rssi_color"])

    # AUDIO
    ay = CARD_Y + AUD_TXT_Y
    c.text(x + PAD, ay + 1, "AUDIO", DIM)
    c.text(vx + VAL_W, ay + 1, d["aud_txt"], d["aud_color"], align="right")
    draw_meter_env(c, x + PAD, CARD_Y + AUD_BAR_Y, CONTENT_W, AUD_BAR_H,
                   d["aud_pct"], d.get("phase", 7) + 3, d["aud_color"])

    c.hline(CARD_Y + DIV2_Y, x + PAD, x + PAD + CONTENT_W - 1, LINE)

    # 状态行
    s1 = CARD_Y + ST1_Y
    c.text(x + PAD, s1 + 1, "AFC", GREEN if d["afc"] else GREY)
    c.text(x + 80, s1 + 1, "DIV", GREEN if d["div"] else GREY)

    s2 = CARD_Y + ST2_Y
    draw_icon(c, "batt", x + PAD, s2 + 4, 16, 9, d["batt_color"], level=d["batt"])
    c.text(x + 24, s2 + 1, d["batt_txt"], d["batt_color"])
    draw_icon(c, "spk", x + 80, s2 + 3, 11, 10, d["mute_color"], slashed=d["muted"])
    c.text(x + 96, s2 + 1, "MUTE" if d["muted"] else "UNMUTE", d["mute_color"])


def build():
    c = Canvas()

    # 状态栏
    c.rect(0, 0, W, STATUS_H, BAR)
    c.hline(STATUS_H - 1, 0, W - 1, LINE)
    draw_icon(c, "ant", 8, 8, 12, 12, DIM)
    c.text(24, 8, "UHF RX 470-960MHz", DIM)
    draw_icon(c, "lock", 152, 9, 10, 11, GREEN)
    c.text(165, 8, "PLL LOCKED", GREEN)
    draw_icon(c, "batt", 238, 10, 16, 9, GREEN, level=85)
    c.text(257, 8, "3.7V", TEXT)
    draw_icon(c, "gear", 294, 8, 13, 13, DIM)

    ch_a = dict(
        selected=True, freq="520.000",
        link=True, pilot=True, link_color=GREEN, pilot_color=GREEN,
        pilot_txt="PILOT",
        rssi_txt="-62dBm", rssi_color=GREEN, rssi_pct=rssi_pct(-62),
        aud_txt="-12dB", aud_color=GREEN, aud_pct=80,
        afc=True, div=True, batt=85, batt_txt="85%", batt_color=GREEN,
        muted=False, mute_color=GREEN, phase=7,
    )
    ch_b = dict(
        selected=False, freq="568.125",
        link=True, pilot=False, link_color=YELLOW, pilot_color=GREY,
        pilot_txt="NO PILOT",
        rssi_txt="-85dBm", rssi_color=ORANGE, rssi_pct=rssi_pct(-85),
        aud_txt="--", aud_color=GREY, aud_pct=0,
        afc=True, div=True, batt=42, batt_txt="42%", batt_color=YELLOW,
        muted=True, mute_color=RED, phase=11,
    )
    draw_card(c, CARD_AX, "CH-A", ch_a)
    draw_card(c, CARD_BX, "CH-B", ch_b)

    # 提示栏
    c.hline(HINT_Y, 0, W - 1, LINE)
    draw_icon(c, "tri_l", 6, 226, 6, 8, DIM)
    draw_icon(c, "tri_r", 13, 226, 6, 8, DIM)
    c.text(22, HINT_Y + 7, "ENCODER", DIM)
    c.text(160, HINT_Y + 7, "PRESS MENU", DIM, align="center")
    c.text(314, HINT_Y + 7, "HOLD PAIR", DIM, align="right")

    out = c.img.resize((W, H), Image.LANCZOS)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui_rx_preview.png")
    out.save(path)
    print("saved:", path)
    return path


if __name__ == "__main__":
    build()
