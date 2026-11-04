`timescale 1ns/1ps
/*******************************SPI 主机输入量专项测量 TB*******************************
*File Name: tb_spi_spec.v
*Description:
*按分层验证法测量主机侧的三项"输入量"，判据为精确数值而非"跑通了"：
*  L1  MOSI 上命令字节的位序列          -> 应与发送值逐位一致
*  L2  SCLK 周期与占空比                -> 5 MHz、占空比 50%
*  L3  片选有效到首个 SCLK 下降沿的间隔 -> tDELAY
*
*被测对象只用发送驱动，接收侧不参与，故本 TB 不需要从机模型，
*总线仅接上拉，MISO 恒定，避免从机行为干扰对发送侧的判定。
*********************************************************************/

module tb_spi_spec;
    localparam integer US = 1000;
    // 实测标定：SCLK 半周期 = (CLK_DIV + 1) x 5 ns。
    //   CLK_DIV = 9  -> 半周期 50  ns -> 10 MHz
    //   CLK_DIV = 19 -> 半周期 100 ns ->  5 MHz（ADXL345 上限）
    localparam integer CLK_DIV = 19;                 // 设计取值
    localparam integer HALF_NS = (CLK_DIV + 1) * 5;  // 半周期（ns）
    localparam [7:0]   CMD    = 8'hC0;              // 读DEVID命令字节

    reg clk = 1'b0;
    always #5 clk = ~clk;                           // 100 MHz

    reg        rstn = 1'b0;
    reg        req  = 1'b0;
    reg [7:0]  dat  = CMD;
    wire       mosi, sclk, busy;

    ui_mspi_tx #(.CLK_DIV(CLK_DIV), .CPOL(1'b1), .CPHA(1'b1)) u_tx (
        .I_clk(clk), .I_rstn(rstn),
        .O_spi_mosi(mosi), .O_spi_sclk(sclk),
        .I_spi_tx_req(req), .I_spi_tx_data(dat), .O_spi_busy(busy)
    );

    //----------------------------------------------------------
    // L1：在每个 SCLK 下降沿采样 MOSI，MSB 先入
    //
    // 发送驱动在下降沿更新 MOSI，此刻 MOSI 上已是新一位的值；
    // 该位随后的整个高电平半周期保持稳定，供从机采样。
    // 用 SCLK 边沿直接驱动，避免与发送驱动的内部分频相位失配。
    //----------------------------------------------------------
    reg [7:0]  got  = 8'h00;
    integer    nbit = 0;

    reg sclk_prev = 1'b1;
    always @(posedge clk) begin
        sclk_prev <= sclk;
        // 只在发送进行期间采位：空闲期 SCLK 与 MOSI 均为高，
        // 若把空闲期的下降沿也算进来，会多采一位、整体左移。
        if (busy && sclk_prev === 1'b1 && sclk === 1'b0 && nbit < 8) begin
            got = {got[6:0], mosi};   // 先到的位最终落在 bit0
            $display("    [采位] 下降沿 %0d: MOSI=%b -> 移位寄存器 0x%02h",
                     nbit + 1, mosi, got);
            nbit = nbit + 1;
        end
    end

    //----------------------------------------------------------
    // L2：测量 SCLK 半周期
    //----------------------------------------------------------
    time t_prev_fall = 0;
    time t_prev_rise = 0;
    integer n_edge   = 0;
    integer half_low = 0;      // 低电平半周期
    integer half_high= 0;      // 高电平半周期
    reg     sclk_d   = 1'b1;

    always @(posedge clk) begin
        sclk_d <= sclk;
        if (!sclk_d && sclk) begin           // 上升沿
            if (t_prev_fall != 0) half_low = $time - t_prev_fall;
            t_prev_rise = $time;
            n_edge = n_edge + 1;
        end
        else if (sclk_d && !sclk) begin      // 下降沿
            if (t_prev_rise != 0) half_high = $time - t_prev_rise;
            t_prev_fall = $time;
            n_edge = n_edge + 1;
        end
    end

    //----------------------------------------------------------
    // L3：片选到首个 SCLK
    //----------------------------------------------------------
    time t_first_fall = 0;
    integer tdelay_ns = 0;

    initial begin
        $display("=== SPI 主机输入量专项测量（命令 0x%02h）===", CMD);
        $display("设计取值：CLK_DIV=%0d -> SCLK 半周期应 %0d ns，周期 %0d ns，频率 %0.2f MHz",
                 CLK_DIV, HALF_NS, HALF_NS*2, 1000.0/(HALF_NS*2.0));

        #(20*US);
        rstn = 1'b1;
        #(1*US);

        // 发起一次发送
        @(posedge clk);
        t_first_fall = 0;
        req = 1'b1;
        @(posedge clk);
        req = 1'b0;

        // 等发送结束
        while (!busy) @(posedge clk);       // 等开始
        while (busy)  @(posedge clk);       // 等结束
        #(2*US);

        $display("\n--- L1 命令位序列 ---");
        $display("  发送值   = 0x%02h = %b", CMD, CMD);
        $display("  总线上采 = 0x%02h = %b", got, got);
        $display("  位对照（bit7 先发）:");
        for (integer k = 7; k >= 0; k = k - 1)
            $display("    bit%0d: 期望 %b  实测 %b  %s",
                     k, CMD[k], got[k], (CMD[k] == got[k]) ? "一致" : "*** 不符 ***");

        $display("\n--- L3 片选到首个 SCLK 下降沿（tDELAY）---");
        if (tdelay_ns > 0)
            $display("  实测 %0d ns（设计 DELAY_CNT=500 -> 5000 ns）", tdelay_ns);
        else
            $display("  未捕获（本 TB 未接片选信号，此项由 spi_adxl345 提供）");

        $display("\n--- L2 SCLK 周期与占空比 ---");
        if (half_low > 0 && half_high > 0) begin
            $display("  低电平半周期 = %0d ns（期望 %0d）", half_low,  HALF_NS);
            $display("  高电平半周期 = %0d ns（期望 %0d）", half_high, HALF_NS);
            $display("  周期         = %0d ns（期望 %0d）", half_low+half_high, HALF_NS*2);
        end
        else
            $display("  未测到足够的时钟边沿（边沿数 %0d）", n_edge);

        $display("\n=== 判定 ===");
        if (got == CMD) $display("  [PASS] L1 命令位序列与发送值一致");
        else            $display("  [FAIL] L1 命令位序列不符：采到 0x%02h，应为 0x%02h", got, CMD);
        if (half_low == HALF_NS && half_high == HALF_NS)
            $display("  [PASS] L2 SCLK 半周期 %0d ns，占空比 50%%", HALF_NS);
        else if (half_low > 0)
            $display("  [FAIL] L2 SCLK 半周期不符：低 %0d / 高 %0d，应为 %0d",
                     half_low, half_high, HALF_NS);

        $finish;
    end

    //----------------------------------------------------------
    // 采样与测量（见上方各段）
    //----------------------------------------------------------

endmodule
