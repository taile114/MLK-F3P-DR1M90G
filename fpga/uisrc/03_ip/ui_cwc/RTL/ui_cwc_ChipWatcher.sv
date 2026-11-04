// synthesis syn_black_box 
module ui_cwc_ChipWatcher ( 
    input [15:0] probe0, 
    input [0:0] probe1, 
    input [0:0] probe2, 
    input [15:0] probe3, 
    input [15:0] probe4, 
    input [15:0] probe5, 
    input [0:0] probe6, 
    input [0:0] probe7, 
    input [7:0] probe8, 
    input [15:0] probe9, 
    input [0:0] probe10, 
    input [0:0] probe11, 
    input [0:0] probe12, 
    input [2:0] probe13, 
    input       clk  
);
    localparam string IP_TYPE  = "ChipWatcher";
    localparam CWC_BUS_NUM     = 14;
    localparam INPUT_PIPE_NUM  = 0;
    localparam OUTPUT_PIPE_NUM = 0;
    localparam RAM_DATA_DEPTH  = 2048;
    localparam CAPTURE_CONTROL = 1;

    localparam TRIG_BIT_EXIST  = 1;
    localparam integer CWC_BUS_WIDTH   [CWC_BUS_NUM-1:0] = {16,1,1,16,16,16,1,1,8,16,1,1,1,3};
    localparam integer CWC_DATA_ENABLE [CWC_BUS_NUM-1:0] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1};    
    localparam integer CWC_TRIG_ENABLE [CWC_BUS_NUM-1:0] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1};    
endmodule



