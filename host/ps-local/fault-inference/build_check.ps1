# 桌面构建：编译引擎 DLL 与检查程序，用于在 PC 上回放冻结 test 集核对数值。
# 需要 MinGW g++（PATH 中可用）。产物写入 build-desktop/。
param(
    [string]$Output = "$PSScriptRoot\build-desktop"
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Output | Out-Null
g++ -O2 -std=c++17 -shared -o "$Output\libmlk_fault.dll" "$PSScriptRoot\engine\fault_engine.cpp"
g++ -O2 -std=c++17 -o "$Output\fault_check.exe" "$PSScriptRoot\check\fault_check.cpp"
Write-Output "built: $Output\libmlk_fault.dll $Output\fault_check.exe"
