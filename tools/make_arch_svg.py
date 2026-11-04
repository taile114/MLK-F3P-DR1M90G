#!/usr/bin/env python3
"""生成《方案 B 系统架构图》SVG（保证文字不出框、不叠压、标题与正文有足够行距）。

用法:  nix-shell -p python3Packages.pillow noto-fonts-cjk-sans fontconfig \
         --run "python3 tools/make_arch_svg.py"

产出:  docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg
思路:  用与 SVG 相同的字体(Noto Sans CJK SC)实测每行文字宽度 → 定框宽；
       标题基线之后固定留 TITLE_GAP 再排正文；横向/纵向都做硬校验（越界即报错）。
"""
from __future__ import annotations

import glob
import os
import subprocess

from PIL import ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "MLK-F3P-DR1M90G-方案B-系统架构图.svg")


# ---------------------------------------------------------------- 字体
def _font_file() -> str:
    for pat in (
        "/nix/store/*/share/fonts/opentype/noto-cjk/NotoSansCJK-*.ttc",
        "/usr/share/fonts/**/NotoSansCJK*.ttc",
    ):
        h = sorted(glob.glob(pat, recursive=True))
        if h:
            return h[-1]
    o = subprocess.run(["fc-match", "-f", "%{file}", "Noto Sans CJK SC"],
                       capture_output=True, text=True).stdout.strip()
    if o and os.path.exists(o):
        return o
    raise SystemExit("缺 CJK 字体：nix-shell -p noto-fonts-cjk-sans")


FONT_FILE = _font_file()
_cache: dict = {}


def face(size: int, bold: bool = False):
    key = (size, bold)
    if key not in _cache:
        f = ImageFont.truetype(FONT_FILE, size)
        if bold:
            try:
                f.set_variation_by_name("Bold")
            except Exception:
                pass
        _cache[key] = f
    return _cache[key]


def tw(text: str, size: int, bold: bool = False) -> float:
    w = face(size, False).getlength(text)
    return w * (1.02 if bold else 1.0)


# ---------------------------------------------------------------- 画布 / 主题
W, H = 2000, 1170
PAD = 26            # 框内左右留白
PAD_T = 18          # 框内上留白
SAFE = 20           # 额外安全边距（防不同渲染器字体度量差异）
TITLE_GAP = 44      # 标题基线 → 正文首行顶部 的距离（关键：标题/正文不打架）
LH = 1.55           # 正文行距系数

S_TITLE, S_SUB = 34, 24
S_BOX, S_RUN, S_TXT, S_SM = 30, 22, 21, 20

NAVY, TEAL, RED, GREEN, ORANGE, GRAY, INK = (
    "#12325B", "#1F7A8C", "#C0392B", "#2E7D32", "#B8791A", "#5A6472", "#1B2430")
T_BLUE = ("#EEF3FB", "#7C97C0")
T_GREEN = ("#EDF7F1", "#6FAE86")
T_ORANGE = ("#FDF6EA", "#CBA164")
T_RED = ("#FCEFEF", "#C97A72")

E: list[str] = []


class Bad(Exception):
    pass


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def box(x, y, w, h, theme, stroke_w=2.5, rx=16):
    fill, st = theme
    E.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
             f'stroke="{st}" stroke-width="{stroke_w}"/>')


def text(x, y, s, size=S_TXT, color=INK, bold=False, anchor="start"):
    E.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" '
             f'font-weight="{"bold" if bold else "normal"}" text-anchor="{anchor}">{esc(s)}</text>')


def box_title(bx, by, bw, s, size=S_BOX, color=NAVY, gap=TITLE_GAP) -> float:
    """居中标题；返回正文起始 y（=标题基线 + gap）。"""
    w = tw(s, size, True)
    if w > bw - 2 * PAD - SAFE:
        raise Bad(f"标题出框: {s!r} {w:.0f}")
    base = by + PAD_T + size
    E.append(f'<text x="{bx + bw / 2}" y="{base}" font-size="{size}" fill="{color}" '
             f'font-weight="bold" text-anchor="middle">{esc(s)}</text>')
    return base + gap


def bullet(bx, y0, bw, s, line, size=S_TXT, color=INK, x_off=PAD, prefix="• ") -> float:
    """y0 = 正文区顶部；第 line 行。返回该行基线 y。"""
    y = y0 + size + line * size * LH
    t = prefix + s
    if bx + x_off + tw(t, size) > bx + bw - PAD - SAFE:
        raise Bad(f"正文出框: {t!r}")
    E.append(f'<text x="{bx + x_off}" y="{y}" font-size="{size}" fill="{color}">{esc(t)}</text>')
    return y


def cline(bx, y0, bw, s, line=0, size=S_TXT, color=INK) -> float:
    y = y0 + size + line * size * LH
    if tw(s, size) > bw - 2 * PAD - SAFE:
        raise Bad(f"居中行出框: {s!r}")
    E.append(f'<text x="{bx + bw / 2}" y="{y}" font-size="{size}" fill="{color}" '
             f'text-anchor="middle">{esc(s)}</text>')
    return y


MARK = {GRAY: "ah-g", TEAL: "ah-t", GREEN: "ah-n", NAVY: "ah-b", RED: "ah-r"}


def arrow(pts, color=GRAY, w=3, dash=False, both=False):
    d = "M " + " L ".join(f"{x},{y}" for (x, y) in pts)
    extra = ' stroke-dasharray="9 7"' if dash else ""
    mk = MARK.get(color, "ah-g")
    E.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{w}" '
             f'stroke-linecap="round" stroke-linejoin="round"{extra} marker-end="url(#{mk})"/>')
    if both:
        E.append(f'<path d="M {pts[1][0]},{pts[1][1]} L {pts[0][0]},{pts[0][1]}" fill="none" '
                 f'stroke="{color}" stroke-width="{w}" marker-end="url(#{mk})" stroke-linecap="round"/>')


def alabel(x, y, s, color=GRAY, size=S_SM, anchor="middle"):
    w = tw(s, size)
    x0 = x - w / 2 - 8 if anchor == "middle" else x - 8
    E.append(f'<rect x="{x0}" y="{y - size - 3}" width="{w + 16}" height="{size + 10}" '
             f'fill="#FFFFFF" opacity="0.94"/>')
    E.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}">'
             f'{esc(s)}</text>')


def fit(box_rect, last_y, gap=14, name=""):
    if last_y + gap > box_rect[1] + box_rect[3]:
        raise Bad(f"{name} 底部余量不足: last={last_y:.0f} bottom={box_rect[1] + box_rect[3]}")


# ---------------------------------------------------------------- 布局
B1 = (100, 196, 500, 138)      # 电脑 / USB 5V
B2 = (100, 412, 500, 310)      # 传感器载板
B3 = (900, 320, 660, 470)      # 核心板
B4 = (1640, 262, 330, 222)     # 编码器
B5 = (1640, 566, 330, 190)     # 显示器
B6 = (790, 880, 780, 212)      # 传感器小模块
B7 = (100, 880, 610, 246)      # 电机整套

E.append(f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>')
text(70, 74, "方案 B 系统架构：核心板 + 传感器载板 + 市售电机整套", S_TITLE, NAVY, True)
text(70, 122, "传感器载板与核心板经 P1.2 单点共地；电机整套独立运行，不与核心板共地、不通信", S_SUB, GRAY)

# ---- 连线
arrow([(350, 334), (350, 412)])                                          # 5V → 传感器板
arrow([(600, 560), (900, 560)], color=NAVY, w=3.5, both=True)             # P1 ⇄ J12
arrow([(1560, 396), (1640, 386)], color=TEAL)                             # → 编码器
arrow([(1560, 646), (1640, 646)], color=TEAL)                             # → 显示器
arrow([(350, 722), (350, 828), (900, 828), (900, 880)], color=GREEN)      # J2 → 小模块
arrow([(1450, 790), (1450, 880)], color=NAVY)                             # P1 IO → 小模块
arrow([(160, 880), (160, 724)], color=RED, w=2.6, dash=True)              # 霍尔（选用）

# ---- B1
box(*B1, T_BLUE)
y0 = box_title(B1[0], B1[1], B1[2], "电脑 / USB 5V", size=28)
fit(B1, cline(B1[0], y0, B1[2], "传感器载板独立供电（不挂大负载）", 0, S_SM, GRAY), name="B1")
alabel(350, 376, "USB-C 5V", GRAY)

# ---- B2
box(*B2, T_GREEN, stroke_w=3.5)
y0 = box_title(B2[0], B2[1], B2[2], "传感器载板 MLK-F3P-DR1M90G", size=29)
last = 0
for i, s in enumerate([
    "J1 USB-C → F1(PTC) → +5V",
    "J2 (2×8)：8×+5V / 8×GND",
    "H1：+3V3 抽头（核心板 J12.1 → P1.1）",
    "U1 / U2：霍尔电流端（选用）",
    "P1 (2×13) ⇄ 核心板 J12",
]):
    last = bullet(B2[0], y0, B2[2], s, i, S_SM)
foot = B2[1] + B2[3] - 22
if foot - last < 18:
    raise Bad(f"B2 脚注与正文太近: {foot - last:.0f}")
text(B2[0] + PAD, foot, "全板单域共地（经 P1.2）", S_SM, GREEN, True)

# ---- B3 核心板
box(*B3, T_BLUE, stroke_w=3.5)
y0 = box_title(B3[0], B3[1], B3[2], "核心板 MLK-F3P-DR1M90G", size=28, gap=52)
ry = y0 + S_RUN
for a, b in [
    ("J12  PL-GPIO（BANK13，3V3）", "1=VCC_CEP1   2=GND   3–26=CEP1_1P…12N"),
    ("HDMI OUT（JH1）", "→ 本地显示器"),
    ("CAN0TX / CAN0RX（3V3）", "⇄ 编码器（24V 供电取电机侧）"),
    ("电机驱动 / 调速", "不经过本板（市售整套自带）"),
]:
    if tw(a, S_RUN, True) > B3[2] - 2 * PAD - SAFE:
        raise Bad(f"核心板行标题出框: {a!r}")
    text(B3[0] + PAD, ry, a, S_RUN, NAVY, True)
    if tw(b, S_SM) > B3[2] - 2 * PAD - SAFE:
        raise Bad(f"核心板行内容出框: {b!r}")
    text(B3[0] + PAD, ry + 32, b, S_SM, GRAY)
    ry += 92
fit(B3, ry - 92 + 32, name="B3")

# ---- B4 编码器
box(*B4, T_ORANGE)
y0 = box_title(B4[0], B4[1], B4[2], "编码器", size=28)
last = 0
for i, s in enumerate(["测速 / 角度 / 方向", "CAN 直连核心板 J11", "24V 供电取电机侧"]):
    last = bullet(B4[0], y0, B4[2], s, i, S_SM, x_off=34)
fit(B4, last, name="B4")

# ---- B5 显示器
box(*B5, T_BLUE)
y0 = box_title(B5[0], B5[1], B5[2], "本地显示器", size=28)
last = 0
for i, s in enumerate(["HDMI 输出（JH1）", "用电脑屏幕代替"]):
    last = bullet(B5[0], y0, B5[2], s, i, S_SM, x_off=62)
fit(B5, last, name="B5")

# ---- B6 传感器小模块
box(*B6, T_ORANGE, stroke_w=3.5)
y0 = box_title(B6[0], B6[1], B6[2], "传感器小模块（市售）", size=29)
last = 0
for i, s in enumerate(["温度传感器 · 振动传感器 · 电流传感器（霍尔） · 声光报警",
                       "供电：J2 的 +5V / GND；信号：跳线到 P1 的 PL IO"]):
    last = bullet(B6[0], y0, B6[2], s, i, S_SM, x_off=64)
fit(B6, last, name="B6")

# ---- B7 电机整套
box(*B7, T_RED, stroke_w=3.5)
y0 = box_title(B7[0], B7[1], B7[2], "被测电机整套（市售，独立运行）", size=27, color=RED)
last = 0
for i, s in enumerate(["24V 电源 + 调速器 + 755 有刷电机",
                       "不画板 · 自带调速 · 不共地、不通信",
                       "测电流可选：霍尔模块串联 U1 / U2"]):
    last = bullet(B7[0], y0, B7[2], s, i, S_SM, x_off=40)
warn = "⚠ 额定 5.2A / 堵转 36A：必须粗线（≥18AWG）+ 螺钉端子"
if tw(warn, S_SM, True) > B7[2] - 2 * PAD - SAFE:
    raise Bad("电机整套警示行出框")
fit(B7, last, name="B7")
text(B7[0] + PAD, B7[1] + B7[3] - 20, warn, S_SM, RED, True)

# ---- 连线标签
alabel(750, 538, "P1 ⇄ J12（26P）", NAVY)
alabel(750, 566, "3V3 / GND / 24×PL IO", GRAY)
alabel(1604, 394, "CAN", TEAL)
alabel(1606, 638, "HDMI", TEAL)
alabel(636, 790, "J2: 8×+5V / 8×GND", GREEN, anchor="start")
alabel(1450, 818, "P1 的 PL IO", NAVY)
alabel(262, 812, "霍尔电流（选用）", RED)

# ---------------------------------------------------------------- 输出
head = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <defs>
    <marker id="ah-g" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" markerHeight="6"
            orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{GRAY}"/></marker>
    <marker id="ah-t" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" markerHeight="6"
            orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{TEAL}"/></marker>
    <marker id="ah-n" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" markerHeight="6"
            orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{GREEN}"/></marker>
    <marker id="ah-b" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" markerHeight="6"
            orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{NAVY}"/></marker>
    <marker id="ah-r" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" markerHeight="6"
            orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{RED}"/></marker>
    <style>
      text {{ font-family:"Noto Sans CJK SC","Source Han Sans SC","Microsoft YaHei",
              "WenQuanYi Zen Hei",sans-serif; }}
    </style>
  </defs>
'''
svg = head + "\n".join("  " + e for e in E) + "\n</svg>\n"
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as fh:
    fh.write(svg)
print(f"saved -> {OUT}  ({len(svg)} bytes)")
