`timescale 1ns/1ps
// Bit-level ADS1115 model: four 16-bit registers, MSB-first transfers,
// retained pointer across repeated START, single-shot OS status and latency.
module ads1115_i2c_slave_model #(
    parameter [6:0] DEV_ADDR = 7'h48,
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer CONVERSION_US = 8000
)(
    input wire I_clk,
    input wire I_rstn,
    input wire I_scl,
    inout wire IO_sda,
    input wire [15:0] I_sample,
    input wire I_ack_enable,
    input wire [3:0] I_nack_byte,
    input wire I_nack_read_address,
    input wire I_corrupt_config,
    input wire I_stall_conversion,
    output reg [31:0] O_start_count,
    output reg [31:0] O_stop_count,
    output reg [31:0] O_write_count,
    output reg [31:0] O_read_count,
    output reg [31:0] O_conversion_count,
    output reg [31:0] O_master_ack_count,
    output reg [31:0] O_master_nack_count,
    output reg [31:0] O_nack_count,
    output reg [15:0] O_last_config_write
);
    localparam [2:0] IDLE = 0, ADDRESS = 1, POINTER = 2,
                     WRITE_DATA = 3, TRANSMIT = 4, IGNORE = 5;
    reg [15:0] regs [0:3];
    reg [2:0] state;
    reg scl_previous, sda_previous, sda_low;
    reg [3:0] rx_bits, tx_bits, byte_index;
    reg [7:0] rx_shift, write_high, tx_byte;
    reg [1:0] pointer;
    reg [15:0] tx_word, pending_sample;
    reg ack_driven, accepted, tx_low_byte, tx_released, tx_done;
    reg master_nack, converting, continuous;
    integer conversion_cycles;
    reg accept_next;

    assign IO_sda = sda_low ? 1'b0 : 1'bz;
    wire sda = IO_sda;
    wire scl_rise = !scl_previous && I_scl;
    wire scl_fall = scl_previous && !I_scl;
    wire start_event = I_scl && sda_previous && !sda;
    wire stop_event = I_scl && !sda_previous && sda;

    function [15:0] read_register;
        input [1:0] index;
        begin
            read_register = regs[index];
            if (index == 1 && I_corrupt_config)
                read_register = regs[index] ^ 16'h0200;
        end
    endfunction

    always @(posedge I_clk) begin
        if (!I_rstn) begin
            regs[0] <= 0;
            regs[1] <= 16'h8583;
            regs[2] <= 16'h8000;
            regs[3] <= 16'h7fff;
            state <= IDLE;
            scl_previous <= 1;
            sda_previous <= 1;
            sda_low <= 0;
            rx_bits <= 0;
            tx_bits <= 0;
            byte_index <= 0;
            rx_shift <= 0;
            write_high <= 0;
            tx_byte <= 0;
            pointer <= 0;
            tx_word <= 0;
            pending_sample <= 0;
            ack_driven <= 0;
            accepted <= 0;
            tx_low_byte <= 0;
            tx_released <= 0;
            tx_done <= 0;
            master_nack <= 1;
            converting <= 0;
            continuous <= 0;
            conversion_cycles <= 0;
            O_start_count <= 0;
            O_stop_count <= 0;
            O_write_count <= 0;
            O_read_count <= 0;
            O_conversion_count <= 0;
            O_master_ack_count <= 0;
            O_master_nack_count <= 0;
            O_nack_count <= 0;
            O_last_config_write <= 0;
        end else begin
            scl_previous <= I_scl;
            sda_previous <= sda;
            if (converting) begin
                conversion_cycles <= conversion_cycles + 1;
                if (conversion_cycles >=
                    CONVERSION_US * (CLK_FREQ_HZ / 1_000_000) &&
                    !I_stall_conversion) begin
                    regs[0] <= pending_sample;
                    regs[1][15] <= 1;
                    O_conversion_count <= O_conversion_count + 1;
                    if (continuous) begin
                        // Continuous mode restarts immediately at the DR rate.
                        conversion_cycles <= 0;
                        pending_sample <= I_sample;
                    end else begin
                        converting <= 0;
                    end
                end
            end
            if (start_event) begin
                O_start_count <= O_start_count + 1;
                state <= ADDRESS;
                rx_bits <= 0;
                byte_index <= 0;
                rx_shift <= 0;
                ack_driven <= 0;
                tx_released <= 0;
                tx_done <= 0;
                sda_low <= 0;
            end else if (stop_event) begin
                O_stop_count <= O_stop_count + 1;
                state <= IDLE;
                sda_low <= 0;
                ack_driven <= 0;
            end else if (scl_rise) begin
                if (state == TRANSMIT) begin
                    if (tx_released) master_nack <= sda;
                end else if (state != IDLE && state != IGNORE && rx_bits < 8) begin
                    rx_shift <= {rx_shift[6:0], sda};
                    rx_bits <= rx_bits + 1;
                end
            end else if (scl_fall) begin
                if (state == TRANSMIT) begin
                    if (tx_done) sda_low <= 0;
                    else if (tx_bits < 8) begin
                        sda_low <= !tx_byte[7 - tx_bits];
                        tx_bits <= tx_bits + 1;
                    end else if (!tx_released) begin
                        sda_low <= 0;
                        tx_released <= 1;
                    end else if (!master_nack) begin
                        O_master_ack_count <= O_master_ack_count + 1;
                        tx_low_byte <= !tx_low_byte;
                        tx_byte <= tx_low_byte ? tx_word[15:8] : tx_word[7:0];
                        sda_low <= tx_low_byte ? !tx_word[15] : !tx_word[7];
                        tx_bits <= 1;
                        tx_released <= 0;
                    end else begin
                        O_master_nack_count <= O_master_nack_count + 1;
                        tx_done <= 1;
                        sda_low <= 0;
                    end
                end else if (rx_bits == 8 && !ack_driven &&
                             state != IDLE && state != IGNORE) begin
                    accept_next = I_ack_enable && (I_nack_byte != byte_index);
                    if (state == ADDRESS)
                        accept_next = accept_next && (rx_shift[7:1] == DEV_ADDR) &&
                                      !(rx_shift[0] && I_nack_read_address);
                    accepted <= accept_next;
                    sda_low <= accept_next;
                    ack_driven <= 1;
                    if (!accept_next) O_nack_count <= O_nack_count + 1;
                end else if (ack_driven) begin
                    sda_low <= 0;
                    rx_bits <= 0;
                    ack_driven <= 0;
                    byte_index <= byte_index + 1;
                    if (!accepted) state <= IGNORE;
                    else case (state)
                        ADDRESS: begin
                            if (rx_shift[0]) begin
                                state <= TRANSMIT;
                                tx_word <= read_register(pointer);
                                tx_byte <= read_register(pointer) >> 8;
                                sda_low <= !(read_register(pointer) & 16'h8000);
                                tx_bits <= 1;
                                tx_low_byte <= 0;
                                tx_released <= 0;
                                tx_done <= 0;
                                O_read_count <= O_read_count + 1;
                            end else state <= POINTER;
                        end
                        POINTER: begin
                            pointer <= rx_shift[1:0];
                            state <= WRITE_DATA;
                        end
                        WRITE_DATA: begin
                            if (byte_index == 2) write_high <= rx_shift;
                            else if (byte_index == 3) begin
                                regs[pointer] <= {write_high, rx_shift};
                                O_write_count <= O_write_count + 1;
                                if (pointer == 1) begin
                                    O_last_config_write <= {write_high, rx_shift};
                                    // bit8 MODE: 1 = single shot, 0 = continuous.
                                    // bit15 OS: single-shot start trigger.
                                    if (write_high[0]) begin
                                        continuous <= 0;
                                        if (write_high[7] && !converting) begin
                                            regs[1][15] <= 0;
                                            converting <= 1;
                                            conversion_cycles <= 0;
                                            pending_sample <= I_sample;
                                        end
                                    end else begin
                                        continuous <= 1;
                                        regs[1][15] <= 0;
                                        converting <= 1;
                                        conversion_cycles <= 0;
                                        pending_sample <= I_sample;
                                    end
                                end
                            end
                        end
                        default: state <= IGNORE;
                    endcase
                end
            end
        end
    end
endmodule
