`timescale 1ns/1ps
module tb_encoder_uart;
    localparam BIT_CLKS=10417;
    reg clk=0; always #5 clk=~clk;
    reg rst_n=0,rx=1,enable=1,start=0;
    reg [7:0] tx_data=0;
    wire tx,busy,done,valid,error,rx_busy,rx_sync;
    wire [7:0] data;
    encoder_uart dut(.clk(clk),.rst_n(rst_n),.tx_start(start),.tx_data(tx_data),
        .tx(tx),.tx_busy(busy),.tx_done(done),.rx(rx),.rx_enable(enable),
        .rx_data(data),.rx_valid(valid),.rx_error(error),.rx_busy(rx_busy),.rx_sync(rx_sync));
    integer valid_count=0,error_count=0,checks=0,i;
    reg [7:0] last_data;
    always @(posedge clk) begin
        if(valid) begin valid_count=valid_count+1; last_data=data; end
        if(error) error_count=error_count+1;
    end
    task send;
        input [7:0] value; input integer clocks; input bad_stop;
        integer b;
        begin
            @(negedge clk); rx=0; repeat(clocks) @(negedge clk);
            for(b=0;b<8;b=b+1) begin rx=value[b]; repeat(clocks) @(negedge clk); end
            rx=!bad_stop; repeat(clocks) @(negedge clk); rx=1;
        end
    endtask
    task rx_check;
        input [7:0] value; input integer clocks;
        integer old;
        begin
            old=valid_count; send(value,clocks,0); repeat(10) @(negedge clk);
            checks=checks+1;
            if(valid_count!=old+1 || last_data!==value) $fatal(1,"RX value=%h clocks=%0d got=%h",value,clocks,last_data);
        end
    endtask
    initial begin
        repeat(10) @(negedge clk); rst_n=1;
        repeat(BIT_CLKS*2) @(negedge clk);
        rx_check(8'h00,BIT_CLKS); rx_check(8'hff,BIT_CLKS);
        rx_check(8'ha5,BIT_CLKS); rx_check(8'h5a,BIT_CLKS);
        rx_check(8'h96,10209); rx_check(8'h69,10625); // about +/-2% baud skew
        send(8'h55,BIT_CLKS,1); repeat(BIT_CLKS*2) @(negedge clk);
        if(error_count!=1 || valid_count!=6) $fatal(1,"stop-bit rejection"); checks=checks+1;
        rx=0; repeat(BIT_CLKS*30) @(negedge clk); rx=1;
        repeat(BIT_CLKS*2) @(negedge clk);
        if(error_count!=2 || valid_count!=6) $fatal(1,"BREAK rejection"); checks=checks+1;
        rx_check(8'h42,BIT_CLKS);
        // A sub-half-bit start glitch must not publish data.
        rx=0; repeat(BIT_CLKS/4) @(negedge clk); rx=1;
        repeat(BIT_CLKS*12) @(negedge clk);
        if(valid_count!=7) $fatal(1,"false start accepted"); checks=checks+1;
        // Verify exact default baud, bit order and full stop interval.
        tx_data=8'ha6; start=1;
        @(negedge clk); start=0;
        if(tx!==0 || !busy) $fatal(1,"TX launch");
        repeat(BIT_CLKS) @(negedge clk);
        for(i=0;i<8;i=i+1) begin
            if(tx!==tx_data[i] || !busy) $fatal(1,"TX bit %0d",i);
            repeat(BIT_CLKS) @(negedge clk);
        end
        if(tx!==1 || !busy) $fatal(1,"TX stop");
        repeat(BIT_CLKS-1) @(negedge clk);
        if(!busy || done) $fatal(1,"TX released before full stop");
        @(negedge clk); if(busy || !done || !tx) $fatal(1,"TX completion"); checks=checks+1;
        // Mid-byte reset cancels the frame and requires a full idle bit.
        rx=0; repeat(BIT_CLKS*3) @(negedge clk); rst_n=0;
        repeat(5) @(negedge clk); rst_n=1;
        repeat(BIT_CLKS*12) @(negedge clk); rx=1;
        repeat(BIT_CLKS*2) @(negedge clk);
        if(valid_count!=7) $fatal(1,"reset accepted stale frame");
        rx_check(8'h81,BIT_CLKS);
        // A faster sender must not accumulate phase error over a burst.
        for(i=0;i<16;i=i+1) rx_check(i^8'ha6,10209);
        $display("PASS tb_encoder_uart checks=%0d rx=%0d errors=%0d default_bit_clocks=%0d",checks,valid_count,error_count,BIT_CLKS);
        $finish;
    end
    initial begin #40000000; $fatal(1,"watchdog uart"); end
endmodule
