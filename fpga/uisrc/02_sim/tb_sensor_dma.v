`timescale 1ns/1ps
// sensor_dma（DMA 写通道）自检测 TB：AXI4 写通道从机内存模型 + 外部样本流。
// 覆盖：ramp 模式 64 B 单突发、4 KB 多突发、非法 I_bytes、写响应非 OKAY、
//       I_abort 中止、外部流经 O_s_ready 反压一次不丢数；
//       环形模式 N=4 连续 9 块的块地址回绕 / BLK_SEQ / 单拍 O_block_done、
//       环大小非法（0 / 257 / 基地址超 4 GiB）启动即拒绝、START 清零 BLK_SEQ、
//       环形传输后单次模式行为不变；
//       I_stop 优雅停止：环形模式完成当前突发后释放 BUSY、ERRCODE 保持 0、
//       BLK_SEQ 保留、无 DONE、不再发起突发，停止后可重新 START；
//       单次模式 I_stop 同样不报错、不置 DONE，O_written 停在突发边界。
//
// 接口未规定、由本 TB 约定的语义（被测模块须与之一致）：
//   1) I_test=1 时内部产生 64 位 ramp，复位后第 i 拍 = i；
//      实现若改用 {ts,ch,sample} 形式，可用 -P tb_sensor_dma.RAMP_MODE=1/2 对齐；
//   2) I_start 为单拍启动脉冲；I_abort 保持高直到 O_busy 释放；I_stop 为单拍停止脉冲；
//   3) I_bytes 非 8 倍数或大于 4096 时启动即拒绝：O_error != 0、不产生 AXI 事务；
//   4) O_error 由复位清除；
//   5) 每拍 8 字节、INCR 突发、突发不跨 4 KB、wlast 位于突发末拍；
//   6) STATUS.blocks_done 不是 sensor_dma 端口：TB 以 O_written/64（64 B 块数）
//      作为单元级等价量，寄存器侧接线由 SoC / 寄存器集成 TB 覆盖；
//   7) 环形模式块固定 4 KiB，块地址 = BASE + (块号 mod N) × 4096，O_block_done 为单拍，
//      O_blk_seq 为 START 起的完成块数；环形模式不置位 O_done，I_bytes 忽略；
//   8) 环形模式 I_blocks 超出 1~256 或基地址非法时启动即拒绝（与 3 同一路径），
//      单次模式下 BLK_SEQ / block_done 保持 0；
//   9) I_stop 完成当前突发（等写响应）后回到 IDLE：不置 O_error、不置 O_done，
//      O_blk_seq 保留，O_written 为突发整倍数；与 I_abort（立即释放 BUSY、
//      ERRCODE=3、末拍补位）不同。单次模式收到 I_stop 时若当前突发恰好是最后一个，
//      传输已完成，仍置 O_done。
module tb_sensor_dma;
    localparam [63:0] BASE_TEST     = 64'h0000_0000_1F00_0000;
    localparam [32:0] BASE_TEST_A   = 33'h1F00_0000;
    localparam [63:0] BASE_STREAM   = 64'h0000_0000_1F00_1000;
    localparam [32:0] BASE_STREAM_A = 33'h1F00_1000;
    localparam [63:0] BASE_RING     = 64'h0000_0000_1F00_2000;   // 4 KiB 对齐
    localparam [32:0] BASE_RING_A   = 33'h1F00_2000;

    // 0：64 位递增（默认约定）；1：{16'd0,16'd0,i}；2：{i,16'd0,i}。
    parameter integer RAMP_MODE = 0;

    reg clk = 0;
    always #5 clk = ~clk;
    reg rstn = 0;

    reg start = 0, abort = 0, stop = 0, test_mode = 0;
    reg ring = 0;
    reg [63:0] base = 0;
    reg [31:0] bytes = 0;
    reg [31:0] blocks = 0;
    wire busy, done, s_ready, block_done;
    wire [2:0] error;
    wire [31:0] written, blk_seq;
    reg [63:0] s_data = 0;
    reg s_valid = 0;

    wire [32:0] m_awaddr;
    wire [7:0]  m_awlen;
    wire [2:0]  m_awsize;
    wire [1:0]  m_awburst;
    wire        m_awvalid, m_awready;
    wire [63:0] m_wdata;
    wire [7:0]  m_wstrb;
    wire        m_wlast, m_wvalid, m_wready;
    wire [1:0]  m_bresp;
    wire        m_bvalid, m_bready;

    reg mem_clear = 0, mem_stall = 0, mem_aw_stall = 0;
    wire mem_awready;
    assign m_awready = mem_awready && !mem_aw_stall;
    reg [32:0] stalled_addr;
    reg [63:0] stalled_data;
    reg [1:0] mem_bresp = 2'b00;

    integer checks = 0;
    integer done_pulses = 0, error_seen = 0, bp_cycles = 0, wlast_pulses = 0;
    integer block_pulses = 0;
    reg last_block_done = 0;
    reg [2:0] error_code = 0;
    integer s_idx = 0, s_total = 0;
    reg s_en = 0, s_ready_seen = 0;
    integer k, stop_beats;
    integer aw_mark;
    integer stop_bursts, stop_beats_mark;
    reg [31:0] stop_wr_mark, stop_seq_mark;

    sensor_dma dut(
        .I_clk(clk), .I_rstn(rstn),
        .I_start(start), .I_abort(abort), .I_stop(stop),
        .I_base(base), .I_bytes(bytes), .I_test(test_mode),
        .I_ring(ring), .I_blocks(blocks),
        .O_busy(busy), .O_done(done), .O_error(error), .O_written(written),
        .O_block_done(block_done), .O_blk_seq(blk_seq),
        .I_s_data(s_data), .I_s_valid(s_valid), .O_s_ready(s_ready),
        .M_axi_awaddr(m_awaddr), .M_axi_awlen(m_awlen), .M_axi_awsize(m_awsize),
        .M_axi_awburst(m_awburst), .M_axi_awvalid(m_awvalid), .M_axi_awready(m_awready),
        .M_axi_wdata(m_wdata), .M_axi_wstrb(m_wstrb), .M_axi_wlast(m_wlast),
        .M_axi_wvalid(m_wvalid), .M_axi_wready(m_wready),
        .M_axi_bresp(m_bresp), .M_axi_bvalid(m_bvalid), .M_axi_bready(m_bready));

    // 环形 9 块 = 4608 拍，记录容量放大到 16 K 拍。
    axi_slave_mem_model #(.MEM_WORDS(16384)) u_mem(
        .I_clk(clk), .I_rstn(rstn), .I_clear(mem_clear),
        .I_w_stall(mem_stall), .I_bresp(mem_bresp),
        .I_awaddr(m_awaddr), .I_awlen(m_awlen), .I_awsize(m_awsize),
        .I_awburst(m_awburst), .I_awvalid(m_awvalid && !mem_aw_stall), .O_awready(mem_awready),
        .I_wdata(m_wdata), .I_wstrb(m_wstrb), .I_wlast(m_wlast),
        .I_wvalid(m_wvalid), .O_wready(m_wready),
        .O_bresp(m_bresp), .O_bvalid(m_bvalid), .I_bready(m_bready));

    // 完成 / 错误 / 源端反压 / 块完成监测。
    always @(posedge clk) if (rstn) begin
        if (done) done_pulses = done_pulses + 1;
        if (error != 3'd0) begin
            error_seen = error_seen + 1;
            error_code = error;
        end
        if (block_done) block_pulses = block_pulses + 1;
        if (block_done && last_block_done) $fatal(1, "FAIL block_done wider than one cycle");
        last_block_done = block_done;
        if (s_en && s_valid && s_ready) s_ready_seen = 1;
        if (s_en && s_valid && s_ready_seen && !s_ready) bp_cycles = bp_cycles + 1;
        if (m_wvalid && m_wready && m_wlast) wlast_pulses = wlast_pulses + 1;
    end

    // 外部样本流源：仅在 O_s_ready 接受时推进，保持数据稳定，不丢不重。
    always @(negedge clk) begin
        if (!rstn || !s_en) begin
            s_valid <= 1'b0;
            s_data  <= 64'd0;
        end else if (s_idx < s_total) begin
            s_valid <= 1'b1;
            s_data  <= stream_word(s_idx);
        end else begin
            s_valid <= 1'b0;
        end
    end
    always @(posedge clk) begin
        if (!rstn || !s_en) s_idx <= 0;
        else if (s_valid && s_ready) s_idx <= s_idx + 1;
    end

    function [63:0] ramp_word(input [31:0] idx);
        begin
            case (RAMP_MODE)
                1: ramp_word = {32'd0, 16'd0, idx[15:0]};
                2: ramp_word = {idx[31:0], 16'd0, idx[15:0]};
                default: ramp_word = {32'd0, idx[31:0]};
            endcase
        end
    endfunction

    function [63:0] stream_word(input [31:0] idx);
        begin
            stream_word = {32'h1000_0000 + idx, 16'h0001, idx[15:0]};
        end
    endfunction

    task check(input bit good, input [255:0] name);
        begin
            checks = checks + 1;
            if (!good) $fatal(1, "FAIL %s check=%0d", name, checks);
        end
    endtask

    task reset_dut;
        begin
            rstn = 0;
            start = 0; abort = 0; stop = 0; test_mode = 0; s_en = 0;
            ring = 0; blocks = 0;
            base = 0; bytes = 0; s_total = 0;
            mem_clear = 1; mem_stall = 0; mem_aw_stall = 0; mem_bresp = 2'b00;
            repeat(4) @(negedge clk);
            rstn = 1;
            repeat(2) @(negedge clk);
            mem_clear = 0;
            repeat(2) @(negedge clk);
            done_pulses = 0; error_seen = 0; error_code = 0;
            bp_cycles = 0; s_ready_seen = 0; wlast_pulses = 0;
            block_pulses = 0; last_block_done = 0;
        end
    endtask

    task do_start(input [63:0] b, input [31:0] n, input t);
        begin
            @(negedge clk);
            base = b; bytes = n; test_mode = t; ring = 0; blocks = 0;
            start = 1;
            @(negedge clk);
            start = 0;
        end
    endtask

    // 环形启动：I_ring=1、环大小 I_blocks=nb，I_bytes 置 4 KiB（环形模式忽略）。
    task do_ring_start(input [63:0] b, input [31:0] n, input [31:0] nb);
        begin
            @(negedge clk);
            base = b; bytes = n; test_mode = 1'b1; ring = 1'b1; blocks = nb;
            start = 1;
            @(negedge clk);
            start = 0;
        end
    endtask

    // 环形模式中止：I_abort 保持到 O_busy 释放，再等收尾（补齐在途突发 + 写响应）
    // 结束，避免下一次 START 落在非 IDLE 状态。



    task wait_finish(input integer limit, input [255:0] name);
        integer j;
        begin
            j = 0;
            while (j < limit && done_pulses == 0 && error_seen == 0) begin
                @(negedge clk); j = j + 1;
            end
            if (done_pulses == 0 && error_seen == 0) $fatal(1, "FAIL %s timeout", name);
            repeat(4) @(negedge clk);
        end
    endtask

    task wait_idle(input integer limit, input [255:0] name);
        integer j;
        begin
            j = 0;
            while (j < limit && busy) begin @(negedge clk); j = j + 1; end
            if (busy) $fatal(1, "FAIL %s timeout", name);
            repeat(4) @(negedge clk);
        end
    endtask

    // 等环形模式完成指定块数（轮询 O_blk_seq）。
    task wait_blocks(input integer n, input integer limit, input [255:0] name);
        integer j;
        begin
            j = 0;
            while (j < limit && blk_seq < n) begin @(negedge clk); j = j + 1; end
            if (blk_seq < n) $fatal(1, "FAIL %s timeout", name);
            repeat(2) @(negedge clk);
        end
    endtask

    // 环形模式中止：I_abort 保持到 O_busy 释放，再等收尾（补齐在途突发 + 写响应）
    // 结束，避免下一次 START 落在非 IDLE 状态。
    task abort_ring(input [255:0] name);
        begin
            abort = 1;
            wait_idle(4000, name);
            repeat(96) @(negedge clk);
            abort = 0;
            repeat(2) @(negedge clk);
        end
    endtask

    // 优雅停止脉冲：I_stop 保持两拍（寄存器侧写 DMA_CTRL[4]=1 只产生一拍）。
    task do_stop;
        begin
            @(negedge clk);
            stop = 1;
            @(negedge clk);
            stop = 0;
        end
    endtask

    // 逐突发核对环形块地址：块 b 的突发 j：BASE + (b mod nper)*4096 + (j mod 32)*128。
    task check_ring_addresses(input [32:0] b, input integer nblocks, input integer nper);
        integer j, blk, idx;
        reg [32:0] exp_addr;
        begin
            for (j = 0; j < nblocks * 32; j = j + 1) begin
                blk = j / 32;
                idx = j % 32;
                exp_addr = b + (blk % nper) * 4096 + idx * 128;
                check(u_mem.S_burst_addr_log[j] === exp_addr, "ring block address wraps");
            end
        end
    endtask

    // 环形参数非法：启动即拒绝，不发 AXI、不产生块完成。
    task expect_bad_ring(input [31:0] nb);
        begin
            reset_dut;
            do_ring_start(BASE_RING, 32'd4096, nb);
            wait_finish(200, "bad ring blocks");
            check(error_seen > 0, "bad ring blocks sets O_error");
            check(error_code == 3'd1, "bad ring blocks ERRCODE=1");
            check(busy == 1'b0, "bad ring blocks does not start");
            check(u_mem.S_aw_count == 0, "bad ring blocks no AW");
            check(blk_seq == 32'd0, "bad ring blocks BLK_SEQ=0");
            check(block_pulses == 0, "bad ring blocks no block_done");
        end
    endtask

    task check_words(input [32:0] b, input integer n, input integer is_ramp);
        integer j;
        reg [32:0] exp_addr;
        begin
            for (j = 0; j < n; j = j + 1) begin
                exp_addr = b + j * 8;
                check(u_mem.S_waddr[j] === exp_addr, "AXI write address increments");
                check(u_mem.S_wdata[j] === (is_ramp ? ramp_word(j) : stream_word(j)),
                      "AXI write data matches");
            end
        end
    endtask

    task check_bursts(input [32:0] b, input integer total_beats);
        integer j;
        reg [32:0] exp_addr;
        reg [12:0] beat_bytes;
        begin
            check(u_mem.S_burst_count > 1, "4KB multiple bursts");
            exp_addr = b;
            for (j = 0; j < u_mem.S_burst_count; j = j + 1) begin
                check(u_mem.S_burst_addr_log[j] === exp_addr, "burst start address contiguous");
                beat_bytes = ({5'd0, u_mem.S_burst_len_log[j]} + 13'd1) * 13'd8;
                check(beat_bytes <= 13'd4096, "burst fits 4KB window");
                exp_addr = exp_addr + beat_bytes;
            end
            check(exp_addr === (b + total_beats * 8), "bursts cover all bytes");
        end
    endtask

    task expect_bad_bytes(input [31:0] n);
        begin
            reset_dut;
            do_start(BASE_TEST, n, 1'b1);
            wait_finish(200, "bad bytes");
            check(error_seen > 0, "bad bytes sets O_error");
            check(error_code != 3'd0, "O_error nonzero");
            check(busy == 1'b0, "bad bytes does not start");
            check(u_mem.S_aw_count == 0, "bad bytes no AW");
            check(u_mem.S_beat_count == 0, "bad bytes no W");
            check(done_pulses == 0, "bad bytes no DONE");
            check(written == 32'd0, "bad bytes O_written=0");
        end
    endtask

    initial begin
        repeat(4) @(negedge clk);
        rstn = 1;
        repeat(2) @(negedge clk);

        // 1) ramp 模式 64 字节单突发。
        reset_dut;
        do_start(BASE_TEST, 32'd64, 1'b1);
        wait_finish(2000, "ramp 64B");
        wait_idle(100, "ramp 64B");
        check(done_pulses > 0, "ramp DONE");
        check(error_seen == 0, "ramp no error");
        check(busy == 1'b0, "ramp releases BUSY");
        check(written == 32'd64, "ramp O_written=64");
        check(u_mem.S_aw_count == 1, "ramp one AW");
        check(u_mem.S_beat_count == 8, "ramp 8 beats");
        check(u_mem.S_burst_count == 1, "ramp single burst");
        check(u_mem.S_burst_len_log[0] == 8'd7, "ramp AWLEN=7");
        check(u_mem.S_burst_addr_log[0] === BASE_TEST_A, "ramp first address");
        check(wlast_pulses == 1, "ramp one WLAST");
        check(u_mem.S_error_count == 0, "ramp AXI protocol clean");
        check(u_mem.S_byte_count == written, "ramp bytes match O_written");
        check((written >> 6) == 32'd1, "ramp blocks_done equivalent 1");
        check(blk_seq == 32'd0, "ramp single mode BLK_SEQ=0");
        check(block_pulses == 0, "ramp single mode no block_done");
        check_words(BASE_TEST_A, 8, 1);

        // 2) 4 KB 多突发。
        reset_dut;
        do_start(BASE_TEST, 32'd4096, 1'b1);
        wait_finish(20000, "4KB");
        wait_idle(1000, "4KB");
        check(done_pulses > 0, "4KB DONE");
        check(error_seen == 0, "4KB no error");
        check(busy == 1'b0, "4KB releases BUSY");
        check(written == 32'd4096, "4KB O_written=4096");
        check(u_mem.S_beat_count == 512, "4KB 512 beats");
        check(u_mem.S_burst_count > 1, "4KB multiple bursts");
        check(wlast_pulses == u_mem.S_burst_count, "4KB WLAST per burst");
        check(u_mem.S_aw_count == u_mem.S_burst_count, "4KB one AW per burst");
        check(u_mem.S_error_count == 0, "4KB AXI protocol clean");
        check(u_mem.S_byte_count == written, "4KB bytes match O_written");
        check((written >> 6) == 32'd64, "4KB blocks_done equivalent 64");
        check(blk_seq == 32'd0, "4KB single mode BLK_SEQ=0");
        check(block_pulses == 0, "4KB single mode no block_done");
        check_bursts(BASE_TEST_A, 512);
        check_words(BASE_TEST_A, 512, 1);

        // 3) 非法字节数：非 8 倍数、超过 4096。
        expect_bad_bytes(32'd68);
        expect_bad_bytes(32'd4104);

        // 4) 写响应非 OKAY（SLVERR）。
        reset_dut;
        mem_bresp = 2'b10;
        do_start(BASE_TEST, 32'd4096, 1'b1);
        wait_finish(20000, "SLVERR");
        wait_idle(1000, "SLVERR");
        check(error_seen > 0, "SLVERR sets O_error");
        check(error_code != 3'd0, "SLVERR O_error nonzero");
        check(busy == 1'b0, "SLVERR releases BUSY");
        check(written <= 32'd4096, "SLVERR O_written within range");

        // 5) I_abort 中止传输。
        reset_dut;
        s_total = 128;
        do_start(BASE_STREAM, 32'd1024, 1'b0);
        s_en = 1;
        k = 0;
        while (k < 2000 && (s_idx < 16 || u_mem.S_beat_count == 0)) begin
            @(negedge clk); k = k + 1;
        end
        check(s_idx >= 16, "abort stream accepted");
        check(u_mem.S_beat_count > 0, "abort AXI writes started");
        abort = 1;
        wait_idle(500, "abort");
        s_en = 0;
        check(done_pulses == 0, "abort no DONE");
        // 中止后仍要把在途突发补齐（最多 16 拍），等 AXI 完全静止后再取样。
        repeat(64) @(negedge clk);
        stop_beats = u_mem.S_beat_count;
        repeat(32) @(negedge clk);
        check(u_mem.S_beat_count == stop_beats, "abort stops AXI writes");
        check(stop_beats > 0 && stop_beats < 128, "abort transfer incomplete");
        abort = 0;

        // 6) 外部流经 O_s_ready 反压一次，数据不丢。
        reset_dut;
        s_total = 512;
        do_start(BASE_STREAM, 32'd4096, 1'b0);
        s_en = 1;
        k = 0;
        while (k < 4000 && s_idx < 32) begin @(negedge clk); k = k + 1; end
        check(s_idx >= 32, "stream initial beats accepted");
        bp_cycles = 0;
        mem_stall = 1;
        k = 0;
        while (k < 8000 && bp_cycles == 0) begin @(negedge clk); k = k + 1; end
        check(bp_cycles > 0, "stream O_s_ready backpressure");
        check(s_idx < s_total, "stream data pending during backpressure");
        mem_stall = 0;
        wait_finish(40000, "stream");
        wait_idle(4000, "stream");
        check(done_pulses > 0, "stream DONE");
        check(error_seen == 0, "stream no error");
        check(busy == 1'b0, "stream releases BUSY");
        check(written == 32'd4096, "stream O_written=4096");
        check(u_mem.S_beat_count == 512, "stream 512 beats");
        check(u_mem.S_burst_count > 1, "stream multiple bursts");
        check(wlast_pulses == u_mem.S_burst_count, "stream WLAST per burst");
        check(u_mem.S_error_count == 0, "stream AXI protocol clean");
        check(u_mem.S_byte_count == written, "stream bytes match O_written");
        check((written >> 6) == 32'd64, "stream blocks_done equivalent 64");
        check_words(BASE_STREAM_A, 512, 0);

        // 7) 环形模式：N=4 连续写 9 块（36 KB），块地址按 4 块回绕，每块一拍 O_block_done。
        reset_dut;
        do_ring_start(BASE_RING, 32'd4096, 32'd4);
        wait_blocks(9, 200000, "ring 9 blocks");
        check(blk_seq == 32'd9, "ring BLK_SEQ=9");
        check(block_pulses == 9, "ring one block_done per block");
        check(done_pulses == 0, "ring no single-mode DONE");
        check(error_seen == 0, "ring no error");
        check(busy == 1'b1, "ring stays busy across blocks");
        check(written == 32'd36864, "ring O_written cumulative");
        check(u_mem.S_byte_count >= 36864, "ring bytes on AXI");
        check(u_mem.S_beat_count >= 4608, "ring 9x512 beats");
        check(u_mem.S_error_count == 0, "ring AXI protocol clean");
        abort_ring("ring abort");
        check(error_code == 3'd3, "ring abort ERRCODE=3");
        check(blk_seq == 32'd9, "ring abort keeps BLK_SEQ");
        check(written == 32'd36864, "ring abort no extra bytes");
        check(u_mem.S_burst_count >= 288 && u_mem.S_burst_count <= 290, "ring burst count");
        check_ring_addresses(BASE_RING_A, 9, 4);
        check(u_mem.S_wdata[0] === ramp_word(0), "ring data starts at word 0");
        check(u_mem.S_wdata[2048] === ramp_word(2048), "ring data continues across blocks");
        check(u_mem.S_wdata[4607] === ramp_word(4607), "ring data runs over 9 blocks");

        // 8) 环形参数非法：N=0 / N=257 / 基地址超出 4 GiB 启动即拒绝。
        expect_bad_ring(32'd0);
        expect_bad_ring(32'd257);
        reset_dut;
        do_ring_start(64'h0000_0001_0000_0000, 32'd4096, 32'd4);
        wait_finish(200, "bad ring base");
        check(error_code == 3'd1, "bad ring base ERRCODE=1");
        check(u_mem.S_aw_count == 0, "bad ring base no AW");

        // 9) START 清零 BLK_SEQ；N=2 正常回绕（第 2 块起回到块 0）。
        reset_dut;
        do_ring_start(BASE_RING, 32'd4096, 32'd2);
        wait_blocks(3, 200000, "ring N=2");
        check(blk_seq == 32'd3, "ring N=2 BLK_SEQ=3");
        check(u_mem.S_burst_addr_log[32] === (BASE_RING_A + 33'd4096), "ring N=2 second block address");
        check(u_mem.S_burst_addr_log[64] === BASE_RING_A, "ring N=2 wraps back to block 0");
        abort_ring("ring N=2 abort");
        do_ring_start(BASE_RING, 32'd4096, 32'd2);
        repeat(3) @(negedge clk);
        check(blk_seq == 32'd0, "ring START clears BLK_SEQ");
        wait_blocks(1, 200000, "ring N=2 restart");
        check(blk_seq == 32'd1, "ring restart BLK_SEQ=1");
        abort_ring("ring N=2 abort 2");

        // 10) 环形传输后单次模式仍按原行为工作。
        reset_dut;
        do_start(BASE_TEST, 32'd4096, 1'b1);
        wait_finish(20000, "single after ring");
        wait_idle(1000, "single after ring");
        check(done_pulses > 0, "single after ring DONE");
        check(error_seen == 0, "single after ring no error");
        check(busy == 1'b0, "single after ring releases BUSY");
        check(written == 32'd4096, "single after ring O_written=4096");
        check(u_mem.S_burst_count == 32, "single after ring 32 bursts");
        check(blk_seq == 32'd0, "single mode BLK_SEQ stays 0");
        check(block_pulses == 0, "single mode no block_done");
        check_words(BASE_TEST_A, 512, 1);

        // 11) I_stop 优雅停止（环形 N=4）：完成当前突发后释放 BUSY，不置 ERRCODE / DONE，
        //     BLK_SEQ 保留，不再发起突发；停止后可重新 START。
        reset_dut;
        do_ring_start(BASE_RING, 32'd4096, 32'd4);
        wait_blocks(2, 200000, "ring stop pre");
        stop_seq_mark = blk_seq;
        stop_wr_mark = written;
        do_stop;
        repeat(2) @(negedge clk);
        check(busy == 1'b1, "ring stop keeps BUSY until burst completes");
        wait_idle(4000, "ring stop");
        check(error_seen == 0 && error_code == 3'd0, "ring stop no error");
        check(done_pulses == 0, "ring stop no DONE");
        check(busy == 1'b0, "ring stop releases BUSY");
        check(blk_seq == stop_seq_mark && blk_seq == 32'd2, "ring stop keeps BLK_SEQ");
        check(written >= stop_wr_mark && written < 32'd12288, "ring stop ends inside block 3");
        check((written & 32'd127) == 32'd0, "ring stop at burst boundary");
        check(u_mem.S_byte_count == written, "ring stop completes current burst");
        check(u_mem.S_aw_count == u_mem.S_burst_count, "ring stop no half burst");
        check(u_mem.S_error_count == 0, "ring stop AXI protocol clean");
        stop_bursts = u_mem.S_burst_count;
        stop_beats_mark = u_mem.S_beat_count;
        repeat(64) @(negedge clk);
        check(busy == 1'b0, "ring stop stays idle");
        check(u_mem.S_burst_count == stop_bursts, "ring stop issues no new AW burst");
        check(u_mem.S_beat_count == stop_beats_mark, "ring stop issues no new W beat");
        do_start(BASE_TEST, 32'd64, 1'b1);
        wait_finish(2000, "single after ring stop");
        wait_idle(100, "single after ring stop");
        check(done_pulses > 0, "single after ring stop DONE");
        check(written == 32'd64, "single after ring stop O_written=64");
        check(error_seen == 0, "single after ring stop no error");

        // 12) I_stop 优雅停止（单次模式）：不报错、不置 DONE，O_written 停在突发边界，
        //     停止后可重新 START 并正常完成。
        reset_dut;
        do_start(BASE_TEST, 32'd4096, 1'b1);
        k = 0;
        while (k < 4000 && u_mem.S_beat_count < 40) begin @(negedge clk); k = k + 1; end
        check(u_mem.S_beat_count >= 40, "single stop transfer started");
        stop_wr_mark = written;
        do_stop;
        repeat(2) @(negedge clk);
        check(busy == 1'b1, "single stop keeps BUSY until burst completes");
        wait_idle(4000, "single stop");
        check(error_seen == 0 && error_code == 3'd0, "single stop no error");
        check(done_pulses == 0, "single stop no DONE");
        check(busy == 1'b0, "single stop releases BUSY");
        check(written >= stop_wr_mark, "single stop keeps completed bytes");
        check(written > 32'd0 && written < 32'd4096, "single stop is partial");
        check((written & 32'd127) == 32'd0, "single stop at burst boundary");
        check(u_mem.S_byte_count == written, "single stop bytes match O_written");
        check(u_mem.S_aw_count == u_mem.S_burst_count, "single stop no half burst");
        check(u_mem.S_error_count == 0, "single stop AXI protocol clean");
        stop_bursts = u_mem.S_burst_count;
        repeat(64) @(negedge clk);
        check(!busy && !done && error == 3'd0, "single stop stays stopped");
        check(u_mem.S_burst_count == stop_bursts, "single stop issues no new burst");
        do_start(BASE_TEST, 32'd64, 1'b1);
        wait_finish(2000, "single restart after stop");
        wait_idle(100, "single restart after stop");
        check(done_pulses > 0, "single restart DONE");
        check(written == 32'd64, "single restart O_written=64");
        check(error_seen == 0, "single restart no error");

        // 15) 外部流无数据：数据就绪前不得发 AWVALID。
        //     回归：AW 先握手、W 等数据会把 PS 侧 HP 写通道长期挂住（实板表现为整机复位或
        //     mmc 总线超时）；无在途事务时 I_stop 必须能直接干净退出。
        reset_dut;
        aw_mark = u_mem.S_aw_count;
        do_start(BASE_STREAM, 32'd4096, 1'b0);
        repeat(200) @(negedge clk);
        check(busy == 1'b1, "no-data starts busy");
        check(m_awvalid === 1'b0, "no-data AWVALID stays low");
        check(m_wvalid === 1'b0, "no-data WVALID stays low");
        check(u_mem.S_aw_count == aw_mark, "no-data opens no AXI transaction");
        do_stop;
        wait_idle(200, "no-data stop");
        check(error_seen == 0 && error_code == 3'd0, "no-data stop no error");
        check(done_pulses == 0, "no-data stop no DONE");
        check(u_mem.S_aw_count == aw_mark, "no-data stop issues no burst");


        // Shared HP1 can stall AW for many cycles. ABORT must retain an
        // already asserted AWVALID and drain the buffered burst after grant.
        reset_dut;
        mem_aw_stall = 1;
        do_start(BASE_TEST, 32'd1024, 1'b1);
        k = 0;
        while (k < 200 && !m_awvalid) begin @(negedge clk); k = k + 1; end
        check(m_awvalid && !m_awready, "abort AW stalled");
        stalled_addr = m_awaddr;
        abort = 1;
        @(negedge clk); abort = 0;
        repeat (12) begin
            @(negedge clk);
            check(m_awvalid && m_awaddr == stalled_addr && m_awlen == 15,
                  "abort holds pending AW");
        end
        check(!busy && error == 3, "abort reports while draining");
        check(u_mem.S_aw_count == 0, "abort has no premature AW handshake");
        mem_aw_stall = 0;
        repeat (96) @(negedge clk);
        check(u_mem.S_aw_count == 1 && u_mem.S_beat_count == 16 &&
              u_mem.S_burst_count == 1, "abort drains one pending burst");
        check(u_mem.S_error_count == 0 && written == 0 && !done,
              "abort drain protocol and counters");
        check_words(BASE_TEST_A, 16, 1);
        do_start(BASE_TEST, 32'd64, 1'b1);
        wait_idle(2000, "restart after stalled abort");
        check(written == 64, "restart after stalled abort completes");

        // Graceful STOP has the same AW hold obligation, but preserves the
        // completed-byte count and must not produce an abort error.
        reset_dut;
        mem_aw_stall = 1;
        do_start(BASE_TEST, 32'd1024, 1'b1);
        k = 0;
        while (k < 200 && !m_awvalid) begin @(negedge clk); k = k + 1; end
        check(m_awvalid && !m_awready, "stop AW stalled");
        stalled_addr = m_awaddr;
        do_stop;
        repeat (12) begin
            @(negedge clk);
            check(m_awvalid && m_awaddr == stalled_addr && m_awlen == 15,
                  "stop holds pending AW");
        end
        check(busy, "stop waits for pending AW");
        mem_aw_stall = 0;
        wait_idle(2000, "stop with stalled AW");
        check(written == 128 && error == 0 && !done, "stop finishes pending burst");
        check(u_mem.S_aw_count == 1 && u_mem.S_burst_count == 1,
              "stop issues no second burst");

        // ABORT while W is stalled must not replace a presented payload.
        reset_dut;
        do_start(BASE_TEST, 32'd1024, 1'b1);
        k = 0;
        while (k < 200 && u_mem.S_beat_count < 3) begin @(negedge clk); k = k + 1; end
        mem_stall = 1;
        @(negedge clk);
        check(m_wvalid && !m_wready, "abort W stalled");
        stalled_data = m_wdata;
        abort = 1;
        @(negedge clk); abort = 0;
        repeat (12) begin
            @(negedge clk);
            check(m_wvalid && m_wdata == stalled_data, "abort holds pending W");
        end
        mem_stall = 0;
        repeat (96) @(negedge clk);
        check(u_mem.S_aw_count == 1 && u_mem.S_beat_count == 16 &&
              u_mem.S_burst_count == 1, "abort drains stalled W burst");
        check_words(BASE_TEST_A, 16, 1);

        $display("RESULT tb_sensor_dma PASS=%0d FAIL=0", checks);
        $finish;
    end

    initial begin #20000000; $fatal(1, "sensor_dma timeout"); end
endmodule
