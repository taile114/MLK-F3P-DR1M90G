`timescale 1ns/1ps
/*******************************ChipWatcher IP 仿真替身*******************************
*File Name: ui_cwc_stub.v
*Description:
*ui_cwc 为 ChipWatcher 调试核，仿真时以空模块替代（综合工程不使用本文件）。
*
*本工程四个探针均为 1 位，对应 SPI 的四根信号线。
*********************************************************************/

module ui_cwc
(
    input [0:0] probe0,
    input [0:0] probe1,
    input [0:0] probe2,
    input [0:0] probe3,
    input       clk
);

    wire unused = &{1'b0, probe0, probe1, probe2, probe3, clk};

endmodule
