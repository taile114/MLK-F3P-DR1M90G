// 板测顶层：自校验 PL 特征链路，上板后只看 LED 即可判定计算结果。
//
// 结构：固定向量发生器 -> sensor_features（X/Y/Z/电流四通道）
//                          -> vibration_fft（X 轴，LOGN=8，矩形窗 WINDOW=0）
//       -> 片上比较器（特征七字段 + FFT 峰值） -> LED。
//
// 向量按顺序跑，每向量 256 点，恰好一个特征窗口（HOP=N）与一个 FFT 窗口：
//
//   V0  常数     X/Y/Z=+1000，电流=+500      零方差、零过零、零峭度
//   V1  斜坡     x[n]=64·n，0..16320          全正、无过零，验 DC/RMS/方差
//   V2  单音     x[n]=A·sin(2π·32n/256)，A=8000，8 相位表 32 次重复
//   V3  方波     前半窗 +30000、后半窗 -30000
//
// 期望常数由 02_sim/gen_board_test_vectors.py 按解析公式生成，写入
// board_test_vectors.vh；RTL 只取常数并比较，不含参考模型，也不用 RTL 仿真
// 结果反填。容差（相对）：
//   时域字段 |got-exp| <= exp/64 + exp/256 + 1，约 1.95%
//   FFT 峰值功率 |got-exp| <= exp/32 + exp/128 + 1，约 3.9%
//   FFT peak_bin 必须精确相等
// 期望为零的字段（V0 的方差/峰峰/过零/峭度、V2 与 V3 的直流）容差为 1。
//
// LED 语义（高有效，无需 PS）：
//   O_led[3] done          四组向量全部跑完并比较完毕；运行中为 0
//   O_led[2] feature_pass  done 后，四通道七字段全部在容差内（累计）
//   O_led[1] fft_pass      done 后，peak_bin 精确相等且 peak_power 在容差内
//   O_led[0] status        运行中慢闪，done 且全部通过时常亮，done 且失败时快闪
// 板上三态由此可直接目视区分：慢闪 = 运行中，常亮 = 全部通过，快闪 = 失败；
// 失败时 [2]/[1] 进一步指出特征侧还是 FFT 侧。
// 正常板：O_led = 4'b1111（[0] 常亮）。
// 失败判据：字段超差、FFT 超差、特征 overrun、FFT 丢点/削顶、看门狗超时。
// 闪烁分频由参数 BT_SLOW_SHIFT / BT_FAST_SHIFT 给出（100 MHz 下约 1.5 Hz / 12 Hz），
// 仿真可用更小的值观察三态。
//
// 握手：特征侧每样本至少两拍（ACCEPT/UPDATE），四通道同步，故以 &O_ready 作
// 单拍选通；FFT 侧无背压，只在 COLLECT 期给 valid，喂满 256 点后等待结果，
// 因此不会丢点（O_flags 应为 0）。看门狗约 2^19 拍无进展即判失败并停止。

`timescale 1ns/1ps

module board_test_top #(
    // status 灯翻转间隔 = 2^BT_SLOW_SHIFT / 2^BT_FAST_SHIFT 拍。
    // 100 MHz：2^25 = 335 ms（约 1.5 Hz），2^22 = 42 ms（约 12 Hz）。
    parameter integer BT_SLOW_SHIFT = 25,
    parameter integer BT_FAST_SHIFT = 22
)(
    input  wire       I_sysclk,
    output reg  [3:0] O_led
);
`include "board_test_vectors.vh"

    // 期望 FFT 峰值：下标为向量号，I_pow=0 取 peak_bin，1 取 peak_power。
    function [63:0] f_bt_fft;
        input [1:0] I_vec;
        input       I_pow;
        begin
            case ({I_vec, I_pow})
                3'd0: f_bt_fft = {32'd0, BT_FFT_BIN_0};
                3'd1: f_bt_fft = BT_FFT_POW_0;
                3'd2: f_bt_fft = {32'd0, BT_FFT_BIN_1};
                3'd3: f_bt_fft = BT_FFT_POW_1;
                3'd4: f_bt_fft = {32'd0, BT_FFT_BIN_2};
                3'd5: f_bt_fft = BT_FFT_POW_2;
                3'd6: f_bt_fft = {32'd0, BT_FFT_BIN_3};
                default: f_bt_fft = BT_FFT_POW_3;
            endcase
        end
    endfunction

    // ------------------------------------------------------------ 复位 ----
    // 必须带初值：无初值在仿真里恒为 x，&x = x 使复位永不释放（上电后
    // 综合网表由配置初值 0 起算，行为与初值 0 一致）。
    reg [11:0] S_rst_cnt = 12'd0;
    wire       S_rstn = &S_rst_cnt;

    always @(posedge I_sysclk) begin
        if (!S_rstn) S_rst_cnt <= S_rst_cnt + 12'd1;
    end

    // -------------------------------------------------------- 主状态机 ----
    localparam [2:0] ST_IDLE = 3'd0, ST_FEED = 3'd1, ST_WAIT = 3'd2,
                     ST_LOAD = 3'd3, ST_JUDGE = 3'd4, ST_FIN = 3'd5,
                     ST_DONE = 3'd6;

    reg [2:0] S_state;
    reg [1:0] S_vec;
    reg [7:0] S_idx;

    reg [2:0] S_ci;      // 当前比较的通道 0..3
    reg [2:0] S_fi;      // 当前比较的字段 0..6
    reg       S_phase;   // 0=特征字段，1=FFT 峰值
    reg       S_ft;      // 1 时比较 peak_power，0 时比较 peak_bin

    reg [19:0] S_wdog;
    reg        S_wdog_hit;

    // ------------------------------------------------------ 向量发生器 ----
    reg signed [15:0] S_sample_acc;

    always @* begin
        case (S_vec)
            2'd0: S_sample_acc = BT_V0_ACC;
            2'd1: S_sample_acc = {2'b00, S_idx, 6'b0};          // 64·n
            2'd2: case (S_idx[2:0])
                      3'd0: S_sample_acc = BT_SINE_0;
                      3'd1: S_sample_acc = BT_SINE_1;
                      3'd2: S_sample_acc = BT_SINE_2;
                      3'd3: S_sample_acc = BT_SINE_3;
                      3'd4: S_sample_acc = BT_SINE_4;
                      3'd5: S_sample_acc = BT_SINE_5;
                      3'd6: S_sample_acc = BT_SINE_6;
                      default: S_sample_acc = BT_SINE_7;
                  endcase
            default: S_sample_acc = S_idx[7] ? -BT_V3_AMP : BT_V3_AMP;
        endcase
    end

    // 电流通道：V0 段取常数 500，其余段与 X 轴同向量（四通道走同一组样本，
    // 便于用同一套期望值检查四份 time_features 是否算得一致）。
    wire signed [15:0] S_sample_cur = (S_vec == 2'd0) ? BT_V0_CUR : S_sample_acc;

    // ---------------------------------------------------- 被验证的实例 ----
    wire [3:0]    S_fready, S_fvalid, S_fovr;
    wire [1791:0] S_feat;

    wire S_strobe = (S_state == ST_FEED) && (&S_fready);

    sensor_features #(.WINDOW_LOG2(BT_WLOG2), .HOP(BT_N)) X_feat (
        .I_clk(I_sysclk), .I_rstn(S_rstn),
        .I_acc_valid(S_strobe), .I_current_valid(S_strobe),
        .I_acc_x(S_sample_acc), .I_acc_y(S_sample_acc), .I_acc_z(S_sample_acc),
        .I_current(S_sample_cur),
        .O_ready(S_fready), .O_valid(S_fvalid), .O_overrun(S_fovr),
        .O_flags(), .O_features(S_feat));

    wire        S_fres_valid;
    wire [31:0] S_fseq, S_fpeak_bin, S_fflags32;
    wire [63:0] S_fpeak_power;

    vibration_fft #(.LOGN(BT_WLOG2), .WINDOW(0)) X_fft (
        .I_clk(I_sysclk), .I_rstn(S_rstn),
        .I_clear(1'b0), .I_enable(1'b1),
        .I_valid(S_strobe), .I_sample(S_sample_acc), .I_ready(1'b1),
        .O_valid(), .O_bin(), .O_power(),
        .O_result_valid(S_fres_valid),
        .O_sequence(S_fseq), .O_peak_bin(S_fpeak_bin),
        .O_flags(S_fflags32), .O_peak_power(S_fpeak_power));

    // ------------------------------------------------------ 结果锁存 ----
    reg [1791:0] S_fsnap;
    reg [3:0]    S_fseen;
    reg [31:0]   S_fft_bin;
    reg [63:0]   S_fft_pow;
    reg [31:0]   S_fft_seq;
    reg [31:0]   S_fft_flags;
    reg          S_fft_seen;
    reg          S_ovr;

    // ST_FIN 一拍清掉本向量的有效标志；清零由锁存块自己完成，避免同一寄存器
    // 被两个 always 块驱动（TD 报 HDL-8007 多驱动）。
    wire S_clr_seen = (S_state == ST_FIN) && (S_vec != 2'd3);

    always @(posedge I_sysclk) begin
        if (!S_rstn) begin
            S_fsnap <= 1792'd0;
            S_fseen <= 4'd0;
            S_fft_bin <= 32'd0;
            S_fft_pow <= 64'd0;
            S_fft_seq <= 32'd0;
            S_fft_flags <= 32'd0;
            S_fft_seen <= 1'b0;
            S_ovr <= 1'b0;
        end else begin
            if (S_fvalid[0]) S_fsnap[0*448 +: 448] <= S_feat[0*448 +: 448];
            if (S_fvalid[1]) S_fsnap[1*448 +: 448] <= S_feat[1*448 +: 448];
            if (S_fvalid[2]) S_fsnap[2*448 +: 448] <= S_feat[2*448 +: 448];
            if (S_fvalid[3]) S_fsnap[3*448 +: 448] <= S_feat[3*448 +: 448];
            if (S_clr_seen) begin
                S_fseen <= 4'd0;
                S_fft_seen <= 1'b0;
            end else begin
                if (S_fvalid[0]) S_fseen[0] <= 1'b1;
                if (S_fvalid[1]) S_fseen[1] <= 1'b1;
                if (S_fvalid[2]) S_fseen[2] <= 1'b1;
                if (S_fvalid[3]) S_fseen[3] <= 1'b1;
                if (S_fres_valid) S_fft_seen <= 1'b1;
            end
            if (S_fres_valid) begin
                S_fft_bin   <= S_fpeak_bin;
                S_fft_pow   <= S_fpeak_power;
                S_fft_seq   <= S_fseq;
                S_fft_flags <= S_fflags32;
            end
            if (|S_fovr) S_ovr <= 1'b1;
        end
    end

    // -------------------------------------------------------- 比较器 ----
    reg [1791:0] S_shift;
    reg [63:0]   S_cmp_got, S_cmp_exp, S_cmp_tol;
    wire [63:0]  S_diff = (S_cmp_got >= S_cmp_exp) ? (S_cmp_got - S_cmp_exp)
                                                   : (S_cmp_exp - S_cmp_got);

    wire [2:0]  W_set = (S_vec == 2'd0 && S_ci == 3'd3) ? 3'd4 : {1'b0, S_vec};
    wire [63:0] W_exp_feat = f_bt_expected(W_set, S_fi);
    wire [63:0] W_exp_fft  = f_bt_fft(S_vec, S_ft);
    wire [63:0] W_exp      = S_phase ? W_exp_fft : W_exp_feat;
    wire [63:0] W_got_fft  = S_ft ? S_fft_pow : {32'd0, S_fft_bin};
    wire [63:0] W_got      = S_phase ? W_got_fft : S_shift[63:0];

    wire [63:0] W_tol = S_phase ? (S_ft ? (W_exp >> 5) + (W_exp >> 7) + 64'd1
                                       : 64'd0)
                                : (W_exp >> 6) + (W_exp >> 8) + 64'd1;

    // 带初值，与复位值一致：TD 对复位为非零的寄存器会报 SYN-5123
    // （按初值 1 推断），显式写出可让 RTL 与综合网表的上电状态一致。
    reg S_feature_pass = 1'b1;
    reg S_fft_pass     = 1'b1;
    reg S_done         = 1'b0;
    reg S_vec_fail, S_fft_fail;
    reg [15:0] S_err_cnt;

    always @(posedge I_sysclk) begin
        if (!S_rstn) begin
            S_state <= ST_IDLE;
            S_vec <= 2'd0;
            S_idx <= 8'd0;
            S_ci <= 3'd0;
            S_fi <= 3'd0;
            S_phase <= 1'b0;
            S_ft <= 1'b0;
            S_wdog <= 20'd0;
            S_wdog_hit <= 1'b0;
            S_shift <= 1792'd0;
            S_cmp_got <= 64'd0;
            S_cmp_exp <= 64'd0;
            S_cmp_tol <= 64'd0;
            S_feature_pass <= 1'b1;
            S_fft_pass <= 1'b1;
            S_done <= 1'b0;
            S_vec_fail <= 1'b0;
            S_fft_fail <= 1'b0;
            S_err_cnt <= 16'd0;
        end else begin
            case (S_state)
                ST_IDLE: if (S_rstn) S_state <= ST_FEED;

                ST_FEED: begin
                    if (&S_fready) begin
                        S_wdog <= 20'd0;
                        S_idx <= S_idx + 8'd1;
                        if (S_idx == 8'd255) begin
                            S_idx <= 8'd0;
                            S_state <= ST_WAIT;
                        end
                    end else begin
                        S_wdog <= S_wdog + 20'd1;
                        if (S_wdog[19]) begin
                            S_wdog_hit <= 1'b1;
                            S_feature_pass <= 1'b0;
                            S_fft_pass <= 1'b0;
                            S_done <= 1'b1;
                            S_state <= ST_DONE;
                        end
                    end
                end

                ST_WAIT: begin
                    S_wdog <= S_wdog + 20'd1;
                    if (S_wdog[19]) begin
                        S_wdog_hit <= 1'b1;
                        S_feature_pass <= 1'b0;
                        S_fft_pass <= 1'b0;
                        S_done <= 1'b1;
                        S_state <= ST_DONE;
                    end else if (&S_fseen && S_fft_seen) begin
                        S_shift <= S_fsnap;
                        S_cmp_got <= 64'd0;
                        S_cmp_exp <= 64'd0;
                        S_cmp_tol <= 64'd0;
                        S_ci <= 3'd0;
                        S_fi <= 3'd0;
                        S_phase <= 1'b0;
                        S_ft <= 1'b0;
                        S_vec_fail <= 1'b0;
                        S_fft_fail <= 1'b0;
                        S_state <= ST_LOAD;
                    end
                end

                ST_LOAD: begin
                    S_cmp_got <= W_got;
                    S_cmp_exp <= W_exp;
                    S_cmp_tol <= W_tol;
                    S_state <= ST_JUDGE;
                end

                ST_JUDGE: begin
                    if (S_diff > S_cmp_tol) begin
                        if (S_phase) S_fft_fail <= 1'b1;
                        else S_vec_fail <= 1'b1;
                        S_err_cnt <= S_err_cnt + 16'd1;
                    end
                    // FFT 结果必须没有丢点/削顶，且结果序号等于向量号+1（O_sequence 为 1 起的计数）
                    if (S_phase && !S_ft &&
                        (S_fft_flags != 32'd0 ||
                         S_fft_seq != ({30'd0, S_vec} + 32'd1)))
                        S_fft_fail <= 1'b1;
                    if (!S_phase) begin
                        // 特征 28 项：通道优先，字段在 64 bit 低位
                        S_shift <= {64'd0, S_shift[1791:64]};
                        if (S_fi == 3'd6) begin
                            S_fi <= 3'd0;
                            if (S_ci == 3'd3) S_phase <= 1'b1;
                            else S_ci <= S_ci + 3'd1;
                        end else begin
                            S_fi <= S_fi + 3'd1;
                        end
                        S_state <= ST_LOAD;
                    end else if (!S_ft) begin
                        S_ft <= 1'b1;
                        S_state <= ST_LOAD;
                    end else begin
                        S_state <= ST_FIN;
                    end
                end

                ST_FIN: begin
                    S_wdog <= 20'd0;
                    S_wdog_hit <= 1'b0;
                    S_feature_pass <= S_feature_pass && !S_vec_fail;
                    S_fft_pass <= S_fft_pass && !S_fft_fail;
                    if (S_vec == 2'd3) begin
                        S_done <= 1'b1;
                        S_state <= ST_DONE;
                    end else begin
                        S_vec <= S_vec + 2'd1;
                        S_state <= ST_FEED;
                    end
                end

                default: S_state <= ST_DONE;   // ST_DONE 保持
            endcase
        end
    end

    // ----------------------------------------------------------- LED ----
    // 失败：任一字段/FFT 超差、特征 overrun、FFT 丢点或削顶、看门狗超时。
    wire S_fail_any = ~(S_feature_pass && S_fft_pass) | S_ovr | S_wdog_hit;

    // 心跳计数器自由运行，不随复位清零：复位期间 O_led 被强制为 0，
    // 复位释放后立即给出稳定的闪烁相位。
    reg [25:0] S_blink = 26'd0;   // 无初值时仿真恒为 x，x+1 仍为 x
    always @(posedge I_sysclk) S_blink <= S_blink + 26'd1;

    wire W_blink    = S_fail_any ? S_blink[BT_FAST_SHIFT] : S_blink[BT_SLOW_SHIFT];
    wire W_status   = S_done ? (S_fail_any ? W_blink : 1'b1) : W_blink;

    always @(posedge I_sysclk) begin
        if (!S_rstn) O_led <= 4'b0000;
        else O_led <= {S_done, S_done && S_feature_pass,
                       S_done && S_fft_pass, W_status};
    end

endmodule
