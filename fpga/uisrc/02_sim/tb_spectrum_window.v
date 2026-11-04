`timescale 1ns/1ps
// 完整频谱窗口（§4.6）自检：bin RAM 写入口、整窗逐字推送、AXI 读窗口、
// 窗口边界与 0x78 序列配对。bin 流由测试台直接驱动 FFT 的输出端口，采集
// 通路保持复位值，因此每个 bin 的功率逐 bin 可预测。
// 推送通路按「索引 0 开窗、最后一字置有效位与完成脉冲」校验，四相握手的
// 单 bit 翻转沿与数据总线都在寄存器级，本仿真能覆盖其功能；但「同一数组
// 被两个时钟域访问」在仿真里看不出综合差异，实板曾因此把 bin 读到全 0，
// 故 bin RAM 的读写口必须在同一域（见 sensor_soc_axi 中的说明）。
// 覆盖点：
//   1) 未写过窗口时 0x400+4k 读 0；
//   2) 一窗 bin 在结果字到达前不可见（半窗快照保持上一窗）；
//   3) 整窗写完后逐 bin 校验，bin 0 用带非零低位与更高位的原始值验证
//      取 O_power[39:8]；
//   4) 窗口边界 0x3FC / 0x400 / 0x400+4(N-1) / 0x400+4N 与未对齐、未映射地址；
//   5) 0x78~0x88 与快照同窗发布，读 bin 前后序号一致；
//   6) 推送进行中的读等待到整窗推完后再返回新窗；
//   7) 整窗 N 个字到齐才给完成脉冲（完成拍索引必为 N-1、字数必为 N）；
//   8) 推送途中软复位：窗口立即读 0，在途尾字不回填有效位；下一窗从索引 0
//      重来并正常发布；
//   9) 硬复位与 CTRL.RST 后窗口读 0、序号回 0；
//  10) 定标 >>8 的边界：|X|² 接近 2^40 仍保留高位、0x100 读出 1、
//      0xFFFFFFFF00 读出 0xFFFF_FFFF、高于 39 位的原始位被丢弃。
module tb_spectrum_window;
    parameter integer LOGN=8;
    localparam integer BINS=1<<LOGN;
    localparam [15:0] BASE=16'h0400;
    localparam [15:0] LAST=BASE+(BINS*4)-4;
    localparam [15:0] ENDA=BASE+(BINS*4);
    localparam [15:0] BELOW=16'h03fc;
    localparam [15:0] UNALIGN=16'h0402;
    localparam [15:0] FAR=16'h1000;
    reg clk=0,sensor_clk=0,rstn=0;
    always #10 clk=~clk;              // 寄存器域 50 MHz
    always #5 sensor_clk=~sensor_clk; // 采集域 100 MHz
    reg [15:0] awaddr=0,araddr=0;
    reg awvalid=0,wvalid=0,bready=0,arvalid=0,rready=0;
    reg [31:0] wdata=0;
    reg [3:0] wstrb=0;
    wire awready,wready,bvalid,arready,rvalid,irq;
    wire [1:0] bresp,rresp;
    wire [31:0] rdata;
    tri sdio,scl,sda;
    wire sclk,cs;
    wire [3:0] led;
    reg [31:0] rd;
    integer checks=0,k,w;
    sensor_soc_axi #(.LOGN(LOGN)) dut(.I_clk(clk),.I_rstn(rstn),.I_sysclk(sensor_clk),
        .I_uart_rx(1'b1),.I_int1(1'b0),.O_spi_sclk(sclk),.O_spi_cs_n(cs),
        .IO_spi_sdio(sdio),.IO_iic_scl(scl),.IO_iic_sda(sda),.O_led(led),
        .I_awaddr(awaddr),.I_awvalid(awvalid),.O_awready(awready),.I_wdata(wdata),
        .I_wstrb(wstrb),.I_wvalid(wvalid),.O_wready(wready),.O_bresp(bresp),.O_bvalid(bvalid),
        .I_bready(bready),.I_araddr(araddr),.I_arvalid(arvalid),.O_arready(arready),
        .O_rdata(rdata),.O_rresp(rresp),.O_rvalid(rvalid),.I_rready(rready),.O_irq(irq),
        .I_rs485_rx(1'b1),.O_rs485_tx(),.O_rs485_de());
    pullup(sdio);pullup(scl);pullup(sda);

    task check(input bit good,input [255:0] name);
        begin checks=checks+1;if(!good) $fatal(1,"FAIL %s check=%0d",name,checks);end
    endtask
    task send_aw(input [15:0] addr);
        begin
            @(negedge clk); awaddr=addr; awvalid=1;
            @(posedge clk); while(!awready) @(posedge clk);
            @(negedge clk); awvalid=0;
        end
    endtask
    task send_w(input [31:0] data,input [3:0] strb);
        begin
            @(negedge clk);wdata=data;wstrb=strb;wvalid=1;
            @(posedge clk);while(!wready) @(posedge clk);
            @(negedge clk);wvalid=0;
        end
    endtask
    task write_reg(input [15:0] addr,input [31:0] data,input [3:0] strb,input integer order,input [1:0] resp);
        begin
            if(order<0) begin send_aw(addr);repeat(3) @(negedge clk);send_w(data,strb);end
            else if(order>0) begin send_w(data,strb);repeat(3) @(negedge clk);send_aw(addr);end
            else fork send_aw(addr);send_w(data,strb);join
            while(!bvalid) @(negedge clk);
            repeat(4) begin
                check(bvalid && bresp===resp,"B stall/response");
                check(!awready && !wready,"no overwrite stalled B");
                @(negedge clk);
            end
            bready=1;@(negedge clk);bready=0;
        end
    endtask
    task read_reg(input [15:0] addr,input [1:0] resp,output [31:0] data);
        begin
            @(negedge clk);araddr=addr;arvalid=1;
            @(posedge clk);while(!arready) @(posedge clk);
            @(negedge clk);arvalid=0;
            while(!rvalid) @(negedge clk);
            data=rdata;
            repeat(4) begin
                check(rvalid && rresp===resp && rdata===data,"R stability/response");
                @(negedge clk);
            end
            rready=1;@(negedge clk);rready=0;
        end
    endtask
    task expect_reg(input [15:0] addr,input [31:0] value);
        begin read_reg(addr,0,rd);check(rd===value,"register value");end
    endtask

    // 第 w 个窗口第 k 个 bin 的功率；各窗互不相同，便于识别陈旧数据。
    function [31:0] fsel(input integer wi,input integer k);
        begin
            case(wi)
                1: fsel=32'h1234_5678+k;
                2: fsel=32'h9000_0000-k;
                3: fsel=32'h4000_0000+(k<<1);
                4: fsel=32'h0f00_0000+(k<<2);
                default: fsel=32'h7a00_0000+k;
            endcase
        end
    endfunction
    // 直接给出 64 bit 的 O_power，用于验证只取 [39:8]。
    task drive_bin_raw(input integer k,input [63:0] raw);
        begin
            @(negedge sensor_clk);
            force dut.S_fft_power_bin=k[LOGN-1:0];
            force dut.S_fft_power=raw;
            force dut.S_fft_power_valid=1'b1;
            @(negedge sensor_clk);
            force dut.S_fft_power_valid=1'b0;
        end
    endtask
    task drive_bin(input integer k,input [31:0] power);
        drive_bin_raw(k,{24'd0,power,8'd0});
    endtask
    // 结果字（seq/bin/peak/flags）与 O_result_valid 同拍给出，与振动 FFT 一致。
    task complete_window(input integer wi,input integer seq);
        begin
            wait(dut.X_fft_result.O_ready===1'b1);
            @(negedge sensor_clk);
            force dut.S_fft_seq=seq[31:0];
            force dut.S_fft_bin=32'h0000_0010+wi[31:0];
            force dut.S_fft_peak={32'h5000_0000+wi[31:0],32'ha000_0000+wi[31:0]};
            force dut.S_fft_flags=32'hf000_0000+wi[31:0];
            force dut.S_fft_valid=1'b1;
            @(negedge sensor_clk);
            force dut.S_fft_valid=1'b0;
        end
    endtask
    // 推送进行中：自索引 0 起、到最后一字写完为止。
    task wait_push_start;
        integer t;
        begin
            t=0;
            while(!dut.X_regs.S_push_busy && t<2000) begin @(negedge clk);t=t+1;end
            check(dut.X_regs.S_push_busy,"push start");
        end
    endtask
    task wait_push_done;
        integer t;
        begin
            t=0;
            while(dut.X_regs.S_push_busy && t<40000) begin @(negedge clk);t=t+1;end
            check(!dut.X_regs.S_push_busy,"push done");
        end
    endtask
    // 采集域推送状态机回到空闲（在途传输含最后一字的回执）。
    task wait_src_idle;
        integer t;
        begin
            t=0;
            while(dut.S_xstate!==2'd0 && t<40000) begin @(negedge clk);t=t+1;end
            check(dut.S_xstate===2'd0,"push source idle");
        end
    endtask
    // 完成脉冲必须落在最后一字：索引为 N-1，且此前恰好推入 N 个字。
    integer words=0,done_cnt=0,done_mark=0;
    always @(posedge clk) begin
        if(dut.X_regs.S_push_edge) begin
            if(dut.X_regs.I_spect_index===0) words=1;
            else words=words+1;
        end
        if(dut.S_spect_done) begin
            done_cnt=done_cnt+1;
            check(dut.X_regs.I_spect_index===BINS-1,"done on last bin");
            check(words===BINS,"whole window pushed before done");
            words=0;
        end
    end
    // 结果字与快照同窗：序号、峰值 bin、峰值功率、标志与本次窗口一致。
    task check_result(input integer wi,input integer seq);
        begin
            expect_reg(16'h78,seq[31:0]);
            expect_reg(16'h7c,32'h0000_0010+wi[31:0]);
            expect_reg(16'h80,32'ha000_0000+wi[31:0]);
            expect_reg(16'h84,32'h5000_0000+wi[31:0]);
            expect_reg(16'h88,32'hf000_0000+wi[31:0]);
        end
    endtask
    task sweep_spectrum(input integer wi);
        integer kk;
        reg [31:0] exp;
        begin
            for(kk=0;kk<BINS;kk=kk+1) begin
                exp=fsel(wi,kk);
                read_reg(BASE+4*kk,0,rd);
                check(rd===exp,"spectrum bin");
            end
        end
    endtask

    initial begin
        // FFT 输出端口改由测试台驱动；FFT 内部即使产生结果也不进邮箱。
        force dut.S_fft_power_valid=1'b0;
        force dut.S_fft_valid=1'b0;
        repeat(10) @(posedge clk);
        rstn=1;
        repeat(40) @(negedge clk);

        // 1) 复位后未写过窗口：窗口内读 0，窗口外按未映射地址处理。
        expect_reg(16'h00,32'h504c0103);
        expect_reg(16'h78,0);
        read_reg(BASE,0,rd);     check(rd===0,"empty window base");
        read_reg(LAST,0,rd);     check(rd===0,"empty window last");
        read_reg(BELOW,2,rd);    check(rd===0,"below window SLVERR");
        read_reg(ENDA,2,rd);     check(rd===0,"window end SLVERR");
        read_reg(UNALIGN,2,rd);  check(rd===0,"unaligned SLVERR");
        read_reg(FAR,2,rd);      check(rd===0,"unmapped SLVERR");
        write_reg(16'h04,32'h0000_0001,4'hf,0,0);
        expect_reg(16'h04,32'h0000_0001);

        // 2) 第一个窗口：半窗时快照与序号都不更新。
        drive_bin_raw(0,64'hbeef_de12_3456_78ad); // 丢弃 0xbeefde 与 0xad
        for(k=1;k<64;k=k+1) drive_bin(k,fsel(1,k));
        expect_reg(16'h78,0);
        read_reg(BASE,0,rd);     check(rd===0,"no snapshot mid window");
        for(k=64;k<BINS;k=k+1) drive_bin(k,fsel(1,k));
        complete_window(1,1);
        wait_push_start;
        expect_reg(16'h78,0);            // 拷贝未完成，序号保持上一窗
        wait_push_done;
        check_result(1,1);
        sweep_spectrum(1);
        expect_reg(16'h78,1);

        // 3) 窗口边界：0x3FC 与 0x800 为未映射，0x400 与末 bin 为有效数据。
        read_reg(BELOW,2,rd);    check(rd===0,"boundary below SLVERR");
        read_reg(BASE,0,rd);     check(rd===fsel(1,0),"boundary first bin");
        read_reg(LAST,0,rd);     check(rd===fsel(1,BINS-1),"boundary last bin");
        read_reg(ENDA,2,rd);     check(rd===0,"boundary end SLVERR");
        read_reg(UNALIGN,2,rd);  check(rd===0,"boundary unaligned SLVERR");
        // 频谱窗口只读：写返回 SLVERR，内容不变。
        write_reg(BASE,32'hdead_beef,4'hf,0,2);
        expect_reg(BASE,fsel(1,0));

        // 4) 第二个窗口：半窗时仍读到上一窗，推送中的读等待到新窗。
        for(k=0;k<64;k=k+1) drive_bin(k,fsel(2,k));
        read_reg(BASE,0,rd);     check(rd===fsel(1,0),"old window kept mid window");
        expect_reg(16'h78,1);
        for(k=64;k<BINS;k=k+1) drive_bin(k,fsel(2,k));
        complete_window(2,2);
        wait_push_start;
        expect_reg(16'h78,1);            // 序号与快照同拍发布，推送中仍是上一窗
        read_reg(BASE,0,rd);     check(rd===fsel(2,0),"read during push gets new window");
        wait_push_done;
        check_result(2,2);
        sweep_spectrum(2);
        expect_reg(16'h78,2);

        // 5) 推送途中软复位：窗口立即读 0，在途尾字不回填有效位，下一窗从
        //    索引 0 重来并正常发布。
        for(k=0;k<BINS;k=k+1) drive_bin(k,fsel(3,k));
        complete_window(3,3);
        wait_push_start;
        done_mark=done_cnt;
        write_reg(16'h04,32'h0000_0007,4'hf,0,0);
        repeat(20) @(negedge clk);
        expect_reg(16'h78,0);
        read_reg(BASE,0,rd);     check(rd===0,"reset during push clears window");
        wait_src_idle;                       // 在途传输收尾，尾字不得置有效位
        check(done_cnt===done_mark,"aborted window no done");
        expect_reg(16'h78,0);
        read_reg(BASE,0,rd);     check(rd===0,"aborted window never published");
        for(k=0;k<BINS;k=k+1) drive_bin(k,fsel(4,k));
        complete_window(4,1);
        wait_push_start;
        expect_reg(16'h78,0);
        wait_push_done;
        check_result(4,1);
        sweep_spectrum(4);
        expect_reg(16'h78,1);

        // 6) CTRL.RST：窗口读 0、序号回 0，半窗不产生新数据。
        write_reg(16'h04,32'h0000_0007,4'hf,0,0);
        repeat(40) @(negedge clk);
        expect_reg(16'h04,32'h0000_0005);
        expect_reg(16'h78,0);
        read_reg(BASE,0,rd);     check(rd===0,"soft reset clears window");
        read_reg(LAST,0,rd);     check(rd===0,"soft reset clears last bin");
        for(k=0;k<(BINS/2);k=k+1) drive_bin(k,fsel(5,k));
        expect_reg(16'h78,0);
        read_reg(BASE,0,rd);     check(rd===0,"incomplete window stays clear");
        for(k=(BINS/2);k<BINS;k=k+1) drive_bin(k,fsel(5,k));
        complete_window(5,1);
        wait_push_start;
        expect_reg(16'h78,0);            // 复位后首个窗口同样在推送完成时才发布
        wait_push_done;
        check_result(5,1);
        sweep_spectrum(5);

        // 7) >>8 定标边界：接近 2^40 的功率保留高位，0x100 读出 1，
        //    0xFFFFFFFF00 读出 0xFFFF_FFFF，高于 39 位的原始位被丢弃。
        for(k=0;k<BINS;k=k+1) begin
            case(k)
                0: drive_bin_raw(k,64'h0000_00ef_ffff_ff00);
                1: drive_bin_raw(k,64'h0000_0000_0000_0100);
                2: drive_bin_raw(k,64'h0000_00ff_ffff_ff00);
                3: drive_bin_raw(k,64'h0000_0000_ffff_ffff);
                default: drive_bin(k,fsel(6,k));
            endcase
        end
        expect_reg(16'h78,1);            // 本窗推送完成前序号仍是上一窗
        complete_window(6,2);
        wait_push_start;
        expect_reg(16'h78,1);
        wait_push_done;
        check_result(6,2);
        read_reg(BASE,0,rd);
        check(rd===32'hefff_ffff,"near 2^40 keeps high bits");
        read_reg(BASE+4,0,rd);
        check(rd===32'd1,"small power 0x100 reads 1");
        read_reg(BASE+8,0,rd);
        check(rd===32'hffff_ffff,"max power reaches uint32 max");
        read_reg(BASE+12,0,rd);
        check(rd===32'h00ff_ffff,"bits above 39 dropped");
        for(k=4;k<BINS;k=k+1) begin
            read_reg(BASE+(k*4),0,rd);
            check(rd===fsel(6,k),"scaled bin value");
        end
        check(done_cnt===5,"one done per completed window");

        $display("RESULT tb_spectrum_window LOGN=%0d BINS=%0d PASS=%0d FAIL=0",LOGN,BINS,checks);
        $finish;
    end
endmodule
