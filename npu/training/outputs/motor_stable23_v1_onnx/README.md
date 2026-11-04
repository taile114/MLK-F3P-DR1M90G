# FKP-DVGT optimized FP32 ONNX

rul_fp32.onnx is the verified fixed-input ONNX model after ORT basic graph optimization. Only standard ONNX operators remain; the source model is recorded in optimization_report.json.

Input: float32 [1,23,110]. Output: float32 [1,1], measured in observed aging-test sessions. Normalization is external. The prepared replay binary is already normalized.

All 165 verification windows were compared with the original PyTorch CPU output. The artifact remains FP32; quantization and actual F3P/Alnpu execution are not validated. f3p_parser_static_check.json contains static evidence only, not an operator-support guarantee.
