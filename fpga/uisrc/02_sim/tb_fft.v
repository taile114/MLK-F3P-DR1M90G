`timescale 1ns/1ps
module tb_fft;
    parameter LOGN=8, WINDOW=1;
    localparam N=1<<LOGN, NF=7;
    reg clk=0; always #5 clk=~clk;
    reg rstn=0, valid=0, ready_out=0;
    reg signed [15:0] sample=0;
    wire ready_in,valid_out,last;
    wire [LOGN-1:0] bin;
    wire signed [31:0] real_out,imag_out;
    reg [15:0] samples[0:NF*N-1];
    reg [63:0] expected[0:NF*N-1];
    integer count=0, errors=0, cycles=0, i, in_count=0, start_cycle=0, max_first_bin=0;
    reg held=0;
    reg [63:0] held_data;
    reg [LOGN-1:0] held_bin;
    fft_radix2 #(.LOGN(LOGN),.WINDOW(WINDOW)) dut(.I_clk(clk),.I_rstn(rstn),
        .I_valid(valid),.I_sample(sample),.O_ready(ready_in),.O_valid(valid_out),
        .I_ready(ready_out),.O_bin(bin),.O_real(real_out),.O_imag(imag_out),.O_last(last));
    always @(negedge clk) begin
        cycles=cycles+1;
        ready_out=(cycles%7!=0 && cycles%7!=1 && cycles%7!=2);
    end
    always @(posedge clk) if(!rstn) in_count=0;
    else begin
        if(dut.S_we && dut.S_re) $fatal(1,"FFT RAM read/write collision");
        if(valid && ready_in) begin
            in_count=in_count+1;
            if(in_count%N==0) start_cycle=cycles;
        end
        if(held && (!valid_out || {real_out,imag_out}!==held_data || bin!==held_bin))
            $fatal(1,"FFT output changed while stalled");
        held=valid_out && !ready_out; held_data={real_out,imag_out}; held_bin=bin;
        if(valid_out && ready_out) begin
            if(count%N==0 && cycles-start_cycle>max_first_bin) max_first_bin=cycles-start_cycle;
            if(bin!==count%N || last!==(count%N==N-1) || {real_out,imag_out}!==expected[count]) begin
                if(errors<10) $display("FAIL FFT N=%0d window=%0d index=%0d got=%h expected=%h",N,WINDOW,count,{real_out,imag_out},expected[count]);
                errors=errors+1;
            end
            count=count+1;
        end
    end
    initial begin
        if(LOGN==8) begin
            $readmemh("02_sim/vectors/fft_256_samples.hex",samples);
            if(WINDOW) $readmemh("02_sim/vectors/fft_256_1_expected.hex",expected);
            else $readmemh("02_sim/vectors/fft_256_0_expected.hex",expected);
        end else begin
            $readmemh("02_sim/vectors/fft_512_samples.hex",samples);
            if(WINDOW) $readmemh("02_sim/vectors/fft_512_1_expected.hex",expected);
            else $readmemh("02_sim/vectors/fft_512_0_expected.hex",expected);
        end
        repeat(4) @(negedge clk); rstn=1;
        // 部分输入帧复位后重新从 bin0 收帧。
        repeat(5) begin
            @(negedge clk);valid=1;sample=-123;
            @(posedge clk);while(!ready_in) @(posedge clk);
            @(negedge clk);valid=0;
        end
        rstn=0;repeat(3) @(negedge clk);rstn=1;
        for(i=0;i<NF*N;i=i+1) begin
            @(negedge clk);
            sample=samples[i]; valid=1;
            @(posedge clk);while(!ready_in) @(posedge clk);
            @(negedge clk); valid=0;
            if(i%11==0) repeat(3) @(negedge clk);
        end
        wait(count==NF*N); @(negedge clk);
        if(errors) $fatal(1,"FFT errors=%0d",errors);
        $display("RESULT tb_fft N=%0d Hann=%0d PASS=%0d FAIL=0 max_first_bin=%0d",N,WINDOW,count*3,max_first_bin); $finish;
    end
    initial begin #100000000; $fatal(1,"FFT timeout count=%0d",count); end
endmodule
