`timescale 1ns/1ps
module tb_uart_rx;
    reg clk = 0, rstn = 0;
    always #5 clk = !clk;
    wire tx, ready, valid, error, rx_sync;
    wire [7:0] byte_value;
    wire [1:0] error_code;
    temp_uart_source source(.O_tx(tx));
    uart_rx_guard dut (
        .I_clk(clk), .I_rstn(rstn), .I_uart_rx(tx),
        .O_byte(byte_value), .O_byte_vld(valid), .O_error(error),
        .O_error_code(error_code), .O_ready(ready), .O_rx_sync(rx_sync)
    );
    integer passes = 0, bytes_received = 0, stop_errors = 0, breaks = 0;
    integer index, before_bytes, before_errors;
    reg [7:0] expected;
    reg previous_valid = 0, check_bytes = 0;
    always @(posedge clk) begin
        if (!rstn) previous_valid <= 0;
        else begin
            if (valid && previous_valid) $fatal(1, "UART valid exceeds one clock");
            previous_valid <= valid;
            if (valid) begin
                bytes_received = bytes_received + 1;
                if (check_bytes && byte_value !== expected)
                    $fatal(1, "UART byte got %h expected %h", byte_value, expected);
            end
            if (error && error_code == 1) stop_errors = stop_errors + 1;
            if (error && error_code == 2) breaks = breaks + 1;
        end
    end
    task check;
        input condition;
        input [8*96-1:0] label;
        begin
            if (condition !== 1'b1) $fatal(1, "FAIL: %0s", label);
            passes = passes + 1;
            $display("PASS: %0s", label);
        end
    endtask
    task expect_byte;
        input [7:0] value;
        input integer bit_ns;
        begin
            expected = value;
            before_bytes = bytes_received;
            source.send_byte(value, bit_ns, 0);
            #1000;
            if (bytes_received != before_bytes + 1)
                $fatal(1, "Missing or duplicated byte %h at bit_ns=%0d", value, bit_ns);
        end
    endtask
    initial begin
        #200; rstn = 1;
        wait(ready); #113;
        check_bytes = 1;
        for (index = 0; index < 256; index = index + 1) expect_byte(index, 8681);
        check(bytes_received == 256 && stop_errors == 0 && breaks == 0,
              "all 256 byte values received at 115200 baud");
        expect_byte(8'h55, 8507); expect_byte(8'h80, 8507);
        expect_byte(8'haa, 8855); expect_byte(8'h01, 8855);
        check(stop_errors == 0, "sender bit periods +/-2 percent decode correctly");
        before_bytes = bytes_received;
        source.O_tx = 0; #100; source.O_tx = 1;
        #110000;
        check(bytes_received == before_bytes && stop_errors == 0,
              "short false start produces no byte");
        check_bytes = 0;
        before_bytes = bytes_received;
        before_errors = stop_errors;
        source.send_byte(8'ha5, 8681, 1);
        #20000;
        check(bytes_received == before_bytes && stop_errors == before_errors + 1,
              "bad stop bit is rejected and reported once");
        wait(ready); check_bytes = 1;
        expect_byte(8'h41, 8681);
        check(ready, "valid byte succeeds after stop error recovery");
        check_bytes = 0;
        before_bytes = bytes_received;
        source.send_break(8681);
        check(bytes_received == before_bytes && breaks == 1,
              "continuous low is reported as BREAK without a false byte");
        wait(ready); check_bytes = 1;
        expect_byte(8'h0a, 8681);
        check(ready, "valid byte succeeds after BREAK recovery");
        check_bytes = 0;
        before_bytes = bytes_received;
        fork
            source.send_byte(8'h41, 8681, 0);
            begin #20000; rstn = 0; #1000; rstn = 1; end
        join
        #20000;
        check(bytes_received == before_bytes, "reset discards an interrupted byte");
        wait(ready); check_bytes = 1;
        expect_byte(8'h42, 8681);
        check(ready, "receiver resumes after reset during a byte");
        for (index = 0; index < 10; index = index + 1) begin
            check_bytes = 0;
            before_bytes = bytes_received;
            fork
                source.send_byte(8'h55, 8681, 0);
                begin #(index * 8681 + 4000); rstn = 0; #1000; rstn = 1; end
            join
            wait(ready); #1000;
            check(bytes_received == before_bytes, "reset in each UART bit discards the interrupted byte");
            check_bytes = 1;
            expect_byte(8'h42, 8681);
            check(ready, "each reset phase accepts the next byte after idle recovery");
        end
        $display("RESULT tb_uart_rx: PASS=%0d FAIL=0 bytes=%0d", passes, bytes_received);
        $finish;
    end
    initial begin
        if ($test$plusargs("vcd")) begin
            $dumpfile("tb_uart_rx.vcd"); $dumpvars(0, tb_uart_rx);
        end
        #30000000; $fatal(1, "UART test timeout");
    end
endmodule
