`default_nettype none
// 共同时基与 1 kHz 对齐帧；同拍 valid 纳入当前帧。
// 采集器保持独立节奏；温度为到达即打戳的准同步通道。
module timebase #(
    parameter integer CLK_FREQ_HZ = 100_000_000
)(
    input wire I_clk, I_rstn,
    input wire I_enable, I_clear,
    input wire I_temp_valid, I_acc_valid, I_current_valid,
    input wire signed [15:0] I_temp, I_acc_x, I_acc_y, I_acc_z, I_current,
    output wire O_tick,
    output wire [31:0] O_ts, O_ms,
    output reg [31:0] O_frame_ts,
    output reg signed [15:0] O_frame_temp, O_frame_acc_x, O_frame_acc_y,
    output reg signed [15:0] O_frame_acc_z, O_frame_current,
    output reg [31:0] O_acc_ts, O_current_ts, O_temp_ts
);
    localparam integer TICK_CYCLES = CLK_FREQ_HZ / 1000;
    localparam integer DIV_WIDTH = TICK_CYCLES > 1 ? $clog2(TICK_CYCLES) : 1;
    localparam integer DIV_LAST = TICK_CYCLES - 1;
    localparam [DIV_WIDTH-1:0] LAST_CYCLE = DIV_LAST[DIV_WIDTH-1:0];
    reg [DIV_WIDTH-1:0] r_div;
    reg [31:0] r_ts, r_ms;
    reg signed [15:0] r_temp, r_x, r_y, r_z, r_current;
    reg [31:0] r_acc_ts, r_current_ts, r_temp_ts;

    // tick 是时钟使能：高电平所覆盖的上升沿执行帧锁存。
    assign O_tick = I_rstn && (r_div == LAST_CYCLE);
    assign O_ts = r_ts;
    assign O_ms = r_ms;
    always @(posedge I_clk) begin
        if (!I_rstn) begin
            r_div <= 0;
            r_ts <= 0;
            r_ms <= 0;
        end else begin
            r_ts <= r_ts + 32'd1;
            if (O_tick) begin
                r_div <= 0;
                r_ms <= r_ms + 32'd1;
            end else r_div <= r_div + 1'b1;
        end
    end

    always @(posedge I_clk) begin
        if (!I_rstn || I_clear) begin
            r_temp <= 0; r_x <= 0; r_y <= 0; r_z <= 0; r_current <= 0;
            r_acc_ts <= 0; r_current_ts <= 0; r_temp_ts <= 0;
            O_frame_ts <= 0;
            O_frame_temp <= 0; O_frame_acc_x <= 0; O_frame_acc_y <= 0;
            O_frame_acc_z <= 0; O_frame_current <= 0;
            O_acc_ts <= 0; O_current_ts <= 0; O_temp_ts <= 0;
        end else if (I_enable) begin
            if (I_temp_valid) begin r_temp <= I_temp; r_temp_ts <= r_ts; end
            if (I_acc_valid) begin
                r_x <= I_acc_x; r_y <= I_acc_y; r_z <= I_acc_z; r_acc_ts <= r_ts;
            end
            if (I_current_valid) begin r_current <= I_current; r_current_ts <= r_ts; end
            if (O_tick) begin
                O_frame_ts <= r_ms + 32'd1;
                // 旁路避免非阻塞赋值使同拍 valid 延后一帧。
                O_frame_temp <= I_temp_valid ? I_temp : r_temp;
                O_frame_acc_x <= I_acc_valid ? I_acc_x : r_x;
                O_frame_acc_y <= I_acc_valid ? I_acc_y : r_y;
                O_frame_acc_z <= I_acc_valid ? I_acc_z : r_z;
                O_frame_current <= I_current_valid ? I_current : r_current;
                O_acc_ts <= I_acc_valid ? r_ts : r_acc_ts;
                O_current_ts <= I_current_valid ? r_ts : r_current_ts;
                O_temp_ts <= I_temp_valid ? r_ts : r_temp_ts;
            end
        end
    end
    // synthesis translate_off
    initial begin
        if (CLK_FREQ_HZ < 1000 || CLK_FREQ_HZ % 1000 != 0)
            $fatal(1, "CLK_FREQ_HZ must be a positive multiple of 1000");
    end
    // synthesis translate_on
endmodule
`default_nettype wire
