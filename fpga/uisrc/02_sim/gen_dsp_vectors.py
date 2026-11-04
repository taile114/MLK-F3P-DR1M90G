#!/usr/bin/env python3
"""确定性整数参考模型、时域测试向量与 FFT 量化 ROM。无第三方依赖。"""
from pathlib import Path
import math
import random

BASE = Path(__file__).resolve().parent
DATA = BASE / "vectors"
DATA.mkdir(exist_ok=True)

def features(xs):
    n = len(xs)
    total = sum(xs)
    d2 = sum((n*x-total)**2 for x in xs)
    d4 = sum((n*x-total)**4 for x in xs)
    rms = math.isqrt((sum(x*x for x in xs) << 32)//n)
    fields = [(total << 16)//n, rms, (d2 << 16)//n**3,
              max(xs)-min(xs), (max(map(abs, xs)) << 32)//rms if rms else 0,
              (sum((a < 0) != (b < 0) for a,b in zip(xs,xs[1:])) << 16)//(n-1),
              (d4*n << 16)//(d2*d2) if d2 else 0]
    return sum((v & ((1 << 64)-1)) << (64*i) for i,v in enumerate(fields)), (rms==0) | ((d2==0)<<1)

rng = random.Random(104729)
for n in (256, 512):
    samples = ([0]*n + [37]*n + [-71]*n +
               [-32768,32767]*(n//2) + [i-n//2 for i in range(n)] +
               [rng.randint(-32768,32767) for _ in range(2*n)] +
               [-32768]*n + [32767]+[0]*(n-1))
    (DATA / f"time_{n}_samples.hex").write_text(''.join(f'{x & 65535:04x}\n' for x in samples))
    out = [features(samples[i-n:i]) for i in range(n,len(samples)+1,128)]
    (DATA / f"time_{n}_expected.hex").write_text(''.join(f'{v:0112x}\n' for v,flags in out))
    (DATA / f"time_{n}_flags.hex").write_text(''.join(f'{flags:x}\n' for v,flags in out))

# 512 点母表，256 点仅使用偶数旋转地址；18 bit signed Q15 精确表示 +1。
ROM = BASE.parent / '01_rtl' / 'fft' / 'fft_coeff_rom.v'
lines = ['// FFT 系数 ROM：512 点旋转母表及 256/512 点周期 Hann，signed Q15。',
         '// 由 02_sim/gen_dsp_vectors.py 生成；同步读，可推断 ERAM。',
         'module fft_coeff_rom(input wire I_clk, input wire [7:0] I_tw_addr,',
         '    input wire [8:0] I_win_addr, input wire I_size512,',
         '    output reg signed [17:0] O_cos, O_sin, output reg [16:0] O_window);',
         'reg [35:0] S_tw[0:255];', 'reg [16:0] S_win[0:511];', 'initial begin']
for k in range(256):
    c = round(math.cos(2*math.pi*k/512)*32768)
    s = round(-math.sin(2*math.pi*k/512)*32768)
    lines.append(f"    S_tw[{k}]=36'h{((c&262143)<<18)|(s&262143):09x};")
for k in range(512):
    w = round((0.5-0.5*math.cos(2*math.pi*k/512))*32768)
    lines.append(f"    S_win[{k}]=17'd{w};")
lines += ['end', 'always @(posedge I_clk) begin',
          '    {O_cos,O_sin} <= S_tw[I_tw_addr];',
          "    O_window <= S_win[I_size512 ? I_win_addr : {I_win_addr[7:0],1'b0}];",
          'end', 'endmodule']
ROM.write_text('\n'.join(lines)+'\n')

def fft_fixed(xs, hann):
    n = len(xs)
    logn = n.bit_length()-1
    buf = [0j]*n
    for i,x in enumerate(xs):
        w = round((0.5-0.5*math.cos(2*math.pi*i/n))*32768) if hann else 32768
        rev = int(f'{i:0{logn}b}'[::-1],2)
        buf[rev] = ((x*w) >> 7,0)
    span = 2
    while span <= n:
        for group in range(0,n,span):
            for j in range(span//2):
                c = round(math.cos(2*math.pi*j/span)*32768)
                s = round(-math.sin(2*math.pi*j/span)*32768)
                ar,ai = buf[group+j]
                br,bi = buf[group+j+span//2]
                tr,ti = (br*c-bi*s)>>15,(br*s+bi*c)>>15
                buf[group+j] = ((ar+tr)>>1,(ai+ti)>>1)
                buf[group+j+span//2] = ((ar-tr)>>1,(ai-ti)>>1)
        span *= 2
    return buf

rng = random.Random(99991)
for n in (256,512):
    frames = [[0]*n,[1000]*n,[16000]+[0]*(n-1),
              [round(12000*math.cos(2*math.pi*13*k/n)) for k in range(n)],
              [round(9000*math.sin(2*math.pi*37*k/n)+4000) for k in range(n)],
              [-32768,32767]*(n//2),[rng.randint(-32768,32767) for _ in range(n)]]
    (DATA/f'fft_{n}_samples.hex').write_text(''.join(f'{v&65535:04x}\n' for xs in frames for v in xs))
    for hann in (0,1):
        expected = []
        max_error = 0
        for xs in frames:
            fixed = fft_fixed(xs,hann)
            # 直接 DFT 独立核对数值精度，不复用蝶形索引。
            for k,(r,i) in enumerate(fixed):
                exact = sum(x*((0.5-0.5*math.cos(2*math.pi*t/n)) if hann else 1)*
                            complex(math.cos(-2*math.pi*k*t/n),math.sin(-2*math.pi*k*t/n))
                            for t,x in enumerate(xs))*256/n
                max_error = max(max_error,abs(r-exact.real),abs(i-exact.imag))
            expected.extend(fixed)
        assert max_error < 256, (n,hann,max_error)
        print(f'FFT reference N={n} Hann={hann} max DFT error={max_error/256:.6f} input LSB')
        (DATA/f'fft_{n}_{hann}_expected.hex').write_text(''.join(f'{r&0xffffffff:08x}{i&0xffffffff:08x}\n' for r,i in expected))
