// UART 8N1 wrapper around the unchanged Milianke receiver.
// The vendor valid pulse occurs near the middle of the stop bit.
module uart_rx_guard #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer BAUD = 115200,
    parameter integer BREAK_BITS = 12
)(
    input wire I_clk,
    input wire I_rstn,
    input wire I_uart_rx,
    output reg [7:0] O_byte,
    output reg O_byte_vld,
    output reg O_error,
    output reg [1:0] O_error_code,
    output wire O_ready,
    output wire O_rx_sync
);
    localparam integer BIT_CYCLES = CLK_FREQ_HZ / BAUD;
    localparam integer BREAK_CYCLES = BIT_CYCLES * BREAK_BITS;
    localparam integer RESET_HOLD_CYCLES = BIT_CYCLES * 10;
    reg [1:0] r_rx_sync = 2'b11;
    reg r_recover, r_break_reported;
    reg [31:0] r_high_cycles, r_low_cycles, r_reset_cycles;
    wire [7:0] S_vendor_byte;
    wire S_vendor_valid;
    assign O_rx_sync = r_rx_sync[1];
    assign O_ready = I_rstn && !r_recover;

    always @(posedge I_clk) begin
        if (!I_rstn) r_rx_sync <= 2'b11;
        else r_rx_sync <= {r_rx_sync[0], I_uart_rx};
    end
    ui_uart_rx #(.BAUD_DIV(BIT_CYCLES - 1)) X_ui_uart_rx (
        .I_clk(I_clk), .I_uart_rstn(O_ready), .I_uart_rx(O_rx_sync),
        .O_uart_rdata(S_vendor_byte), .O_uart_rvalid(S_vendor_valid)
    );
    always @(posedge I_clk) begin
        if (!I_rstn) begin
            O_byte <= 0;
            O_byte_vld <= 0;
            O_error <= 0;
            O_error_code <= 0;
            r_recover <= 1;
            r_break_reported <= 0;
            r_high_cycles <= 0;
            r_low_cycles <= 0;
            r_reset_cycles <= RESET_HOLD_CYCLES;
        end else begin
            O_byte_vld <= 0;
            O_error <= 0;
            O_error_code <= 0;
            if (O_rx_sync) begin
                r_low_cycles <= 0;
                r_break_reported <= 0;
            end else if (r_low_cycles < BREAK_CYCLES)
                r_low_cycles <= r_low_cycles + 1'b1;

            if (r_recover) begin
                // Discard the remaining character if reset occurs mid-byte.
                // Then require one idle bit before releasing the vendor core.
                if (r_reset_cycles != 0) begin
                    r_reset_cycles <= r_reset_cycles - 1'b1;
                    r_high_cycles <= 0;
                end else if (!O_rx_sync) r_high_cycles <= 0;
                else if (r_high_cycles >= BIT_CYCLES - 1) begin
                    r_recover <= 0;
                    r_high_cycles <= 0;
                end else r_high_cycles <= r_high_cycles + 1'b1;
            end else if (S_vendor_valid) begin
                if (O_rx_sync) begin
                    O_byte <= S_vendor_byte;
                    O_byte_vld <= 1;
                end else begin
                    // Reject a byte whose stop bit is low; await clean idle.
                    O_error <= 1;
                    O_error_code <= 1;
                    r_recover <= 1;
                    r_high_cycles <= 0;
                end
            end
            if (!O_rx_sync && r_low_cycles >= BREAK_CYCLES - 1 &&
                !r_break_reported) begin
                O_error <= 1;
                O_error_code <= 2;
                r_break_reported <= 1;
                r_recover <= 1;
                r_high_cycles <= 0;
            end
        end
    end
endmodule
