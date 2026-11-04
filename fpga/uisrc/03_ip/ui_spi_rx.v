
//////////////////////////////////////////////////////////////////////////////////
/*******************************MILIANKE*******************************
*Company : MiLianKe Electronic Technology Co., Ltd.
*WebSite:https://www.milianke.com
*TechWeb:https://www.uisrc.com
*tmall-shop:https://milianke.tmall.com
*jd-shop:https://milianke.jd.com
*taobao-shop1: https://milianke.taobao.com
*Create Date: 2023/08/07
*Module Name:uispi_rx
*File Name:uispi_rx.v
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
/*******************************SPI接收驱动*********************
--以下是米联客设计的SPI接收驱动器
--1.代码简洁，占用极少逻辑资源，代码结构清晰，逻辑设计严谨
--2.O_spi_rvalid有效，代表数据O_spi_rdata有效
--3.SPI的时钟以及选通总线进行采样是异步采样，因此多次寄存消除亚稳态
*********************************************************************/
`timescale 1ns / 1ns//仿真时间刻度/精度

module ui_spi_rx#
(
parameter BITS_LEN = 8,
parameter CPOL = 1'b0,
parameter CPHA = 1'b0
)
(
input        I_clk,//系统时钟输入
input        I_rstn,//系统复位输入
input        I_spi_clk,//SPI时钟输入
input        I_spi_rx,//SPI rx数据输入
input        I_spi_ss,//SPI片选信号
output       O_spi_rvalid, //SPI rx 接收数据有效信号，当为1的时候O_spi_rdata数据有效
output [BITS_LEN-1'b1:0] O_spi_rdata//SPI rx接收到的数据输出
 );

reg  spi_cap   = 1'b0;
reg  [3:0]spi_clk_r = 4'd0;
reg  [4:0] spi_bit_cnt = 5'd0;
reg  [BITS_LEN-1'b1:0] spi_rx_r1;
reg  [3:0]spi_ss_r=4'd0;

wire spi_rx_en ;
wire spi_clkp ;
wire spi_clkn ;

assign O_spi_rdata  = spi_rx_r1;
assign O_spi_rvalid = (spi_bit_cnt == BITS_LEN);

assign spi_clkp   = spi_clk_r[3:2]==2'b01;                              //SPI时钟信号上升沿
assign spi_clkn   = spi_clk_r[3:2]==2'b10;                              //SPI时钟信号下降沿

assign spi_rx_en  = (~spi_ss_r[3]);                          //I_spi_ss片选信号持续拉低，使能拉高，接收启动

always @(posedge I_clk or negedge I_rstn)begin                       //SPI时钟信号，进行异步转同步处理
   if(I_rstn == 1'b0)
      spi_clk_r <= 4'd0;
   else
      spi_clk_r <= {spi_clk_r[2:0],I_spi_clk};
end

always @(posedge I_clk or negedge I_rstn)begin
   if(I_rstn == 1'b0)
      spi_ss_r <= 4'd0;
   else
      spi_ss_r <= {spi_ss_r[2:0],I_spi_ss};                               //将I_spi_ss接收到的数据进行缓存
end
//当总线空闲时，当CPHA＝1时，SCL＝1；当CPHA=0时，则SCL=0
//CPOL用于控制第一时钟样本或第二时钟样本
//capture stroble 设置
always @(*)begin
      if(CPHA)begin 
         if(CPOL) spi_cap = spi_clkp;//CPHA=1  CPOL=1 
         else  spi_cap = spi_clkn;   //CPHA=1  CPOL=0
      end
      else begin 
         if(CPOL) spi_cap = spi_clkn;//CPHA=0  CPOL=1 
         else  spi_cap = spi_clkp;   //CPHA=0  CPOL=0    
      end
end

//spi bit counter
always @(posedge I_clk)begin
    if(spi_rx_en&&spi_cap&&(spi_bit_cnt < BITS_LEN))       //计数到未到达参数BITS_LEN设定值
       spi_bit_cnt <= spi_bit_cnt + 1'b1;                    //spi_bit_cnt计数器+1
    else if(spi_rx_en==0||spi_bit_cnt == BITS_LEN)          //单次传输的长度由参数BITS_LEN来控制
       spi_bit_cnt <= 0;                                        //计数到达设定值，计数清零
end          

//spi bit shift
always @(posedge I_clk)begin
     if(spi_rx_en&&spi_cap)                                         //spi_cap信号有效时，进行数据采样
        spi_rx_r1 <= {spi_rx_r1[BITS_LEN-2:0],I_spi_rx};         //采样的数据进行移位，准备进行下次采样  
     else if(spi_rx_en == 1'b0)                                    //spi_rx_en拉低，采样结束
        spi_rx_r1 <= 0;                                             //spi_rx_r1清零
end
endmodule
