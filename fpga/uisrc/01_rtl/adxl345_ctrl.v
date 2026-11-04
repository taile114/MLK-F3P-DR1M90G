/*******************************MILIANKE*******************************
*Company : MiLianKe Electronic Technology Co., Ltd.
*WebSite:https://www.milianke.com
*TechWeb:https://www.uisrc.com
*Create Date: 2026/10/11
*Module Name:adxl345_ctrl
*File Name:adxl345_ctrl.v
*Description:
*ADXL345 命令层：上电自检、寄存器初始化、周期性突发读取三轴加速度。
*SPI 为 3 线模式，DATA_FORMAT 的 D6（SPI 位）置 1。
*
*工作流程：
*  1. 复位结束后等待一段上电稳定时间；
*  2. 先写 DATA_FORMAT（D6 = 1）切换到 3 线 SPI；
*  3. 读 DEVID(0x00)，期望 0xE5，作为 SPI 通信自检；
*  4. 依次写 BW_RATE / INT_ENABLE / POWER_CTL；
*  5. 按 DATA_READY（ADXL345 的 INT1）逐样本突发读
*     DATAX0(0x32) 起 6 个字节，拼成 X / Y / Z。
*
*关键约束（ADXL345 手册）：
*  - POWER_CTL(0x2D) 的 Measure 位必须置 1，否则器件停在待机、数据寄存器不更新；
*  - 三轴必须突发读取，单字节读会丢弃当前样本的剩余字节；
*  - 读数据寄存器后需间隔 >= 5 us 才能再次发起传输，由 SPI 层的片选间隔保证；
*  - 以 DATA_READY 同步读取：手册推荐以中断同步 SPI 事务，避免读到正在更新的
*    寄存器，也避免 PL 采样时钟与器件 ODR 不同源造成的漂移。每个高电平窗口读
*    一次，等于一样本一读，采样率即为 ODR（BW_RATE 设定，上限 3200 Hz）。
*  - 上电等待以微秒为单位：r_usdiv 把系统时钟分频出 1 us 时基，r_timer 只在每个
*    时基脉冲 +1。若令 r_timer 每个系统时钟都 +1（100 MHz 下 1 周期 = 10 ns），
*    INIT_MS = 10 只有 10000 个时钟 = 100 us，DEVID 自检会赶在 ADXL345 上电
*    就绪（约 1.5 ms）前发出而失败。
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
/*********adxl345_ctrl***********
-- 数据格式：全分辨率模式下 1 LSB = 3.9 mg，输出为 16 位有符号补码。
--   加速度(g) = (int16)raw * 0.0039
*********************************************************************/

`timescale 1ns / 1ps
module adxl345_ctrl #(
    parameter CLK_FREQ_HZ = 100_000_000,   // 系统时钟频率
    parameter INIT_MS     = 10             // 上电稳定等待（ms）
)(
    input               I_clk      , // 系统时钟
    input               I_rstn     , // 同步复位，低有效
    input               I_dr       , // DATA_READY（ADXL345 的 INT1），高有效

    // 与 spi_adxl345 的接口
    output reg          O_spi_start,
    output reg [3:0]    O_spi_num  ,
    output reg [7:0]    O_spi_cmd  ,
    output reg [55:0]   O_spi_wdata,
    input               I_spi_busy ,
    input               I_spi_done ,
    input      [55:0]   I_spi_rdata,

    // 结果与状态
    output reg [15:0]   O_acc_x    , // X 轴原始值（16 位有符号补码）
    output reg [15:0]   O_acc_y    , // Y 轴原始值
    output reg [15:0]   O_acc_z    , // Z 轴原始值
    output reg          O_data_vld , // 一次三轴读取完成，单周期脉冲
    output reg          O_dev_ok   , // DEVID 自检通过（读到 0xE5），保持
    output reg [7:0]    O_dev_id   , // 读回的 DEVID
    output reg          O_dev_rd   , // DEVID 读取完成，单周期脉冲
    output reg          O_busy       // 传输进行中
);

    //----------------------------------------------------------
    // 寄存器地址
    //----------------------------------------------------------
    localparam REG_DEVID       = 8'h00;
    localparam REG_BW_RATE     = 8'h2C;
    localparam REG_POWER_CTL   = 8'h2D;
    localparam REG_INT_ENABLE  = 8'h2E;
    localparam REG_DATA_FORMAT = 8'h31;
    localparam REG_DATAX0      = 8'h32;

    localparam DEVID_EXPECT    = 8'hE5;

    // 寄存器写入值
    localparam CFG_BW_RATE     = 8'h0F;   // 3200 Hz 输出数据速率
    localparam CFG_DATA_FORMAT = 8'h4B;   // 全分辨率、±16g、D6=1 选 3 线 SPI
    localparam CFG_INT_ENABLE  = 8'h80;   // DATA_READY 中断输出到 INT1
    localparam CFG_POWER_CTL   = 8'h08;   // Measure 位置 1，进入测量模式

    // 命令字节：bit7 = R/W（1 读 0 写），bit6 = MB（1 多字节），bit5:0 = 地址
    localparam CMD_READ_MB  = 8'hC0;
    localparam CMD_WRITE    = 8'h00;

    localparam US_CYC       = CLK_FREQ_HZ / 1_000_000; // 1 us 对应的系统时钟数
    localparam INIT_CNT     = INIT_MS   * 1000;        // 上电等待（微秒）

    //----------------------------------------------------------
    // 状态机编码
    //----------------------------------------------------------
    localparam S_PWR_WAIT = 4'd0;   // 上电稳定等待
    localparam S_SEND     = 4'd1;   // 发起一次 SPI 传输
    localparam S_WAIT     = 4'd2;   // 等待传输完成
    localparam S_NEXT     = 4'd3;   // 判断下一步
    localparam S_SAMPLE   = 4'd4;   // 采样周期等待

    reg [3:0]  s_state;
    reg [3:0]  r_step ;        // 初始化步骤编号
    reg [31:0] r_timer;        // 上电等待计数（微秒）
    reg [15:0] r_usdiv;        // 1 us 时基分频

    reg [1:0]  r_dr_sync;      // I_dr 两级同步
    reg        r_dr_rd ;       // 当前 DATA_READY 高电平窗口内是否已发起过读取

    wire S_us_tick = (r_usdiv == US_CYC[15:0] - 16'd1);  // 每 US_CYC 个时钟出一个微秒脉冲
    wire S_dr      = r_dr_sync[1];                        // 同步后的 DATA_READY（高有效）

    always @(posedge I_clk) begin
        if (!I_rstn) begin
            s_state     <= S_PWR_WAIT;
            r_step      <= 4'd0;
            r_timer     <= 32'd0;
            r_usdiv     <= 16'd0;
            r_dr_sync   <= 2'd0;
            r_dr_rd     <= 1'b0;
            O_spi_start <= 1'b0;
            O_spi_num   <= 4'd1;
            O_spi_cmd   <= 8'd0;
            O_spi_wdata <= 56'd0;
            O_acc_x     <= 16'd0;
            O_acc_y     <= 16'd0;
            O_acc_z     <= 16'd0;
            O_data_vld  <= 1'b0;
            O_dev_ok    <= 1'b0;
            O_dev_id    <= 8'd0;
            O_dev_rd    <= 1'b0;
            O_busy      <= 1'b0;
        end
        else begin
            O_spi_start <= 1'b0;
            O_data_vld  <= 1'b0;
            O_dev_rd    <= 1'b0;

            // 1 us 时基分频
            r_usdiv <= S_us_tick ? 16'd0 : r_usdiv + 16'd1;
            // DATA_READY 两级同步（异步输入）
            r_dr_sync <= {r_dr_sync[0], I_dr};

            case (s_state)

            //--------------------------------------------------
            // 上电稳定等待
            //--------------------------------------------------
            S_PWR_WAIT: begin
                O_busy <= 1'b0;
                if (r_timer >= INIT_CNT - 1) begin
                    r_timer <= 32'd0;
                    r_step  <= 4'd0;
                    s_state <= S_NEXT;
                end
                else if (S_us_tick) begin
                    r_timer <= r_timer + 1'b1;
                end
            end

            //--------------------------------------------------
            // 发起一次 SPI 传输
            //--------------------------------------------------
            S_SEND: begin
                O_busy      <= 1'b1;
                O_spi_start <= 1'b1;
                s_state     <= S_WAIT;
            end

            //--------------------------------------------------
            // 等待传输完成
            //--------------------------------------------------
            S_WAIT: begin
                if (I_spi_done) s_state <= S_NEXT;
            end

            //--------------------------------------------------
            // 判断下一步
            //--------------------------------------------------
            S_NEXT: begin
                case (r_step)

                // 步骤 0：先写 DATA_FORMAT，置 D6 = 1 进入 3 线 SPI。
                // 器件上电默认 4 线（D6 = 0），此时 SDO（引脚 12）被模块 0 欧姆
                // 接地，读回必为 0；写操作只用 SDI，在 4 线模式下即可完成。
                // 因此必须在任何读之前先切到 3 线。
                4'd0: begin
                    O_spi_num   <= 4'd2;
                    O_spi_cmd   <= CMD_WRITE | REG_DATA_FORMAT;
                    O_spi_wdata <= {48'd0, CFG_DATA_FORMAT};
                    s_state     <= S_SEND;
                end

                // 步骤 1：读 DEVID 自检（此时已是 3 线模式）
                4'd1: begin
                    O_spi_num   <= 4'd2;
                    O_spi_cmd   <= CMD_READ_MB | REG_DEVID;
                    O_spi_wdata <= 56'd0;
                    s_state     <= S_SEND;
                end

                // 步骤 2：自检结果判定，并准备写 BW_RATE
                4'd2: begin
                    O_dev_id <= I_spi_rdata[7:0];
                    O_dev_rd <= 1'b1;                 // 通知上层：已完成一次读取
                    if (I_spi_rdata[7:0] == DEVID_EXPECT) O_dev_ok <= 1'b1;
                    O_spi_num   <= 4'd2;
                    O_spi_cmd   <= CMD_WRITE | REG_BW_RATE;
                    O_spi_wdata <= {48'd0, CFG_BW_RATE};
                    s_state     <= S_SEND;
                end

                // 步骤 3：写 INT_ENABLE
                4'd3: begin
                    O_spi_num   <= 4'd2;
                    O_spi_cmd   <= CMD_WRITE | REG_INT_ENABLE;
                    O_spi_wdata <= {48'd0, CFG_INT_ENABLE};
                    s_state     <= S_SEND;
                end

                // 步骤 4：写 POWER_CTL，进入测量模式
                4'd4: begin
                    O_spi_num   <= 4'd2;
                    O_spi_cmd   <= CMD_WRITE | REG_POWER_CTL;
                    O_spi_wdata <= {48'd0, CFG_POWER_CTL};
                    s_state     <= S_SEND;
                end

                // 步骤 5：初始化结束，进入采样周期
                4'd5: begin
                    s_state <= S_SAMPLE;
                end

                // 步骤 6：周期性突发读三轴
                4'd6: begin
                    O_spi_num   <= 4'd7;                 // 命令 + 6 字节数据
                    O_spi_cmd   <= CMD_READ_MB | REG_DATAX0;
                    O_spi_wdata <= 56'd0;
                    s_state     <= S_SEND;
                end

                // 步骤 7：解析三轴数据，回到采样周期
                4'd7: begin
                    // 读回缓冲低字节在前：rdata[7:0]=X0, [15:8]=X1, ...
                    O_acc_x    <= {I_spi_rdata[15:8], I_spi_rdata[7:0]};
                    O_acc_y    <= {I_spi_rdata[31:24], I_spi_rdata[23:16]};
                    O_acc_z    <= {I_spi_rdata[47:40], I_spi_rdata[39:32]};
                    O_data_vld <= 1'b1;
                    O_busy     <= 1'b0;
                    s_state    <= S_SAMPLE;
                end

                default: s_state <= S_SAMPLE;
                endcase

                if (r_step == 4'd7)      r_step <= 4'd6;
                else if (r_step == 4'd4) r_step <= 4'd5;
                else                     r_step <= r_step + 1'b1;
            end

            //--------------------------------------------------
            // 等待 DATA_READY：每个高电平窗口只发起一次读取
            //
            // DATA_READY 由器件按 ODR 置位，读取数据寄存器后清除；故一个高电平
            // 窗口对应一个新样本，做到"一样本一读"，不受 PL 与器件时钟漂移影响。
            // r_dr_rd 在窗口内置 1 避免重复发起；DATA_READY 回低后清零。
            //--------------------------------------------------
            S_SAMPLE: begin
                if (S_dr && !r_dr_rd) begin
                    r_dr_rd <= 1'b1;
                    r_step  <= 4'd6;
                    s_state <= S_NEXT;
                end
                else if (!S_dr) begin
                    r_dr_rd <= 1'b0;
                end
            end

            default: s_state <= S_PWR_WAIT;
            endcase
        end
    end

endmodule
