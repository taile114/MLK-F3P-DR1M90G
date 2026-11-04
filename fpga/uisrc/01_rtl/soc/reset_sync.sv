module reset_sync(
    input logic I_clk, I_arstn,
    output wire O_rstn
);
    (* ASYNC_REG="TRUE" *) logic [1:0] S_pipe;
    always_ff @(posedge I_clk or negedge I_arstn)
        if (!I_arstn) S_pipe<=0;
        else S_pipe<={S_pipe[0],1'b1};
    assign O_rstn=S_pipe[1];
endmodule
