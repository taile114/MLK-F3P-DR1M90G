// tb_board_test.v —— board_test_top 自校验板测顶层的 RTL 级测试。
//
// 检查三部分：
//   1. 四组固定向量的 30 个时域字段与 8 个 FFT 结果全部在容差内；
//   2. LED 三态：运行中 status 慢闪、全部通过后 status 常亮、
//      注入错误重跑后 status 快闪且对应 pass 位拉低；
//   3. 无特征 overrun、无 FFT 丢点/削顶、无看门狗超时。
//
// 失败信息打印 "[FAIL] ..."，结尾再打印 "FAIL: n 项"，命中 run_sim.sh 的
// 失败模式 FAIL[=: ]+[1-9]；全部通过时打印 "ALL PASS"。
//
// 闪烁分频用参数覆盖成小值（64 / 16 拍翻转），板上默认 2^25 / 2^22 拍。
// 第二次运行用 force 复位计数器与电流样本重跑，验证失败态可见：
// 只污染电流通道，故 feature_pass 应降为 0、fft_pass 应保持 1。

`timescale 1ns/1ps

module tb_board_test;
`include "board_test_vectors.vh"

    localparam integer SLOW_SHIFT = 6;      // 与 DUT 参数一致：64 拍翻转
    localparam integer FAST_SHIFT = 4;      // 16 拍翻转
    localparam integer MAX_CYC    = 300000; // 单次运行上限（实际约 44000 拍）
    localparam integer STEADY_CYC = 512;    // 常亮观察窗（8 个慢闪周期）

    localparam [2:0] ST_WAIT_MON = 3'd2;    // board_test_top 的 ST_WAIT
    localparam [2:0] ST_LOAD_MON = 3'd3;    // board_test_top 的 ST_LOAD

    reg  I_sysclk = 1'b0;
    wire [3:0] O_led;

    always #5 I_sysclk = ~I_sysclk;         // 100 MHz

    board_test_top #(.BT_SLOW_SHIFT(SLOW_SHIFT), .BT_FAST_SHIFT(FAST_SHIFT)) X_dut (
        .I_sysclk (I_sysclk),
        .O_led    (O_led));

    integer errors      = 0;
    integer cyc;
    integer run_toggles = 0;
    integer fail_toggles = 0;
    integer i;
    reg     led0_q = 1'b0;

    // 运行中统计 status 灯翻转（LED[3]=0）；结束后统计失败态翻转。
    always @(posedge I_sysclk) begin
        led0_q <= O_led[0];
        if (led0_q != O_led[0]) begin
            if (!O_led[3]) run_toggles <= run_toggles + 1;
            else           fail_toggles <= fail_toggles + 1;
        end
    end

    task chk(input cond, input string what);
        begin
            if (!cond) begin
                errors = errors + 1;
                $display("[FAIL] %s", what);
            end
        end
    endtask

    // 期望 FFT 结果（与 .vh 同源）。
    function [31:0] f_bin(input [1:0] v);
        case (v)
            2'd0: f_bin = BT_FFT_BIN_0;
            2'd1: f_bin = BT_FFT_BIN_1;
            2'd2: f_bin = BT_FFT_BIN_2;
            default: f_bin = BT_FFT_BIN_3;
        endcase
    endfunction

    function [63:0] f_pow(input [1:0] v);
        case (v)
            2'd0: f_pow = BT_FFT_POW_0;
            2'd1: f_pow = BT_FFT_POW_1;
            2'd2: f_pow = BT_FFT_POW_2;
            default: f_pow = BT_FFT_POW_3;
        endcase
    endfunction

    // 每进入一组向量的比较（ST_WAIT -> ST_LOAD，ci=fi=0）时打印实测与解析值。
    reg [2:0] st_q = 3'd0;
    wire [2:0] W_set_ch3 = (X_dut.S_vec == 2'd0) ? 3'd4 : {1'b0, X_dut.S_vec};

    always @(posedge I_sysclk) begin
        st_q <= X_dut.S_state;
        if (X_dut.S_state == ST_LOAD_MON && st_q == ST_WAIT_MON &&
            X_dut.S_ci == 3'd0 && X_dut.S_fi == 3'd0) begin
            $display("[V%0d] 实测 ch0  DC=%0d RMS=%0d VAR=%0d P2P=%0d CREST=%0d ZC=%0d KURT=%0d",
                X_dut.S_vec,
                X_dut.S_fsnap[0*448+0*64 +: 64], X_dut.S_fsnap[0*448+1*64 +: 64],
                X_dut.S_fsnap[0*448+2*64 +: 64], X_dut.S_fsnap[0*448+3*64 +: 64],
                X_dut.S_fsnap[0*448+4*64 +: 64], X_dut.S_fsnap[0*448+5*64 +: 64],
                X_dut.S_fsnap[0*448+6*64 +: 64]);
            $display("[V%0d] 解析 ch0  DC=%0d RMS=%0d VAR=%0d P2P=%0d CREST=%0d ZC=%0d KURT=%0d",
                X_dut.S_vec,
                f_bt_expected({1'b0, X_dut.S_vec}, 3'd0), f_bt_expected({1'b0, X_dut.S_vec}, 3'd1),
                f_bt_expected({1'b0, X_dut.S_vec}, 3'd2), f_bt_expected({1'b0, X_dut.S_vec}, 3'd3),
                f_bt_expected({1'b0, X_dut.S_vec}, 3'd4), f_bt_expected({1'b0, X_dut.S_vec}, 3'd5),
                f_bt_expected({1'b0, X_dut.S_vec}, 3'd6));
            $display("[V%0d] 实测 ch3  DC=%0d RMS=%0d VAR=%0d P2P=%0d CREST=%0d ZC=%0d KURT=%0d",
                X_dut.S_vec,
                X_dut.S_fsnap[3*448+0*64 +: 64], X_dut.S_fsnap[3*448+1*64 +: 64],
                X_dut.S_fsnap[3*448+2*64 +: 64], X_dut.S_fsnap[3*448+3*64 +: 64],
                X_dut.S_fsnap[3*448+4*64 +: 64], X_dut.S_fsnap[3*448+5*64 +: 64],
                X_dut.S_fsnap[3*448+6*64 +: 64]);
            $display("[V%0d] 解析 ch3  DC=%0d RMS=%0d VAR=%0d P2P=%0d CREST=%0d ZC=%0d KURT=%0d",
                X_dut.S_vec,
                f_bt_expected(W_set_ch3, 3'd0), f_bt_expected(W_set_ch3, 3'd1),
                f_bt_expected(W_set_ch3, 3'd2), f_bt_expected(W_set_ch3, 3'd3),
                f_bt_expected(W_set_ch3, 3'd4), f_bt_expected(W_set_ch3, 3'd5),
                f_bt_expected(W_set_ch3, 3'd6));
            $display("[V%0d] 实测 FFT bin=%0d power=%0d flags=%0d seq=%0d；解析 bin=%0d power=%0d",
                X_dut.S_vec, X_dut.S_fft_bin, X_dut.S_fft_pow,
                X_dut.S_fft_flags, X_dut.S_fft_seq,
                f_bin(X_dut.S_vec), f_pow(X_dut.S_vec));
        end
    end

    initial begin
        // ---------------- 第一次运行：全部向量应通过 ----------------
        cyc = 0;
        while (O_led[3] !== 1'b1 && cyc < MAX_CYC) begin
            @(posedge I_sysclk);
            cyc = cyc + 1;
        end
        chk(cyc < MAX_CYC, "run1: done 未在超时上限内出现（状态机卡住）");
        $display("[run1] %0d 拍后 done，O_led=%b", cyc, O_led);

        chk(O_led === 4'b1111, "run1: done 后 O_led 应为 4'b1111（全部通过且 status 常亮）");
        chk(X_dut.S_feature_pass === 1'b1, "run1: feature_pass 应为 1");
        chk(X_dut.S_fft_pass === 1'b1, "run1: fft_pass 应为 1");
        if (X_dut.S_err_cnt !== 16'd0) begin
            errors = errors + 1;
            $display("[FAIL] run1: %0d 个比较项超出容差", X_dut.S_err_cnt);
        end
        chk(X_dut.S_ovr === 1'b0, "run1: 出现了特征 overrun");
        chk(X_dut.S_wdog_hit === 1'b0, "run1: 看门狗超时");

        // 运行中 status 慢闪：复位释放到 done 之间至少翻转两次。
        if (run_toggles < 2) begin
            errors = errors + 1;
            $display("[FAIL] run1: 运行中 status 灯只翻转了 %0d 次，应至少闪烁", run_toggles);
        end else begin
            $display("[run1] 运行中 status 灯翻转 %0d 次（慢闪）", run_toggles);
        end

        // 通过后 status 常亮：STEADY_CYC 拍内不得翻转。
        i = 0;
        chk(O_led[0] === 1'b1, "run1: 通过后 status 灯应为常亮");
        while (i < STEADY_CYC) begin
            @(posedge I_sysclk);
            if (O_led[0] !== 1'b1) begin
                errors = errors + 1;
                $display("[FAIL] run1: 通过后 status 灯在 %0d 拍处跳变，应常亮", i);
                i = STEADY_CYC;
            end else begin
                i = i + 1;
            end
        end

        // ---------------- 第二次运行：注入错误，应判失败 ----------------
        // 电流通道样本强制为 +2000（V0 期望 500），其余通道与 FFT 不受影响。
        @(posedge I_sysclk);
        force X_dut.S_sample_cur = 16'sd2000;
        force X_dut.S_rst_cnt    = 12'd0;
        repeat (8) @(posedge I_sysclk);
        release X_dut.S_rst_cnt;
        fail_toggles = 0;

        cyc = 0;
        while (O_led[3] !== 1'b1 && cyc < MAX_CYC) begin
            @(posedge I_sysclk);
            cyc = cyc + 1;
        end
        chk(cyc < MAX_CYC, "run2: 复位重跑后 done 未出现");
        $display("[run2] %0d 拍后 done，O_led=%b", cyc, O_led);

        chk(X_dut.S_feature_pass === 1'b0, "run2: 电流通道被注入错误，feature_pass 应为 0");
        chk(X_dut.S_fft_pass === 1'b1, "run2: FFT 只看 X 轴，fft_pass 应保持 1");
        chk(O_led[3] === 1'b1, "run2: done 位应为 1");
        chk(O_led[2] === 1'b0, "run2: feature_pass 灯应为 0");
        chk(O_led[1] === 1'b1, "run2: fft_pass 灯应为 1");

        // 失败态 status 快闪：128 拍内至少翻转两次（16 拍一次）。
        i = 0;
        fail_toggles = 0;
        while (i < 128) begin
            @(posedge I_sysclk);
            i = i + 1;
        end
        if (fail_toggles < 2) begin
            errors = errors + 1;
            $display("[FAIL] run2: 失败态 status 灯 128 拍内只翻转 %0d 次，应快闪",
                     fail_toggles);
        end else begin
            $display("[run2] 失败态 status 灯 128 拍内翻转 %0d 次（快闪）", fail_toggles);
        end

        release X_dut.S_sample_cur;

        if (errors == 0) begin
            $display("ALL PASS: board_test_top 四组向量在容差内，LED 三态与故障注入均符合预期");
        end else begin
            $display("FAIL: %0d 项检查未通过", errors);
        end
        #100;
        $finish;
    end

endmodule
