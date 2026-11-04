`timescale 1ns/1ps
module tb_encoder_top;
    localparam BIT_CLKS=100, DEBUG_BIT=10;
    reg clk=0; always #5 clk=~clk;
    reg slave_tx=1;
    wire tx,de,debug_tx; wire [3:0] leds;
    tri sdio,scl,sda;
    pullup(sdio); pullup(scl); pullup(sda);
    // Exercise the shared production top and its unchanged temperature/vibration/
    // current LED meanings while the RS485 channel succeeds, fails and recovers.
    fpga_top #(.CLK_FREQ_HZ(960000),.ENCODER_STARTUP_MS(2),.ENCODER_POLL_MS(4),
        .ENCODER_TIMEOUT_MS(30),.DEBUG_BAUD(96000)) dut (
        .I_sysclk(clk),.I_rs485_rx(slave_tx),.O_rs485_tx(tx),.O_rs485_de(de),
        .I_uart_rx(1'b1),.O_spi_sclk(),.IO_spi_sdio(sdio),.O_spi_cs_n(),
        .I_int1(1'b0),.IO_iic_scl(scl),.IO_iic_sda(sda),
        .O_uart_tx(debug_tx),.O_led(leds));
    `include "uart_bfm.vh"
    reg [7:0] packet [0:19];
    reg [7:0] debug_byte;
    reg [15:0] packet_crc;
    integer i,b,packet_count=0, capture;
    initial capture=$fopen("out/sim/telemetry.bin","wb");
    // Independent decoder runs concurrently, including the initial start bit.
    initial forever begin
        packet_crc=16'hffff;
        for(i=0;i<20;i=i+1) begin
            @(negedge debug_tx); repeat(DEBUG_BIT+DEBUG_BIT/2) @(negedge clk);
            for(b=0;b<8;b=b+1) begin
                debug_byte[b]=debug_tx; repeat(DEBUG_BIT) @(negedge clk);
            end
            check(debug_tx,"telemetry UART stop");
            packet[i]=debug_byte; packet_crc=bfm_crc(packet_crc,debug_byte);
        end
        check(packet_crc==0,"telemetry CRC");
        for(i=0;i<20;i=i+1) $fwrite(capture,"%c",packet[i]);
        $fflush(capture);
        check({packet[0],packet[1],packet[2]}==24'ha55a01,"telemetry header/version");
        if(packet_count==0) begin
            check(packet[3]==1,"telemetry online flags");
            check({packet[4],packet[5],packet[6],packet[7]}==1,"snapshot sequence");
            check({packet[8],packet[9],packet[10],packet[11]}==1000,"snapshot raw");
            check({packet[12],packet[13],packet[14],packet[15]}==146484,"snapshot RPM milli");
            check(packet[16]==0 && packet[17]==0,"snapshot no error");
        end else if(packet_count==1) begin
            check(packet[3]==4 && packet[16]==2,"error telemetry flags/code");
            check({packet[4],packet[5],packet[6],packet[7]}==1,"error keeps last sequence");
        end else begin
            check(packet[3]==1 && packet[16]==0,"recovery telemetry");
            check({packet[12],packet[13],packet[14],packet[15]}==32'hfff6d840,"negative rpm -600000");
        end
        packet_count=packet_count+1;
    end
    integer j;
    initial begin
        force dut.S_online=1'b1;
        force dut.S_dev_ok=1'b1;
        force dut.S_ads_error=1'b0;
        receive_request(6);
        for(j=0;j<8;j=j+1) response[j]=request[j]; send_response(8);
        receive_request(3); prepare_response(1000); send_response(9);
        wait(packet_count==1);
        check(dut.S_encoder_online && dut.S_encoder_configured,"encoder online/configured");
        check(leds[3:1]==3'b011,"temperature/vibration LEDs and no current fault");
        receive_request(3); prepare_response(1); response[8]=response[8]^1; send_response(9);
        wait(packet_count==2);
        check(!dut.S_encoder_online,"encoder offline after CRC failure");
        check(leds[3:1]==3'b011,"RS485 fault preserves other sensor LEDs");
        receive_request(3); prepare_response(-4096); send_response(9);
        wait(packet_count==3);
        check(dut.S_encoder_online,"encoder recovered");
        check(leds[3:1]==3'b011,"RS485 recovery preserves other sensor LEDs");
        force dut.S_ads_error=1'b1;
        @(negedge clk);
        check(leds[3:1]==3'b111,"current fault still controls LED4");
        check(dut.S_encoder_sample_count==2 && dut.S_encoder_error_count==1,"sample/error counters");
        $display("PASS tb_encoder_top checks=%0d packets=%0d",checks,packet_count); $finish;
    end
    initial begin #10000000; $fatal(1,"watchdog top"); end
endmodule
