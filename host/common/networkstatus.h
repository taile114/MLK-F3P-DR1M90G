#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <limits>

namespace mlk {
struct NetworkStatus
{
    QString kind = QStringLiteral("disconnected");
    QString interface;

    QString text() const {
        const auto state = kind == "wifi" ? QStringLiteral("WiFi")
            : kind == "cellular" ? QStringLiteral("4G")
            : kind == "ethernet" ? QStringLiteral("千兆网")
            : kind == "other" ? interface : QStringLiteral("未连接");
        return QStringLiteral("网络：%1").arg(state);
    }
    QJsonObject toJson() const { return {{"version",1},{"kind",kind},{"interface",interface}}; }
    static bool fromJson(const QJsonObject &o, NetworkStatus &status) {
        const auto kind = o.value("kind").toString();
        const auto iface = o.value("interface").toString();
        if (o.value("version") != QJsonValue(1) || !o.value("interface").isString()
            || !QStringList{"disconnected","wifi","cellular","ethernet","other"}.contains(kind)
            || (kind != "disconnected" && iface.isEmpty()) || iface.size() > 64) return false;
        for (QChar c : iface) if (!c.isLetterOrNumber() && c != '_' && c != '-' && c != '.' && c != ':') return false;
        status.kind = kind; status.interface = iface;
        return true;
    }
};

struct NetworkLink {
    QString name;
    bool connected = false; // Running with an IPv4 address; not an Internet probe.
    bool wireless = false;
    QString driver;
};

inline NetworkStatus selectNetworkStatus(const QByteArray &routes, const QVector<NetworkLink> &links)
{
    auto classify = [](const NetworkLink &link) {
        if (link.wireless || link.name.startsWith("wl")) return QStringLiteral("wifi");
        const bool cellularDriver = link.driver.compare("GobiNet",Qt::CaseInsensitive)==0
            || link.driver=="qmi_wwan" || link.driver=="cdc_mbim";
        if (link.name.startsWith("wwan") || link.name.startsWith("usb")
            || link.name.startsWith("rmnet") || cellularDriver) return QStringLiteral("cellular");
        // ppp0 由 PPPoE 在有线口上建立，承载的是千兆网而非 4G；4G 走 wwan/usb 接口。
        if (link.name.startsWith("ppp")) return QStringLiteral("ethernet");
        if (link.name.startsWith("eth") || link.name.startsWith("en")) return QStringLiteral("ethernet");
        return QStringLiteral("other");
    };
    int selected = -1;
    quint64 bestMetric = std::numeric_limits<quint64>::max();
    for (const auto &line : routes.split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 8 || fields[1] != "00000000" || fields[7] != "00000000") continue;
        bool flagsOk = false, metricOk = false;
        const auto flags = fields[3].toUInt(&flagsOk,16);
        const auto metric = fields[6].toULongLong(&metricOk);
        if (!flagsOk || !metricOk || !(flags & 1) || (flags & 0x200) || metric >= bestMetric) continue;
        for (int i=0; i<links.size(); ++i) {
            if (links[i].connected && links[i].name==QString::fromLatin1(fields[0])) {
                selected=i; bestMetric=metric; break;
            }
        }
    }
    // A connected local subnet can operate without a default route.
    if (selected < 0) {
        int best = 4;
        for (int i=0; i<links.size(); ++i) {
            if (!links[i].connected) continue;
            const auto kind=classify(links[i]);
            // Link preference matches the runtime policy: 千兆网 > 4G > WiFi.
            const int rank=kind=="ethernet" ? 0 : kind=="cellular" ? 1 : kind=="wifi" ? 2 : 3;
            if (rank < best) { best=rank; selected=i; }
        }
    }
    if (selected < 0) return {};
    return {classify(links[selected]),links[selected].name};
}
} // namespace mlk
