// 故障诊断系统 PL 顶层：温度、振动、电流、转速四路采集集成。
//
// 四路采集各自独立，共用底板 100 MHz 系统时钟与一个上电同步复位：
//   温度：GT-M601 串口板 115200 8N1 -> uart_rx_guard -> temp_frame_parser
//   振动：ADXL345 三线 SPI -> spi_adxl345 -> adxl345_ctrl
//   电流：ADS1115 I2C -> uii2c -> ads1115_ctrl
//   转速：OID-R3808B-12S1S RS485 -> Modbus RTU -> rpm -> PL USB UART
//
// J12 接线见 ../../README.md 与 04_pin/fpga_pin.adc，三路互不共用信号。
// 转速走板载 J11；遥测走 PL USB UART，不占用温度 UART 或 PS 控制台。
// 采集验证顶层；生产 SoC 入口为 soc_top。
module fpga_top #(
    // 系统时钟
    parameter integer CLK_FREQ_HZ   = 100_000_000,
    // 温度 UART
    parameter integer TEMP_BAUD     = 115200,
    parameter integer TEMP_BYTE_TIMEOUT_US = 2000,
    parameter integer TEMP_STREAM_TIMEOUT_MS = 0,
    // 振动 SPI
    parameter integer ADXL_SPI_CLK_DIV = 19,
    // 电流 I2C
    parameter integer IIC_FREQ_HZ   = 100_000,
    parameter [6:0]   ADS_DEV_ADDR  = 7'h48,
    parameter [2:0]   ADS_MUX       = 3'b100,
    parameter [2:0]   ADS_PGA       = 3'b001,
    parameter [2:0]   ADS_DATA_RATE = 3'b111,
    // 转速 RS485 与 PL USB UART 遥测
    parameter integer ENCODER_BAUD = 9600,
    parameter integer ENCODER_COUNTS_PER_REV = 4096,
    parameter integer ENCODER_SAMPLE_MS = 100,
    parameter integer ENCODER_STARTUP_MS = 1000,
    parameter integer ENCODER_POLL_MS = 100,
    parameter integer ENCODER_TIMEOUT_MS = 100,
    parameter integer DEBUG_BAUD = 115200,
    parameter integer FEATURE_WINDOW_LOG2 = 8,
    parameter integer FEATURE_HOP = 128
)(
    input  wire       I_sysclk     , // 100 MHz 系统时钟（H16）
    // 温度：GT-M601 串口板
    input  wire       I_uart_rx    , // 模块 TX -> PL（J12.5 / T9）
    // 振动：ADXL345 三线 SPI
    output wire       O_spi_sclk   , // SPI 时钟（J12.3 / Y12）
    inout  wire       IO_spi_sdio  , // SPI 数据，三线双向（J12.4 / Y13）
    output wire       O_spi_cs_n   , // 片选，低有效（J12.6 / U10）
    input  wire       I_int1       , // DATA_READY（J12.7 / T5）
    // 电流：ADS1115 I2C
    inout  wire       IO_iic_scl   , // I2C SCL，开漏（J12.9 / V6）
    inout  wire       IO_iic_sda   , // I2C SDA，开漏（J12.10 / W6）
    // 转速：板载 RS485（J11）与 PL USB UART
    input  wire       I_rs485_rx   , // 收发器 RO（N16）
    output wire       O_rs485_tx   , // 收发器 DI（L17）
    output wire       O_rs485_de   , // 收发方向（N15），1=发送
    output wire       O_uart_tx    , // PL USB UART TX（J16），115200 8N1
    // 板载 LED
    output wire O_alarm, // SoC alarm is PS-controlled; acquisition-only top stays low
    output wire [3:0] O_led
);

    assign O_alarm = 1'b0;

    //----------------------------------------------------------
    // 上电同步复位：约 655 us
    //----------------------------------------------------------
    reg [15:0] r_rst_cnt = 16'd0;
    reg        r_rstn    = 1'b0;
    always @(posedge I_sysclk) begin
        if (r_rst_cnt != 16'hffff) begin
            r_rst_cnt <= r_rst_cnt + 1'b1;
            r_rstn    <= 1'b0;
        end else begin
            r_rstn <= 1'b1;
        end
    end

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

    //==========================================================
    // 转速：RS485 Modbus RTU，32 位速度计数与 0.001 rpm 换算。
    // 保持独立事务节奏；当前通过 PL UART 读出，尚未接 AXI / DMA。
    //==========================================================
    wire signed [31:0] S_raw_speed, S_rpm_milli;
    wire S_sample_valid, S_rpm_valid, S_encoder_configured, S_encoder_online;
    wire S_encoder_error_valid, S_rpm_overflow, S_rpm_busy;
    wire [3:0] S_encoder_error_code;
    wire [7:0] S_encoder_exception_code;
    wire [31:0] S_encoder_sample_count, S_encoder_error_count;
    encoder_modbus #(
        .CLK_HZ(CLK_FREQ_HZ), .BAUD(ENCODER_BAUD), .SLAVE_ID(8'd1),
        .SAMPLE_MS(ENCODER_SAMPLE_MS), .STARTUP_MS(ENCODER_STARTUP_MS),
        .POLL_MS(ENCODER_POLL_MS), .TIMEOUT_MS(ENCODER_TIMEOUT_MS)
    ) X_encoder_modbus (
        .clk(I_sysclk), .rst_n(r_rstn), .rs485_rx(I_rs485_rx),
        .rs485_tx(O_rs485_tx), .rs485_de(O_rs485_de),
        .raw_speed(S_raw_speed), .sample_valid(S_sample_valid),
        .configured(S_encoder_configured), .online(S_encoder_online),
        .error_valid(S_encoder_error_valid), .error_code(S_encoder_error_code),
        .exception_code(S_encoder_exception_code),
        .sample_count(S_encoder_sample_count), .error_count(S_encoder_error_count)
    );
    encoder_rpm #(
        .COUNTS_PER_REV(ENCODER_COUNTS_PER_REV), .SAMPLE_MS(ENCODER_SAMPLE_MS)
    ) X_encoder_rpm (
        .clk(I_sysclk), .rst_n(r_rstn), .raw(S_raw_speed), .raw_valid(S_sample_valid),
        .rpm_milli(S_rpm_milli), .rpm_valid(S_rpm_valid),
        .overflow(S_rpm_overflow), .busy(S_rpm_busy)
    );
    encoder_telemetry #(.CLK_HZ(CLK_FREQ_HZ), .BAUD(DEBUG_BAUD)) X_encoder_telemetry (
        .clk(I_sysclk), .rst_n(r_rstn), .event_valid(S_rpm_valid | S_encoder_error_valid),
        .online(S_encoder_online), .overflow(S_rpm_overflow),
        .error_event(S_encoder_error_valid), .sample_count(S_encoder_sample_count),
        .raw_speed(S_raw_speed), .rpm_milli(S_rpm_milli),
        .error_code(S_encoder_error_code), .exception_code(S_encoder_exception_code),
        .tx(O_uart_tx)
    );

    // 温度、振动、电流的共同时基、样本接收时间戳与 1 kHz 对齐帧；尚未接 SoC。
    wire S_tick;
    wire [31:0] S_ts, S_ms, S_frame_ts, S_acc_ts, S_current_ts, S_temp_ts;
    wire signed [15:0] S_frame_temp, S_frame_acc_x, S_frame_acc_y, S_frame_acc_z, S_frame_current;
    timebase #(.CLK_FREQ_HZ(CLK_FREQ_HZ)) X_timebase(
        .I_clk(I_sysclk),.I_rstn(r_rstn),.I_enable(1'b1),.I_clear(1'b0),
        .I_temp_valid(S_temp_vld),.I_acc_valid(S_acc_vld),.I_current_valid(S_data_vld),
        .I_temp(S_temp_x10),.I_acc_x(S_acc_x),.I_acc_y(S_acc_y),.I_acc_z(S_acc_z),.I_current(S_adc_raw),
        .O_tick(S_tick),.O_ts(S_ts),.O_ms(S_ms),.O_frame_ts(S_frame_ts),
        .O_frame_temp(S_frame_temp),.O_frame_acc_x(S_frame_acc_x),.O_frame_acc_y(S_frame_acc_y),
        .O_frame_acc_z(S_frame_acc_z),.O_frame_current(S_frame_current),
        .O_acc_ts(S_acc_ts),.O_current_ts(S_current_ts),.O_temp_ts(S_temp_ts));

    // 四路时域特征，结果在本地快照保留；AXI 本体独立验证。
    wire [3:0] S_feature_ready, S_feature_valid, S_feature_overrun;
    wire [7:0] S_feature_flags;
    wire [1791:0] S_features;
    wire [31:0] S_feature_read;
    wire [3:0] S_feature_available;
    sensor_features #(.WINDOW_LOG2(FEATURE_WINDOW_LOG2),.HOP(FEATURE_HOP)) X_features(
        .I_clk(I_sysclk),.I_rstn(r_rstn),.I_acc_valid(S_acc_vld),.I_current_valid(S_data_vld),
        .I_acc_x(S_acc_x),.I_acc_y(S_acc_y),.I_acc_z(S_acc_z),.I_current(S_adc_raw),
        .O_ready(S_feature_ready),.O_valid(S_feature_valid),.O_overrun(S_feature_overrun),
        .O_flags(S_feature_flags),.O_features(S_features));
    feature_bank X_feature_bank(.I_clk(I_sysclk),.I_rstn(r_rstn),.I_clear(1'b0),
        .I_snapshot(|S_feature_valid),.I_valid(S_feature_valid),.I_features(S_features),
        .I_flags(S_feature_flags),.I_word_addr(8'h40),.O_data(S_feature_read),
        .O_available(S_feature_available));

    //==========================================================
    // LED
    //   LED1：系统心跳
    //   LED2：温度在线（收到有效温度帧后常亮）
    //   LED3：振动 DEVID 自检通过（读回 0xE5）常亮
    //   LED4：电流采集错误
    //==========================================================
    reg [25:0] r_heartbeat = 26'd0;
    always @(posedge I_sysclk) begin
        if (!r_rstn) r_heartbeat <= 26'd0;
        else         r_heartbeat <= r_heartbeat + 1'b1;
    end

    assign O_led[0] = r_heartbeat[25];
    assign O_led[1] = S_online;
    assign O_led[2] = S_dev_ok;
    assign O_led[3] = S_ads_error;

endmodule
