// Terminate unused template apertures with DECERR, including split AW/W writes.
module axi_lite_error(
    input wire aclk, rst_n,
    input wire [15:0] S_AXI_AWADDR, S_AXI_ARADDR,
    input wire [2:0] S_AXI_AWPROT, S_AXI_ARPROT,
    input wire [31:0] S_AXI_WDATA,
    input wire [3:0] S_AXI_WSTRB,
    input wire S_AXI_AWVALID, S_AXI_WVALID, S_AXI_ARVALID, S_AXI_BREADY, S_AXI_RREADY,
    output wire S_AXI_AWREADY, S_AXI_WREADY, S_AXI_ARREADY,
    output reg S_AXI_BVALID, S_AXI_RVALID,
    output wire [1:0] S_AXI_BRESP, S_AXI_RRESP,
    output wire [31:0] S_AXI_RDATA
);
    reg S_aw, S_w;
    assign S_AXI_AWREADY=rst_n && !S_aw && !S_AXI_BVALID;
    assign S_AXI_WREADY=rst_n && !S_w && !S_AXI_BVALID;
    assign S_AXI_ARREADY=rst_n && !S_AXI_RVALID;
    assign S_AXI_BRESP=2'b11;
    assign S_AXI_RRESP=2'b11;
    assign S_AXI_RDATA=0;
    always @(posedge aclk) begin
        if(!rst_n) begin S_aw<=0; S_w<=0; S_AXI_BVALID<=0; S_AXI_RVALID<=0; end
        else begin
            if(S_AXI_AWVALID && S_AXI_AWREADY) S_aw<=1;
            if(S_AXI_WVALID && S_AXI_WREADY) S_w<=1;
            if(S_aw && S_w && !S_AXI_BVALID) begin
                S_aw<=0; S_w<=0; S_AXI_BVALID<=1;
            end else if(S_AXI_BVALID && S_AXI_BREADY) S_AXI_BVALID<=0;
            if(S_AXI_ARVALID && S_AXI_ARREADY) S_AXI_RVALID<=1;
            else if(S_AXI_RVALID && S_AXI_RREADY) S_AXI_RVALID<=0;
        end
    end
endmodule
