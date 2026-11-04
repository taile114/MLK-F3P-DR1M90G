// Independent pin-level UART BFM: no DUT UART instances in the slave.
// The including testbench supplies clk, tx, de, slave_tx and BIT_CLKS.
reg [7:0] request [0:7];
reg [7:0] response [0:15];
integer checks=0;
function [15:0] bfm_crc;
    input [15:0] c0; input [7:0] d;
    reg [15:0] c; integer j;
    begin
        c=c0^{8'b0,d};
        for(j=0;j<8;j=j+1) c=c[0] ? (c>>1)^16'ha001 : c>>1;
        bfm_crc=c;
    end
endfunction
task check;
    input condition; input [511:0] label_text;
    begin
        checks=checks+1;
        if (condition !== 1'b1) $fatal(1,"FAIL %0s at %0t",label_text,$time);
    end
endtask
task receive_request;
    input [7:0] func;
    integer i,b; reg [7:0] value; reg [15:0] crc;
    begin
        crc=16'hffff;
        for(i=0;i<8;i=i+1) begin
            @(negedge tx);
            check(de,"DE asserted before start bit");
            repeat(BIT_CLKS/2) @(negedge clk);
            check(tx===0,"start bit");
            for(b=0;b<8;b=b+1) begin
                repeat(BIT_CLKS) @(negedge clk); value[b]=tx;
                check(de,"DE covers data bits");
            end
            repeat(BIT_CLKS) @(negedge clk);
            check(tx===1 && de,"DE covers stop midpoint");
            repeat(BIT_CLKS/2-2) @(negedge clk);
            check(de,"DE covers full stop bit");
            request[i]=value; crc=bfm_crc(crc,value);
        end
        check(crc==0,"request CRC byte order and value");
        check(request[0]==1 && request[1]==func,"request ID/function");
        if(func==6) check({request[2],request[3],request[4],request[5]}==32'h000a0064,"configure 100ms only");
        else check({request[2],request[3],request[4],request[5]}==32'h00200002,"read signed32 speed registers");
        wait(!de);
        check(tx===1,"TX idle on DE release");
        // Early encoder response exercises RX arming immediately after DE falls.
        repeat(BIT_CLKS/8) @(negedge clk);
    end
endtask
task send_byte;
    input [7:0] value; input bad_stop;
    integer b;
    begin
        slave_tx=0; repeat(BIT_CLKS) @(negedge clk);
        for(b=0;b<8;b=b+1) begin
            slave_tx=value[b]; repeat(BIT_CLKS) @(negedge clk);
        end
        slave_tx=!bad_stop; repeat(BIT_CLKS) @(negedge clk);
        slave_tx=1;
    end
endtask
task finish_response;
    input integer payload_length;
    reg [15:0] crc; integer i;
    begin
        crc=16'hffff;
        for(i=0;i<payload_length;i=i+1) crc=bfm_crc(crc,response[i]);
        response[payload_length]=crc[7:0]; response[payload_length+1]=crc[15:8];
    end
endtask
task prepare_response;
    input [31:0] raw;
    begin
        response[0]=1; response[1]=3; response[2]=4;
        response[3]=raw[31:24]; response[4]=raw[23:16];
        response[5]=raw[15:8]; response[6]=raw[7:0];
        finish_response(7);
    end
endtask
task send_response;
    input integer length;
    integer i;
    begin
        for(i=0;i<length;i=i+1) begin
            check(!de,"no master/slave contention"); send_byte(response[i],0);
        end
    end
endtask
