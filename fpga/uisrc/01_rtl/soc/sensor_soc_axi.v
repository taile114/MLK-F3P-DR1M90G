// AXI registers run in the PS clock domain; all acquisition stays at 100 MHz.
// WITH_FEATURES=0 时关闭四路时域特征（sensor_features / time_features / feature_bank
// 及其存储），特征寄存器读 0 且正常返回 OKAY；合并码流用 0，独立工程保持默认 1。
// LOGN 为振动 FFT 点数指数，同时决定 §4.6 频谱窗口的 bin 数与 bin RAM 深度。
// 转速通道（§14，v1.2）：RS485 Modbus 主站与有符号换算在采集域自由运行，
// 每次事件把整条 192 bit 快照经单信箱送入 AXI 域；寄存器窗口与 DMA 记录
// （CH=3/4/5）共用同一份锁存快照。
module sensor_soc_axi #(
    parameter integer WITH_FEATURES = 1,
    parameter integer AXI_CLK_FREQ_HZ = 100_000_000,
    parameter integer LOGN = 8,
    // 转速采集链参数：默认对应 OID-R3808B-12S1S（9600 8N1、站号 1、4096 计数/圈、
    // 100 ms 采样）。仿真可缩短轮询 / 超时或提高波特率，不改默认行为。
    parameter integer SPEED_BAUD = 9600,
    parameter integer SPEED_COUNTS_PER_REV = 4096,
    parameter integer SPEED_SAMPLE_MS = 100,
    parameter integer SPEED_STARTUP_MS = 1000,
    parameter integer SPEED_POLL_MS = 100,
    parameter integer SPEED_TIMEOUT_MS = 100
)(
    input wire I_clk, I_rstn, I_sysclk,
    input wire I_uart_rx, I_int1,
    // 板载 RS485（§14）：采集域半双工 Modbus RTU 主站
    input wire I_rs485_rx,
    output wire O_rs485_tx,
    output wire O_rs485_de,
    output wire O_spi_sclk, O_spi_cs_n,
    inout wire IO_spi_sdio, IO_iic_scl, IO_iic_sda,
    output wire [3:0] O_led,
    output wire O_alarm,
    input wire [15:0] I_awaddr,
    input wire I_awvalid,
    output wire O_awready,
    input wire [31:0] I_wdata,
    input wire [3:0] I_wstrb,
    input wire I_wvalid,
    output wire O_wready,
    output wire [1:0] O_bresp,
    output wire O_bvalid,
    input wire I_bready,
    input wire [15:0] I_araddr,
    input wire I_arvalid,
    output wire O_arready,
    output wire [31:0] O_rdata,
    output wire [1:0] O_rresp,
    output wire O_rvalid,
    input wire I_rready,
    output wire [32:0] M_axi_awaddr,
    output wire [7:0] M_axi_awlen,
    output wire [2:0] M_axi_awsize,
    output wire [1:0] M_axi_awburst,
    output wire M_axi_awvalid,
    input wire M_axi_awready,
    output wire [63:0] M_axi_wdata,
    output wire [7:0] M_axi_wstrb,
    output wire M_axi_wlast,
    output wire M_axi_wvalid,
    input wire M_axi_wready,
    input wire [1:0] M_axi_bresp,
    input wire M_axi_bvalid,
    output wire M_axi_bready,
    output wire O_irq,
    // 块完成中断：DMA_IRQ[16] 的 PEND 电平（LEVEL_HIGH，接 f2p_irq[3]），
    // PS 写 DMA_IRQ[8]=1 清挂起后拉低
    output wire O_dma_irq
);
    wire S_axi_rstn, S_sensor_rstn;
    reset_sync X_axi_reset(.I_clk(I_clk),.I_arstn(I_rstn),.O_rstn(S_axi_rstn));
    reset_sync X_sensor_reset(.I_clk(I_sysclk),.I_arstn(I_rstn),.O_rstn(S_sensor_rstn));
    wire S_enable, S_irq_enable, S_soft_reset;
    (* ASYNC_REG="TRUE" *) reg [1:0] S_enable_sync;
    always @(posedge I_sysclk or negedge S_sensor_rstn)
        if (!S_sensor_rstn) S_enable_sync<=0;
        else S_enable_sync<={S_enable_sync[0],S_enable};
    wire S_clear, S_clear_ready, S_clear_overflow, S_clear_data;
    cdc_mailbox #(.WIDTH(1)) X_clear(
        .I_src_clk(I_clk),.I_src_rstn(S_axi_rstn),.I_valid(S_soft_reset),.I_data(1'b1),
        .O_ready(S_clear_ready),.O_overflow(S_clear_overflow),
        .I_dst_clk(I_sysclk),.I_dst_rstn(S_sensor_rstn),.O_valid(S_clear),.O_data(S_clear_data));
    // Drain stale events around reset. Sensor state is cleared before ack returns.
    reg [3:0] S_drain;
    always @(posedge I_clk) begin
        if (!S_axi_rstn || S_soft_reset || !S_clear_ready) S_drain<=15;
        else if (S_drain!=0) S_drain<=S_drain-1'b1;
    end
    wire S_accept=S_drain==0 && !S_soft_reset;
    wire [79:0] S_raw;
    wire [2:0] S_raw_valid;
    wire [7:0] S_faults;
    wire [255:0] S_frame;
    wire S_frame_valid, S_busy;
    wire [31:0] S_capture_ts;
    wire [1791:0] S_features;
    wire [7:0] S_feature_flags;
    wire [3:0] S_feature_valid;
    sensor_capture #(.WITH_FEATURES(WITH_FEATURES)) X_capture(
        .I_sysclk(I_sysclk),.I_rstn(S_sensor_rstn),.I_enable(S_enable_sync[1]),.I_clear(S_clear),
        .I_uart_rx(I_uart_rx),.I_int1(I_int1),.O_spi_sclk(O_spi_sclk),.O_spi_cs_n(O_spi_cs_n),
        .IO_spi_sdio(IO_spi_sdio),.IO_iic_scl(IO_iic_scl),.IO_iic_sda(IO_iic_sda),.O_led(O_led),
        .O_samples(S_raw),.O_sample_valid(S_raw_valid),.O_faults(S_faults),.O_ts(S_capture_ts),
        .O_frame(S_frame),.O_frame_valid(S_frame_valid),.O_features(S_features),
        .O_feature_flags(S_feature_flags),.O_feature_valid(S_feature_valid),.O_busy(S_busy));
    reg [7:0] S_previous_faults;
    always @(posedge I_sysclk) begin
        if (!S_sensor_rstn) S_previous_faults<=0;
        else S_previous_faults<=S_faults;
    end
    wire [90:0] S_raw_axi;
    wire S_raw_axi_valid, S_raw_ready, S_raw_overflow;
    cdc_event_fifo X_samples(
        .I_src_clk(I_sysclk),.I_src_rstn(S_sensor_rstn),
        .I_valid((|S_raw_valid) || S_faults!=S_previous_faults),.I_data({S_faults,S_raw,S_raw_valid}),
        .O_ready(S_raw_ready),.O_overflow(S_raw_overflow),
        .I_dst_clk(I_clk),.I_dst_rstn(S_axi_rstn),.O_valid(S_raw_axi_valid),.O_data(S_raw_axi));
    wire [255:0] S_frame_axi;
    wire S_frame_axi_valid, S_frame_ready, S_frame_overflow;
    cdc_mailbox #(.WIDTH(256)) X_frame(
        .I_src_clk(I_sysclk),.I_src_rstn(S_sensor_rstn),.I_valid(S_frame_valid),.I_data(S_frame),
        .O_ready(S_frame_ready),.O_overflow(S_frame_overflow),
        .I_dst_clk(I_clk),.I_dst_rstn(S_axi_rstn),.O_valid(S_frame_axi_valid),.O_data(S_frame_axi));
    reg [255:0] S_frame_hold;
    always @(posedge I_clk) begin
        if (!S_axi_rstn || S_soft_reset || !S_accept) S_frame_hold<=0;
        else if(S_frame_axi_valid && S_enable) S_frame_hold<=S_frame_axi;
    end
    wire [3:0] S_feature_axi_valid, S_feature_overflow, S_feature_ready;
    wire [1791:0] S_features_axi;
    wire [7:0] S_feature_flags_axi;
    genvar c;
    generate if (WITH_FEATURES != 0) begin : G_feature_cdc
        for(c=0;c<4;c=c+1) begin : G_features
            cdc_mailbox #(.WIDTH(450)) X_feature(
                .I_src_clk(I_sysclk),.I_src_rstn(S_sensor_rstn),.I_valid(S_feature_valid[c]),
                .I_data({S_feature_flags[c*2+:2],S_features[c*448+:448]}),
                .O_ready(S_feature_ready[c]),.O_overflow(S_feature_overflow[c]),
                .I_dst_clk(I_clk),.I_dst_rstn(S_axi_rstn),.O_valid(S_feature_axi_valid[c]),
                .O_data({S_feature_flags_axi[c*2+:2],S_features_axi[c*448+:448]}));
        end
    end else begin : G_no_feature_cdc
        // 不实现特征跨域邮箱；特征输入恒 0，寄存器窗口仍为已映射地址。
        assign S_feature_ready=4'd0;
        assign S_feature_overflow=4'd0;
        assign S_feature_axi_valid=4'd0;
        assign S_features_axi=1792'd0;
        assign S_feature_flags_axi=8'd0;
    end endgenerate
    wire S_fft_valid, S_fft_power_valid;
    wire [LOGN-1:0] S_fft_power_bin;
    wire [63:0] S_fft_power, S_fft_peak;
    wire [31:0] S_fft_seq, S_fft_bin, S_fft_flags;
    vibration_fft #(.LOGN(LOGN)) X_fft(
        .I_clk(I_sysclk),.I_rstn(S_sensor_rstn),.I_clear(S_clear),.I_enable(S_enable_sync[1]),
        .I_valid(S_raw_valid[1]),.I_sample(S_raw[63:48]),.I_ready(1'b1),
        .O_valid(S_fft_power_valid),.O_bin(S_fft_power_bin),.O_power(S_fft_power),
        .O_result_valid(S_fft_valid),.O_sequence(S_fft_seq),.O_peak_bin(S_fft_bin),
        .O_flags(S_fft_flags),.O_peak_power(S_fft_peak));
    //==========================================================
    // 完整频谱窗口 bin RAM（§4.6）：采集域在 FFT 每拍输出 bin 功率时写入
    // O_power[39:8]（丢低 8 位），地址直接取 O_bin，写口上不加组合逻辑。
    // 有效量程：O_power=real²+imag²（|X|² 的 Q16 值），ADXL345 ±16 g、
    // 256 LSB/g 时最大约 2^39，>>8 后约 2^31，unsigned32 不溢出。
    // 读写口必须同域：整窗结束后由本域逐字读出，经寄存器级四相握手推到
    // 寄存器域存快照。若把读口放到另一个时钟域，综合工具可能把同一数组
    // 实现成读写分离的存储而读回恒 0，仿真里两个域访问同一数组看不出差异。
    //==========================================================
    // 不加 ram_style：TD 的 AUTO 推断已把它实现为 ERAM20K（SYN-5041 会提示
    // 其它厂商的 ram_style 取值不被识别）。
    reg [31:0] S_spect[0:(1<<LOGN)-1];
    always @(posedge I_sysclk) begin
        if(S_fft_power_valid) S_spect[S_fft_power_bin]<=S_fft_power[39:8];
    end
    // 整窗读出：读口固定在本域时钟，数据下一拍有效。S_xdata/S_xidx 在握手
    // 期间保持不变，对端在 push 翻转沿采样：跨域只有 push/ack 两个单 bit
    // 翻转沿，以及随 push 沿稳定的数据与索引总线。
    reg [31:0] S_spect_q, S_xdata;
    reg [LOGN-1:0] S_xidx, S_xaddr;
    reg S_xpush;
    wire S_spect_push=S_xpush;
    wire [31:0] S_spect_data=S_xdata;
    wire [LOGN-1:0] S_spect_index=S_xidx;
    wire S_spect_ack;
    (* ASYNC_REG = "TRUE" *) reg [1:0] S_xack_sync;
    reg [1:0] S_xstate;
    localparam [1:0] X_IDLE=2'd0, X_SEND=2'd1, X_WAIT=2'd2;
    always @(posedge I_sysclk) S_spect_q<=S_spect[S_xaddr];
    always @(posedge I_sysclk or negedge S_sensor_rstn)
        if(!S_sensor_rstn) S_xack_sync<=2'd0;
        else S_xack_sync<={S_xack_sync[0],S_spect_ack};
    always @(posedge I_sysclk) begin
        if(!S_sensor_rstn) begin
            S_xdata<=0;S_xidx<=0;S_xaddr<=0;S_xpush<=0;S_xstate<=X_IDLE;
        end else case(S_xstate)
            X_IDLE: begin
                // 空闲时读口停在 bin 0，触发后第一字已在 S_spect_q。
                S_xaddr<=0;
                if(S_fft_valid) S_xstate<=X_SEND;
            end
            X_SEND: begin
                S_xdata<=S_spect_q;S_xidx<=S_xaddr;S_xpush<=~S_xpush;
                S_xaddr<=S_xaddr+1'b1;S_xstate<=X_WAIT;
            end
            default: if(S_xack_sync[1]==S_xpush)   // 该字已回执才发下一字
                S_xstate<=(S_xaddr==0) ? X_IDLE : X_SEND;
        endcase
    end
    wire [159:0] S_fft_axi;
    wire S_fft_axi_valid, S_fft_ready, S_fft_overflow;
    cdc_mailbox #(.WIDTH(160)) X_fft_result(
        .I_src_clk(I_sysclk),.I_src_rstn(S_sensor_rstn),.I_valid(S_fft_valid),
        .I_data({S_fft_seq,S_fft_bin,S_fft_peak,S_fft_flags}),
        .O_ready(S_fft_ready),.O_overflow(S_fft_overflow),
        .I_dst_clk(I_clk),.I_dst_rstn(S_axi_rstn),.O_valid(S_fft_axi_valid),.O_data(S_fft_axi));
    // 完整频谱窗口：结果字先暂存，等整窗快照拷贝结束时再与快照同拍发布，
    // 因此 0x78~0x88 与 0x400+4k 始终属同一窗，不会出现新序号配旧 bin。
    reg [159:0] S_fft_hold, S_fft_pend;
    reg S_fft_pend_valid;
    wire S_spect_done;
    wire S_spect_event=S_fft_axi_valid && S_enable && S_accept;
    always @(posedge I_clk) begin
        if (!S_axi_rstn || S_soft_reset || !S_accept) begin
            S_fft_hold<=0; S_fft_pend<=0; S_fft_pend_valid<=0;
        end else begin
            if(S_spect_event) begin S_fft_pend<=S_fft_axi; S_fft_pend_valid<=1; end
            if(S_spect_done) begin
                if(S_fft_pend_valid) S_fft_hold<=S_fft_pend;
                S_fft_pend_valid<=0;
            end
        end
    end
    wire S_speed_mbox_overflow;
    (* ASYNC_REG="TRUE" *) reg [1:0] S_overflow_sync;
    always @(posedge I_clk or negedge S_axi_rstn)
        if (!S_axi_rstn) S_overflow_sync<=0;
        else S_overflow_sync<={S_overflow_sync[0],S_raw_overflow || S_frame_overflow ||
                              (|S_feature_overflow) || S_fft_overflow ||
                              S_speed_mbox_overflow};
    reg [7:0] S_faults_axi;
    always @(posedge I_clk) begin
        if (!S_axi_rstn) S_faults_axi<=0;
        else if(S_raw_axi_valid) S_faults_axi<=S_raw_axi[90:83];
    end
    wire [2:0] S_valid=S_raw_axi[2:0] & {3{S_raw_axi_valid && S_accept}};
    //==========================================================
    // PL_TS_NOW：采集域共同时基计数（10 ns/LSB）以格雷码跨到寄存器域。
    // 格雷码每拍只变 1 bit，2 级采样最多落后 1~2 拍（≤20 ns），对 ms 级
    // 时延测量无影响；32 bit 计数自然回绕，读 0xB0 得到当前值。
    //==========================================================
    reg [31:0] S_ts_gray_src;
    always @(posedge I_sysclk) begin
        if (!S_sensor_rstn) S_ts_gray_src<=0;
        else S_ts_gray_src<=S_capture_ts ^ (S_capture_ts>>1);
    end
    (* ASYNC_REG="TRUE" *) reg [31:0] S_ts_gray_meta, S_ts_gray_sync;
    always @(posedge I_clk) begin
        if (!S_axi_rstn) begin S_ts_gray_meta<=0; S_ts_gray_sync<=0; end
        else begin S_ts_gray_meta<=S_ts_gray_src; S_ts_gray_sync<=S_ts_gray_meta; end
    end
    function [31:0] F_gray_to_bin(input [31:0] g);
        integer i;
        begin
            F_gray_to_bin[31]=g[31];
            for (i=30;i>=0;i=i-1) F_gray_to_bin[i]=F_gray_to_bin[i+1]^g[i];
        end
    endfunction
    wire [31:0] S_pl_ts=F_gray_to_bin(S_ts_gray_sync);

    //==========================================================
    // 转速通道（§14，v1.2）：采集域 Modbus 主站自由运行（复位仅 S_sensor_rstn，
    // 不受 S_enable 门控，采集侧计数保持连续）。每次事件把整条 192 bit 快照
    // 经一次 cdc_mailbox 送入 AXI 域；AXI 域在 S_accept 拍整体锁存并 seq+1，
    // 寄存器窗口与 DMA 记录（CH=3/4/5）共用这份快照，不逐字段更新。
    //==========================================================
    wire signed [31:0] S_enc_raw, S_enc_rpm;
    wire S_enc_sample_valid, S_enc_rpm_valid, S_enc_configured, S_enc_online;
    wire S_enc_error_valid, S_enc_rpm_overflow, S_enc_rpm_busy;
    wire [3:0] S_enc_error_code;
    wire [7:0] S_enc_exception_code;
    wire [31:0] S_enc_sample_count, S_enc_error_count;
    encoder_modbus #(
        .CLK_HZ(100_000_000),.BAUD(SPEED_BAUD),.SLAVE_ID(8'd1),
        .SAMPLE_MS(SPEED_SAMPLE_MS),.STARTUP_MS(SPEED_STARTUP_MS),
        .POLL_MS(SPEED_POLL_MS),.TIMEOUT_MS(SPEED_TIMEOUT_MS)
    ) X_encoder_modbus (
        .clk(I_sysclk),.rst_n(S_sensor_rstn),
        .rs485_rx(I_rs485_rx),.rs485_tx(O_rs485_tx),.rs485_de(O_rs485_de),
        .raw_speed(S_enc_raw),.sample_valid(S_enc_sample_valid),
        .configured(S_enc_configured),.online(S_enc_online),
        .error_valid(S_enc_error_valid),.error_code(S_enc_error_code),
        .exception_code(S_enc_exception_code),
        .sample_count(S_enc_sample_count),.error_count(S_enc_error_count));
    encoder_rpm #(.COUNTS_PER_REV(SPEED_COUNTS_PER_REV),.SAMPLE_MS(SPEED_SAMPLE_MS))
    X_encoder_rpm (
        .clk(I_sysclk),.rst_n(S_sensor_rstn),
        .raw(S_enc_raw),.raw_valid(S_enc_sample_valid),
        .rpm_milli(S_enc_rpm),.rpm_valid(S_enc_rpm_valid),
        .overflow(S_enc_rpm_overflow),.busy(S_enc_rpm_busy));

    // 采样时刻：raw_speed 有效时锁存本域共同时基（10 ns/LSB）；错误事件记录
    // 各自的事件时刻供 DMA 第 1 字使用。rpm_milli / overflow 与 rpm_valid 同拍
    // 以非阻塞赋值更新，error_code / online 与 error_valid 同拍更新，故发布
    // 统一延后一拍取样。
    reg S_speed_pub_valid, S_speed_pub_sample;
    reg [31:0] S_speed_pub_ts, S_speed_sample_ts, S_speed_error_ts;
    reg S_speed_sample_pend, S_speed_error_pend;
    always @(posedge I_sysclk) begin
        if (!S_sensor_rstn) begin
            S_speed_pub_valid<=0; S_speed_pub_sample<=0; S_speed_pub_ts<=0;
            S_speed_sample_ts<=0; S_speed_error_ts<=0;
            S_speed_sample_pend<=0; S_speed_error_pend<=0;
        end else begin
            S_speed_pub_valid<=0;
            if (S_enc_sample_valid) S_speed_sample_ts<=S_capture_ts;
            if (S_enc_error_valid) begin
                S_speed_error_ts<=S_capture_ts; S_speed_error_pend<=1;
            end
            if (S_enc_rpm_valid) S_speed_sample_pend<=1;
            // 错误事件优先；两者同拍时成功样本顺延一拍，不丢事件。
            if (S_speed_error_pend) begin
                S_speed_error_pend<=0;
                S_speed_pub_valid<=1; S_speed_pub_sample<=0;
                S_speed_pub_ts<=S_speed_error_ts;
            end else if (S_speed_sample_pend) begin
                S_speed_sample_pend<=0;
                S_speed_pub_valid<=1; S_speed_pub_sample<=1;
                S_speed_pub_ts<=S_speed_sample_ts;
            end
        end
    end
    // 事件状态字（与 0xC0 低 16 位、DMA 第 1 字 [15:0] 同一位图）：
    // bit0 在线、bit1 饱和、bit2 本次为错误、bit3 已配置、[7:4] 错误码、[15:8] 异常码。
    wire [7:0] S_speed_ev_code=S_speed_pub_sample ? 8'd0 : {4'd0,S_enc_error_code};
    wire [7:0] S_speed_ev_exc=S_speed_pub_sample ? 8'd0 : S_enc_exception_code;
    wire [7:0] S_speed_ev_flags={S_speed_ev_code[3:0],S_enc_configured,
        S_enc_online,S_enc_rpm_overflow,S_speed_pub_sample};
    wire [191:0] S_speed_ev_data={S_enc_raw,S_enc_rpm,S_enc_sample_count,
        S_enc_error_count,S_speed_pub_ts,S_speed_ev_flags,S_speed_ev_code,
        S_speed_ev_exc,8'h00};
    wire S_speed_mbox_ready;
    wire S_speed_axi_valid;
    wire [191:0] S_speed_axi;
    cdc_mailbox #(.WIDTH(192)) X_speed(
        .I_src_clk(I_sysclk),.I_src_rstn(S_sensor_rstn),
        .I_valid(S_speed_pub_valid),.I_data(S_speed_ev_data),
        .O_ready(S_speed_mbox_ready),.O_overflow(S_speed_mbox_overflow),
        .I_dst_clk(I_clk),.I_dst_rstn(S_axi_rstn),
        .O_valid(S_speed_axi_valid),.O_data(S_speed_axi));
    wire [31:0] S_speed_ev_raw=S_speed_axi[191:160];
    wire [31:0] S_speed_ev_rpm=S_speed_axi[159:128];
    wire [31:0] S_speed_ev_count=S_speed_axi[127:96];
    wire [31:0] S_speed_ev_errc=S_speed_axi[95:64];
    wire [31:0] S_speed_ev_ts=S_speed_axi[63:32];
    wire [7:0] S_speed_ev_flags_axi=S_speed_axi[31:24];
    wire [7:0] S_speed_ev_code_axi=S_speed_axi[23:16];
    wire [7:0] S_speed_ev_exc_axi=S_speed_axi[15:8];
    wire [15:0] S_speed_ev_status_axi={S_speed_ev_exc_axi,
        S_speed_ev_code_axi[3:0],S_speed_ev_flags_axi[3],
        ~S_speed_ev_flags_axi[0],S_speed_ev_flags_axi[1],S_speed_ev_flags_axi[2]};
    // CTRL.EN=0 不更新窗口（§4.3：禁止时不更新数据、计数与故障，不清历史值）；
    // 采集侧不受门控，重新使能后计数连续。
    wire S_speed_event=S_speed_axi_valid && S_accept && S_enable;
    reg signed [31:0] S_speed_raw_l, S_speed_rpm_l;
    reg [31:0] S_speed_count_l, S_speed_ts_l, S_speed_seq_l, S_speed_err_count_l;
    reg [15:0] S_speed_status_l;
    always @(posedge I_clk) begin
        if (!S_axi_rstn || S_soft_reset) begin
            S_speed_raw_l<=0; S_speed_rpm_l<=0; S_speed_count_l<=0;
            S_speed_ts_l<=0; S_speed_seq_l<=0; S_speed_err_count_l<=0;
            S_speed_status_l<=0;
        end else if (S_speed_event) begin
            S_speed_seq_l<=S_speed_seq_l+32'd1;
            S_speed_status_l<=S_speed_ev_status_axi;
            if (S_speed_ev_flags_axi[0]) begin
                S_speed_raw_l<=S_speed_ev_raw;
                S_speed_rpm_l<=S_speed_ev_rpm;
                S_speed_count_l<=S_speed_ev_count;
                S_speed_ts_l<=S_speed_ev_ts;
            end else begin
                // 错误事件：保留最近一次成功的 RAW / RPM / TS，只递增错误计数。
                S_speed_err_count_l<=S_speed_ev_errc;
            end
        end
    end

    //==========================================================
    // DMA 写通道：振动三轴样本打包为 64 bit 字 {ts, ch, sample} 后写 DDR。
    // ts 取最近一次 1 kHz 对齐帧的 acc_ts（10 ns/LSB）；逐样本时间戳需扩 CDC，
    // 因此同一帧间隔内 DMA 字的 ts 相同。转速记录（CH=3/4/5）按 §14 在振动帧
    // 空隙发射，两种记录共用一个数据口。打包缓冲只在 DMA 忙时接收样本，
    // 丢弃计入 DMA_STATUS.overflow。环形模式（DMA_CTRL[3]）由 DMA_BLOCKS 定环大小，
    // 每完成一块产生 O_block_done 脉冲与 BLK_SEQ 计数，中断由 DMA_IRQ 控制。
    //==========================================================
    wire S_dma_start, S_dma_abort, S_dma_stop, S_dma_test, S_dma_busy, S_dma_done;
    wire S_dma_ring, S_dma_block_done;
    wire [31:0] S_dma_blocks, S_dma_blk_seq;
    wire S_dma_s_ready;
    wire [2:0] S_dma_error;
    wire [63:0] S_dma_base;
    wire [31:0] S_dma_bytes, S_dma_written;
    wire S_acc_event=S_raw_axi_valid && S_accept && S_raw_axi[1];
    // 振动帧 3 字（CH=0/1/2）与转速记录 3 字（CH=3/4/5）共用同一个 DMA 数据口。
    // 仲裁：转速记录只在振动打包器空闲时起步，起步后独占数据口直到第 3 字发完；
    // 振动帧三字发完前转速记录不插队，因此两种记录都不被对方打断。新事件到达而
    // 上一条转速记录尚未发完时置丢弃标志（仅 DMA 忙时计数），丢弃计入
    // DMA_STATUS.overflow；DMA 空闲时既不记录也不计丢。
    reg [63:0] S_vib_x, S_vib_y, S_vib_z;
    reg [1:0] S_vib_index;
    reg S_vib_valid, S_vib_overflow;
    reg [63:0] S_speed_w0, S_speed_w1, S_speed_w2;
    reg [1:0] S_speed_index;
    reg S_speed_valid, S_speed_run, S_speed_discard;
    wire S_speed_sel=S_speed_run || (S_speed_valid && S_speed_index==0 && !S_vib_valid);
    wire S_speed_take=S_speed_sel && S_dma_s_ready;
    wire S_speed_done=S_speed_take && S_speed_index==2'd2;
    wire S_vib_take=S_vib_valid && S_dma_s_ready && !S_speed_sel;
    wire S_vib_done=S_vib_take && S_vib_index==2'd2;
    wire S_vib_free=!S_vib_valid || S_vib_done;
    wire [63:0] S_vib_data=(S_vib_index==2'd0) ? S_vib_x :
                            (S_vib_index==2'd1) ? S_vib_y : S_vib_z;
    wire [63:0] S_speed_data=(S_speed_index==2'd0) ? S_speed_w0 :
                              (S_speed_index==2'd1) ? S_speed_w1 : S_speed_w2;
    wire [63:0] S_pack_data=S_speed_sel ? S_speed_data : S_vib_data;
    wire S_pack_valid=S_speed_sel ? 1'b1 : S_vib_valid;
    always @(posedge I_clk) begin
        if (!S_axi_rstn || S_soft_reset || !S_dma_busy) begin
            S_vib_valid<=0; S_vib_index<=0; S_vib_overflow<=0;
            S_speed_valid<=0; S_speed_index<=0; S_speed_run<=0;
        end else begin
            if (S_vib_take) begin
                if (S_vib_index==2'd2) S_vib_valid<=0;
                else S_vib_index<=S_vib_index+1'b1;
            end
            if (S_acc_event) begin
                if (!S_vib_free) S_vib_overflow<=1;
                else begin
                    S_vib_x<={S_frame_hold[95:64],16'd0,S_raw_axi[66:51]};
                    S_vib_y<={S_frame_hold[95:64],16'd1,S_raw_axi[50:35]};
                    S_vib_z<={S_frame_hold[95:64],16'd2,S_raw_axi[34:19]};
                    S_vib_valid<=1; S_vib_index<=0;
                end
            end
            if (S_speed_take) begin
                if (S_speed_index==2'd2) begin
                    S_speed_valid<=0; S_speed_index<=2'd0; S_speed_run<=0;
                end else begin
                    S_speed_index<=S_speed_index+1'b1; S_speed_run<=1'b1;
                end
            end
            // 新事件：上一条记录未发完且本拍没有完成则丢弃（粘滞，软复位清）；
            // 否则整体装入三字，第 1 字状态字与寄存器窗口同一位图。
            if (S_speed_event) begin
                if (!((S_speed_valid || S_speed_run) && !S_speed_done)) begin
                    S_speed_w0<={S_speed_ev_ts,16'h0003,S_speed_ev_status_axi};
                    S_speed_w1<={S_speed_ev_rpm,16'h0004,16'd0};
                    S_speed_w2<={S_speed_ev_raw,16'h0005,16'd0};
                    S_speed_valid<=1; S_speed_index<=2'd0; S_speed_run<=0;
                end
            end
        end
    end
    // 丢弃标志为粘滞位，只在硬复位 / 软复位清除（DMA 停止不清）。
    always @(posedge I_clk) begin
        if (!S_axi_rstn || S_soft_reset) S_speed_discard<=0;
        else if (S_speed_event && S_dma_busy &&
                 (S_speed_valid || S_speed_run) && !S_speed_done)
            S_speed_discard<=1;
    end
    sensor_dma X_dma(
        .I_clk(I_clk),.I_rstn(S_axi_rstn),
        .I_start(S_dma_start),.I_abort(S_dma_abort),.I_stop(S_dma_stop),
        .I_base(S_dma_base),.I_bytes(S_dma_bytes),.I_test(S_dma_test),
        .I_ring(S_dma_ring),.I_blocks(S_dma_blocks),
        .O_busy(S_dma_busy),.O_done(S_dma_done),.O_error(S_dma_error),
        .O_written(S_dma_written),.O_block_done(S_dma_block_done),.O_blk_seq(S_dma_blk_seq),
        .I_s_data(S_pack_data),.I_s_valid(S_pack_valid),.O_s_ready(S_dma_s_ready),
        .M_axi_awaddr(M_axi_awaddr),.M_axi_awlen(M_axi_awlen),.M_axi_awsize(M_axi_awsize),
        .M_axi_awburst(M_axi_awburst),.M_axi_awvalid(M_axi_awvalid),.M_axi_awready(M_axi_awready),
        .M_axi_wdata(M_axi_wdata),.M_axi_wstrb(M_axi_wstrb),.M_axi_wlast(M_axi_wlast),
        .M_axi_wvalid(M_axi_wvalid),.M_axi_wready(M_axi_wready),
        .M_axi_bresp(M_axi_bresp),.M_axi_bvalid(M_axi_bvalid),.M_axi_bready(M_axi_bready));
    axi_lite_reg #(.CLK_FREQ_HZ(AXI_CLK_FREQ_HZ),.EXTERNAL_FRAME(1),.EXTERNAL_FFT(1),.WITH_FEATURES(WITH_FEATURES),
        .SPECT_LOG_N(LOGN)) X_regs(
        .O_alarm(O_alarm), .I_clk(I_clk),.I_rstn(S_axi_rstn),
        .I_awaddr(I_awaddr),
        .I_awvalid(I_awvalid),
        .O_awready(O_awready),
        .I_wdata(I_wdata),
        .I_wstrb(I_wstrb),
        .I_wvalid(I_wvalid),
        .O_wready(O_wready),
        .O_bresp(O_bresp),
        .O_bvalid(O_bvalid),
        .I_bready(I_bready),
        .I_araddr(I_araddr),
        .I_arvalid(I_arvalid),
        .O_arready(O_arready),
        .O_rdata(O_rdata),
        .O_rresp(O_rresp),
        .O_rvalid(O_rvalid),
        .I_rready(I_rready),
        .I_busy(S_enable),.I_overflow(S_overflow_sync[1] || S_clear_overflow),
        .I_done(S_frame_axi_valid && S_accept),.I_state(8'd0),
        .I_sample(S_raw_axi[66:51]),.I_sample_valid(S_valid[1]),
        .I_temp(S_raw_axi[82:67]),.I_acc_x(S_raw_axi[66:51]),.I_acc_y(S_raw_axi[50:35]),
        .I_acc_z(S_raw_axi[34:19]),.I_current(S_raw_axi[18:3]),
        .I_temp_valid(S_valid[2]),.I_acc_valid(S_valid[1]),.I_current_valid(S_valid[0]),
        .I_faults(S_faults_axi | {3'd0,S_overflow_sync[1],4'd0}),
        .I_feature_valid(S_feature_axi_valid & {4{S_accept}}),.I_features(S_features_axi),
        .I_feature_flags(S_feature_flags_axi),.O_enable(S_enable),.O_irq_enable(S_irq_enable),
        .O_soft_reset(S_soft_reset),.O_irq(O_irq),.I_frame(S_frame_hold),
        .I_fft_sequence(S_fft_hold[159:128]),.I_fft_bin(S_fft_hold[127:96]),
        .I_fft_power(S_fft_hold[95:32]),.I_fft_flags(S_fft_hold[31:0]),
        .I_spect_push(S_spect_push),.I_spect_data(S_spect_data),
        .I_spect_index(S_spect_index),.O_spect_ack(S_spect_ack),
        .I_spect_event(S_spect_event),.O_spect_done(S_spect_done),
        .O_dma_start(S_dma_start),.O_dma_abort(S_dma_abort),.O_dma_stop(S_dma_stop),
        .O_dma_base(S_dma_base),.O_dma_bytes(S_dma_bytes),.O_dma_test(S_dma_test),
        .I_dma_busy(S_dma_busy),.I_dma_done(S_dma_done),.I_dma_error(S_dma_error),
        .I_dma_written(S_dma_written),
        .O_dma_ring(S_dma_ring),.O_dma_blocks(S_dma_blocks),
        .I_dma_blk_seq(S_dma_blk_seq),.I_dma_block_done(S_dma_block_done),
        .O_dma_irq(O_dma_irq),.I_pl_ts(S_pl_ts),
        .I_dma_overflow(S_vib_overflow || S_speed_discard),
        .I_speed_raw(S_speed_raw_l),.I_speed_rpm(S_speed_rpm_l),
        .I_speed_status({16'd0,S_speed_status_l}),
        .I_speed_count(S_speed_count_l),.I_speed_ts(S_speed_ts_l),
        .I_speed_seq(S_speed_seq_l),.I_speed_err_count(S_speed_err_count_l));
endmodule
