// Bundled-data toggle handshake. Both resets must assert together.
// Source data is held until the acknowledgement returns; valid while busy is lost
// and reported. Constrain hold -> data with a destination-period max delay.
module cdc_mailbox #(
    parameter integer WIDTH = 32
)(
    input logic I_src_clk, I_src_rstn,
    input logic I_valid,
    input logic [WIDTH-1:0] I_data,
    output logic O_ready, O_overflow,
    input logic I_dst_clk, I_dst_rstn,
    output logic O_valid,
    output logic [WIDTH-1:0] O_data
);
    logic [WIDTH-1:0] S_hold;
    logic S_request, S_ack;
    (* ASYNC_REG = "TRUE" *) logic [1:0] S_req_sync, S_ack_sync;
    assign O_ready = S_request == S_ack_sync[1];
    always_ff @(posedge I_src_clk or negedge I_src_rstn) begin
        if (!I_src_rstn) begin
            S_hold <= '0; S_request <= 0; S_ack_sync <= 0; O_overflow <= 0;
        end else begin
            S_ack_sync <= {S_ack_sync[0], S_ack};
            if (I_valid) begin
                if (O_ready) begin S_hold <= I_data; S_request <= ~S_request; end
                else O_overflow <= 1;
            end
        end
    end
    always_ff @(posedge I_dst_clk or negedge I_dst_rstn) begin
        if (!I_dst_rstn) begin
            S_req_sync <= 0; S_ack <= 0; O_valid <= 0; O_data <= '0;
        end else begin
            S_req_sync <= {S_req_sync[0], S_request};
            O_valid <= 0;
            if (S_req_sync[1] != S_ack) begin
                O_data <= S_hold;
                O_valid <= 1;
                S_ack <= S_req_sync[1];
            end
        end
    end
endmodule
