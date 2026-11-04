`timescale 1ns/1ps
module tb_time_features;
    parameter integer LOGN=8;
    localparam integer N=1<<LOGN, NS=9*N, NR=(NS-N)/128+1;
    reg clk=0; always #5 clk=~clk;
    reg rstn=0, valid=0;
    reg signed [15:0] sample=0;
    wire ready, result_valid, overrun;
    wire [447:0] result;
    wire [1:0] flags;
    reg [15:0] samples[0:NS-1];
    reg [447:0] expected[0:NR-1];
    reg [1:0] expected_flags[0:NR-1];
    integer i, received=0, checks=0, errors=0, busy_cycles=0, max_busy=0;
    time_features #(.WINDOW_LOG2(LOGN)) dut(.I_clk(clk),.I_rstn(rstn),.I_valid(valid),
        .I_sample(sample),.O_ready(ready),.O_valid(result_valid),.O_overrun(overrun),
        .O_flags(flags),.O_features(result));
    always @(posedge clk) if(rstn) begin
        if(dut.S_we && dut.S_re) $fatal(1,"time RAM read/write collision");
        if(!ready) busy_cycles=busy_cycles+1;
        else begin
            if(busy_cycles>max_busy) max_busy=busy_cycles;
            busy_cycles=0;
        end
    end
    always @(negedge clk) if(rstn && result_valid) begin
        if(received>=NR || result !== expected[received] || flags !== expected_flags[received]) begin
            $display("FAIL time N=%0d window=%0d got=%h expected=%h flags=%h",N,received,result,expected[received],flags);
            errors=errors+1;
        end
        received=received+1; checks=checks+8;
    end
    initial begin
        if(LOGN==8) begin
            $readmemh("02_sim/vectors/time_256_samples.hex",samples);
            $readmemh("02_sim/vectors/time_256_expected.hex",expected);
            $readmemh("02_sim/vectors/time_256_flags.hex",expected_flags);
        end else begin
            $readmemh("02_sim/vectors/time_512_samples.hex",samples);
            $readmemh("02_sim/vectors/time_512_expected.hex",expected);
            $readmemh("02_sim/vectors/time_512_flags.hex",expected_flags);
        end
        repeat(4) @(negedge clk); rstn=1;
        // 中途复位丢弃未满窗口。
        repeat(13) begin
            while(!ready) @(negedge clk);
            sample=-123; valid=1; @(negedge clk); valid=0;
        end
        rstn=0; repeat(3) @(negedge clk); rstn=1;
        for(i=0;i<NS;i=i+1) begin
            while(!ready) @(negedge clk);
            sample=samples[i]; valid=1; @(negedge clk); valid=0;
            repeat(i%3+1) @(negedge clk);
            if(i==N-1) begin
                if(ready) $fatal(1,"feature calculation did not block input");
                sample=1234;valid=1;@(negedge clk);valid=0;
                if(!overrun) $fatal(1,"missing overrun pulse");
                checks=checks+1;
            end
        end
        wait(received==NR); @(negedge clk);
        if(max_busy>=31250) $fatal(1,"feature latency exceeds ADXL sample period");
        checks=checks+1;
        if(errors) $fatal(1,"time feature errors=%0d",errors);
        $display("RESULT tb_time_features N=%0d PASS=%0d FAIL=0 windows=%0d max_busy=%0d",N,checks,received,max_busy);
        $finish;
    end
    initial begin #100000000; $fatal(1,"time features timeout N=%0d received=%0d",N,received); end
endmodule
