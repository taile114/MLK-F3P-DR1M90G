// 64 bit 无符号逐对开方：32 拍输出向下取整平方根，同步低有效复位。
module unsigned_sqrt(
    input wire I_clk, I_rstn, I_start,
    input wire [63:0] I_value,
    output reg O_busy, O_done,
    output reg [31:0] O_root
);
    reg [63:0] S_value;
    reg [65:0] S_rem;
    reg [5:0] S_count;
    wire [65:0] S_shift = {S_rem[63:0],S_value[63:62]};
    wire [65:0] S_trial = {32'd0,O_root,2'b01};
    always @(posedge I_clk) begin
        if(!I_rstn) begin
            S_value<=0; S_rem<=0; S_count<=0; O_busy<=0; O_done<=0; O_root<=0;
        end else begin
            O_done<=0;
            if(I_start && !O_busy) begin
                S_value<=I_value; S_rem<=0; S_count<=0; O_root<=0; O_busy<=1;
            end else if(O_busy) begin
                S_value<={S_value[61:0],2'b0};
                if(S_shift>=S_trial) begin
                    S_rem<=S_shift-S_trial; O_root<={O_root[30:0],1'b1};
                end else begin S_rem<=S_shift; O_root<={O_root[30:0],1'b0}; end
                if(S_count==31) begin O_busy<=0; O_done<=1; end
                else S_count<=S_count+1'b1;
            end
        end
    end
endmodule
