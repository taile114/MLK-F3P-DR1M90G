// Sensor protocol controllers and the single acquisition-domain timebase.
// Register/DMA consumers receive explicit ports; no diagnostic probe instances.
module sensor_capture #(
    parameter integer CLK_FREQ_HZ=100_000_000,
    parameter integer TEMP_BAUD=115200,
    parameter integer TEMP_BYTE_TIMEOUT_US=2000,
    parameter integer TEMP_STREAM_TIMEOUT_MS=0,
    parameter integer ADXL_SPI_CLK_DIV=19,
    parameter integer IIC_FREQ_HZ=100_000,
    // WITH_FEATURES=0 时不实现四路时域特征（含存储），特征输出恒 0；
    // 合并码流因特征存储保持时间无法收敛而关闭该单元，独立工程保持默认 1。
    parameter integer WITH_FEATURES=1,
    parameter integer FEATURE_WINDOW_LOG2=8, FEATURE_HOP=128,
    parameter [6:0] ADS_DEV_ADDR=7'h48,
    parameter [2:0] ADS_MUX=3'b100, ADS_PGA=3'b001, ADS_DATA_RATE=3'b111
)(
    input wire I_sysclk, I_rstn, I_enable, I_clear, I_uart_rx, I_int1,
    output wire O_spi_sclk, O_spi_cs_n,
    inout wire IO_spi_sdio, IO_iic_scl, IO_iic_sda,
    output wire [3:0] O_led,
    output wire [79:0] O_samples,
    output wire [2:0] O_sample_valid,
    output wire [7:0] O_faults,
    output wire [31:0] O_ts,          // 采集域共同时基计数，10 ns/LSB，供 PL_TS_NOW 读取
    output wire [255:0] O_frame,
    output wire O_frame_valid,
    output wire [1791:0] O_features,
    output wire [7:0] O_feature_flags,
    output wire [3:0] O_feature_valid,
    output wire O_busy
);
    wire r_rstn=I_rstn;
    //==========================================================
    // 温度：GT-M601 串口板
    //==========================================================
    wire [7:0]  S_uart_byte;
    wire        S_uart_byte_vld, S_uart_error, S_uart_ready, S_uart_rx_sync;
    wire [1:0]  S_uart_error_code;
    uart_rx_guard #(
        .CLK_FREQ_HZ (CLK_FREQ_HZ),
        .BAUD        (TEMP_BAUD  )
    ) X_uart_rx_guard (
        .I_clk       (I_sysclk        ),
        .I_rstn      (r_rstn          ),
        .I_uart_rx   (I_uart_rx       ),
        .O_byte      (S_uart_byte     ),
        .O_byte_vld  (S_uart_byte_vld ),
        .O_error     (S_uart_error    ),
        .O_error_code(S_uart_error_code),
        .O_ready     (S_uart_ready    ),
        .O_rx_sync   (S_uart_rx_sync  )
    );

    wire signed [15:0] S_temp_x10;
    wire        S_temp_vld, S_seen_frame, S_online, S_temp_error;
    wire [2:0]  S_temp_error_code;
    wire [15:0] S_temp_error_count;
    wire [31:0] S_frame_count;
    temp_frame_parser #(
        .CLK_FREQ_HZ       (CLK_FREQ_HZ),
        .BYTE_TIMEOUT_US   (TEMP_BYTE_TIMEOUT_US),
        .STREAM_TIMEOUT_MS (TEMP_STREAM_TIMEOUT_MS)
    ) X_temp_frame_parser (
        .I_clk           (I_sysclk          ),
        .I_rstn          (r_rstn            ),
        .I_byte          (S_uart_byte       ),
        .I_byte_vld      (S_uart_byte_vld   ),
        .I_uart_error    (S_uart_error      ),
        .I_uart_error_code(S_uart_error_code),
        .O_temp_x10      (S_temp_x10        ),
        .O_temp_vld      (S_temp_vld        ),
        .O_seen_frame    (S_seen_frame      ),
        .O_online        (S_online          ),
        .O_error         (S_temp_error      ),
        .O_error_code    (S_temp_error_code ),
        .O_error_count   (S_temp_error_count),
        .O_frame_count   (S_frame_count     )
    );

    //==========================================================
    // 振动：ADXL345 三线 SPI
    //==========================================================
    wire        S_spi_start;
    wire [3:0]  S_spi_num;
    wire [7:0]  S_spi_cmd;
    wire [55:0] S_spi_wdata;
    wire        S_spi_busy, S_spi_done;
    wire [55:0] S_spi_rdata;
    spi_adxl345 #(
        .CLK_FREQ_HZ (CLK_FREQ_HZ     ),
        .CLK_DIV     (ADXL_SPI_CLK_DIV),
        .BYTE_CNT    (8               ),
        .DELAY_CNT   (500             )
    ) X_spi_adxl345 (
        .I_clk       (I_sysclk   ),
        .I_rstn      (r_rstn     ),
        .I_start     (S_spi_start),
        .I_byte_num  (S_spi_num  ),
        .I_cmd       (S_spi_cmd  ),
        .I_wr_data   (S_spi_wdata),
        .O_busy      (S_spi_busy ),
        .O_done      (S_spi_done ),
        .O_rd_data   (S_spi_rdata),
        .O_spi_cs_n  (O_spi_cs_n ),
        .O_spi_sclk  (O_spi_sclk ),
        .IO_spi_sdio (IO_spi_sdio)
    );

    wire [15:0] S_acc_x, S_acc_y, S_acc_z;
    wire        S_acc_vld, S_dev_ok, S_dev_rd, S_adxl_busy;
    wire [7:0]  S_dev_id;
    adxl345_ctrl #(
        .CLK_FREQ_HZ (CLK_FREQ_HZ),
        .INIT_MS     (10         )
    ) X_adxl345_ctrl (
        .I_clk       (I_sysclk   ),
        .I_rstn      (r_rstn     ),
        .I_dr        (I_int1     ),
        .O_spi_start (S_spi_start),
        .O_spi_num   (S_spi_num  ),
        .O_spi_cmd   (S_spi_cmd  ),
        .O_spi_wdata (S_spi_wdata),
        .I_spi_busy  (S_spi_busy ),
        .I_spi_done  (S_spi_done ),
        .I_spi_rdata (S_spi_rdata),
        .O_acc_x     (S_acc_x    ),
        .O_acc_y     (S_acc_y    ),
        .O_acc_z     (S_acc_z    ),
        .O_data_vld  (S_acc_vld  ),
        .O_dev_ok    (S_dev_ok   ),
        .O_dev_id    (S_dev_id   ),
        .O_dev_rd    (S_dev_rd   ),
        .O_busy      (S_adxl_busy)
    );

    //==========================================================
    // 电流：ADS1115 I2C
    //==========================================================
    localparam integer ADS_HALF_CYCLES = CLK_FREQ_HZ / (2 * IIC_FREQ_HZ);
    localparam integer ADS_IIC_CLK_DIV = 2 * ADS_HALF_CYCLES - 2;

    wire        S_iic_req, S_iic_busy, S_bus_err, S_scl_drive, S_iic_rstn;
    wire [31:0] S_wr_data;
    wire [7:0]  S_wr_cnt, S_rd_cnt;
    wire [15:0] S_rd_data;
    wire        S_sda_debug;
    wire        S_bus_rstn = r_rstn && S_iic_rstn;

    // 开漏：仅拉低，释放由模块侧上拉提供高电平。
    assign IO_iic_scl = (S_bus_rstn && !S_scl_drive) ? 1'b0 : 1'bz;

    uii2c #(.WMEN_LEN(4), .RMEN_LEN(2), .CLK_DIV(ADS_IIC_CLK_DIV)) X_uii2c (
        .I_clk          (I_sysclk   ),
        .I_rstn         (S_bus_rstn ),
        .O_iic_scl      (S_scl_drive),
        .IO_iic_sda     (IO_iic_sda ),
        .I_wr_data      (S_wr_data  ),
        .I_wr_cnt       (S_wr_cnt   ),
        .O_rd_data      (S_rd_data  ),
        .I_rd_cnt       (S_rd_cnt   ),
        .I_iic_req      (S_iic_req  ),
        .I_iic_mode     (1'b1       ),
        .O_iic_busy     (S_iic_busy ),
        .O_iic_bus_error(S_bus_err  ),
        .IO_iic_sda_dg  (S_sda_debug)
    );

    wire signed [15:0] S_adc_raw;
    wire [15:0] S_config_read, S_ads_error_count;
    wire        S_data_vld, S_ads_dev_ok, S_ads_error, S_ads_busy;
    wire [2:0]  S_ads_error_code;
    ads1115_ctrl #(
        .CLK_FREQ_HZ (CLK_FREQ_HZ ),
        .DEV_ADDR    (ADS_DEV_ADDR),
        .MUX         (ADS_MUX     ),
        .PGA         (ADS_PGA     ),
        .DATA_RATE   (ADS_DATA_RATE)
    ) X_ads1115_ctrl (
        .I_clk         (I_sysclk        ),
        .I_rstn        (r_rstn          ),
        .O_iic_req     (S_iic_req       ),
        .O_wr_data     (S_wr_data       ),
        .O_wr_cnt      (S_wr_cnt        ),
        .O_rd_cnt      (S_rd_cnt        ),
        .I_iic_busy    (S_iic_busy      ),
        .I_bus_error   (S_bus_err       ),
        .I_rd_data     (S_rd_data       ),
        .O_iic_rstn    (S_iic_rstn      ),
        .O_adc_raw     (S_adc_raw       ),
        .O_data_vld    (S_data_vld      ),
        .O_config_read (S_config_read   ),
        .O_dev_ok      (S_ads_dev_ok    ),
        .O_error       (S_ads_error     ),
        .O_error_code  (S_ads_error_code),
        .O_error_count (S_ads_error_count),
        .O_busy        (S_ads_busy      )
    );

    wire S_tick;
    wire [31:0] S_ts, S_ms, S_frame_ts, S_acc_ts, S_current_ts, S_temp_ts;
    wire signed [15:0] S_frame_temp, S_frame_acc_x, S_frame_acc_y, S_frame_acc_z, S_frame_current;
    timebase #(.CLK_FREQ_HZ(CLK_FREQ_HZ)) X_timebase(
        .I_clk(I_sysclk),.I_rstn(r_rstn),.I_enable(I_enable),.I_clear(I_clear),
        .I_temp_valid(S_temp_vld),.I_acc_valid(S_acc_vld),.I_current_valid(S_data_vld),
        .I_temp(S_temp_x10),.I_acc_x(S_acc_x),.I_acc_y(S_acc_y),.I_acc_z(S_acc_z),.I_current(S_adc_raw),
        .O_tick(S_tick),.O_ts(S_ts),.O_ms(S_ms),.O_frame_ts(S_frame_ts),
        .O_frame_temp(S_frame_temp),.O_frame_acc_x(S_frame_acc_x),.O_frame_acc_y(S_frame_acc_y),
        .O_frame_acc_z(S_frame_acc_z),.O_frame_current(S_frame_current),
        .O_acc_ts(S_acc_ts),.O_current_ts(S_current_ts),.O_temp_ts(S_temp_ts));

    // 共同时基计数直出，供寄存器 0xB0 PL_TS_NOW 读取（跨域由调用方处理）。
    assign O_ts=S_ts;

    // 四路时域特征通过显式结果端口送跨域快照邮箱。
    wire [3:0] S_feature_ready, S_feature_valid, S_feature_overrun;
    wire [7:0] S_feature_flags;
    wire [1791:0] S_features;
    generate if (WITH_FEATURES != 0) begin : G_features
        sensor_features #(.WINDOW_LOG2(FEATURE_WINDOW_LOG2),.HOP(FEATURE_HOP)) X_features(
            .I_clk(I_sysclk),.I_rstn(r_rstn && !I_clear),.I_acc_valid(S_acc_vld && I_enable),.I_current_valid(S_data_vld && I_enable),
            .I_acc_x(S_acc_x),.I_acc_y(S_acc_y),.I_acc_z(S_acc_z),.I_current(S_adc_raw),
            .O_ready(S_feature_ready),.O_valid(S_feature_valid),.O_overrun(S_feature_overrun),
            .O_flags(S_feature_flags),.O_features(S_features));
    end else begin : G_no_features
        // 不例化 sensor_features / time_features / feature_bank，输出恒 0。
        assign S_feature_ready=4'd0;
        assign S_feature_valid=4'd0;
        assign S_feature_overrun=4'd0;
        assign S_feature_flags=8'd0;
        assign S_features=1792'd0;
    end endgenerate

    assign O_samples={S_temp_x10,S_acc_x,S_acc_y,S_acc_z,S_adc_raw};
    assign O_sample_valid={S_temp_vld,S_acc_vld,S_data_vld} & {3{I_enable}};
    assign O_frame={48'd0,S_frame_ts,S_frame_temp,S_frame_acc_x,S_frame_acc_y,
                    S_frame_acc_z,S_frame_current,S_acc_ts,S_current_ts,S_temp_ts};
    // timebase publishes its frame on the tick edge; delay valid one cycle.
    reg S_frame_valid;
    always @(posedge I_sysclk) begin
        if (!I_rstn) S_frame_valid<=0;
        else S_frame_valid<=S_tick && I_enable && !I_clear;
    end
    assign O_frame_valid=S_frame_valid;
    assign O_features=S_features;
    assign O_feature_flags=S_feature_flags;
    assign O_feature_valid=S_feature_valid;
    assign O_faults={4'd0,|S_feature_overrun,S_ads_error,!S_dev_ok,S_temp_error};
    assign O_busy=I_enable;

    // LED1 is a free-running heartbeat in the acquisition clock domain, so a
    // configured PL is visible even before the PS writes CTRL.EN. LED2/3/4
    // report temperature online, ADXL345 ID and ADS1115 error respectively.
    reg [25:0] S_heartbeat;
    always @(posedge I_sysclk) begin
        if (!I_rstn) S_heartbeat<=0;
        else         S_heartbeat<=S_heartbeat+1'b1;
    end
    assign O_led={S_ads_error,S_dev_ok,S_online,S_heartbeat[25]};
endmodule
