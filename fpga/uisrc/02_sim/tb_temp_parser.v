`timescale 1ns/1ps
module tb_temp_parser;
    reg clk = 0, rstn = 0, byte_valid = 0, uart_error = 0;
    reg [7:0] byte_value = 0;
    reg [1:0] uart_error_code = 0;
    always #5 clk = !clk;
    wire signed [15:0] temperature;
    wire valid, seen, online, error;
    wire [2:0] error_code;
    wire [15:0] error_count;
    wire [31:0] frame_count;
    temp_frame_parser #(.BYTE_TIMEOUT_US(30), .STREAM_TIMEOUT_MS(1)) dut (
        .I_clk(clk), .I_rstn(rstn), .I_byte(byte_value), .I_byte_vld(byte_valid),
        .I_uart_error(uart_error), .I_uart_error_code(uart_error_code),
        .O_temp_x10(temperature), .O_temp_vld(valid), .O_seen_frame(seen),
        .O_online(online), .O_error(error), .O_error_code(error_code),
        .O_error_count(error_count), .O_frame_count(frame_count)
    );
    integer passes = 0, valid_count = 0, before_valid, before_errors, index;
    reg previous_valid = 0;
    reg [71:0] bad_frame;
    always @(posedge clk) begin
        if (!rstn) begin valid_count = 0; previous_valid <= 0; end
        else begin
            if (valid && previous_valid) $fatal(1, "temperature valid exceeds one clock");
            previous_valid <= valid;
            if (valid) valid_count = valid_count + 1;
        end
    end
    task check;
        input condition;
        input [8*96-1:0] label;
        begin
            if (condition !== 1'b1) $fatal(1, "FAIL: %0s", label);
            passes = passes + 1; $display("PASS: %0s", label);
        end
    endtask
    task send_byte;
        input [7:0] value;
        begin
            @(negedge clk); byte_value = value; byte_valid = 1;
            @(negedge clk); byte_valid = 0;
            repeat (2) @(negedge clk);
        end
    endtask
    task send_frame;
        input [71:0] text;
        integer i;
        begin for (i = 8; i >= 0; i = i - 1) send_byte(text[i*8 +: 8]); end
    endtask
    task expect_frame;
        input [71:0] text;
        input signed [15:0] expected;
        begin
            before_valid = valid_count;
            send_frame(text);
            check(valid_count == before_valid + 1 && temperature === expected,
                  "complete frame yields exactly one correct signed temperature");
            check(online && seen && !error && error_code == 0,
                  "valid frame establishes online status and clears current error");
        end
    endtask
    initial begin
        #100; rstn = 1;
        send_byte(8'h00); send_byte(8'h5a); send_byte(8'h0a);
        check(!seen && valid_count == 0 && error_count == 0,
              "noise before the first A is ignored");
        expect_frame({"A+23.5B", 8'h0d, 8'h0a}, 235);
        expect_frame({"A-05.2B", 8'h0d, 8'h0a}, -52);
        expect_frame({"A+00.0B", 8'h0d, 8'h0a}, 0);
        expect_frame({"A-00.0B", 8'h0d, 8'h0a}, 0);
        expect_frame({"A+99.9B", 8'h0d, 8'h0a}, 999);
        expect_frame({"A-99.9B", 8'h0d, 8'h0a}, -999);
        for (index = 1; index < 9; index = index + 1) begin
            bad_frame = {"A+12.3B", 8'h0d, 8'h0a};
            bad_frame[(8-index)*8 +: 8] = 8'h78;
            before_valid = valid_count; before_errors = error_count;
            send_frame(bad_frame);
            check(valid_count == before_valid && temperature == -999 &&
                  error_code == 3 && error_count == before_errors + 1,
                  "illegal sign/digit/dot/B/CR/LF preserves the last good sample");
            // Keep the same last good value for the next malformed frame.
            expect_frame({"A-99.9B", 8'h0d, 8'h0a}, -999);
        end
        before_errors = error_count; before_valid = valid_count;
        send_byte("A"); send_byte("+"); send_byte("2");
        #32000;
        check(valid_count == before_valid && error_code == 4 &&
              error_count == before_errors + 1, "truncated frame times out once");
        expect_frame({"A+01.0B", 8'h0d, 8'h0a}, 10);
        before_errors = error_count;
        send_byte("A"); send_byte("-"); send_byte("3");
        expect_frame({"A+12.3B", 8'h0d, 8'h0a}, 123);
        check(error_count == before_errors + 1, "new A resynchronizes a partial frame");
        send_byte("A"); send_byte("+");
        @(negedge clk); uart_error = 1; uart_error_code = 1;
        @(negedge clk); uart_error = 0;
        check(temperature == 123 && error_code == 1 && !online,
              "UART stop error invalidates the partial frame");
        expect_frame({"A-01.0B", 8'h0d, 8'h0a}, -10);
        @(negedge clk); uart_error = 1; uart_error_code = 2;
        @(negedge clk); uart_error = 0;
        check(temperature == -10 && error_code == 2, "UART BREAK preserves the last temperature");
        expect_frame({"A+25.0B", 8'h0d, 8'h0a}, 250);
        #1002000;
        check(error_code == 5 && !online && temperature == 250,
              "enabled stream watchdog marks old data offline");
        before_errors = error_count; #1100000;
        check(error_count == before_errors, "stream timeout is reported once per outage");
        expect_frame({"A+25.1B", 8'h0d, 8'h0a}, 251);
        send_byte("A"); send_byte("-");
        @(negedge clk); rstn = 0;
        repeat (3) @(negedge clk); rstn = 1;
        send_byte("0"); send_byte("5"); send_byte("."); send_byte("2");
        send_byte("B"); send_byte(8'h0d); send_byte(8'h0a);
        check(!seen && temperature == 0 && frame_count == 0,
              "reset drops an unfinished frame and its remaining tail");
        expect_frame({"A-05.2B", 8'h0d, 8'h0a}, -52);
        check(frame_count == 1, "frame counter resets and resumes on valid frames");
        $display("RESULT tb_temp_parser: PASS=%0d FAIL=0", passes);
        $finish;
    end
    initial begin
        if ($test$plusargs("vcd")) begin
            $dumpfile("tb_temp_parser.vcd"); $dumpvars(0, tb_temp_parser);
        end
        #4000000; $fatal(1, "parser test timeout");
    end
endmodule
