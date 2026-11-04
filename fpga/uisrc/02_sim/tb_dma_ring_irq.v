`timescale 1ns/1ps
// 环形 DMA + 块完成中断的寄存器集成自检：
//   axi_lite_reg（0xA8/0xAC/0xB0/0xB4、DMA_CTRL[3]）+ sensor_dma（测试 ramp）
//   + AXI4 写通道从机内存模型，AXI-Lite 主机用任务驱动。
// 覆盖：
//   1) 复位值：DMA_BLK_SEQ=0、DMA_IRQ=0、DMA_BLOCKS=0、PL_TS_NOW=0；
//   2) RO 寄存器写回 SLVERR：0xA8、0xB0；0xAD 未对齐 → SLVERR；
//   3) DMA_BLOCKS 写入回读、PL_TS_NOW 读回 I_pl_ts；
//   4) DMA_CTRL[3]=RING 使能后按 DMA_BLOCKS 连续写块，DMA_BLK_SEQ 递增、0xA8 读回；
//   5) 块完成置起 DMA_IRQ[16] PEND，O_dma_irq 为 LEVEL_HIGH：PEND 置起后线路
//      持续为高（不是单拍脉冲），[8] W1C 清挂起后拉低，每清一次计一个上升沿；
//   6) 使能位 DMA_IRQ[0]=0 时 PEND 仍驱动线路（PS 驱动在 [0]=0 时也执行 W1C），
//      关使能状态下 W1C 依然有效；重新使能不产生多余电平；
//   7) DMA_CTRL[4]=STOP（自清）优雅停止：完成当前突发后释放 BUSY，ERRCODE 保持 0、
//      BLK_SEQ 保留、无 DONE、不再发起突发；停止后可重新 START；
//   8) ABORT 后 DMA_STATUS.ERRCODE=3、BUSY=0、RING 读回 0、BLK_SEQ 停止；
//   9) RING=0 的单次模式（TEST|START）不改变 BLK_SEQ / 不产生块完成、线路保持低。
//
// 约定：O_dma_irq 为 DMA_IRQ[16] 的 PEND 电平（LEVEL_HIGH，接 f2p_irq[3]）；
//       DMA_IRQ 写 [8] 时用 WSTRB 字节 1，写 [0] 时用字节 0；
//       写 0x90 会同时锁存 TEST/RING，因此 STOP 写须携带运行中的模式位。
module tb_dma_ring_irq;
    localparam [63:0] BASE   = 64'h0000_0000_1F00_8000;   // 4 KiB 对齐
    localparam [32:0] BASE_A = 33'h1F00_8000;

    reg clk = 0;
    always #5 clk = ~clk;
    reg rstn = 0;

    // AXI-Lite 主机
    reg [15:0] awaddr = 0;
    reg awvalid = 0;
    wire awready;
    reg [31:0] wdata = 0;
    reg [3:0] wstrb = 0;
    reg wvalid = 0;
    wire wready;
    wire [1:0] bresp;
    wire bvalid;
    reg bready = 0;
    reg [15:0] araddr = 0;
    reg arvalid = 0;
    wire arready;
    wire [31:0] rdata;
    wire [1:0] rresp;
    wire rvalid;
    reg rready = 0;
    reg [31:0] rd;

    // 寄存器 → DMA / DMA → 寄存器
    wire dma_start, dma_abort, dma_stop, dma_test, dma_ring;
    wire [63:0] dma_base;
    wire [31:0] dma_bytes, dma_blocks, dma_written, dma_blk_seq;
    wire dma_busy, dma_done, dma_block_done, irq;
    wire [2:0] dma_error;
    reg [31:0] pl_ts = 0;

    // sensor_dma 的 AXI4 主口
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

    reg mem_clear = 0;

    integer checks = 0;
    integer irq_rise = 0, block_events = 0, done_pulses = 0;
    reg last_irq = 0;
    integer events_mark = 0, axi_mark = 0, beat_mark = 0, irq_mark = 0;

    axi_lite_reg #(.EXTERNAL_FRAME(1), .EXTERNAL_FFT(1), .WITH_FEATURES(0)) X_regs(
        .I_clk(clk), .I_rstn(rstn),
        .I_awaddr(awaddr), .I_awvalid(awvalid), .O_awready(awready),
        .I_wdata(wdata), .I_wstrb(wstrb), .I_wvalid(wvalid), .O_wready(wready),
        .O_bresp(bresp), .O_bvalid(bvalid), .I_bready(bready),
        .I_araddr(araddr), .I_arvalid(arvalid), .O_arready(arready),
        .O_rdata(rdata), .O_rresp(rresp), .O_rvalid(rvalid), .I_rready(rready),
        .I_busy(1'b0), .I_overflow(1'b0), .I_done(1'b0), .I_state(8'd0),
        .I_sample(16'd0), .I_sample_valid(1'b0),
        .I_temp(16'sd0), .I_acc_x(16'sd0), .I_acc_y(16'sd0), .I_acc_z(16'sd0),
        .I_current(16'sd0),
        .I_temp_valid(1'b0), .I_acc_valid(1'b0), .I_current_valid(1'b0),
        .I_faults(8'd0), .I_feature_valid(4'd0), .I_features(1792'd0),
        .I_feature_flags(8'd0), .O_enable(), .O_irq_enable(), .O_soft_reset(),
        .O_irq(), .I_frame(256'd0),
        .I_fft_sequence(32'd0), .I_fft_bin(32'd0), .I_fft_flags(32'd0),
        .I_fft_power(64'd0),
        .O_dma_start(dma_start), .O_dma_abort(dma_abort), .O_dma_stop(dma_stop),
        .O_dma_base(dma_base), .O_dma_bytes(dma_bytes), .O_dma_test(dma_test),
        .I_dma_busy(dma_busy), .I_dma_done(dma_done), .I_dma_error(dma_error),
        .I_dma_written(dma_written), .I_dma_overflow(1'b0),
        .O_dma_ring(dma_ring), .O_dma_blocks(dma_blocks),
        .I_dma_blk_seq(dma_blk_seq), .I_dma_block_done(dma_block_done),
        .O_dma_irq(irq), .I_pl_ts(pl_ts),
        // v1.2 转速窗口本 TB 不覆盖，输入固定 0。
        .I_speed_raw(32'sd0), .I_speed_rpm(32'sd0), .I_speed_status(32'd0),
        .I_speed_count(32'd0), .I_speed_ts(32'd0), .I_speed_seq(32'd0),
        .I_speed_err_count(32'd0));

    sensor_dma X_dma(
        .I_clk(clk), .I_rstn(rstn),
        .I_start(dma_start), .I_abort(dma_abort), .I_stop(dma_stop),
        .I_base(dma_base), .I_bytes(dma_bytes), .I_test(dma_test),
        .I_ring(dma_ring), .I_blocks(dma_blocks),
        .O_busy(dma_busy), .O_done(dma_done), .O_error(dma_error),
        .O_written(dma_written), .O_block_done(dma_block_done), .O_blk_seq(dma_blk_seq),
        .I_s_data(64'd0), .I_s_valid(1'b0), .O_s_ready(),
        .M_axi_awaddr(m_awaddr), .M_axi_awlen(m_awlen), .M_axi_awsize(m_awsize),
        .M_axi_awburst(m_awburst), .M_axi_awvalid(m_awvalid), .M_axi_awready(m_awready),
        .M_axi_wdata(m_wdata), .M_axi_wstrb(m_wstrb), .M_axi_wlast(m_wlast),
        .M_axi_wvalid(m_wvalid), .M_axi_wready(m_wready),
        .M_axi_bresp(m_bresp), .M_axi_bvalid(m_bvalid), .M_axi_bready(m_bready));

    axi_slave_mem_model #(.MEM_WORDS(8192), .MAX_BURSTS(1024)) u_mem(
        .I_clk(clk), .I_rstn(rstn), .I_clear(mem_clear),
        .I_w_stall(1'b0), .I_bresp(2'b00),
        .I_awaddr(m_awaddr), .I_awlen(m_awlen), .I_awsize(m_awsize),
        .I_awburst(m_awburst), .I_awvalid(m_awvalid), .O_awready(m_awready),
        .I_wdata(m_wdata), .I_wstrb(m_wstrb), .I_wlast(m_wlast),
        .I_wvalid(m_wvalid), .O_wready(m_wready),
        .O_bresp(m_bresp), .O_bvalid(m_bvalid), .I_bready(m_bready));

    // 中断上升沿、块完成与 DONE 计数监测。
    always @(posedge clk) if (rstn) begin
        if (irq && !last_irq) irq_rise = irq_rise + 1;
        last_irq = irq;
        if (dma_block_done) block_events = block_events + 1;
        if (dma_done) done_pulses = done_pulses + 1;
    end

    task check(input bit good, input [255:0] name);
        begin
            checks = checks + 1;
            if (!good) $fatal(1, "FAIL %s check=%0d", name, checks);
        end
    endtask

    // 连续若干拍采样中断电平；negedge 采样避开与 DUT 同时序竞争。
    task check_irq_level(input bit level, input integer cycles, input [255:0] name);
        integer j, bad;
        begin
            bad = 0;
            for (j = 0; j < cycles; j = j + 1) begin
                @(negedge clk);
                if (irq !== level) bad = bad + 1;
            end
            check(bad == 0, name);
        end
    endtask

    //================ AXI-Lite 主机任务（与 tb_axi_lite_reg 同风格）================
    task send_aw(input [15:0] addr);
        begin
            @(negedge clk); awaddr = addr; awvalid = 1;
            @(posedge clk); while (!awready) @(posedge clk);
            @(negedge clk); awvalid = 0;
        end
    endtask
    task send_w(input [31:0] data, input [3:0] strb);
        begin
            @(negedge clk); wdata = data; wstrb = strb; wvalid = 1;
            @(posedge clk); while (!wready) @(posedge clk);
            @(negedge clk); wvalid = 0;
        end
    endtask
    task write_reg(input [15:0] addr, input [31:0] data, input [3:0] strb,
                   input integer order, input [1:0] resp);
        begin
            if (order < 0) begin send_aw(addr); repeat(3) @(negedge clk); send_w(data, strb); end
            else if (order > 0) begin send_w(data, strb); repeat(3) @(negedge clk); send_aw(addr); end
            else fork send_aw(addr); send_w(data, strb); join
            while (!bvalid) @(negedge clk);
            repeat(4) begin
                check(bvalid && bresp === resp, "B stall/response");
                check(!awready && !wready, "no overwrite stalled B");
                @(negedge clk);
            end
            bready = 1; @(negedge clk); bready = 0;
        end
    endtask
    task read_reg(input [15:0] addr, input [1:0] resp, output [31:0] data);
        begin
            @(negedge clk); araddr = addr; arvalid = 1;
            @(posedge clk); while (!arready) @(posedge clk);
            @(negedge clk); arvalid = 0;
            while (!rvalid) @(negedge clk);
            data = rdata;
            repeat(4) begin
                check(rvalid && rresp === resp && rdata === data, "R stability/response");
                @(negedge clk);
            end
            rready = 1; @(negedge clk); rready = 0;
        end
    endtask
    task expect_reg(input [15:0] addr, input [31:0] value);
        begin read_reg(addr, 0, rd); check(rd === value, "register value"); end
    endtask

    task wait_done(input integer n, input integer limit, input [255:0] name);
        integer j;
        begin
            j = 0;
            while (j < limit && done_pulses < n) begin @(negedge clk); j = j + 1; end
            if (done_pulses < n) $fatal(1, "FAIL %s timeout", name);
            repeat(4) @(negedge clk);
        end
    endtask

    task wait_blocks(input integer n, input integer limit, input [255:0] name);
        integer j;
        begin
            j = 0;
            while (j < limit && block_events < n) begin @(negedge clk); j = j + 1; end
            if (block_events < n) $fatal(1, "FAIL %s timeout", name);
            repeat(2) @(negedge clk);
        end
    endtask

    initial begin
        repeat(4) @(negedge clk);
        rstn = 1;
        repeat(4) @(negedge clk);
        mem_clear = 1; repeat(2) @(negedge clk); mem_clear = 0;

        // 1) 复位值。
        expect_reg(16'hA8, 32'd0);
        expect_reg(16'hAC, 32'd0);
        expect_reg(16'hB0, 32'd0);
        expect_reg(16'hB4, 32'd0);
        check(irq === 1'b0, "reset leaves O_dma_irq low");

        // 2) RO 寄存器写回 SLVERR，未对齐地址同样 SLVERR。
        write_reg(16'hA8, 32'd1, 4'hf, 0, 2'b10);
        write_reg(16'hB0, 32'd1, 4'hf, 0, 2'b10);
        write_reg(16'hAD, 32'd1, 4'hf, 0, 2'b10);
        // 3) 可写项：DMA_BLOCKS、PL_TS_NOW 由端口直入。
        write_reg(16'hB4, 32'd4, 4'hf, 0, 2'b00);
        expect_reg(16'hB4, 32'd4);
        pl_ts = 32'h1234_5678;
        expect_reg(16'hB0, 32'h1234_5678);
        pl_ts = 32'h89ab_cdef;
        expect_reg(16'hB0, 32'h89ab_cdef);

        // 4) 使能块完成中断，配置 4 KiB 环并启动（RING|TEST|START）。
        write_reg(16'hAC, 32'd1, 4'h1, 0, 2'b00);
        expect_reg(16'hAC, 32'd1);
        write_reg(16'h94, BASE[31:0], 4'hf, 0, 2'b00);
        write_reg(16'h98, BASE[63:32], 4'hf, 0, 2'b00);
        write_reg(16'h9C, 32'd4096, 4'hf, 0, 2'b00);
        expect_reg(16'h9C, 32'd4096);
        write_reg(16'h90, 32'h0000_000d, 4'h1, 0, 2'b00);
        read_reg(16'h90, 0, rd);
        check(rd[3] === 1'b1, "DMA_CTRL RING readback");
        check(rd[2] === 1'b1, "DMA_CTRL TEST readback");
        check(rd[8] === 1'b1 || dma_busy === 1'b1, "ring transfer started");

        // 5) 第 1 块完成：BLK_SEQ 递增、PEND 置位；O_dma_irq 为 LEVEL_HIGH
        //    （PEND 置起后保持高，不是单拍脉冲）。
        wait_blocks(1, 20000, "first block");
        check(dma_blk_seq >= 32'd1, "BLK_SEQ advanced");
        events_mark = block_events;
        read_reg(16'hA8, 0, rd);
        check(rd === dma_blk_seq || rd + 32'd1 === dma_blk_seq, "0xA8 reads BLK_SEQ");
        read_reg(16'hAC, 0, rd);
        check(rd[0] === 1'b1, "IRQ enable readback");
        check(rd[16] === 1'b1, "IRQ pending after block");
        check(rd[31:17] === 15'd0 && rd[15:9] === 7'd0, "DMA_IRQ reserved bits 0");
        check(irq === 1'b1, "O_dma_irq follows PEND level");
        check(irq_rise === 1, "one IRQ rising edge per cleared block");
        check_irq_level(1'b1, 32, "O_dma_irq holds high while PEND set");
        check(u_mem.S_byte_count >= 4096, "ring block written to AXI");
        check(u_mem.S_burst_addr_log[0] === BASE_A, "ring block 0 base address");
        check(u_mem.S_burst_count >= 32, "ring block uses 32 bursts");

        // 6) W1C 清挂起：PEND=0、O_dma_irq 拉低；若期间又完成一块则重新置起。
        events_mark = block_events;
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);
        read_reg(16'hAC, 0, rd);
        check(rd[0] === 1'b1, "W1C keeps IRQ enable");
        if (block_events == events_mark) begin
            check(rd[16] === 1'b0, "W1C clears pending");
            check(irq === 1'b0, "W1C drops O_dma_irq level");
            check_irq_level(1'b0, 32, "O_dma_irq stays low after W1C");
        end else begin
            check(rd[16] === 1'b1, "pending re-set by new block");
            check(irq === 1'b1, "new block re-raises O_dma_irq");
        end

        // 7) 第 2 块：再次置起并拉高线路；线路由 PEND 驱动，不受 [0] 门控
        //    （PS 用整字写清 pending，写 0x100 会顺带清 [0]），W1C 始终有效。
        wait_blocks(2, 20000, "second block");
        check(irq === 1'b1, "second block raises level");
        check(irq_rise === 2, "second block second rising edge");
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);      // 清挂起
        check(irq === 1'b0, "level low after second W1C");
        write_reg(16'hAC, 32'd0, 4'h1, 0, 2'b00);        // 关中断使能
        events_mark = block_events;
        wait_blocks(3, 20000, "third block disabled");
        check(block_events > events_mark, "block completes while disabled");
        read_reg(16'hAC, 0, rd);
        check(rd[0] === 1'b0, "IRQ enable cleared");
        check(rd[16] === 1'b1, "pending set while disabled");
        check(irq === 1'b1, "PEND drives O_dma_irq regardless of [0]");
        check(irq_rise === 3, "level rises once while disabled");
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);      // 关使能状态下 W1C
        check(irq === 1'b0, "W1C works while disabled");
        write_reg(16'hAC, 32'd1, 4'h1, 0, 2'b00);        // 恢复使能
        check(irq === 1'b0, "no spurious level after re-enable");

        // 8) DMA_CTRL[4]=STOP（自清）优雅停止：完成当前突发后释放 BUSY，
        //    ERRCODE 保持 0、BLK_SEQ 保留、无 DONE、不再发起突发。
        wait_blocks(4, 20000, "fourth block");
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);      // 清挂起
        check(irq === 1'b0, "level low before stop");
        events_mark = block_events;
        write_reg(16'h90, 32'h0000_001c, 4'h1, 0, 2'b00); // TEST|RING|STOP
        repeat(400) @(negedge clk);
        check(dma_busy === 1'b0, "STOP releases BUSY");
        check(dma_error === 3'd0, "STOP keeps ERRCODE=0");
        check(irq === 1'b0, "STOP leaves level low");
        read_reg(16'h90, 0, rd);
        check(rd[8] === 1'b0, "0x90 BUSY=0 after stop");
        check(rd[9] === 1'b0, "ring mode no DONE after stop");
        check(rd[4] === 1'b0, "STOP self-clear readback");
        check(rd[3] === 1'b1, "STOP write keeps RING");
        read_reg(16'hA0, 0, rd);
        check(((rd & 32'h000e_0000) >> 17) === 3'd0, "STOP ERRCODE=0");
        read_reg(16'hA8, 0, rd);
        check(rd === 32'd4 && dma_blk_seq === 32'd4, "STOP keeps BLK_SEQ=4");
        read_reg(16'hA4, 0, rd);
        check(rd >= 32'd16384, "STOP keeps completed block bytes");
        check(rd < 32'd20480, "STOP ends inside block 5");
        check((rd & 32'd127) === 32'd0, "STOP at burst boundary");
        check(u_mem.S_byte_count === rd, "STOP byte count matches O_written");
        check(u_mem.S_aw_count === u_mem.S_burst_count, "STOP no half burst");
        check(block_events === events_mark, "STOP emits no block_done");
        axi_mark = u_mem.S_burst_count;
        beat_mark = u_mem.S_beat_count;
        repeat(64) @(negedge clk);
        check(u_mem.S_burst_count === axi_mark, "STOP issues no new AW burst");
        check(u_mem.S_beat_count === beat_mark, "STOP issues no new W beat");
        check(!dma_busy && !dma_done && dma_error === 3'd0, "STOP stays stopped");

        // 9) 停止后重新 START 继续工作；ABORT 仍按中止处理（ERRCODE=3）。
        //    block_events 为累计值：先记基准，再等新增两块。
        events_mark = block_events;
        write_reg(16'h90, 32'h0000_000d, 4'h1, 0, 2'b00); // TEST|RING|START
        wait_blocks(events_mark + 2, 20000, "restart after stop");
        check(dma_blk_seq >= 32'd2, "restart BLK_SEQ");
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);      // 清挂起
        write_reg(16'h90, 32'h0000_0002, 4'h1, 0, 2'b00); // ABORT
        repeat(100) @(negedge clk);
        check(dma_busy === 1'b0, "ABORT releases BUSY");
        read_reg(16'hA0, 0, rd);
        check(((rd & 32'h000e_0000) >> 17) === 3'd3, "ABORT ERRCODE=3");
        read_reg(16'h90, 0, rd);
        check(rd[8] === 1'b0, "0x90 BUSY=0 after abort");
        check(rd[9] === 1'b0, "ring mode no DONE");
        check(rd[3] === 1'b0, "DMA_CTRL RING cleared by 0x90 write");
        read_reg(16'hA8, 0, rd);
        check(rd === dma_blk_seq && rd >= 32'd2, "BLK_SEQ frozen after abort");
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);      // 清挂起
        check(irq === 1'b0, "level low after abort+W1C");
        events_mark = block_events;
        axi_mark = u_mem.S_burst_count;
        repeat(64) @(negedge clk);
        check(block_events === events_mark, "no block_done after abort");
        check(u_mem.S_burst_count === axi_mark, "no AXI burst after abort");

        // 10) RING=0 单次模式：不改 BLK_SEQ、不产生块完成、线路保持低。
        write_reg(16'hAC, 32'h100, 4'h2, 0, 2'b00);      // 清挂起
        write_reg(16'h9C, 32'd4096, 4'hf, 0, 2'b00);
        events_mark = block_events;
        irq_mark = irq_rise;
        write_reg(16'h90, 32'h0000_0005, 4'h1, 0, 2'b00); // TEST|START
        wait_done(1, 40000, "single mode transfer");
        read_reg(16'hA8, 0, rd);
        check(rd === dma_blk_seq, "single mode keeps BLK_SEQ");
        check(block_events === events_mark, "single mode no extra block_done");
        read_reg(16'hA4, 0, rd);
        check(rd === 32'd4096, "single mode writes 4 KiB");
        read_reg(16'hA0, 0, rd);
        // 约定 0x90 START「清 DONE 与 DMA_STATUS；ERROR 仅由复位清除」：上一轮 ABORT
        // 的 ERRCODE=3 保持到复位，单次模式不再改动它。
        check(((rd & 32'h000e_0000) >> 17) === 3'd3, "ERRCODE sticky until reset");
        read_reg(16'h90, 0, rd);
        check(rd[9] === 1'b1, "single mode DONE set");
        check(rd[8] === 1'b0, "single mode releases BUSY");
        check(dma_ring === 1'b0, "single mode RING shadow cleared");
        check(irq_rise === irq_mark, "single mode no IRQ edge");
        check(irq === 1'b0, "single mode leaves level low");

        $display("RESULT tb_dma_ring_irq PASS=%0d FAIL=0 IRQ_RISE=%0d", checks, irq_rise);
        $finish;
    end

    initial begin #10000000; $fatal(1, "dma ring irq timeout"); end
endmodule
