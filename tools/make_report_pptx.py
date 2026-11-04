#!/usr/bin/env python3
"""生成《MLK-F3P-DR1M90G 外设方案报告》PPTX。

用法:  nix-shell -p python3Packages.python-pptx python3Packages.pillow noto-fonts-cjk-sans fontconfig \
         --run "python3 tools/make_report_pptx.py"
（flake.nix 的 py 环境已含 python-pptx / openpyxl / pillow，可直接 `nix develop` 后运行）

排版校验（可选）: 需 libreoffice + poppler-utils
  soffice --headless --convert-to pdf --outdir /tmp docs/MLK-F3P-DR1M90G-外设方案报告.pptx
  pdftoppm -png -r 55 /tmp/MLK-F3P-DR1M90G-外设方案报告.pdf /tmp/slide

产出:  docs/MLK-F3P-DR1M90G-外设方案报告.pptx   （16:9，14 页）
素材:  ~/Pictures/Screenshots/*.jpg（自动裁边） + PIL 现画的系统架构图
"""
from __future__ import annotations

import glob
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.expanduser("~/Pictures/Screenshots")
OUT_PPTX = os.path.join(ROOT, "docs", "MLK-F3P-DR1M90G-外设方案报告.pptx")
WORK = tempfile.mkdtemp(prefix="mlkf3p-ppt-")

# ---------------------------------------------------------------- 主题
NAVY = (0x12, 0x32, 0x5B)
TEAL = (0x1F, 0x7A, 0x8C)
RED = (0xC0, 0x39, 0x2B)
GREEN = (0x2E, 0x7D, 0x32)
GRAY = (0x5A, 0x64, 0x72)
LIGHT = (0xF2, 0xF5, 0xF9)
LINE = (0xD0, 0xD7, 0xE0)
CJK = "微软雅黑"


def _font_path() -> str:
    for pat in (
        "/nix/store/*/share/fonts/opentype/noto-cjk/NotoSansCJK-*.ttc",
        "/usr/share/fonts/**/NotoSansCJK*.ttc",
    ):
        hits = sorted(glob.glob(pat, recursive=True))
        if hits:
            return hits[-1]
    out = subprocess.run(
        ["fc-match", "-f", "%{file}", "Noto Sans CJK SC"],
        capture_output=True, text=True,
    ).stdout.strip()
    if out and os.path.exists(out):
        return out
    raise SystemExit("找不到 CJK 字体，请在 nix-shell 里加 noto-fonts-cjk-sans")


FONT_FILE = None


def pil_font(size: int):
    global FONT_FILE
    if FONT_FILE is None:
        FONT_FILE = _font_path()
    return ImageFont.truetype(FONT_FILE, size)


# ---------------------------------------------------------------- 图片裁边
def _trim(im: Image.Image, bg, thr=18, pad=8) -> Image.Image:
    diff = ImageChops.difference(im.convert("RGB"), Image.new("RGB", im.size, bg)).convert("L")
    bbox = diff.point(lambda p: 255 if p > thr else 0).getbbox()
    if not bbox:
        return im
    l, t, r, b = bbox
    return im.crop((max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad)))


JOBS = {
    "传感器板-电路原理图.jpg": ("light", "sch_sensor.jpg"),
    "传感器板-PCB-2D.jpg": ("light", "pcb2d_sensor.jpg"),
    "传感器板-PCB-3D.jpg": ("dark", "pcb3d_sensor.jpg"),
    "电机板-电路原理图.jpg": ("light", "sch_motor.jpg"),
    "电机板-PCB-2D.jpg": ("light", "pcb2d_motor.jpg"),
    "电机板-PCB-3D.jpg": ("dark", "pcb3d_motor.jpg"),
    "电机一整套外设.jpg": (None, "motor_set.jpg"),
    "编码器（测速度，CAN总线）.jpg": (None, "encoder.jpg"),
}


def prep_images() -> dict:
    out = {}
    for src, (mode, dst) in JOBS.items():
        p = os.path.join(SHOTS, src)
        if not os.path.exists(p):
            print(f"  ! 缺图 {src}", file=sys.stderr)
            continue
        im = Image.open(p)
        if mode == "light":
            im = _trim(im, (255, 255, 255))
        elif mode == "dark":
            im = _trim(im, (0, 0, 0), thr=25)
        im = im.convert("RGB")
        q = os.path.join(WORK, dst)
        im.save(q, quality=94)
        out[dst] = q
        print(f"  {dst:22s} {im.size}  ar={im.width/im.height:.2f}")
    return out


# ---------------------------------------------------------------- 架构图
W, H = 2600, 1460
DIAG_FG = (0x14, 0x22, 0x38)


def _rr(d, box, r, fill, outline, w=3):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=w)


def _center(d, box, lines, fns, fill=None, gap=6):
    fs = [pil_font(s) for s in fns]
    hs = []
    for (txt, f) in zip(lines, fs):
        b = d.textbbox((0, 0), txt, font=f)
        hs.append(b[3] - b[1])
    total = sum(hs) + gap * (len(lines) - 1)
    y = box[1] + (box[3] - box[1] - total) / 2
    for txt, f, fh in zip(lines, fs, hs):
        b = d.textbbox((0, 0), txt, font=f)
        x = box[0] + (box[2] - box[0] - (b[2] - b[0])) / 2
        d.text((x, y - b[1]), txt, font=f, fill=fill or DIAG_FG)
        y += fh + gap


def _arrow(d, p0, p1, color=DIAG_FG, w=4, head=16, dash=False):
    x0, y0 = p0
    x1, y1 = p1
    if dash:
        import math
        n = int(math.hypot(x1 - x0, y1 - y0) / 26) or 1
        for i in range(n):
            ax = x0 + (x1 - x0) * i / n
            ay = y0 + (y1 - y0) * i / n
            bx = x0 + (x1 - x0) * (i + 0.5) / n
            by = y0 + (y1 - y0) * (i + 0.5) / n
            d.line([(ax, ay), (bx, by)], fill=color, width=w)
    else:
        d.line([p0, p1], fill=color, width=w)
    import math
    a = math.atan2(y1 - y0, x1 - x0)
    pts = [
        (x1, y1),
        (x1 - head * math.cos(a - 0.4), y1 - head * math.sin(a - 0.4)),
        (x1 - head * math.cos(a + 0.4), y1 - head * math.sin(a + 0.4)),
    ]
    d.polygon(pts, fill=color)


def make_diagram(path: str):
    im = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(im)
    f_t = pil_font(40)
    f_s = pil_font(30)
    f_x = pil_font(26)
    f_note = pil_font(28)

    d.text((60, 40), "方案 B 系统架构：核心板 + 传感器载板 + 市售电机整套", font=f_t, fill=NAVY)
    d.text((60, 100), "传感器载板与核心板共地（经 P1.2）；电机整套独立运行，不与核心板共地、不通信", font=f_note, fill=GRAY)

    # ---- PC 5V
    box_pc = (90, 210, 610, 320)
    _rr(d, box_pc, 18, (0xF4, 0xF8, 0xFF), (0x6E, 0x8A, 0xB0))
    _center(d, box_pc, ["电脑 / USB 5V", "（传感器板独立供电）"], [32, 26])

    # ---- 传感器载板
    box_sb = (90, 420, 610, 900)
    _rr(d, box_sb, 18, (0xEF, 0xF7, 0xF3), GREEN, 4)
    _center(d, (box_sb[0], box_sb[1] + 20, box_sb[2], box_sb[1] + 90),
            ["传感器载板 MLK-F3P-DR1M90G"], [34])
    inner = [
        "J1 USB-C → F1 PTC → +5V（独立供电）",
        "J2 2×8：8×+5V / 8×GND → 传感器小模块",
        "H1：+3V3 抽头（取自核心板 J12.1）",
        "U1 / U2：霍尔电流模块电流端（选用）",
        "P1 2×13 ⇄ 核心板 J12（PL-GPIO）",
    ]
    y = box_sb[1] + 110
    for s in inner:
        d.text((box_sb[0] + 26, y), "• " + s, font=f_x, fill=DIAG_FG)
        y += 52
    d.text((box_sb[0] + 26, y + 6), "单域共地 → 经 P1.2 与核心板共地", font=f_x, fill=GREEN)

    # ---- 核心板
    box_cb = (1100, 380, 1780, 930)
    _rr(d, box_cb, 18, (0xEF, 0xF2, 0xFA), (0x6E, 0x8A, 0xB0), 4)
    _center(d, (box_cb[0], box_cb[1] + 20, box_cb[2], box_cb[1] + 90),
            ["核心板 MLK-F3P-DR1M90G"], [34])
    rows = [
        ("J12  PL-GPIO（BANK13，3V3）", "1=VCC_CEP1  2=GND  3–26=CEP1_1P…12N"),
        ("HDMI OUT（JH1）", "→ 本地显示器（电脑屏幕）"),
        ("CAN（U22 / J11）", "CAN0TX/RX  ⇄  编码器（24V 供电取电机侧）"),
        ("电机驱动 / 调速", "不经过本板 —— 市售整套自带"),
    ]
    y = box_cb[1] + 110
    for a, b in rows:
        d.text((box_cb[0] + 26, y), a, font=f_s, fill=NAVY)
        d.text((box_cb[0] + 26, y + 38), b, font=f_x, fill=GRAY)
        y += 96

    # ---- 传感器小模块
    box_sm = (1100, 1010, 1780, 1230)
    _rr(d, box_sm, 18, (0xFD, 0xF6, 0xEC), (0xC8, 0x9B, 0x5A))
    _center(d, box_sm, ["传感器小模块（市售）",
                        "温度 · 振动 · 电流（霍尔）· 声光报警",
                        "5V 由 J2 供电，信号接 P1 的 PL IO"], [32, 26, 26])

    # ---- 编码器
    box_en = (1880, 440, 2510, 660)
    _rr(d, box_en, 18, (0xFD, 0xF6, 0xEC), (0xC8, 0x9B, 0x5A))
    _center(d, box_en, ["编码器（测速度 / 角度 / 方向）",
                        "CAN 总线直连核心板 J11",
                        "24V 供电 + 接地取电机侧"], [30, 26, 26])

    # ---- 显示器
    box_hd = (1880, 720, 2510, 900)
    _rr(d, box_hd, 18, (0xEF, 0xF2, 0xFA), (0x6E, 0x8A, 0xB0))
    _center(d, box_hd, ["本地显示器（HDMI）", "电脑屏幕代替"], [30, 26])

    # ---- 电机整套（独立）
    box_ms = (90, 1010, 960, 1300)
    _rr(d, box_ms, 18, (0xFC, 0xF0, 0xF0), (0xC0, 0x39, 0x2B), 4)
    d.text((box_ms[0] + 26, box_ms[1] + 22), "被测电机整套（市售，独立运行）", font=pil_font(34), fill=RED)
    d.text((box_ms[0] + 26, box_ms[1] + 82), "24V 电源 + 调速器 + 755 有刷电机", font=f_x, fill=DIAG_FG)
    d.text((box_ms[0] + 26, box_ms[1] + 126), "不画板 · 自带调速 · 与核心板不共地、不通信", font=f_x, fill=DIAG_FG)
    d.text((box_ms[0] + 26, box_ms[1] + 170), "如需测电流：霍尔模块串联 U1/U2（选用）", font=f_x, fill=RED)
    d.text((box_ms[0] + 26, box_ms[1] + 214), "⚠ 额定 5.2A / 堵转 36A —— 粗线 + 螺钉端子", font=f_x, fill=RED)

    # ---- 连线
    _arrow(d, (350, 320), (350, 420))                      # 5V → 传感器板
    _arrow(d, (610, 560), (1100, 560), w=6)                # P1 → J12
    d.text((700, 520), "P1 ⇄ J12（26P：3V3 / GND / 24×PL IO）", font=f_x, fill=NAVY)
    _arrow(d, (610, 700), (1100, 1120), w=5, dash=False)   # J2 → 小模块
    d.text((640, 900), "J2 5V + P1 IO", font=f_x, fill=GRAY)
    _arrow(d, (1780, 550), (1880, 550), w=5)               # 核心板 → 编码器
    d.text((1775, 500), "CAN", font=f_x, fill=GRAY)
    _arrow(d, (1780, 810), (1880, 810), w=5)               # 核心板 → 显示器
    d.text((1775, 760), "HDMI", font=f_x, fill=GRAY)
    _arrow(d, (960, 1155), (1100, 1155), w=5, dash=True)   # 电机整套 → 小模块（可选霍尔）
    d.text((640, 1246), "U1/U2 可选串联霍尔模块测电机电流", font=f_x, fill=RED)

    im.save(path, quality=95)
    print("  diagram ->", path, im.size)
    return path


# ---------------------------------------------------------------- PPTX
def build(imgs: dict, diagram: str):
    from pptx import Presentation
    from pptx.dml.color import RGBColor
    from pptx.enum.shapes import MSO_SHAPE
    from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
    from pptx.util import Emu, Inches, Pt

    def rgb(t):
        return RGBColor(*t)

    prs = Presentation()
    prs.slide_width = Inches(13.333)
    prs.slide_height = Inches(7.5)
    SW, SH = 13.333, 7.5
    blank = prs.slide_layouts[6]

    def cjk(run, name=CJK):
        """python-pptx 只设 latin 字体，这里补 ea（东亚）字体。"""
        rPr = run._r.get_or_add_rPr()
        for tag in ("a:ea", "a:cs"):
            el = rPr.makeelement(
                "{http://schemas.openxmlformats.org/drawingml/2006/main}" + tag.split(":")[1],
                {"typeface": name},
            )
            rPr.append(el)

    def txbox(slide, x, y, w, h, align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP):
        tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
        tf = tb.text_frame
        tf.word_wrap = True
        tf.margin_left = tf.margin_right = Inches(0.04)
        tf.margin_top = tf.margin_bottom = Inches(0.02)
        tf.vertical_anchor = anchor
        tf.paragraphs[0].alignment = align
        return tf

    def para(tf, text, size, color, bold=False, first=False, space_before=6, space_after=2,
             align=None, name=CJK, bullet=None):
        p = tf.paragraphs[0] if first else tf.add_paragraph()
        if align is not None:
            p.alignment = align
        p.space_before = Pt(space_before)
        p.space_after = Pt(space_after)
        r = p.add_run()
        r.text = (("• " if bullet is True else (bullet + " " if isinstance(bullet, str) else "")) + text)
        r.font.size = Pt(size)
        r.font.bold = bold
        r.font.color.rgb = rgb(color)
        r.font.name = name
        cjk(r)
        return p

    def rect(slide, x, y, w, h, fill, line=None, shape=MSO_SHAPE.RECTANGLE, shadow=False,
             line_w=1.0):
        s = slide.shapes.add_shape(shape, Inches(x), Inches(y), Inches(w), Inches(h))
        if fill is None:
            s.fill.background()
        else:
            s.fill.solid()
            s.fill.fore_color.rgb = rgb(fill)
        if line is None:
            s.line.fill.background()
        else:
            s.line.color.rgb = rgb(line)
            s.line.width = Pt(line_w)
        s.shadow.inherit = shadow
        s.text_frame.word_wrap = True
        return s

    def header(slide, title, kicker=None, part=None):
        rect(slide, 0, 0, SW, 1.02, NAVY)
        rect(slide, 0, 1.02, SW, 0.055, TEAL)
        tf = txbox(slide, 0.52, 0.02, 10.4, 0.58, anchor=MSO_ANCHOR.MIDDLE)
        para(tf, title, 26, (255, 255, 255), bold=True, first=True, space_before=0)
        if kicker:
            tf2 = txbox(slide, 0.55, 0.62, 11.9, 0.36)
            para(tf2, kicker, 12, (0xC7, 0xD6, 0xE8), first=True, space_before=0)
        if part:
            tfp = txbox(slide, 11.1, 0.02, 1.85, 0.58, align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE)
            para(tfp, part, 13, (0x9F, 0xC4, 0xD8), first=True, space_before=0)

    def footer(slide, n):
        tf = txbox(slide, 0.52, 7.03, 8.0, 0.34)
        para(tf, "MLK-F3P-DR1M90G 外设方案报告 · 设备智能运维（故障预测与健康管理）· 2026-09-29",
             10.5, (0x8A, 0x93, 0xA0), first=True, space_before=0)
        tf2 = txbox(slide, 12.3, 7.03, 0.6, 0.34, align=PP_ALIGN.RIGHT)
        para(tf2, str(n), 11, (0x8A, 0x93, 0xA0), first=True, space_before=0)

    def pic(slide, path, x, y, w, h):
        from PIL import Image as _I
        iw, ih = _I.open(path).size
        box_ar = w / h
        ar = iw / ih
        if ar > box_ar:
            ww, hh = w, w / ar
        else:
            hh, ww = h, h * ar
        return slide.shapes.add_picture(
            path, Inches(x + (w - ww) / 2), Inches(y + (h - hh) / 2), Inches(ww), Inches(hh)
        )

    def table(slide, x, y, w, rows, col_w=None, fs=11.5, head_fs=12, row_h=0.34, head_h=0.4):
        r, c = len(rows), len(rows[0])
        shp = slide.shapes.add_table(r, c, Inches(x), Inches(y), Inches(w), Inches(head_h + row_h * (r - 1)))
        tb = shp.table
        tb.first_row = True
        if col_w:
            tot = sum(col_w)
            for i, cw in enumerate(col_w):
                tb.columns[i].width = Emu(int(Inches(w) * cw / tot))
        tb.rows[0].height = Inches(head_h)
        for i in range(1, r):
            tb.rows[i].height = Inches(row_h)
        for ri, row in enumerate(rows):
            for ci, val in enumerate(row):
                cell = tb.cell(ri, ci)
                cell.margin_left = cell.margin_right = Inches(0.06)
                cell.margin_top = cell.margin_bottom = Inches(0.015)
                cell.vertical_anchor = MSO_ANCHOR.MIDDLE
                tf = cell.text_frame
                tf.word_wrap = True
                p = tf.paragraphs[0]
                run = p.add_run()
                run.text = str(val)
                run.font.size = Pt(head_fs if ri == 0 else fs)
                run.font.bold = ri == 0
                run.font.name = CJK
                run.font.color.rgb = rgb((255, 255, 255) if ri == 0 else (0x22, 0x2B, 0x38))
                cjk(run)
                cell.fill.solid()
                if ri == 0:
                    cell.fill.fore_color.rgb = rgb(NAVY)
                else:
                    cell.fill.fore_color.rgb = rgb((255, 255, 255) if ri % 2 else LIGHT)
        return shp

    def note(slide, x, y, w, text, color=RED, size=12, h=0.5):
        rect(slide, x, y, w, h, (0xFD, 0xF2, 0xF2) if color == RED else LIGHT, color, line_w=1.2)
        tf = txbox(slide, x + 0.12, y, w - 0.24, h, anchor=MSO_ANCHOR.MIDDLE)
        para(tf, text, size, color, bold=True, first=True, space_before=0)

    n = 0

    def page(title=None, kicker=None, part=None, bg=True):
        nonlocal n
        n += 1
        s = prs.slides.add_slide(blank)
        if title:
            header(s, title, kicker, part)
        footer(s, n)
        return s

    # ============================================================ 1 封面
    s = page()
    rect(s, 0, 0, SW, SH, NAVY)
    rect(s, 0, 4.62, SW, 0.06, TEAL)
    tf = txbox(s, 0.9, 1.55, 11.6, 1.2)
    para(tf, "MLK-F3P-DR1M90G 外设方案报告", 46, (255, 255, 255), bold=True, first=True, space_before=0)
    tf = txbox(s, 0.95, 2.85, 11.6, 1.6)
    para(tf, "从「自研电机板」到「传感器载板」", 24, (0xBF, 0xD6, 0xE8), bold=True, first=True, space_before=0)
    para(tf, "—— 为什么放弃方案 A，以及现在的方案 B 长什么样 ——", 17, (0x8F, 0xB2, 0xCE), space_before=10)
    tf = txbox(s, 0.95, 4.95, 11.6, 1.8)
    para(tf, "硬件平台：MLK-F3P-DR1M90G（安路 DR1M90G FPSoC，PL=FPGA / PS=ARM）",
         15, (0xD7, 0xE3, 0xEF), first=True, space_before=0)
    para(tf, "选题方向：领域 5 —— 设备智能运维 / 故障诊断（电机外设采集系统）",
         15, (0xD7, 0xE3, 0xEF), space_before=8)
    para(tf, "报告日期：2026-09-29", 13, (0x9F, 0xC4, 0xD8), space_before=8)

    # ============================================================ 2 目录
    s = page("报告要点", "两句话结论：① 方案 A（自研电机板）器件多、还要处理共地与隔离；② 方案 B 把复杂留给市售产品",
             part="目录")
    tf = txbox(s, 0.75, 1.5, 11.8, 0.6)
    para(tf, "第一部分 · 为什么不用方案 A（自研「电机板」）", 20, NAVY, bold=True, first=True, space_before=0)
    b = txbox(s, 1.0, 2.2, 11.4, 1.6)
    for t in ["A-1  方案 A 是什么：一块板包办 电源 / 电机驱动 / 电流采集 / 通信",
              "A-2  问题①：器件多、板子复杂、调试风险高（124 器件 / 4 层 / 三级电源）",
              "A-3  问题②：共地与隔离 —— 单域共地、11 路信号直连、倒灌与地环",
              "A-4  问题③：被测对象自带驱动（市售整套），自研反而把系统与被测对象绑死"]:
        para(b, t, 14, (0x33, 0x3D, 0x4D), first=(t == "A-1  方案 A 是什么：一块板包办 电源 / 电机驱动 / 电流采集 / 通信"),
             space_before=7)
    tf = txbox(s, 0.75, 4.1, 11.8, 0.6)
    para(tf, "第二部分 · 现在的方案 B（核心板 + 传感器载板 + 市售电机整套）", 20, TEAL, bold=True, first=True, space_before=0)
    b = txbox(s, 1.0, 4.8, 11.4, 1.9)
    for i, t in enumerate(["B-1  系统架构总览（一张图）",
                           "B-2  传感器载板：13 器件 / 独立 5V 供电 / 与核心板单点共地",
                           "B-3  接口 J12（PL-GPIO）↔ 传感器板 P1；电机整套独立；编码器走 CAN；其余用小模块"]):
        para(b, t, 14, (0x33, 0x3D, 0x4D), first=(i == 0), space_before=7)

    # ============================================================ 3 方案A是什么
    s = page("方案 A：一块自研「电机板」包办全部功能", "最初计划：电机驱动 + 电流采集 + 电源管理 + 总线通信，全部自己画在一块板子上",
             part="第一部分")
    tf = txbox(s, 0.55, 1.35, 6.6, 4.9)
    para(tf, "板上的功能块（9 个）：", 16, NAVY, bold=True, first=True, space_before=0)
    for t in ["① 电源输入：USB-C PD 20V（HUSB238 协商）",
              "② DC-DC 降压：20V→12V/3A（TPS54560 Buck + 电感 + 续流）",
              "③ 电机驱动：VNH5050A H 桥（30A，三块散热 slug）",
              "④ 电流采集：ACS725 霍尔（±20A，板载）",
              "⑤ 辅助电源：12V→3.3V（原 LM317，后改 DC-DC + LDO）",
              "⑥ ADC：MCP3204（SPI 12bit，4 通道）",
              "⑦ CAN 总线：SN65HVD232D + 端子 + TVS",
              "⑧ 声光报警：蜂鸣器 + LED + 驱动管",
              "⑨ 本地过流保护：窗口比较器 + 逻辑联锁（不经 PL）"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), space_before=5)
    pic(s, imgs["sch_motor.jpg"], 7.35, 1.3, 5.45, 4.9)
    note(s, 0.55, 6.25, 5.6, "原理图 9 个分栏、页面密集；元件位号 + 网络名交错", color=GRAY, size=11.5, h=0.42)
    tf = txbox(s, 7.35, 6.25, 5.45, 0.5)
    para(tf, "▲ 电机板（MLK-F3P-Motor）原理图总览", 11.5, GRAY, first=True, space_before=0, align=PP_ALIGN.CENTER)

    # ============================================================ 4 问题①器件多
    s = page("问题①：器件多、板子复杂、调试风险高", "124 器件 / 369 导线 / 71 网络 —— 电源与功率部分尤其难伺候",
             part="第一部分")
    tf = txbox(s, 0.55, 1.3, 6.5, 5.2)
    para(tf, "规模", 16, NAVY, bold=True, first=True, space_before=0)
    for t in ["124 器件 / 369 导线 / 71 网络（归档口径）",
              "三级电源链：20V → 12V → 3.3V，每级都要电感/电容/补偿",
              "4 层板：功率铜、AGND 岛、星接点、散热过孔阵列"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    para(tf, "工程难度", 16, NAVY, bold=True, space_before=12)
    for t in ["H 桥三块散热焊盘必须落在三个不同网络（接错=投板即毁）",
              "大电流：额定 5.2A、堵转 36A → 线宽/铺铜/端子都要单独算",
              "EMC：开关节点、电机 PWM 与模拟采样链（1LSB≈0.8mV）必须分区隔离",
              "保护/滤波：20+ 串阻、TVS、磁珠、缓冲网络，任何一处接反都炸管"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    pic(s, imgs["pcb2d_motor.jpg"], 7.3, 1.35, 5.5, 2.5)
    pic(s, imgs["pcb3d_motor.jpg"], 7.3, 3.95, 5.5, 2.25)
    tf = txbox(s, 7.3, 6.25, 5.5, 0.45)
    para(tf, "▲ 电机板 PCB：元件密度、功率分区、四层叠层", 11.5, GRAY, first=True, space_before=0, align=PP_ALIGN.CENTER)

    # ============================================================ 5 问题②共地隔离
    s = page("问题②：共地与隔离 —— 最费精力的一环", "这块板与核心板必须「共地」，但共地又会把电机侧的噪声/回流/倒灌全部带进来",
             part="第一部分")
    tf = txbox(s, 0.55, 1.3, 6.35, 5.3)
    para(tf, "必须做的处理", 16, NAVY, bold=True, first=True, space_before=0)
    for t in ["单域共地：本板 GND 经连接器与核心板连通，所有电源（12V/5V/3.3V）共用同一参考地",
              "模拟地分区：AGND（霍尔/ADC/基准）用 0Ω 单点星接到数字地 GND",
              "地环/地弹：电机回流不能穿过模拟区，功率地与信号地要分开",
              "接口倒灌：11 路信号直连核心板 PL，两板上电时序不一致就会互灌电流",
              "只能靠 100Ω 串阻限流（单线 ≤20mA），并禁止带电插拔"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=6)
    para(tf, "为什么没做「真隔离」", 16, RED, bold=True, space_before=12)
    for t in ["要么全隔离：数字隔离器 + 隔离 DC-DC + 隔离放大器（AMC3302）",
              "代价：器件数 / 成本 / 面积再上一个量级，还会破坏单域共地的调试便利"]:
        para(tf, t, 12.5, RED, bullet=True, space_before=6)
    rect(s, 7.15, 1.35, 5.65, 4.15, LIGHT, LINE)
    tf = txbox(s, 7.35, 1.5, 5.25, 3.9)
    para(tf, "方案 A 的「共地账」", 15, NAVY, bold=True, first=True, space_before=0)
    for t in ["本板电源 5 路（PD 20V / 母线 12V / 3.3V_LDO / 3.3V 静轨 / AGND）全部同参考",
              "核心板 PL IO 与电机功率级共用一块地",
              "电流：霍尔虽自带 4.8kV 隔离，但整板仍是非隔离系统",
              "结果：想把「采集系统」做干净，就得为电机侧噪声买单",
              "结论：隔离不是不能做，而是与本题「简单、可交付」的目标相反"]:
        para(tf, t, 12, (0x33, 0x3D, 0x4D), bullet=True, space_before=6)
    note(s, 7.15, 5.65, 5.65, "这才是换方案的核心原因：共地/隔离的成本与风险远大于收益",
         color=RED, size=13, h=0.6)

    # ============================================================ 6 问题③需求
    s = page("问题③：被测对象自带驱动，自研板是重复建设", "被测电机是市售整套（电源 + 调速器 + 电机），本来就会自己转 —— 我们只负责「观察」",
             part="第一部分")
    tf = txbox(s, 0.55, 1.3, 6.35, 5.3)
    para(tf, "市售整套已经包含", 16, NAVY, bold=True, first=True, space_before=0)
    for t in ["电源适配器（24V）", "调速器（支持外部电位器/开关调速、正反转）", "755 有刷直流电机本体"]:
        para(tf, t, 13, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    para(tf, "因此方案 A 的驱动板是重复建设", 16, NAVY, bold=True, space_before=12)
    for t in ["再画一块 H 桥 + Buck：功能与调速器重叠，却引入 20V/H 桥/大电流风险",
              "把「采集系统」和「被测对象」焊死在同一块板上：电机一坏，板子一起坏",
              "答辩时还要额外解释：为什么采集板要能驱动电机？"]:
        para(tf, t, 13, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    note(s, 0.55, 5.9, 6.35, "方案 A 的定位错位：把「被测对象」当成了「自研系统的一部分」",
         color=TEAL, size=13, h=0.6)
    pic(s, imgs["motor_set.jpg"], 7.2, 1.35, 5.6, 5.05)
    tf = txbox(s, 7.2, 6.45, 5.6, 0.45)
    para(tf, "▲ 市售「电源 + 调速器 + 755 电机」整套：买来即用，独立运行", 11.5, GRAY,
         first=True, space_before=0, align=PP_ALIGN.CENTER)

    # ============================================================ 7 A vs B
    s = page("决策：方案 A → 方案 B", "把复杂交给成熟产品，把精力放在「采集 + 诊断」这件正事上",
             part="小结")
    rows = [
        ["维度", "方案 A：自研「电机板」", "方案 B：传感器载板（采用）"],
        ["画板/器件", "1 块 4 层板，124 器件，369 导线", "1 块小板，13 器件，81 导线"],
        ["电源", "自研 20V→12V→3.3V 三级电源", "传感器板独立 5V（USB 取电）+ 核心板 3V3"],
        ["电机驱动", "自研 H 桥 VNH5050A（30A）", "市售整套（电源 + 调速器 + 755 电机）"],
        ["接地", "与核心板单域共地 + AGND 星接", "传感器板经 P1.2 单点共地；电机整套不共地"],
        ["隔离", "非隔离，11 路信号直连，靠串阻限倒灌", "电机侧完全独立，无跨域信号，无需隔离"],
        ["电流采集", "板载 ACS725（±20A）", "市售霍尔小模块（U1/U2 串联，选用）"],
        ["通信", "板载 CAN + SPI 直连 PL", "编码器 CAN → 核心板 J11；传感器 → P1 的 PL IO"],
        ["调试风险", "20V / H 桥 / 大电流，接错即毁板", "5V 低压，风险极低，随时可改"],
        ["交付周期/成本", "高（多层板 + 多次返工）", "低（小板 + 现成模块）"],
    ]
    table(s, 0.72, 1.32, 11.9, rows, col_w=[1.5, 3.6, 4.0], fs=11, head_fs=12, row_h=0.5, head_h=0.42)
    note(s, 0.72, 6.42, 11.9, "一句话：方案 B 用「市售产品」消化掉电源/驱动/隔离，只保留一块 5V 的「信号引出 + 供电」板",
         color=GREEN, size=13.5, h=0.52)

    # ============================================================ 8 架构图
    s = page("方案 B 系统架构总览", "核心板 ⇄ 传感器载板（共地、独立 5V 供电）；电机整套独立；编码器走 CAN；其余传感器用小模块",
             part="第二部分")
    pic(s, diagram, 0.42, 1.25, 12.5, 5.6)

    # ============================================================ 9 传感器板
    s = page("传感器载板 MLK-F3P-DR1M90G：13 器件，只做三件事", "① 给传感器小模块供电 ② 把信号引到 PL（J12）③ 给霍尔模块留电流通路",
             part="第二部分")
    rows = [
        ["Ref", "器件", "作用"],
        ["J1", "USB-C 16P（受电）", "插电脑取 5V"],
        ["R1/R2", "5.1kΩ", "CC1/CC2 下拉（取 5V）"],
        ["F1", "PTC 1.1A", "5V 过流保护"],
        ["D1", "TVS SMBJ5.0A", "5V 浪涌钳位"],
        ["C1/C2", "10µF + 100nF", "+5V 储能 / 去耦"],
        ["J2", "2×8 排母", "8×+5V + 8×GND → 传感器模块"],
        ["P1", "2×13 排母", "对插核心板 J12（PL-GPIO）"],
        ["H1", "PZ1.0 4P", "+3V3 抽头（4 脚全 3V3）"],
        ["U1/U2", "2P 螺钉端子", "霍尔电流模块电流端（选用）"],
    ]
    table(s, 0.6, 1.3, 5.5, rows, col_w=[0.9, 2.0, 2.6], fs=10.5, head_fs=11.5, row_h=0.44, head_h=0.42)
    pic(s, imgs["sch_sensor.jpg"], 6.5, 1.28, 6.3, 5.2)
    tf = txbox(s, 0.6, 6.06, 5.5, 0.6)
    para(tf, "PCB：顶层+底层铺铜，GND 就近打地过孔；全板单域共地（经 P1.2）。",
         11.5, GRAY, first=True, space_before=0)
    tf = txbox(s, 6.5, 6.55, 6.3, 0.45)
    para(tf, "▲ 传感器载板原理图（13 器件 / 81 导线 / 32 网络）", 11.5, GRAY,
         first=True, space_before=0, align=PP_ALIGN.CENTER)

    # ============================================================ 10 传感器板 PCB
    s = page("传感器载板：PCB 与实物形态", "小板、低压、无功率回路 —— 与方案 A 的 4 层功率板形成鲜明对比",
             part="第二部分")
    pic(s, imgs["pcb2d_sensor.jpg"], 0.6, 1.30, 12.1, 2.42)
    tf = txbox(s, 0.6, 3.76, 12.1, 0.34)
    para(tf, "▲ 2D 布局：四角 M3 安装孔；J2 供电排母 / P1 对插排母 / U1·U2 螺钉端子 / H1 3V3 抽头",
         11.5, GRAY, first=True, space_before=0, align=PP_ALIGN.CENTER)
    pic(s, imgs["pcb3d_sensor.jpg"], 0.6, 4.16, 12.1, 2.34)
    tf = txbox(s, 0.6, 6.54, 12.1, 0.34)
    para(tf, "▲ 3D 渲染：全为小封装贴片 + 排母/端子，无电感 / 无大电解 / 无散热铜",
         11.5, GRAY, first=True, space_before=0, align=PP_ALIGN.CENTER)

    # ============================================================ 11 J12 接口
    s = page("关键接口：核心板 J12 ⇄ 传感器板 P1", "J12 = BANK13（3V3）：1 脚 3V3、2 脚 GND、3–26 脚 = 24 路 PL IO",
             part="第二部分")
    rows = [["脚", "网络", "脚", "网络"]]
    pairs = [("1", "VCC_CEP1（3V3）", "2", "GND（两板唯一地）")]
    for i in range(1, 13):
        pairs.append((str(2 * i + 1), f"CEP1_{i}P", str(2 * i + 2), f"CEP1_{i}N"))
    rows += [list(p) for p in pairs]
    table(s, 0.62, 1.28, 7.4, rows, col_w=[0.7, 2.9, 0.7, 2.9], fs=10, head_fs=11, row_h=0.36, head_h=0.38)
    tf = txbox(s, 8.35, 1.35, 4.35, 5.1)
    para(tf, "要点", 16, NAVY, bold=True, first=True, space_before=0)
    for t in ["J12.1 = 3V3（VCC_CEP1）→ 经 P1.1 引到 H1 抽头",
              "J12.2 = GND —— 两板之间唯一的地，不要再拉第二根",
              "3–26 脚为 PL IO（CEP1_xP/N 成对命名，可做差分/单端）",
              "⚠ 偶数脚全是 IO，绝不可接地（J12 只有 2 脚是地）",
              "传感器信号直接跳线到对应 IO，PL 端写逻辑即可",
              "IO 电平 3V3；接 5V 模块时注意电平/钳位"]:
        para(tf, t, 12, (0x33, 0x3D, 0x4D), bullet=True, space_before=6)
    note(s, 8.35, 6.35, 4.35, "两板共地只有 P1.2 一个点 → 无地环", color=GREEN, size=12, h=0.52)

    # ============================================================ 12 电机整套
    s = page("被测电机：市售整套，独立运行", "不需要画板，也不需要与核心板共地或通信 —— 我们只采集",
             part="第二部分")
    tf = txbox(s, 0.62, 1.3, 6.1, 5.2)
    para(tf, "组成", 16, NAVY, bold=True, first=True, space_before=0)
    for t in ["24V 开关电源适配器", "调速器（面板调速 / 正反转）", "755 有刷直流电机（额定 5.2A / 24V / 6000rpm）"]:
        para(tf, t, 13, (0x33, 0x3D, 0x4D), bullet=True, space_before=6)
    para(tf, "与核心板的关系", 16, NAVY, bold=True, space_before=12)
    for t in ["不共地：没有一根地线与采集系统相连",
              "不通信：不接收 PWM/指令，也不回传数据",
              "完全独立：整套自带电源与调速，插电即转"]:
        para(tf, t, 13, (0x33, 0x3D, 0x4D), bullet=True, space_before=6)
    para(tf, "霍尔电流端子 U1 / U2（选用）", 16, RED, bold=True, space_before=12)
    for t in ["用于把电机电源「串联」进霍尔电流模块（IP+ / IP−）",
              "如果不需要测电机电流，这两个端子可完全不接（选装）",
              "⚠ 电流大：额定 5.2A、堵转 36A → 必须 ≥18AWG 粗线 + 螺钉端子；严禁排母/杜邦"]:
        para(tf, t, 12.5, RED if "⚠" in t else (0x33, 0x3D, 0x4D), bullet=True, space_before=6)
    rect(s, 7.0, 1.35, 5.75, 4.9, LIGHT, LINE)
    tf = txbox(s, 7.25, 1.55, 5.3, 4.5)
    para(tf, "为什么这样最省事", 15, TEAL, bold=True, first=True, space_before=0)
    for t in ["电源/驱动/保护由成品负责 → 无高压、无大电流设计风险",
              "两套系统物理上分开 → 无共地噪声、无倒灌、无需隔离",
              "电机坏了不影响采集板；采集板坏了不耽误电机转",
              "答辩口径清晰：我们做的是「状态监测」，不是「电机控制」",
              "只测电流时，串一个市售霍尔模块到 U1/U2 即可（选装）"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=8)

    # ============================================================ 13 编码器+模块
    s = page("其他外设：编码器走 CAN，传感器用小模块", "全部为市售件，只做「接线 + 供电」，不增加任何自研电路",
             part="第二部分")
    pic(s, imgs["encoder.jpg"], 0.62, 1.35, 4.0, 4.9)
    tf = txbox(s, 0.62, 6.3, 4.0, 0.45)
    para(tf, "▲ 绝对式旋转编码器（CAN 输出）", 11.5, GRAY, first=True, space_before=0, align=PP_ALIGN.CENTER)
    tf = txbox(s, 4.95, 1.3, 7.9, 5.3)
    para(tf, "编码器（测转速 / 角度 / 方向）", 15, NAVY, bold=True, first=True, space_before=0)
    for t in ["CAN 总线：直接接到核心板的 CAN 接口（底板 J11 / U22，CAN0TX/RX 3V3）",
              "供电与接地：取自电机侧 24V（不占用传感器板的 5V，也不与传感器板共地）",
              "PL 侧只需解析报文 → 得到实时转速/角度，用于故障诊断特征"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    para(tf, "其他传感器：全部市售小模块", 15, NAVY, bold=True, space_before=12)
    for t in ["温度传感器、振动传感器、电流传感器（霍尔）、声光报警装置",
              "供电：J2 的 +5V / GND（最多 8 组）；信号：跳线到 P1 的 PL IO",
              "需要 3V3 的模块：从 H1 抽头取（来自核心板 J12.1）"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    para(tf, "本地显示", 15, NAVY, bold=True, space_before=12)
    for t in ["核心板带 HDMI OUT（JH1）→ 直接接电脑显示器当本地屏",
              "显示诊断结果 / 实时波形，不额外画显示电路"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=5)
    note(s, 4.95, 6.35, 7.9, "所有外设共 4 类接口：PL IO（P1）· 5V/3V3 供电（J2/H1）· CAN（J11）· HDMI（JH1）",
         color=TEAL, size=12.5, h=0.52)

    # ============================================================ 14 总结
    s = page("总结", None, part="小结")
    rect(s, 0.62, 1.28, 5.85, 5.0, (0xFC, 0xF0, 0xF0), RED, line_w=1.5)
    tf = txbox(s, 0.88, 1.44, 5.35, 4.7)
    para(tf, "方案 A（弃用）", 19, RED, bold=True, first=True, space_before=0)
    for t in ["124 器件 / 369 导线 / 4 层板",
              "自研三级电源 + H 桥 + ADC + CAN + 保护",
              "必须与核心板共地 → AGND 星接、地环、倒灌、禁带电插拔",
              "真隔离要加数字隔离器 + 隔离 DC-DC + 隔离放大器 → 量级上升",
              "被测对象自带驱动，自研驱动板重复且危险"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=8)
    rect(s, 6.85, 1.28, 5.85, 5.0, (0xEF, 0xF7, 0xF3), GREEN, line_w=1.5)
    tf = txbox(s, 7.1, 1.44, 5.35, 4.7)
    para(tf, "方案 B（采用）", 19, GREEN, bold=True, first=True, space_before=0)
    for t in ["传感器载板：13 器件 / 81 导线 / 单域共地",
              "独立 5V 供电（USB 取电），经 P1.2 一点共地",
              "P1 ⇄ J12 引 24 路 PL IO；H1 引 3V3；J2 供 8 组 +5V",
              "电机整套市售：不共地、不通信、无高压风险",
              "编码器走 CAN → 核心板 J11；其余传感器用小模块",
              "本地显示直接用电脑显示器（HDMI）"]:
        para(tf, t, 12.5, (0x33, 0x3D, 0x4D), bullet=True, space_before=8)
    note(s, 0.62, 6.42, 12.08, "核心思想：把「电源 / 驱动 / 隔离」交给成熟产品，用可交付性换复杂度；我们只做「采集 + 诊断」",
         color=NAVY, size=13.5, h=0.52)

    os.makedirs(os.path.dirname(OUT_PPTX), exist_ok=True)
    prs.save(OUT_PPTX)
    print(f"\n  saved -> {OUT_PPTX}  ({len(prs.slides.__iter__.__self__._sldIdLst)} slides)")


def main():
    print("[1] 裁图")
    imgs = prep_images()
    print("[2] 画架构图")
    dia = make_diagram(os.path.join(WORK, "diagram.png"))
    print("[3] 生成 PPTX")
    build(imgs, dia)


if __name__ == "__main__":
    main()
