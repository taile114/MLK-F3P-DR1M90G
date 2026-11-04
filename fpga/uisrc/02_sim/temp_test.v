// GT-M601 serial acquisition board -> F3P PL, UART RX at J12.5 / T9.
module temp_test #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer BAUD = 115200,
    parameter integer BYTE_TIMEOUT_US = 2000,
    parameter integer STREAM_TIMEOUT_MS = 0,
    parameter integer VALID_LED_MS = 100
)(
    input wire I_sysclk,
    input wire I_uart_rx,
    output wire [3:0] O_led
);
    reg [9:0] r_reset_count = 0;
    wire S_rstn = r_reset_count[9];
    always @(posedge I_sysclk)
        if (!S_rstn) r_reset_count <= r_reset_count + 1'b1;

    wire [7:0] S_uart_byte;
    wire S_uart_byte_vld, S_uart_error, S_uart_ready, S_uart_rx_sync;
    wire [1:0] S_uart_error_code;
    uart_rx_guard #(.CLK_FREQ_HZ(CLK_FREQ_HZ), .BAUD(BAUD)) X_uart_rx_guard (
        .I_clk(I_sysclk), .I_rstn(S_rstn), .I_uart_rx(I_uart_rx),
        .O_byte(S_uart_byte), .O_byte_vld(S_uart_byte_vld),
        .O_error(S_uart_error), .O_error_code(S_uart_error_code),
        .O_ready(S_uart_ready), .O_rx_sync(S_uart_rx_sync)
    );
    wire signed [15:0] S_temp_x10;
    wire S_temp_vld, S_seen_frame, S_online, S_error;
    wire [2:0] S_error_code;
    wire [15:0] S_error_count;
    wire [31:0] S_frame_count;
    temp_frame_parser #(
        .CLK_FREQ_HZ(CLK_FREQ_HZ), .BYTE_TIMEOUT_US(BYTE_TIMEOUT_US),
        .STREAM_TIMEOUT_MS(STREAM_TIMEOUT_MS)
    ) X_temp_frame_parser (
        .I_clk(I_sysclk), .I_rstn(S_rstn),
        .I_byte(S_uart_byte), .I_byte_vld(S_uart_byte_vld),
        .I_uart_error(S_uart_error), .I_uart_error_code(S_uart_error_code),
        .O_temp_x10(S_temp_x10), .O_temp_vld(S_temp_vld),
        .O_seen_frame(S_seen_frame), .O_online(S_online),
        .O_error(S_error), .O_error_code(S_error_code),
        .O_error_count(S_error_count), .O_frame_count(S_frame_count)
    );
    localparam integer LED_CYCLES = (CLK_FREQ_HZ / 1000) * VALID_LED_MS;
    reg [25:0] r_heartbeat = 0;
    reg [31:0] r_valid_led_cycles = 0;
    always @(posedge I_sysclk) begin
        if (!S_rstn) begin
            r_heartbeat <= 0;
            r_valid_led_cycles <= 0;
        end else begin
            r_heartbeat <= r_heartbeat + 1'b1;
            if (S_temp_vld) r_valid_led_cycles <= LED_CYCLES;
            else if (r_valid_led_cycles != 0)
                r_valid_led_cycles <= r_valid_led_cycles - 1'b1;
        end
    end
    assign O_led[0] = r_heartbeat[25];
    assign O_led[1] = S_online;
    assign O_led[2] = (r_valid_led_cycles != 0);
    assign O_led[3] = S_error;

    // 50 MHz debug clock. Falling-edge registers give the capture clock
    // a half-cycle setup margin and hold each 100 MHz valid event for 20 ns.
    reg r_cwc_clk = 0;
    wire S_cwc_clk = r_cwc_clk;
    always @(posedge I_sysclk)
        if (!S_rstn) r_cwc_clk <= 0;
        else r_cwc_clk <= !r_cwc_clk;
    reg [1:0] r_byte_valid_cwc = 0, r_temp_valid_cwc = 0;
    reg [7:0] S_uart_byte_cwc = 0;
    reg signed [15:0] S_temp_x10_cwc = 0;
    reg [2:0] S_error_code_cwc = 0;
    reg [1:0] r_uart_error_cwc = 0;
    wire S_uart_error_cwc = |r_uart_error_cwc;
    wire S_uart_byte_vld_cwc = |r_byte_valid_cwc;
    wire S_temp_vld_cwc = |r_temp_valid_cwc;
    always @(negedge I_sysclk) begin
        if (!S_rstn) begin
            r_byte_valid_cwc <= 0;
            r_temp_valid_cwc <= 0;
            S_uart_byte_cwc <= 0;
            S_temp_x10_cwc <= 0;
            S_error_code_cwc <= 0;
            r_uart_error_cwc <= 0;
        end else begin
            r_byte_valid_cwc <= {r_byte_valid_cwc[0], S_uart_byte_vld};
            r_temp_valid_cwc <= {r_temp_valid_cwc[0], S_temp_vld};
            if (S_uart_byte_vld) S_uart_byte_cwc <= S_uart_byte;
            if (S_temp_vld) S_temp_x10_cwc <= S_temp_x10;
            S_error_code_cwc <= S_error_code;
            r_uart_error_cwc <= {r_uart_error_cwc[0], S_uart_error};
        end
    end
    // Temperature profile: 16 / 1 / 8 / 3 bits, 50 MHz, depth 65536.
    ui_cwc X_chipwatcher (
        .probe0(S_temp_x10_cwc), .probe1(S_temp_vld_cwc),
        .probe2(S_uart_byte_cwc), .probe3(S_error_code_cwc),
        .clk(S_cwc_clk)
    );
endmodule
