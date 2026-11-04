#pragma once

// Network TIME_SYNC frame (control command 0x0014), see
// docs/上位机-PS接口约定.md §4 and §7.
//
// The frame is a fixed 10-byte little-endian payload:
//   ver(u8) + reserved[3] + epoch_seconds(i32 LE) + uncertainty_ms(u16 LE)
//
// Header-only and QtCore-only (QByteArray); it encodes and decodes the
// payload and computes the clock offset. It performs no system call and never
// changes the host or device clock: applying the offset on the PS side stays
// reserved.

#include <QByteArray>
#include <QtGlobal>
#include <cstring>

namespace mlk {

constexpr quint8 kTimeSyncVersion = 1;
constexpr int kTimeSyncSize = 10;

#pragma pack(push, 1)

struct TimeSyncFrame
{
    quint8 ver = kTimeSyncVersion;
    quint8 reserved[3] = {0, 0, 0};
    qint32 epoch_seconds = 0;
    quint16 uncertainty_ms = 0;
};

#pragma pack(pop)

static_assert(sizeof(TimeSyncFrame) == kTimeSyncSize,
              "TimeSyncFrame must stay 10 bytes on the wire");

namespace detail {

inline void writeLe16(char *p, quint16 value)
{
    p[0] = char(value & 0xFFu);
    p[1] = char((value >> 8) & 0xFFu);
}

inline void writeLe32(char *p, quint32 value)
{
    p[0] = char(value & 0xFFu);
    p[1] = char((value >> 8) & 0xFFu);
    p[2] = char((value >> 16) & 0xFFu);
    p[3] = char((value >> 24) & 0xFFu);
}

inline quint16 readLe16(const char *p)
{
    const quint16 lo = quint8(p[0]);
    const quint16 hi = quint8(p[1]);
    return quint16(lo | quint16(hi << 8));
}

inline quint32 readLe32(const char *p)
{
    const quint32 b0 = quint8(p[0]);
    const quint32 b1 = quint8(p[1]);
    const quint32 b2 = quint8(p[2]);
    const quint32 b3 = quint8(p[3]);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

} // namespace detail

inline QByteArray buildTimeSync(const TimeSyncFrame &frame)
{
    QByteArray wire(kTimeSyncSize, '\0');
    char *p = wire.data();
    p[0] = char(frame.ver);
    p[1] = char(frame.reserved[0]);
    p[2] = char(frame.reserved[1]);
    p[3] = char(frame.reserved[2]);
    quint32 epoch = 0;
    std::memcpy(&epoch, &frame.epoch_seconds, sizeof epoch);
    detail::writeLe32(p + 4, epoch);
    detail::writeLe16(p + 8, frame.uncertainty_ms);
    return wire;
}

inline bool parseTimeSync(const QByteArray &wire, TimeSyncFrame *frame)
{
    if (frame == nullptr || wire.size() != kTimeSyncSize)
        return false;
    const char *p = wire.constData();
    if (quint8(p[0]) != kTimeSyncVersion)
        return false;
    frame->ver = quint8(p[0]);
    frame->reserved[0] = quint8(p[1]);
    frame->reserved[1] = quint8(p[2]);
    frame->reserved[2] = quint8(p[3]);
    const quint32 epoch = detail::readLe32(p + 4);
    std::memcpy(&frame->epoch_seconds, &epoch, sizeof epoch);
    frame->uncertainty_ms = detail::readLe16(p + 8);
    return true;
}

// Offset to add to a local epoch (milliseconds) so it follows the sender:
// frame.epoch_seconds * 1000 - localEpochMs. Positive advances the local
// clock, negative sets it back. uncertainty_ms is a quality hint only; it is
// not applied and does not affect the sign.
inline qint64 timeSyncOffsetMs(const TimeSyncFrame &frame, qint64 localEpochMs)
{
    return qint64(frame.epoch_seconds) * 1000 - localEpochMs;
}

} // namespace mlk
