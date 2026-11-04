#pragma once

#include <QRegularExpression>
#include <QString>

namespace mlk {

// 从 `ip -4 -o addr show dev <iface>` 的输出中取 IPv4 地址。
// 点到点接口（ppp0）输出形如
//   22: ppp0    inet 100.67.27.233 peer 100.67.0.1/32 scope global ppp0
// 本地地址后直接跟 peer，没有前缀长度；普通网口则形如
//   2: eth0    inet 100.67.234.111/16 brd 100.67.255.255 scope global eth0
// 前缀长度存在时保留，不存在时只返回地址。
inline QString ipv4AddressFromIpOutput(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("inet\\s+([0-9.]+)(?:/([0-9]+))?"));
    const auto match = re.match(output);
    if (!match.hasMatch()) return QString();
    const auto address = match.captured(1);
    return match.captured(2).isEmpty() ? address
                                       : address + QLatin1Char('/') + match.captured(2);
}

} // namespace mlk
