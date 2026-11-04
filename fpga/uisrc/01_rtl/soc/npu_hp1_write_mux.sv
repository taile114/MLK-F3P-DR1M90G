`timescale 1ns/1ps
`default_nettype none

// HP1 has six ID bits. Serialize complete writes rather than stealing an ID
// bit from either master: s[0] is sensor DMA, s[1] is the vendor soft NPU.
// Read traffic uses the independent HP1 read channels directly.
module npu_hp1_write_mux (
    input  wire               clk,
    input  wire               rst_n,
    input  wire  [1:0][5:0]    s_awid,
    input  wire  [1:0][32:0]   s_awaddr,
    input  wire  [1:0][3:0]    s_awlen,
    input  wire  [1:0][1:0]    s_awsize,
    input  wire  [1:0][1:0]    s_awburst,
    input  wire  [1:0]         s_awlock,
    input  wire  [1:0][3:0]    s_awcache,
    input  wire  [1:0][2:0]    s_awprot,
    input  wire  [1:0]         s_awvalid,
    output logic [1:0]         s_awready,
    input  wire  [1:0][63:0]   s_wdata,
    input  wire  [1:0][7:0]    s_wstrb,
    input  wire  [1:0]         s_wlast,
    input  wire  [1:0]         s_wvalid,
    output logic [1:0]         s_wready,
    output logic [1:0][5:0]    s_bid,
    output logic [1:0][1:0]    s_bresp,
    output logic [1:0]         s_bvalid,
    input  wire  [1:0]         s_bready,
    output logic [5:0]         m_awid,
    output logic [32:0]        m_awaddr,
    output logic [3:0]         m_awlen,
    output logic [1:0]         m_awsize,
    output logic [1:0]         m_awburst,
    output logic               m_awlock,
    output logic [3:0]         m_awcache,
    output logic [2:0]         m_awprot,
    output logic               m_awvalid,
    input  wire                m_awready,
    output logic [63:0]        m_wdata,
    output logic [7:0]         m_wstrb,
    output logic               m_wlast,
    output logic               m_wvalid,
    input  wire                m_wready,
    input  wire  [5:0]         m_bid,
    input  wire  [1:0]         m_bresp,
    input  wire                m_bvalid,
    output logic               m_bready
);
    typedef enum logic [1:0] {IDLE, ADDR, DATA, RESP} state_t;
    state_t state;
    logic owner;
    logic prefer_one;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            state      <= IDLE;
            owner      <= 1'b0;
            prefer_one <= 1'b0;
        end else begin
            case (state)
                IDLE: if (|s_awvalid) begin
                    owner <= s_awvalid[prefer_one] ? prefer_one : ~prefer_one;
                    state <= ADDR;
                end
                ADDR: if (m_awvalid && m_awready) state <= DATA;
                DATA: if (m_wvalid && m_wready && m_wlast) state <= RESP;
                RESP: if (m_bvalid && m_bready) begin
                    prefer_one <= ~owner;
                    state      <= IDLE;
                end
                default: state <= IDLE;
            endcase
        end
    end

    always_comb begin
        m_awid    = s_awid[owner];
        m_awaddr  = s_awaddr[owner];
        m_awlen   = s_awlen[owner];
        m_awsize  = s_awsize[owner];
        m_awburst = s_awburst[owner];
        m_awlock  = s_awlock[owner];
        m_awcache = s_awcache[owner];
        m_awprot  = s_awprot[owner];
        m_wdata   = s_wdata[owner];
        m_wstrb   = s_wstrb[owner];
        m_wlast   = s_wlast[owner];
        s_bid     = {m_bid, m_bid};
        s_bresp   = {m_bresp, m_bresp};
        m_awvalid = 1'b0;
        m_wvalid  = 1'b0;
        m_bready  = 1'b0;
        s_awready = '0;
        s_wready  = '0;
        s_bvalid  = '0;
        if (rst_n) begin
            case (state)
                ADDR: begin
                    m_awvalid        = s_awvalid[owner];
                    s_awready[owner] = m_awready;
                end
                DATA: begin
                    m_wvalid        = s_wvalid[owner];
                    s_wready[owner] = m_wready;
                end
                RESP: begin
                    m_bready        = s_bready[owner];
                    s_bvalid[owner] = m_bvalid;
                end
                default: begin end
            endcase
        end
    end
endmodule

`default_nettype wire
