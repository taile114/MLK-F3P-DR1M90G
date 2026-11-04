// ADS1115 continuous-conversion sequencer. Timers use a 1 us clock enable.
// uii2c packs the first bus byte in the least-significant byte.
// ADS1115 register words are transferred most-significant byte first.
//
// 连续转换：上电写一次 Config（MODE=0），此后按器件数据率周期读 Conversion。
// 不再逐样本写 Config 与轮询 OS，因此接近器件数据率上限；代价是没有 ALERT/RDY
// 时无法用 OS 判定“新样本就绪”，只能按周期读取。
module ads1115_ctrl #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter [6:0] DEV_ADDR = 7'h48,
    parameter [2:0] MUX = 3'b100,
    parameter [2:0] PGA = 3'b001,
    parameter [2:0] DATA_RATE = 3'b111,
    parameter integer INIT_MS = 10,
    parameter integer IIC_TIMEOUT_US = 2000,
    parameter integer RETRY_MS = 100
)(
    input wire I_clk,
    input wire I_rstn,
    output reg O_iic_req,
    output reg [31:0] O_wr_data,
    output reg [7:0] O_wr_cnt,
    output reg [7:0] O_rd_cnt,
    input wire I_iic_busy,
    input wire I_bus_error,
    input wire [15:0] I_rd_data,
    output reg O_iic_rstn,
    output reg signed [15:0] O_adc_raw,
    output reg O_data_vld,
    output reg [15:0] O_config_read,
    output reg O_dev_ok,
    output reg O_error,
    output reg [2:0] O_error_code,
    output reg [15:0] O_error_count,
    output wire O_busy
);
    localparam integer US_CYCLES = CLK_FREQ_HZ / 1_000_000;
    localparam [7:0] ADDR_W = {DEV_ADDR, 1'b0};
    // OS=1, selected MUX/PGA, MODE=0 (continuous), comparator disabled.
    localparam [15:0] CONFIG_WORD = {1'b1, MUX, PGA, 1'b0,
                                     DATA_RATE, 3'b000, 2'b11};
    // Device data rate from the DR field; period is the read cadence.
    localparam integer DR_SPS = (DATA_RATE == 3'b000) ? 8   :
                                (DATA_RATE == 3'b001) ? 16  :
                                (DATA_RATE == 3'b010) ? 32  :
                                (DATA_RATE == 3'b011) ? 64  :
                                (DATA_RATE == 3'b100) ? 128 :
                                (DATA_RATE == 3'b101) ? 250 :
                                (DATA_RATE == 3'b110) ? 475 : 860;
    localparam integer CONV_PERIOD_US = (1_000_000 + DR_SPS - 1) / DR_SPS;
    localparam [3:0] S_POWER = 0, S_CONFIG = 1, S_TRANSFER = 2, S_VERIFY = 3,
                     S_FIRST = 4, S_READ = 5, S_PACE = 6, S_BUS_RESET = 7,
                     S_RETRY = 8;
    localparam [1:0] OP_CFG = 0, OP_VERIFY = 1, OP_READ = 2;
    localparam [2:0] ERR_NACK = 1, ERR_CONFIG = 2, ERR_TRANSFER = 4;

    reg [3:0] r_state;
    reg [1:0] r_operation;
    reg [31:0] r_us_div, r_timer_us;
    reg r_seen_busy, r_transfer_error;
    wire S_us_tick = (r_us_div == US_CYCLES - 1);
    wire [15:0] S_read_word = {I_rd_data[7:0], I_rd_data[15:8]};
    assign O_busy = (r_state != S_POWER && r_state != S_FIRST &&
                     r_state != S_PACE && r_state != S_RETRY);

    // Errors persist until a complete new sample succeeds. The counter is sticky.
    task report_error;
        input [2:0] code;
        begin
            O_iic_req <= 1'b0;
            O_iic_rstn <= 1'b0;
            O_dev_ok <= 1'b0;
            O_error <= 1'b1;
            O_error_code <= code;
            if (O_error_count != 16'hffff)
                O_error_count <= O_error_count + 1'b1;
            r_timer_us <= 0;
            r_state <= S_BUS_RESET;
        end
    endtask

    always @(posedge I_clk) begin
        if (!I_rstn) begin
            r_state <= S_POWER;
            r_operation <= OP_CFG;
            r_us_div <= 0;
            r_timer_us <= 0;
            r_seen_busy <= 0;
            r_transfer_error <= 0;
            O_iic_req <= 0;
            O_iic_rstn <= 0;
            O_wr_data <= 0;
            O_wr_cnt <= 0;
            O_rd_cnt <= 0;
            O_adc_raw <= 0;
            O_data_vld <= 0;
            O_config_read <= 0;
            O_dev_ok <= 0;
            O_error <= 0;
            O_error_code <= 0;
            O_error_count <= 0;
        end else begin
            O_data_vld <= 0;
            r_us_div <= S_us_tick ? 0 : r_us_div + 1'b1;
            if (S_us_tick) r_timer_us <= r_timer_us + 1'b1;
            case (r_state)
                S_POWER: begin
                    O_iic_rstn <= 1;
                    if (r_timer_us >= INIT_MS * 1000) begin
                        r_timer_us <= 0;
                        r_state <= S_CONFIG;
                    end
                end
                S_CONFIG: begin
                    O_wr_data <= {CONFIG_WORD[7:0], CONFIG_WORD[15:8],
                                   8'h01, ADDR_W};
                    O_wr_cnt <= 4;
                    O_rd_cnt <= 0;
                    O_iic_req <= 1;
                    r_seen_busy <= 0;
                    r_transfer_error <= 0;
                    r_operation <= OP_CFG;
                    r_timer_us <= 0;
                    r_state <= S_TRANSFER;
                end
                S_VERIFY: begin
                    O_wr_data <= {16'h0000, 8'h01, ADDR_W};
                    O_wr_cnt <= 2;
                    O_rd_cnt <= 2;
                    O_iic_req <= 1;
                    r_seen_busy <= 0;
                    r_transfer_error <= 0;
                    r_operation <= OP_VERIFY;
                    r_timer_us <= 0;
                    r_state <= S_TRANSFER;
                end
                S_FIRST: begin
                    // First conversion after the config write; then read
                    // once per data-rate period.
                    if (r_timer_us >= CONV_PERIOD_US) begin
                        r_timer_us <= 0;
                        r_state <= S_READ;
                    end
                end
                S_READ: begin
                    O_wr_data <= {16'h0000, 8'h00, ADDR_W};
                    O_wr_cnt <= 2;
                    O_rd_cnt <= 2;
                    O_iic_req <= 1;
                    r_seen_busy <= 0;
                    r_transfer_error <= 0;
                    r_operation <= OP_READ;
                    r_timer_us <= 0;
                    r_state <= S_TRANSFER;
                end
                S_PACE: begin
                    // Pace from the previous read start so reads follow the
                    // device data rate; the read transaction fits inside it.
                    if (r_timer_us >= CONV_PERIOD_US) begin
                        r_timer_us <= 0;
                        r_state <= S_READ;
                    end
                end
                S_TRANSFER: begin
                    // uii2c's ACK error can clear before busy falls; latch it.
                    if (I_bus_error) r_transfer_error <= 1;
                    if (I_iic_busy) begin
                        r_seen_busy <= 1;
                        O_iic_req <= 0;
                    end
                    if (r_seen_busy && !I_iic_busy) begin
                        if (r_transfer_error || I_bus_error)
                            report_error(ERR_NACK);
                        else begin
                            case (r_operation)
                                OP_CFG: r_state <= S_VERIFY;
                                OP_VERIFY: begin
                                    O_config_read <= S_read_word;
                                    // OS is status when read, not a fixed config bit.
                                    if ((S_read_word & 16'h7fff) !=
                                        (CONFIG_WORD & 16'h7fff))
                                        report_error(ERR_CONFIG);
                                    else begin
                                        O_dev_ok <= 1;
                                        r_state <= S_FIRST;
                                    end
                                end
                                OP_READ: begin
                                    O_adc_raw <= S_read_word;
                                    O_data_vld <= 1;
                                    O_error <= 0;
                                    O_error_code <= 0;
                                    r_state <= S_PACE;
                                end
                                default: report_error(ERR_TRANSFER);
                            endcase
                        end
                    end else if (r_timer_us >= IIC_TIMEOUT_US)
                        report_error(ERR_TRANSFER);
                end
                S_BUS_RESET: begin
                    // Reset the vendor sequencer after a failed transaction.
                    if (r_timer_us >= 20) begin
                        O_iic_rstn <= 1;
                        r_timer_us <= 0;
                        r_state <= S_RETRY;
                    end
                end
                S_RETRY: begin
                    if (r_timer_us >= RETRY_MS * 1000) begin
                        r_timer_us <= 0;
                        r_state <= S_CONFIG;
                    end
                end
                default: report_error(ERR_TRANSFER);
            endcase
        end
    end
endmodule
