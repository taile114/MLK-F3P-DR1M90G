
//////////////////////////////////////////////////////////////////////////////////
/*******************************MILIANKE*******************************
*Company : MiLianKe Electronic Technology Co., Ltd.
*WebSite:https://www.milianke.com
*TechWeb:https://www.uisrc.com
*tmall-shop:https://milianke.tmall.com
*jd-shop:https://milianke.jd.com
*taobao-shop1: https://milianke.taobao.com
*Create Date: 2023/08/07
*Module Name:ui_mspi_tx
*File Name:ui_mspi_tx.v
*Description: 
*The reference demo provided by Milianke is only used for learning. 
*We cannot ensure that the demo itself is free of bugs, so users 
*should be responsible for the technical problems and consequences
*caused by the use of their own products.
*Copyright: Copyright (c) MiLianKe
*All rights reserved.
*Revision: 1.0
*Signal description
*1) I_ input
*2) O_ output
*3) IO_ input output
*4) S_ system internal signal
*5) _n activ low
*6) _dg debug signal 
*7) _r delay or register
*8) _s state mechine
*********************************************************************/
/*******************************SPI Master发送驱动器*********************
--以下是米联客设计的SPI Master发送驱动器
--1.代码简洁，占用极少逻辑资源，代码结构清晰，逻辑设计严谨
--2.I_spi_tx_req，用户程序通过设置I_spi_tx_req为高，请求SPI Master发送驱动器，发送数据，SPI Master发送驱动器把I_spi_tx_data数据通过 O_spi_mosi总线发送出去
--3.通过CPOL以及CPHA自由的控制SPI Master发送驱动器的时钟极性一级时钟相位，并且在时序上做出相应的匹配。
--4.O_spi_busy,表示当前*SPI Master发送总线整忙，这时用户程序需要等待非忙的时候，请求发送数据。
*********************************************************************/
`timescale 1ns / 1ps                                            //定义仿真时间刻度/精度

module ui_mspi_tx#
(
parameter CLK_DIV = 100,
parameter CPOL = 1'b0,                                            //时钟极性参数设置
parameter CPHA = 1'b0                                             //时钟相位参数设置
)
(
input       I_clk,                                                 //系统时钟输入
input       I_rstn,                                                //系统复位输入
output      O_spi_mosi,                                            //发送SPI数据
output      O_spi_sclk,                                            //发送SPI时钟
input       I_spi_tx_req,                                         //发送数据请求
input [7:0] I_spi_tx_data,                                        //发送数据  
output      O_spi_busy                                            //发送状态忙，代表正在发送数据 
);

localparam [9:0] SPI_DIV     = CLK_DIV;                         //第二时钟边沿计数器
localparam [9:0] SPI_DIV1    = SPI_DIV/2;                       //第一时钟边沿计数器

reg [9:0]   clk_div  = 10'd0;   
reg         spi_en   = 1'b0;
reg         spi_clk  = 1'b0;
reg [3:0]   tx_cnt   = 4'd0;
reg [7:0]   spi_tx_data_r=8'd0;
wire        clk_end;
wire        clk_en1;                                           //第一内部时钟边沿使能
wire        clk_en2;                                           //第二内部时钟边沿使能
reg         spi_strobe_en;
wire        spi_strobe;                                        //CPHA＝0数据在第一时钟边沿上传输，CPHA＝1数据在第二时钟边沿上发送

assign      clk_en1     = (clk_div == SPI_DIV1);//第一内部时钟边沿使能
assign      clk_en2     = (clk_div == SPI_DIV);//第二内部时钟边沿使能
assign      clk_end     = (clk_div == SPI_DIV1)&&(tx_cnt==4'd8);
//计数器发送第一个内部时钟0到7次，当计数达到最后8时，不发送时钟//当CPHA＝0时，数据的第一个SCLK转换边缘被采样，因此数据更新在第二个转换边缘上
//当CPHA＝1时，数据的第二个SCLK转换边缘被采样，因此数据更新在第一个转换边缘上
assign      spi_strobe  = CPHA ? clk_en1&spi_strobe_en : clk_en2&spi_strobe_en ;
assign      O_spi_sclk  = (CPOL == 1'b1) ? ~spi_clk : spi_clk;//设置SPI时钟的初始电平
assign      O_spi_mosi  = spi_tx_data_r[7];
assign      O_spi_busy  = spi_en;

always@(posedge I_clk)begin                                   //时钟分频器
    if(spi_en == 1'b0)
        clk_div <= 10'd0;
    else if(clk_div < SPI_DIV)
        clk_div <= clk_div + 1'b1;
    else 
        clk_div <= 0;
end
always@(posedge I_clk)begin                                   //生成spi内部时钟
        if(spi_en == 1'b0)
            spi_clk <= 1'b0;
    else if(clk_en2) 
            spi_clk <= 1'b0;                                   //第二时钟边沿
        else if(clk_en1&&(tx_cnt<4'd8))                       //第一时钟边沿
            spi_clk <= 1'b1; 
    else
        spi_clk <= spi_clk;
end

always@(posedge I_clk)begin  
          if(I_rstn == 1'b0) 
             spi_strobe_en <= 1'b0;
          else if(tx_cnt < 4'd8)begin
               if(clk_en1) spi_strobe_en <= 1'b1;    
          end 
          else 
               spi_strobe_en <= 1'b0;         
end

always@(posedge I_clk)begin  
          if((I_rstn == 1'b0)||(spi_en == 1'b0)) 
             tx_cnt <= 4'd0;
          else if(clk_en1) 
             tx_cnt <= tx_cnt + 1'b1;      
end

always@(posedge I_clk)begin                                           //spi发送模块
    if(I_rstn == 1'b0 || clk_end)begin
        spi_en <= 1'b0;
        spi_tx_data_r <= 8'h00;
    end
    else if(I_spi_tx_req&&(spi_en == 1'b0)) begin                    //启用传输
            spi_en <= 1'b1;
            spi_tx_data_r <= I_spi_tx_data;
    end
    else if(spi_en)begin
         spi_tx_data_r[7:0] <= (spi_strobe) ? {spi_tx_data_r[6:0],1'b1} : spi_tx_data_r;
    end
 
end   


endmodule