`timescale 1ns/1ps
module tb_cdc_soc;
    reg src_clk=0,dst_clk=0,rstn=0;
    always #5 src_clk=~src_clk;
    always #11 dst_clk=~dst_clk;
    reg valid=0;
    reg [31:0] data=0;
    wire ready,overflow,out_valid;
    wire [31:0] out_data;
    integer sent=0,received=0,i;
    cdc_event_fifo #(.WIDTH(32)) dut(
        .I_src_clk(src_clk),.I_src_rstn(rstn),.I_valid(valid),.I_data(data),
        .O_ready(ready),.O_overflow(overflow),.I_dst_clk(dst_clk),.I_dst_rstn(rstn),
        .O_valid(out_valid),.O_data(out_data));
    always @(negedge dst_clk) if(rstn && out_valid) begin
        if(out_data!==received) $fatal(1,"CDC order/data expected=%0d got=%0d",received,out_data);
        received=received+1;
    end
    initial begin
        repeat(4) @(negedge src_clk);rstn=1;
        // Random phase, full queue, pointer wrap, then complete drain.
        for(i=0;i<1000;i=i+1) begin
            @(negedge src_clk);valid=ready && (i%7!=0);data=sent;
            @(posedge src_clk);if(valid && ready) sent=sent+1;
        end
        @(negedge src_clk);valid=0;
        repeat(40) @(negedge dst_clk);
        if(received!=sent || overflow) $fatal(1,"CDC count/overflow");
        for(i=0;i<100;i=i+1) begin
            @(negedge src_clk);valid=1;data=sent;
            @(posedge src_clk);if(ready) sent=sent+1;
        end
        @(negedge src_clk);valid=0;
        repeat(40) @(negedge dst_clk);
        if(received!=sent || !overflow) $fatal(1,"CDC full/loss indication");
        rstn=0;repeat(4) @(negedge src_clk);sent=0;received=0;rstn=1;
        @(negedge src_clk);data=0;valid=1;
        @(negedge src_clk);valid=0;
        repeat(12) @(negedge dst_clk);
        if(received!=1) $fatal(1,"CDC reset/recovery");
        $display("RESULT tb_cdc_soc PASS=3 FAIL=0");$finish;
    end
    initial begin #1000000;$fatal(1,"CDC timeout");end
endmodule
