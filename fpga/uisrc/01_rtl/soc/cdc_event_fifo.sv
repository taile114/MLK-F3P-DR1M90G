// Single writer / reader asynchronous event queue, power-of-two depth.
// Gray-pointer crossings require max-delay <= the faster clock period.
module cdc_event_fifo #(
    parameter integer WIDTH = 91,
    parameter integer ADDR_BITS = 4
)(
    input logic I_src_clk, I_src_rstn,
    input logic I_valid,
    input logic [WIDTH-1:0] I_data,
    output logic O_ready, O_overflow,
    input logic I_dst_clk, I_dst_rstn,
    output logic O_valid,
    output logic [WIDTH-1:0] O_data
);
    localparam integer PTR_BITS = ADDR_BITS+1;
    logic [WIDTH-1:0] S_mem[0:(1<<ADDR_BITS)-1];
    logic [PTR_BITS-1:0] S_wbin, S_wgray, S_rbin, S_rgray;
    (* ASYNC_REG = "TRUE" *) logic [PTR_BITS-1:0] S_rsync1, S_rsync2, S_wsync1, S_wsync2;
    logic S_full, S_empty;
    wire [PTR_BITS-1:0] S_wnext = S_wbin + {{ADDR_BITS{1'b0}}, (I_valid && !S_full)};
    wire [PTR_BITS-1:0] S_wgray_next = (S_wnext>>1)^S_wnext;
    wire [PTR_BITS-1:0] S_rnext = S_rbin + {{ADDR_BITS{1'b0}}, !S_empty};
    wire [PTR_BITS-1:0] S_rgray_next = (S_rnext>>1)^S_rnext;
    assign O_ready = !S_full;
    always_ff @(posedge I_src_clk or negedge I_src_rstn) begin
        if (!I_src_rstn) begin
            S_wbin<=0; S_wgray<=0; S_rsync1<=0; S_rsync2<=0; S_full<=0; O_overflow<=0;
        end else begin
            S_rsync1<=S_rgray; S_rsync2<=S_rsync1;
            S_wbin<=S_wnext; S_wgray<=S_wgray_next;
            S_full<=S_wgray_next=={~S_rsync2[PTR_BITS-1:PTR_BITS-2],S_rsync2[PTR_BITS-3:0]};
            if (I_valid && S_full) O_overflow<=1;
        end
    end
    always_ff @(posedge I_src_clk)
        if (I_valid && O_ready) S_mem[S_wbin[ADDR_BITS-1:0]]<=I_data;
    always_ff @(posedge I_dst_clk or negedge I_dst_rstn) begin
        if (!I_dst_rstn) begin
            S_rbin<=0; S_rgray<=0; S_wsync1<=0; S_wsync2<=0;
            S_empty<=1; O_valid<=0; O_data<='0;
        end else begin
            S_wsync1<=S_wgray; S_wsync2<=S_wsync1;
            S_rbin<=S_rnext; S_rgray<=S_rgray_next;
            S_empty<=S_rgray_next==S_wsync2;
            O_valid<=!S_empty;
            if (!S_empty) O_data<=S_mem[S_rbin[ADDR_BITS-1:0]];
        end
    end
endmodule
