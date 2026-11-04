// 256/512 点串行 radix-2 DIT FFT，实输入、复输出，统一 I_clk 时钟。
// 输入自然序写入位反转地址；每级缩放 1/2，输出自然序 X[k]/N、signed32 Q8。
// WINDOW=0 矩形，WINDOW=1 周期 Hann；输入/输出支持 valid/ready 背压。
module fft_radix2 #(
    parameter integer LOGN=8,
    parameter integer WINDOW=1
)(
    input wire I_clk, I_rstn,
    input wire I_valid,
    input wire signed [15:0] I_sample,
    output wire O_ready,
    output reg O_valid,
    input wire I_ready,
    output reg [LOGN-1:0] O_bin,
    output reg signed [31:0] O_real, O_imag,
    output wire O_last
);
    localparam integer N=1<<LOGN;
    localparam [LOGN:0] N_EXT={1'b1,{LOGN{1'b0}}};
    localparam [3:0] LAST_STAGE=LOGN[3:0];
    localparam LOAD=0, INIT=1, RA=2, RB=3, CAPTURE=4, MUL=5, ADD=6,
        SCALE=7, WA=8, WB=9, OUT_READ=10, OUT_LATCH=11, OUT_HOLD=12;
    reg [3:0] S_state;
    reg [LOGN-1:0] S_input, S_j, S_group, S_out;
    reg [3:0] S_stage;
    wire [LOGN-1:0] S_half = {{(LOGN-1){1'b0}},1'b1} << (S_stage-1'b1);
    wire [LOGN-1:0] S_a = S_group+S_j;
    wire [LOGN-1:0] S_b = S_a+S_half;
    wire [8:0] S_j9={{(9-LOGN){1'b0}},S_j};
    wire [8:0] S_tw_addr = S_j9 << (4'd9-S_stage);
    wire [LOGN:0] S_next_group={1'b0,S_group}+({1'b0,S_half}<<1);
    reg [LOGN-1:0] S_reverse;
    integer k;
    always @* begin
        for(k=0;k<LOGN;k=k+1) S_reverse[k]=S_input[LOGN-1-k];
    end
    (* ram_style = "bram" *) reg [63:0] S_mem[0:N-1];
    reg [63:0] S_read;
    reg signed [31:0] S_ar,S_ai,S_br,S_bi,S_tr,S_ti;
    reg signed [31:0] S_yr0,S_yi0,S_yr1,S_yi1;
    // 乘法结果必须保留为独立寄存器：否则 optimize_gate 会把 MUL 与 ADD 两个状态
    // 压成一条 DSP 乘法 + 宽加法进位链，在 100 MHz 下只剩约 0.004 ns 裕量。
    (* keep *) reg signed [49:0] S_rr,S_ii,S_ri,S_ir;
    reg signed [50:0] S_product_r,S_product_i;
    wire signed [17:0] S_cos,S_sin;
    wire [16:0] S_window;
    fft_coeff_rom X_rom(.I_clk(I_clk),.I_tw_addr(S_tw_addr[7:0]),
        .I_win_addr({{(9-LOGN){1'b0}},S_input}),.I_size512(LOGN==9),
        .O_cos(S_cos),.O_sin(S_sin),.O_window(S_window));
    // 输入锁存与 RAM 写入隔一拍，窗 ROM 延迟与样本对齐。
    reg signed [15:0] S_sample;
    reg [LOGN-1:0] S_loadaddr;
    reg S_pending;
    wire signed [17:0] S_coeff = WINDOW==0 ? 18'sd32768 : $signed({1'b0,S_window});
    wire signed [33:0] S_windowed = S_sample*S_coeff;
    wire signed [31:0] S_loaded = {{5{S_windowed[33]}},S_windowed[33:7]};
    wire signed [32:0] S_plus_r = {S_ar[31],S_ar}+{S_tr[31],S_tr};
    wire signed [32:0] S_plus_i = {S_ai[31],S_ai}+{S_ti[31],S_ti};
    wire signed [32:0] S_minus_r = {S_ar[31],S_ar}-{S_tr[31],S_tr};
    wire signed [32:0] S_minus_i = {S_ai[31],S_ai}-{S_ti[31],S_ti};
    assign O_ready=S_state==LOAD && !S_pending;
    assign O_last=O_valid && (&O_bin);
    wire S_we=I_rstn && ((S_state==LOAD && S_pending) || S_state==WA || S_state==WB);
    wire [LOGN-1:0] S_wa=S_state==LOAD ? S_loadaddr : (S_state==WA ? S_a : S_b);
    wire [63:0] S_wdata=S_state==LOAD ? {S_loaded,32'd0} :
        (S_state==WA ? {S_yr0,S_yi0} : {S_yr1,S_yi1});
    wire S_re=S_state==RA || S_state==RB || S_state==OUT_READ;
    wire [LOGN-1:0] S_ra=S_state==OUT_READ ? S_out : (S_state==RA ? S_a : S_b);
    always @(posedge I_clk) begin
        if(S_we) S_mem[S_wa]<=S_wdata;
        if(S_re) S_read<=S_mem[S_ra];
    end
    always @(posedge I_clk) begin
        if(!I_rstn) begin
            S_state<=LOAD; S_input<=0; S_j<=0; S_group<=0; S_out<=0; S_stage<=1;
            S_ar<=0;S_ai<=0;S_br<=0;S_bi<=0;S_tr<=0;S_ti<=0;
            S_rr<=0;S_ii<=0;S_ri<=0;S_ir<=0;S_product_r<=0;S_product_i<=0;
            S_sample<=0;S_loadaddr<=0;S_pending<=0;
            O_valid<=0;O_bin<=0;O_real<=0;O_imag<=0;
        end else begin
            case(S_state)
                LOAD: begin
                    if(I_valid && O_ready) begin
                        S_sample<=I_sample; S_loadaddr<=S_reverse; S_pending<=1;
                    end else if(S_pending) begin
                        S_pending<=0;
                        if((&S_input)) begin S_input<=0; S_state<=INIT; end
                        else S_input<=S_input+1'b1;
                    end
                end
                INIT: begin S_stage<=1; S_group<=0; S_j<=0; S_state<=RA; end
                RA: S_state<=RB;
                RB: begin {S_ar,S_ai}<=S_read; S_state<=CAPTURE; end
                CAPTURE: begin {S_br,S_bi}<=S_read; S_state<=MUL; end
                MUL: begin
                    S_rr<=S_br*S_cos; S_ii<=S_bi*S_sin;
                    S_ri<=S_br*S_sin; S_ir<=S_bi*S_cos; S_state<=ADD;
                end
                ADD: begin
                    S_product_r<={S_rr[49],S_rr}-{S_ii[49],S_ii};
                    S_product_i<={S_ri[49],S_ri}+{S_ir[49],S_ir}; S_state<=SCALE;
                end
                SCALE: begin S_tr<=S_product_r[46:15]; S_ti<=S_product_i[46:15]; S_state<=WA; end
                WA: begin
                    S_state<=WB;
                end
                WB: begin
                    if(S_j==S_half-1'b1) begin
                        S_j<=0;
                        if(S_next_group>=N_EXT) begin
                            S_group<=0;
                            if(S_stage==LAST_STAGE) begin S_out<=0; S_state<=OUT_READ; end
                            else begin S_stage<=S_stage+1'b1; S_state<=RA; end
                        end else begin S_group<=S_group+(S_half<<1); S_state<=RA; end
                    end else begin S_j<=S_j+1'b1; S_state<=RA; end
                end
                OUT_READ: S_state<=OUT_LATCH;
                OUT_LATCH: begin
                    {O_real,O_imag}<=S_read; O_bin<=S_out; O_valid<=1; S_state<=OUT_HOLD;
                end
                OUT_HOLD: if(I_ready) begin
                    O_valid<=0;
                    if((&S_out)) S_state<=LOAD;
                    else begin S_out<=S_out+1'b1; S_state<=OUT_READ; end
                end
                default: begin S_state<=LOAD; O_valid<=0; S_pending<=0; end
            endcase
        end
    end
    // 蝶形缩放为带符号向下取整，A/B 分两拍写回单写端口缓存。
    always @* begin
        S_yr0=S_plus_r[32:1]; S_yi0=S_plus_i[32:1];
        S_yr1=S_minus_r[32:1]; S_yi1=S_minus_i[32:1];
    end
endmodule
