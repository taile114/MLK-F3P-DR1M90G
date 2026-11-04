// 无符号恢复除法器：逐位除法、逐 16 bit 借位，限制长进位路径。
// start 仅在 !busy 时接受；除零返回 0 并置 O_div_zero；同步低有效复位。
module unsigned_divider #(
    parameter integer WIDTH = 160,
    parameter integer WORD = 16
)(
    input wire I_clk, I_rstn, I_start,
    input wire [WIDTH-1:0] I_numerator, I_denominator,
    output wire O_busy,
    output reg O_done, O_div_zero,
    output reg [WIDTH-1:0] O_quotient
);
    localparam integer PARTS = WIDTH / WORD;
    localparam IDLE=0, SHIFT=1, SUB=2, COMMIT=3;
    reg [2:0] S_state;
    reg [WIDTH:0] S_rem;
    reg [WIDTH-1:0] S_num, S_den, S_trial;
    reg S_borrow;
    integer S_bit, S_part;
    wire [WORD:0] S_sub = {1'b0,S_rem[S_part*WORD +: WORD]} -
                          {1'b0,S_den[S_part*WORD +: WORD]} - S_borrow;
    assign O_busy = S_state != IDLE;
    always @(posedge I_clk) begin
        if (!I_rstn) begin
            S_state<=IDLE; S_rem<=0; S_num<=0; S_den<=0; S_trial<=0;
            S_borrow<=0; S_bit<=0; S_part<=0;
            O_done<=0; O_div_zero<=0; O_quotient<=0;
        end else begin
            O_done<=0;
            case(S_state)
                IDLE: if(I_start) begin
                    O_div_zero<=I_denominator==0; O_quotient<=0;
                    S_num<=I_numerator; S_den<=I_denominator; S_rem<=0;
                    S_bit<=WIDTH-1;
                    if(I_denominator==0) O_done<=1;
                    else S_state<=SHIFT;
                end
                SHIFT: begin
                    S_rem<={S_rem[WIDTH-1:0],S_num[WIDTH-1]};
                    S_num<={S_num[WIDTH-2:0],1'b0};
                    S_part<=0; S_borrow<=0; S_state<=SUB;
                end
                SUB: begin
                    S_trial[S_part*WORD +: WORD]<=S_sub[WORD-1:0];
                    S_borrow<=S_sub[WORD];
                    if(S_part==PARTS-1) S_state<=COMMIT;
                    else S_part<=S_part+1;
                end
                COMMIT: begin
                    if(S_rem[WIDTH] || !S_borrow) begin
                        S_rem<={1'b0,S_trial}; O_quotient[S_bit]<=1;
                    end
                    if(S_bit==0) begin O_done<=1; S_state<=IDLE; end
                    else begin S_bit<=S_bit-1; S_state<=SHIFT; end
                end
                default: S_state<=IDLE;
            endcase
        end
    end
endmodule
