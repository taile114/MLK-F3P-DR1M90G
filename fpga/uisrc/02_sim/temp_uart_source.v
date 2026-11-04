`timescale 1ns/1ps
// Independent asynchronous UART source. No DUT divider signals are used.
module temp_uart_source(output reg O_tx = 1'b1);
    task send_byte;
        input [7:0] value;
        input integer bit_ns;
        input bad_stop;
        integer bit_index;
        begin
            O_tx = 0;
            #(bit_ns);
            for (bit_index = 0; bit_index < 8; bit_index = bit_index + 1) begin
                O_tx = value[bit_index];
                #(bit_ns);
            end
            O_tx = !bad_stop;
            #(bit_ns);
            O_tx = 1;
        end
    endtask
    task send_frame;
        input [71:0] text;
        input integer bit_ns;
        input integer gap_ns;
        integer index;
        begin
            for (index = 8; index >= 0; index = index - 1) begin
                send_byte(text[index*8 +: 8], bit_ns, 1'b0);
                #(gap_ns);
            end
        end
    endtask
    task send_break;
        input integer bit_ns;
        begin
            O_tx = 0;
            #(13 * bit_ns);
            O_tx = 1;
            #(2 * bit_ns);
        end
    endtask
endmodule
