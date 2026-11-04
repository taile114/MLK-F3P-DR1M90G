`timescale 1ns/1ps
module tb_timebase;
    reg clk=0; always #5 clk=~clk;
    reg rstn=0,enable=1,clear=0,tv=0,av=0,cv=0;
    reg signed [15:0] temp=0,x=0,y=0,z=0,current=0;
    wire tick;
    wire [31:0] ts,ms,frame_ts,acc_ts,current_ts,temp_ts;
    wire signed [15:0] ft,fx,fy,fz,fc;
    timebase dut(.I_clk(clk),.I_rstn(rstn),.I_enable(enable),.I_clear(clear),
        .I_temp_valid(tv),.I_acc_valid(av),.I_current_valid(cv),
        .I_temp(temp),.I_acc_x(x),.I_acc_y(y),.I_acc_z(z),.I_current(current),
        .O_tick(tick),.O_ts(ts),.O_ms(ms),.O_frame_ts(frame_ts),
        .O_frame_temp(ft),.O_frame_acc_x(fx),.O_frame_acc_y(fy),.O_frame_acc_z(fz),
        .O_frame_current(fc),.O_acc_ts(acc_ts),.O_current_ts(current_ts),.O_temp_ts(temp_ts));
    integer checks=0,cycles=0,div_model=0,ticks=0,frames=0,i;
    reg [31:0] ts_model=0,ms_model=0,at_model=0,ct_model=0,tt_model=0;
    reg signed [15:0] mt=0,mx=0,my=0,mz=0,mc=0;
    reg [207:0] frame_model=0;
    reg tick_model;
    time last_tick=0;
    task check(input bit good,input [511:0] name);
        begin checks=checks+1;if(!good) $fatal(1,"FAIL %s",name);end
    endtask
    // 独立参考：先消费 valid 再组帧；逐拍比较，不依赖 DUT 分频/帧内部实现。
    always @(posedge clk) begin
        tick_model=rstn && div_model==99999;
        if(tick!==tick_model) $fatal(1,"tick phase/width cycle=%0d",cycles);
        if(!rstn) begin
            ts_model=0;ms_model=0;div_model=0;
        end
        if(!rstn || clear) begin
            mt=0;mx=0;my=0;mz=0;mc=0;at_model=0;ct_model=0;tt_model=0;frame_model=0;
        end else if(enable) begin
            if(tv) begin mt=temp;tt_model=ts_model;end
            if(av) begin mx=x;my=y;mz=z;at_model=ts_model;end
            if(cv) begin mc=current;ct_model=ts_model;end
            if(tick_model) begin
                frame_model={ms_model+32'd1,mt,mx,my,mz,mc,at_model,ct_model,tt_model};
                frames=frames+1;
            end
        end
        if(rstn) begin
            cycles=cycles+1;
            ts_model=ts_model+32'd1;
            if(tick_model) begin
                check(last_tick==0 || $time-last_tick==1000000,"tick period = 1 ms");
                last_tick=$time;ticks=ticks+1;ms_model=ms_model+32'd1;div_model=0;
            end else div_model=div_model+1;
        end else last_tick=0;
        #1;
        if(ts!==ts_model || ms!==ms_model) $fatal(1,"timebase increments/wrap");
        if({frame_ts,ft,fx,fy,fz,fc,acc_ts,current_ts,temp_ts}!==frame_model)
            $fatal(1,"frame values/stamps/hold/reset cycle=%0d",cycles);
        if({dut.r_acc_ts,dut.r_current_ts,dut.r_temp_ts}!=={at_model,ct_model,tt_model})
            $fatal(1,"valid sample timestamp cycle=%0d",cycles);
    end
    initial begin
        $dumpfile("out/sim/tb_timebase.vcd");
        $dumpvars(0,tick,frame_ts,ft,fx,fy,fz,fc,acc_ts,current_ts,temp_ts,tv,av,cv,enable,clear,rstn);
        repeat(3) @(negedge clk);rstn=1;
        // 第一帧无样本，全零；后续错相、连续 valid 和 tick 同拍 valid。
        repeat(100000) @(negedge clk);
        check(frame_ts==1 && {ft,fx,fy,fz,fc,acc_ts,current_ts,temp_ts}==0,"first empty frame at 1 ms");
        for(i=0;i<200000;i=i+1) begin
            tv=(i==11 || i==99999);av=(i%313==0 || i==99999);cv=(i%1163==0 || i==99999);
            temp=-125;x=i[15:0];y=16'h8000;z=16'h7fff;current=-i[15:0];
            @(negedge clk);
        end
        tv=0;av=0;cv=0;
        check(temp_ts==199999 && ft==-125,"same-tick temperature stamp/value");
        check(frames==3 && ticks==3,"three frames and ticks");
        // 禁止期间不接受样本或更新帧，时基继续。
        enable=0;tv=1;av=1;cv=1;temp=999;
        repeat(100000) @(negedge clk);
        check(frame_ts==3 && ts==400000 && ms==4,"disable preserves frames, not timebase");
        enable=1;tv=0;av=0;cv=0;
        repeat(100000) @(negedge clk);
        check(frame_ts==5 && ft==-125,"reenable ignores disabled samples");
        // 软清除与 valid 同拍优先，保留时基与分频相位。
        clear=1;tv=1;av=1;cv=1;
        @(negedge clk);clear=0;tv=0;av=0;cv=0;
        check(frame_ts==0 && ts==500001 && ms==5,"clear leaves timebase running");
        repeat(99999) @(negedge clk);
        check(frame_ts==6 && {ft,fx,fy,fz,fc,acc_ts,current_ts,temp_ts}==0,"cleared channels in next frame");
        // TB 定点注入计数器边界，避免仿真数十亿拍；独立参考同步设定边界。
        dut.r_ts=32'hfffffffe;ts_model=32'hfffffffe;
        dut.r_ms=32'hffffffff;ms_model=32'hffffffff;
        tv=1;av=1;cv=1;temp=-1;x=-32768;y=32767;z=-2;current=-3;
        repeat(3) @(negedge clk);
        tv=0;av=0;cv=0;
        check(ts==1 && dut.r_temp_ts==0,"cycle timestamp wraps modulo 2^32");
        repeat(99997) @(negedge clk);
        check(frame_ts==0 && ms==0 && ft==-1 && fx==-32768 && fc==-3,"millisecond/frame wrap");
        // 非 tick 相位硬复位重新起算。
        repeat(17) @(negedge clk);rstn=0;
        repeat(2) @(negedge clk);
        check(ts==0 && ms==0 && frame_ts==0,"hard reset clears timebase and frames");
        rstn=1;repeat(100000) @(negedge clk);
        check(frame_ts==1 && ts==100000,"hard reset restarts full 1 ms period");
        $display("RESULT tb_timebase PASS=%0d FAIL=0 cycles=%0d ticks=%0d frames=%0d",checks,cycles,ticks,frames);$finish;
    end
    initial begin #12000000;$fatal(1,"timebase timeout");end
endmodule
