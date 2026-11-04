`timescale 1ns/1ps
// rpm_milli = trunc_toward_zero(raw * 60,000,000 / (CPR * SAMPLE_MS)).
// raw is the signed 32-bit count delta reported by registers 0x0020..0x0021.
// A 64-cycle restoring divider avoids a long combinational division path.
module encoder_rpm #(
    parameter integer COUNTS_PER_REV = 4096,
    parameter integer SAMPLE_MS = 100
)(
    input wire clk, input wire rst_n,
    input wire signed [31:0] raw, input wire raw_valid,
    output reg signed [31:0] rpm_milli,
    output reg rpm_valid, output reg overflow, output reg busy
);
    localparam [63:0] DENOM = 64'd1 * COUNTS_PER_REV * SAMPLE_MS;
    wire [31:0] magnitude = raw[31] ? (~raw + 32'd1) : raw;
    reg negative;
    reg [63:0] dividend, remainder, quotient;
    reg [6:0] count;
    wire [64:0] trial = {remainder,dividend[63]};
    wire take = trial >= {1'b0,DENOM};
    wire [63:0] next_q = {quotient[62:0],take};
    always @(posedge clk) begin
        if (!rst_n) begin
            rpm_milli <= 0; rpm_valid <= 0; overflow <= 0; busy <= 0;
            negative <= 0; dividend <= 0; remainder <= 0; quotient <= 0; count <= 0;
        end else begin
            rpm_valid <= 0;
            if (!busy) begin
                if (raw_valid) begin
                    dividend <= {32'd0,magnitude} * 64'd60000000;
                    remainder <= 0; quotient <= 0; count <= 0;
                    negative <= raw[31]; busy <= 1;
                end
            end else begin
                dividend <= {dividend[62:0],1'b0};
                remainder <= take ? trial[63:0] - DENOM : trial[63:0];
                quotient <= next_q; count <= count+1'b1;
                if (count == 63) begin
                    busy <= 0; rpm_valid <= 1; overflow <= 0;
                    if ((!negative && next_q > 64'd2147483647) ||
                        (negative && next_q > 64'd2147483648)) begin
                        rpm_milli <= negative ? 32'sh80000000 : 32'sh7fffffff;
                        overflow <= 1;
                    end else rpm_milli <= negative ? (~next_q[31:0]+32'd1) : next_q[31:0];
                end
            end
        end
    end
endmodule
