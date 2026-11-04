`timescale 1ns/1ps
// Synchronous 8N1 UART. All counters run on clk; no generated clocks.
// RX uses two synchronizer flops, midpoint sampling and stop-bit validation.
module encoder_uart #(
    parameter integer CLK_HZ = 100_000_000,
    parameter integer BAUD = 9600
)(
    input wire clk, input wire rst_n,
    input wire tx_start, input wire [7:0] tx_data,
    output reg tx, output reg tx_busy, output reg tx_done,
    input wire rx, input wire rx_enable,
    output reg [7:0] rx_data, output reg rx_valid,
    output reg rx_error, output wire rx_busy, output wire rx_sync
);
    localparam integer BIT_CLKS = (CLK_HZ + BAUD/2) / BAUD;
    localparam integer HALF_CLKS = BIT_CLKS/2;
    reg [31:0] tx_timer, rx_timer;
    reg [9:0] tx_shift;
    reg [3:0] tx_bit;
    (* async_reg = "true" *) reg [1:0] rx_ff;
    reg [7:0] rx_shift;
    reg [2:0] rx_bit;
    localparam R_IDLE=0, R_START=1, R_DATA=2, R_STOP=3, R_TAIL=4, R_RECOVER=5;
    reg [2:0] rx_state;
    assign rx_sync = rx_ff[1];
    assign rx_busy = rx_state != R_IDLE;

    always @(posedge clk) begin
        if (!rst_n) rx_ff <= 2'b11;
        else rx_ff <= {rx_ff[0], rx_enable ? rx : 1'b1};
    end
    always @(posedge clk) begin
        if (!rst_n) begin
            tx <= 1; tx_busy <= 0; tx_done <= 0;
            tx_timer <= 0; tx_shift <= 10'h3ff; tx_bit <= 0;
        end else begin
            tx_done <= 0;
            if (!tx_busy) begin
                tx <= 1;
                if (tx_start) begin
                    tx_shift <= {1'b1,tx_data,1'b0}; tx <= 0;
                    tx_busy <= 1; tx_timer <= BIT_CLKS-1; tx_bit <= 0;
                end
            end else if (tx_timer != 0) tx_timer <= tx_timer-1'b1;
            else if (tx_bit == 9) begin
                // Busy/done changes only AFTER the complete stop bit.
                tx <= 1; tx_busy <= 0; tx_done <= 1;
            end else begin
                tx_shift <= {1'b1,tx_shift[9:1]}; tx <= tx_shift[1];
                tx_timer <= BIT_CLKS-1; tx_bit <= tx_bit+1'b1;
            end
        end
    end
    always @(posedge clk) begin
        if (!rst_n) begin
            rx_state <= R_RECOVER; rx_timer <= 0;
            rx_data <= 0; rx_valid <= 0; rx_error <= 0;
            rx_bit <= 0; rx_shift <= 0;
        end else if (!rx_enable) begin
            // While transmitting, the synchronized RX is forced to known idle.
            // Arm immediately on DE release, including zero-delay slave replies.
            rx_state <= R_IDLE; rx_timer <= 0;
            rx_valid <= 0; rx_error <= 0; rx_bit <= 0; rx_shift <= 0;
        end else begin
            rx_valid <= 0; rx_error <= 0;
            case (rx_state)
                R_IDLE: if (!rx_sync) begin
                    rx_timer <= HALF_CLKS-1; rx_state <= R_START;
                end
                R_START: if (rx_timer != 0) rx_timer <= rx_timer-1'b1;
                    else if (rx_sync) rx_state <= R_IDLE;
                    else begin rx_timer <= BIT_CLKS-1; rx_bit <= 0; rx_state <= R_DATA; end
                R_DATA: if (rx_timer != 0) rx_timer <= rx_timer-1'b1;
                    else begin
                        rx_shift[rx_bit] <= rx_sync; rx_timer <= BIT_CLKS-1;
                        if (rx_bit == 7) rx_state <= R_STOP;
                        else rx_bit <= rx_bit+1'b1;
                    end
                R_STOP: if (rx_timer != 0) rx_timer <= rx_timer-1'b1;
                    else if (!rx_sync) begin
                        rx_error <= 1; rx_state <= R_RECOVER; rx_timer <= 0;
                    end else begin
                        rx_data <= rx_shift; rx_valid <= 1;
                        rx_timer <= BIT_CLKS-HALF_CLKS-1; rx_state <= R_TAIL;
                    end
                R_TAIL: if (!rx_sync) begin
                        // A slightly faster sender may start its next character
                        // before our nominal stop-tail timer expires. Re-anchor
                        // to that start edge instead of accumulating baud skew.
                        rx_timer <= HALF_CLKS-1; rx_state <= R_START;
                    end else if (rx_timer != 0) rx_timer <= rx_timer-1'b1;
                    else rx_state <= R_IDLE;
                R_RECOVER: begin
                    // BREAK produces one framing error, then requires clean idle.
                    if (!rx_sync) rx_timer <= 0;
                    else if (rx_timer >= BIT_CLKS-1) begin rx_state <= R_IDLE; rx_timer <= 0; end
                    else rx_timer <= rx_timer+1'b1;
                end
                default: rx_state <= R_RECOVER;
            endcase
        end
    end
endmodule
