`timescale 1ns/1ps
module tb_temp_test;
    reg clk = 0;
    always #5 clk = !clk;
    wire tx;
    wire [3:0] led;
    temp_uart_source source(.O_tx(tx));
    temp_test #(.VALID_LED_MS(1)) dut (
        .I_sysclk(clk), .I_uart_rx(tx), .O_led(led)
    );
    integer passes = 0, core_bytes = 0, core_frames = 0, core_errors = 0;
    integer debug_bytes = 0, debug_frames = 0, debug_errors = 0;
    integer before_frames, before_errors, before_bytes;
    reg last_byte_valid = 0, last_temp_valid = 0;
    time last_capture = 0;
    always @(negedge dut.S_rstn) last_capture = 0;
    always @(posedge clk) begin
        if (!dut.S_rstn) begin
            last_byte_valid <= 0;
            last_temp_valid <= 0;
        end else begin
            if (dut.S_uart_byte_vld && last_byte_valid)
                $fatal(1, "core byte-valid pulse is longer than one clock");
            if (dut.S_temp_vld && last_temp_valid)
                $fatal(1, "core temperature-valid pulse is longer than one clock");
            last_byte_valid <= dut.S_uart_byte_vld;
            last_temp_valid <= dut.S_temp_vld;
            if (dut.S_uart_byte_vld) core_bytes = core_bytes + 1;
            if (dut.S_temp_vld) core_frames = core_frames + 1;
            if (dut.S_uart_error) core_errors = core_errors + 1;
        end
    end
    // Sample exactly as a CWC clocked at 50 MHz would sample these probes.
    always @(posedge dut.X_chipwatcher.clk) begin
        if (dut.S_rstn) begin
            if (last_capture != 0 && $time - last_capture != 20)
                $fatal(1, "CWC capture period is not 20 ns");
            last_capture = $time;
            if (dut.S_uart_byte_vld_cwc) begin
                debug_bytes = debug_bytes + 1;
                if (dut.X_chipwatcher.probe2 !== dut.S_uart_byte)
                    $fatal(1, "CWC byte does not match its valid event");
            end
            if (dut.X_chipwatcher.probe1) begin
                debug_frames = debug_frames + 1;
                if (dut.X_chipwatcher.probe0 !== dut.S_temp_x10)
                    $fatal(1, "CWC temperature does not match its valid event");
                if (dut.X_chipwatcher.probe3 !== 3'd0)
                    $fatal(1, "CWC successful temperature update has a nonzero error code");
            end
            if (dut.S_uart_error_cwc) debug_errors = debug_errors + 1;
        end
    end
    task check;
        input condition;
        input [8*112-1:0] label;
        begin
            if (condition !== 1'b1) $fatal(1, "FAIL: %0s", label);
            passes = passes + 1;
            $display("PASS: %0s", label);
        end
    endtask
    task expect_frame;
        input [71:0] text;
        input signed [15:0] expected;
        input integer bit_ns;
        begin
            before_frames = core_frames;
            source.send_frame(text, bit_ns, 0);
            #1000;
            check(core_frames == before_frames + 1 && dut.S_temp_x10 === expected,
                  "UART frame produces one correct signed sample");
            check(led[1] && led[2] && !led[3],
                  "successful frame lights status/sample LEDs and clears error LED");
        end
    endtask
    initial begin
        wait(dut.S_rstn); wait(dut.S_uart_ready); #113;
        check(!led[1] && !led[2] && !led[3] && dut.S_frame_count == 0,
              "power-on starts without a valid sample or error");
        expect_frame({"A+23.5B", 8'h0d, 8'h0a}, 235, 8681);
        expect_frame({"A-05.2B", 8'h0d, 8'h0a}, -52, 8681);
        expect_frame({"A+00.0B", 8'h0d, 8'h0a}, 0, 8681);
        expect_frame({"A-00.0B", 8'h0d, 8'h0a}, 0, 8681);
        expect_frame({"A+99.9B", 8'h0d, 8'h0a}, 999, 8681);
        expect_frame({"A-99.9B", 8'h0d, 8'h0a}, -999, 8681);
        expect_frame({"A+12.3B", 8'h0d, 8'h0a}, 123, 8507);
        expect_frame({"A-45.6B", 8'h0d, 8'h0a}, -456, 8855);
        before_frames = core_frames;
        source.send_frame({"A+01.2B", 8'h0d, 8'h0a}, 8681, 0);
        source.send_frame({"A-03.4B", 8'h0d, 8'h0a}, 8681, 0);
        #1000;
        check(core_frames == before_frames + 2 && dut.S_temp_x10 == -34,
              "back-to-back 8N1 frames need no extra byte gap");
        before_errors = dut.S_error_count; before_frames = core_frames;
        source.send_frame({"A+12x3B", 8'h0d, 8'h0a}, 8681, 0); #1000;
        check(core_frames == before_frames && dut.S_temp_x10 == -34 &&
              dut.S_error_count == before_errors + 1 && dut.S_error_code == 3,
              "bad ASCII grammar preserves temperature and records format error");
        check(!led[1] && led[3], "malformed frame changes the status and error LEDs");
        expect_frame({"A+02.5B", 8'h0d, 8'h0a}, 25, 8681);
        before_errors = dut.S_error_count; before_frames = core_frames;
        source.send_byte("A", 8681, 0); source.send_byte("+", 8681, 0);
        source.send_byte("2", 8681, 0); #2100000;
        check(core_frames == before_frames && dut.S_error_code == 4 &&
              dut.S_error_count == before_errors + 1 && dut.S_temp_x10 == 25,
              "missing frame tail reaches the real 2 ms inter-byte timeout");
        expect_frame({"A+03.0B", 8'h0d, 8'h0a}, 30, 8681);
        before_errors = dut.S_error_count;
        source.send_byte("A", 8681, 0); source.send_byte("-", 8681, 0);
        expect_frame({"A+04.0B", 8'h0d, 8'h0a}, 40, 8681);
        check(dut.S_error_count == before_errors + 1,
              "a new header recovers from a partial frame over UART");
        before_errors = dut.S_error_count; before_bytes = core_bytes;
        source.send_byte("A", 8681, 1); #20000;
        check(core_bytes == before_bytes && dut.S_error_code == 1 &&
              dut.S_error_count == before_errors + 1 && dut.S_temp_x10 == 40,
              "bad physical stop bit cannot create a byte or temperature");
        wait(dut.S_uart_ready);
        expect_frame({"A-01.0B", 8'h0d, 8'h0a}, -10, 8681);
        before_bytes = core_bytes;
        source.send_break(8681); #1000;
        check(core_bytes == before_bytes && dut.S_error_code == 2 &&
              dut.S_temp_x10 == -10 && led[3],
              "BREAK is detected and retains the last valid sample");
        wait(dut.S_uart_ready);
        expect_frame({"A+25.0B", 8'h0d, 8'h0a}, 250, 8681);
        before_errors = dut.S_error_count; #3000000;
        check(led[1] && !led[2] && !led[3] && dut.S_error_count == before_errors,
              "default disabled stream timeout permits unknown reporting intervals");
        // Reset midway through a frame, between bytes while the UART is idle.
        source.send_byte("A", 8681, 0); source.send_byte("-", 8681, 0);
        @(negedge clk); force dut.r_reset_count = 0;
        #1000; @(negedge clk); release dut.r_reset_count;
        wait(dut.S_rstn); wait(dut.S_uart_ready); #113;
        source.send_byte("0", 8681, 0); source.send_byte("5", 8681, 0);
        source.send_byte(".", 8681, 0); source.send_byte("2", 8681, 0);
        source.send_byte("B", 8681, 0); source.send_byte(8'h0d, 8681, 0);
        source.send_byte(8'h0a, 8681, 0); #1000;
        check(!dut.S_seen_frame && dut.S_frame_count == 0 && dut.S_temp_x10 == 0,
              "reset drops a partial frame and ignores its tail");
        expect_frame({"A-05.2B", 8'h0d, 8'h0a}, -52, 8681);
        check(dut.S_frame_count == 1, "top-level frame counter resets and resumes");
        #1000;
        check(debug_bytes == core_bytes && debug_frames == core_frames &&
              debug_errors == core_errors,
              "50 MHz CWC capture sees every byte, sample, and UART error exactly once");
        $display("RESULT tb_temp_test: PASS=%0d FAIL=0 bytes=%0d frames=%0d debug_bytes=%0d debug_frames=%0d",
                 passes, core_bytes, core_frames, debug_bytes, debug_frames);
        $finish;
    end
    initial begin
        if ($test$plusargs("vcd")) begin
            $dumpfile("tb_temp_test.vcd"); $dumpvars(0, tb_temp_test);
        end
        #25000000; $fatal(1, "integrated temperature test timeout");
    end
endmodule
