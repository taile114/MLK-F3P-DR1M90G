# 故障模型测试归档

当前使用 [f3p-fault-test-v2.tar.gz](f3p-fault-test-v2.tar.gz)，解压目录为 `f3p-fault-test-v2`。选定 `hybrid_split` 将四个卷积放在硬 NPU，其余算子留 CPU；[360 窗实机结果](../BOARD_TEST.md)及[机器摘要](../validation/board-v2/summary.json)可用。

| 归档 | SHA-256 | 用途 |
|---|---|---|
| `f3p-fault-test-v2.tar.gz` | `d806f5f4670e059e536d19662afe9f85ed10684e203bf4ca02c14fbfa49594df` | 已验证的四卷积 NPU 分段计划，含实际输入输出审计；默认 `hybrid_split` |
| `f3p-fault-test-v1.tar.gz` | `66ea396e2cb5dbb1e6af00e9eefc01ad23c1f0885a9753336bd59faa9cac510e` | 保留 CPU、量化及融合表达对照；融合 ReLU 数值不正确，不用于硬件交付 |

归档内容与内部校验保持冻结；当前硬件验证状态由外部板测摘要提供。板端精简 tar 不支持 `-z`，使用：

```sh
mkdir -p /mnt/fault-test
gzip -dc f3p-fault-test-v2.tar.gz | tar -xf - -C /mnt/fault-test
```
