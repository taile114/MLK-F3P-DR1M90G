# host/common — 传输加密共享库

供 `remote-pc` 与 `ps-local` 共用的传输加密：**ECDH 为主、PSK 为后备**，两者共存，配置切换。基于 OpenSSL（AES-256-GCM / X25519 / HKDF-SHA256）。

接口契约见 [`../../docs/上位机-PS接口约定.md`](../../docs/上位机-PS接口约定.md) §6。

## 文件

| 文件 | 作用 |
|---|---|
| `crypto.*` | 原语：随机数、SHA-256、HMAC、HKDF、X25519、AES-256-GCM `seal`/`open`、指纹 |
| `securechannel.*` | `SecureChannel`：模式配置、握手（Hello/Reply）、逐包 `seal`/`open`、抗重放 |
| `test_crypto.cpp` · `crypto_test.pro` | 自测：GCM 往返 / 抗篡改、PSK 往返 / 抗重放、ECDH 握手 / 双向 / 错误 pin 拒绝 |
| `sharedconfig.h` | 共享标定 / 告警 JSON 编解码与字段校验 |
| `workorders.*` | 故障工单的加密、原子保存、去重与确认标记 |
| `timesync.h` | 网络 `TIME_SYNC` 帧（`0x0014`）编解码与偏移计算，只依赖 QtCore，不含系统调用、不改系统时钟；自测见 `../../host/tests/test_timesync.cpp` |

## 两种模式

| 模式 | 密钥 | 握手 | 前向保密 |
|---|---|---|---|
| `Psk` | 预共享 32B（HKDF 导出会话密钥） | 无 | 无 |
| `Ecdh` | X25519 临时 + 身份（静态-静态）→ HKDF | Hello / Reply | 有 |

`Off` 为明文。ECDH 需双方各自身份密钥对 + 固定（pin）对端公钥。

## 线协议（安全数据报）

独立于应用帧：`magic 0x4D53`、`ver 2`、`kind`。

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 2 | `magic` | `0x4D53`（小端） |
| 2 | 1 | `ver` | `2` |
| 3 | 1 | `kind` | `1` Hello · `2` Reply · `3` Secure |
| 4 | — | 载荷 | 见下 |

- Hello（kind=1）：`eph_pub[32] + nonce_c[16]`
- Reply（kind=2）：`eph_pub[32] + nonce_s[16] + tag[32]`，`tag = HMAC-SHA256(session_key, nonce_c‖nonce_s‖eph_c‖eph_s)`
- Secure（kind=3）：`session[16] + seq[8] + tag[16] + ct`。每个发送 session 随机生成，方向密钥为 `HKDF(root_key, session, "mlk-direction-v2")`，nonce 为小端 seq8 + 4 个零字节，完整安全头参与 AAD。
- 接收端按 session 使用 64 包乱序窗口并拒绝反射本端发送包；两端与发送端重启后的密钥域独立。安全封装 v1 不兼容。防重放状态不跨接收进程重启保存；跨重启的新鲜性要求使用 ECDH。

会话密钥：`HKDF(ECDH(eph_c,eph_s) ‖ ECDH(id_c,id_s), salt = nonce_c‖nonce_s, info="mlk-ecdh-v1")`。

## 配对（ECDH，一次性，走串口可信通道）

1. PS：配置 → 安全 → 「生成身份密钥」，记下**指纹**（8 hex）。
2. 串口把 PS 身份公钥（或指纹）传到 PC。
3. PC：配置 → 安全 → 「生成身份密钥」，把 **PS 公钥**填入「对端公钥」并核对指纹。
4. 反向同理：把 PC 身份公钥填入 PS 的「对端公钥」（双向认证）。
5. 两侧模式选 `ECDH`。PC 监听后自动发 Hello，PS 回 Reply，成功后状态栏显示「加密会话已建立」。

PSK：一侧「生成」得到 32B hex，串口抄到另一侧，两侧模式选 `PSK`。

## 使用

PC 端：`配置 → 安全`；或命令行（自测）：

```bash
./mlk_udp_host --target 192.168.1.10:8000 --sec psk --psk <64-hex>
./mlk_udp_host --target 192.168.1.10:8000 --sec ecdh
```

PS 端：HDMI `配置 → 安全`；板端 UDP 服务自动应答握手/解包。

## 构建与自测

```bash
cd host && nix develop
cd common && qmake crypto_test.pro && make -j && ./crypto_test
```

## 依赖

- 目标 rootfs 已带 `libcrypto.so.1.1`（OpenSSL 1.1），用户态即可，无需内核/Buildroot 改动。
- 交叉编译须对齐 OpenSSL **1.1**（目标 ABI）；PC 端用 3.x 无妨。

## 已配对设备发现

`peerdiscovery.*` 提供已配对设备的身份认证发现，使用独立于应用会话的派生密钥、随机挑战与源地址绑定。协议见 [自动发现契约](../../docs/上位机-PS接口约定.md#61-已配对设备自动发现)，密钥生成与公钥文件交换见 [连接与密钥配置](../../docs/上位机连接与密钥配置.md)。

## 本地加密（at-rest）

两端共用 `capturestore.*` 保存四传感器 MLKC 与标签 MLKL，始终使用 AES-256-GCM + PBKDF2 记录口令加密。`capturedialog.*` 提供独立回放与数据表，`capturetool.pro` 构建 `mlk-capture` 用于解密 CSV，`capture_api.py` 调用运行中的标签接口。文件字节格式、训练列与命令见 [采集记录、回放与训练接口](../../docs/采集记录-回放与训练接口.md)。MLKF 为兼容读取格式。寿命记录使用 `.mlkr`，故障工单使用始终加密的 `.mlko`，两端独立保存；见 [无线联网与双端协同](../../docs/无线联网与双端协同.md)。

`inferencejournal.*` 对寿命文件维护按修改时间排序的增量索引，写入前检查保留月数、磁盘余量和 10000 文件上限；每 30 s 核对外部文件变动。自身新增与清理即时更新索引，避免每条推理结果重新全量排序。完整规则见 [寿命记录契约](../../docs/寿命模型-上位机接入.md#6-结果记录与验收)。

模型故障接入：`modelfault.h` 定义 JSON API v1 校验与摘要；`mlk_fault_client.h/.c` 提供无 Qt 依赖的同步 Unix Socket 客户端。字段、返回值与线程规则见[模型故障 API](../../docs/声光告警与模型故障API.md)。
