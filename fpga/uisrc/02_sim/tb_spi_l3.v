`timescale 1ns/1ps
/*******************************SPI 整体时序专项测量 TB（3 线）*******************************
*File Name: tb_spi_l3.v
*Description:
*测量 spi_adxl345 层面的时序，判据与单位无关（以系统时钟周期计）：
*  L1  SCLK 半周期              -> (CLK_DIV + 1) 个系统时钟
*  L2  片选有效到首个时钟的间隔 -> DELAY_CNT 个系统时钟（5 us）
*  L3  一次 2 字节传输的时钟沿数 -> 16
*  L4  两次传输之间片选高电平时间 -> >= DELAY_CNT 个系统时钟（手册 5 us 要求）
*
*从机直接用 adxl345_slave_model，读回值应为 0xE5。
*********************************************************************/

module tb_spi_l3;
    localparam integer CLK_DIV   = 19;
    localparam integer DELAY_CNT = 500;
    localparam [7:0]   CMD       = 8'hC0;           // 读 DEVID

    reg clk = 1'b0;
    always #5 clk = ~clk;                           // 100 MHz

    reg         rstn  = 1'b0;
    reg         start = 1'b0;
    reg  [3:0]  num   = 4'd2;
    wire        busy, done;
    wire [55:0] rdata;
    wire        cs_n, sclk, sdio;

    integer cyc = 0;
    always @(posedge clk) cyc = cyc + 1;

    spi_adxl345 #(
        .CLK_FREQ_HZ (100_000_000),
        .CLK_DIV     (CLK_DIV),
        .BYTE_CNT    (8),
        .DELAY_CNT   (DELAY_CNT)
    ) u_spi (
        .I_clk       (clk   ),
        .I_rstn      (rstn  ),
        .I_start     (start ),
        .I_byte_num  (num   ),
        .I_cmd       (CMD   ),
        .I_wr_data   (56'd0 ),
        .O_busy      (busy  ),
        .O_done      (done  ),
        .O_rd_data   (rdata ),
        .O_spi_cs_n  (cs_n  ),
        .O_spi_sclk  (sclk  ),
        .IO_spi_sdio (sdio  )
    );

    adxl345_slave_model #(.DEVID(8'hE5)) u_slave (
        .I_clk       (clk   ),
        .I_spi_cs_n  (cs_n  ),
        .I_spi_sclk  (sclk  ),
        .IO_spi_sdio (sdio  ),
        .O_mon_cmd   (      ),
        .O_mon_addr  (      ),
        .O_mon_wr    (      )
    );

    //----------------------------------------------------------
    // 时序测量（全部以 cyc 计数，单位无关）
    //----------------------------------------------------------
    reg cs_prev = 1'b1, sclk_prev = 1'b1;
    integer n_fall = 0;
    integer cyc_cs_fall = 0, cyc_first_fall = 0;
    integer cyc_half_rise = 0, cyc_half_fall = 0, half_cyc = 0;
    integer tdelay_cyc = 0;
    integer cyc_cs_rise_prev = 0, cyc_cs_fall_prev = 0, cs_high_cyc = 0;
    integer n_transfer = 0;

    always @(posedge clk) begin
        cs_prev   <= cs_n;
        sclk_prev <= sclk;

        // 片选下降沿：开始一次传输
        if (cs_prev === 1'b1 && cs_n === 1'b0) begin
            n_transfer = n_transfer + 1;
            if (n_transfer == 2) begin
                cs_high_cyc = cyc - cyc_cs_rise_prev;   // 上一次片选拉高到本次拉低
            end
            cyc_cs_fall_prev = cyc;
            cyc_cs_fall      = cyc;
            n_fall           = 0;
        end

        // 片选上升沿
        if (cs_prev === 1'b0 && cs_n === 1'b1) begin
            cyc_cs_rise_prev = cyc;
        end

        // SCLK 边沿
        if (!cs_n) begin
            if (sclk_prev === 1'b1 && sclk === 1'b0) begin   // 下降沿
                if (n_fall == 0) begin
                    cyc_first_fall = cyc;
                    tdelay_cyc     = cyc - cyc_cs_fall;
                end
                n_fall = n_fall + 1;
            end
            if (sclk_prev === 1'b0 && sclk === 1'b1) begin   // 上升沿
                if (cyc_half_fall != 0 && half_cyc == 0)
                    half_cyc = cyc - cyc_half_fall;          // 一次低电平半周期
                cyc_half_rise = cyc;
            end
            if (sclk_prev === 1'b1 && sclk === 1'b0)
                cyc_half_fall = cyc;
        end
    end

    integer pass_cnt = 0, fail_cnt = 0;
    task chk;
        input [255:0] nm;
        input         ok;
        begin
            if (ok) begin pass_cnt = pass_cnt + 1; $display("  [PASS] %0s", nm); end
            else    begin fail_cnt = fail_cnt + 1; $display("  [FAIL] %0s", nm); end
        end
    endtask

    integer n_read = 0;
    always @(posedge clk) begin
        if (done && n_read < 2) begin
            n_read = n_read + 1;
            $display("[传输 %0d 完成] rdata[7:0] = 0x%02h", n_read, rdata[7:0]);
        end
    end

    initial begin
        $display("=== SPI 整体时序专项测量（3 线，CLK_DIV=%0d, DELAY_CNT=%0d）===",
                 CLK_DIV, DELAY_CNT);

        #(20*1000);              // 复位等待
        rstn = 1'b1;
        #(1*1000);

        // 第一次传输
        @(posedge clk); start = 1'b1;
        @(posedge clk); start = 1'b0;
        while (!done) @(posedge clk);
        @(posedge clk);

        // 第二次传输，用于测量两次传输之间的片选高电平时间
        @(posedge clk); start = 1'b1;
        @(posedge clk); start = 1'b0;
        while (!done) @(posedge clk);
        #(1*1000);

        $display("\n--- 结果（单位：系统时钟周期，100 MHz）---");
        $display("  L1 SCLK 低电平半周期   = %0d 周期（期望 %0d）", half_cyc, (CLK_DIV + 1) / 2);
        $display("  L2 片选到首个时钟       = %0d 周期（期望 >= %0d）", tdelay_cyc, DELAY_CNT);
        $display("  L3 一次传输下降沿数     = %0d（期望 %0d）", n_fall, 16);
        $display("  L4 两次传输间片选高     = %0d 周期（期望 >= %0d）", cs_high_cyc, DELAY_CNT);

        chk("L1 SCLK 半周期 = (CLK_DIV+1)/2", half_cyc == (CLK_DIV + 1) / 2);
        chk("L2 片选到首个时钟 >= DELAY_CNT", tdelay_cyc >= DELAY_CNT);
        chk("L3 一次传输 16 个时钟沿",      n_fall == 16);
        chk("L4 两次传输间片选高 >= 5 us",  cs_high_cyc >= DELAY_CNT);

        $display("\n PASS = %0d   FAIL = %0d", pass_cnt, fail_cnt);
        $finish;
    end
endmodule
