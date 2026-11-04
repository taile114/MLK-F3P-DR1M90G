// Nonoverlapping X-axis windows; signed mean removal precedes the unchanged core.
// Input is never backpressured: overrun is explicit and does not affect raw/time paths.
module vibration_fft #(
    parameter integer LOGN=8,
    parameter integer WINDOW=1
)(
    input logic I_clk, I_rstn, I_clear, I_enable, I_valid,
    input logic signed [15:0] I_sample,
    input logic I_ready,
    output logic O_valid,
    output wire [LOGN-1:0] O_bin,
    output logic [63:0] O_power,
    output logic O_result_valid,
    output logic [31:0] O_sequence, O_peak_bin, O_flags,
    output logic [63:0] O_peak_power
);
    localparam integer N=1<<LOGN;
    typedef enum logic [3:0] {COLLECT=1, MEAN=2, READ=4, FEED=8} state_t;
    state_t S_state, S_next;
    logic signed [15:0] S_mem[0:N-1], S_read, S_mean;
    logic signed [LOGN+15:0] S_sum;
    logic [LOGN-1:0] S_index;
    logic S_clipped, S_lost;
    logic S_input_ready, S_core_valid, S_last;
    wire signed [16:0] S_centered={S_read[15],S_read}-{S_mean[15],S_mean};
    wire signed [15:0] S_input = S_centered>17'sd32767 ? 16'sh7fff :
        (S_centered< -17'sd32768 ? 16'sh8000 : S_centered[15:0]);
    wire signed [31:0] S_real, S_imag;
    wire S_take=O_valid && I_ready;
    wire signed [63:0] S_real2=S_real*S_real, S_imag2=S_imag*S_imag;
    logic [63:0] S_peak;
    logic [31:0] S_peak_bin;
    assign O_valid=S_core_valid;
    assign O_power=$unsigned(S_real2)+$unsigned(S_imag2);
    fft_radix2 #(.LOGN(LOGN),.WINDOW(WINDOW)) X_fft(
        .I_clk(I_clk),.I_rstn(I_rstn && !I_clear),
        .I_valid(S_state==FEED),.I_sample(S_input),.O_ready(S_input_ready),
        .O_valid(S_core_valid),.I_ready(I_ready),.O_bin(O_bin),
        .O_real(S_real),.O_imag(S_imag),.O_last(S_last));
    always_comb begin
        S_next=S_state;
        case (S_state)
            COLLECT: if(I_valid && I_enable && (&S_index)) S_next=MEAN;
            MEAN: S_next=READ;
            READ: S_next=FEED;
            FEED: if (S_input_ready) begin
                if (&S_index) S_next=COLLECT;
                else S_next=READ;
            end
            default: S_next=COLLECT;
        endcase
    end
    // A full input window is collected while the previous FFT completes.
    // A next full window cannot be fed until the core accepts it.
    always_ff @(posedge I_clk) begin
        if (!I_rstn || I_clear) begin
            S_state<=COLLECT; S_index<=0; S_sum<=0; S_mean<=0; S_read<=0;
            S_clipped<=0; S_lost<=0;
            S_peak<=0; S_peak_bin<=0;
            O_sequence<=0; O_peak_bin<=0; O_peak_power<=0; O_flags<=0; O_result_valid<=0;
        end else begin
            S_state<=S_next;
            O_result_valid<=0;
            if (I_valid && I_enable && S_state!=COLLECT) S_lost<=1;
            case (S_state)
                COLLECT: if (I_valid && I_enable) begin
                    S_mem[S_index]<=I_sample;
                    S_sum<=S_sum+{{LOGN{I_sample[15]}},I_sample};
                    S_index<=S_index+1'b1;
                end
                MEAN: begin S_mean<=S_sum[LOGN +:16]; S_sum<=0; end
                READ: S_read<=S_mem[S_index];
                FEED: if (S_input_ready) begin
                    if (S_centered>17'sd32767 || S_centered< -17'sd32768) S_clipped<=1;
                    S_index<=S_index+1'b1;
                end
                default: begin end
            endcase
            if (S_take) begin
                if (O_bin==0) begin S_peak<=0; S_peak_bin<=0; end
                else if (O_bin<={1'b1,{(LOGN-1){1'b0}}} && O_power>S_peak) begin
                    S_peak<=O_power; S_peak_bin<={{(32-LOGN){1'b0}},O_bin};
                end
                if (S_last) begin
                    O_sequence<=O_sequence+1'b1;
                    O_peak_bin<=S_peak_bin; O_peak_power<=S_peak;
                    O_flags<={30'd0,S_lost,S_clipped};
                    O_result_valid<=1;
                    S_clipped<=0;
                end
            end
        end
    end
endmodule
