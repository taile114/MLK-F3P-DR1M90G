// 故障诊断 AXI4-Lite 寄存器本体（接口 v1.3，ID=0x504C0103）；窗口偏移以
// docs/PL-PS接口约定.md 为准。
// 单个未完成读/写事务；AW/W 独立缓冲，响应背压期间数据保持，支持 WSTRB。
// 与传感器/特征块同一时钟域；本模块不例化 PS 或 AXI 互联。
// 完整频谱窗口（§4.6）：AXI 只读本域快照 RAM；快照由采集域整窗结束后逐字
// 推入（push/ack 单 bit 翻转沿 + 随沿稳定的数据、索引），本域没有跨域存储。
module axi_lite_reg #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer EXTERNAL_FRAME = 0,
    parameter integer EXTERNAL_FFT = 0,
    // WITH_FEATURES=0 时不实现特征快照存储（feature_bank），0x100~0x1FF 读 0
    // 并正常返回 OKAY；合并码流关闭特征单元时同步关闭。
    parameter integer WITH_FEATURES = 1,
    // 完整频谱窗口 bin 数指数：N=1<<SPECT_LOG_N，窗口 0x400+4k，k=0..N-1。
    parameter integer SPECT_LOG_N = 8
)(
    input wire I_clk, I_rstn,
    input wire [15:0] I_awaddr,
    input wire I_awvalid,
    output wire O_awready,
    input wire [31:0] I_wdata,
    input wire [3:0] I_wstrb,
    input wire I_wvalid,
    output wire O_wready,
    output reg [1:0] O_bresp,
    output reg O_bvalid,
    input wire I_bready,
    input wire [15:0] I_araddr,
    input wire I_arvalid,
    output wire O_arready,
    output reg [31:0] O_rdata,
    output reg [1:0] O_rresp,
    output reg O_rvalid,
    input wire I_rready,
    input wire I_busy, I_overflow, I_done,
    input wire [7:0] I_state,
    input wire [15:0] I_sample,
    input wire I_sample_valid,
    input wire signed [15:0] I_temp, I_acc_x, I_acc_y, I_acc_z, I_current,
    input wire I_temp_valid, I_acc_valid, I_current_valid,
    input wire [7:0] I_faults,
    input wire [3:0] I_feature_valid,
    input wire [1791:0] I_features,
    input wire [7:0] I_feature_flags,
    output wire O_enable, O_irq_enable,
    output reg O_soft_reset,
    output wire O_irq,
    input wire [255:0] I_frame,
    input wire [31:0] I_fft_sequence, I_fft_bin, I_fft_flags,
    input wire [63:0] I_fft_power,
    // DMA 写通道（0x90~0xB4）
    output reg O_dma_start, O_dma_abort, O_dma_stop,
    output reg [63:0] O_dma_base,
    output reg [31:0] O_dma_bytes,
    output reg O_dma_test,
    input wire I_dma_busy, I_dma_done,
    input wire [2:0] I_dma_error,
    input wire [31:0] I_dma_written,
    input wire I_dma_overflow,
    // 环形模式与块完成中断（0xA8~0xB4）；O_dma_irq 为 DMA_IRQ[16] 的 PEND 电平（LEVEL_HIGH）
    output reg O_dma_ring,
    output reg [31:0] O_dma_blocks,
    input wire [31:0] I_dma_blk_seq,
    input wire I_dma_block_done,
    output wire O_dma_irq,
    input wire [31:0] I_pl_ts,
    // 转速通道（§14，v1.2 新增）：0xB8~0xD0 全部只读。本域在事件接受拍
    // 整体锁存一次快照，SPEED_SEQ 与各字段同拍更新；DMA 记录复用同一快照。
    input wire signed [31:0] I_speed_raw,
    input wire signed [31:0] I_speed_rpm,
    input wire [31:0] I_speed_status,
    input wire [31:0] I_speed_count,
    input wire [31:0] I_speed_ts,
    input wire [31:0] I_speed_seq,
    input wire [31:0] I_speed_err_count,
    // 完整频谱窗口（§4.6）：采集域整窗结束后逐字推入，本域按索引写快照。
    // I_spect_push 为翻转沿（每字一次），I_spect_data/I_spect_index 随该沿
    // 保持不变；O_spect_ack 为回执翻转沿。I_spect_event 为本域单拍脉冲，
    // 表示该窗结果字已到，供结果字暂存；O_spect_done 为整窗推完的单拍脉冲，
    // 供结果字与快照同拍发布。
    input wire I_spect_push,
    input wire [31:0] I_spect_data,
    input wire [SPECT_LOG_N-1:0] I_spect_index,
    output wire O_spect_ack,
    input wire I_spect_event,
    output wire O_spect_done,
    output wire O_alarm
);
    wire S_tick;
    wire [31:0] S_ts, S_ms, S_frame_ts, S_acc_ts, S_current_ts, S_temp_ts;
    wire signed [15:0] S_frame_temp, S_frame_acc_x, S_frame_acc_y, S_frame_acc_z, S_frame_current;
    generate if (EXTERNAL_FRAME != 0) begin : G_external_frame
        assign {S_frame_ts, S_frame_temp, S_frame_acc_x, S_frame_acc_y,
                S_frame_acc_z, S_frame_current, S_acc_ts, S_current_ts, S_temp_ts} = I_frame[207:0];
        assign S_tick=0; assign S_ts=0; assign S_ms=0;
    end else begin : G_local_frame
    timebase #(.CLK_FREQ_HZ(CLK_FREQ_HZ)) X_timebase(
        .I_clk(I_clk),.I_rstn(I_rstn),.I_enable(O_enable),.I_clear(O_soft_reset),
        .I_temp_valid(I_temp_valid),.I_acc_valid(I_acc_valid),.I_current_valid(I_current_valid),
        .I_temp(I_temp),.I_acc_x(I_acc_x),.I_acc_y(I_acc_y),.I_acc_z(I_acc_z),.I_current(I_current),
        .O_tick(S_tick),.O_ts(S_ts),.O_ms(S_ms),.O_frame_ts(S_frame_ts),
        .O_frame_temp(S_frame_temp),.O_frame_acc_x(S_frame_acc_x),.O_frame_acc_y(S_frame_acc_y),
        .O_frame_acc_z(S_frame_acc_z),.O_frame_current(S_frame_current),
        .O_acc_ts(S_acc_ts),.O_current_ts(S_current_ts),.O_temp_ts(S_temp_ts));
    end endgenerate

    reg S_awfull,S_wfull;
    reg [15:0] S_awaddr;
    reg [31:0] S_wdata;
    reg [3:0] S_wstrb;
    wire S_write=S_awfull && S_wfull && !O_bvalid;
    wire S_aligned=S_awaddr[1:0]==0;
    wire [5:0] S_alarm_state;
    wire [31:0] S_alarm_status;
    alarm_sequencer #(.CLK_FREQ_HZ(CLK_FREQ_HZ)) X_alarm (
        .clk(I_clk), .rst_n(I_rstn), .clear_i(O_soft_reset),
        .write_i(S_write && S_aligned && S_awaddr==16'hd4 && S_wstrb[0]),
        .state_i(S_wdata[5:0]), .alarm_o(O_alarm),
        .state_o(S_alarm_state), .status_o(S_alarm_status));
    wire S_ctrl_write=S_write && S_aligned && S_awaddr==16'h04 && S_wstrb[0];
    wire S_clear_irq=S_write && S_awaddr==16'h10 && S_wstrb[0] && S_wdata[0];
    wire S_clear_ovf=S_write && S_awaddr==16'h08 && S_wstrb[0] && S_wdata[1];
    wire S_snapshot=S_write && S_awaddr==16'h14 && S_wstrb[0] && S_wdata[0];
    // DMA_CTRL：START / ABORT / STOP 单拍自清，TEST / RING 为电平；
    // DONE、溢出与块计数由状态输入维护。
    wire S_dma_ctrl_wr=S_write && S_aligned && S_awaddr==16'h90 && S_wstrb[0];
    wire S_dma_start_wr=S_dma_ctrl_wr && S_wdata[0];
    wire S_dma_abort_wr=(S_dma_ctrl_wr && S_wdata[1]) || O_soft_reset;
    // STOP（[4]）：写 1 产生一拍 I_stop，sensor_dma 在当前突发收尾后释放 BUSY。
    // 写 0x90 同时锁存 TEST / RING（沿用现有语义），因此 STOP 写须携带运行中的模式位。
    wire S_dma_stop_wr=S_dma_ctrl_wr && S_wdata[4];
    // DMA_IRQ（0xAC）：[0] 块完成中断使能，[8] W1C 清挂起，RO [16] 挂起。
    // p2f_irq 只用于产生挂起标志；块完成中断线 O_dma_irq 由 PEND 电平直接驱动
    // （LEVEL_HIGH，PS 侧按电平触发注册），PS 写 [8]=1 清挂起后线路拉低。
    // O_pulse 悬空：不再使用单拍脉冲，p2f_irq 输出不接不产生未驱动告警。
    wire S_dma_irq_ctrl_wr=S_write && S_aligned && S_awaddr==16'hac;
    wire S_dma_irq_clear=S_dma_irq_ctrl_wr && S_wstrb[1] && S_wdata[8];
    reg S_dma_irq_en;
    wire S_dma_irq_pending;
    p2f_irq X_dma_irq(.I_clk(I_clk),.I_rstn(I_rstn && !O_soft_reset),
        .I_clear(S_dma_irq_clear),.I_event(I_dma_block_done),.I_enable(S_dma_irq_en),
        .O_pending(S_dma_irq_pending),.O_pulse());
    // 线路电平 = PEND；[0] 使能位只影响 p2f_irq 内部脉冲，不改变 PEND 与线路。
    assign O_dma_irq=S_dma_irq_pending;
    reg [31:0] S_dma_wr_prev;
    reg [15:0] S_dma_bursts;
    reg S_dma_done, S_dma_ovf;
    reg [7:0] S_ctrl;
    reg S_ovf;
    reg [31:0] S_data;
    reg signed [15:0] S_temp,S_x,S_y,S_z,S_current;
    reg [15:0] S_temp_count,S_acc_count,S_current_count;
    reg [15:0] S_temp_threshold,S_acc_threshold,S_current_threshold;
    reg [7:0] S_faults;
    reg signed [31:0] S_current_zero, S_current_gain;
    wire [15:0] S_absx=I_acc_x[15] ? (~I_acc_x+16'd1) : I_acc_x;
    wire [15:0] S_absy=I_acc_y[15] ? (~I_acc_y+16'd1) : I_acc_y;
    wire [15:0] S_absz=I_acc_z[15] ? (~I_acc_z+16'd1) : I_acc_z;
    wire [15:0] S_absc=I_current[15] ? (~I_current+16'd1) : I_current;
    wire S_threshold=(I_temp_valid && S_temp_threshold!=0 && I_temp>$signed(S_temp_threshold)) ||
        (I_acc_valid && S_acc_threshold!=0 && (S_absx>S_acc_threshold || S_absy>S_acc_threshold || S_absz>S_acc_threshold)) ||
        (I_current_valid && S_current_threshold!=0 && S_absc>S_current_threshold);
    wire S_event=O_enable && (I_done || |I_feature_valid || S_threshold);
    wire S_pending;
    p2f_irq X_irq(.I_clk(I_clk),.I_rstn(I_rstn && !O_soft_reset),
        .I_clear(S_clear_irq),.I_event(S_event),.I_enable(O_irq_enable),
        .O_pending(S_pending),.O_pulse(O_irq));
    wire [31:0] S_feature_data;
    wire [3:0] S_available;
    generate if (WITH_FEATURES != 0) begin : G_feature_bank
        feature_bank X_bank(.I_clk(I_clk),.I_rstn(I_rstn),.I_clear(O_soft_reset),
            .I_snapshot(S_snapshot),.I_valid(I_feature_valid & {4{O_enable}}),
            .I_features(I_features),.I_flags(I_feature_flags),
            .I_word_addr(I_araddr[9:2]),.O_data(S_feature_data),.O_available(S_available));
    end else begin : G_no_feature_bank
        // 不例化特征快照存储；窗口 0x100~0x1FF 仍按已映射地址读回 0。
        assign S_feature_data=32'd0;
        assign S_available=4'd0;
    end endgenerate
    //==========================================================
    // 完整频谱窗口（§4.6）。窗口 0x400+4k 存 BIN_POWER[k]=(|X_k|²)>>8，
    // k=0..N-1（N=1<<SPECT_LOG_N）。
    // 采集域在整窗结束后逐字推入，本域按 I_spect_index 写快照 RAM；AXI 只读
    // 快照，一次读拿到的是最近一个完整窗口。跨域只有 push/ack 两个单 bit
    // 翻转沿与随 push 稳定的数据、索引总线，没有跨域存储。
    // 索引 0 到达表示一窗开始（置 S_push_open），最后一字写完时置快照有效位
    // 并给出一拍 O_spect_done，结果字发布方据此同拍更新 0x78，因此不存在
    // 「序号已更新、bin 仍是上一窗」或反向的可见时刻。
    // 软复位只清快照有效位与推送状态：该窗剩余的字仍会写入快照，但
    // S_push_open 已清，不会置有效位，读到的是 0；下一窗从索引 0 重新开始。
    // 快照 RAM 不参与复位。
    //==========================================================
    localparam integer SPECT_BINS=1<<SPECT_LOG_N;
    localparam [16:0] SPECT_LIMIT=17'h0400+(SPECT_BINS*4);
    localparam [SPECT_LOG_N-1:0] SPECT_LAST_IDX=SPECT_BINS-1;
    wire S_spect_sel=(I_araddr>=16'h0400) && ({1'b0,I_araddr}<SPECT_LIMIT) &&
        (I_araddr[1:0]==0);
    wire [15:0] S_spect_off=I_araddr-16'h0400;
    wire [SPECT_LOG_N-1:0] S_spect_bin=S_spect_off[SPECT_LOG_N+1:2];
    (* ram_style = "bram" *) reg [31:0] S_snap[0:SPECT_BINS-1];
    reg [31:0] S_snap_q;
    reg [SPECT_LOG_N-1:0] S_axi_bin;
    reg [1:0] S_spect_phase;
    reg S_snap_valid;
    (* ASYNC_REG = "TRUE" *) reg [1:0] S_push_sync;
    reg S_push_ack,S_push_open,S_push_busy,S_done_pulse;
    wire S_push_edge=S_push_sync[1]^S_push_ack;
    assign O_spect_ack=S_push_ack;
    assign O_spect_done=S_done_pulse;
    // 快照读口：地址在 AR 握手时锁存，数据下一拍有效；未写入值由
    // S_snap_valid 屏蔽。
    always @(posedge I_clk) S_snap_q<=S_snap[S_axi_bin];
    always @(posedge I_clk or negedge I_rstn)
        if(!I_rstn) S_push_sync<=2'd0;
        else S_push_sync<={S_push_sync[0],I_spect_push};
    // 推送接收：push 翻转沿到达时按索引写快照并翻转 ack 回执。索引 0 置开窗
    // 标志，最后一字与快照有效位、完成脉冲同拍。ack 不参与软复位，避免复位
    // 打断在途传输造成握手相位错位（对端仍在等该字的回执）。
    always @(posedge I_clk) begin
        S_done_pulse<=1'b0;
        if(!I_rstn) begin
            S_push_ack<=0;S_push_open<=0;S_push_busy<=0;S_snap_valid<=0;
        end else if(O_soft_reset) begin
            S_push_open<=0;S_push_busy<=0;S_snap_valid<=0;
        end else if(S_push_edge) begin
            S_snap[I_spect_index]<=I_spect_data;
            S_push_ack<=S_push_sync[1];
            if(I_spect_index==0) begin S_push_open<=1;S_push_busy<=1; end
            if(I_spect_index==SPECT_LAST_IDX) begin
                S_push_busy<=0;
                if(S_push_open) begin
                    S_push_open<=0;S_snap_valid<=1;S_done_pulse<=1;
                end
            end
        end
    end
    assign O_awready=I_rstn && !S_awfull && !O_bvalid;
    assign O_wready=I_rstn && !S_wfull && !O_bvalid;
    // 频谱读占用 2 拍的快照读取，其间不接受新的读地址。
    assign O_arready=I_rstn && !O_rvalid && S_spect_phase==0;
    assign O_enable=S_ctrl[0];
    assign O_irq_enable=S_ctrl[2];
    reg [31:0] S_read_data;
    reg S_read_ok, S_write_ok;
    always @* begin
        S_read_data=0; S_read_ok=1;
        case(I_araddr)
            16'h00: S_read_data=32'h504c0103;
            16'h04: S_read_data={24'd0,S_ctrl};
            16'h08: S_read_data={16'd0,I_state,6'd0,S_ovf,I_busy};
            16'h0c: S_read_data=S_data;
            16'h10: S_read_data={31'd0,S_pending};
            16'h14: S_read_data=0;
            16'h18: S_read_data={28'd0,S_available};
            16'h20: S_read_data={{16{S_temp[15]}},S_temp};
            16'h24: S_read_data={{16{S_x[15]}},S_x};
            16'h28: S_read_data={{16{S_y[15]}},S_y};
            16'h2c: S_read_data={{16{S_z[15]}},S_z};
            16'h30: S_read_data={{16{S_current[15]}},S_current};
            16'h34: S_read_data={S_acc_count,S_temp_count};
            16'h38: S_read_data={16'd0,S_current_count};
            16'h3c: S_read_data={24'd0,S_faults};
            16'h40: S_read_data={{16{S_temp_threshold[15]}},S_temp_threshold};
            16'h44: S_read_data={16'd0,S_acc_threshold};
            16'h48: S_read_data={16'd0,S_current_threshold};
            16'h4c: S_read_data=S_current_zero;
            16'h50: S_read_data=S_current_gain;
            16'h54: S_read_data=S_frame_ts;
            16'h58: S_read_data={{16{S_frame_temp[15]}},S_frame_temp};
            16'h5c: S_read_data={{16{S_frame_acc_x[15]}},S_frame_acc_x};
            16'h60: S_read_data={{16{S_frame_acc_y[15]}},S_frame_acc_y};
            16'h64: S_read_data={{16{S_frame_acc_z[15]}},S_frame_acc_z};
            16'h68: S_read_data={{16{S_frame_current[15]}},S_frame_current};
            16'h6c: S_read_data=S_acc_ts;
            16'h70: S_read_data=S_current_ts;
            16'h74: S_read_data=S_temp_ts;
            16'h78: S_read_data=(EXTERNAL_FFT!=0) ? I_fft_sequence : 0;
            16'h7c: S_read_data=(EXTERNAL_FFT!=0) ? I_fft_bin : 0;
            16'h80: S_read_data=(EXTERNAL_FFT!=0) ? I_fft_power[31:0] : 0;
            16'h84: S_read_data=(EXTERNAL_FFT!=0) ? I_fft_power[63:32] : 0;
            16'h88: S_read_data=(EXTERNAL_FFT!=0) ? I_fft_flags : 0;
            16'h90: S_read_data={21'd0,|I_dma_error,S_dma_done,I_dma_busy,{4'd0,O_dma_ring},O_dma_test,2'd0};
            16'h94: S_read_data=O_dma_base[31:0];
            16'h98: S_read_data=O_dma_base[63:32];
            16'h9c: S_read_data=O_dma_bytes;
            16'ha0: S_read_data={12'd0,I_dma_error,S_dma_ovf,S_dma_bursts};
            16'ha4: S_read_data=I_dma_written;
            16'ha8: S_read_data=I_dma_blk_seq;
            16'hac: S_read_data={15'd0,S_dma_irq_pending,15'd0,S_dma_irq_en};
            16'hb0: S_read_data=I_pl_ts;
            16'hb4: S_read_data=O_dma_blocks;
            // 转速窗口（§14）：全部只读，写白名单不变（写入 SLVERR）。
            16'hb8: S_read_data=I_speed_raw;
            16'hbc: S_read_data=I_speed_rpm;
            16'hc0: S_read_data=I_speed_status;
            16'hc4: S_read_data=I_speed_count;
            16'hc8: S_read_data=I_speed_ts;
            16'hcc: S_read_data=I_speed_seq;
            16'hd0: S_read_data=I_speed_err_count;
            16'hd4: S_read_data={26'd0,S_alarm_state};
            16'hd8: S_read_data=S_alarm_status;
            default: begin
                if(I_araddr>=16'h100 && I_araddr<16'h200 && I_araddr[1:0]==0)
                    S_read_data=S_feature_data;
                else if(!S_spect_sel) S_read_ok=0;
            end
        endcase
        S_write_ok=S_aligned && (S_awaddr==16'h04 || S_awaddr==16'h08 ||
            S_awaddr==16'h10 || S_awaddr==16'h14 || S_awaddr==16'h40 ||
            S_awaddr==16'h44 || S_awaddr==16'h48 ||
            S_awaddr==16'h4c || S_awaddr==16'h50 ||
            S_awaddr==16'h90 || S_awaddr==16'h94 || S_awaddr==16'h98 ||
            S_awaddr==16'h9c || S_awaddr==16'hac || S_awaddr==16'hb4 || S_awaddr==16'hd4);
    end
    integer b;
    always @(posedge I_clk) begin
        if(!I_rstn) begin
            S_awfull<=0;S_wfull<=0;S_awaddr<=0;S_wdata<=0;S_wstrb<=0;
            O_bvalid<=0;O_bresp<=0;O_rvalid<=0;O_rdata<=0;O_rresp<=0;
            S_axi_bin<=0;S_spect_phase<=0;
            S_ctrl<=0;O_soft_reset<=0;
            S_temp_threshold<=0;S_acc_threshold<=0;S_current_threshold<=0;
            S_current_zero<=0;S_current_gain<=0;
            O_dma_base<=0;O_dma_bytes<=0;O_dma_blocks<=0;S_dma_irq_en<=0;
        end else begin
            O_soft_reset<=0;
            if(I_awvalid && O_awready) begin S_awaddr<=I_awaddr;S_awfull<=1;end
            if(I_wvalid && O_wready) begin S_wdata<=I_wdata;S_wstrb<=I_wstrb;S_wfull<=1;end
            if(O_bvalid && I_bready) O_bvalid<=0;
            if(S_write) begin
                S_awfull<=0; S_wfull<=0; O_bvalid<=1; O_bresp<=S_write_ok ? 2'b00 : 2'b10;
                if(S_ctrl_write) begin S_ctrl<=S_wdata[7:0] & 8'hf5; O_soft_reset<=S_wdata[1];end
                if(S_write_ok) for(b=0;b<4;b=b+1) if(S_wstrb[b]) begin
                    case(S_awaddr)
                        16'h40:if(b<2) S_temp_threshold[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h44:if(b<2) S_acc_threshold[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h48:if(b<2) S_current_threshold[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h4c:S_current_zero[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h50:S_current_gain[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h94:O_dma_base[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h98:O_dma_base[32+b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'h9c:O_dma_bytes[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        16'hac:if(b==0) S_dma_irq_en<=S_wdata[0];
                        16'hb4:O_dma_blocks[b*8 +: 8]<=S_wdata[b*8 +: 8];
                        default: begin end
                    endcase
                end
            end
            if(O_rvalid && I_rready) O_rvalid<=0;
            if(I_arvalid && O_arready) begin
                if(S_spect_sel) begin
                    // 频谱读：锁存 bin 地址，快照 RAM 数据下一拍可得。
                    S_axi_bin<=S_spect_bin; S_spect_phase<=1;
                end else begin
                    O_rvalid<=1; O_rdata<=S_read_data; O_rresp<=S_read_ok ? 2'b00 : 2'b10;
                end
            end
            if(S_spect_phase==1) S_spect_phase<=2;
            else if(S_spect_phase==2) begin
                // 推送进行中快照是半新半旧的，回到等待拍重取，直到整窗推完。
                if(S_push_busy) S_spect_phase<=1;
                else begin
                    // 快照数据在 O_rvalid 时有效；未推完过窗口时读 0。
                    S_spect_phase<=0; O_rvalid<=1; O_rresp<=2'b00;
                    O_rdata<=S_snap_valid ? S_snap_q : 32'd0;
                end
            end
        end
    end
    always @(posedge I_clk) begin
        if(!I_rstn || O_soft_reset) begin
            S_ovf<=0;S_data<=0;S_temp<=0;S_x<=0;S_y<=0;S_z<=0;S_current<=0;
            S_temp_count<=0;S_acc_count<=0;S_current_count<=0;S_faults<=0;
        end else begin
            S_ovf<=(S_ovf && !S_clear_ovf) || (O_enable && I_overflow);
            if(O_enable) begin
                S_faults<=I_faults;
                if(I_sample_valid) S_data<={S_data[31:16]+16'd1,I_sample};
                if(I_temp_valid) begin S_temp<=I_temp;S_temp_count<=S_temp_count+1'b1;end
                if(I_acc_valid) begin S_x<=I_acc_x;S_y<=I_acc_y;S_z<=I_acc_z;S_acc_count<=S_acc_count+1'b1;end
                if(I_current_valid) begin S_current<=I_current;S_current_count<=S_current_count+1'b1;end
            end
        end
    end
    // DMA_CTRL 的 START / ABORT 推迟一拍送出，使 sensor_dma 采到本次写入的
    // DMA_BASE / DMA_BYTES；TEST / RING 为电平，随 0x90 写入更新；
    // DONE 与 BURSTS 由完成脉冲 / O_written 增量维护，
    // OVF 粘滞到下次 START。BURSTS 计本次传输已完成突发数（§4.4）。
    always @(posedge I_clk) begin
        if(!I_rstn) begin
            O_dma_start<=0;O_dma_abort<=0;O_dma_stop<=0;O_dma_test<=0;O_dma_ring<=0;
            S_dma_wr_prev<=0;S_dma_bursts<=0;S_dma_done<=0;S_dma_ovf<=0;
        end else begin
            O_dma_start<=S_dma_start_wr;
            O_dma_abort<=S_dma_abort_wr;
            O_dma_stop<=S_dma_stop_wr;
            if(S_dma_ctrl_wr) begin O_dma_test<=S_wdata[2]; O_dma_ring<=S_wdata[3]; end
            if(S_dma_start_wr) begin
                S_dma_done<=0;S_dma_ovf<=0;S_dma_bursts<=0;S_dma_wr_prev<=0;
            end else begin
                if(I_dma_done) S_dma_done<=1;
                if(I_dma_overflow) S_dma_ovf<=1;
                if(I_dma_busy && I_dma_written>S_dma_wr_prev) S_dma_bursts<=S_dma_bursts+1'b1;
                S_dma_wr_prev<=I_dma_written;
            end
        end
    end
endmodule
