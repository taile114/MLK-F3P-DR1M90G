`timescale 1ns/1ps
// 注入采集器输出验证实际顶层特征连接；总线协议由三个通道 TB 验证。
module tb_fpga_top;
    reg clk=0;always #5 clk=~clk;
    tri sdio,scl,sda;
    wire sclk,cs;
    wire [3:0] led;
    reg acc_valid=0,current_valid=0,temp_valid=0;
    reg [31:0] last_acc_ts=0,last_current_ts=0,last_temp_ts=0;
    reg [15:0] x=0,y=0,z=0,current=0;
    integer i,c,checks=0;
    reg [3:0] seen=0;
    fpga_top dut(.I_sysclk(clk),.I_uart_rx(1'b1),.O_spi_sclk(sclk),
        .IO_spi_sdio(sdio),.O_spi_cs_n(cs),.I_int1(1'b0),.IO_iic_scl(scl),.IO_iic_sda(sda),
        .I_rs485_rx(1'b1),.O_rs485_tx(),.O_rs485_de(),.O_uart_tx(),.O_led(led));
    pullup(sdio);pullup(scl);pullup(sda);
    always @(negedge clk) if(dut.r_rstn) begin
        if(|dut.S_feature_overrun) $fatal(1,"top feature overrun");
        seen=seen | dut.S_feature_valid;
        if(led[1]!==0 || led[2]!==0) $fatal(1,"LED semantics changed");
    end
    initial begin
        force dut.S_temp_vld=temp_valid;force dut.S_temp_x10=-16'sd125;
        force dut.S_acc_vld=acc_valid;force dut.S_data_vld=current_valid;
        force dut.S_acc_x=x;force dut.S_acc_y=y;force dut.S_acc_z=z;force dut.S_adc_raw=current;
        wait(dut.r_rstn);@(negedge clk);
        for(i=0;i<256;i=i+1) begin
            if(dut.S_feature_ready!==15) $fatal(1,"top not ready");
            x=16'd17;y=-16'd29;z=16'd0;current=i%2 ? 16'h7fff : 16'h8000;
            acc_valid=1;current_valid=1;temp_valid=(i==0);
            @(negedge clk);
            last_acc_ts=dut.S_ts-1;last_current_ts=dut.S_ts-1;
            if(temp_valid) last_temp_ts=dut.S_ts-1;
            acc_valid=0;current_valid=0;temp_valid=0;
            repeat(4) @(negedge clk);
        end
        wait(seen==15);repeat(4) @(negedge clk);
        for(c=0;c<3;c=c+1) begin
            if(dut.S_features[c*448+128 +:64]!==0 || dut.S_features[c*448+384 +:64]!==0)
                $fatal(1,"top constant variance/kurtosis");
            checks=checks+2;
        end
        if($signed(dut.S_features[0 +:64])!==64'sd1114112) $fatal(1,"top X DC");
        if($signed(dut.S_features[448 +:64])!==-64'sd1900544) $fatal(1,"top Y DC");
        if(dut.S_features[1344+192 +:64]!==64'd65535) $fatal(1,"top current p2p");
        if(dut.S_feature_available!==15 || dut.S_feature_read!==32'd1114112) $fatal(1,"top feature snapshot");
        checks=checks+5;
        wait(dut.S_frame_ts==1);@(negedge clk);
        if(dut.S_frame_temp!==-16'sd125 || dut.S_frame_acc_x!==17 ||
            dut.S_frame_acc_y!==-16'sd29 || dut.S_frame_acc_z!==0 || dut.S_frame_current!==16'h7fff)
            $fatal(1,"top aligned frame values");
        checks=checks+5;
        if(dut.S_acc_ts!==last_acc_ts || dut.S_current_ts!==last_current_ts || dut.S_temp_ts!==last_temp_ts)
            $fatal(1,"top channel timestamps");
        checks=checks+3;
        if(dut.S_ts!==100000 || dut.S_ms!==1) $fatal(1,"top timebase phase");
        checks=checks+2;
        wait(dut.S_frame_ts==2);@(negedge clk);
        if(dut.S_frame_temp!==-16'sd125 || dut.S_acc_ts!==last_acc_ts || dut.S_feature_read!==32'd1114112)
            $fatal(1,"top stale frame or feature snapshot changed");
        checks=checks+3;
        $display("RESULT tb_fpga_top PASS=%0d FAIL=0",checks);$finish;
    end
    initial begin #10000000;$fatal(1,"top timeout");end
endmodule
