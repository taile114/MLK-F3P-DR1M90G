`timescale 1ns/1ps
// 转速通道（接口 v1.2，§14）自检：
//   * 0xB8/0xBC/0xC0/0xC4/0xC8/0xCC/0xD0 只读窗口、复位值与写拒绝；
//   * Modbus 主站经引脚级 UART BFM 驱动（1 Mbaud，每 bit 100 个采集时钟），
//     覆盖正转 / 反转 / 零速 / 错误保留最近有效值 / 异常码 / 饱和 / 恢复；
//   * 采样时刻来自采集域共同时基（10 ns/LSB），与 AXI 域 PL_TS 对齐；
//   * seq-字段-seq 读一致性；事件在 CDC 完成后才进入窗口；
//   * DMA 三字记录 {ts,CH=3,状态} / {rpm,CH=4,0} / {raw,CH=5,0} 的字段与顺序；
//   * 振动帧（CH=0/1/2）与转速记录互不打断，丢弃计入 DMA_STATUS.overflow；
//   * 软复位清零窗口。
// 采集域时钟 100 MHz、寄存器域 50 MHz；转速事件在测试后期用强驱动注入到
// cdc_mailbox 的 AXI 侧输出，使振动帧 / 转速记录的到达顺序可控。
module tb_speed_channel;
    localparam integer BIT_CLKS = 100;          // 1 Mbaud @ 100 MHz

    reg axi_clk = 0, sensor_clk = 0, rstn = 0;
    always #10 axi_clk = ~axi_clk;              // 50 MHz 寄存器域
    always #5  sensor_clk = ~sensor_clk;        // 100 MHz 采集域
    wire clk = sensor_clk;                      // BFM 采样时钟

    //------------------------------------------------------------------
    // AXI-Lite 主机
    //------------------------------------------------------------------
    reg [15:0] awaddr = 0; reg awvalid = 0; wire awready;
    reg [31:0] wdata = 0;  reg [3:0] wstrb = 0; reg wvalid = 0; wire wready;
    wire [1:0] bresp; wire bvalid; reg bready = 0;
    reg [15:0] araddr = 0; reg arvalid = 0; wire arready;
    wire [31:0] rdata; wire [1:0] rresp; wire rvalid; reg rready = 0;
    wire irq, dma_irq;
    reg [31:0] rd, rd2, rd_ts, rd_st, rd_rpm, rd_raw;
    reg [191:0] inj;
    reg [255:0] inj_frame;
    reg [90:0]  inj_raw;
    integer i, n, mark;

    //------------------------------------------------------------------
    // RS485 半双工总线：主站 DE 有效期间线上按主站自己的回波处理（从站静默）
    //------------------------------------------------------------------
    reg slave_tx = 1;
    wire tx, de;
    wire rs485_rx = de ? 1'b0 : slave_tx;

    wire sclk, cs, sdio, scl, sda;
    wire [3:0] led;

    // DMA 主口 → AXI 从机内存模型
    wire [32:0] m_awaddr; wire [7:0] m_awlen; wire [2:0] m_awsize; wire [1:0] m_awburst;
    wire m_awvalid, m_awready; wire [63:0] m_wdata; wire [7:0] m_wstrb;
    wire m_wlast, m_wvalid, m_wready; wire [1:0] m_bresp; wire m_bvalid, m_bready;

    sensor_soc_axi #(
        .SPEED_BAUD(1_000_000), .SPEED_STARTUP_MS(1),
        .SPEED_POLL_MS(1), .SPEED_TIMEOUT_MS(5)
    ) dut (
        .I_clk(axi_clk), .I_rstn(rstn), .I_sysclk(sensor_clk),
        .I_rs485_rx(rs485_rx), .O_rs485_tx(tx), .O_rs485_de(de),
        .I_uart_rx(1'b1), .I_int1(1'b0),
        .O_spi_sclk(sclk), .O_spi_cs_n(cs),
        .IO_spi_sdio(sdio), .IO_iic_scl(scl), .IO_iic_sda(sda),
        .O_led(led),
        .I_awaddr(awaddr), .I_awvalid(awvalid), .O_awready(awready),
        .I_wdata(wdata), .I_wstrb(wstrb), .I_wvalid(wvalid), .O_wready(wready),
        .O_bresp(bresp), .O_bvalid(bvalid), .I_bready(bready),
        .I_araddr(araddr), .I_arvalid(arvalid), .O_arready(arready),
        .O_rdata(rdata), .O_rresp(rresp), .O_rvalid(rvalid), .I_rready(rready),
        .M_axi_awaddr(m_awaddr), .M_axi_awlen(m_awlen), .M_axi_awsize(m_awsize),
        .M_axi_awburst(m_awburst), .M_axi_awvalid(m_awvalid), .M_axi_awready(m_awready),
        .M_axi_wdata(m_wdata), .M_axi_wstrb(m_wstrb), .M_axi_wlast(m_wlast),
        .M_axi_wvalid(m_wvalid), .M_axi_wready(m_wready),
        .M_axi_bresp(m_bresp), .M_axi_bvalid(m_bvalid), .M_axi_bready(m_bready),
        .O_irq(irq), .O_dma_irq(dma_irq));

    axi_slave_mem_model #(.MEM_WORDS(1024), .MAX_BURSTS(64)) u_mem (
        .I_clk(axi_clk), .I_rstn(rstn), .I_clear(1'b0), .I_w_stall(1'b0),
        .I_bresp(2'b00),
        .I_awaddr(m_awaddr), .I_awlen(m_awlen), .I_awsize(m_awsize),
        .I_awburst(m_awburst), .I_awvalid(m_awvalid), .O_awready(m_awready),
        .I_wdata(m_wdata), .I_wstrb(m_wstrb), .I_wlast(m_wlast),
        .I_wvalid(m_wvalid), .O_wready(m_wready),
        .O_bresp(m_bresp), .O_bvalid(m_bvalid), .I_bready(m_bready));

    `include "uart_bfm.vh"

    //------------------------------------------------------------------
    // AXI-Lite 事务
    //------------------------------------------------------------------
    task send_aw(input [15:0] addr);
        begin
            @(negedge axi_clk); awaddr=addr; awvalid=1;
            @(posedge axi_clk); while(!awready) @(posedge axi_clk);
            @(negedge axi_clk); awvalid=0;
        end
    endtask
    task send_w(input [31:0] data, input [3:0] strb);
        begin
            @(negedge axi_clk); wdata=data; wstrb=strb; wvalid=1;
            @(posedge axi_clk); while(!wready) @(posedge axi_clk);
            @(negedge axi_clk); wvalid=0;
        end
    endtask
    task write_reg(input [15:0] addr, input [31:0] data, input [3:0] strb,
                   input integer order, input [1:0] resp);
        begin
            if(order<0) begin send_aw(addr); repeat(3) @(negedge axi_clk); send_w(data,strb); end
            else if(order>0) begin send_w(data,strb); repeat(3) @(negedge axi_clk); send_aw(addr); end
            else fork send_aw(addr); send_w(data,strb); join
            while(!bvalid) @(negedge axi_clk);
            repeat(4) begin
                check(bvalid && bresp===resp, "AXI: B response/stability");
                check(!awready && !wready, "AXI: channel not reused during stall");
                @(negedge axi_clk);
            end
            bready=1; @(negedge axi_clk); bready=0;
        end
    endtask
    task read_reg(input [15:0] addr, input [1:0] resp, output [31:0] data);
        begin
            @(negedge axi_clk); araddr=addr; arvalid=1;
            @(posedge axi_clk); while(!arready) @(posedge axi_clk);
            @(negedge axi_clk); arvalid=0;
            while(!rvalid) @(negedge axi_clk);
            data=rdata;
            repeat(4) begin
                check(rvalid && rresp===resp && rdata===data, "AXI: R response/stability");
                @(negedge axi_clk);
            end
            rready=1; @(negedge axi_clk); rready=0;
        end
    endtask
    task expect_reg(input [15:0] addr, input [31:0] value);
        begin
            read_reg(addr, 2'b00, rd);
            if(rd !== value) $display("  expect_reg addr=%04x got=%08x want=%08x", addr, rd, value);
            check(rd===value, "reg value");
        end
    endtask

    //------------------------------------------------------------------
    // 转速事件 / 振动帧注入（DMA 打包仲裁用）
    //------------------------------------------------------------------
    task inject_speed(input [31:0] raw, input [31:0] rpm, input [31:0] count,
                      input [31:0] errc, input [31:0] ts, input [7:0] flags,
                      input [7:0] code, input [7:0] exc);
        begin
            inj = {raw, rpm, count, errc, ts, flags, code, exc, 8'h00};
            @(negedge axi_clk);
            force dut.S_speed_axi = inj;
            force dut.S_speed_axi_valid = 1'b1;
            @(negedge axi_clk);
            release dut.S_speed_axi_valid;
            release dut.S_speed_axi;
        end
    endtask
    // 连续两个事件：第二条在上一条记录未发完时到达，用于检查丢弃标志。
    task inject_speed_pair(input [191:0] first, input [191:0] second);
        begin
            @(negedge axi_clk);
            force dut.S_speed_axi = first;
            force dut.S_speed_axi_valid = 1'b1;
            @(negedge axi_clk);
            force dut.S_speed_axi = second;
            @(negedge axi_clk);
            release dut.S_speed_axi_valid;
            release dut.S_speed_axi;
        end
    endtask
    task inject_frame(input [31:0] ts, input [15:0] x, input [15:0] y, input [15:0] z);
        begin
            inj_frame = 256'd0;
            inj_frame[95:64] = ts;      // 振动 DMA 字的时间戳
            inj_frame[63:48] = x;
            inj_frame[47:32] = y;
            inj_frame[31:16] = z;
            @(negedge axi_clk);
            force dut.S_frame_axi = inj_frame;
            force dut.S_frame_axi_valid = 1'b1;
            @(negedge axi_clk);
            release dut.S_frame_axi_valid;
            release dut.S_frame_axi;
        end
    endtask
    task inject_acc(input [15:0] x, input [15:0] y, input [15:0] z);
        begin
            inj_raw = 91'd0;
            inj_raw[2:0] = 3'b010;      // acc_valid
            inj_raw[66:51] = x;
            inj_raw[50:35] = y;
            inj_raw[34:19] = z;
            @(negedge axi_clk);
            force dut.S_raw_axi = inj_raw;
            force dut.S_raw_axi_valid = 1'b1;
            @(negedge axi_clk);
            release dut.S_raw_axi_valid;
            release dut.S_raw_axi;
        end
    endtask

    //------------------------------------------------------------------
    // Modbus 事务与事件等待
    //------------------------------------------------------------------
    task good_read(input [31:0] value);
        begin
            receive_request(8'd3);
            prepare_response(value);
            send_response(9);
        end
    endtask
    // 等待已发布事件计数到达期望值（每秒轮询 0xCC，超时即失败）。
    task wait_seq(input [31:0] want);
        begin
            n = 0; rd = 32'hffffffff;
            while (n < 4000 && rd !== want) begin
                read_reg(16'h00cc, 2'b00, rd);
                n = n + 1;
            end
            check(rd === want, "seq increments per event");
        end
    endtask
    task start_dma(input [31:0] base, input [31:0] bytes);
        begin
            write_reg(16'h0094, base, 4'hf, 0, 2'b00);
            write_reg(16'h0098, 32'd0, 4'hf, 0, 2'b00);
            write_reg(16'h009c, bytes, 4'hf, 0, 2'b00);
            write_reg(16'h0090, 32'd1, 4'hf, 0, 2'b00);   // START，测试模式=0、环形=0
            n = 0; rd = 32'd0;
            while (n < 200 && rd[8] !== 1'b1) begin       // 0x90[8] = BUSY
                read_reg(16'h0090, 2'b00, rd);
                n = n + 1;
            end
            check(rd[8] === 1'b1, "DMA: busy after start");
        end
    endtask
    task wait_dma_done;
        begin
            n = 0; rd = 32'd0;
            while (n < 4000 && rd[9] !== 1'b1) begin      // 0x90[9] = DONE
                read_reg(16'h0090, 2'b00, rd);
                n = n + 1;
            end
            check(rd[9] === 1'b1, "DMA: done");
        end
    endtask

    initial begin
        if($test$plusargs("vcd")) begin
            $dumpfile("speed_channel.vcd"); $dumpvars(0, tb_speed_channel);
        end
        awvalid=0; wvalid=0; bready=0; arvalid=0; rready=0; slave_tx=1;
        repeat(20) @(negedge axi_clk);
        rstn=1;
        repeat(20) @(negedge axi_clk);

        //=============================================================
        // 1. 复位值、ID (v1.2)、只读窗口
        //=============================================================
        $display("== phase 1: reset values / ID / RO window");
        expect_reg(16'h0000, 32'h504c0103);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c0, 32'd0);
        expect_reg(16'h00c4, 32'd0);
        expect_reg(16'h00c8, 32'd0);
        expect_reg(16'h00cc, 32'd0);
        expect_reg(16'h00d0, 32'd0);
        for(i=16'hb8; i<=16'hd0; i=i+4)
            write_reg(i[15:0], 32'hffffffff, 4'hf, 0, 2'b10);   // 只读 → SLVERR
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00c0, 32'd0);
        write_reg(16'h0004, 32'h0000_0001, 4'hf, 0, 2'b00);     // CTRL.EN=1

        //=============================================================
        // 2. FC06 配置：不应产生转速事件
        //=============================================================
        $display("== phase 2: FC06 configure");
        receive_request(8'd6);
        for(i=0;i<8;i=i+1) response[i]=request[i];
        send_response(8);
        expect_reg(16'h00cc, 32'd0);                            // 配置不发事件

        //=============================================================
        // 3. 正转 +1000 counts/100 ms → 146484 milli-rpm
        //    应答刚结束就回读：事件尚未穿过 CDC，窗口保持旧值
        //=============================================================
        $display("== phase 3: forward 1000");
        receive_request(8'd3);
        prepare_response(32'd1000);
        send_response(9);
        read_reg(16'h00cc, 2'b00, rd);
        check(rd === 32'd0, "CDC: window holds old value before event");
        wait_seq(32'd1);
        expect_reg(16'h00b8, 32'd1000);
        expect_reg(16'h00bc, 32'd146484);
        expect_reg(16'h00c4, 32'd1);
        expect_reg(16'h00d0, 32'd0);
        expect_reg(16'h00cc, 32'd1);
        // status: [0]在线 [3]已配置，其余（饱和/错误/错误码/异常码）为 0
        expect_reg(16'h00c0, 32'h0000_0009);
        // 采样时刻取自采集域共同时基，与 AXI 域 PL_TS（0xB0）相差在 20 ms 内
        read_reg(16'h00c8, 2'b00, rd_ts);
        read_reg(16'h00b0, 2'b00, rd2);
        check(rd_ts !== 32'd0, "window: TS from capture timebase");
        check(rd2 > rd_ts, "CDC: PL_TS not earlier than sample ts");
        check((rd2 - rd_ts) < 32'd2_000_000, "CDC: ts skew < 20 ms");

        //=============================================================
        // 4. 反转 -4096 → -600000 milli-rpm
        //=============================================================
        $display("== phase 4: reverse -4096");
        good_read(32'hffff_f000);
        wait_seq(32'd2);
        expect_reg(16'h00b8, 32'hffff_f000);
        expect_reg(16'h00bc, 32'hfff6_d840);
        expect_reg(16'h00c4, 32'd2);
        expect_reg(16'h00c0, 32'h0000_0009);

        //=============================================================
        // 5. 零速
        //=============================================================
        $display("== phase 5: zero speed");
        good_read(32'd0);
        wait_seq(32'd3);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c4, 32'd3);
        read_reg(16'h00c8, 2'b00, rd_ts);                       // 最近有效采样时刻

        //=============================================================
        // 6. 应答 CRC 错：保留最近有效 RAW/RPM/TS，online=0、错误码 2
        //=============================================================
        $display("== phase 6: CRC error keeps last good");
        receive_request(8'd3);
        prepare_response(32'd1234);
        response[8] = response[8] ^ 8'h40;
        send_response(9);
        wait_seq(32'd4);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c8, rd_ts);
        expect_reg(16'h00c4, 32'd3);                            // 失败不计样本
        expect_reg(16'h00d0, 32'd1);
        // [2]=1 本次错误、[7:4]=2 CRC、[15:8]=0、[0]=0 online
        expect_reg(16'h00c0, 32'h0000_002c);

        //=============================================================
        // 7. Modbus 异常应答：错误码 5、异常码 2（[15:8]）
        //=============================================================
        $display("== phase 7: modbus exception");
        receive_request(8'd3);
        response[1] = 8'h83; response[2] = 8'd2;
        finish_response(3);
        send_response(5);
        wait_seq(32'd5);
        expect_reg(16'h00d0, 32'd2);
        expect_reg(16'h00c0, 32'h0000_025c);

        //=============================================================
        // 8. 恢复：111612 → 16349414
        //=============================================================
        $display("== phase 8: recovery 111612");
        good_read(32'd111612);
        wait_seq(32'd6);
        expect_reg(16'h00b8, 32'd111612);
        expect_reg(16'h00bc, 32'd16349414);
        expect_reg(16'h00c4, 32'd4);
        expect_reg(16'h00c0, 32'h0000_0009);

        //=============================================================
        // 9. 换算饱和：两种符号都置 [1]，RAW 保持原值
        //=============================================================
        $display("== phase 9: saturation both signs");
        good_read(32'h7fff_ffff);
        wait_seq(32'd7);
        expect_reg(16'h00b8, 32'h7fff_ffff);
        expect_reg(16'h00bc, 32'h7fff_ffff);
        expect_reg(16'h00c0, 32'h0000_000b);
        good_read(32'h8000_0000);
        wait_seq(32'd8);
        expect_reg(16'h00b8, 32'h8000_0000);
        expect_reg(16'h00bc, 32'h8000_0000);
        expect_reg(16'h00c0, 32'h0000_000b);

        //=============================================================
        // 10. 读一致性：seq-字段-seq 两次相等（无更新发生）
        //=============================================================
        $display("== phase 10: read consistency");
        read_reg(16'h00cc, 2'b00, rd);
        read_reg(16'h00b8, 2'b00, rd_raw);
        read_reg(16'h00bc, 2'b00, rd_rpm);
        read_reg(16'h00c4, 2'b00, rd_st);
        read_reg(16'h00d0, 2'b00, rd2);
        read_reg(16'h00cc, 2'b00, n);
        check(n === rd, "window: seq-fields-seq consistent");
        check(rd_raw === 32'h8000_0000 && rd_rpm === 32'h8000_0000,
              "一致性快照内容与最近事件相同");
        check(rd_st === 32'd6 && rd2 === 32'd2, "window: sample/error counts");
        // DMA 未启动：事件只更新窗口，既不产生记录也不计丢
        read_reg(16'h00a0, 2'b00, rd);
        check(rd[16] === 1'b0, "DMA idle: no drop counted");

        //=============================================================
        // 11. DMA 记录：真实事件 3 字 {ts,CH=3,状态}/{rpm,CH=4,0}/{raw,CH=5,0}
        //=============================================================
        $display("== phase 11: DMA record of a real event");
        start_dma(32'h1000_0000, 32'd24);
        good_read(32'd4096);
        wait_seq(32'd9);
        wait_dma_done;
        expect_reg(16'h00a4, 32'd24);                           // WRITTEN 字节数
        check(u_mem.S_beat_count === 3, "DMA: one speed record = 3 beats");
        read_reg(16'h00c8, 2'b00, rd_ts);
        read_reg(16'h00c0, 2'b00, rd_st);
        read_reg(16'h00bc, 2'b00, rd_rpm);
        read_reg(16'h00b8, 2'b00, rd_raw);
        check(u_mem.S_wdata[0] === {rd_ts, 16'h0003, rd_st[15:0]}, "DMA: W0={ts,CH3,status}");
        check(u_mem.S_wdata[1] === {rd_rpm, 16'h0004, 16'd0}, "DMA: W1={rpm,CH4,0}");
        check(u_mem.S_wdata[2] === {rd_raw, 16'h0005, 16'd0}, "DMA: W2={raw,CH5,0}");
        check(rd_st[15:0] === 16'h0009, "DMA: status word same bitmap as 0xC0");
        read_reg(16'h00a0, 2'b00, rd);
        check(rd[16] === 1'b0, "DMA: no overflow");
        check(rd[3:0] === 4'd1, "DMA: one burst counted");

        //=============================================================
        // 12. 振动帧先到、转速记录后到：两种记录各自连续、不交错
        //=============================================================
        $display("== phase 12: vibration then speed");
        start_dma(32'h1000_1000, 32'd48);
        mark = u_mem.S_beat_count;
        inject_frame(32'h1122_3344, 16'h1111, 16'h2222, 16'h3333);
        inject_acc(16'h1111, 16'h2222, 16'h3333);
        inject_speed(32'd8000, 32'd1171875, 32'd7, 32'd1,
                     32'h5566_7788, 8'h0d, 8'h00, 8'h00);
        wait_dma_done;
        check(u_mem.S_beat_count === mark+6, "DMA: 6 beats vib then speed");
        check(u_mem.S_wdata[mark+0] === {32'h1122_3344, 16'h0000, 16'h1111}, "DMA: vib X word");
        check(u_mem.S_wdata[mark+1] === {32'h1122_3344, 16'h0001, 16'h2222}, "DMA: vib Y word");
        check(u_mem.S_wdata[mark+2] === {32'h1122_3344, 16'h0002, 16'h3333}, "DMA: vib Z word");
        check(u_mem.S_wdata[mark+3] === {32'h5566_7788, 16'h0003, 16'h0009}, "DMA: speed record not interleaved");
        check(u_mem.S_wdata[mark+4] === {32'd1171875, 16'h0004, 16'd0}, "DMA: speed W1");
        check(u_mem.S_wdata[mark+5] === {32'd8000, 16'h0005, 16'd0}, "DMA: speed W2");
        read_reg(16'h00a0, 2'b00, rd);
        check(rd[16] === 1'b0, "DMA: still no overflow");

        //=============================================================
        // 13. 转速记录先到、振动帧后到：转速三字不被振动样本打断
        //=============================================================
        $display("== phase 13: speed then vibration");
        start_dma(32'h1000_2000, 32'd48);
        mark = u_mem.S_beat_count;
        inject_speed(32'd1234, 32'd180859, 32'd8, 32'd1,
                     32'h99aa_bbcc, 8'h0d, 8'h00, 8'h00);
        inject_frame(32'h5566_7788, 16'h4444, 16'h5555, 16'h6666);
        inject_acc(16'h4444, 16'h5555, 16'h6666);
        wait_dma_done;
        check(u_mem.S_beat_count === mark+6, "DMA: 6 beats speed then vib");
        check(u_mem.S_wdata[mark+0] === {32'h99aa_bbcc, 16'h0003, 16'h0009}, "DMA: speed record first");
        check(u_mem.S_wdata[mark+1] === {32'd180859, 16'h0004, 16'd0}, "DMA: speed W1 first");
        check(u_mem.S_wdata[mark+2] === {32'd1234, 16'h0005, 16'd0}, "DMA: speed W2 first");
        check(u_mem.S_wdata[mark+3] === {32'h5566_7788, 16'h0000, 16'h4444}, "DMA: vib X word after speed");
        check(u_mem.S_wdata[mark+4] === {32'h5566_7788, 16'h0001, 16'h5555}, "DMA: vib Y word after speed");
        check(u_mem.S_wdata[mark+5] === {32'h5566_7788, 16'h0002, 16'h6666}, "DMA: vib Z word after speed");

        //=============================================================
        // 14. 上一条记录未发完时新事件到达：丢弃并计入 DMA_STATUS.overflow
        //=============================================================
        $display("== phase 14: record drop while pending");
        start_dma(32'h1000_3000, 32'd24);
        mark = u_mem.S_beat_count;
        inject_speed_pair({32'h0000_1111, 32'd180859, 32'd9, 32'd1,
                           32'h0bad_f00d, 8'h0d, 8'h00, 8'h00, 8'h00},
                          {32'h0000_2222, 32'd361719, 32'd10, 32'd1,
                           32'h0bad_f00e, 8'h0d, 8'h00, 8'h00, 8'h00});
        wait_dma_done;
        check(u_mem.S_beat_count === mark+3, "DMA: only first record emitted");
        check(u_mem.S_wdata[mark+0] === {32'h0bad_f00d, 16'h0003, 16'h0009}, "DMA: first record kept");
        check(u_mem.S_wdata[mark+1] === {32'd180859, 16'h0004, 16'd0}, "DMA: first W1");
        check(u_mem.S_wdata[mark+2] === {32'h0000_1111, 16'h0005, 16'd0}, "DMA: first W2");
        read_reg(16'h00a0, 2'b00, rd);
        check(rd[16] === 1'b1, "DMA: drop counted in OVF");
        wait_seq(32'd13);                                       // 两个事件都已计数
        read_reg(16'h00cc, 2'b00, rd);
        check(rd === 32'd13, "event: drop does not stop seq");

        //=============================================================
        // 15. 软复位清零窗口与丢弃标志
        //=============================================================
        $display("== phase 15: soft reset clears window");
        write_reg(16'h0004, 32'h0000_0002, 4'hf, 0, 2'b00);     // CTRL.RST
        repeat(4) @(negedge axi_clk);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c0, 32'd0);
        expect_reg(16'h00c4, 32'd0);
        expect_reg(16'h00c8, 32'd0);
        expect_reg(16'h00cc, 32'd0);
        expect_reg(16'h00d0, 32'd0);
        write_reg(16'h0004, 32'h0000_0000, 4'hf, 0, 2'b00);

        $display("RESULT tb_speed_channel PASS=%0d FAIL=0", checks);
        $finish;
    end

    initial begin
        #50_000_000;
        $fatal(1, "转速通道仿真超时");
    end
endmodule
