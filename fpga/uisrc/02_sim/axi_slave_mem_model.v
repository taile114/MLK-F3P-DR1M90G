`timescale 1ns/1ps
// AXI4 写通道从机内存模型：64 位数据、INCR 突发、默认 OKAY 响应。
// 按接收顺序记录每拍数据 / 地址 / WSTRB、每个突发的首地址与 AWLEN，以及 AXI 协议违例计数，
// 供自检测 TB 核对：
//   - 每拍 8 字节（AWSIZE=3）、INCR 突发、wlast 位于突发末拍、突发不跨 4 KB 边界；
//   - 写入数据、突发划分、地址连续性，以及 O_written 与实际字节数的一致性。
// 非 AXI 控制口：
//   - I_clear 在总线空闲时清零记录与计数；
//   - I_w_stall 拉低 WREADY，制造写通道反压；
//   - I_bresp 为 B 通道注入的响应（2'b00 OKAY / 2'b10 SLVERR / 2'b11 DECERR）。
// 只实现写通道：被测 DMA 为 AXI4 只写主口，不需要读通道。
module axi_slave_mem_model #(
    parameter integer MEM_WORDS  = 1024,   // 记录容量（64 位字），4 KB = 512 拍
    parameter integer MAX_BURSTS = 512     // 突发记录容量（最坏按每拍一个突发也够 4 KB）
) (
    input  wire        I_clk,
    input  wire        I_rstn,
    input  wire        I_clear,
    input  wire        I_w_stall,
    input  wire [1:0]  I_bresp,

    input  wire [32:0] I_awaddr,
    input  wire [7:0]  I_awlen,
    input  wire [2:0]  I_awsize,
    input  wire [1:0]  I_awburst,
    input  wire        I_awvalid,
    output wire        O_awready,

    input  wire [63:0] I_wdata,
    input  wire [7:0]  I_wstrb,
    input  wire        I_wlast,
    input  wire        I_wvalid,
    output wire        O_wready,

    output wire [1:0]  O_bresp,
    output wire        O_bvalid,
    input  wire        I_bready
);
    localparam [1:0] S_IDLE = 2'd0;
    localparam [1:0] S_DATA = 2'd1;
    localparam [1:0] S_RESP = 2'd2;

    reg [1:0]  S_state;
    reg [32:0] S_burst_addr;
    reg [7:0]  S_burst_len;     // AWLEN：拍数 - 1
    reg [7:0]  S_beat;          // 突发内已接收拍号
    reg [1:0]  S_bresp_r;
    reg        S_bvalid_r;

    // 记录与计数（TB 直接层次引用核对）。
    reg [63:0] S_wdata [0:MEM_WORDS-1];
    reg [32:0] S_waddr [0:MEM_WORDS-1];
    reg [7:0]  S_wstrb [0:MEM_WORDS-1];
    reg [32:0] S_burst_addr_log [0:MAX_BURSTS-1];
    reg [7:0]  S_burst_len_log  [0:MAX_BURSTS-1];
    integer    S_aw_count;      // AW 握手数
    integer    S_beat_count;    // W 握手数
    integer    S_burst_count;   // 完成（收到 wlast）的突发数
    integer    S_byte_count;    // 按 WSTRB 统计的写入字节数
    integer    S_error_count;   // AXI 协议违例数

    integer    S_init_i;

    // 突发内第 S_beat 拍的地址；AWLEN 最大 255，按 8 字节拍宽计算总字节数。
    wire [32:0] S_beat_addr    = S_burst_addr + (S_beat * 8);
    wire [12:0] S_burst_bytes  = ({5'd0, I_awlen} + 13'd1) * 13'd8;
    wire [12:0] S_end_offset   = {20'd0, I_awaddr[11:0]} + S_burst_bytes;

    assign O_awready = (S_state == S_IDLE) && !I_clear;
    assign O_wready  = (S_state == S_DATA) && !I_w_stall && !I_clear;
    assign O_bvalid  = S_bvalid_r;
    assign O_bresp   = S_bresp_r;

    always @(posedge I_clk or negedge I_rstn) begin
        if (!I_rstn) begin
            S_state      <= S_IDLE;
            S_burst_addr <= 33'd0;
            S_burst_len  <= 8'd0;
            S_beat       <= 8'd0;
            S_bresp_r    <= 2'b00;
            S_bvalid_r   <= 1'b0;
        end else if (I_clear) begin
            S_state        <= S_IDLE;
            S_burst_addr   <= 33'd0;
            S_burst_len    <= 8'd0;
            S_beat         <= 8'd0;
            S_bresp_r      <= 2'b00;
            S_bvalid_r     <= 1'b0;
            S_aw_count     <= 0;
            S_beat_count   <= 0;
            S_burst_count  <= 0;
            S_byte_count   <= 0;
            S_error_count  <= 0;
        end else begin
            case (S_state)
                S_IDLE: begin
                    if (I_awvalid) begin
                        S_burst_addr <= I_awaddr;
                        S_burst_len  <= I_awlen;
                        S_beat       <= 8'd0;
                        S_state      <= S_DATA;
                        S_aw_count   <= S_aw_count + 1;
                        if (I_awsize != 3'd3)
                            S_error_count <= S_error_count + 1;
                        if (I_awburst != 2'b01)
                            S_error_count <= S_error_count + 1;
                        if (S_end_offset > 13'd4096)
                            S_error_count <= S_error_count + 1;
                    end
                end
                S_DATA: begin
                    if (I_wvalid && !I_w_stall) begin
                        // 握手条件含 WREADY（I_w_stall 拉低时不消费该拍）。
                        // wlast 必须精确落在 AWLEN 对应的末拍。
                        if (I_wlast != (S_beat == S_burst_len))
                            S_error_count <= S_error_count + 1;
                        if (S_beat_count < MEM_WORDS) begin
                            S_wdata[S_beat_count] <= I_wdata;
                            S_waddr[S_beat_count] <= S_beat_addr;
                            S_wstrb[S_beat_count] <= I_wstrb;
                        end else begin
                            S_error_count <= S_error_count + 1;
                        end
                        S_byte_count <= S_byte_count + I_wstrb[0] + I_wstrb[1] +
                                        I_wstrb[2] + I_wstrb[3] + I_wstrb[4] +
                                        I_wstrb[5] + I_wstrb[6] + I_wstrb[7];
                        S_beat_count <= S_beat_count + 1;
                        if (I_wlast) begin
                            if (S_burst_count < MAX_BURSTS) begin
                                S_burst_addr_log[S_burst_count] <= S_burst_addr;
                                S_burst_len_log[S_burst_count]  <= S_burst_len;
                            end
                            S_burst_count <= S_burst_count + 1;
                            S_bresp_r     <= I_bresp;
                            S_bvalid_r    <= 1'b1;
                            S_state       <= S_RESP;
                        end else begin
                            S_beat <= S_beat + 1;
                        end
                    end
                end
                S_RESP: begin
                    if (S_bvalid_r && I_bready) begin
                        S_bvalid_r <= 1'b0;
                        S_state    <= S_IDLE;
                    end
                end
                default: S_state <= S_IDLE;
            endcase
        end
    end

    // 仿真初值：避免未复位前读记录得到 X。
    initial begin
        S_state       = S_IDLE;
        S_burst_addr  = 33'd0;
        S_burst_len   = 8'd0;
        S_beat        = 8'd0;
        S_bresp_r     = 2'b00;
        S_bvalid_r    = 1'b0;
        S_aw_count    = 0;
        S_beat_count  = 0;
        S_burst_count = 0;
        S_byte_count  = 0;
        S_error_count = 0;
        for (S_init_i = 0; S_init_i < MEM_WORDS; S_init_i = S_init_i + 1) begin
            S_wdata[S_init_i] = 64'd0;
            S_waddr[S_init_i] = 33'd0;
            S_wstrb[S_init_i] = 8'd0;
        end
        for (S_init_i = 0; S_init_i < MAX_BURSTS; S_init_i = S_init_i + 1) begin
            S_burst_addr_log[S_init_i] = 33'd0;
            S_burst_len_log[S_init_i]  = 8'd0;
        end
    end
endmodule
