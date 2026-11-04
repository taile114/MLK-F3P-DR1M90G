`timescale 1ns/1ps
/*******************************ADXL345 快速验证 TB（3 线 SPI）*******************************
*File Name: tb_fast.v
*Description:
*用 adxl345_slave_model 模拟真实器件，验证主机侧 RTL 的完整流程：
*  上电稳定 -> 读 DEVID(0x00) 自检 -> 写 4 个配置寄存器
*  -> 周期性突发读 0x32~0x37 -> 解析三轴
*
*计时参数用 INIT_MS(1) 缩短；采样由从机模型的 DATA_READY（3200 Hz）触发，
*SPI 时钟保持真实的 5 MHz，因为这两项是验证对象，不能缩短。
*********************************************************************/

module tb_fast;
    localparam integer US = 1000;
    localparam [15:0] TEST_X = 16'h0123;   // X 轴给定值
    localparam [15:0] TEST_Y = 16'hFEDC;   // Y 轴给定值（负数，验证有符号处理）
    localparam [15:0] TEST_Z = 16'h0100;   // Z 轴给定值（约 +1 g）

    reg clk = 1'b0;
    always #5 clk = ~clk;                  // 100 MHz

    wire       spi_cs_n, spi_sclk;
    wire       spi_sdio;                    // 双向数据线
    wire       dr;                          // DATA_READY（INT1）
    wire [3:0] led;

    //----------------------------------------------------------
    // 被测设计
    //----------------------------------------------------------
    adxl345_test #(.INIT_MS(1)) u_dut (
        .I_sysclk    (clk      ),
        .O_spi_cs_n  (spi_cs_n ),
        .O_spi_sclk  (spi_sclk ),
        .IO_spi_sdio (spi_sdio ),
        .I_int1      (dr       ),
        .O_led       (led      )
    );

    //----------------------------------------------------------
    // 从机模型（3 线，双向数据线直接并联）
    //----------------------------------------------------------
    wire [7:0] mon_cmd, mon_addr;
    wire       mon_wr;

    adxl345_slave_model #(
        .X_VAL (TEST_X),
        .Y_VAL (TEST_Y),
        .Z_VAL (TEST_Z),
        .DEVID (8'hE5)
    ) u_slave (
        .I_clk       (clk      ),
        .I_spi_cs_n  (spi_cs_n ),
        .I_spi_sclk  (spi_sclk ),
        .IO_spi_sdio (spi_sdio ),
        .O_mon_cmd   (mon_cmd  ),
        .O_mon_addr  (mon_addr ),
        .O_mon_wr    (mon_wr   ),
        .O_int1      (dr       )
    );

    //----------------------------------------------------------
    // 观测
    //----------------------------------------------------------
    integer n_vld = 0;
    reg [15:0] got_x, got_y, got_z;

    // SCLK 边沿计数（判断 SPI 是否真的在跑）
    integer n_sclk = 0;
    reg sclk_d = 1'b1;
    always @(posedge clk) begin
        sclk_d <= spi_sclk;
        if (spi_sclk !== sclk_d) n_sclk = n_sclk + 1;
    end

    // 从机收到的命令
    reg [7:0] cmd_d = 8'hEE;
    always @(posedge clk) begin
        if (u_slave.O_mon_cmd !== cmd_d) begin
            $display("[%8t us] 从机收到命令 0x%02h", $time/US, u_slave.O_mon_cmd);
            cmd_d <= u_slave.O_mon_cmd;
        end
    end

    always @(posedge clk) begin
        if (u_dut.S_data_vld && n_vld < 4) begin
            n_vld = n_vld + 1;
            got_x = u_dut.S_acc_x;
            got_y = u_dut.S_acc_y;
            got_z = u_dut.S_acc_z;
            $display("[%8t us] 三轴读取 #%0d  X=0x%04h Y=0x%04h Z=0x%04h",
                     $time/US, n_vld, u_dut.S_acc_x, u_dut.S_acc_y, u_dut.S_acc_z);
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

    initial begin
        $display("=== ADXL345 快速验证（3 线 SPI，从机 X=0x%04h Y=0x%04h Z=0x%04h）===",
                 TEST_X, TEST_Y, TEST_Z);

        #(20000*US);      // 20 ms，足够完成自检 + 初始化 + 多次采样

        $display("\n--- 结果 ---");
        $display("  O_dev_id   = 0x%02h   (期望 0xE5)", u_dut.S_dev_id);
        $display("  O_dev_ok   = %b", u_dut.S_dev_ok);
        $display("  三轴读取次数 = %0d", n_vld);
        $display("  最后一次 X=0x%04h Y=0x%04h Z=0x%04h", got_x, got_y, got_z);
        $display("  从机 BW_RATE=0x%02h DATA_FORMAT=0x%02h POWER_CTL=0x%02h INT_ENABLE=0x%02h",
                 u_slave.regs[8'h2C], u_slave.regs[8'h31],
                 u_slave.regs[8'h2D], u_slave.regs[8'h2E]);

        chk("DEVID 自检通过（读到 0xE5）", u_dut.S_dev_id == 8'hE5);
        chk("DEVID 标志置位",             u_dut.S_dev_ok == 1'b1);
        chk("BW_RATE 写入 0x0F",          u_slave.regs[8'h2C] == 8'h0F);
        chk("DATA_FORMAT 写入 0x4B（3 线）", u_slave.regs[8'h31] == 8'h4B);
        chk("INT_ENABLE 写入 0x80",       u_slave.regs[8'h2E] == 8'h80);
        chk("POWER_CTL 写入 0x08",        u_slave.regs[8'h2D] == 8'h08);
        chk("完成至少一次三轴读取",        n_vld >= 1);
        chk("X 轴数据正确",               got_x == TEST_X);
        chk("Y 轴数据正确",               got_y == TEST_Y);
        chk("Z 轴数据正确",               got_z == TEST_Z);

        $display("\n PASS = %0d   FAIL = %0d", pass_cnt, fail_cnt);
        $finish;
    end
endmodule
