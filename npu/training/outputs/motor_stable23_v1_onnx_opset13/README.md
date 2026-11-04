# FKP-DVGT FP32 ONNX

rul_fp32.onnx contains the existing trained model with fixed float32 input [1,23,110] and output [1,1]. Dropout and stochastic graph sampling are disabled. Normalization is external; already normalized replay inputs must not be normalized again.

Verified on 165 real windows with PyTorch CPU and ONNX Runtime CPU (graph optimization both disabled and enabled). Exact results are in export_report.json.

RUL is measured in observed aging-test sessions, not hours. This export preserves the trained model's existing prediction errors.

F3P Arm NN parsing, Alnpu execution, quantization and board performance have not been validated. node_inventory.csv lists the operators requiring compatibility checks.
