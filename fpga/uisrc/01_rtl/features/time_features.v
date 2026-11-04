// 精确滑窗时域特征：signed16 输入，N=2^WINDOW_LOG2，步长 HOP。
// 环形缓存与滚动一阶/二阶和；中心矩以 d=N*x-sum(x) 精确计算。
// 输出七个 64 bit 字段：DC / RMS / 方差 / 峰峰值 / 峰值因子 / 过零率 / 峭度。
// 除峰峰值为整数外均 Q16；低位字段在前。ready=0 时 valid 被丢弃并报单拍 overrun。
module time_features #(
    parameter integer WINDOW_LOG2 = 8,
    parameter integer HOP = 128
)(
    input wire I_clk, I_rstn,
    input wire I_valid,
    input wire signed [15:0] I_sample,
    output wire O_ready,
    output reg O_valid, O_overrun,
    output reg [1:0] O_flags,
    output reg [447:0] O_features
);
    localparam integer N=1<<WINDOW_LOG2;
    localparam [159:0] ZC_DEN={{(160-WINDOW_LOG2){1'b0}},{WINDOW_LOG2{1'b1}}};
    localparam ACCEPT=0, UPDATE=1, PREP=2, READ=3, DIFF=4, SQUARE=5,
        FOURTH=6, ACCUM=7, DENOM=8, ROOT_START=9, ROOT_WAIT=10,
        DIV_START=11, DIV_WAIT=12, PUBLISH=13;
    reg [3:0] S_state;
    (* ram_style = "bram" *) reg signed [15:0] S_mem [0:N-1];
    reg [WINDOW_LOG2-1:0] S_wr, S_scan;
    integer S_fill, S_hop, S_index;
    reg signed [15:0] S_new, S_read, S_min, S_max;
    reg signed [31:0] S_sum;
    reg [47:0] S_sum2;
    wire signed [31:0] S_new2 = S_new*S_new;
    wire signed [31:0] S_old2 = S_read*S_read;
    reg signed [31:0] S_delta;
    reg [63:0] S_d2, S_m2;
    reg [127:0] S_d4, S_m4, S_den;
    reg S_sign;
    reg [15:0] S_zc, S_peak;
    wire [15:0] S_abs = S_read[15] ? (~S_read+16'd1) : S_read;
    reg [447:0] S_result;
    reg [1:0] S_flags;
    reg [1:0] S_job;
    reg [159:0] S_num, S_divden;
    wire [159:0] S_quot;
    wire [127:0] S_variance = ({64'd0,S_m2} << 16) >> (3*WINDOW_LOG2);
    wire signed [16:0] S_range=$signed({S_max[15],S_max})-$signed({S_min[15],S_min});
    wire S_divdone, S_rootdone;
    wire [31:0] S_root;
    wire [63:0] S_radicand = {16'd0,S_sum2} << (32-WINDOW_LOG2);
    unsigned_divider X_div(.I_clk(I_clk),.I_rstn(I_rstn),.I_start(S_state==DIV_START),
        .I_numerator(S_num),.I_denominator(S_divden),.O_busy(),.O_done(S_divdone),
        .O_div_zero(),.O_quotient(S_quot));
    unsigned_sqrt X_sqrt(.I_clk(I_clk),.I_rstn(I_rstn),.I_start(S_state==ROOT_START),
        .I_value(S_radicand),.O_busy(),.O_done(S_rootdone),.O_root(S_root));
    assign O_ready=S_state==ACCEPT;
    // RAM 无复位；填满 N 点前不读取未写入数据，支持 ERAM 推断。
    // 接受拍读旧样本，下一拍再写新样本；避免 ERAM 同地址读写的不确定值。
    wire S_we=I_rstn && S_state==UPDATE;
    wire S_re=(O_ready && I_valid) || S_state==READ;
    wire [WINDOW_LOG2-1:0] S_ra=S_state==READ ? S_scan : S_wr;
    always @(posedge I_clk) begin
        if(S_we) S_mem[S_wr]<=S_new;
        if(S_re) S_read<=S_mem[S_ra];
    end
    always @(posedge I_clk) begin
        if(!I_rstn) begin
            S_state<=ACCEPT; S_wr<=0; S_scan<=0; S_fill<=0; S_hop<=0; S_index<=0;
            S_new<=0; S_min<=0; S_max<=0; S_sum<=0; S_sum2<=0;
            S_delta<=0; S_d2<=0; S_d4<=0; S_m2<=0; S_m4<=0; S_den<=0;
            S_sign<=0; S_zc<=0; S_peak<=0; S_result<=0; S_flags<=0;
            S_job<=0; S_num<=0; S_divden<=0;
            O_valid<=0; O_overrun<=0; O_flags<=0; O_features<=0;
        end else begin
            O_valid<=0; O_overrun<=I_valid && !O_ready;
            case(S_state)
                ACCEPT: if(I_valid) begin S_new<=I_sample; S_state<=UPDATE; end
                UPDATE: begin
                    S_sum<=S_sum+$signed({{16{S_new[15]}},S_new})-(S_fill==N ? $signed({{16{S_read[15]}},S_read}) : 32'sd0);
                    S_sum2<=S_sum2+{16'd0,S_new2}-(S_fill==N ? {16'd0,S_old2} : 48'd0);
                    S_wr<=S_wr+1'b1;
                    if(S_fill<N) S_fill<=S_fill+1;
                    if(S_fill==N-1 || (S_fill==N && S_hop==HOP-1)) begin
                        S_hop<=0; S_state<=PREP;
                    end else begin
                        if(S_fill==N) S_hop<=S_hop+1;
                        S_state<=ACCEPT;
                    end
                end
                PREP: begin
                    S_result<=0;
                    S_result[63:0]<=($signed({{32{S_sum[31]}},S_sum}) <<< 16) >>> WINDOW_LOG2;
                    S_scan<=S_wr; S_index<=0; S_m2<=0; S_m4<=0; S_zc<=0; S_peak<=0;
                    S_flags<=0; S_state<=READ;
                end
                READ: S_state<=DIFF;
                DIFF: begin
                    S_delta<=($signed({{16{S_read[15]}},S_read}) <<< WINDOW_LOG2)-S_sum;
                    if(S_index==0 || S_read<S_min) S_min<=S_read;
                    if(S_index==0 || S_read>S_max) S_max<=S_read;
                    if(S_abs>S_peak) S_peak<=S_abs;
                    if(S_index!=0 && S_read[15]!=S_sign) S_zc<=S_zc+1'b1;
                    S_sign<=S_read[15]; S_state<=SQUARE;
                end
                SQUARE: begin S_d2<=S_delta*S_delta; S_state<=FOURTH; end
                FOURTH: begin S_d4<=S_d2*S_d2; S_state<=ACCUM; end
                ACCUM: begin
                    S_m2<=S_m2+S_d2; S_m4<=S_m4+S_d4;
                    S_scan<=S_scan+1'b1;
                    if(S_index==N-1) S_state<=DENOM;
                    else begin S_index<=S_index+1; S_state<=READ; end
                end
                DENOM: begin
                    S_den<=S_m2*S_m2;
                    S_result[191:128]<=S_variance[63:0];
                    S_result[255:192]<={47'd0,S_range};
                    S_flags[1]<=S_m2==0; S_state<=ROOT_START;
                end
                ROOT_START: S_state<=ROOT_WAIT;
                ROOT_WAIT: if(S_rootdone) begin
                    S_result[127:64]<={32'd0,S_root}; S_flags[0]<=S_root==0;
                    S_num<={112'd0,S_peak,32'd0}; S_divden<={128'd0,S_root};
                    S_job<=0; S_state<=DIV_START;
                end
                DIV_START: S_state<=DIV_WAIT;
                DIV_WAIT: if(S_divdone) begin
                    case(S_job)
                        0: begin
                            S_result[319:256]<=S_quot[63:0];
                            S_num<={128'd0,S_zc,16'd0}; S_divden<=ZC_DEN;
                            S_job<=1; S_state<=DIV_START;
                        end
                        1: begin
                            S_result[383:320]<=S_quot[63:0];
                            S_num<={32'd0,S_m4} << (16+WINDOW_LOG2); S_divden<={32'd0,S_den};
                            S_job<=2; S_state<=DIV_START;
                        end
                        default: begin S_result[447:384]<=S_quot[63:0]; S_state<=PUBLISH; end
                    endcase
                end
                PUBLISH: begin
                    O_features<=S_result; O_flags<=S_flags; O_valid<=1; S_state<=ACCEPT;
                end
                default: S_state<=ACCEPT;
            endcase
        end
    end
endmodule
