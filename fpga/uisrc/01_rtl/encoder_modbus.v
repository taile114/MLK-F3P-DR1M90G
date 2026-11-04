`timescale 1ns/1ps
// Single-slave Modbus RTU master for OID RS485 single-turn encoder V3.08.
// Boot: FC06 writes the speed sample period. Then FC03 polls 0x0020/2.
// A complete frame is accepted only after t3.5, exact length and CRC checks.
module encoder_modbus #(
    parameter integer CLK_HZ = 100_000_000,
    parameter integer BAUD = 9600,
    parameter [7:0] SLAVE_ID = 8'd1,
    parameter integer SAMPLE_MS = 100,
    parameter integer STARTUP_MS = 1000,
    parameter integer POLL_MS = 100,
    parameter integer TIMEOUT_MS = 100
)(
    input wire clk, input wire rst_n, input wire rs485_rx,
    output wire rs485_tx, output reg rs485_de,
    output reg signed [31:0] raw_speed, output reg sample_valid,
    output reg configured, output reg online,
    output reg error_valid, output reg [3:0] error_code,
    output reg [7:0] exception_code,
    output reg [31:0] sample_count, output reg [31:0] error_count
);
    localparam integer BIT_CLKS = (CLK_HZ+BAUD/2)/BAUD;
    localparam integer GAP_CLKS = 35*BIT_CLKS;
    // Consecutive stop-midpoint timestamps differ by 10 bits + idle gap.
    localparam integer MAX_BYTE_CLKS = 25*BIT_CLKS;
    localparam integer START_CLKS = (CLK_HZ/1000)*STARTUP_MS;
    localparam integer POLL_CLKS = (CLK_HZ/1000)*POLL_MS;
    localparam integer TIMEOUT_CLKS = (CLK_HZ/1000)*TIMEOUT_MS;
    localparam S_IDLE=0, S_SETUP=1, S_SEND=2, S_TXWAIT=3, S_RX=4, S_CHECK=5;
    // Error codes: 1 timeout, 2 CRC, 3 structure/address/function/echo,
    // 4 UART framing, 5 Modbus exception, 6 t1.5 gap, 7 RX overflow.
    reg [2:0] state;
    reg [31:0] wait_timer, timeout_timer, idle_timer, setup_timer, byte_timer;
    reg [3:0] tx_index, rx_count;
    reg init_request, frame_bad;
    reg [3:0] frame_error;
    reg [7:0] frame [0:8];
    reg [15:0] rx_crc;
    reg tx_start;
    reg [7:0] tx_data;
    wire tx_busy, tx_done, rx_valid, rx_error, rx_busy, rx_sync;
    wire [7:0] rx_data;
    function [15:0] crc_byte;
        input [15:0] crc; input [7:0] data;
        reg [15:0] c; integer k;
        begin
            c=crc^{8'd0,data};
            for(k=0;k<8;k=k+1) c=c[0] ? (c>>1)^16'ha001 : c>>1;
            crc_byte=c;
        end
    endfunction
    function [7:0] request_byte;
        input [3:0] index; input init;
        begin
            case(index)
                0: request_byte=SLAVE_ID;
                1: request_byte=init ? 8'h06 : 8'h03;
                2: request_byte=0;
                3: request_byte=init ? 8'h0a : 8'h20;
                4: request_byte=init ? SAMPLE_MS[15:8] : 8'd0;
                5: request_byte=init ? SAMPLE_MS[7:0] : 8'h02;
                default: request_byte=0;
            endcase
        end
    endfunction
    function [15:0] request_crc;
        input init;
        reg [15:0] c; integer k;
        begin
            c=16'hffff;
            for(k=0;k<6;k=k+1) c=crc_byte(c,request_byte(k[3:0],init));
            request_crc=c;
        end
    endfunction
    wire [15:0] tx_crc = request_crc(init_request);
    encoder_uart #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) uart (
        .clk(clk),.rst_n(rst_n),.tx_start(tx_start),.tx_data(tx_data),
        .tx(rs485_tx),.tx_busy(tx_busy),.tx_done(tx_done),
        .rx(rs485_rx),.rx_enable(!rs485_de),.rx_data(rx_data),
        .rx_valid(rx_valid),.rx_error(rx_error),.rx_busy(rx_busy),.rx_sync(rx_sync)
    );
    task fail;
        input [3:0] code;
        begin
            error_valid <= 1; error_code <= code; error_count <= error_count+1'b1;
            if (code != 5) exception_code <= 0;
            online <= 0; wait_timer <= POLL_CLKS-1; timeout_timer <= 0; state <= S_IDLE;
        end
    endtask
    always @(posedge clk) begin
        if (!rst_n) begin
            state <= S_IDLE; rs485_de <= 0; tx_start <= 0; tx_data <= 0;
            wait_timer <= START_CLKS; timeout_timer <= 0; idle_timer <= 0; setup_timer <= 0;
            tx_index <= 0; rx_count <= 0; init_request <= 1;
            frame_bad <= 0; frame_error <= 0; rx_crc <= 16'hffff; byte_timer <= 0;
            raw_speed <= 0; sample_valid <= 0; configured <= 0; online <= 0;
            error_valid <= 0; error_code <= 0; exception_code <= 0;
            sample_count <= 0; error_count <= 0;
        end else begin
            tx_start <= 0; sample_valid <= 0; error_valid <= 0;
            if (rs485_de || !rx_sync || rx_busy || rx_valid || rx_error) idle_timer <= 0;
            else if (idle_timer < GAP_CLKS) idle_timer <= idle_timer+1'b1;
            case(state)
                S_IDLE: begin
                    rs485_de <= 0;
                    if (wait_timer != 0) begin
                        wait_timer <= wait_timer-1'b1; timeout_timer <= 0;
                    end
                    else if (idle_timer >= GAP_CLKS) begin
                        init_request <= !configured; rs485_de <= 1;
                        setup_timer <= BIT_CLKS-1; tx_index <= 0; state <= S_SETUP;
                    end else if (timeout_timer >= TIMEOUT_CLKS-1) fail(1);
                    else timeout_timer <= timeout_timer+1'b1;
                end
                S_SETUP: if (setup_timer != 0) setup_timer <= setup_timer-1'b1;
                    else state <= S_SEND;
                S_SEND: if (!tx_busy) begin
                    tx_data <= tx_index < 6 ? request_byte(tx_index,init_request) :
                               (tx_index == 6 ? tx_crc[7:0] : tx_crc[15:8]);
                    tx_start <= 1; state <= S_TXWAIT;
                end
                S_TXWAIT: if (tx_done) begin
                    if (tx_index == 7) begin
                        // tx_done follows the full stop bit; release DE immediately.
                        rs485_de <= 0; rx_count <= 0; rx_crc <= 16'hffff;
                        frame_bad <= 0; frame_error <= 0;
                        timeout_timer <= 0; byte_timer <= 0; state <= S_RX;
                    end else begin tx_index <= tx_index+1'b1; state <= S_SEND; end
                end
                S_RX: begin
                    timeout_timer <= timeout_timer+1'b1;
                    if (byte_timer < TIMEOUT_CLKS) byte_timer <= byte_timer+1'b1;
                    if (rx_error) begin frame_bad <= 1; frame_error <= 4; end
                    if (rx_valid) begin
                        byte_timer <= 0;
                        if (rx_count != 0 && byte_timer > MAX_BYTE_CLKS) begin
                            frame_bad <= 1; frame_error <= 6;
                        end
                        if (rx_count < 9) begin
                            frame[rx_count] <= rx_data; rx_count <= rx_count+1'b1;
                            rx_crc <= crc_byte(rx_crc,rx_data);
                        end else begin frame_bad <= 1; frame_error <= 7; end
                    end
                    if ((rx_count != 0 || frame_bad) && idle_timer >= GAP_CLKS && !rx_busy && !rx_valid)
                        state <= S_CHECK;
                    else if (timeout_timer >= TIMEOUT_CLKS-1) fail(1);
                end
                S_CHECK: begin
                    if (frame_bad) fail(frame_error);
                    else if (rx_crc != 0) fail(2);
                    else if (frame[0] != SLAVE_ID) fail(3);
                    else if (rx_count == 5 && frame[1] == (init_request ? 8'h86 : 8'h83)) begin
                        exception_code <= frame[2]; fail(5);
                    end else if (init_request) begin
                        if (rx_count != 8 || frame[1] != 6 || frame[2] != 0 || frame[3] != 10 ||
                            frame[4] != request_byte(4,1) || frame[5] != request_byte(5,1)) fail(3);
                        else begin
                            configured <= 1; wait_timer <= POLL_CLKS-1;
                            error_code <= 0; exception_code <= 0; state <= S_IDLE;
                        end
                    end else if (rx_count != 9 || frame[1] != 3 || frame[2] != 4) fail(3);
                    else begin
                        raw_speed <= {frame[3],frame[4],frame[5],frame[6]}; sample_valid <= 1;
                        sample_count <= sample_count+1'b1; online <= 1;
                        error_code <= 0; exception_code <= 0;
                        wait_timer <= POLL_CLKS-1; state <= S_IDLE;
                    end
                end
                default: begin state <= S_IDLE; rs485_de <= 0; end
            endcase
        end
    end
endmodule
