`timescale 1ns/1ps
// Top-level test at the real 100 MHz system clock and 100 kHz I2C speed.
// Continuous mode: one config write, then one register read per data-rate
// period (DR = 860 SPS -> 1163 us).
module tb_ads1115;
    reg clk = 0;
    always #5 clk = !clk;
    tri scl, sda;
    pullup(scl);
    pullup(sda);
    wire [3:0] led;
    reg [15:0] sample = 16'h1234;
    ads1115_test dut (
        .I_sysclk(clk), .IO_iic_scl(scl), .IO_iic_sda(sda), .O_led(led)
    );
    wire [31:0] starts, stops, writes, reads, conversions, master_acks, master_nacks;
    wire [15:0] last_config;
    // Conversion completes faster than the read period so every read sees the
    // current input code.
    ads1115_i2c_slave_model #(.CONVERSION_US(100)) slave (
        .I_clk(clk), .I_rstn(dut.S_rstn), .I_scl(scl), .IO_sda(sda),
        .I_sample(sample), .I_ack_enable(1'b1), .I_nack_byte(4'hf),
        .I_nack_read_address(1'b0), .I_corrupt_config(1'b0),
        .I_stall_conversion(1'b0), .O_start_count(starts),
        .O_stop_count(stops), .O_write_count(writes), .O_read_count(reads),
        .O_conversion_count(conversions), .O_master_ack_count(master_acks),
        .O_master_nack_count(master_nacks), .O_nack_count(),
        .O_last_config_write(last_config)
    );
    localparam [15:0] CONFIG_WORD = 16'hc2e3; // OS=1, AIN0/GND, +/-4.096 V, continuous, 860 SPS
    integer passes = 0, samples = 0, scl_periods = 0;
    reg previous_valid = 0, timing_armed = 0;
    time last_rise = 0, first_start = 0, previous_vld = 0, previous_interval = 0;
    task check;
        input condition;
        input [8*96-1:0] label;
        begin
            if (condition !== 1'b1) $fatal(1, "FAIL: %0s", label);
            passes = passes + 1;
            $display("PASS: %0s", label);
        end
    endtask
    // Measure bus timing independently of the divider implementation.
    always @(posedge scl) begin
        if (timing_armed && $time - last_rise < 15000) begin
            if ($time - last_rise != 10000)
                $fatal(1, "SCL period is %0t, expected 10 us", $time-last_rise);
            scl_periods = scl_periods + 1;
        end
        if (dut.S_rstn) timing_armed = 1;
        last_rise = $time;
    end
    always @(negedge sda)
        if (scl === 1'b1 && dut.S_rstn && first_start == 0)
            first_start = $time;
    always @(posedge clk) begin
        if (dut.S_rstn) begin
            if (dut.S_data_vld && previous_valid)
                $fatal(1, "data_valid must last one system clock");
            previous_valid <= dut.S_data_vld;
            if (dut.S_data_vld) begin
                samples = samples + 1;
                if (samples > 1) begin
                    previous_interval = $time - previous_vld;
                    previous_vld = $time;
                end else begin
                    previous_vld = $time;
                end
                case (samples)
                    1: begin
                        check(dut.S_adc_raw === 16'h1234, "MSB-first positive sample 0x1234");
                        sample <= 16'h0000;
                    end
                    2: begin
                        check(dut.S_adc_raw === 16'h0000, "zero sample after a new conversion");
                        sample <= 16'h7fff;
                    end
                    3: begin
                        check(dut.S_adc_raw === 16'h7fff, "positive full-scale code");
                        check(first_start >= 10000000 && first_start < 10100000,
                              "power-up wait is 10 ms at 100 MHz");
                        check(last_config === CONFIG_WORD,
                              "A0/GND, +/-4.096 V, continuous, 860 SPS");
                        check((dut.S_config_read & 16'h7fff) === (CONFIG_WORD & 16'h7fff),
                              "configuration readback matches (OS masked)");
                        check(writes == 1, "one config write for continuous streaming");
                        check(conversions >= 3, "device keeps converting after config");
                        check(master_acks == reads && master_nacks == reads,
                              "each two-byte read ends with ACK then NACK");
                        check(starts == writes + 2 * reads && stops == writes + reads,
                              "register reads use repeated START and a single STOP");
                        check(scl_periods > 100, "100 kHz bus verified over more than 100 periods");
                        check(previous_interval > 1000000 && previous_interval < 1300000,
                              "read cadence follows the 860 SPS period (~1163 us)");
                        check(led[1] && !led[3] && dut.S_error_count == 0,
                              "config LED on, error LED off, zero errors");
                        $display("RESULT tb_ads1115: PASS=%0d FAIL=0 samples=%0d", passes, samples);
                        $finish;
                    end
                endcase
            end
        end
    end
    initial begin
        if ($test$plusargs("vcd")) begin
            $dumpfile("tb_ads1115.vcd");
            $dumpvars(0, dut, slave);
        end
        #100000000;
        $fatal(1, "top-level timeout: samples=%0d cfg=%h error=%0d", samples,
               dut.S_config_read, dut.S_error_code);
    end
endmodule
