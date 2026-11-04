#include <cstdio>

#include <QByteArray>

#include "timesync.h"

using namespace mlk;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char *name)
{
    ++g_checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_failures;
}

void roundTrip()
{
    TimeSyncFrame out;
    out.ver = kTimeSyncVersion;
    out.reserved[1] = 0xA5;
    out.epoch_seconds = 1700000000;
    out.uncertainty_ms = 250;

    const QByteArray wire = buildTimeSync(out);
    check(wire.size() == kTimeSyncSize, "build yields 10 bytes");

    TimeSyncFrame in;
    check(parseTimeSync(wire, &in), "parse accepts a 10-byte frame");
    check(in.ver == kTimeSyncVersion, "version roundtrips");
    check(in.reserved[1] == 0xA5, "reserved bytes roundtrip");
    check(in.epoch_seconds == 1700000000, "epoch_seconds roundtrips");
    check(in.uncertainty_ms == 250, "uncertainty_ms roundtrips");

    TimeSyncFrame negative = out;
    negative.epoch_seconds = -123456789;
    negative.uncertainty_ms = 65535;
    check(parseTimeSync(buildTimeSync(negative), &in), "parse accepts a negative epoch");
    check(in.epoch_seconds == -123456789, "negative epoch roundtrips");
    check(in.uncertainty_ms == 65535, "maximum uncertainty roundtrips");
}

void wireLayout()
{
    TimeSyncFrame frame;
    frame.epoch_seconds = 0x01020304;
    frame.uncertainty_ms = 0x0506;
    const QByteArray wire = buildTimeSync(frame);

    check(quint8(wire[0]) == kTimeSyncVersion, "offset 0 holds ver");
    check(quint8(wire[1]) == 0 && quint8(wire[2]) == 0 && quint8(wire[3]) == 0,
          "offsets 1-3 hold zero reserved bytes");
    check(quint8(wire[4]) == 0x04 && quint8(wire[5]) == 0x03 && quint8(wire[6]) == 0x02 &&
              quint8(wire[7]) == 0x01,
          "epoch_seconds is little endian at offset 4");
    check(quint8(wire[8]) == 0x06 && quint8(wire[9]) == 0x05,
          "uncertainty_ms is little endian at offset 8");
}

void rejectBadInput()
{
    TimeSyncFrame frame;
    frame.epoch_seconds = 1700000000;
    const QByteArray wire = buildTimeSync(frame);

    TimeSyncFrame out;
    check(!parseTimeSync(QByteArray(), &out), "empty payload rejected");
    check(!parseTimeSync(wire.left(9), &out), "9-byte payload rejected");
    check(!parseTimeSync(wire + char(0), &out), "11-byte payload rejected");
    check(!parseTimeSync(wire, nullptr), "null output rejected");

    TimeSyncFrame future;
    future.ver = kTimeSyncVersion + 1;
    check(!parseTimeSync(buildTimeSync(future), &out), "unsupported version rejected");

    TimeSyncFrame zero;
    zero.ver = 0;
    check(!parseTimeSync(buildTimeSync(zero), &out), "version 0 rejected");
}

void offsetArithmetic()
{
    TimeSyncFrame frame;
    frame.epoch_seconds = 1700000000;
    const qint64 local = 1700000000LL * 1000;

    check(timeSyncOffsetMs(frame, local) == 0, "aligned clocks give zero offset");
    check(timeSyncOffsetMs(frame, local + 500) == -500, "local clock ahead gives negative offset");
    check(timeSyncOffsetMs(frame, local - 1200) == 1200, "local clock behind gives positive offset");

    frame.uncertainty_ms = 65535;
    check(timeSyncOffsetMs(frame, local + 500) == -500,
          "uncertainty_ms does not change a negative offset");
    check(timeSyncOffsetMs(frame, local - 1200) == 1200,
          "uncertainty_ms does not change a positive offset");
    check(timeSyncOffsetMs(frame, local) == 0, "uncertainty_ms does not change a zero offset");
}

} // namespace

int main()
{
    roundTrip();
    wireLayout();
    rejectBadInput();
    offsetArithmetic();

    std::printf("PASS=%d FAIL=%d\n", g_checks - g_failures, g_failures);
    if (g_failures == 0)
        std::printf("all %d checks passed\n", g_checks);
    return g_failures == 0 ? 0 : 1;
}
