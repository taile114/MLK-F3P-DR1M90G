// 四路时域特征：通道 X/Y/Z/电流，各 448 bit；共享时钟，独立采样节奏。
module sensor_features #(
    parameter integer WINDOW_LOG2=8,
    parameter integer HOP=128
)(
    input wire I_clk, I_rstn,
    input wire I_acc_valid, I_current_valid,
    input wire [15:0] I_acc_x,I_acc_y,I_acc_z,I_current,
    output wire [3:0] O_ready,O_valid,O_overrun,
    output wire [7:0] O_flags,
    output wire [1791:0] O_features
);
    wire [63:0] S_samples={I_current,I_acc_z,I_acc_y,I_acc_x};
    wire [3:0] S_valid={I_current_valid,{3{I_acc_valid}}};
    genvar c;
    generate for(c=0;c<4;c=c+1) begin: gen_channel
        time_features #(.WINDOW_LOG2(WINDOW_LOG2),.HOP(HOP)) X_feature(
            .I_clk(I_clk),.I_rstn(I_rstn),.I_valid(S_valid[c]),
            .I_sample(S_samples[c*16 +: 16]),.O_ready(O_ready[c]),
            .O_valid(O_valid[c]),.O_overrun(O_overrun[c]),
            .O_flags(O_flags[c*2 +: 2]),.O_features(O_features[c*448 +: 448]));
    end endgenerate
endmodule
