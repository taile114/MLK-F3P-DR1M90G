// 传感器采样流 DMA 写通道：AXI4 主口只写，8 字节/拍、突发不超过 16 拍、单 outstanding。
// 数据字格式（小端）{ts[31:0], ch[15:0], sample[15:0]}；ch 0 = X、1 = Y、2 = Z、16'hFFFF = pad。
// I_test=1 时内部产生测试 ramp：数据字为 64 bit 无符号递增计数，复位后第 i 拍 = i；
// I_test=0 时消费外部 I_s_* 流；每个突发收到写响应后才发下一个突发。
// 突发缓冲：数据先写入 16 拍深度缓冲，攒满当前突发的全部拍数后才发 AWVALID。AW 一旦发出，
//         W 数据全部就绪，事务在若干周期内必然收尾，不会出现「AW 已握手、W 等数据」而把
//         PS 侧 HP 写通道长期挂住的情况；数据源断流时只是不再发起新事务，O_s_ready 保持
//         拉低，I_stop / I_abort 仍能在没有在途事务时直接干净退出。
// 单次模式（I_ring=0，默认）：I_bytes 非 8 的整数倍、超过 4096、基地址超出 4 GiB 或未按
//         8 字节对齐时只置 O_error，不发 AXI；全部字节写完置 O_done。
// 环形模式（I_ring=1）：块固定 4 KiB，块地址 = I_base + (已完成块数 mod I_blocks) × 4096；
//         每完成一块拉高一拍 O_block_done 并递增 O_blk_seq（START 清 0），写完 I_blocks 块
//         后回到块 0 继续，直到 I_abort；本模式 O_done 不置位、I_bytes 忽略，I_ring / I_blocks
//         在 START 锁存。I_blocks 超出 1~256、基地址超出 4 GiB 或未按 8 字节对齐时只置
//         O_error，不发 AXI。环形基地址需按 4 KiB 对齐，块内 128 字节突发才不跨
//         4 KiB 边界（单次模式只要求 8 字节对齐，与现状一致）。
// 写响应非 OKAY 时置 O_error 并结束；O_written 只统计已收到 OKAY 响应的字节数
//         （环形模式按 START 起累计）。
// I_abort 立即拉低 O_busy 并停止产生新突发；AWVALID 已发出的突发保持请求并用已缓冲的
// 真实数据收尾，避免撤回等待仲裁的 AW 或改变受背压的 W；收尾期间新的 I_start 被忽略。
// I_stop 为优雅停止：完成当前突发（含写响应）后释放 O_busy，不再发起新突发 / 新块，
// 不置 O_error、不置 O_done，O_blk_seq 保留。与 I_abort 的差异：I_abort 立即拉低
// O_busy、置 ERRCODE=3 并用已有缓冲收尾在途突发；I_stop 用真实样本把当前突发正常
// 收尾、错误码保持原值，回到 IDLE 后可再次 I_start。单次模式同样按「停止但不报错」
// 处理，已写字节以 O_written 为准；若收到 I_stop 时当前突发恰好是最后一个，传输
// 已完成，仍按完成置 O_done。
// 错误码（docs/PL-PS接口约定.md §4.4）：0 = 无错误，1 = 参数非法（长度 / 对齐 / 超出窗口），
//         2 = 写响应非 OKAY，3 = 中止。O_error 粘滞，仅 I_rstn 清除。
module sensor_dma(
    input wire I_clk, I_rstn,
    input wire I_start, I_abort, I_stop,
    input wire [63:0] I_base,
    input wire [31:0] I_bytes,
    input wire I_test,
    input wire I_ring,
    input wire [31:0] I_blocks,
    output wire O_busy, O_done,
    output reg [2:0] O_error,
    output reg [31:0] O_written,
    output wire O_block_done,
    output reg [31:0] O_blk_seq,
    input wire [63:0] I_s_data,
    input wire I_s_valid,
    output wire O_s_ready,
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
    output wire M_axi_bready
);
    localparam [31:0] BYTES_MAX=32'd4096;         // 单次传输 / 环形块上限
    localparam [31:0] BYTES_BURST=32'd128;        // 16 拍 × 8 字节
    localparam [31:0] RING_MAX=32'd256;           // 环形模式块数上限
    localparam [2:0] S_IDLE=3'd0, S_BURST=3'd1, S_BWAIT=3'd2, S_DONE=3'd3;
    localparam [2:0] E_PARAM=3'd1, E_BRESP=3'd2, E_ABORT=3'd3;

    reg [2:0] S_state;
    reg [31:0] S_base, S_remaining, S_burst_bytes;
    reg [4:0] S_burst_beats, S_beats_left;
    reg S_aw_done, S_stop;
    reg S_stop_req;                               // 已收到 I_stop，当前突发收尾后回 IDLE
    // 突发缓冲：移位队列，S_q[0] 为队首。队首是寄存器输出，W 数据不经过异步读阵列，
    // 不会被综合成同步读的块 RAM。
    reg [63:0] S_q[0:15];
    reg [4:0] S_count;                            // 队列内有效拍数 0~16
    integer S_i;
    reg [63:0] S_ramp;
    reg S_ring;                                   // 本次传输的环形模式（START 锁存）
    reg [31:0] S_ring_base, S_ring_n, S_ring_idx; // 环基地址、环大小、已完成块数 mod N
    reg [31:0] S_blk_written;                     // 当前块内已写字节（块内地址偏移）
    reg S_blk_done;                               // O_block_done 的单拍寄存器

    wire S_burst=S_state==S_BURST;
    // AWVALID 一经发出便有保持义务，即使共享 HP1 尚未授予 AWREADY。
    wire S_inflight=M_axi_awvalid || S_aw_done || (S_beats_left!=S_burst_beats);
    wire [31:0] S_burst_next=S_remaining-S_burst_bytes;
    wire [31:0] S_burst_len=(S_burst_next>BYTES_BURST) ? BYTES_BURST : S_burst_next;
    wire [4:0] S_beats_next=S_burst_len[7:3];
    // 下一块的 (块序号+1) mod N 与块地址；N 在 START 锁存，环形运行期间不变。
    wire [31:0] S_ring_idx_next=(S_ring_idx+32'd1>=S_ring_n) ? 32'd0 : S_ring_idx+32'd1;
    wire [31:0] S_ring_next_base=S_ring_base+(S_ring_idx_next<<12);

    // 数据源：测试模式内部 ramp，否则外部流
    wire [63:0] S_src_data=I_test ? S_ramp : I_s_data;
    wire S_src_valid=I_test ? 1'b1 : I_s_valid;
    // 攒满当前突发的全部拍数后才发 AWVALID：AW 握手时 W 数据已全部就绪，
    // 事务不会因为等数据而长期占用 PS 侧 HP 写通道。
    wire S_buf_room=S_burst && !S_stop && (S_count<S_burst_beats);
    wire S_src_take=S_buf_room && S_src_valid;
    wire S_buf_ready=(S_count>=S_burst_beats);
    wire S_wvalid=S_burst && S_aw_done && S_beats_left!=0 && S_count!=0;
    wire [63:0] S_wdata=S_q[0];
    wire S_w_hs=S_wvalid && M_axi_wready;
    // 入队下标：同拍出队时队列整体前移一格，新样本落在队尾。
    wire [3:0] S_push_idx=S_w_hs ? (S_count[3:0]-4'd1) : S_count[3:0];
    wire [4:0] S_count_next=S_count+(S_src_take?5'd1:5'd0)-(S_w_hs?5'd1:5'd0);

    assign O_s_ready=S_buf_room;
    assign M_axi_awvalid=S_burst && !S_aw_done && S_buf_ready;
    // 地址 = 块基地址 + 块内偏移：单次模式下块内偏移与 O_written 恒等，行为不变。
    assign M_axi_awaddr={1'b0,S_base+S_blk_written};   // bit32 恒 0，地址按 32 bit 回绕
    assign M_axi_awlen={3'd0,S_burst_beats}-8'd1;
    assign M_axi_awsize=3'b011;
    assign M_axi_awburst=2'b01;
    assign M_axi_wvalid=S_wvalid;
    assign M_axi_wdata=S_wdata;
    assign M_axi_wstrb=8'hff;
    assign M_axi_wlast=S_wvalid && S_beats_left==5'd1;
    assign M_axi_bready=S_state==S_BWAIT;
    assign O_busy=S_state!=S_IDLE && !S_stop;
    assign O_done=I_rstn && S_state==S_DONE;
    assign O_block_done=S_blk_done;

    always @(posedge I_clk) begin
        if (!I_rstn) begin
            S_state<=S_IDLE; S_base<=0; S_remaining<=0; S_burst_bytes<=0; O_written<=0;
            S_burst_beats<=0; S_beats_left<=0; S_aw_done<=0; S_stop<=0; S_stop_req<=0;
            S_count<=0; S_ramp<=0; O_error<=0;
            for (S_i=0;S_i<16;S_i=S_i+1) S_q[S_i]<=64'd0;
            S_ring<=0; S_ring_base<=0; S_ring_n<=0; S_ring_idx<=0;
            S_blk_written<=0; S_blk_done<=0; O_blk_seq<=0;
        end else begin
            S_blk_done<=0;    // O_block_done 只在块完成那一拍置位
            // 采样先入突发缓冲（移位队列），W 数据取自队首寄存器，保证 WVALID 拉高后 WDATA 稳定
            if (S_src_take) begin
                if (I_test) S_ramp<=S_ramp+64'd1;
            end
            if (S_w_hs) begin
                for (S_i=0;S_i<15;S_i=S_i+1) S_q[S_i]<=S_q[S_i+1];
                S_beats_left<=S_beats_left-1'b1;
            end
            if (S_src_take) S_q[S_push_idx]<=S_src_data;
            S_count<=S_count_next;
            if (M_axi_awvalid && M_axi_awready) S_aw_done<=1;
            case (S_state)
            S_IDLE: begin
                if (I_start) begin
                    S_aw_done<=0; S_stop<=0; S_stop_req<=0;
                    S_count<=0;
                    O_written<=0; S_blk_written<=0; O_blk_seq<=0;
                    S_base<=I_base[31:0];
                    S_ring<=I_ring; S_ring_base<=I_base[31:0];
                    S_ring_n<=I_blocks; S_ring_idx<=0;
                    if (I_base[63:32]!=0 || I_base[2:0]!=0) O_error<=E_PARAM;
                    else if (I_ring) begin
                        // 环形：块固定 4 KiB，块数 1~256，I_bytes 忽略。
                        if (I_blocks==0 || I_blocks>RING_MAX) O_error<=E_PARAM;
                        else begin
                            S_remaining<=BYTES_MAX;
                            S_burst_bytes<=BYTES_BURST;
                            S_burst_beats<=5'd16;
                            S_beats_left<=5'd16;
                            S_state<=S_BURST;
                        end
                    end else if (I_bytes[2:0]!=0 || I_bytes>BYTES_MAX) O_error<=E_PARAM;
                    else if (I_bytes==0) S_state<=S_DONE;
                    else begin
                        S_remaining<=I_bytes;
                        S_burst_bytes<=(I_bytes>BYTES_BURST) ? BYTES_BURST : I_bytes;
                        S_burst_beats<=(I_bytes>BYTES_BURST) ? 5'd16 : I_bytes[7:3];
                        S_beats_left<=(I_bytes>BYTES_BURST) ? 5'd16 : I_bytes[7:3];
                        S_state<=S_BURST;
                    end
                end
            end
            S_BURST: begin
                // I_stop 在突发进行中锁存，当前突发收尾后生效。
                if (I_stop) S_stop_req<=1;
                if (I_abort && !S_inflight) begin
                    S_state<=S_IDLE; S_aw_done<=0; S_stop_req<=0; O_error<=E_ABORT;
                end else begin
                    if (I_abort) begin S_stop<=1; O_error<=E_ABORT; end
                    if (S_aw_done && S_beats_left==0) S_state<=S_BWAIT;
                    // 尚未发 AW（正在攒数据）时收到 STOP：没有在途事务，直接干净退出。
                    else if (S_stop_req && !S_inflight) begin
                        S_state<=S_IDLE; S_stop_req<=0;
                    end
                end
            end
            S_BWAIT: begin
                if (I_stop) S_stop_req<=1;
                if (I_abort) begin S_stop<=1; O_error<=E_ABORT; end
                if (M_axi_bvalid) begin
                    S_aw_done<=0;
                    if (S_stop || I_abort) begin
                        S_state<=S_IDLE; S_stop<=0; S_stop_req<=0;
                    end else if (M_axi_bresp!=2'b00) begin
                        O_error<=E_BRESP; S_state<=S_IDLE; S_stop_req<=0;
                    end else begin
                        O_written<=O_written+S_burst_bytes;
                        if (S_remaining==S_burst_bytes && S_ring) begin
                            // 块完成：块计数 +1、块地址回绕；未收到 I_stop 时 BUSY 不释放，
                            // 继续下一块。I_stop 已锁存则在块边界收尾回 IDLE。
                            S_blk_done<=1;
                            O_blk_seq<=O_blk_seq+32'd1;
                            S_ring_idx<=S_ring_idx_next;
                            S_base<=S_ring_next_base;
                            S_blk_written<=0;
                            S_remaining<=BYTES_MAX;
                            S_burst_bytes<=BYTES_BURST;
                            S_burst_beats<=5'd16;
                            S_beats_left<=5'd16;
                            if (S_stop_req) begin
                                // 优雅停止：块已计完，不置 DONE / ERROR，释放 BUSY 回 IDLE。
                                S_state<=S_IDLE; S_stop_req<=0;
                            end else S_state<=S_BURST;
                        end else begin
                            S_blk_written<=S_blk_written+S_burst_bytes;
                            if (S_remaining==S_burst_bytes) begin
                                // 传输已完成：I_stop 不改变完成语义，仍按完成置 DONE。
                                S_state<=S_DONE;
                            end else if (S_stop_req) begin
                                // 突发边界停止：不置 DONE / ERROR，O_written 为已完成字节。
                                S_state<=S_IDLE; S_stop_req<=0;
                            end else begin
                                S_remaining<=S_burst_next;
                                S_burst_bytes<=S_burst_len;
                                S_burst_beats<=S_beats_next;
                                S_beats_left<=S_beats_next;
                                S_state<=S_BURST;
                            end
                        end
                    end
                end
            end
            S_DONE: begin
                S_state<=S_IDLE; S_aw_done<=0; S_stop<=0; S_stop_req<=0;
            end
            default: S_state<=S_IDLE;
            endcase
        end
    end
endmodule
