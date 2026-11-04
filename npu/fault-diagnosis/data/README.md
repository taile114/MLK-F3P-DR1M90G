# 故障诊断数据使用说明

本文规定原生数据选择、状态对应关系、读取方式、标签解释、切窗与训练缓存准备方法。原始模拟故障标注为 `bench_fault_sim_v2`，当前单主类训练任务为 `bench_fault_single_v1`；输入 `base6` 为 XYZ 加速度、电流、转速、外壳温度，电脑端使用 PyTorch。

当前单故障训练缓存为 [active_selection_single_v1](../model/cache/active_selection_single_v1/dataset_manifest.json)：train 1063 窗、valid 360 窗、test 360 窗。正常与四类故障互斥分类，只返回一个最可能的主故障及其分级，正常为 0 级。每份覆盖全部 26 段及两次采集背景，实际比例约 59.6%/20.2%/20.2%。已完成 CUDA 25 轮开发训练；入口、权重及结果见 [单故障训练与推断](../model/单故障训练与推断.md)。

物理特征来源为 [active_selection_exploratory_v2](../model/cache/active_selection_exploratory_v2/dataset_manifest.json)，单主类缓存仅转换监督与版本清单，不重新切窗或拟合标准化。主类标签见 [primary_labels.csv](../model/cache/active_selection_single_v1/primary_labels.csv)，源多标签中的未知信息仍保存在来源字段中。

快速配置使用四秒有效上下文、原生 DMA 块频谱及训练侧正常水平参考。机械故障的原始 logits 和等级头不读取温度；五类 softmax 竞争仍允许温度升高时过热成为最高类别。test 是现有记录内部的开发留出，不等同独立重采，连续波形配置的质量结果另行保留。

## 1. 当前读取入口

训练侧使用 [reviewed/active_selection_v2/](reviewed/active_selection_v2/)；标签由操作者明确确认文件名类别和等级，规则为 `operator_filename_grade_v1`。目录已包含以下产物：

| 文件 | 用途 |
|---|---|
| [episodes.jsonl](reviewed/active_selection_v2/episodes.jsonl)、[labels.csv](reviewed/active_selection_v2/labels.csv) | 26 段训练侧唯一 ID、四类目标、监督掩码与标签依据 |
| [operator_confirmation.json](reviewed/active_selection_v2/operator_confirmation.json) | 文件名等级和暖机正常、水平放置的确认范围 |
| [base6.reviewed.yaml](reviewed/active_selection_v2/base6.reviewed.yaml) | 独立准备配置；姿态校准和待定门限保留为空 |
| [split-map.json](reviewed/active_selection_v2/split-map.json)、[split_review.json](reviewed/active_selection_v2/split_review.json) | 连续波形正式配置的共享历史分组；均为训练候选，没有独立验证/测试划分 |
| [split_blocks.csv](../model/cache/active_selection_exploratory_v2/split_blocks.csv) | 快速训练使用的三份时间块划分，每源片段三个 train 块、一个 valid 块、一个 test 块 |
| [native_4s_windows.csv](reviewed/active_selection_v2/native_4s_windows.csv) | 覆盖全部 26 段的 3939 个四秒候选，其中 3266 个位于单一源文件；另附缺口、重复时间戳 |
| [source_integrity.json](reviewed/active_selection_v2/source_integrity.json)、[preparation_manifest.json](reviewed/active_selection_v2/preparation_manifest.json) | 原件哈希、来源和准备状态 |

原生来源选择由以下三份文件固定，不遍历全部 NPZ：

| 文件 | 用途 |
|---|---|
| [segments/active_selection_20261031/segments.csv](segments/active_selection_20261031/segments.csv) | 当前选中的 26 个 NPZ；包含分段唯一 ID、目标状态、时间范围、路径与哈希 |
| [segments/active_selection_20261031/episodes.jsonl](segments/active_selection_20261031/episodes.jsonl) | 对应的四类标签、掩码、依据、采集分组与质量信息；每行一个分段对象 |
| [segments/active_selection_20261031/selection_manifest.json](segments/active_selection_20261031/selection_manifest.json) | 选择版本、来源批次、数量和校验信息 |

当前包含 **24 段故障：4 类×3 级×2 组，以及 2 段已确认正常的暖机运行片段**。两段暖机来自同一次记录，不是两次独立实验。原生选择清单保留采集时的候选状态；训练侧派生清单使用 `operator_confirmed_quality_pending`，四类故障各有 6 段启用分类和分级监督。标签确认不自动完成仪器校准和独立实验验收；连续波形正式配置没有合格完整窗口，单独的短上下文探索配置已有 1783 个训练/验证/测试缓存。

归档中还有另一次下午采集的正常暖机段：[session_001/normal_warmup.npz](segments/session_001/normal_warmup.npz)，时间为 10/29 15:20:01.643～15:37:28.467。它与傍晚采集是两次分开的记录，尚未加入当前 26 段选择。当前所称“同一次暖机的两段”仅指傍晚 18:49:48.713～18:57:44.949、18:57:58.041～19:01:36.793 两个子片段，中间排除了约 13.092 秒；不是指下午与傍晚。按已有正常片段计，归档覆盖两次采集、三个正常暖机片段，不能将傍晚两个子片段各自计为独立暖机重复。

`segments.csv` 中的 `npz_path` 相对本文所在的 **data 根目录**解析，不相对 CSV 目录解析。CSV 的 `episode_id` 是分段唯一 ID，对应 JSONL 的 **`segment_id`**；JSONL 中 `episode_id` 表示父事件，暖机两段共享该值，不能据此构造唯一分段字典。

当前选择排除了原来缺少 XYZ 的两组轻度欠压，改用晚间补采；原件没有删除，见 [excluded_superseded.csv](segments/active_selection_20261031/excluded_superseded.csv)。不要同时读取原批次索引与当前索引，否则会重复计数或重新纳入失效片段。

## 2. 目录与文件分工

| 路径或文件 | 内容 | 训练用途 |
|---|---|---|
| `raw/` | 只读归档的加密 MLKC、导入清单 | 来源与哈希追溯；训练循环不解密、不读取口令 |
| `decoded/` | 认证解密后的 `.plain` 二进制载荷 | 重建原生数据；不是 CSV 或 NumPy 文件 |
| `processed/batch_20261030/` | 原批次文件时间索引、块表和核验信息 | 源文件映射、速率、诊断计数和质量核验 |
| `processed/batch_20261031_supplement/` | 补采文件索引、块表和核验信息 | 同上 |
| `segments/batch_20261030/` | 原批次按状态裁切的原生 NPZ 与同名 JSON | 仅按当前选择清单读取其中有效条目 |
| `segments/batch_20261031_supplement/` | 两组轻度欠压和第二组重度过载 | 当前补采来源 |
| `segments/active_selection_20261031/` | 跨批次选择和标签清单 | 当前入口；不包含重复复制的波形 |
| `reviewed/active_selection_v2/` | 操作者确认的派生标签、源块适配、窗口候选和审查结果 | 训练准备入口；原始选择与波形不覆盖 |
| `segments/session_001/`、`segments/session_001_v2/` | 首次自然升温及其派生标签 | 单独的早期数据集；不默认加入当前 26 段 |
| `*_overview_1s.csv`、`overview_1s.csv` | 检查用低速统计 | 查看曲线；不能作为 kHz 振动训练输入 |
| `excluded_context/` | 未进入主索引的短上下文 | 不自动加入训练 |

`batch_20261031_supplement` 是导入批次名称，其采集日期实际为 10/30。故障时间使用载荷内部估算 UTC，不由目录名、文件创建时间或日期字符串推断；本文状态表显示 UTC+8。

## 3. 每段数据对应的状态

`g1/g2/g3` 表示操作者确认的轻/中/重模拟档位；`r01/r02` 表示记录分组；`part01/part02` 表示同一事件中拆开的运行区间。当前文件名由操作者授权作为目标类别和等级的标注载体，转换器校验名称、原清单和源文件一致性，并保存独立标签；文件名不进入模型输入。具体掩码见第 5 节。

| NPZ 文件 | 状态分组 | UTC+8 起止时间 | 时长/秒 |
|---|---|---|---:|
| [warmup_g0_r01_part01.npz](segments/batch_20261030/warmup_g0_r01_part01.npz) | 正常暖机 / 四类均正常 | 2026-10-29 18:49:48.713～18:57:44.949 | 476.236 |
| [warmup_g0_r01_part02.npz](segments/batch_20261030/warmup_g0_r01_part02.npz) | 正常暖机 / 四类均正常 | 2026-10-29 18:57:58.041～19:01:36.793 | 218.752 |
| [overload_g1_r01_part01.npz](segments/batch_20261030/overload_g1_r01_part01.npz) | 过载 / 轻度 | 2026-10-29 19:58:10.018～19:59:59.956 | 109.938 |
| [overload_g1_r02_part01.npz](segments/batch_20261030/overload_g1_r02_part01.npz) | 过载 / 轻度 | 2026-10-29 20:12:10.077～20:13:59.996 | 109.919 |
| [overload_g2_r01_part01.npz](segments/batch_20261030/overload_g2_r01_part01.npz) | 过载 / 中度 | 2026-10-29 20:25:10.113～20:28:59.943 | 229.830 |
| [overload_g2_r02_part01.npz](segments/batch_20261030/overload_g2_r02_part01.npz) | 过载 / 中度 | 2026-10-29 20:43:10.011～20:45:59.955 | 169.944 |
| [overload_g3_r01_part01.npz](segments/batch_20261030/overload_g3_r01_part01.npz) | 过载 / 重度 | 2026-10-29 21:00:10.009～21:01:59.962 | 109.953 |
| [overload_g3_r02_part01.npz](segments/batch_20261031_supplement/overload_g3_r02_part01.npz) | 过载 / 重度 | 2026-10-30 21:38:47.395～21:40:59.888 | 132.493 |
| [undervoltage_g1_replacement_r01_part01.npz](segments/batch_20261031_supplement/undervoltage_g1_replacement_r01_part01.npz) | 欠压 / 轻度 | 2026-10-30 21:27:23.008～21:29:59.980 | 156.972 |
| [undervoltage_g1_replacement_r02_part01.npz](segments/batch_20261031_supplement/undervoltage_g1_replacement_r02_part01.npz) | 欠压 / 轻度 | 2026-10-30 21:56:46.091～21:59:59.878 | 193.787 |
| [undervoltage_g2_r01_part01.npz](segments/batch_20261030/undervoltage_g2_r01_part01.npz) | 欠压 / 中度 | 2026-10-30 12:37:10.044～12:39:59.979 | 169.935 |
| [undervoltage_g2_r02_part01.npz](segments/batch_20261030/undervoltage_g2_r02_part01.npz) | 欠压 / 中度 | 2026-10-30 12:47:10.025～12:49:59.174 | 169.149 |
| [undervoltage_g3_r01_part01.npz](segments/batch_20261030/undervoltage_g3_r01_part01.npz) | 欠压 / 重度 | 2026-10-30 12:53:10.016～12:54:59.930 | 109.914 |
| [undervoltage_g3_r02_part01.npz](segments/batch_20261030/undervoltage_g3_r02_part01.npz) | 欠压 / 重度 | 2026-10-30 13:01:10.074～13:02:59.224 | 109.150 |
| [overtemperature_g1_r01_part01.npz](segments/batch_20261030/overtemperature_g1_r01_part01.npz) | 外壳过热 / 轻度 | 2026-10-29 21:37:28.508～21:38:59.964 | 91.456 |
| [overtemperature_g1_r02_part01.npz](segments/batch_20261030/overtemperature_g1_r02_part01.npz) | 外壳过热 / 轻度 | 2026-10-29 21:40:26.384～21:41:59.947 | 93.563 |
| [overtemperature_g2_r01_part01.npz](segments/batch_20261030/overtemperature_g2_r01_part01.npz) | 外壳过热 / 中度 | 2026-10-29 21:48:27.215～21:49:59.752 | 92.537 |
| [overtemperature_g2_r02_part01.npz](segments/batch_20261030/overtemperature_g2_r02_part01.npz) | 外壳过热 / 中度 | 2026-10-29 21:51:46.490～21:52:59.946 | 73.456 |
| [overtemperature_g3_r01_part01.npz](segments/batch_20261030/overtemperature_g3_r01_part01.npz) | 外壳过热 / 重度 | 2026-10-30 18:06:36.651～18:08:59.917 | 143.266 |
| [overtemperature_g3_r02_part01.npz](segments/batch_20261030/overtemperature_g3_r02_part01.npz) | 外壳过热 / 重度 | 2026-10-30 18:19:14.808～18:21:59.901 | 165.093 |
| [mount_tilt_g1_r01_part01.npz](segments/batch_20261030/mount_tilt_g1_r01_part01.npz) | 安装倾斜 / 轻度 | 2026-10-30 16:43:44.284～16:45:59.990 | 135.706 |
| [mount_tilt_g1_r02_part01.npz](segments/batch_20261030/mount_tilt_g1_r02_part01.npz) | 安装倾斜 / 轻度 | 2026-10-30 16:53:15.011～16:55:59.973 | 164.962 |
| [mount_tilt_g2_r01_part01.npz](segments/batch_20261030/mount_tilt_g2_r01_part01.npz) | 安装倾斜 / 中度 | 2026-10-30 17:12:41.366～17:14:59.885 | 138.519 |
| [mount_tilt_g2_r02_part01.npz](segments/batch_20261030/mount_tilt_g2_r02_part01.npz) | 安装倾斜 / 中度 | 2026-10-30 17:21:38.508～17:23:59.958 | 141.450 |
| [mount_tilt_g3_r01_part01.npz](segments/batch_20261030/mount_tilt_g3_r01_part01.npz) | 安装倾斜 / 重度 | 2026-10-30 17:47:22.462～17:49:59.997 | 157.535 |
| [mount_tilt_g3_r02_part01.npz](segments/batch_20261030/mount_tilt_g3_r02_part01.npz) | 安装倾斜 / 重度 | 2026-10-30 17:57:10.937～17:59:59.973 | 169.036 |

状态表中的两段暖机经操作者确认完全正常运行、无任何故障植入，作为四类模拟故障的正常负例；两段实测温度均低于 35 ℃。当前标签补充依据见 [label_overrides.json](segments/active_selection_20261031/label_overrides.json)，只更新当前选择清单，不改写原生 NPZ 或其生成时元数据。过热按真实温区裁切；其他类别按报告的模拟工艺记录，不能将等级换算为实测力矩、母线电压百分比或真实倾角。

## 4. NPZ 内容与原生读取

每个文件是长短不一的原生片段，不是已经生成好的定长网络输入。`N` 在不同通道之间不相同；不能把同一行号的温度、电流、转速和 XYZ 当作同时测量。

| 字段 | 类型与形状 | 含义 |
|---|---|---|
| `vibration_t_us` | int64 `[Nv]` | XYZ 的估算 UTC 微秒时间戳 |
| `vibration_value` | float32 `[Nv,3]` | 物理加速度 g，列依次为 X/Y/Z，保留重力 |
| `vibration_raw` | int16 `[Nv,3]` | 原始码，供量程/饱和及源数据核验 |
| `vibration_block` | int32 `[Nv]` | 每个 XYZ 样本的源块 ID |
| `temperature_t_us/value/raw/block` | int64 / float32 / int16 / int32，各 `[Nt]` | 时间、壳温 ℃、原始码、源块 ID |
| `current_t_us/value/raw/block` | int64 / float32 / int32 / int32，各 `[Ni]` | 时间、电流 A、原始码、源块 ID |
| `speed_t_us/value/raw/block` | int64 / float32 / int32 / int32，各 `[Nn]` | 时间、转速 rpm、原始码、源块 ID |
| `metadata_json` | 零维 Unicode 字符串数组 | 生成 NPZ 时写入的分段元数据，使用 `json.loads(z['metadata_json'].item())` 读取 |

`*_value` 已应用文件内标定：电流已经是 A，不再除以 1000；温度和转速不再乘 0.1；振动不再乘 0.0039。额外校准必须有独立版本和依据。缺失与饱和另行判断，`*_raw` 不能直接替代物理量输入。

同一估算时间戳可能对应不同真实样本，不能直接按时间戳去重。保留 int64 时间戳；如需相对秒数，先做整数相减，再转浮点，避免大 UTC 数的精度损失。

下面的示例仅做原生读取、校验和标签占位，不执行特征提取或训练。保存为本目录内的 Python 文件运行；依赖 NumPy，路径也可直接指定为 data 根目录。

```python
from pathlib import Path
import csv
import hashlib
import json
import numpy as np

DATA = Path(__file__).resolve().parent
selection = DATA / 'segments/active_selection_20261031'
with (selection / 'segments.csv').open(encoding='utf-8-sig', newline='') as f:
    rows = list(csv.DictReader(f))
episodes = [json.loads(line) for line in
            (selection / 'episodes.jsonl').read_text(encoding='utf-8').splitlines()
            if line.strip()]
by_segment = {ep['segment_id']: ep for ep in episodes}
assert len(rows) == len(by_segment) == 26

# 选一个补采轻度欠压片段；整批迭代时逐文件读取，避免一次加载所有波形。
row = next(r for r in rows if r['episode_id'] ==
           'undervoltage_g1_replacement_r01_part01')
ep = by_segment[row['episode_id']]
# 同时兼容清单中的 Windows 分隔符及其他系统上的 pathlib。
path = (DATA / row['npz_path'].replace('\\', '/')).resolve()
assert path.is_relative_to(DATA.resolve())
assert hashlib.sha256(path.read_bytes()).hexdigest() == row['npz_sha256']
with np.load(path, allow_pickle=False) as z:
    arrays = {k: z[k] for k in z.files if k != 'metadata_json'}
    embedded_metadata = json.loads(z['metadata_json'].item())

t_us = arrays['vibration_t_us']
xyz_g = arrays['vibration_value']      # [Nv,3]，不是 CNN 输入形状
relative_s = (t_us - t_us[0]).astype(np.float64) / 1e6
assert xyz_g.shape == (len(t_us), 3)
assert np.all(np.diff(t_us) >= 0)

class_mask = np.asarray(ep['class_mask'], dtype=np.float32)
grade_mask = np.asarray(ep['grade_mask'], dtype=np.float32)
fault_target = np.asarray([p if m else 0 for p, m in
                           zip(ep['present'], class_mask)], dtype=np.float32)
grade_target = np.asarray([g if m else 0 for g, m in
                           zip(ep['grade'], grade_mask)], dtype=np.int64)
print(row['episode_id'], xyz_g.shape, ep['fault_order'])
print(fault_target, grade_target, class_mask, grade_mask)
```

读取原生 NPZ 不需要 PyTorch；后续合格特征缓存才转成 Tensor。`metadata_json` 和同名 JSON 保留生成时的信息；当前取舍、批次限定 session 与标签审核应使用当前选择 JSONL 及另行生成的审核清单，不依赖嵌入字符串推断最新版资格。

## 5. 类别、等级和监督掩码

类别顺序固定，所有向量均为四维：

| 下标 | 标识 | 状态 | 当前等级依据 |
|---|---|---|---|
| 0 | `overload` | 模拟过载 | 操作者确认文件名 `g1/g2/g3` 对应的负载模拟档位 |
| 1 | `undervoltage` | 模拟欠压 | 操作者确认文件名对应的降压模拟档位；没有在线电压输入 |
| 2 | `overtemperature` | 模拟外壳过热 | 故障目标等级按已确认文件名；采集温区为 35/40/45 ℃规则，物理误差核验单独保留 |
| 3 | `mount_tilt` | 安装倾斜异常 | 操作者确认文件名对应的倾斜模拟档位；不推定实测角度或与 3/6/10°边界等价 |

| 字段 | 语义 |
|---|---|
| `fault`、`operator_grade` | 本段已确认的主要模拟异常及文件名档位；非目标类别仍按各自依据和掩码处理 |
| `present[c]` | 1=报告存在，0=已有对应正常侧依据，`null`=未知 |
| `grade[c]` | 0=无该类；1/2/3=等级记录；`null`=未知。数值存在时也必须检查掩码 |
| `class_mask[c]` | 1=按声明依据允许该类别监督，0=排除；窗口测量质量单独检查 |
| `grade_mask[c]` | 1=按声明依据允许等级监督，0=排除；仅在该类已确认存在时使用 |
| `label_basis`、`label_status` | 审核依据和状态，不进入神经网络特征 |

本批补采轻度欠压的实际向量为 `present=[null,1,0,null]`、`grade=[null,1,0,null]`、`class_mask=[0,1,1,0]`、`grade_mask=[0,1,0,0]`。只监督欠压存在/一级以及温度正常侧，不把另外两类自动当负例。

六段倾斜在训练侧清单中使用 `present[3]=1,grade[3]=1/2/3,class_mask[3]=1,grade_mask[3]=1`，依据为 `operator_confirmed_filename_grade`，规则为 `operator_filename_grade_v1`。入口要求显式确认、正确的标签契约版本、源文件名与目标/掩码一致。参考角度和误差保持为空；候选几何规则只用于 `inclination_reference` 标签，两种倾斜等级依据不能在同一数据集中混合监督。

暖机两段训练侧标签为 `present=[0,0,0,0]`、`grade=[0,0,0,0]`、`class_mask=[1,1,1,1]`、`grade_mask=[0,0,0,0]`，依据为操作者确认正常电压、正常负载、电机平放且正常运行。第四类依据为 `operator_confirmed_horizontal`。两段可作为四类分类的正常负例，不监督故障三级头；其他故障片段的未确认非目标类别保持原有未知掩码。

水平正常确认支持倾斜负例，不产生数值重力校准。原生暖机观测均值与中位数保存在 [gravity_reference_observations.json](reviewed/active_selection_v2/gravity_reference_observations.json)，状态为未核验观测；零偏、增益、冻结参考仍为空。

损失先逐元素计算，再乘相应掩码并按有效元素归一化。未知目标可临时占位为 0，但掩码必须保持 0；不能让 `null` 转为 NaN 进入损失。分级有效掩码至少为 `grade_mask × class_mask × 1[present=1]`。正常 grade=0 不计入故障三级分级准确率。

## 6. 原生分段到训练窗口

本节表格为连续波形配置的时间支持。快速配置将有效状态上下文限制为四秒，较早的 56 个慢状态格标为无效；每个窗口完整落在单一数据集合的时间块内。每分钟保存造成的约一秒停采保留原始时间和缺失标记，不补造振动波形。详细规则见 [快速训练说明 §3](../model/快速训练说明.md#3-时间缺口与水平参考)。

| 分支 | 时间支持 | 要求 |
|---|---|---|
| XYZ 动态振动、PSD、统计与姿态 | 最近 4 秒 | 高频候选窗口完整放在同一源文件内；核验源块与连续性，并排除滤波初始化暂态 |
| 电流/转速快状态 | 最近 4 秒 | 按真实时间因果对齐；附有效位与样本年龄 |
| 温度/电流/转速慢状态 | 最近 60 秒 | 保存缺口仍占真实时间；完整历史或经训练验证的缺失感知策略，不能删除缺失时间 |

一个 NPZ 可能汇集多个原始文件，**NPZ 文件内部不代表连续**。不能按“每 6400 个原生点”切 4 秒：原生振动约 3.1 kHz，6400 点是经合格抗混叠/重采样到 1600 Hz 后的目标长度。

源块追溯方法：根据当前 JSONL 的 `source_batch`，读取 `processed/<source_batch>/blocks.jsonl`；NPZ 的 `*_block` 对应其中 `block_id`，块内 `file_id` 再对应同目录的 `capture_metadata.json` 或 `capture_index.csv`。块 ID 仅在批次内唯一，不能跨批次混用。

补采已提供 [same_file_4s_windows.csv](segments/batch_20261031_supplement/same_file_4s_windows.csv)，共 443 个不跨源文件的 4 秒候选，步长 1 秒。此索引只覆盖补采三段，不覆盖全部 26 段；它不是最终特征缓存或完整 60 秒输入。443 个候选均通过振动 0.5 秒粗筛，但均有诊断计数跳变，需核验 DMA 计数语义与时间重建，不能直接视作无丢样波形。

全部片段的四秒候选已在训练侧清单目录生成，并保存窗口起止、源文件 ID、片段 ID、缺口和重复时间戳；源文件哈希由适配块表追溯。完整准备器对近期四秒执行单一源文件检查。原生波形缺失不通过补零、复制末值、插值或压缩时间轴修复；慢状态可采用与训练/现场一致的因果占位、有效位和年龄，但当前严格质量规则尚未据此放宽。

姿态必须从高通前 XYZ 保留重力；动态振动再单独高通。不能在每次启动/每个窗口把 XYZ 均值归零，也不能直接用未核验的倾斜段建立水平基准。参考角度若有，仅用于参考规则标签，不能拼入模型输入；当前文件名模拟档位不生成参考角度。

同级稳态窗口要求完整 60 秒历史位于同一可信状态区间；原生片段开头不足 60 秒的部分不能从其他独立记录补满。事件训练允许历史包含前一状态，但需要单独事件标签及验收。跨保存缺口的慢历史与滤波状态必须保留因果时间定义。

## 7. 接入现有 PyTorch 工程

当前模型训练读取 `model/cache/active_selection_single_v1/`，物理特征配置为 `base6_native_blocks_short_context_v2`。先由 [exploratory.py](../model/fault_mtl/exploratory.py) 冻结物理特征与三份时间划分，再由 [prepare_single_class.py](../model/fault_mtl/prepare_single_class.py) 建立唯一主类标签；当前推断为五类 argmax 加选中类分级。以下连续波形入口和审查结果用于对照。

[model/](../model/README.md) 实现 v2 六分支网络。[prepare_selection.py](../model/fault_mtl/prepare_selection.py) 将原生选择转换为独立训练侧清单，已完成 ID/路径、标签依据、源块和哈希适配。原生选择 JSONL 保留采集格式，不直接传给模型准备器。

`data/reviewed/active_selection_v2/episodes.jsonl`、`split-map.json` 和 `base6.reviewed.yaml` 已生成，转换规则如下：

1. 逐项读取当前选择清单，将 `segment_id` 用作训练侧唯一 `episode_id`，父 `episode_id` 另行保留；保存原件/索引哈希，原始 JSONL 不覆盖。
2. 将经批次限定的源 session 和共享热历史归并为 `session_id/group_id`。当前 `split_group` 是日期级保守分组；同一暖机两段、同一连续流与其副本不得拆组。
3. `source_data` 指向该段 NPZ，路径相对新审核 JSONL 目录。`source_block_metadata` 指向适配后的源块 JSON，不直接指向原有 JSONL。
4. 块表已独立转换为 `{"blocks":[...]}`，保留真实源块 ID、速率、源文件哈希和采集身份。原有字符串通道转成数值通道；振动为 `channel=2,axes=3`，不重编号或虚构采样。
5. 标签使用 `schema=mlk.fault_training.episode.v2` 和 `label_contract_version=fault_labels_v2_operator_v1`。24 段目标故障均按已确认文件名启用分类/分级，暖机四类正常，其他掩码保留；角度和误差不由档位生成。
6. 五个 `quality_checks`：`vibration_dma/current_calibrated/timestamps_verified/range_reviewed/pose_mount_unchanged` 均保留待核验。标签确认不将这些测量质量标记改为 true。
7. 独立 `base6.reviewed.yaml` 已开启单一源文件振动检查；`gravity_reference_id`、数值校准和待定质量门限仍待真实测量与审核。
8. 完整窗口准备结果保存于 [dataset_manifest.json](../model/cache/active_selection_v2_operator/dataset_manifest.json) 和 [windows.jsonl](../model/cache/active_selection_v2_operator/windows.jsonl)：2483 个候选，0 个合格，0 个特征缓存。近期振动 20 毫秒缺口检查拒绝全部候选，2325 个还含重复估算时间戳；电流历史缺口影响 2461 个，转速历史缺口与慢状态覆盖不足影响全部候选。未拟合标准化、未生成特征或训练权重。

在 `npu/fault-diagnosis/model/` 下执行以下命令。重建输出使用新目录；正式缓存命令需在质量与校准实际补齐后使用新版本清单。

```powershell
$python = 'E:\competion\code\.venv\Scripts\python.exe'
& $python -m fault_mtl.prepare_selection `
  --data-root ../data --selection ../data/segments/active_selection_20261031 `
  --output ../data/reviewed/active_selection_v2_rebuild --operator-confirmed

& $python -m fault_mtl.prepare_dataset `
  --episodes ../data/reviewed/active_selection_v2/episodes.jsonl `
  --split-map ../data/reviewed/active_selection_v2/split-map.json `
  --config ../data/reviewed/active_selection_v2/base6.reviewed.yaml `
  --audit-only --output runs/active_selection_audit

# 审核通过且预处理配置已冻结后，使用已核验配置生成缓存。
& $python -m fault_mtl.prepare_dataset `
  --episodes ../data/reviewed/active_selection_v2/episodes.jsonl `
  --split-map ../data/reviewed/active_selection_v2/split-map.json `
  --config ../data/reviewed/active_selection_v2/base6.reviewed.yaml `
  --output cache/active_selection_v2

# 仅在缓存内存在合格训练和独立验证窗口时训练。
& $python -m fault_mtl.train --dataset cache/active_selection_v2 `
  --device cuda:0 --batch-size 32 --output runs/active_selection_v2_seed42
```

准备器产生 `windows.jsonl`、`dataset_manifest.json`、`split_manifest.csv`，有合格训练窗口时才产生 `preprocess.json` 与 `features/*.npz`。[FaultDataset](../model/fault_mtl/dataset.py) 读取该缓存目录；`--audit-only` 不生成可训练特征缓存。当前 `cache/active_selection_v2_operator` 状态为 `no_qualified_training_windows`。

当前可在模型工程目录按以下方式读取快速训练缓存：

```python
from pathlib import Path
from fault_mtl.dataset import FaultDataset

train_set = FaultDataset(Path('cache/active_selection_single_v1'), split='train')
sample = train_set[0]
for name in ('time_frequency', 'spectrum', 'vibration_stats',
             'mount_pose', 'fast_state', 'slow_state'):
    print(name, sample[name].shape)
print(sample['class_target'], sample['grade_target'])
print(sample['class_mask'], sample['grade_mask'])
```

v2 每窗特征形状为 `time_frequency[8,64,48]`、`spectrum[3,769]`、`vibration_stats[12]`、`mount_pose[8]`、`fast_state[6,40]`、`slow_state[9,60]`；批维由 DataLoader 添加。仅在训练组拟合标准化、类别权重与任何数据驱动参数，再将冻结参数用于验证/测试。v1 的 `mount_looseness` 缓存或权重不能直接作为 v2 数据/模型使用。

## 8. 划分、评估与可用范围

先分完整独立组，再切窗，不逐文件、逐行或逐窗口随机拆分。4 秒、1 秒步长的重叠窗口共享大量样本，不能分到训练与测试两侧。当前仅两个采集日期，并且不同日期的类别分布不齐；直接按日期划分也无法保证每类在训练/验证/测试均有独立覆盖。不能为凑三份数据把同一次暖机、热循环或连续采集拆开。

连续波形配置的 `split-map.json` 将两个保守日期组均保留为训练候选，没有独立验证或测试集。快速配置另用每原始片段五个时间块的 60%/20%/20% 三分；各窗口和 DMA 块不跨集合，三份都包含两次采集的温度背景。轻度和中度过热的重复记录分别共享同一采集 session，快速指标明确按记录内留出解释，不将时间块视作独立实验。

当前全部 24 段故障都是单一主要异常的操作者分组，没有完整复合故障验收集；只有两组也不代表已经验证跨日期、跨装配或跨设备能力。暖机正常状态已获操作者确认；其他类别可靠负例、传感器校准和完整历史质量仍须单独核验。当前倾斜按模拟档位训练；若验收数值角度分级，还需独立参考角度依据。90% 分类、85% 分级是目标，不是这些文件的现成属性。

原始时间与幅值保留用于追溯；训练缓存、增强和缺失策略放在独立目录，不能改写原生 NPZ、原始 MLKC 或既有哈希。说明文件可纳入版本管理，采样、解密数据与缓存不随之提交。

## 9. 相关规格与核验文件

- [模型设计](../设计方案.md)、[采集执行规程](../采集执行规程.md)、[缺口处理方案](../缺口处理方案.md)。
- [原批次说明](../本批采集数据说明.md)、[补采说明](../补采数据说明.md)、[当前模型工程说明](../model/README.md)。
- 原批次 [质量表](segments/batch_20261030/quality_summary.csv)、[窗口表](segments/batch_20261030/window_quality.csv)；其中原轻度欠压已被当前选择排除，比较时按片段 ID 关联。
- 补采 [质量表](segments/batch_20261031_supplement/quality_summary.csv)、[窗口表](segments/batch_20261031_supplement/window_quality.csv) 与 [核验结果](processed/batch_20261031_supplement/selection_verification.json)。
