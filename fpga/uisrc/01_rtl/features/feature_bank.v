// 四路特征保留与原子快照：latest 每次 valid 更新；snapshot 仅在 I_snapshot 更新。
// 通道 0/1/2/3 对应振动 X/Y/Z/电流；字段低位为 DC，见接口契约 §4.3。
module feature_bank(
    input wire I_clk, I_rstn, I_clear, I_snapshot,
    input wire [3:0] I_valid,
    input wire [1791:0] I_features,
    input wire [7:0] I_flags,
    input wire [7:0] I_word_addr,
    output reg [31:0] O_data,
    output reg [3:0] O_available
);
    reg [447:0] S_latest[0:3], S_snapshot[0:3];
    reg [31:0] S_seq[0:3], S_snapseq[0:3];
    reg [1:0] S_flags[0:3], S_snapflags[0:3];
    integer c;
    always @(posedge I_clk) begin
        if(!I_rstn || I_clear) begin
            O_available<=0;
            for(c=0;c<4;c=c+1) begin
                S_latest[c]<=0; S_snapshot[c]<=0; S_seq[c]<=0; S_snapseq[c]<=0;
                S_flags[c]<=0; S_snapflags[c]<=0;
            end
        end else begin
            for(c=0;c<4;c=c+1) begin
                if(I_valid[c]) begin
                    S_latest[c]<=I_features[c*448 +: 448]; S_seq[c]<=S_seq[c]+1'b1;
                    S_flags[c]<=I_flags[c*2 +: 2]; O_available[c]<=1;
                end
                if(I_snapshot) begin
                    // 同拍 valid 纳入本次快照，序号与特征同步。
                    S_snapshot[c]<=I_valid[c] ? I_features[c*448 +: 448] : S_latest[c];
                    S_snapseq[c]<=I_valid[c] ? S_seq[c]+1'b1 : S_seq[c];
                    S_snapflags[c]<=I_valid[c] ? I_flags[c*2 +: 2] : S_flags[c];
                end
            end
        end
    end
    always @* begin
        O_data=0;
        if(I_word_addr[7:6]==2'b01) begin
            case(I_word_addr[3:0])
                4'he: O_data=S_snapseq[I_word_addr[5:4]];
                4'hf: O_data={30'd0,S_snapflags[I_word_addr[5:4]]};
                default: O_data=S_snapshot[I_word_addr[5:4]][I_word_addr[3:0]*32 +: 32];
            endcase
        end
    end
endmodule
