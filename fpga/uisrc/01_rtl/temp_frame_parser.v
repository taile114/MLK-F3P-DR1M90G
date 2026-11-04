// Strict ASCII grammar: A[+-][0-9][0-9].[0-9]B CR LF.
// The output is signed tenths of a degree Celsius; no floating point.
module temp_frame_parser #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer BYTE_TIMEOUT_US = 2000,
    // Reporting period is not specified by the available module protocol.
    // Zero disables stream timeout until the actual reporting period is known.
    parameter integer STREAM_TIMEOUT_MS = 0
)(
    input wire I_clk,
    input wire I_rstn,
    input wire [7:0] I_byte,
    input wire I_byte_vld,
    input wire I_uart_error,
    input wire [1:0] I_uart_error_code,
    output reg signed [15:0] O_temp_x10,
    output reg O_temp_vld,
    output reg O_seen_frame,
    output reg O_online,
    output reg O_error,
    output reg [2:0] O_error_code,
    output reg [15:0] O_error_count,
    output reg [31:0] O_frame_count
);
    localparam integer US_CYCLES = CLK_FREQ_HZ / 1_000_000;
    localparam [3:0] S_FIND = 0, S_SIGN = 1, S_TENS = 2, S_ONES = 3,
                     S_DOT = 4, S_TENTH = 5, S_B = 6, S_CR = 7, S_LF = 8;
    reg [3:0] r_state;
    reg r_negative, r_stale_reported;
    reg [15:0] r_magnitude;
    reg [31:0] r_us_div, r_gap_us, r_stream_us;
    wire S_us_tick = (r_us_div == US_CYCLES - 1);
    wire S_digit = (I_byte >= 8'h30 && I_byte <= 8'h39);
    wire [15:0] S_digit_value = {12'd0, I_byte[3:0]};
    task fail_frame;
        input [2:0] code;
        begin
            r_state <= S_FIND;
            r_gap_us <= 0;
            O_online <= 0;
            O_error <= 1;
            O_error_code <= code;
            if (O_error_count != 16'hffff)
                O_error_count <= O_error_count + 1'b1;
        end
    endtask
    always @(posedge I_clk) begin
        if (!I_rstn) begin
            r_state <= S_FIND;
            r_negative <= 0;
            r_magnitude <= 0;
            r_us_div <= 0;
            r_gap_us <= 0;
            r_stream_us <= 0;
            r_stale_reported <= 0;
            O_temp_x10 <= 0;
            O_temp_vld <= 0;
            O_seen_frame <= 0;
            O_online <= 0;
            O_error <= 0;
            O_error_code <= 0;
            O_error_count <= 0;
            O_frame_count <= 0;
        end else begin
            O_temp_vld <= 0;
            r_us_div <= S_us_tick ? 0 : r_us_div + 1'b1;
            if (S_us_tick) begin
                if (r_state != S_FIND && r_gap_us < BYTE_TIMEOUT_US)
                    r_gap_us <= r_gap_us + 1'b1;
                if (STREAM_TIMEOUT_MS > 0 &&
                    r_stream_us < STREAM_TIMEOUT_MS * 1000)
                    r_stream_us <= r_stream_us + 1'b1;
            end
            if (I_uart_error) begin
                fail_frame(I_uart_error_code == 2 ? 3'd2 : 3'd1);
            end else if (I_byte_vld) begin
                r_gap_us <= 0;
                if (I_byte == 8'h41) begin
                    if (r_state != S_FIND) fail_frame(3'd3);
                    // A also resynchronizes an interrupted frame.
                    r_state <= S_SIGN;
                    r_negative <= 0;
                    r_magnitude <= 0;
                end else case (r_state)
                    S_FIND: begin end
                    S_SIGN: begin
                        if (I_byte == 8'h2b || I_byte == 8'h2d) begin
                            r_negative <= (I_byte == 8'h2d);
                            r_state <= S_TENS;
                        end else fail_frame(3'd3);
                    end
                    S_TENS: begin
                        if (S_digit) begin
                            r_magnitude <= S_digit_value * 16'd100;
                            r_state <= S_ONES;
                        end else fail_frame(3'd3);
                    end
                    S_ONES: begin
                        if (S_digit) begin
                            r_magnitude <= r_magnitude + S_digit_value * 16'd10;
                            r_state <= S_DOT;
                        end else fail_frame(3'd3);
                    end
                    S_DOT: begin
                        if (I_byte == 8'h2e) r_state <= S_TENTH;
                        else fail_frame(3'd3);
                    end
                    S_TENTH: begin
                        if (S_digit) begin
                            r_magnitude <= r_magnitude + S_digit_value;
                            r_state <= S_B;
                        end else fail_frame(3'd3);
                    end
                    S_B: begin
                        if (I_byte == 8'h42) r_state <= S_CR;
                        else fail_frame(3'd3);
                    end
                    S_CR: begin
                        if (I_byte == 8'h0d) r_state <= S_LF;
                        else fail_frame(3'd3);
                    end
                    S_LF: begin
                        if (I_byte == 8'h0a) begin
                            O_temp_x10 <= r_negative ? -$signed(r_magnitude) :
                                                       $signed(r_magnitude);
                            O_temp_vld <= 1;
                            O_seen_frame <= 1;
                            O_online <= 1;
                            O_error <= 0;
                            O_error_code <= 0;
                            O_frame_count <= O_frame_count + 1'b1;
                            r_stream_us <= 0;
                            r_stale_reported <= 0;
                            r_state <= S_FIND;
                        end else fail_frame(3'd3);
                    end
                    default: fail_frame(3'd3);
                endcase
            end else if (r_state != S_FIND && r_gap_us >= BYTE_TIMEOUT_US)
                fail_frame(3'd4);
            else if (STREAM_TIMEOUT_MS > 0 && !r_stale_reported &&
                     r_stream_us >= STREAM_TIMEOUT_MS * 1000) begin
                fail_frame(3'd5);
                r_stale_reported <= 1;
            end
        end
    end
endmodule
