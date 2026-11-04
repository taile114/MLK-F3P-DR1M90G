# 单主故障模型验证

本文记录真实共享库、AArch64 构建、开发机 Qt 集成和板端 NPU 的可复核结果。

| 文件 | 内容 |
|---|---|
| `summary.json` | 环境、产物哈希、验证范围与剩余板端验收 |
| `tensor-binding.json` / `tensor-binding.log` | 输入绑定修复库的身份、配套 SDK 异常回归与 AArch64 CPU 三窗检查 |
| `tensor-binding-board-*` | 修复库板端更新、NPU 三窗与两轮 360 窗统计/逐窗 CSV/完整日志、IRQ/CMA 和应用恢复证据 |
| `live-input.json` / `live-input-tests.log` / `live-arm-smoke.log` / `live-labeled-review.png` | 实时窗口、自动记录、带模型窗口标签的跨段回看离线回归和 ARM 初始化；该应用尚未更新到板端 |
| `binaries.sha256` / `source-sha256.json` | 三个 ARM 产物 / 构建与测试源码的身份 |
| `build-environment.txt` | 工具链、QEMU、Qt 版本与实际配套 Arm NN 文件哈希 |
| `qemu-cpu-report.json` / `qemu-cpu-predictions.csv` | AArch64 库 CPU 后端的 360 窗统计与逐窗判定 |
| `native-cpu-report.json` / `native-cpu-predictions.csv` | x86-64 数值对照 |
| `qt-eval-report.json` / `fault-tests.txt` | 真实 Qt 评估导出与集成测试结果 |
| `library-dynamic.txt` / `exports.txt` / `elf-metadata.json` | 动态依赖、四个 ABI 导出、目标架构与解释器 |
| `staging.log` | 版本暂存、上一版本、重复执行、篡改拒绝和配置优先级；其中 checksum mismatch 为预期拒绝用例 |
| `arm-smoke.log` | ARM Qt 程序 QEMU 短时启动，API socket 就绪，超时结束 |
| `evaluation-panel.png` / `window.png` | 生产主题下的评估页与 1920×1080 完整窗口 |
| `sd-deployment.json` / `sd-final-verification.log` / `sd-unmounted.log` | SD 用户态更新、14 项载荷只读读回、卡上 CPU 三窗检查与同步卸载 |

CPU 360 窗分类 360/360、联合 356/360、与 FP32 参考判定零差异。
各报告仅对应其摘要中登记的共享库 SHA-256；输入绑定修复库的验证范围单独记录于
[`tensor-binding.json`](tensor-binding.json)。
本目录证据通过 `.gitattributes` 保留原始字节，避免 Git 换行转换使回收文件的 SHA-256 失配。
Qt 测试覆盖真实评估、取消重启、导出、错误包/模型身份/等级、重复/乱序/过期结果、
实时与评估切换、等待期间取消，以及评估期间拒绝寿命回放。

QEMU 与虚拟机耗时不作为实板性能。ARM 启动检查只验证运行库解析与应用初始化；
SDK 固定字体前缀产生的提示不代表 HDMI 显示已验收，板端启动脚本另设字库路径。
修复库的 NPU 两轮 360 窗分类 360/360、联合 356/360、参考判定差异 0；四图均显示
`Alnpu / ALHardNPU`，硬 IRQ 每轮增加 1440、软 IRQ 不变，退出后 CmaFree 回到基线。
GUI 操作、双端、实采预处理、原始输出逐层对照与长期稳定性待验收，
见[剩余事项](../../../故障模型接入待完成事项.md)。

SD 卡已更新新应用、运行包与独立评估集，未格式化，启动文件和分区布局保持。
原有 RUL 与 1561 个配置/数据等文件的保留检查通过；文件系统终检与 14 项 SHA-256 读回通过。
卡上程序和依赖在 QEMU CPU 下完成三窗检查；修复库已通过串口更新至板端版本目录，
CLI NPU 验证通过，Qt 进程已恢复且 framebuffer 为 1920×1080。
