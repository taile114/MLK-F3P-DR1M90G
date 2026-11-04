`timescale 1ns/1ps
module tb_encoder_modbus;
    localparam CLK_HZ=960000, BIT_CLKS=100;
    reg clk=0; always #5 clk=~clk;
    reg rst_n=0, slave_tx=1;
    wire tx,de;
    wire signed [31:0] raw;
    wire sample_valid,configured,online,error_valid;
    wire [3:0] error_code;
    wire [7:0] exception_code;
    wire [31:0] sample_count,error_count;
    // Deliberate RX garbage during TX must be masked by the master.
    encoder_modbus #(.CLK_HZ(CLK_HZ),.STARTUP_MS(2),.POLL_MS(2),.TIMEOUT_MS(30)) dut (
        .clk(clk),.rst_n(rst_n),.rs485_rx(de ? 1'b0 : slave_tx),
        .rs485_tx(tx),.rs485_de(de),.raw_speed(raw),.sample_valid(sample_valid),
        .configured(configured),.online(online),.error_valid(error_valid),
        .error_code(error_code),.exception_code(exception_code),
        .sample_count(sample_count),.error_count(error_count)
    );
    `include "uart_bfm.vh"
    integer samples=0, errors=0, i, before_samples;
    reg [3:0] last_error;
    reg [31:0] last_raw;
    always @(posedge clk) if(rst_n) begin
        if(sample_valid) begin samples=samples+1; last_raw=raw; end
        if(error_valid) begin errors=errors+1; last_error=error_code; end
        if(sample_valid && error_valid) $fatal(1,"sample/error simultaneous");
    end
    task await_error;
        input [3:0] expected;
        integer old_errors,t;
        begin
            old_errors=errors; t=0;
            while(errors==old_errors && t<40000) begin @(negedge clk); t=t+1; end
            if(errors==old_errors) $display("await_error expected=%0d errors=%0d state=%0d rx_count=%0d bad=%b",expected,errors,dut.state,dut.rx_count,dut.frame_bad);
            check(errors==old_errors+1,"one bounded error event");
            check(last_error==expected,"error classification");
            check(!online,"bad frame clears online");
            check(samples==before_samples,"bad frame cannot publish sample");
        end
    endtask
    task begin_read;
        begin receive_request(3); before_samples=samples; prepare_response(32'd1000); end
    endtask
    task good_read;
        input [31:0] value;
        integer old_samples,t;
        begin
            begin_read; prepare_response(value); old_samples=samples;
            send_response(9); t=0;
            while(samples==old_samples && t<6000) begin @(negedge clk); t=t+1; end
            check(samples==old_samples+1 && last_raw===value,"valid signed32 sample");
            check(online && error_code==0 && configured,"success clears error and sets online");
        end
    endtask
    initial begin
        if($test$plusargs("vcd")) begin $dumpfile("modbus.vcd"); $dumpvars(0,tb_encoder_modbus); end
        repeat(10) @(negedge clk); rst_n=1;
        // Initialization must be acknowledged; retry CRC / echo / exceptions.
        receive_request(6); before_samples=samples;
        for(i=0;i<8;i=i+1) response[i]=request[i];
        response[7]=response[7]^8'h01; send_response(8); await_error(2);
        check(!configured,"CRC failure cannot configure");
        receive_request(6);
        for(i=0;i<6;i=i+1) response[i]=request[i];
        response[5]=99; finish_response(6); send_response(8); await_error(3);
        check(!configured,"wrong echo cannot configure");
        receive_request(6); response[0]=1; response[1]=8'h86; response[2]=2;
        finish_response(3); send_response(5); await_error(5);
        check(exception_code==2 && !configured,"initialization exception code");
        receive_request(6);
        for(i=0;i<8;i=i+1) response[i]=request[i]; send_response(8);
        wait(configured); check(samples==0,"configuration does not emit speed");
        good_read(1000); good_read(-4096); good_read(0); good_read(111612);
        begin_read; response[8]=response[8]^8'h40; send_response(9); await_error(2);
        begin_read; response[0]=2; finish_response(7); send_response(9); await_error(3);
        begin_read; response[1]=4; finish_response(7); send_response(9); await_error(3);
        begin_read; response[2]=2; finish_response(7); send_response(9); await_error(3);
        begin_read; response[1]=8'h83; response[2]=2; finish_response(3);
        send_response(5); await_error(5); check(exception_code==2,"read exception");
        begin_read; await_error(1); // disconnected slave
        begin_read; send_response(4); await_error(2); // truncated frame
        begin_read; send_response(9); send_byte(8'hff,0); await_error(7);
        begin_read;
        send_response(3); repeat(20*BIT_CLKS) @(negedge clk);
        for(i=3;i<9;i=i+1) send_byte(response[i],0);
        await_error(6); // t1.5 violated without reaching t3.5
        begin_read; send_byte(1,1); await_error(4);
        begin_read; slave_tx=0; repeat(20*BIT_CLKS) @(negedge clk);
        slave_tx=1; await_error(4);
        good_read(32'h80000000); good_read(32'h7fffffff); good_read(4096);
        check(sample_count==samples && error_count==errors,"diagnostic counters");
        // A jammed bus between polls must clear a previously online indication.
        before_samples=samples; slave_tx=0; await_error(1);
        check(!de,"master does not drive a jammed bus");
        slave_tx=1; good_read(4096);
        // Reset during TX: no retained configuration / stale samples.
        @(posedge de); repeat(200) @(negedge clk); rst_n=0;
        repeat(5) @(negedge clk); check(!de && tx,"reset drives receive/idle"); rst_n=1;
        receive_request(6);
        for(i=0;i<8;i=i+1) response[i]=request[i]; send_response(8);
        wait(configured); good_read(1234);
        check(sample_count==1 && error_count==0,"reset clears counters");
        $display("PASS tb_encoder_modbus checks=%0d samples=%0d errors=%0d",checks,samples,errors);
        $finish;
    end
    initial begin #30000000; $fatal(1,"watchdog modbus"); end
endmodule
