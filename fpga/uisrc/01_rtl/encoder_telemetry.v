`timescale 1ns/1ps
// Read-only PL UART telemetry; does not use the PS.
// 20 bytes: A5 5A 01 flags count[31:0] raw[31:0] rpm_milli[31:0]
//           error exception crc_lo crc_hi. Integers are big-endian.
// CRC16/Modbus covers bytes 0..17. flags: online, overflow, error_event.
module encoder_telemetry #(
    parameter integer CLK_HZ=100_000_000,
    parameter integer BAUD=115200
)(
    input wire clk, input wire rst_n, input wire event_valid,
    input wire online, input wire overflow, input wire error_event,
    input wire [31:0] sample_count, input wire [31:0] raw_speed,
    input wire [31:0] rpm_milli, input wire [3:0] error_code,
    input wire [7:0] exception_code, output wire tx
);
    reg [143:0] snapshot;
    reg [15:0] crc;
    reg [4:0] index;
    reg active, waiting, tx_start;
    reg [7:0] tx_data;
    wire tx_busy, tx_done;
    wire [7:0] next_byte = index < 18 ? snapshot[143:136] :
                                         (index == 18 ? crc[7:0] : crc[15:8]);
    function [15:0] crc_byte;
        input [15:0] c0; input [7:0] data;
        reg [15:0] c; integer k;
        begin
            c=c0^{8'd0,data};
            for(k=0;k<8;k=k+1) c=c[0] ? (c>>1)^16'ha001 : c>>1;
            crc_byte=c;
        end
    endfunction
    encoder_uart #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) uart (
        .clk(clk),.rst_n(rst_n),.tx_start(tx_start),.tx_data(tx_data),
        .tx(tx),.tx_busy(tx_busy),.tx_done(tx_done),
        .rx(1'b1),.rx_enable(1'b0),.rx_data(),.rx_valid(),.rx_error(),.rx_busy(),.rx_sync()
    );
    always @(posedge clk) begin
        if (!rst_n) begin
            snapshot <= 0; crc <= 16'hffff; index <= 0;
            active <= 0; waiting <= 0; tx_start <= 0; tx_data <= 0;
        end else begin
            tx_start <= 0;
            if (!active) begin
                if (event_valid) begin
                    snapshot <= {8'ha5,8'h5a,8'h01,{5'd0,error_event,overflow,online},
                                 sample_count,raw_speed,rpm_milli,{4'd0,error_code},exception_code};
                    crc <= 16'hffff; index <= 0; active <= 1; waiting <= 0;
                end
            end else if (!waiting && !tx_busy) begin
                tx_data <= next_byte; tx_start <= 1; waiting <= 1;
                if (index < 18) begin
                    crc <= crc_byte(crc,next_byte); snapshot <= {snapshot[135:0],8'd0};
                end
            end else if (waiting && tx_done) begin
                waiting <= 0;
                if (index == 19) active <= 0;
                else index <= index+1'b1;
            end
        end
    end
endmodule
