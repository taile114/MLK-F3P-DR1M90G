`timescale 1ns/1ps
// 转速通道闭环集成自检（接口 v1.2，§14）：
//   本 TB 不使用任何 DUT 内部强驱动，全部数据都走真实通路
//   RS485 引脚 → encoder_modbus（Modbus RTU 主站）→ encoder_rpm →
//   192 bit cdc_mailbox（采集域 100 MHz → 寄存器域 50 MHz）→ AXI 域整体锁存
//   → 寄存器窗口 / DMA 三字记录。
// TB 侧是一个最小 Modbus 从站：按 1 Mbaud（每 bit 100 个采集时钟）采样主机请求
// 并应答 FC06 回显、FC03 数据帧与一帧 CRC 错的应答；请求 CRC 与站号由 TB 自检。
// 断言：符号与数值、计数/SEC 递增、状态位图、错误后保留最近有效值、跨域时基、
// 以及真实事件在 DMA 里的 CH=3/4/5 三字与寄存器同值。
module tb_speed_soc;
    localparam integer BIT_CLKS = 100;          // 1 Mbaud @ 100 MHz

    reg axi_clk = 0, sensor_clk = 0, rstn = 0;
    always #10 axi_clk = ~axi_clk;              // 50 MHz 寄存器域
    always #5  sensor_clk = ~sensor_clk;        // 100 MHz 采集域
    wire clk = sensor_clk;                      // 从站位采样时钟

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
    integer i, n;

    //------------------------------------------------------------------
    // RS485：TB 是从站，只驱动 slave_tx；主机发送期间线路上是主机的回波
    //------------------------------------------------------------------
    reg slave_tx = 1;
    wire tx, de;
    wire rs485_rx = de ? 1'b0 : slave_tx;

    wire sclk, cs, sdio, scl, sda;
    wire [3:0] led;

    wire [32:0] m_awaddr; wire [7:0] m_awlen; wire [2:0] m_awsize; wire [1:0] m_awburst;
    wire m_awvalid, m_awready; wire [63:0] m_wdata; wire [7:0] m_wstrb;
    wire m_wlast, m_wvalid, m_wready; wire [1:0] m_bresp; wire m_bvalid, m_bready;

    // 交换时间压缩：启动 1 ms、轮询 1 ms；SAMPLE_MS 保持契约分母 4096*100，
    // 它只决定 FC06 写入值与 rpm 换算比例，不参与事务节拍。
    sensor_soc_axi #(
        .SPEED_BAUD(1_000_000), .SPEED_STARTUP_MS(1),
        .SPEED_POLL_MS(1), .SPEED_TIMEOUT_MS(5),
        .SPEED_SAMPLE_MS(100)
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

    axi_slave_mem_model #(.MEM_WORDS(256), .MAX_BURSTS(16)) u_mem (
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
    // 最小 Modbus RTU 从站（引脚级）
    //------------------------------------------------------------------
    // 采样主机 8 字节请求：1 站号 + 1 功能码 + 4 数据 + 2 CRC。
    task soc_recv;
        integer i, b; reg [7:0] value; reg [15:0] crc;
        begin
            crc = 16'hffff;
            for(i=0;i<8;i=i+1) begin
                @(negedge tx);
                check(de, "slave: DE asserted before start bit");
                repeat(BIT_CLKS/2) @(negedge clk);
                check(tx===0, "slave: start bit");
                for(b=0;b<8;b=b+1) begin
                    repeat(BIT_CLKS) @(negedge clk);
                    value[b] = tx;
                end
                repeat(BIT_CLKS) @(negedge clk);
                check(tx===1 && de, "slave: stop bit");
                repeat(BIT_CLKS/2-2) @(negedge clk);
                request[i] = value;
                crc = bfm_crc(crc, value);        // 含 CRC 两字节，正确时为 0
            end
            check(crc === 16'h0000, "slave: request CRC");
            check(request[0]===8'd1, "slave: station address");
            wait(!de);
            check(tx===1, "slave: TX idle on DE release");
            repeat(BIT_CLKS/8) @(negedge clk);
        end
    endtask
    // FC06 回显：应答 = 请求原样 8 字节（含 CRC）。
    task soc_echo;
        begin
            for(i=0;i<8;i=i+1) response[i] = request[i];
            send_response(8);
        end
    endtask
    // FC03 应答：1 站号 + 3 功能码 + 4 字节数 + 4 数据 + 2 CRC = 9 字节。
    task soc_read(input [31:0] raw);
        begin
            check(request[1]===8'd3, "slave: function code 3");
            prepare_response(raw);
            send_response(9);
        end
    endtask
    // 故意破坏 CRC 低字节的应答（错误事件），其余字段合法。
    task soc_read_bad_crc(input [31:0] raw);
        begin
            check(request[1]===8'd3, "slave: function code 3");
            prepare_response(raw);
            response[8] = response[8] ^ 8'h40;
            send_response(9);
        end
    endtask

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
            write_reg(16'h0090, 32'd1, 4'hf, 0, 2'b00);
            n = 0; rd = 32'd0;
            while (n < 200 && rd[8] !== 1'b1) begin
                read_reg(16'h0090, 2'b00, rd);
                n = n + 1;
            end
            check(rd[8] === 1'b1, "DMA: busy after start");
        end
    endtask
    task wait_dma_done;
        begin
            n = 0; rd = 32'd0;
            while (n < 4000 && rd[9] !== 1'b1) begin
                read_reg(16'h0090, 2'b00, rd);
                n = n + 1;
            end
            check(rd[9] === 1'b1, "DMA: done");
        end
    endtask

    initial begin
        if($test$plusargs("vcd")) begin
            $dumpfile("speed_soc.vcd"); $dumpvars(0, tb_speed_soc);
        end
        awvalid=0; wvalid=0; bready=0; arvalid=0; rready=0; slave_tx=1;
        repeat(20) @(negedge axi_clk);
        rstn=1;
        repeat(20) @(negedge axi_clk);

        //=============================================================
        // 1. 复位值 / ID
        //=============================================================
        $display("== phase 1: reset values / ID");
        expect_reg(16'h0000, 32'h504c0103);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c0, 32'd0);
        expect_reg(16'h00c4, 32'd0);
        expect_reg(16'h00c8, 32'd0);
        expect_reg(16'h00cc, 32'd0);
        expect_reg(16'h00d0, 32'd0);
        write_reg(16'h0004, 32'h0000_0001, 4'hf, 0, 2'b00);     // CTRL.EN=1

        //=============================================================
        // 2. FC06：写 0x000A 采样周期，从站原样回显
        //=============================================================
        $display("== phase 2: FC06 configure over the pins");
        soc_recv;
        check(request[1]===8'd6, "slave: function code 6");
        check(request[2]===8'h00 && request[3]===8'h0a, "slave: FC06 register 0x000A");
        soc_echo;

        //=============================================================
        // 3. FC03 raw=+1000：同时开 DMA，检查真实事件的三字记录
        //=============================================================
        $display("== phase 3: forward 1000 through the real datapath");
        start_dma(32'h1000_0000, 32'd24);
        soc_recv;
        check(request[1]===8'd3, "slave: function code 3");
        soc_read(32'd1000);
        wait_seq(32'd1);
        expect_reg(16'h00b8, 32'd1000);
        expect_reg(16'h00bc, 32'd146484);
        expect_reg(16'h00c4, 32'd1);
        expect_reg(16'h00c0, 32'h0000_0009);                    // 在线 + 已配置
        read_reg(16'h00c8, 2'b00, rd_ts);
        check(rd_ts !== 32'd0, "window: TS from capture timebase");
        read_reg(16'h00b0, 2'b00, rd2);
        check(rd2 > rd_ts, "CDC: PL_TS not earlier than sample ts");
        check((rd2 - rd_ts) < 32'd2_000_000, "CDC: ts skew < 20 ms");
        wait_dma_done;
        expect_reg(16'h00a4, 32'd24);
        check(u_mem.S_beat_count === 3, "DMA: one speed record = 3 beats");
        read_reg(16'h00c8, 2'b00, rd_ts);
        read_reg(16'h00c0, 2'b00, rd_st);
        read_reg(16'h00bc, 2'b00, rd_rpm);
        read_reg(16'h00b8, 2'b00, rd_raw);
        check(u_mem.S_wdata[0] === {rd_ts, 16'h0003, rd_st[15:0]}, "DMA: W0={ts,CH3,status}");
        check(u_mem.S_wdata[1] === {rd_rpm, 16'h0004, 16'd0}, "DMA: W1={rpm,CH4,0}");
        check(u_mem.S_wdata[2] === {rd_raw, 16'h0005, 16'd0}, "DMA: W2={raw,CH5,0}");
        check(rd_rpm === 32'd146484 && rd_raw === 32'd1000, "DMA: record matches window");

        //=============================================================
        // 4. FC03 raw=-1000 → -146484，符号由引脚链路真实传过来
        //=============================================================
        $display("== phase 4: reverse -1000");
        soc_recv;
        soc_read(32'hffff_fc18);                                // -1000
        wait_seq(32'd2);
        expect_reg(16'h00b8, 32'hffff_fc18);
        expect_reg(16'h00bc, 32'hfffd_c3cc);                    // -146484
        expect_reg(16'h00c4, 32'd2);
        expect_reg(16'h00c0, 32'h0000_0009);

        //=============================================================
        // 5. FC03 raw=0 → 零速
        //=============================================================
        $display("== phase 5: zero speed");
        soc_recv;
        soc_read(32'd0);
        wait_seq(32'd3);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c4, 32'd3);
        read_reg(16'h00c8, 2'b00, rd_ts);                        // 最近有效采样时刻

        //=============================================================
        // 6. CRC 错的应答：保留最近有效值，online=0、错误码 2
        //=============================================================
        $display("== phase 6: bad CRC reply");
        soc_recv;
        soc_read_bad_crc(32'd4321);
        wait_seq(32'd4);
        expect_reg(16'h00b8, 32'd0);
        expect_reg(16'h00bc, 32'd0);
        expect_reg(16'h00c8, rd_ts);
        expect_reg(16'h00c4, 32'd3);                            // 失败不计样本
        expect_reg(16'h00d0, 32'd1);
        expect_reg(16'h00c0, 32'h0000_002c);                    // [2]=1 错误、[7:4]=2

        //=============================================================
        // 7. 恢复：raw=+2000，错误位清零
        //=============================================================
        $display("== phase 7: recovery 2000");
        soc_recv;
        soc_read(32'd2000);
        wait_seq(32'd5);
        expect_reg(16'h00b8, 32'd2000);
        expect_reg(16'h00bc, 32'd292968);
        expect_reg(16'h00c4, 32'd4);
        expect_reg(16'h00c0, 32'h0000_0009);

        $display("RESULT tb_speed_soc PASS=%0d FAIL=0", checks);
        $finish;
    end

    initial begin
        #50_000_000;
        $fatal(1, "转速通道 SoC 闭环仿真超时");
    end
endmodule
