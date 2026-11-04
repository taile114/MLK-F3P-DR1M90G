// 综合验证壳：四路时域、256/512 点 FFT、AXI/快照/IRQ 的可观测负载。
// 仅用于 headless 资源/时序验证，不用于下载；不修改生产顶层与冻结约束。
module validation_top(
    input wire I_sysclk,
    output wire [3:0] O_led
);
    reg [15:0] S_reset_count;
    initial S_reset_count=0;
    wire S_rstn=&S_reset_count;
    reg [31:0] S_lfsr;
    reg [15:0] S_count;
    wire S_tick=S_count==0;
    always @(posedge I_sysclk) begin
        if(!S_rstn) S_reset_count<=S_reset_count+1'b1;
        if(!S_rstn) begin S_count<=0;S_lfsr<=32'h13579bdf;end
        else begin
            S_count<=S_count+1'b1;
            S_lfsr<={S_lfsr[30:0],S_lfsr[31]^S_lfsr[21]^S_lfsr[1]^S_lfsr[0]};
        end
    end
    wire [3:0] S_fready,S_fvalid,S_overrun;
    wire [7:0] S_flags;
    wire [1791:0] S_features;
    sensor_features X_features(.I_clk(I_sysclk),.I_rstn(S_rstn),
        .I_acc_valid(S_tick),.I_current_valid(S_tick),.I_acc_x(S_lfsr[15:0]),
        .I_acc_y(S_lfsr[31:16]),.I_acc_z(S_lfsr[23:8]),.I_current(S_lfsr[19:4]),
        .O_ready(S_fready),.O_valid(S_fvalid),.O_overrun(S_overrun),
        .O_flags(S_flags),.O_features(S_features));
    wire [1:0] S_fftvalid,S_fftready,S_last;
    wire [63:0] S_real,S_imag;
    wire [7:0] S_bin256;
    wire [8:0] S_bin512;
    fft_radix2 #(.LOGN(8)) X_fft256(.I_clk(I_sysclk),.I_rstn(S_rstn),
        .I_valid(S_tick),.I_sample(S_lfsr[15:0]),.O_ready(S_fftready[0]),
        .O_valid(S_fftvalid[0]),.I_ready(1'b1),.O_bin(S_bin256),
        .O_real(S_real[31:0]),.O_imag(S_imag[31:0]),.O_last(S_last[0]));
    fft_radix2 #(.LOGN(9)) X_fft512(.I_clk(I_sysclk),.I_rstn(S_rstn),
        .I_valid(S_tick),.I_sample(S_lfsr[31:16]),.O_ready(S_fftready[1]),
        .O_valid(S_fftvalid[1]),.I_ready(1'b1),.O_bin(S_bin512),
        .O_real(S_real[63:32]),.O_imag(S_imag[63:32]),.O_last(S_last[1]));
    wire S_awready,S_wready,S_bvalid,S_arready,S_rvalid,S_enable,S_irqen,S_reset,S_irq;
    wire [1:0] S_bresp,S_rresp;
    wire [31:0] S_rdata;
    // 所有地址/写数据均可变化，避免特征寄存器被常量折叠。
    axi_lite_reg X_regs(.I_clk(I_sysclk),.I_rstn(S_rstn),
        .I_awaddr({6'd0,S_lfsr[9:2],2'b0}),.I_awvalid(S_count[3]),.O_awready(S_awready),
        .I_wdata(S_lfsr),.I_wstrb(S_lfsr[3:0]),.I_wvalid(S_count[4]),.O_wready(S_wready),
        .O_bresp(S_bresp),.O_bvalid(S_bvalid),.I_bready(S_count[5]),
        .I_araddr({6'd0,S_lfsr[17:10],2'b0}),.I_arvalid(S_count[6]),.O_arready(S_arready),
        .O_rdata(S_rdata),.O_rresp(S_rresp),.O_rvalid(S_rvalid),.I_rready(S_count[7]),
        .I_busy(|S_fready),.I_overflow(|S_overrun),.I_done(|S_last),.I_state(S_count[15:8]),
        .I_sample(S_lfsr[15:0]),.I_sample_valid(S_tick),.I_temp(S_lfsr[31:16]),
        .I_acc_x(S_lfsr[15:0]),.I_acc_y(S_lfsr[31:16]),.I_acc_z(S_lfsr[23:8]),
        .I_current(S_lfsr[19:4]),.I_temp_valid(S_tick),.I_acc_valid(S_tick),
        .I_current_valid(S_tick),.I_faults(S_flags),.I_feature_valid(S_fvalid),
        .I_features(S_features),.I_feature_flags(S_flags),.O_enable(S_enable),
        .O_irq_enable(S_irqen),.O_soft_reset(S_reset),.O_irq(S_irq),
        // 转速只读窗口输入：接 LFSR 的不同切片，避免悬空与常量折叠。
        .I_speed_raw(S_lfsr),.I_speed_rpm({S_lfsr[30:0],S_lfsr[31]}),
        .I_speed_status({S_lfsr[7:0],24'd0}),
        .I_speed_count({S_lfsr[23:0],S_lfsr[31:24]}),
        .I_speed_ts({S_lfsr[15:0],S_lfsr[31:16]}),
        .I_speed_seq({S_lfsr[11:0],S_lfsr[31:12]}),
        .I_speed_err_count(~S_lfsr));
    reg [31:0] S_signature;
    always @(posedge I_sysclk) begin
        if(!S_rstn) S_signature<=0;
        else S_signature<={S_signature[30:0],S_signature[31]} ^ S_rdata ^
            S_real[31:0] ^ S_real[63:32] ^ S_imag[31:0] ^ S_imag[63:32] ^
            {1'b0,S_bin256,S_bin512,S_fready,S_fftready,S_last,S_fftvalid,S_rvalid,S_bvalid,S_irq,S_reset} ^
            {22'd0,S_awready,S_wready,S_arready,S_enable,S_irqen,S_rresp,S_bresp,1'b0};
    end
    assign O_led=S_signature[3:0];
endmodule
