#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ui_port_check.py —— SGL 页面"模拟器 -> 板卡"搬运校验

解决的问题
----------
在 PC 模拟器 (sgl-port-windows-vscode) 里调好的界面, 拷到 AT32F403A 工程后
经常出现"模拟器好好的, 上板不显示 / 只显示一半 / 链接失败". 原因固定就那么几条,
这个脚本全部覆盖:

  1. 两边的 [A] 段代码漂移     -> 逐行 diff, 指出行号
  2. 用了板卡没编译的控件      -> filebrowser/gif/avi/img_ext/label_ext ...
  3. 用了板卡没有的字体        -> 只允许 consolas14/23/24/32 + song23
  4. 控件摆出画布             -> 解析 #define 后算真实坐标 (画布方向取自 port 层)
  5. set_text_fmt 打到没绑缓冲的 label -> 运行时会写野指针 / 显示空串
  6. 板卡文件里残留 SDL/main/while  -> 平台代码没剪干净

用法
----
    python tools/ui_port_check.py
    python tools/ui_port_check.py --sim <模拟器>/demo/main.c --board <工程>/app/Src/ui_demo.c

退出码: 0 = 全部通过, 1 = 有 ERROR (WARN 不影响退出码)
"""

import argparse
import ast
import io
import os
import re
import sys

# ---------------------------------------------------------------- 常量

CANVAS_W = 240          # 运行时由 BSP/UI_Driver/sgl_port_at32.h 的 SGL_PORT_LANDSCAPE 决定
CANVAS_H = 320          # 竖屏 240x320 / 横屏 320x240

ALLOWED_FONTS = {
    "consolas14", "consolas23", "consolas24", "consolas32", "song23",
}

# 板卡 Keil 工程刻意跳过的控件目录 (依赖文件系统 / 解码器)
BANNED_WIDGETS = [
    "filebrowser", "filedialog", "gif", "gifdec", "img_ext",
    "label_ext", "avi", "unzip_image", "vortex",
]

# 布局小工具的签名: (parent, x, y, w, h, ...)
LAYOUT_HELPERS = ("ui_label", "ui_label_dyn", "ui_button")

SECTION_START = "#define UI_W"
SECTION_END = "[A] 段结束"

C_RED = "\033[31m" if os.name != "nt" else ""
C_YEL = "\033[33m" if os.name != "nt" else ""
C_GRN = "\033[32m" if os.name != "nt" else ""
C_DIM = "\033[2m" if os.name != "nt" else ""
C_OFF = "\033[0m" if os.name != "nt" else ""


class Report:
    def __init__(self):
        self.err = 0
        self.warn = 0

    def error(self, where, msg):
        self.err += 1
        print("%s[ERROR]%s %s%s%s\n        %s" % (C_RED, C_OFF, C_YEL, where, C_OFF, msg))

    def warning(self, where, msg):
        self.warn += 1
        print("%s[WARN ]%s %s%s%s\n        %s" % (C_YEL, C_OFF, C_DIM, where, C_OFF, msg))

    def ok(self, msg):
        print("%s[ OK  ]%s %s" % (C_GRN, C_OFF, msg))


# ---------------------------------------------------------------- 工具

def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def split_top_level(s):
    """按顶层逗号切分函数实参, 忽略括号内的逗号."""
    out, cur, depth = [], [], 0
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return [x.strip() for x in out]


def collect_defines(text):
    """收集 #define 数值宏, 支持跨行续行符."""
    text = text.replace("\\\n", " ")
    defs = {}
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n/]+)",
                         text, re.M):
        defs[m.group(1)] = m.group(2).strip()
    return defs


def resolve(expr, defs, depth=0):
    """把表达式里的宏替换成数字后求值; 失败返回 None."""
    if depth > 10 or expr is None:
        return None
    expr = expr.strip()
    # 逐个剥掉最外层括号
    while expr.startswith("(") and expr.endswith(")") and \
            expr.count("(") == expr.count(")") and _balanced(expr):
        expr = expr[1:-1].strip()
    # 标识符 -> 数值
    def sub(m):
        name = m.group(0)
        if name in defs:
            v = resolve(defs[name], defs, depth + 1)
            if v is None:
                raise ValueError(name)
            return "(%d)" % v
        raise ValueError(name)
    try:
        expr = re.sub(r"\b[A-Za-z_]\w*\b", sub, expr)
    except ValueError:
        return None
    expr = expr.replace("/", "//")
    try:
        node = ast.parse(expr, mode="eval")
    except SyntaxError:
        return None
    for n in ast.walk(node):
        if not isinstance(n, (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant,
                              ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Mod,
                              ast.USub, ast.UAdd, ast.Load)):
            return None
    try:
        return int(eval(compile(node, "<expr>", "eval"), {"__builtins__": {}}))
    except Exception:
        return None


def _balanced(expr):
    d = 0
    for i, ch in enumerate(expr):
        if ch == "(":
            d += 1
        elif ch == ")":
            d -= 1
            if d == 0 and i != len(expr) - 1:
                return False
    return d == 0


def sanitize(text):
    """把注释和字符串字面量替换成空格, 长度与换行位置保持不变.

    目的: 让所有"代码模式"检查不受注释里的示例代码干扰 —— 比如文件头注释
    里写了 "SDL_GetTicks() 替代 ...", 不能让脚本误判成板卡残留 SDL 调用.
    因为只替换字符不删行, 行号映射依然准确.
    """
    out = list(text)
    i, n = 0, len(text)
    state = None          # None / "line" / "block" / "str" / "chr"
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if ch == "/" and nxt == "/":
                state = "line"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if ch == '"':
                state = "str"
                out[i] = " "
            elif ch == "'":
                state = "chr"
                out[i] = " "
        else:
            if ch == "\n":
                # 行注释在换行处结束; 块注释/字符串里的换行保持状态
                if state == "line":
                    state = None
                out[i] = "\n"
                i += 1
                continue
            if state == "line":
                out[i] = " "
            elif state == "block":
                if ch == "*" and nxt == "/":
                    out[i] = out[i + 1] = " "
                    state = None
                    i += 2
                    continue
                out[i] = " "
            elif state == "str":
                if ch == "\\" and nxt:
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                if ch == '"':
                    state = None
                out[i] = " "
            elif state == "chr":
                if ch == "\\" and nxt:
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                if ch == "'":
                    state = None
                out[i] = " "
        i += 1
    return "".join(out)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


# ---------------------------------------------------------------- [A] 段比对

def extract_section(text):
    """返回 (start_line_no, [lines]) 形式的 [A] 段; 找不到返回 None."""
    end = text.find(SECTION_END)
    if end < 0:
        return None
    head = text[:end]
    start = head.rfind(SECTION_START)
    if start < 0:
        return None
    start = head.rfind("\n", 0, start) + 1
    start_line = line_of(text, start)
    body = text[start:end]
    lines = [ln.rstrip() for ln in body.splitlines()]
    return start_line, lines


def norm(lines):
    """去掉行尾空白与末尾空行, 空行压缩不影响 diff 判定."""
    out = [ln for ln in lines if ln.strip() != ""]
    return out


def diff_sections(sim_text, board_text, rep):
    a = extract_section(sim_text)
    b = extract_section(board_text)

    if a is None:
        rep.error("sim", "找不到 [A] 段 (缺少 '#define UI_W' 或 '[A] 段结束' 标记)")
        return
    if b is None:
        rep.error("board", "找不到 [A] 段 (缺少 '#define UI_W' 或 '[A] 段结束' 标记)")
        return

    la, lb = norm(a[1]), norm(b[1])
    if la == lb:
        rep.ok("[A] 段逐字一致 (%d 行有效代码)" % len(la))
        return

    # 逐行对比, 给出前 20 处差异
    import difflib
    sm = difflib.SequenceMatcher(None, la, lb)
    shown = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        if shown >= 20:
            print("        ... 差异过多, 只显示前 20 处")
            break
        if tag == "replace":
            for k in range(max(i2 - i1, j2 - j1)):
                s = la[i1 + k] if i1 + k < i2 else "<缺失>"
                d = lb[j1 + k] if j1 + k < j2 else "<缺失>"
                rep.error("sim:%d / board:%d" % (a[0] + i1 + k, b[0] + j1 + k),
                          "模拟器: %s\n        板卡  : %s" % (s.strip(), d.strip()))
                shown += 1
                if shown >= 20:
                    break
        elif tag == "delete":
            for k in range(i1, i2):
                rep.error("sim:%d" % (a[0] + k),
                          "模拟器有但板卡没有: %s" % la[k].strip())
                shown += 1
                if shown >= 20:
                    break
        else:  # insert
            for k in range(j1, j2):
                rep.error("board:%d" % (b[0] + k),
                          "板卡有但模拟器没有: %s" % lb[k].strip())
                shown += 1
                if shown >= 20:
                    break


# ---------------------------------------------------------------- 静态规则

def check_banned_widgets(text, rep, fname):
    """只查 <控件>_create 形式.

    以前是 \\bsgl_\\w*<name>\\w*\\s*\\( , 但 "avi" 会误伤 sgl_key_n**avi**gate()
    这类核心 API. 用控件只可能是先 create, 所以把匹配收窄到 *_create 结尾,
    既不漏真问题, 也不会把 navigate / event / obj 这些核心函数算进来.
    """
    hit = False
    for name in BANNED_WIDGETS:
        pat = r"\bsgl_\w*%s\w*_create\s*\(" % re.escape(name)
        for m in re.finditer(pat, text):
            rep.error("%s:%d" % (fname, line_of(text, m.start())),
                      "用了板卡未编译的控件 '%s' (Keil 工程跳过了该目录), "
                      "链接会报 undefined symbol" % m.group(0).strip())
            hit = True
    if not hit:
        rep.ok("控件: 没有使用板卡未编译的控件")


def check_fonts(text, rep, fname):
    hit = False
    seen = set()
    for m in re.finditer(r"\b(consolas\d+(?:_compress)?|song\d+|kai\d+|"
                         r"sgl_icon_\w+)\b", text):
        name = m.group(1)
        if name.startswith("CONFIG_"):
            continue
        if name in ALLOWED_FONTS or name in seen:
            continue
        seen.add(name)
        rep.error("%s:%d" % (fname, line_of(text, m.start())),
                  "字体 '%s' 不在板卡白名单 %s 内"
                  % (name, sorted(ALLOWED_FONTS)))
        hit = True
    if not hit:
        rep.ok("字体: 全部在板卡白名单内")


def check_canvas(text, rep, fname):
    defs = collect_defines(text)
    boxes = {}          # var -> [x, y, w, h]
    problems = 0
    checked = 0

    # 1) 布局小工具: ui_label(parent, x, y, w, h, ...)
    for m in re.finditer(r"\b(%s)\s*\(" % "|".join(LAYOUT_HELPERS), text):
        # 找到匹配的右括号
        i = m.end() - 1
        depth = 0
        for j in range(i, len(text)):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    args = split_top_level(text[i + 1:j])
                    if len(args) >= 5:
                        vals = [resolve(a, defs) for a in args[1:5]]
                        if all(v is not None for v in vals):
                            x, y, w, h = vals
                            checked += 1
                            if x < 0 or y < 0 or x + w > CANVAS_W or y + h > CANVAS_H:
                                ln = line_of(text, m.start())
                                rep.error("%s:%d" % (fname, ln),
                                          "%s 越界: x=%d..%d (宽 %d), y=%d..%d (高 %d); "
                                          "画布只有 %dx%d"
                                          % (m.group(1), x, x + w - 1, w,
                                             y, y + h - 1, h, CANVAS_W, CANVAS_H))
                                problems += 1
                    break
        continue

    # 2) set_pos / set_size 分开写的控件
    for m in re.finditer(r"\bsgl_obj_set_pos\s*\(\s*([A-Za-z_]\w*)\s*,\s*([^,]+),\s*([^)]+)\)", text):
        var = m.group(1)
        x, y = resolve(m.group(2), defs), resolve(m.group(3), defs)
        if x is not None and y is not None:
            boxes.setdefault(var, [None] * 4)[0:2] = [x, y]

    for m in re.finditer(r"\bsgl_obj_set_size\s*\(\s*([A-Za-z_]\w*)\s*,\s*([^,]+),\s*([^)]+)\)", text):
        var = m.group(1)
        w, h = resolve(m.group(2), defs), resolve(m.group(3), defs)
        if w is not None and h is not None:
            boxes.setdefault(var, [None] * 4)[2:4] = [w, h]

    for var, (x, y, w, h) in boxes.items():
        if None in (x, y, w, h):
            continue
        checked += 1
        if x < 0 or y < 0 or x + w > CANVAS_W or y + h > CANVAS_H:
            problems += 1
            rep.error(fname, "控件 %s 越界: x=%d..%d, y=%d..%d; 画布只有 %dx%d"
                      % (var, x, x + w - 1, y, y + h - 1, CANVAS_W, CANVAS_H))

    if problems == 0:
        rep.ok("画布: %d 个控件全部落在 %dx%d 内" % (checked, CANVAS_W, CANVAS_H))
    elif checked == 0:
        rep.warning(fname, "画布: 一个坐标都没解析出来, 检查是否用了非常量表达式")


def check_text_fmt(text, rep, fname):
    """检查 sgl_label_set_text_fmt 的目标 label 是否绑定过文本缓冲.

    判据: 文件名作用域的 static 对象 + 由 ui_label_dyn() 创建 (内部会绑缓冲)
    或显式调用过 sgl_label_set_text_buffer 的, 才算"有缓冲".
    只认文件作用域的 static 变量, 避免把 ui_label_dyn() 函数体里的局部变量
    obj 也算进来 (那会造成假阴性).
    """
    static_objs = set(re.findall(r"static\s+sgl_obj_t\s*\*\s*([A-Za-z_]\w*)", text))

    from_dyn = set(re.findall(r"([A-Za-z_]\w*)\s*=\s*ui_label_dyn\s*\(", text))
    from_buf = set(re.findall(r"sgl_label_set_text_buffer\s*\(\s*([A-Za-z_]\w*)", text))

    dyn = static_objs & (from_dyn | from_buf)

    bad = 0
    for m in re.finditer(r"sgl_label_set_text_fmt\s*\(\s*([A-Za-z_]\w*)", text):
        var = m.group(1)
        if var not in dyn:
            rep.error("%s:%d" % (fname, line_of(text, m.start())),
                      "%s 没有绑定文本缓冲就调 set_text_fmt. 必须先 "
                      "sgl_label_set_text_buffer(%s, buf, sizeof buf), "
                      "否则 text_capacity=0, 写进去是空串" % (var, var))
            bad += 1
    if bad == 0:
        rep.ok("动态文本: %d 个 label 都已绑定缓冲 (%s)"
               % (len(dyn), ", ".join(sorted(dyn)) if dyn else "无"))


def check_platform_leak(text, rep, fname):
    bad = 0
    for pat, why in (
        (r"\bSDL_\w+", "残留 SDL 调用"),
        (r"#include\s*<SDL\.h>", "残留 SDL 头文件"),
        (r"\bint\s+main\s*\(", "板卡文件里不该有 main()"),
        (r"while\s*\(\s*1\s*\)", "板卡文件里的 while(1) 会独占调度器"),
    ):
        for m in re.finditer(pat, text):
            rep.error("%s:%d" % (fname, line_of(text, m.start())), why)
            bad += 1
    if bad == 0:
        rep.ok("平台代码: 没有 SDL/main/while(1) 残留")


def check_config_consistency(sim_main, board_file, rep):
    """两边的画布尺寸宏必须都等于板卡当前方向的分辨率."""
    for path, tag in ((sim_main, "sim"), (board_file, "board")):
        text = read(path)
        defs = collect_defines(text)
        w = resolve("UI_W", defs)
        h = resolve("UI_H", defs)
        if w is None or h is None:
            rep.warning(tag, "没找到 UI_W / UI_H 宏, 跳过画布尺寸一致性检查")
        elif (w, h) != (CANVAS_W, CANVAS_H):
            rep.error(tag, "UI_W x UI_H = %dx%d, 但板卡画布是 %dx%d"
                      % (w, h, CANVAS_W, CANVAS_H))
        else:
            rep.ok("%s: UI_W x UI_H = %dx%d" % (tag, w, h))


# ---------------------------------------------------------------- main

def detect_canvas(proj):
    """从 port 层读当前方向, 返回 (宽, 高).

    方向由 BSP/UI_Driver/sgl_port_at32.h 的 SGL_PORT_LANDSCAPE 决定,
    和 sgl_port_init() 里传给 st7789 的 rotation 是同一个宏, 不会走偏.
    """
    path = os.path.join(proj, "BSP", "UI_Driver", "sgl_port_at32.h")
    try:
        text = read(path)
    except OSError:
        return 240, 320

    m = re.search(r"#define\s+SGL_PORT_LANDSCAPE\s+(\d+)", text)
    if m and m.group(1) != "0":
        return 320, 240
    return 240, 320


def main():
    global CANVAS_W, CANVAS_H

    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    CANVAS_W, CANVAS_H = detect_canvas(proj)

    ap = argparse.ArgumentParser(description="SGL 模拟器 -> 板卡 搬运校验")
    ap.add_argument("--sim", default=r"C:\Users\zed\Desktop\Github\sgl-port-windows-vscode\demo\main.c",
                    help="模拟器页面文件 (demo/main.c)")
    ap.add_argument("--board", default=os.path.join(proj, "app", "Src", "ui_demo.c"),
                    help="板卡页面文件 (app/Src/ui_demo.c)")
    args = ap.parse_args()

    for p in (args.sim, args.board):
        if not os.path.isfile(p):
            print("找不到文件: %s" % p)
            return 2

    print("sim  : %s" % args.sim)
    print("board: %s" % args.board)
    print("-" * 78)

    sim = read(args.sim)
    board = read(args.board)
    # 注释/字符串先抹成空格再做模式匹配 (行号不变), 避免注释里的示例代码误报
    board_code = sanitize(board)
    rep = Report()

    print("[1/6] 检查画布尺寸声明")
    check_config_consistency(args.sim, args.board, rep)

    print("\n[2/6] 比对 [A] 段 (页面代码) 是否漂移")
    diff_sections(sim, board, rep)

    print("\n[3/6] 检查控件可用性")
    check_banned_widgets(board_code, rep, "board")

    print("\n[4/6] 检查字体可用性")
    check_fonts(board_code, rep, "board")

    print("\n[5/6] 检查控件是否越出 %dx%d" % (CANVAS_W, CANVAS_H))
    check_canvas(board_code, rep, "board")

    print("\n[6/6] 检查动态文本缓冲 + 平台代码残留")
    check_text_fmt(board_code, rep, "board")
    check_platform_leak(board_code, rep, "board")

    print("\n" + "=" * 78)
    if rep.err == 0:
        print("%s全部通过%s (0 error, %d warning)" % (C_GRN, C_OFF, rep.warn))
        return 0
    print("%s发现 %d 个问题%s (另有 %d warning)" % (C_RED, rep.err, C_OFF, rep.warn))
    return 1


if __name__ == "__main__":
    sys.exit(main())
