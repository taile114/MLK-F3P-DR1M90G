#pragma once

#include <QByteArray>
#include <QList>
#include <QVector>
#include <QtGlobal>
#include <QString>
#include <QMetaType>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <cstring>

namespace mlk {

constexpr quint16 kMagic = 0x4B4D;
constexpr quint8 kVersion = 2;
constexpr quint8 kInferenceVersion = 3;
constexpr quint8 kControlVersion = 4;
constexpr quint8 kLegacyWaveformVersion = 5;
constexpr quint8 kWaveformVersion = 6; // UTC first sample; current/speed signed32
/* 完整故障状态使用版本 6 + 类型 8；波形仍以类型 0 区分，二者不冲突。
 * 摘要只由该消息控制，BoardStatus 的 modelFault 仅保留 v1 兼容用途。 */
constexpr quint8 kFaultStatusVersion = 6;
constexpr int kHeaderSize = 28;
constexpr int kFaultStatusMaxFrame = 1200;

enum FrameType : quint8 {
    kTypeWaveform = 0,
    kTypeDiagnosis = 1,
    kTypeInject = 2,
    kTypeReplayControl = 3,
    kTypeConfig = 4,
    kTypeWorkOrder = 5,
    kTypeOrderAck = 6,
    kTypeBoardStatus = 7,
    kTypeFaultStatus = 8,
};

enum ReplayAction : quint8 { kReplayStart = 1, kReplayPause, kReplayStep,
                            kReplayStop, kReplaySeek, kReplayStatus };
enum InferenceState : quint8 { kStopped, kLoading, kReady, kRunning,
                              kPaused, kCompleted, kInferenceError, kStopping };
constexpr quint8 kValidRul = 1;
constexpr quint8 kDatasetReplay = 1;
constexpr quint8 kObservedAgingSession = 1;

enum Channel : quint8 {
    kTemp = 0,
    kCurrent = 1,
    kVibration = 2,
    kSpeed = 3,
    kChannelCount = 4,
};

struct Frame
{
    quint8 ver = 0;
    quint8 type = 0;
    quint32 seq = 0;
    quint32 cnt = 0;
    quint16 rateHz = 0;
    quint64 tsUs = 0;
    quint8 ch = 0;
    QJsonObject object;

    QVector<qint32> samples;
    QVector<qint32> samplesY;
    QVector<qint32> samplesZ;

    quint8 cls = 0;
    quint8 grade = 0;
    quint16 conf = 0;
    quint32 rul = 0;
    quint32 inferUs = 0;
    quint8 task = 0;
    quint8 valid = 0;
    quint8 source = 0;
    quint8 backend = 0;
    float rulValue = 0;
    quint32 windowIndex = 0;
    quint32 windowCount = 0;
    /* 当前回放窗口的实际寿命标签：只在本机回放时由运行包的标签文件填入，
     * 不写入 v3 帧，因此经 UDP 收到的帧这两个字段恒为 0。 */
    float rulLabel = 0;
    quint8 rulLabelValid = 0;
    /* 本次回放已处理窗口的平均绝对偏差（%）与参与平均的窗口数，同样只在
     * 本机维护；窗口数为 0 时表示还没有可比较的窗口。 */
    float rulDeviationAvg = 0;
    quint32 rulDeviationCount = 0;
    quint8 inferenceState = kStopped;
    quint8 rulUnit = 0;
    QString modelId;
    QString datasetId;
    QString message;
    quint32 requestId = 0;
    quint16 commandStatus = 0; // 0 heartbeat, 1 accepted, 2 rejected
    quint8 action = 0;
    quint32 intervalMs = 1000;

    qint16 setpoint[4] = {};
    quint16 durationMs = 0;
};

inline quint16 rd16(const char *p)
{
    return quint16(quint8(p[0])) | (quint16(quint8(p[1])) << 8);
}

inline quint32 rd32(const char *p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16)
           | (quint32(quint8(p[3])) << 24);
}

inline quint64 rd64(const char *p)
{
    return quint64(rd32(p)) | (quint64(rd32(p + 4)) << 32);
}

inline void wr16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}

inline void wr32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

inline void wr64(QByteArray &b, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

inline bool parseFrame(const QByteArray &d, Frame &f)
{
    if (d.size() < kHeaderSize)
        return false;
    const char *p = d.constData();
    if (rd16(p) != kMagic)
        return false;
    const auto version = quint8(p[2]);
    const auto type = quint8(p[3]);
    if (version != 1 && version != kVersion && version != kInferenceVersion
        && version != kControlVersion && version != kLegacyWaveformVersion
        && version != kWaveformVersion) return false;
    if (type > kTypeFaultStatus
        || (type == kTypeFaultStatus && version != kFaultStatusVersion)
        || (type != kTypeFaultStatus && type >= kTypeConfig && version != kControlVersion)
        || (type == kTypeReplayControl && version != kInferenceVersion)) return false;
    if (version == kInferenceVersion && type != kTypeDiagnosis && type != kTypeReplayControl) return false;
    if (version == kControlVersion && type != kTypeWaveform && type < kTypeConfig) return false;
    if (version == kWaveformVersion && type != kTypeWaveform && type != kTypeFaultStatus) return false;
    if (version == kLegacyWaveformVersion && type != kTypeWaveform) return false;
    f = Frame{};

    f.ver = quint8(p[2]);
    f.type = quint8(p[3]);
    f.seq = rd32(p + 4);
    f.cnt = rd32(p + 8);
    f.rateHz = rd16(p + 12);
    f.ch = quint8(p[24]);
    f.tsUs = rd64(p + 16);

    if (f.ver == kFaultStatusVersion && f.type == kTypeFaultStatus) {
        if (d.size() <= kHeaderSize || d.size() > kFaultStatusMaxFrame) return false;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(d.mid(kHeaderSize), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
        f.object = doc.object();
    } else if (f.ver == 4 && f.type >= kTypeConfig) {
        if (d.size() > 1200 || d.size() <= kHeaderSize) return false;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(d.mid(kHeaderSize), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
        f.object = doc.object();
    } else if (f.ver == 3 && f.type == kTypeDiagnosis) {
        if (d.size() != 224) return false;
        f.task = quint8(p[24]); f.valid = quint8(p[25]);
        f.source = quint8(p[26]); f.backend = quint8(p[27]);
        const auto bits = rd32(p + 28);
        std::memcpy(&f.rulValue, &bits, 4);
        f.inferUs = rd32(p + 32);
        f.windowIndex = rd32(p + 36); f.windowCount = rd32(p + 40);
        f.inferenceState = quint8(p[44]); f.rulUnit = quint8(p[45]);
        f.commandStatus = rd16(p + 46);
        f.modelId = QString::fromLatin1(QByteArray(p + 48, 32).split('\0').first());
        f.datasetId = QString::fromLatin1(QByteArray(p + 80, 12).split('\0').first());
        f.requestId = rd32(p + 92);
        f.message = QString::fromUtf8(QByteArray(p + 96, 128).split('\0').first());
        if (f.task != 1 || (f.valid & ~kValidRul) || f.source != kDatasetReplay
            || f.backend > 2 || f.inferenceState > kStopping || f.rulUnit != kObservedAgingSession || f.commandStatus > 2)
            return false;
        if (f.valid && (!std::isfinite(f.rulValue) || !f.windowCount
            || f.windowIndex >= f.windowCount || !f.backend)) return false;
    } else if (f.ver == 3 && f.type == kTypeReplayControl) {
        if (d.size() != 40) return false;
        f.action = quint8(p[24]); f.backend = quint8(p[25]);
        f.windowIndex = rd32(p + 28); f.intervalMs = rd32(p + 32);
        f.requestId = f.seq;
        if (f.action < kReplayStart || f.action > kReplayStatus || f.backend < 1
            || f.backend > 2 || f.intervalMs < 100 || f.intervalMs > 60000) return false;
    } else if (f.type == kTypeWaveform) {
        const int n = rd16(p + 14);
        const int axes = f.ver >= 4 ? quint8(p[25]) : 1;
        const bool wide = f.ver >= 6 && (f.ch == kCurrent || f.ch == kSpeed);
        const int width = wide ? 4 : 2;
        if (f.ch >= kChannelCount || (axes != 1 && axes != 3)
            || (axes == 3 && f.ch != kVibration) || n == 0
            || d.size() != kHeaderSize + width * n * axes)
            return false;
        f.samples.resize(n);
        for (int i = 0; i < n; ++i)
            f.samples[i] = wide ? qint32(rd32(p + kHeaderSize + 4 * i))
                                : qint16(rd16(p + kHeaderSize + 2 * i));
        if (axes == 3) {
            f.samplesY.resize(n); f.samplesZ.resize(n);
            for (int i = 0; i < n; ++i) {
                f.samplesY[i] = qint16(rd16(p + kHeaderSize + 2 * (n + i)));
                f.samplesZ[i] = qint16(rd16(p + kHeaderSize + 2 * (2 * n + i)));
            }
        }
    } else if (f.type == kTypeDiagnosis) {
        if (d.size() < 36)
            return false;
        f.cls = quint8(p[24]);
        f.grade = quint8(p[25]);
        f.conf = rd16(p + 26);
        f.rul = rd32(p + 28);
        f.inferUs = rd32(p + 32);
    } else if (f.type == kTypeInject) {
        if (d.size() < 40)
            return false;
        for (int i = 0; i < 4; ++i)
            f.setpoint[i] = qint16(rd16(p + 28 + 2 * i));
        f.durationMs = rd16(p + 36);
    }
    return true;
}

inline QByteArray inferenceHeader(quint8 type, quint32 sequence, quint64 tsUs)
{
    QByteArray d; wr16(d, kMagic); d.append(char(kInferenceVersion)); d.append(char(type));
    wr32(d, sequence); wr32(d, 0); wr16(d, 0); wr16(d, 0); wr64(d, tsUs);
    return d;
}

inline void fixedText(QByteArray &d, const QByteArray &text, int width)
{
    const auto part = text.left(width - 1); d.append(part); d.append(width - part.size(), '\0');
}

inline QByteArray buildInferenceFrame(const Frame &f)
{
    auto d = inferenceHeader(kTypeDiagnosis, f.seq, f.tsUs);
    d.append(char(f.task)); d.append(char(f.valid)); d.append(char(f.source)); d.append(char(f.backend));
    quint32 bits; static_assert(sizeof(float) == 4, "IEEE float32 required");
    std::memcpy(&bits, &f.rulValue, 4); wr32(d, bits); wr32(d, f.inferUs);
    wr32(d, f.windowIndex); wr32(d, f.windowCount);
    d.append(char(f.inferenceState)); d.append(char(f.rulUnit)); wr16(d, f.commandStatus);
    fixedText(d, f.modelId.toLatin1(), 32); fixedText(d, f.datasetId.toLatin1(), 12);
    wr32(d, f.requestId); fixedText(d, f.message.toUtf8(), 128);
    return d;
}

inline QByteArray buildReplayControl(quint8 action, quint8 backend, quint32 window,
                                     quint32 interval, quint32 request, quint64 tsUs)
{
    auto d = inferenceHeader(kTypeReplayControl, request, tsUs);
    d.append(char(action)); d.append(char(backend)); wr16(d, 0);
    wr32(d, window); wr32(d, interval); wr32(d, 0);
    return d;
}

inline bool hasClassification(const Frame &f) { return f.ver < 3 && f.cls <= 2; }

inline QByteArray buildInjectFrame(const qint16 v[4], quint16 durationMs, quint32 seq, quint64 tsUs)
{
    QByteArray d;
    wr16(d, kMagic);
    d.append(char(kVersion));
    d.append(char(kTypeInject));
    wr32(d, seq);
    wr32(d, 0);
    wr16(d, 0);
    wr16(d, 0);
    wr64(d, tsUs);
    d.append(char(0));
    d.append(3, '\0');
    for (int i = 0; i < 4; ++i)
        wr16(d, quint16(v[i]));
    wr16(d, durationMs);
    wr16(d, 0);
    return d;
}

inline QByteArray buildObjectFrame(quint8 type, const QJsonObject &object, quint32 seq = 0)
{
    auto d = inferenceHeader(type, seq, 0);
    d[2] = char(kControlVersion); d.append(4, '\0');
    d += QJsonDocument(object).toJson(QJsonDocument::Compact);
    return d.size() <= 1200 ? d : QByteArray();
}

/* UDP v6/type8 完整故障状态：28 字节头（tsUs = 最后窗口 endUs）+ JSON 摘要，
 * 整帧不超过 1200 字节。seq 是传输序号，与模型窗口 seq 分开。 */
inline QByteArray buildFaultStatus(const QJsonObject &object, quint32 seq, quint64 tsUs)
{
    QByteArray d;
    wr16(d, kMagic); d.append(char(kFaultStatusVersion)); d.append(char(kTypeFaultStatus));
    wr32(d, seq); wr32(d, 0); wr16(d, 0); wr16(d, 0); wr64(d, tsUs);
    d.append(4, '\0');
    if (d.size() != kHeaderSize) return QByteArray();
    d += QJsonDocument(object).toJson(QJsonDocument::Compact);
    return d.size() <= kFaultStatusMaxFrame ? d : QByteArray();
}

inline QByteArray buildWaveform(const Frame &f)
{
    const int n = f.samples.size();
    const bool xyz = f.samplesY.size() == n && f.samplesZ.size() == n;
    const bool wide = f.ch == kCurrent || f.ch == kSpeed;
    if (n < 1 || n > 128 || f.ch >= kChannelCount || (xyz && f.ch != kVibration)) return {};
    if (!wide) {
        for (const auto *axis : {&f.samples, &f.samplesY, &f.samplesZ})
            for (auto v : *axis) if (v < -32768 || v > 32767) return {};
    }
    QByteArray d; wr16(d, kMagic); d.append(char(kWaveformVersion)); d.append(char(kTypeWaveform));
    wr32(d, f.seq); wr32(d, f.cnt); wr16(d, f.rateHz); wr16(d, quint16(n)); wr64(d, f.tsUs);
    d.append(char(f.ch)); d.append(char(xyz ? 3 : 1)); wr16(d, 0);
    for (auto v : f.samples) {
        if (wide) wr32(d, quint32(v));
        else wr16(d, quint16(v));
    }
    if (xyz) {
        for (auto v : f.samplesY) wr16(d, quint16(v));
        for (auto v : f.samplesZ) wr16(d, quint16(v));
    }
    return d;
}

} // namespace mlk

Q_DECLARE_METATYPE(mlk::Frame)
