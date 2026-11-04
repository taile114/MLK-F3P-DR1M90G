#pragma once

/* 网络门户（captive portal）探测。板端千兆网接入局域网后通常需要先登录网络
 * 门户；这里用 204 探测区分三种状态：已联网 / 需要门户登录 / 链路不可用。
 * 探测只在调用方发起，不驻留后台。 */

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace mlk {

enum class PortalState { Offline, Online, LoginRequired };

inline QString portalProbeUrl() { return QStringLiteral("http://connect.rom.miui.com/generate_204"); }

/* 发起一次探测；url 为空时探测直连连通性（204 探测），否则请求指定地址
 * （例如门户网关 192.168.1.1）。返回的 reply 归 net 管理，调用方负责连接
 * finished。8 秒无响应即 abort，避免门户劫持时长时间挂起。 */
inline QNetworkReply *startPortalProbe(QNetworkAccessManager *net, const QUrl &url = QUrl())
{
    QNetworkRequest request{url.isEmpty() ? QUrl(portalProbeUrl()) : url};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "mlk-portal/1");
    request.setRawHeader("Cache-Control", "no-cache");
    auto *reply = net->get(request);
    QTimer::singleShot(8000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    return reply;
}

/* 204 表示直连成功；200 或 3xx 表示被门户接管，portalUrl 给出登录页地址。 */
inline PortalState classifyPortal(QNetworkReply *reply, QUrl *portalUrl = nullptr,
                                  QString *detail = nullptr)
{
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl finalUrl = reply->url();
    if (code == 204) {
        if (portalUrl) *portalUrl = QUrl();
        if (detail) *detail = QStringLiteral("已联网，无需门户认证");
        return PortalState::Online;
    }
    if (code == 200 || (code >= 300 && code < 400)) {
        if (portalUrl) *portalUrl = finalUrl;
        if (detail) *detail = QStringLiteral("需要门户登录：%1").arg(finalUrl.toString());
        return PortalState::LoginRequired;
    }
    if (detail) {
        *detail = reply->error() != QNetworkReply::NoError
            ? reply->errorString() : QStringLiteral("HTTP %1").arg(code);
    }
    return PortalState::Offline;
}

} // namespace mlk
