#pragma once

#include <QString>
#include <QtGlobal>

/* 单主故障模型的公共类型。分类契约 single_primary_fault_v1 只有五个类别；
 * 具体故障类型是结构化字段，不能由消息文本反推。旧 API v1 的 class 0/1/2
 * 只保留兼容用途，见 modelfault.h。 */
namespace mlk {

enum FaultClass : int {
    kFaultNormal = 0,
    kFaultOverload = 1,
    kFaultUndervoltage = 2,
    kFaultOvertemperature = 3,
    kFaultMountTilt = 4,
    kFaultClassCount = 5,
    kFaultUnavailable = -1,
};

/* 模型诊断 API 2 的 state 取值，顺序与 JSON 字符串一一对应。 */
enum FaultState : int {
    kFaultStopped = 0,
    kFaultLoading,
    kFaultWaiting,
    kFaultRunning,
    kFaultDataInsufficient,
    kFaultStale,
    kFaultError,
    kFaultStopping,
    kFaultStateCount,
};

enum FaultBackend : int {
    kFaultBackendUnknown = 0,
    kFaultBackendCpu = 1,
    kFaultBackendCpuCnnNpu = 2,
};

constexpr const char *kFaultTask = "bench_fault_single_v1";
constexpr const char *kFaultContract = "single_primary_fault_v1";
constexpr const char *kFaultPreprocess = "base6_native_blocks_short_context_v2";
constexpr const char *kFaultInputLayout = "base6_single_native4s_flat_v1";
constexpr const char *kFaultResultSchema = "mlk.fault.result.v2";
constexpr const char *kFaultOrderSchema = "mlk.fault.order.v2";
constexpr const char *kFaultStatusSchema = "mlk.fault.status.v1";
constexpr const char *kFaultRuntimeSchema = "mlk.fault.runtime.v1";

/* 窗口：四秒历史、两秒步长，单位微秒。 */
constexpr quint64 kFaultWindowUs = 4000000ull;
constexpr quint64 kFaultStrideUs = 2000000ull;
/* 结果最大年龄（REF）。停发超过该值即撤销当前模型告警。 */
constexpr qint64 kFaultResultMaxAgeMs = 6000;
/* 确认：同类别同等级连续两窗，恢复正常连续三窗（REF）。 */
constexpr int kFaultConfirmWindows = 2;
constexpr int kFaultRecoverWindows = 3;
/* 实采标定前的试用模型告警门槛，不改变冻结数据集解码与分级阈值。 */
constexpr double kFaultAlarmMinConfidence = 0.80;
constexpr int kFaultReasonMaxBytes = 64;
constexpr int kFaultIdentityMaxBytes = 64;

inline QString faultName(int classIndex)
{
    switch (classIndex) {
    case kFaultNormal: return QStringLiteral("normal");
    case kFaultOverload: return QStringLiteral("overload");
    case kFaultUndervoltage: return QStringLiteral("undervoltage");
    case kFaultOvertemperature: return QStringLiteral("overtemperature");
    case kFaultMountTilt: return QStringLiteral("mount_tilt");
    default: return QStringLiteral("unavailable");
    }
}

/* 界面用中文名；无效值显示 N/A，不显示“正常”。 */
inline QString faultDisplayName(int classIndex)
{
    switch (classIndex) {
    case kFaultNormal: return QStringLiteral("正常");
    case kFaultOverload: return QStringLiteral("过载");
    case kFaultUndervoltage: return QStringLiteral("欠压");
    case kFaultOvertemperature: return QStringLiteral("过热");
    case kFaultMountTilt: return QStringLiteral("电机倾斜");
    default: return QStringLiteral("N/A");
    }
}

inline bool faultClassFromName(const QString &name, int *classIndex)
{
    for (int i = 0; i < kFaultClassCount; ++i) {
        if (name == faultName(i)) {
            if (classIndex) *classIndex = i;
            return true;
        }
    }
    return false;
}

inline QString faultStateName(int state)
{
    switch (state) {
    case kFaultStopped: return QStringLiteral("stopped");
    case kFaultLoading: return QStringLiteral("loading");
    case kFaultWaiting: return QStringLiteral("waiting");
    case kFaultRunning: return QStringLiteral("running");
    case kFaultDataInsufficient: return QStringLiteral("data_insufficient");
    case kFaultStale: return QStringLiteral("stale");
    case kFaultError: return QStringLiteral("error");
    case kFaultStopping: return QStringLiteral("stopping");
    default: return QStringLiteral("error");
    }
}

inline QString faultStateDisplay(int state)
{
    switch (state) {
    case kFaultStopped: return QStringLiteral("已停止");
    case kFaultLoading: return QStringLiteral("加载中");
    case kFaultWaiting: return QStringLiteral("等待窗口");
    case kFaultRunning: return QStringLiteral("运行中");
    case kFaultDataInsufficient: return QStringLiteral("数据不足");
    case kFaultStale: return QStringLiteral("已过期");
    case kFaultError: return QStringLiteral("错误");
    case kFaultStopping: return QStringLiteral("停止中");
    default: return QStringLiteral("错误");
    }
}

inline bool faultStateFromName(const QString &name, int *state)
{
    for (int i = 0; i < kFaultStateCount; ++i) {
        if (name == faultStateName(i)) {
            if (state) *state = i;
            return true;
        }
    }
    return false;
}

inline QString faultBackendName(int backend)
{
    return backend == kFaultBackendCpu ? QStringLiteral("cpu")
         : backend == kFaultBackendCpuCnnNpu ? QStringLiteral("cpu_cnn_npu")
                                             : QStringLiteral("unknown");
}

inline QString faultBackendDisplay(int backend)
{
    return backend == kFaultBackendCpu ? QStringLiteral("CPU")
         : backend == kFaultBackendCpuCnnNpu ? QStringLiteral("CPU+CNN NPU")
                                             : QStringLiteral("未知");
}

inline bool faultBackendFromName(const QString &name, int *backend)
{
    if (name == QStringLiteral("cpu")) { if (backend) *backend = kFaultBackendCpu; return true; }
    if (name == QStringLiteral("cpu_cnn_npu")) { if (backend) *backend = kFaultBackendCpuCnnNpu; return true; }
    if (name == QStringLiteral("unknown")) { if (backend) *backend = kFaultBackendUnknown; return true; }
    return false;
}

/* 正常只允许 0 级；四类故障只允许 1～3 级；无效值为 -1。 */
inline bool faultGradeValid(int classIndex, int grade)
{
    if (classIndex == kFaultUnavailable) return grade == -1;
    if (classIndex == kFaultNormal) return grade == 0;
    return classIndex >= kFaultOverload && classIndex <= kFaultMountTilt
        && grade >= 1 && grade <= 3;
}

/* 结构化工单的兼容粗分类：grade=1→1，grade=2/3→2。新端不以该字段推断类型。 */
inline int faultLegacyClass(int grade)
{
    return grade <= 0 ? 0 : grade == 1 ? 1 : 2;
}

inline bool asciiWithin(const QString &text, int maxBytes)
{
    const auto utf8 = text.toUtf8();
    if (utf8.size() > maxBytes) return false;
    for (char c : utf8)
        if (uchar(c) >= 0x80) return false;
    return true;
}

/* RFC 4122 文本形式，小写十六进制与连字符，36 字节。 */
inline bool validUuid(const QString &text)
{
    if (text.size() != 36) return false;
    for (int i = 0; i < 36; ++i) {
        const QChar c = text.at(i);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != QLatin1Char('-')) return false;
        } else if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                     || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                     || (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) {
            return false;
        }
    }
    return true;
}

} // namespace mlk
