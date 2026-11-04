/*******************************MILIANKE*******************************
*Company : MiLianKe Electronic Technology Co., Ltd.
*WebSite:https://www.milianke.com
*TechWeb:https://www.uisrc.com
*Create Date: 2026/10/11
*Module Name:spi_adxl345
*File Name:spi_adxl345.v
*Description:
*SPI 收发层，供 ADXL345 使用，3 线 SPI 模式。
*
*为什么用 3 线：所用 ADXL345 模块原理图上 SDO（ADXL345 引脚 12）经
*R4 = 0 欧姆接地。4 线 SPI 下该脚是主机输入 MISO，恒被拉低，读回全 0；
*I2C 模式把该脚当 ALT ADDRESS 选择脚，接地即地址 0xA6，故 I2C 可用。
*3 线 SPI 不用该脚，数据在 SDA/SDI/SDIO（引脚 13）上双向传输，器件在
*读数据阶段驱动该脚，主机在命令字节与写数据阶段驱动该脚，其余时间释放。
*该用法与手册一致：手册对 3 线 SPI 建议把 SDO 上拉至 VDD I/O 或经
*10 kOhm 下拉至 GND，本模块的 0 欧姆下拉同样满足（该脚在 3 线下不用）。
*
*结构沿用厂商例程 16_spi_loop 的发送驱动 ui_mspi_tx 与接收驱动 ui_spi_rx：
*  - 发送驱动产生 SCLK 并移位 MOSI；3 线模式下 MOSI 即 SDIO 的主机驱动值；
*  - 接收驱动按 CPOL / CPHA 选采样沿，对 SCLK 与片选做多级寄存消亚稳态；
*  - 片选 O_spi_cs_n 由本模块统一控制，一次传输内保持低，传输之间拉高。
*
*手册约束（ADXL345）：
*  - CPOL = 1、CPHA = 1（模式 3）：SCLK 空闲高，数据在下降沿更新、
*    上升沿采样；
*  - CS 低 = SPI 模式，CS 高 = I2C 模式；CS 无默认状态，必须由主机驱动；
*  - 访问不连续寄存器时，两次传输之间必须拉高 CS 并重新发命令字节；
*  - 速率 > 1.6 MHz 时，读数据寄存器后需 CS 拉高并间隔 >= 5 us。
*    本模块的 S_GAP 状态拉高 CS 并等待 DELAY_CNT（5 us），同时满足上述两条。
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
module spi_adxl345 #(
    parameter CLK_FREQ_HZ = 100_000_000,   // 系统时钟频率
    parameter CLK_DIV     = 9,             // SCLK 半周期分频：5 MHz @ 100 MHz
    parameter BYTE_CNT    = 8,             // 一次传输的最大字节数
    parameter DELAY_CNT   = 500            // 片选有效到首个时钟的等待（5 us）
)(
    input               I_clk      , // 系统时钟
    input               I_rstn     , // 同步复位，低有效

    input               I_start    , // 启动一次传输（单周期脉冲）
    input      [3:0]    I_byte_num , // 本次传输字节数，1 ~ BYTE_CNT
    input      [7:0]    I_cmd      , // 命令/地址字节
    input      [8*BYTE_CNT-9:0] I_wr_data, // 待写数据（低字节在前）

    output reg          O_busy     , // 传输进行中
    output reg          O_done     , // 传输完成，单周期脉冲
    output reg [8*BYTE_CNT-9:0] O_rd_data, // 读回数据（低字节在前）

    output reg          O_spi_cs_n , // 片选，低有效（本模块驱动）
    output wire         O_spi_sclk , // SPI 时钟（由厂商发送驱动产生）
    inout               IO_spi_sdio  // SPI 数据（3 线，双向，接模块 SDA/SDI/SDIO）
);

    localparam S_IDLE = 3'd0;
    localparam S_DLY  = 3'd1;   // 片选有效后的等待
    localparam S_TX   = 3'd2;   // 驱动 ui_mspi_tx 发送一个字节
    localparam S_WAIT = 3'd3;   // 等待字节发送完成
    localparam S_BYTE = 3'd4;   // 字节衔接
    localparam S_GAP  = 3'd5;   // 片选拉高后的间隔
    localparam S_DONE = 3'd6;

    reg [2:0]  s_state;
    reg [31:0] r_dly  ;
    reg [3:0]  r_byte ;
    reg [7:0]  r_cmd  ;
    reg [7:0]  r_wbuf [0:7];     // [0] 为命令字节，[1]~[7] 为待写数据
    reg [7:0]  r_rbuf [0:7];     // [0]~[6] 为读回的数据字节
    reg        r_read ;          // 本次传输为读（命令字节 bit7 = 1）
    reg        r_tx_req ;
    reg [7:0]  r_tx_data;
    reg        r_first_byte;
    reg        r_sdio_oe;        // SDIO 输出使能：命令字节与写数据驱动，读数据释放

    wire       w_tx_busy;
    wire       w_mosi;           // 发送驱动输出，经输出使能后送 SDIO

    wire       w_rx_vld_raw;
    wire [7:0] w_rx_dat_raw;

    integer    i;

    //----------------------------------------------------------
    // 发送驱动：沿用厂商 ui_mspi_tx，CPOL = 1、CPHA = 1（模式 3）
    //----------------------------------------------------------
    ui_mspi_tx #(
        .CLK_DIV (CLK_DIV),
        .CPOL    (1'b1),
        .CPHA    (1'b1)
    ) X_ui_mspi_tx (
        .I_clk         (I_clk     ),
        .I_rstn        (I_rstn    ),
        .O_spi_mosi    (w_mosi    ),
        .O_spi_sclk    (O_spi_sclk),
        .I_spi_tx_req  (r_tx_req  ),
        .I_spi_tx_data (r_tx_data ),
        .O_spi_busy    (w_tx_busy )
    );

    //----------------------------------------------------------
    // SDIO 双向：主机驱动命令字节与写数据，读数据阶段释放给器件驱动。
    // 读数据阶段器件在 SCLK 下降沿更新该脚，主机在上升沿采样。
    //----------------------------------------------------------
    assign IO_spi_sdio = r_sdio_oe ? w_mosi : 1'bz;

    //----------------------------------------------------------
    // 接收：使用米联客接收驱动 ui_spi_rx，直接以 SDIO 线上的实际电平为输入。
    //
    // 该驱动与发送驱动 ui_mspi_tx 配套，对 SCLK 与片选做多级寄存消除
    // 亚稳态后按 CPOL / CPHA 选择采样沿，边界情况已在原厂硬件上验证。
    //
    // 两处使用注意：
    //   1. 片选 I_spi_ss 为低有效，与 ADXL345 的 CS 极性一致，直接相连；
    //   2. O_spi_rvalid 由 (spi_bit_cnt == BITS_LEN) 产生，比移位寄存器
    //      收满一字节早一个系统时钟，故在其后一拍再取数据，否则丢最后一位。
    //----------------------------------------------------------
    ui_spi_rx #(
        .BITS_LEN (8),
        .CPOL     (1'b1),          // ADXL345 要求模式 3
        .CPHA     (1'b1)
    ) X_ui_spi_rx (
        .I_clk       (I_clk      ),
        .I_rstn      (I_rstn     ),
        .I_spi_clk   (O_spi_sclk ),
        .I_spi_rx    (IO_spi_sdio),
        .I_spi_ss    (O_spi_cs_n ),
        .O_spi_rvalid(w_rx_vld_raw),
        .O_spi_rdata (w_rx_dat_raw)
    );

    // rvalid 之后一拍取数，此时移位已完成
    reg r_rx_vld = 1'b0;
    reg [7:0] r_rx_dat = 8'd0;

    always @(posedge I_clk) begin
        if (!I_rstn) begin
            r_rx_vld <= 1'b0;
            r_rx_dat <= 8'd0;
        end
        else begin
            r_rx_vld <= w_rx_vld_raw;
            if (w_rx_vld_raw) r_rx_dat <= w_rx_dat_raw;
        end
    end

    //----------------------------------------------------------
    // 读回数据缓冲：最多 7 个数据字节，低字节在前。
    //   固定 56 位宽，未使用的位返回 0。
    //----------------------------------------------------------
    reg [55:0] w_rd_pack;

    always @(*) begin
        w_rd_pack = 56'd0;
        w_rd_pack[7:0]   = r_rbuf[0];
        w_rd_pack[15:8]  = r_rbuf[1];
        w_rd_pack[23:16] = r_rbuf[2];
        w_rd_pack[31:24] = r_rbuf[3];
        w_rd_pack[39:32] = r_rbuf[4];
        w_rd_pack[47:40] = r_rbuf[5];
        w_rd_pack[55:48] = r_rbuf[6];
    end

    //----------------------------------------------------------
    // 主状态机
    //----------------------------------------------------------
    always @(posedge I_clk) begin
        if (!I_rstn) begin
            s_state      <= S_IDLE;
            r_dly        <= 32'd0;
            r_byte       <= 4'd0;
            r_cmd        <= 8'd0;
            r_read       <= 1'b0;
            r_tx_req     <= 1'b0;
            r_tx_data    <= 8'd0;
            r_first_byte <= 1'b1;
            r_sdio_oe    <= 1'b0;
            O_busy       <= 1'b0;
            O_done       <= 1'b0;
            O_rd_data    <= {(8*BYTE_CNT-8){1'b0}};
            O_spi_cs_n   <= 1'b1;
            for (i = 0; i < 8; i = i + 1) begin
                r_wbuf[i] <= 8'd0;
                r_rbuf[i] <= 8'd0;
            end
        end
        else begin
            O_done <= 1'b0;

            case (s_state)

            //--------------------------------------------------
            // 空闲
            //--------------------------------------------------
            S_IDLE: begin
                O_busy     <= 1'b0;
                O_spi_cs_n <= 1'b1;          // 传输之间片选拉高
                r_tx_req   <= 1'b0;
                r_sdio_oe  <= 1'b0;          // 空闲时释放 SDIO
                if (I_start) begin
                    O_busy     <= 1'b1;
                    O_spi_cs_n <= 1'b0;              // 选中器件
                    r_cmd      <= I_cmd;
                    r_read     <= I_cmd[7];
                    r_byte     <= 4'd0;
                    r_first_byte <= 1'b1;
                    r_dly      <= 32'd0;
                    // 装填待写数据：[1]~[7] 依次为 I_wr_data 的第 1~7 字节。
                    // I_wr_data 共 7 字节（56 位），故下标 i 取 0~6 对应缓冲 [1]~[7]。
                    for (i = 0; i < 7; i = i + 1) begin
                        r_wbuf[i + 1] <= I_wr_data[8*i +: 8];
                    end
                    r_wbuf[0] <= I_cmd;
                    s_state   <= S_DLY;
                end
            end

            //--------------------------------------------------
            // 片选有效到首个时钟之间的等待
            //--------------------------------------------------
            S_DLY: begin
                if (r_dly >= DELAY_CNT) begin
                    r_dly   <= 32'd0;
                    s_state <= S_TX;
                end
                else begin
                    r_dly <= r_dly + 1'b1;
                end
            end

            //--------------------------------------------------
            // 发起一个字节的发送
            //
            // 输出使能：命令字节（r_byte = 0）与写数据字节由主机驱动；
            // 读数据字节释放 SDIO，让器件驱动。
            //--------------------------------------------------
            S_TX: begin
                if (!w_tx_busy) begin
                    r_tx_req  <= 1'b1;
                    r_tx_data <= r_first_byte ? r_cmd : r_wbuf[r_byte[2:0]];
                    r_sdio_oe <= (r_byte == 4'd0) || (!r_read);
                    r_first_byte <= 1'b0;
                    s_state   <= S_WAIT;
                end
            end

            //--------------------------------------------------
            // 等待本字节发送完成
            //--------------------------------------------------
            S_WAIT: begin
                if (w_tx_busy) r_tx_req <= 1'b0;   // 已启动，撤销请求
                else if (!r_tx_req) s_state <= S_BYTE;
            end

            //--------------------------------------------------
            // 字节衔接
            //
            // 第 0 字节为命令，不回存；其后每个字节的接收结果依次存入
            // r_rbuf[0] 起，故下标为 r_byte-1。
            //--------------------------------------------------
            S_BYTE: begin
                if (r_byte != 4'd0) r_rbuf[r_byte[2:0] - 1'b1] <= r_rx_dat;

                if (r_byte == I_byte_num - 1'b1) begin
                    s_state <= S_GAP;
                end
                else begin
                    r_byte  <= r_byte + 1'b1;
                    s_state <= S_TX;
                end
            end

            //--------------------------------------------------
            // 片选拉高并等待间隔，满足手册 5 us 要求
            //--------------------------------------------------
            S_GAP: begin
                O_spi_cs_n <= 1'b1;
                r_sdio_oe  <= 1'b0;
                if (r_dly >= DELAY_CNT) begin
                    r_dly   <= 32'd0;
                    s_state <= S_DONE;
                end
                else begin
                    r_dly <= r_dly + 1'b1;
                end
            end

            //--------------------------------------------------
            // 收尾
            //--------------------------------------------------
            S_DONE: begin
                O_busy    <= 1'b0;
                O_done    <= 1'b1;
                O_rd_data <= w_rd_pack;    // 最多 7 个数据字节，低字节在前
                s_state   <= S_IDLE;
            end

            default: s_state <= S_IDLE;
            endcase
        end
    end

endmodule
