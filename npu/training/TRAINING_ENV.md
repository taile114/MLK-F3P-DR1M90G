# Python 训练与导出环境

本目录依赖 Python 3.12；电脑训练使用 NVIDIA CUDA，导出和板端权重准备可以使用 CPU。

## 已验证环境

| 项目 | 记录 |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 Laptop，8 GB |
| NVIDIA 驱动 | 610.62 |
| 系统 CUDA Toolkit | 12.5，nvcc 12.5.82 |
| Python / PyTorch | 3.12.14 / 2.8.0+cu126 |
| PyTorch 自带 CUDA | 12.6 |
| NumPy / ONNX / ORT | 2.3.5 / 1.19.1 / 1.23.2 |

预编译 PyTorch wheel 使用自带 CUDA 运行库；本模型无自定义 CUDA 扩展，不使用系统 nvcc 编译。环境表是本次已验证记录，不作为其他电脑已满足要求的证明。

## 新建环境

在本目录执行，先确保 `py -3.12` 指向实际 Python 3.12 安装；若未注册该版本，使用实际 Python 3.12 的完整路径替代首条命令。

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install torch==2.8.0 --index-url https://download.pytorch.org/whl/cu126
.\.venv\Scripts\python.exe -m pip install -r requirements-export.txt --index-url https://pypi.tuna.tsinghua.edu.cn/simple
.\.venv\Scripts\python.exe -m pip check
```

普通 Python 库使用清华镜像；CUDA PyTorch 单独使用官方 cu126 源。`requirements-training.txt`、`requirements-export.txt` 固定直接依赖，`.lock.txt` 保存已用完整版本。不需要 torchvision、torchaudio 或额外 CUDA Toolkit。

已有工作区环境为 `E:\competion\code\fkp-dvgt\.venv\Scripts\python.exe`，可直接调用该解释器运行本仓库工具，无需复制虚拟环境。

只读检查版本和 CUDA 可用性：

```powershell
.\.venv\Scripts\python.exe -c "import torch; print(torch.__version__); print(torch.version.cuda); print(torch.cuda.is_available())"
```

训练、评估和导出命令见 [README](README.md)。板端程序不依赖 Python 或 CUDA。
