`timescale 1ns/1ps
/*******************************ADXL345 从机模型（3 线 SPI）*******************************
*File Name: adxl345_slave_model.v
*Description:
*ADXL345 的 3 线 SPI 从机行为模型，用于验证主机侧 RTL 的收发与命令流程。
*
*3 线模式：模块 SDA/SDI/SDIO（ADXL345 引脚 13）为双向数据线。
*  - 命令字节与写数据字节由主机驱动，从机只采样；
*  - 读数据字节由从机驱动，主机在字节之间释放该线；
*  - SDO（引脚 12）在 3 线模式不使用。
*
*器件接口模式由 DATA_FORMAT(0x31) 的 D6（SPI 位）决定：上电默认 D6 = 0（4 线），
*此时模块 SDO（引脚 12）被 0 欧姆接地，主机 MISO 恒为 0。本模型据此建模：
*D6 = 0 时读数据字节驱动数据线为 0；D6 = 1 时才送出寄存器值。故命令层必须先写
*DATA_FORMAT 置 D6 = 1，再读 DEVID 与三轴。
*
*时序（手册模式 3，CPOL = 1、CPHA = 1）：
*  SCLK 空闲为高；数据在 SCLK 下降沿更新，在上升沿采样。
*  从机在上升沿采样 SDI；在下降沿更新 SDIO（读阶段驱动）。
*
*本模型以系统时钟 I_clk 工作（与 DUT 同源），对 SCLK 与片选各寄存一拍做边沿
*检测，避免与 DUT 的非阻塞赋值竞争导致差一位。检测延迟 1 拍（10 ns），远小于
*半周期 100 ns。
*
*支持命令：bit7 = R/W，bit6 = MB，bit5:0 = 地址；支持多字节地址自增。
*********************************************************************/

module adxl345_slave_model #(
    parameter [15:0] X_VAL = 16'h0123,
    parameter [15:0] Y_VAL = 16'hFEDC,
    parameter [15:0] Z_VAL = 16'h0100,
    parameter [7:0]  DEVID = 8'hE5
)(
    input        I_clk      ,
    input        I_spi_cs_n ,
    input        I_spi_sclk ,
    inout        IO_spi_sdio,
    output [7:0] O_mon_cmd  ,
    output [7:0] O_mon_addr ,
    output       O_mon_wr   ,
    output       O_int1       // DATA_READY（高有效）
);

    //----------------------------------------------------------
    // 寄存器
    //----------------------------------------------------------
    reg [7:0] regs [0:127];

    integer i;
    initial begin
        for (i = 0; i < 128; i = i + 1) regs[i] = 8'h00;
        regs[8'h00] = DEVID;
        regs[8'h2C] = 8'h0A;
        regs[8'h2D] = 8'h00;
        regs[8'h2E] = 8'h00;
        regs[8'h31] = 8'h00;
        regs[8'h32] = X_VAL[7:0];
        regs[8'h33] = X_VAL[15:8];
        regs[8'h34] = Y_VAL[7:0];
        regs[8'h35] = Y_VAL[15:8];
        regs[8'h36] = Z_VAL[7:0];
        regs[8'h37] = Z_VAL[15:8];
    end

    //----------------------------------------------------------
    // 事务状态
    //----------------------------------------------------------
    reg        sclk_q = 1'b1;   // 上一拍 SCLK，用于边沿检测
    reg [7:0]  r_sdi  = 8'h00;  // 采样移位寄存器
    reg [7:0]  r_sdo  = 8'h00;  // 发送移位寄存器（bit7 输出）
    reg [7:0]  r_sdo_val = 8'h00; // 下一数据字节的发送值
    reg [3:0]  r_bit  = 4'd0;   // 字节内已采样位数 0~7
    reg [3:0]  r_obit = 4'd0;   // 字节内已输出位数 0~7
    reg        r_drive = 1'b0;  // 读数据阶段驱动 SDIO
    reg [7:0]  r_bcnt = 8'd0;   // 已完成的字节数
    reg [7:0]  r_addr = 8'h00;
    reg        r_read = 1'b0;
    reg [7:0]  r_cmd  = 8'h00;

    // DATA_READY：按 ODR 周期置位，读数据寄存器（0x32~0x37）后清除
    localparam integer DR_CYC = 31250;   // 100 MHz / 3200 Hz = 312.5 us
    reg [15:0] r_dr_cnt = 16'd0;
    reg        r_dr     = 1'b0;

    assign O_int1 = r_dr;

    assign IO_spi_sdio = r_drive ? (regs[8'h31][6] ? r_sdo[7] : 1'b0) : 1'bz;

    assign O_mon_cmd  = r_cmd;
    assign O_mon_addr = r_addr;
    assign O_mon_wr   = ~r_read;

    //----------------------------------------------------------
    // 边沿处理（阻塞赋值，单块内完成，避免多驱动与竞争）
    //----------------------------------------------------------
    always @(posedge I_clk) begin
        // DATA_READY 节拍（与片选无关）
        if (r_dr_cnt == DR_CYC - 1) begin r_dr_cnt = 16'd0; r_dr = 1'b1; end
        else                              r_dr_cnt = r_dr_cnt + 16'd1;

        if (I_spi_cs_n) begin
            // 片选拉高：事务结束，清状态
            r_sdi    = 8'h00;
            r_sdo    = 8'h00;
            r_sdo_val= 8'h00;
            r_bit    = 4'd0;
            r_obit   = 4'd0;
            r_drive  = 1'b0;
            r_bcnt   = 8'd0;
            r_read   = 1'b0;
            r_addr   = 8'h00;
        end
        else begin
            //--------------------------------------------------
            // SCLK 上升沿：采样 SDI
            //--------------------------------------------------
            if (I_spi_sclk && !sclk_q) begin
                r_sdi = {r_sdi[6:0], IO_spi_sdio};
                if (r_bit == 4'd7) begin
                    r_bit = 4'd0;
                    r_obit = 4'd0;
                    if (r_bcnt == 8'd0) begin
                        // 命令字节
                        r_cmd     = r_sdi;
                        r_read    = r_sdi[7];
                        r_addr    = r_sdi[5:0];
                        r_sdo_val = r_sdi[7] ? regs[r_sdi[5:0]] : 8'h00;
                    end
                    else if (r_read) begin
                        // 读数据：地址自增，预取下一字节
                        if (r_addr >= 8'h32 && r_addr <= 8'h37) r_dr = 1'b0; // 读数据寄存器清除 DATA_READY
                        r_addr    = r_addr + 8'd1;
                        r_sdo_val = regs[r_addr];
                    end
                    else begin
                        // 写数据：写寄存器，地址自增
                        regs[r_addr] = r_sdi;
                        r_addr       = r_addr + 8'd1;
                    end
                    r_bcnt = r_bcnt + 8'd1;
                end
                else begin
                    r_bit = r_bit + 4'd1;
                end
            end

            //--------------------------------------------------
            // SCLK 下降沿：读数据阶段更新并驱动 SDIO
            //--------------------------------------------------
            if (!I_spi_sclk && sclk_q) begin
                if (r_read && (r_bcnt != 8'd0)) begin
                    if (r_obit == 4'd0) begin
                        r_sdo   = r_sdo_val;   // 装载，bit7 先出
                        r_drive = 1'b1;
                        r_obit  = 4'd1;
                    end
                    else begin
                        r_sdo  = {r_sdo[6:0], 1'b0};
                        r_obit = r_obit + 4'd1;
                    end
                end
            end
        end

        sclk_q = I_spi_sclk;
    end

endmodule
