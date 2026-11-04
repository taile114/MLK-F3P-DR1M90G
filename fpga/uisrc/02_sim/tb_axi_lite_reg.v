`timescale 1ns/1ps
module tb_axi_lite_reg;
    reg clk=0; always #5 clk=~clk;
    reg rstn=0;
    reg [15:0] awaddr=0,araddr=0;
    reg awvalid=0,wvalid=0,bready=0,arvalid=0,rready=0;
    reg [31:0] wdata=0;
    reg [3:0] wstrb=0;
    wire awready,wready,bvalid,arready,rvalid;
    wire [1:0] bresp,rresp;
    wire [31:0] rdata;
    reg busy=0,overflow=0,done=0,sample_valid=0;
    reg [7:0] state=0,faults=0;
    reg [15:0] sample=0;
    reg signed [15:0] temp=0,x=0,y=0,z=0,current=0;
    reg temp_valid=0,acc_valid=0,current_valid=0;
    reg [3:0] feature_valid=0;
    reg [1791:0] features=0;
    reg [7:0] flags=0;
    wire alarm;
    wire enable,irq_enable,soft_reset,irq;
    integer checks=0,pulses=0,resets=0,i,c,j;
    reg [31:0] rd,model=0,datum,mask;
    reg [3:0] strobe;
    reg last_irq=0;
    reg [31:0] expected_stamp,frame_before,ts_before;
    axi_lite_reg dut(.I_clk(clk),.I_rstn(rstn),.I_awaddr(awaddr),.I_awvalid(awvalid),
        .O_awready(awready),.I_wdata(wdata),.I_wstrb(wstrb),.I_wvalid(wvalid),.O_wready(wready),
        .O_bresp(bresp),.O_bvalid(bvalid),.I_bready(bready),.I_araddr(araddr),.I_arvalid(arvalid),
        .O_arready(arready),.O_rdata(rdata),.O_rresp(rresp),.O_rvalid(rvalid),.I_rready(rready),
        .I_busy(busy),.I_overflow(overflow),.I_done(done),.I_state(state),.I_sample(sample),
        .I_sample_valid(sample_valid),.I_temp(temp),.I_acc_x(x),.I_acc_y(y),.I_acc_z(z),
        .I_current(current),.I_temp_valid(temp_valid),.I_acc_valid(acc_valid),
        .I_current_valid(current_valid),.I_faults(faults),.I_feature_valid(feature_valid),
        .I_features(features),.I_feature_flags(flags),.O_enable(enable),.O_irq_enable(irq_enable),
        .O_soft_reset(soft_reset),.O_irq(irq),.O_alarm(alarm),
        // 0xA8~0xB4 / 块完成中断端口：本 TB 只覆盖 0x00~0xA4，输入固定 0。
        .I_dma_blk_seq(32'd0),.I_dma_block_done(1'b0),.I_pl_ts(32'd0),
        // v1.2 转速窗口（§14）：本 TB 用常量覆盖只读回读与 RO 写拒绝。
        .I_speed_raw(32'hfffffb2e),.I_speed_rpm(32'hffffe9d2),
        .I_speed_status(32'd35),.I_speed_count(32'd7),.I_speed_ts(32'd99),
        .I_speed_seq(32'd5),.I_speed_err_count(32'd2));
    always @(posedge clk) if(rstn) begin
        if(irq) pulses=pulses+1;
        if(soft_reset) resets=resets+1;
        if(irq && last_irq) $fatal(1,"IRQ wider than one cycle");
        last_irq=irq;
    end
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
    task event_pulse;
        begin @(negedge clk);done=1;@(negedge clk);done=0;repeat(3) @(negedge clk);end
    endtask
    initial begin
        repeat(4) @(negedge clk);rstn=1;
        expect_reg(0,32'h504c0103);expect_reg(4,0);expect_reg(8,0);expect_reg(12,0);expect_reg(16,0);
        expect_reg(16'h4c,0);expect_reg(16'h50,0);
        // 转速窗口（§14）：0xB8~0xD0 只读，读值直接来自输入且按 signed32 / u32 原样回读。
        expect_reg(16'hb8,32'hfffffb2e);expect_reg(16'hbc,32'hffffe9d2);
        expect_reg(16'hc0,32'd35);expect_reg(16'hc4,32'd7);
        expect_reg(16'hc8,32'd99);expect_reg(16'hcc,32'd5);expect_reg(16'hd0,32'd2);
        for(i=16'hb8;i<=16'hd0;i=i+4) write_reg(i[15:0],32'hffffffff,15,0,2);
        // signed32 系数全字节掩码，AW/W 顺序与独立存储。
        for(c=0;c<2;c=c+1) begin
            model=0;
            for(i=0;i<16;i=i+1) begin
                datum=32'hfedcba98 ^ (i*32'h1020304);strobe=i;
                mask={{8{strobe[3]}},{8{strobe[2]}},{8{strobe[1]}},{8{strobe[0]}}};
                model=(model & ~mask)|(datum & mask);
                write_reg(16'h4c+c*4,datum,strobe,(i%3)-1,0);
                expect_reg(16'h4c+c*4,model);
            end
        end
        write_reg(16'h4c,32'hffff8000,15,0,0);
        write_reg(16'h50,32'h80000001,15,0,0);
        expect_reg(16'h4c,32'hffff8000);expect_reg(16'h50,32'h80000001);
        model=0;
        for(i=16'h78;i<=16'h88;i=i+4) begin
            expect_reg(i[15:0],0);write_reg(i[15:0],32'hffffffff,15,0,2);
        end
        read_reg(16'h7d,2,rd);
        // 禁止采集时，原始数据与完成事件不更新。
        sample_valid=1;sample=16'h8123;done=1;@(negedge clk);sample_valid=0;done=0;
        expect_reg(12,0);expect_reg(16,0);
        write_reg(4,1,1,-1,0);check(enable && !irq_enable,"CTRL enable");
        @(negedge clk);sample_valid=1;temp_valid=1;acc_valid=1;current_valid=1;
        sample=16'h8123;temp=-125;x=-32768;y=55;z=-77;current=-100;faults=8'h96;
        @(negedge clk);sample_valid=0;temp_valid=0;acc_valid=0;current_valid=0;
        expect_reg(12,32'h00018123);expect_reg(32,32'hffffff83);
        expect_reg(36,32'hffff8000);expect_reg(40,55);expect_reg(44,32'hffffffb3);
        expect_reg(48,32'hffffff9c);expect_reg(52,32'h00010001);expect_reg(56,1);expect_reg(60,8'h96);
        event_pulse;expect_reg(16,1);check(pulses==0,"masked IRQ retained");
        write_reg(4,5,1,1,0);check(pulses==1,"unmask pending IRQ");
        event_pulse;check(pulses==1,"coalesce pending events");
        write_reg(16,1,0,0,0);expect_reg(16,1);
        write_reg(16,1,1,0,0);expect_reg(16,0);
        event_pulse;check(pulses==2,"rearm IRQ");
        // clear 与 event 同拍，事件置位优先且重新产生脉冲。
        fork
            write_reg(16,1,1,0,0);
            begin wait(dut.S_write);@(negedge clk);done=1;@(negedge clk);done=0;end
        join
        expect_reg(16,1);check(pulses==3,"set dominates IRQ clear");
        overflow=1;@(negedge clk);overflow=0;expect_reg(8,2);
        fork
            write_reg(8,2,1,0,0);
            begin wait(dut.S_write);@(negedge clk);overflow=1;@(negedge clk);overflow=0;end
        join
        expect_reg(8,2);write_reg(8,2,1,0,0);expect_reg(8,0);
        // WSTRB 与 AW/W 顺序穷举；上半字节写使能不改变 16 bit 阈值。
        for(i=0;i<64;i=i+1) begin
            datum=32'h13579bdf ^ (i*32'h1020304);strobe=i%16;
            mask={{8{strobe[3]}},{8{strobe[2]}},{8{strobe[1]}},{8{strobe[0]}}};
            model=((model & ~mask)|(datum & mask)) & 65535;
            write_reg(16'h44,datum,strobe,(i%3)-1,0);expect_reg(16'h44,model);
        end
        write_reg(4,0,2,0,0);expect_reg(4,5);
        write_reg(0,0,15,0,2);expect_reg(0,32'h504c0103);
        write_reg(16'h45,1,15,0,2);read_reg(16'h49,2,rd);read_reg(16'h1040,2,rd);
        // DATA.CNT 在连续 65536 个样本后按 16 bit 回绕。
        @(negedge clk);sample_valid=1;sample=16'h2468;
        repeat(65536) @(negedge clk);
        sample_valid=0;expect_reg(12,32'h00012468);
        // 阈值严格大于；零禁用；负温度与 abs(-32768)。
        write_reg(16,1,1,0,0);
        write_reg(16'h40,16'hff9c,3,0,0);write_reg(16'h44,32767,3,0,0);write_reg(16'h48,200,3,0,0);
        @(negedge clk);temp=-100;x=-32767;y=0;z=0;current=-200;
        temp_valid=1;acc_valid=1;current_valid=1;
        @(negedge clk);temp_valid=0;acc_valid=0;current_valid=0;expect_reg(16,0);
        @(negedge clk);temp=-99;temp_valid=1;@(negedge clk);temp_valid=0;
        expect_reg(16,1);write_reg(16,1,1,0,0);
        @(negedge clk);x=-32768;acc_valid=1;@(negedge clk);acc_valid=0;
        expect_reg(16,1);write_reg(16,1,1,0,0);
        @(negedge clk);current=-201;current_valid=1;@(negedge clk);current_valid=0;
        expect_reg(16,1);write_reg(16,1,1,0,0);
        write_reg(16'h40,0,3,0,0);write_reg(16'h44,0,3,0,0);write_reg(16'h48,0,3,0,0);
        @(negedge clk);temp=32767;x=-32768;current=-32768;
        temp_valid=1;acc_valid=1;current_valid=1;
        @(negedge clk);temp_valid=0;acc_valid=0;current_valid=0;expect_reg(16,0);
        write_reg(16'h44,model,3,0,0);
        // 四通道全部字段、序号、flags 与快照一致性。
        @(negedge clk);
        for(c=0;c<4;c=c+1) for(j=0;j<14;j=j+1) features[c*448+j*32 +:32]=32'h12340000+c*256+j;
        flags=8'he4;feature_valid=15;@(negedge clk);feature_valid=0;
        expect_reg(24,15);expect_reg(16'h100,0);
        write_reg(20,1,1,0,0);
        for(c=0;c<4;c=c+1) begin
            for(j=0;j<14;j=j+1) expect_reg(16'h100+c*64+j*4,32'h12340000+c*256+j);
            expect_reg(16'h138+c*64,1);expect_reg(16'h13c+c*64,c);
        end
        @(negedge clk);features=~features;feature_valid=15;@(negedge clk);feature_valid=0;
        expect_reg(16'h100,32'h12340000);expect_reg(16'h138,1);
        // 同拍 snapshot/valid 必须含新特征。
        fork
            write_reg(20,1,1,0,0);
            begin wait(dut.S_write);@(negedge clk);features=0;features[31:0]=32'hfeedbeef;feature_valid=1;
                @(negedge clk);feature_valid=0;end
        join
        expect_reg(16'h100,32'hfeedbeef);expect_reg(16'h138,3);
        // 对齐帧：真实 100 MHz/1 kHz，RO、符号扩展与帧时间戳一致。
        for(i=16'h54;i<=16'h74;i=i+4) expect_reg(i[15:0],0);
        @(negedge clk);temp=-125;x=-2;y=21;z=-45;current=-100;
        temp_valid=1;acc_valid=1;current_valid=1;expected_stamp=dut.S_ts;
        @(negedge clk);temp_valid=0;acc_valid=0;current_valid=0;
        wait(dut.S_frame_ts!=0);@(negedge clk);
        read_reg(16'h54,0,frame_before);
        expect_reg(16'h58,32'hffffff83);expect_reg(16'h5c,32'hfffffffe);
        expect_reg(16'h60,21);expect_reg(16'h64,32'hffffffd3);expect_reg(16'h68,32'hffffff9c);
        expect_reg(16'h6c,expected_stamp);expect_reg(16'h70,expected_stamp);expect_reg(16'h74,expected_stamp);
        expect_reg(16'h54,frame_before);
        for(i=16'h54;i<=16'h74;i=i+4) write_reg(i[15:0],32'hffffffff,15,0,2);
        read_reg(16'h55,2,rd);read_reg(16'h8c,2,rd);
        // R 背压跨越 tick：已发响应保持，后续首尾 FRAME_TS 识别跨帧。
        wait(dut.G_local_frame.X_timebase.r_div==99998);
        read_reg(16'h54,0,rd);check(rd===frame_before,"R response holds across tick");
        expect_reg(16'h54,frame_before+1);
        // 特征快照操作不冻结帧；新 valid 帧前不污染已发布的通道时间戳。
        @(negedge clk);current=-32768;current_valid=1;expected_stamp=dut.S_ts;
        @(negedge clk);current_valid=0;
        expect_reg(16'h68,32'hffffff9c);
        write_reg(20,1,1,0,0);
        frame_before=dut.S_frame_ts;
        wait(dut.S_frame_ts!=frame_before);@(negedge clk);
        expect_reg(16'h68,32'hffff8000);expect_reg(16'h70,expected_stamp);
        // 禁止跨 tick 保持帧，时基运行；重新使能后跳到当前毫秒数。
        write_reg(4,0,1,0,0);frame_before=dut.S_frame_ts;ts_before=dut.S_ts;
        @(negedge clk);temp=999;temp_valid=1;
        repeat(100010) @(negedge clk);temp_valid=0;
        check(dut.S_ts>ts_before,"disabled timebase runs");
        expect_reg(16'h54,frame_before);expect_reg(16'h58,32'hffffff83);
        write_reg(4,5,1,0,0);
        wait(dut.S_frame_ts!=frame_before);@(negedge clk);
        expect_reg(16'h58,32'hffffff83);
        // CTRL.RST 自清，清数据/特征/IRQ；阈值保留。
        write_reg(4,7,1,0,0);check(resets==1,"soft reset single pulse");
        expect_reg(4,5);expect_reg(12,0);expect_reg(16,0);expect_reg(16'h100,0);
        expect_reg(16'h138,0);expect_reg(16'h44,model);
        for(i=16'h54;i<=16'h74;i=i+4) expect_reg(i[15:0],0);
        check(dut.S_ts!=0 && dut.S_ms!=0,"soft reset preserves timebase");
        expect_reg(16'h4c,32'hffff8000);expect_reg(16'h50,32'h80000001);
        // AXI 部分事务复位：清除已缓存 AW，后续 W 不能产生幽灵响应。
        send_aw(4);rstn=0;repeat(3) @(negedge clk);rstn=1;
        expect_reg(16'h4c,0);expect_reg(16'h50,0);
        send_w(1,1);repeat(5) @(negedge clk);check(!bvalid,"reset flushes partial write");
        send_aw(4);while(!bvalid) @(negedge clk);bready=1;@(negedge clk);bready=0;
        expect_reg(4,1);
        // v1.3 alarm: AW/W order, byte enables, read-only status and reset.
        write_reg(16'hd4,8,1,-1,0); expect_reg(16'hd4,8);
        check(alarm,"speed alarm starts high");
        write_reg(16'hd4,0,2,1,0); expect_reg(16'hd4,8);
        check(alarm,"upper-byte strobe cannot clear");
        write_reg(16'hd4,32'h30,1,1,0); expect_reg(16'hd4,32'h30);
        read_reg(16'hd8,0,rd); check(rd[2:0]==7,"model preempts via AXI");
        write_reg(16'hd8,0,15,0,2);
        write_reg(16'hd4,0,1,0,0); check(!alarm,"healthy clears output");
        write_reg(16'hd4,1,1,0,0); write_reg(4,7,1,0,0);
        check(!alarm,"soft reset clears output"); expect_reg(16'hd4,0);
        $display("RESULT tb_axi_lite_reg PASS=%0d FAIL=0 IRQ=%0d",checks,pulses);$finish;
    end
    initial begin #10000000;$fatal(1,"AXI timeout");end
endmodule
