#!/usr/bin/env python3
"""板测向量参数、期望常数与自检报告生成器。

本脚本是板测顶层 ``06_doc/board_test_top.v`` 的期望值唯一来源：它按**解析公式**
算出四组固定向量在 ``time_features`` / ``vibration_fft`` 上的期望输出，写入
``06_doc/board_test_vectors.vh``，RTL 只做「取常数 + 比较」，不含参考模型。

三个要点：

1. 期望值来自闭式公式（常数、等差数列、单音、方波），不是 RTL 仿真反填。
   脚本另有一个逐点整数参考实现 ``bruteforce()``，只用于断言：闭式与逐点必须
   逐位一致，否则脚本报错退出。两者都由本文档的公式独立推出。
2. 向量的生成规则（常数、斜坡步长、正弦 8 相位表、方波幅度）也写进 ``.vh``，
   RTL 用同一组 localparam 产生激励，向量与期望同源。
3. 定标与契约见 ``docs/PL-PS接口约定.md`` §4.3 与 ``06_doc/时域与频域特征.md``：
   时域七字段除峰峰值为整数外都是 Q16；FFT 输出为 DFT(x)/N 的 Q8（整数 = 值×256），
   peak_power = Re²+Im²（两个 Q8 分量），频率刻度 f[k]=k·Fs/N。

用法：``python3 gen_board_test_vectors.py``（无第三方依赖）。
"""

from pathlib import Path
import math

BASE = Path(__file__).resolve().parent
VH = BASE.parent / "06_doc" / "board_test_vectors.vh"

# ---------------------------------------------------------------- 向量参数 ----
N = 256                    # 特征窗口长度 2**8，同时是 FFT 长度
WLOG2 = 8
Q16 = 1 << 16

V0_ACC = 1000              # 常数段：X/Y/Z
V0_CUR = 500               # 常数段：电流
V1_STEP = 64               # 斜坡 x[n]=64*n，范围 0..16320（0..16383 内）
SINE_A = 8000              # 单音幅度，bin=32（窗内 32 个周期）
HALF_A = 5657              # round(A/sqrt(2))，正弦表在 45° 相位的取整值
SINE_TABLE = [0, HALF_A, SINE_A, HALF_A, 0, -HALF_A, -SINE_A, -HALF_A]
SQUARE_A = 30000           # 方波幅度，前半窗 +A、后半窗 -A


def vector(v):
    """第 v 组向量（长度 N 的有符号整数列表）。"""
    if v == 0:
        return [V0_ACC] * N
    if v == 1:
        return [V1_STEP * n for n in range(N)]
    if v == 2:
        return [SINE_TABLE[n % 8] for n in range(N)]
    if v == 3:
        return [SQUARE_A if n < N // 2 else -SQUARE_A for n in range(N)]
    raise ValueError(v)


# ------------------------------------------------------------ 解析期望值 ----
def _div(numa, den):
    """无符号整数除法，分母为零时返回 0（与 unsigned_divider 一致）。"""
    return numa // den if den else 0


def closed_form(v):
    """闭式时域期望值，返回 (dc, rms, var, p2p, crest, zc, kurt)。

    记 n=256，S=Σx，E2=Σx²，D2=Σ(n·x-S)²=n²·E2-n·S²，D4=Σ(n·x-S)⁴，
    P=max|x|，Z=相邻样本符号位变化次数（零归非负）。RTL 逐字段按下列定义计算：

      DC       = (S<<16) >> 8                （算术右移 = floor(S·2^16/n)）
      RMS      = floor(sqrt(E2 << (32-8)))
      方差     = floor(D2·2^16 / n^3)
      峰峰值   = max(x)-min(x)               （整数，非 Q16）
      峰值因子 = floor(P·2^32 / RMS)          （Q16）
      过零率   = floor(Z·2^16 / (n-1))        （Q16）
      峭度     = floor(D4·2^(16+8) / D2²)     （Q16；D2=0 时除零返回 0）

    各向量代入上式后都是初等闭式：
      V0 常数 c：S=n·c，E2=n·c²，D2=D4=0 → RMS=c·2^16，除零字段为 0。
      V1 斜坡 s·k：S=s·n(n-1)/2，E2=s²(n-1)n(2n-1)/6，
                   Σ(k-(n-1)/2)⁴ = n(n²-1)(3n²-7)/240。
      V2 单音：每周期 8 个相位对称，S=0，E2/E4 按 8 相位表求和 ×(n/8)；
               32 个周期有 64 次符号变化，末次在窗口边界不计，Z=63。
      V3 方波：S=0，E2=n·A²，E4=n·A⁴。
    """
    n = N
    if v == 0:                                        # 常数 c
        c = V0_ACC
        return ((n * c << 16) >> WLOG2, c << 16, 0, 0, Q16, 0, 0)

    if v == 1:                                        # 等差斜坡 s·k
        s, last = V1_STEP, V1_STEP * (n - 1)
        S = s * (n - 1) * n // 2
        E2 = s * s * (n - 1) * n * (2 * n - 1) // 6
        c4 = n * (n * n - 1) * (3 * n * n - 7) // 240  # Σ(k-均值)⁴
        D2 = n * n * E2 - n * S * S
        D4 = n ** 4 * s ** 4 * c4
        return ((S << 16) >> WLOG2, math.isqrt(E2 << (32 - WLOG2)),
                (D2 << 16) >> (3 * WLOG2), last, (last << 32) // math.isqrt(E2 << (32 - WLOG2)),
                0, (D4 << (16 + WLOG2)) // (D2 * D2))

    if v == 2:                                        # 8 相位正弦表
        E2 = (n // 8) * sum(x * x for x in SINE_TABLE)
        E4 = (n // 8) * sum(x ** 4 for x in SINE_TABLE)
        D2 = n * n * E2                               # S=0
        D4 = n ** 4 * E4                              # S=0，中心矩即原点矩
        rms = math.isqrt(E2 << (32 - WLOG2))
        return (0, rms, (D2 << 16) >> (3 * WLOG2), 2 * SINE_A,
                (SINE_A << 32) // rms, (2 * 32 - 1) * Q16 // (n - 1),
                (D4 << (16 + WLOG2)) // (D2 * D2))

    A = SQUARE_A                                      # ±A 方波，各占半窗
    E2 = n * A * A
    D2 = n * n * E2
    D4 = n ** 4 * (n * A ** 4)
    rms = math.isqrt(E2 << (32 - WLOG2))
    return (0, rms, (D2 << 16) >> (3 * WLOG2), 2 * A, (A << 32) // rms,
            Q16 // (n - 1), (D4 << (16 + WLOG2)) // (D2 * D2))


def bruteforce(xs):
    """逐点整数参考，仅用于断言闭式公式；按定义直译，与 RTL 实现无关。"""
    n = len(xs)
    S = sum(xs)
    d = [n * x - S for x in xs]
    D2 = sum(t * t for t in d)
    D4 = sum(t ** 4 for t in d)
    E2 = sum(x * x for x in xs)
    rms = math.isqrt(E2 << (32 - WLOG2))
    peak = max(abs(x) for x in xs)
    zc = sum(1 for a, b in zip(xs, xs[1:]) if (a < 0) != (b < 0))
    return ((S << 16) >> WLOG2, rms, (D2 << 16) >> (3 * WLOG2),
            max(xs) - min(xs), _div(peak << 32, rms),
            zc * Q16 // (n - 1), _div(D4 << (16 + WLOG2), D2 * D2))


# --------------------------------------------------------------- FFT 期望 ----
def fft_expected(v):
    """矩形窗（WINDOW=0）下振动 FFT 的 (peak_bin, peak_power)。

    ``fft_radix2`` 载入时把样本放大 2^8（Q8），每级蝶形再右移 1 位；LOGN=8
    时净增益 2^8/2^8 = 1，故 N=256 时输出分量 Re/Im 就是 DFT(x) 的实部/虚部，
    O_power = Re²+Im² = |X[k]|²。峰值取 bin 1..N/2 的最大功率；全零时停在
    bin 0、功率 0。

      V0 常数去均值后全零                       |X|=0
      V1 斜坡：|X[k]| = s·N/(2·sin(πk/N))       k=1
      V2 单音：|X[k0]| = N·A/2                  k0=32（窗内 32 周期，相干采样）
      V3 方波：|X[k]| = 2A/sin(πk/N)（奇次谐波）k=1
    """
    if v == 0:
        return 0, 0
    if v == 1:
        mag = V1_STEP * N / (2 * math.sin(math.pi / N))
        return 1, int(round(mag * mag))
    if v == 2:
        mag = N * SINE_A / 2
        return 32, int(round(mag * mag))
    mag = 2 * SQUARE_A / math.sin(math.pi / N)
    return 1, int(round(mag * mag))


def dft_mag(xs, k):
    """逐点 DFT 在频点 k 的幅度，仅用于交叉校验闭式值。"""
    n = len(xs)
    re = sum(x * math.cos(2 * math.pi * k * i / n) for i, x in enumerate(xs))
    im = -sum(x * math.sin(2 * math.pi * k * i / n) for i, x in enumerate(xs))
    return math.hypot(re, im)


# ----------------------------------------------------------------- 断言 ----
FIELDS = ("DC", "RMS", "VAR", "P2P", "CREST", "ZC", "KURT")


def check():
    cur = ((N * V0_CUR << 16) >> WLOG2, V0_CUR << 16, 0, 0, Q16, 0, 0)
    if cur != bruteforce([V0_CUR] * N):
        raise SystemExit(f"V0 电流通道期望 {cur} 与逐点参考 {bruteforce([V0_CUR]*N)} 不一致")

    for v in range(4):
        xs = vector(v)
        cf, bf = closed_form(v), bruteforce(xs)
        if cf != bf:
            raise SystemExit(f"V{v} 闭式与逐点参考不一致：closed={cf} brute={bf}")

        if v == 0 and (cf[2], cf[3], cf[5], cf[6]) != (0, 0, 0, 0):
            raise SystemExit("V0 零方差字段应全为 0")
        if v == 2:
            if abs(cf[1] / Q16 - SINE_A / math.sqrt(2)) > 0.01 * SINE_A:
                raise SystemExit("V2 RMS 偏离 A/sqrt(2) 超过 1%")
            if abs(cf[6] - 1.5 * Q16) > 0.01 * 1.5 * Q16:
                raise SystemExit("V2 峭度偏离 1.5 超过 1%")
            # 32 个周期共 64 次符号变化，末次落在窗口边界不计，故计 63 次。
            if cf[5] != 63 * Q16 // (N - 1):
                raise SystemExit("V2 过零数应为 63")
        if v == 1 and cf[5] != 0:
            raise SystemExit("V1 全非负，过零率应为 0")

        bin_, power = fft_expected(v)
        if power >= (1 << 64):
            raise SystemExit(f"V{v} 期望功率溢出 64 位")
        if v != 0:
            mags = [dft_mag(xs, k) for k in range(1, N // 2 + 1)]
            best = max(range(len(mags)), key=lambda i: mags[i]) + 1
            if best != bin_:
                raise SystemExit(f"V{v} 闭式峰值 bin={bin_}，逐点 DFT 最大在 {best}")
            if abs(mags[best - 1] ** 2 - power) > 0.01 * power:
                raise SystemExit(f"V{v} 闭式功率与逐点 DFT 相差超过 1%")
        print(f"V{v}: " + " ".join(f"{FIELDS[i]}={cf[i]}" for i in range(7)) +
              f" | peak_bin={bin_} peak_power={power}")


# ----------------------------------------------------------------- 输出 ----
def emit():
    sets = (("V0 加速度", closed_form(0)), ("V1 斜坡", closed_form(1)),
            ("V2 单音", closed_form(2)), ("V3 方波", closed_form(3)),
            ("V0 电流", ((N * V0_CUR << 16) >> WLOG2, V0_CUR << 16, 0, 0, Q16, 0, 0)))

    lines = [
        "// 板测顶层向量参数与期望常数，由 02_sim/gen_board_test_vectors.py 生成。",
        "// 手动修改无效：改脚本后重新生成。期望值全部来自解析公式，非 RTL 仿真反填。",
        "",
        "localparam integer BT_N = 256;      // 特征窗口与 FFT 长度 2**8",
        "localparam integer BT_WLOG2 = 8;",
        "",
        "// 向量生成参数（激励与期望同源）",
        f"localparam signed [15:0] BT_V0_ACC = 16'sd{V0_ACC};",
        f"localparam signed [15:0] BT_V0_CUR = 16'sd{V0_CUR};",
        f"localparam signed [15:0] BT_V1_STEP = 16'sd{V1_STEP};",
    ]
    for i, val in enumerate(SINE_TABLE):
        lines.append(f"localparam signed [15:0] BT_SINE_{i} = 16'sh{val & 0xFFFF:04x};")
    lines += [
        f"localparam signed [15:0] BT_V3_AMP = 16'sd{SQUARE_A};",
        "",
        "// 期望时域字段：组 0..3 对应 V0..V3，组 4 是 V0 电流通道；",
        "// 字段 0..6 = DC/RMS/方差/峰峰值/峰值因子/过零率/峭度，除峰峰值为整数外均 Q16。",
        "function [63:0] f_bt_expected;",
        "    input [2:0] I_set;",
        "    input [2:0] I_field;",
        "    begin",
        "        case ({I_set, I_field})",
    ]
    for s_i, (tag, vals) in enumerate(sets):
        for f_i, val in enumerate(vals):
            lines.append(f"            6'd{s_i * 8 + f_i}: f_bt_expected = 64'd{val};"
                         f"  // {tag} {FIELDS[f_i]}")
    lines += [
        "            default: f_bt_expected = 64'd0;",
        "        endcase",
        "    end",
        "endfunction",
        "",
        "// 期望 FFT：peak_bin 必须精确相等；peak_power = Re²+Im² = |X[peak_bin]|²。",
    ]
    for v in range(4):
        bin_, power = fft_expected(v)
        lines.append(f"localparam [31:0] BT_FFT_BIN_{v} = 32'd{bin_};")
        lines.append(f"localparam [63:0] BT_FFT_POW_{v} = 64'd{power};")
    VH.write_text("\n".join(lines) + "\n")
    print(f"\n写入 {VH}（{len(lines)} 行）")


if __name__ == "__main__":
    check()
    emit()
