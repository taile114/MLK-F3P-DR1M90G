// Standalone PL test: ADS1115 at J12.3 (SCL) / J12.4 (SDA), 3.3 V.
// Both physical bus outputs are open drain. No clock stretching is required
// by ADS1115; the unchanged vendor controller does not support it.
module ads1115_test #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer IIC_FREQ_HZ = 100_000,
    parameter [6:0] DEV_ADDR = 7'h48,
    parameter [2:0] MUX = 3'b100,
    parameter [2:0] PGA = 3'b001,
    parameter [2:0] DATA_RATE = 3'b111,
    parameter integer INIT_MS = 10,
    parameter integer IIC_TIMEOUT_US = 2000,
    parameter integer RETRY_MS = 100
)(
    input wire I_sysclk,
    inout wire IO_iic_scl,
    inout wire IO_iic_sda,
    output wire [3:0] O_led
);
    reg [9:0] r_reset_count = 0;
    wire S_rstn = r_reset_count[9];
    always @(posedge I_sysclk)
        if (!S_rstn) r_reset_count <= r_reset_count + 1'b1;

    localparam integer HALF_CYCLES = CLK_FREQ_HZ / (2 * IIC_FREQ_HZ);
    localparam integer IIC_CLK_DIV = 2 * HALF_CYCLES - 2;
    wire S_iic_req, S_iic_busy, S_bus_err, S_scl_drive, S_iic_rstn;
    wire [31:0] S_wr_data;
    wire [7:0] S_wr_cnt, S_rd_cnt;
    wire [15:0] S_rd_data;
    wire S_sda_debug;
    wire S_bus_rstn = S_rstn && S_iic_rstn;
    assign IO_iic_scl = (S_bus_rstn && !S_scl_drive) ? 1'b0 : 1'bz;

    uii2c #(.WMEN_LEN(4), .RMEN_LEN(2), .CLK_DIV(IIC_CLK_DIV)) X_uii2c (
        .I_clk(I_sysclk), .I_rstn(S_bus_rstn),
        .O_iic_scl(S_scl_drive), .IO_iic_sda(IO_iic_sda),
        .I_wr_data(S_wr_data), .I_wr_cnt(S_wr_cnt),
        .O_rd_data(S_rd_data), .I_rd_cnt(S_rd_cnt),
        .I_iic_req(S_iic_req), .I_iic_mode(1'b1),
        .O_iic_busy(S_iic_busy), .O_iic_bus_error(S_bus_err),
        .IO_iic_sda_dg(S_sda_debug)
    );

    wire signed [15:0] S_adc_raw;
    wire [15:0] S_config_read, S_error_count;
    wire S_data_vld, S_dev_ok, S_error, S_busy;
    wire [2:0] S_error_code;
    ads1115_ctrl #(
        .CLK_FREQ_HZ(CLK_FREQ_HZ), .DEV_ADDR(DEV_ADDR),
        .MUX(MUX), .PGA(PGA), .DATA_RATE(DATA_RATE),
        .INIT_MS(INIT_MS),
        .IIC_TIMEOUT_US(IIC_TIMEOUT_US), .RETRY_MS(RETRY_MS)
    ) X_ads1115_ctrl (
        .I_clk(I_sysclk), .I_rstn(S_rstn),
        .O_iic_req(S_iic_req), .O_wr_data(S_wr_data),
        .O_wr_cnt(S_wr_cnt), .O_rd_cnt(S_rd_cnt),
        .I_iic_busy(S_iic_busy), .I_bus_error(S_bus_err),
        .I_rd_data(S_rd_data), .O_iic_rstn(S_iic_rstn),
        .O_adc_raw(S_adc_raw), .O_data_vld(S_data_vld),
        .O_config_read(S_config_read), .O_dev_ok(S_dev_ok),
        .O_error(S_error), .O_error_code(S_error_code),
        .O_error_count(S_error_count), .O_busy(S_busy)
    );

    reg [25:0] r_heartbeat = 0;
    reg [4:0] r_sample_count = 0;
    always @(posedge I_sysclk) begin
        if (!S_rstn) begin
            r_heartbeat <= 0;
            r_sample_count <= 0;
        end else begin
            r_heartbeat <= r_heartbeat + 1'b1;
            if (S_data_vld) r_sample_count <= r_sample_count + 1'b1;
        end
    end
    assign O_led[0] = r_heartbeat[25];
    assign O_led[1] = S_dev_ok;
    // Divide valid-sample events by 32 for a visible acquisition heartbeat.
    assign O_led[2] = r_sample_count[4];
    assign O_led[3] = S_error;

    // ChipWatcher data profile: widths 16 / 1 / 16 / 3, depth 65536.
    // Capture at 100 MHz; trigger on probe1 (S_data_vld) rising edge.
    ui_cwc X_chipwatcher (
        .probe0 (S_adc_raw    ),
        .probe1 (S_data_vld   ),
        .probe2 (S_config_read),
        .probe3 (S_error_code ),
        .clk    (I_sysclk     )
    );
endmodule
