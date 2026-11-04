// 完成事件中断：pending 为 W1C，输出单个 I_clk 脉冲；pending 未清前合并事件。
// 置位优先于清除；屏蔽期间保留 pending，首次解除屏蔽时补发一次脉冲。
module p2f_irq(
    input wire I_clk, I_rstn, I_clear, I_event, I_enable,
    output reg O_pending, O_pulse
);
    reg S_sent;
    wire S_pending=(O_pending && !I_clear) || I_event;
    wire S_sent_next=S_sent && !I_clear;
    always @(posedge I_clk) begin
        if(!I_rstn) begin O_pending<=0; O_pulse<=0; S_sent<=0; end
        else begin
            O_pending<=S_pending;
            O_pulse<=I_enable && S_pending && !S_sent_next;
            S_sent<=S_sent_next || (I_enable && S_pending);
        end
    end
endmodule
