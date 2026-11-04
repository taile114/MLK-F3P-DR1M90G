/*******************************MILIANKE*******************************
*Company : MiLianKe Electronic Technology Co., Ltd.
*WebSite:https://www.milianke.com
*TechWeb:https://www.uisrc.com
*Create Date: 2026/10/11
*Module Name:adxl345_test
*File Name:adxl345_test.v
*Description:
*ADXL345 三轴加速度采集顶层，3 线 SPI（CPOL = 1，CPHA = 1，模式 3），
*SCLK = 5 MHz（ADXL345 手册上限）。
*
*为什么用 3 线：所用 ADXL345 模块原理图上 SDO（引脚 12）经 R4 = 0 欧姆接地。
*4 线 SPI 下该脚是主机输入 MISO，恒被拉低，读回全 0；故改用 3 线 SPI，
*数据在 SDA/SDI/SDIO（引脚 13）上双向传输。依据与证据见 README §5。
*
*接线（核心板 J12 -> ADXL345 模块）：
*  J12.1  VCC  -> 模块 5V    （模块板载 RT9161 降至 3.3 V；核心板输出 3.3 V，
*                              LDO 低压差，实测模块 3V3 脚约 3.3 V）
*  J12.2  GND  -> 模块 GND
*  J12.3  Y12  -> 模块 SCL   （SPI 时钟）
*  J12.4  Y13  -> 模块 SDA   （SPI 数据，3 线双向）
*  J12.6  U10  -> 模块 CS    （片选，低有效；模块上经 10 k 上拉至 3V3，
*                              必须由 PL 拉低才进入 SPI 模式）
*  J12.7  T5   -> 模块 INT1  （DATA_READY，触发逐样本读取）
*  模块 SDO 脚不接（3 线模式不用；模块上已被 R4 = 0 欧姆接地）。
*
*注意：CS 低 = SPI 模式，CS 高 = I2C 模式，无默认状态，必须由 PL 驱动。
*Signal description
*1) I_ input
*2) O_ output
*3) IO_ input output
*4) S_ system internal signal
*5) n_ activ low
*6) dg_ debug signal
*7) r_ delay or register
*8) s_ state mechine
*********************************************************************/

`timescale 1ns / 1ps
module adxl345_test #(
    parameter integer INIT_MS = 10          // 上电稳定等待（ms）
)(
    input           I_sysclk   , // 底板 100 MHz 时钟
    output          O_spi_cs_n , // SPI 片选，低有效
    output          O_spi_sclk , // SPI 时钟
    inout           IO_spi_sdio, // SPI 数据（3 线，双向）
    input           I_int1     , // ADXL345 DATA_READY 中断
    output [3:0]    O_led        // 板上 LED1~LED4
);

    //----------------------------------------------------------
    // 同步复位：上电后保持约 655 us
    //----------------------------------------------------------
    reg [15:0] r_rst_cnt = 16'd0;
    reg        r_rstn    = 1'b0;

    always @(posedge I_sysclk) begin
        if (r_rst_cnt != 16'hFFFF) begin
            r_rst_cnt <= r_rst_cnt + 1'b1;
            r_rstn    <= 1'b0;
        end
        else begin
            r_rstn <= 1'b1;
        end
    end

    //----------------------------------------------------------
    // SPI 主机与命令层
    //----------------------------------------------------------
    wire        S_spi_start;
    wire [3:0]  S_spi_num  ;
    wire [7:0]  S_spi_cmd  ;
    wire [55:0] S_spi_wdata;
    wire        S_spi_busy ;
    wire        S_spi_done ;
    wire [55:0] S_spi_rdata;

    wire [15:0] S_acc_x   ;
    wire [15:0] S_acc_y   ;
    wire [15:0] S_acc_z   ;
    wire        S_data_vld;
    wire        S_dev_ok  ;
    wire [7:0]  S_dev_id  ;
    wire        S_dev_rd_vld;
    wire        S_busy    ;

    spi_adxl345 #(
        .CLK_FREQ_HZ (100_000_000),
        .CLK_DIV     (19),             // SCLK 半周期 100 ns -> 5 MHz（器件上限）
        .BYTE_CNT    (8),
        .DELAY_CNT   (500)             // 片选有效后等待 5 us
    ) X_spi_adxl345 (
        .I_clk       (I_sysclk   ),
        .I_rstn      (r_rstn     ),
        .I_start     (S_spi_start),
        .I_byte_num  (S_spi_num  ),
        .I_cmd       (S_spi_cmd  ),
        .I_wr_data   (S_spi_wdata),
        .O_busy      (S_spi_busy ),
        .O_done      (S_spi_done ),
        .O_rd_data   (S_spi_rdata),
        .O_spi_cs_n  (O_spi_cs_n ),
        .O_spi_sclk  (O_spi_sclk ),
        .IO_spi_sdio (IO_spi_sdio)
    );

    adxl345_ctrl #(
        .CLK_FREQ_HZ (100_000_000),
        .INIT_MS     (INIT_MS    )
    ) X_adxl345_ctrl (
        .I_clk       (I_sysclk   ),
        .I_rstn      (r_rstn     ),
        .I_dr        (I_int1     ),
        .O_spi_start (S_spi_start),
        .O_spi_num   (S_spi_num  ),
        .O_spi_cmd   (S_spi_cmd  ),
        .O_spi_wdata (S_spi_wdata),
        .I_spi_busy  (S_spi_busy ),
        .I_spi_done  (S_spi_done ),
        .I_spi_rdata (S_spi_rdata),
        .O_acc_x     (S_acc_x    ),
        .O_acc_y     (S_acc_y    ),
        .O_acc_z     (S_acc_z    ),
        .O_data_vld  (S_data_vld ),
        .O_dev_ok    (S_dev_ok   ),
        .O_dev_id    (S_dev_id   ),
        .O_dev_rd    (S_dev_rd_vld),
        .O_busy      (S_busy     )
    );

    //----------------------------------------------------------
    // LED 指示与调试显示
    //
    // 板上可受 PL 驱动的 LED 共 4 只：LED1(T19)、LED2(R19)、LED3(J15)、
    // LED4(M20)。LED5 为 MIO51，属 PS 侧，PL 不可驱动。
    //
    // LED1：约 3 Hz 闪烁，指示时钟 / 复位 / LED 通路正常
    // LED2：DEVID 自检通过（读回 0xE5）时常亮
    // LED3：Z 轴高字节 bit0
    // LED4：Z 轴高字节 bit1
    //
    // Z 轴高字节取自最近一次三轴读取。模块静止平放时 Z 约 +1 g，
    // 全分辨率下 1 LSB = 3.9 mg，1 g = 256 = 0x0100，高字节 0x01，
    // 即 LED4 = 0、LED3 = 1；翻转模块（Z 约 -1 g）高字节 0xFF，
    // 即 LED4 = 1、LED3 = 1。可据此判定三轴读通路是否返回有效数据。
    //----------------------------------------------------------
    reg [7:0] r_z_hi = 8'h00;        // Z 轴高字节（最近一次三轴读取）

    always @(posedge I_sysclk) begin
        if (!r_rstn) r_z_hi <= 8'h00;
        else if (S_data_vld) r_z_hi <= S_acc_z[15:8];
    end

    reg [26:0] r_disp = 27'd0;       // 心跳计时

    always @(posedge I_sysclk) begin
        if (!r_rstn) r_disp <= 27'd0;
        else         r_disp <= r_disp + 1'b1;
    end

    assign O_led[0] = r_disp[24];    // LED1：心跳约 3 Hz
    assign O_led[1] = S_dev_ok;      // LED2：自检通过常亮
    assign O_led[2] = r_z_hi[0];     // LED3：Z 轴高字节 bit0
    assign O_led[3] = r_z_hi[1];     // LED4：Z 轴高字节 bit1

    //----------------------------------------------------------
    // ChipWatcher 在线抓取
    //
    // 用于判定 3 线 SPI 的位级时序：主机是否发出正确命令、片选与时序是否
    // 符合手册、器件是否在读阶段驱动 SDIO。
    //
    // Probe 分配：
    //   Probe0  O_spi_cs_n    片选（低有效）
    //   Probe1  O_spi_sclk    SPI 时钟
    //   Probe2  IO_spi_sdio   数据线（实际电平，含主机与器件驱动）
    //   Probe3  S_busy        命令层忙，用于划分一次传输
    //
    // 采样时钟 100 MHz，采样深度 65536 -> 窗口 655 us，
    // 覆盖一次传输（约 8.3 us）加片选间隔。
    // 推荐触发：Probe0（片选）下降沿，位置 0。
    //
    // 调试完成后应连同本注释一并删除。
    //----------------------------------------------------------
    ui_cwc X_chipwatcher (
        .probe0 (O_spi_cs_n ),   // 1 bit 片选
        .probe1 (O_spi_sclk ),   // 1 bit SPI 时钟
        .probe2 (IO_spi_sdio),   // 1 bit 数据线
        .probe3 (S_busy     ),   // 1 bit 命令层忙
        .clk    (I_sysclk   )    // 100 MHz 采样时钟
    );

endmodule
