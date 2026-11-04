`timescale 1ns/1ps
// Accelerated system clock (1 MHz), unchanged 100 kHz physical I2C speed.
// Faults are injected at the slave pins, except explicit busy watchdog tests.
module tb_ads1115_faults;
    reg clk = 0;
    always #500 clk = !clk;
    tri scl, sda;
    pullup(scl);
    pullup(sda);
    wire [3:0] led;
    reg ack_enable = 1, nack_read = 0, corrupt = 0;
    reg [3:0] nack_byte = 4'hf;
    reg [15:0] sample = 16'hfedc;
    // Differential input permits a negative conversion code.
    ads1115_test #(
        .CLK_FREQ_HZ(1_000_000), .MUX(3'b000),
        .INIT_MS(1), .RETRY_MS(2)
    ) dut (
        .I_sysclk(clk), .IO_iic_scl(scl), .IO_iic_sda(sda), .O_led(led)
    );
    wire [31:0] slave_nacks;
    ads1115_i2c_slave_model #(
        .CLK_FREQ_HZ(1_000_000), .CONVERSION_US(500)
    ) slave (
        .I_clk(clk), .I_rstn(dut.S_rstn), .I_scl(scl), .IO_sda(sda),
        .I_sample(sample), .I_ack_enable(ack_enable), .I_nack_byte(nack_byte),
        .I_nack_read_address(nack_read), .I_corrupt_config(corrupt),
        .I_stall_conversion(1'b0), .O_start_count(), .O_stop_count(),
        .O_write_count(), .O_read_count(), .O_conversion_count(),
        .O_master_ack_count(), .O_master_nack_count(),
        .O_nack_count(slave_nacks), .O_last_config_write()
    );
    integer passes = 0, valid_count = 0, saved_valid, byte_number;
    reg previous_valid = 0;
    always @(posedge clk) begin
        if (!dut.S_rstn) begin
            valid_count = 0;
            previous_valid <= 0;
        end else begin
            if (dut.S_data_vld && previous_valid) $fatal(1, "valid pulse too long");
            previous_valid <= dut.S_data_vld;
            if (dut.S_data_vld) valid_count = valid_count + 1;
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
    task reset_case;
        begin
            @(negedge clk);
            force dut.S_rstn = 1'b0;
            repeat (10) @(negedge clk);
            release dut.S_rstn;
            wait(dut.S_rstn);
        end
    endtask
    task await_error;
        input [2:0] expected_code;
        begin
            wait(dut.S_error);
            @(negedge clk);
            check(dut.S_error_code == expected_code, "fault has the expected error code");
            check(!dut.S_dev_ok && led[3], "fault clears config status and lights error LED");
            check(valid_count == 0 && !dut.S_data_vld, "failed transaction emits no sample");
            check(dut.S_error_count == 1, "one failed acquisition increments sticky counter once");
        end
    endtask
    task await_recovery;
        begin
            wait(dut.S_data_vld);
            @(negedge clk);
            check(dut.S_adc_raw === 16'hfedc && $signed(dut.S_adc_raw) == -292,
                  "retry yields the signed, MSB-first differential sample");
            check(!dut.S_error && dut.S_error_code == 0 && dut.S_dev_ok,
                  "successful sample clears current error and restores config status");
            check(dut.S_error_count == 1, "error history survives successful recovery");
        end
    endtask
    initial begin
        wait(dut.S_rstn);
        $display("CASE: absent device then reconnect");
        ack_enable = 0;
        await_error(1);
        check(slave_nacks > 0, "absent device actually withheld bus ACK");
        ack_enable = 1;
        await_recovery;

        // NACK the address, pointer, configuration MSB and configuration LSB.
        for (byte_number = 0; byte_number < 4; byte_number = byte_number + 1) begin
            $display("CASE: NACK write byte %0d", byte_number);
            nack_byte = byte_number;
            reset_case;
            await_error(1);
            check(slave_nacks > 0, "selected write byte was NACKed on the bus");
            nack_byte = 4'hf;
            await_recovery;
        end

        $display("CASE: repeated-START read address NACK");
        nack_read = 1;
        reset_case;
        await_error(1);
        check(slave_nacks > 0, "read address was NACKed on the bus");
        nack_read = 0;
        await_recovery;

        $display("CASE: acknowledged but incorrect configuration");
        corrupt = 1;
        reset_case;
        await_error(2);
        check(slave_nacks == 0, "configuration mismatch is distinct from ACK failure");
        corrupt = 0;
        await_recovery;

        $display("CASE: controller busy never falls");
        reset_case;
        force dut.S_iic_busy = 1'b1;
        await_error(4);
        release dut.S_iic_busy;
        await_recovery;

        $display("CASE: controller never acknowledges request acceptance");
        reset_case;
        force dut.S_iic_busy = 1'b0;
        await_error(4);
        release dut.S_iic_busy;
        await_recovery;

        $display("CASE: reset during an active transfer");
        reset_case;
        wait(dut.S_iic_busy);
        reset_case;
        @(negedge clk);
        check(dut.S_adc_raw == 0 && !dut.S_data_vld && !dut.S_error,
              "reset clears old sample and fault state");
        wait(dut.S_data_vld);
        @(negedge clk);
        check(dut.S_adc_raw === 16'hfedc && dut.S_error_count == 0,
              "fresh acquisition succeeds after reset during transfer");
        saved_valid = valid_count;
        wait(dut.S_data_vld == 0);
        wait(valid_count > saved_valid);
        check(!dut.S_error, "sampling continues after recovery");
        $display("RESULT tb_ads1115_faults: PASS=%0d FAIL=0", passes);
        $finish;
    end
    initial begin
        if ($test$plusargs("vcd")) begin
            $dumpfile("tb_ads1115_faults.vcd");
            $dumpvars(0, dut, slave);
        end
        #200000000;
        $fatal(1, "fault regression timeout: state=%0d error=%0d", dut.X_ads1115_ctrl.r_state,
               dut.S_error_code);
    end
endmodule
