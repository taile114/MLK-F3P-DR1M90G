// 板测顶层向量参数与期望常数，由 02_sim/gen_board_test_vectors.py 生成。
// 手动修改无效：改脚本后重新生成。期望值全部来自解析公式，非 RTL 仿真反填。

localparam integer BT_N = 256;      // 特征窗口与 FFT 长度 2**8
localparam integer BT_WLOG2 = 8;

// 向量生成参数（激励与期望同源）
localparam signed [15:0] BT_V0_ACC = 16'sd1000;
localparam signed [15:0] BT_V0_CUR = 16'sd500;
localparam signed [15:0] BT_V1_STEP = 16'sd64;
localparam signed [15:0] BT_SINE_0 = 16'sh0000;
localparam signed [15:0] BT_SINE_1 = 16'sh1619;
localparam signed [15:0] BT_SINE_2 = 16'sh1f40;
localparam signed [15:0] BT_SINE_3 = 16'sh1619;
localparam signed [15:0] BT_SINE_4 = 16'sh0000;
localparam signed [15:0] BT_SINE_5 = 16'she9e7;
localparam signed [15:0] BT_SINE_6 = 16'she0c0;
localparam signed [15:0] BT_SINE_7 = 16'she9e7;
localparam signed [15:0] BT_V3_AMP = 16'sd30000;

// 期望时域字段：组 0..3 对应 V0..V3，组 4 是 V0 电流通道；
// 字段 0..6 = DC/RMS/方差/峰峰值/峰值因子/过零率/峭度，除峰峰值为整数外均 Q16。
function [63:0] f_bt_expected;
    input [2:0] I_set;
    input [2:0] I_field;
    begin
        case ({I_set, I_field})
            6'd0: f_bt_expected = 64'd65536000;  // V0 加速度 DC
            6'd1: f_bt_expected = 64'd65536000;  // V0 加速度 RMS
            6'd2: f_bt_expected = 64'd0;  // V0 加速度 VAR
            6'd3: f_bt_expected = 64'd0;  // V0 加速度 P2P
            6'd4: f_bt_expected = 64'd65536;  // V0 加速度 CREST
            6'd5: f_bt_expected = 64'd0;  // V0 加速度 ZC
            6'd6: f_bt_expected = 64'd0;  // V0 加速度 KURT
            6'd8: f_bt_expected = 64'd534773760;  // V1 斜坡 DC
            6'd9: f_bt_expected = 64'd618108647;  // V1 斜坡 RMS
            6'd10: f_bt_expected = 64'd1465993134080;  // V1 斜坡 VAR
            6'd11: f_bt_expected = 64'd16320;  // V1 斜坡 P2P
            6'd12: f_bt_expected = 64'd113400;  // V1 斜坡 CREST
            6'd13: f_bt_expected = 64'd0;  // V1 斜坡 ZC
            6'd14: f_bt_expected = 64'd117962;  // V1 斜坡 KURT
            6'd16: f_bt_expected = 64'd0;  // V2 单音 DC
            6'd17: f_bt_expected = 64'd370732376;  // V2 单音 RMS
            6'd18: f_bt_expected = 64'd2097206034432;  // V2 单音 VAR
            6'd19: f_bt_expected = 64'd16000;  // V2 单音 P2P
            6'd20: f_bt_expected = 64'd92680;  // V2 单音 CREST
            6'd21: f_bt_expected = 64'd16191;  // V2 单音 ZC
            6'd22: f_bt_expected = 64'd98302;  // V2 单音 KURT
            6'd24: f_bt_expected = 64'd0;  // V3 方波 DC
            6'd25: f_bt_expected = 64'd1966080000;  // V3 方波 RMS
            6'd26: f_bt_expected = 64'd58982400000000;  // V3 方波 VAR
            6'd27: f_bt_expected = 64'd60000;  // V3 方波 P2P
            6'd28: f_bt_expected = 64'd65536;  // V3 方波 CREST
            6'd29: f_bt_expected = 64'd257;  // V3 方波 ZC
            6'd30: f_bt_expected = 64'd65536;  // V3 方波 KURT
            6'd32: f_bt_expected = 64'd32768000;  // V0 电流 DC
            6'd33: f_bt_expected = 64'd32768000;  // V0 电流 RMS
            6'd34: f_bt_expected = 64'd0;  // V0 电流 VAR
            6'd35: f_bt_expected = 64'd0;  // V0 电流 P2P
            6'd36: f_bt_expected = 64'd65536;  // V0 电流 CREST
            6'd37: f_bt_expected = 64'd0;  // V0 电流 ZC
            6'd38: f_bt_expected = 64'd0;  // V0 电流 KURT
            default: f_bt_expected = 64'd0;
        endcase
    end
endfunction

// 期望 FFT：peak_bin 必须精确相等；peak_power = Re²+Im² = |X[peak_bin]|²。
localparam [31:0] BT_FFT_BIN_0 = 32'd0;
localparam [63:0] BT_FFT_POW_0 = 64'd0;
localparam [31:0] BT_FFT_BIN_1 = 32'd1;
localparam [63:0] BT_FFT_POW_1 = 64'd445637648514;
localparam [31:0] BT_FFT_BIN_2 = 32'd32;
localparam [63:0] BT_FFT_POW_2 = 64'd1048576000000;
localparam [31:0] BT_FFT_BIN_3 = 32'd1;
localparam [63:0] BT_FFT_POW_3 = 64'd23905866364408;
