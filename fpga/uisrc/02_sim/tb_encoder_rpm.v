`timescale 1ns/1ps
module tb_encoder_rpm;
    reg clk=0; always #5 clk=~clk;
    reg rst_n=0,raw_valid=0;
    reg signed [31:0] raw;
    wire signed [31:0] rpm;
    wire valid,overflow,busy;
    integer checks=0,i,seed=32'h12ab34cd;
    encoder_rpm dut(.clk(clk),.rst_n(rst_n),.raw(raw),.raw_valid(raw_valid),
        .rpm_milli(rpm),.rpm_valid(valid),.overflow(overflow),.busy(busy));
    task vector;
        input signed [31:0] value;
        reg signed [63:0] expected;
        reg expected_overflow; integer cycles;
        begin
            expected=($signed(value)*64'sd60000000)/64'sd409600;
            expected_overflow=(expected>2147483647 || expected < -64'sd2147483648);
            if(expected>2147483647) expected=2147483647;
            if(expected < -64'sd2147483648) expected=-64'sd2147483648;
            @(negedge clk); raw=value; raw_valid=1;
            @(negedge clk); raw_valid=0; cycles=0;
            while(!valid && cycles<66) begin @(negedge clk); cycles=cycles+1; end
            checks=checks+1;
            if(!valid || rpm!==expected[31:0] || overflow!==expected_overflow || busy)
                $fatal(1,"rpm raw=%0d expected=%0d got=%0d overflow=%b cycles=%0d",value,expected,rpm,overflow,cycles);
            @(negedge clk);
            if(valid) $fatal(1,"rpm_valid must be a pulse");
        end
    endtask
    initial begin
        repeat(4) @(negedge clk); rst_n=1;
        vector(0); vector(1); vector(-1); vector(1000); vector(-1000);
        vector(4096); vector(-4096); vector(111612); vector(32'h7fffffff); vector(32'h80000000);
        vector(14660155); vector(14660156); vector(-14660155); vector(-14660156);
        for(i=0;i<100;i=i+1) vector($random(seed)%600000);
        for(i=0;i<100;i=i+1) vector($random(seed));
        // Reset cancels an in-flight conversion without a valid pulse.
        @(negedge clk); raw=100; raw_valid=1;
        @(negedge clk); raw_valid=0; repeat(10) @(negedge clk); rst_n=0;
        @(negedge clk); rst_n=1;
        repeat(70) begin @(negedge clk); if(valid || busy) $fatal(1,"reset cancellation"); end
        vector(4096);
        $display("PASS tb_encoder_rpm checks=%0d",checks); $finish;
    end
    initial begin #1000000; $fatal(1,"watchdog rpm"); end
endmodule
