#pragma once
#include "../remote-pc/src/appconfig.h"
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

namespace mlk {
inline QJsonObject sharedConfig(const AppConfig &cfg) {
    QJsonArray channels;
    for (const auto &c : cfg.ch)
        channels.append(QJsonObject{{"k", c.k}, {"b", c.b}, {"lo", c.alarmLo},
                                    {"hi", c.alarmHi}, {"enabled", c.alarmEnabled}});
    return {{"channels", channels}, {"currentCalibration", QJsonObject{
                {"zero", cfg.currentZero}, {"gainUaPerLsb", cfg.currentGain}}},
            {"revision", double(cfg.sharedRevision)},
            {"owner", cfg.remotePriority ? "remote-pc" : "ps-local"}};
}
inline bool readSharedConfig(const QJsonObject &o, AppConfig &cfg) {
    const auto channels = o.value("channels").toArray();
    const auto owner = o.value("owner").toString();
    const double revision = o.value("revision").toDouble(-1);
    if (channels.size() != 4 || (owner != "ps-local" && owner != "remote-pc")
        || revision < 1 || revision > 4294967294.0 || revision != std::floor(revision)) return false;
    AppConfig next = cfg;
    if (o.contains("currentCalibration")) {
        if (!o.value("currentCalibration").isObject()) return false;
        const auto cal = o.value("currentCalibration").toObject();
        const auto z = cal.value("zero"), g = cal.value("gainUaPerLsb");
        if (!z.isDouble() || !g.isDouble()) return false;
        const double zero = z.toDouble(), gain = g.toDouble();
        if (!std::isfinite(zero) || !std::isfinite(gain) || zero != std::floor(zero)
            || gain != std::floor(gain) || zero < -32768 || zero > 32767
            || gain < 0 || gain > 1000000) return false;
        next.currentZero = qint32(zero); next.currentGain = qint32(gain);
    }
    for (int i = 0; i < 4; ++i) {
        if (!channels[i].isObject()) return false;
        const auto c = channels[i].toObject();
        for (const auto &key : {"k", "b", "lo", "hi"})
            if (!c.value(key).isDouble() || !std::isfinite(c.value(key).toDouble())) return false;
        auto &v = next.ch[i];
        v.k = c.value("k").toDouble(); v.b = c.value("b").toDouble();
        v.alarmLo = c.value("lo").toDouble(); v.alarmHi = c.value("hi").toDouble();
        if (!c.value("enabled").isBool()) return false;
        v.alarmEnabled = c.value("enabled").toBool();
        if (v.k < 0 || v.k > 1e6 || std::abs(v.b) > 1e9 || std::abs(v.alarmLo) > 1e9
            || std::abs(v.alarmHi) > 1e9 || (v.alarmEnabled && v.alarmLo > v.alarmHi)) return false;
    }
    next.remotePriority = owner == "remote-pc"; next.sharedRevision = quint32(revision);
    cfg = next; return true;
}
inline bool sameSharedValues(const AppConfig &a, const AppConfig &b) {
    auto x = sharedConfig(a), y = sharedConfig(b); x.remove("revision"); y.remove("revision");
    return x == y;
}
}
