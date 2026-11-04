`timescale 1ns/1ps
// 仅仿真：集成顶层临时 14 探针 CWC 的接口替身。
module ui_cwc(
    input wire [15:0] probe0,
    input wire probe1, probe2,
    input wire [15:0] probe3, probe4, probe5,
    input wire probe6, probe7,
    input wire [7:0] probe8,
    input wire [15:0] probe9,
    input wire probe10, probe11, probe12,
    input wire [2:0] probe13,
    input wire clk
);
endmodule
