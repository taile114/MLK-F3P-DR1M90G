`timescale 1ns/1ps
module tb_vibration_fft;
    parameter LOGN=8, WINDOW=1;
    localparam N=1<<LOGN;
    reg clk=0;always #5 clk=~clk;
    reg rstn=0,clear=0,valid=0,ready=1;
    reg signed [15:0] sample=0;
    wire out_valid,result_valid;
    wire [LOGN-1:0] bin;
    wire [63:0] power,peak;
    wire [31:0] seq,peak_bin,flags;
    integer i,checks=0,outputs=0;
    reg [63:0] max_power=0;
    reg [31:0] max_bin=0;
    reg [63:0] stalled_power;
    reg [LOGN-1:0] stalled_bin;
    vibration_fft #(.LOGN(LOGN),.WINDOW(WINDOW)) dut(
        .I_clk(clk),.I_rstn(rstn),.I_clear(clear),.I_enable(1'b1),.I_valid(valid),.I_sample(sample),
        .I_ready(ready),.O_valid(out_valid),.O_bin(bin),.O_power(power),
        .O_result_valid(result_valid),.O_sequence(seq),.O_peak_bin(peak_bin),.O_flags(flags),.O_peak_power(peak));
    always @(posedge clk) if(rstn && out_valid && ready) begin
        if(bin!==outputs%N) $fatal(1,"FFT natural bin order");
        if(seq==0 && power!==0) $fatal(1,"DC removal: constant power nonzero");
        if(bin==0) begin max_power=0;max_bin=0;end
        else if(bin<=N/2 && power>max_power) begin max_power=power;max_bin=bin;end
        outputs=outputs+1;
    end
    task send(input integer value);
        begin
            @(negedge clk);sample=value;valid=1;
            @(negedge clk);valid=0;
            // Input spacing leaves preprocessing time; no loss of raw sample cadence.
            repeat(64) @(negedge clk);
        end
    endtask
    initial begin
        repeat(4) @(negedge clk);rstn=1;
        for(i=0;i<N;i=i+1) send(1234);
        wait(seq==1);@(negedge clk);
        if(peak!==0 || peak_bin!==0 || flags!==0) $fatal(1,"constant FFT summary");
        checks=checks+1;
        for(i=0;i<N;i=i+1) send(2000+$rtoi(2000.0*$cos(6.283185307179586*16*i/N)));
        wait(out_valid);@(negedge clk);ready=0;stalled_power=power;stalled_bin=bin;
        repeat(7) begin
            @(negedge clk);
            if(!out_valid || bin!==stalled_bin || power!==stalled_power) $fatal(1,"FFT power backpressure");
        end
        ready=1;
        wait(seq==2);@(negedge clk);
        if(peak_bin!==16 || peak!==max_power || flags!==0 || outputs!=2*N) $fatal(1,"tone FFT summary bin=%0d",peak_bin);
        if(peak<64'd10000000000) $fatal(1,"power product lost upper bits");
        checks=checks+2;
        for(i=0;i<N;i=i+1) send(i%2 ? 32767 : -32768);
        wait(seq==3);@(negedge clk);
        if(peak_bin!==N/2 || !flags[0] || flags[1]) $fatal(1,"saturation/Nyquist summary");
        checks=checks+1;
        clear=1;@(negedge clk);clear=0;
        if(seq!==0 || flags!==0 || peak!==0) $fatal(1,"FFT clear");
        checks=checks+1;
        $display("RESULT tb_vibration_fft PASS=%0d FAIL=0",checks);$finish;
    end
    initial begin #10000000;$fatal(1,"FFT schedule timeout");end
endmodule
