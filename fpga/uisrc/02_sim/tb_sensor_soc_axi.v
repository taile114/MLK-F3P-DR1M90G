`timescale 1ns/1ps
module tb_sensor_soc_axi;
    reg clk=0,sensor_clk=0,rstn=0;
    always #10 clk=~clk;
    always #5 sensor_clk=~sensor_clk;
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
    reg temp_valid=0,acc_valid=0,current_valid=0,done=0;
    reg signed [15:0] temp=-125,x=17,y=-29,z=0,current=100;
    reg [31:0] rd;
    reg [31:0] ts1,ts2;
    integer checks=0,pulses=0,i;
    sensor_soc_axi dut(.I_clk(clk),.I_rstn(rstn),.I_sysclk(sensor_clk),
        .I_uart_rx(1'b1),.I_int1(1'b0),.O_spi_sclk(sclk),.O_spi_cs_n(cs),
        .IO_spi_sdio(sdio),.IO_iic_scl(scl),.IO_iic_sda(sda),.O_led(led),
        .I_awaddr(awaddr),.I_awvalid(awvalid),.O_awready(awready),.I_wdata(wdata),
        .I_wstrb(wstrb),.I_wvalid(wvalid),.O_wready(wready),.O_bresp(bresp),.O_bvalid(bvalid),
        .I_bready(bready),.I_araddr(araddr),.I_arvalid(arvalid),.O_arready(arready),
        .O_rdata(rdata),.O_rresp(rresp),.O_rvalid(rvalid),.I_rready(rready),.O_irq(irq),
        // RS485 转速通道静止（总线空闲高电平）：本 TB 不驱动 Modbus。
        .I_rs485_rx(1'b1),.O_rs485_tx(),.O_rs485_de());
    pullup(sdio);pullup(scl);pullup(sda);
    always @(posedge clk) if(rstn && irq) pulses=pulses+1;
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
        force dut.X_capture.S_temp_x10=temp;force dut.X_capture.S_temp_vld=temp_valid;
        force dut.X_capture.S_acc_x=x;force dut.X_capture.S_acc_y=y;force dut.X_capture.S_acc_z=z;
        force dut.X_capture.S_acc_vld=acc_valid;force dut.X_capture.S_adc_raw=current;
        force dut.X_capture.S_data_vld=current_valid;
        repeat(5) @(negedge clk);rstn=1;repeat(25) @(negedge clk);
        expect_reg(0,32'h504c0103);write_reg(4,5,1,0,0);
        repeat(25) @(negedge sensor_clk);
        for(i=0;i<256;i=i+1) begin
            @(negedge sensor_clk);temp_valid=(i==0);acc_valid=1;current_valid=1;
            @(negedge sensor_clk);temp_valid=0;acc_valid=0;current_valid=0;
            repeat(399) @(negedge sensor_clk);
        end
        repeat(15000) @(negedge sensor_clk);
        expect_reg(16'h20,32'hffffff83);expect_reg(16'h24,17);
        expect_reg(16'h28,32'hffffffe3);expect_reg(16'h2c,0);expect_reg(16'h30,100);
        expect_reg(16'h34,32'h01000001);expect_reg(16'h38,256);
        expect_reg(16'h0c,32'h01000011);
        check(pulses==1,"one coalesced PS IRQ");expect_reg(16'h10,1);
        write_reg(16'h10,1,1,0,0);expect_reg(16'h10,0);
        // Frame contains acquisition-domain stamps, not AXI-clock reconstructed stamps.
        read_reg(16'h54,0,rd);check(rd!=0,"external frame published");
        expect_reg(16'h58,32'hffffff83);expect_reg(16'h5c,17);expect_reg(16'h60,32'hffffffe3);
        read_reg(16'h6c,0,rd);check(rd>90000 && rd<100000,"100 MHz acquisition timestamp");
        expect_reg(16'h18,15);write_reg(16'h14,1,1,0,0);
        expect_reg(16'h100,1114112);expect_reg(16'h140,32'hffe30000);
        expect_reg(16'h138,1);expect_reg(16'h178,1);
        expect_reg(16'h78,1);expect_reg(16'h7c,0);expect_reg(16'h80,0);expect_reg(16'h84,0);
        check(!dut.S_overflow_sync[1],"CDC no loss");
        write_reg(4,7,1,0,0);repeat(25) @(negedge clk);
        expect_reg(16'h0c,0);expect_reg(16'h18,0);expect_reg(16'h78,0);
        check(dut.X_capture.S_ts!=0,"soft reset preserves acquisition time");
        // 0xA8~0xB4：未启动环形传输时完成块数为 0、中断未使能、环大小为 0。
        expect_reg(16'hA8,32'd0);expect_reg(16'hAC,32'd0);expect_reg(16'hB4,32'd0);
        check(dut.O_dma_irq===1'b0,"no DMA IRQ while disabled");
        // 0xB0 是采集域共同时基经格雷码同步到 AXI 时钟域后的计数，随 S_ts 递增。
        read_reg(16'hB0,0,ts1);
        check(ts1!=0,"PL_TS_NOW running");
        check((ts1>=dut.X_capture.S_ts?ts1-dut.X_capture.S_ts:dut.X_capture.S_ts-ts1)<1000,
              "PL_TS_NOW tracks the acquisition timebase");
        repeat(200) @(negedge clk);
        read_reg(16'hB0,0,ts2);
        check(ts2>ts1,"PL_TS_NOW advances");
        // DMA_CTRL[3]=RING 可写可读；不写 START 时 BUSY 保持 0。
        write_reg(16'h90,32'h00000008,1,0,0);
        read_reg(16'h90,0,rd);
        check(rd[3]===1'b1 && rd[8]===1'b0,"DMA_CTRL RING readback without start");
        write_reg(16'h90,32'h0,1,0,0);
        read_reg(16'h90,0,rd);
        check(rd[3]===1'b0,"DMA_CTRL RING cleared");
        $display("RESULT tb_sensor_soc_axi PASS=%0d FAIL=0",checks);$finish;
    end
    initial begin #10000000;$fatal(1,"SoC integration timeout");end
endmodule
