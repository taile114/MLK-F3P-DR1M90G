#pragma once

/* 门户登录窗口：抓取门户登录页，识别第一个含密码输入的登录表单，用用户输入的
 * 账号/密码提交，然后重新做一次 204 探测确认是否已联网。
 *
 * 能力边界：只处理服务端渲染的表单（form + input）。依赖 JavaScript 计算
 * challenge、或使用 CHAP/portal 私有协议的认证页无法在此完成，窗口会显示
 * 门户地址，需在其余设备上完成认证。 */

#include <QDialog>
#include <QList>
#include <QString>
#include <QUrl>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;

class PortalDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PortalDialog(QWidget *parent = nullptr);
    void setPortalUrl(const QUrl &url);
    void setCredentials(const QString &user, const QString &password);
    /* 无界面自动登录：直接抓取登录页，识别表单后立即提交一次。 */
    void autoLogin(const QUrl &url, const QString &user, const QString &password);
signals:
    void portalOnline();
private:
    struct Field { QString name, value, type; };
    struct Form { QUrl action; QString method; QList<Field> fields; bool hasPassword = false; };

    void detect();
    void probe();
    void handleProbe(QNetworkReply *reply);
    void fetchPage();
    void handlePage(QNetworkReply *reply);
    void submit();
    void handleSubmit(QNetworkReply *reply);
    void formFrom(const QString &html, const QUrl &base);
    void log(const QString &text);
    void setBusy(bool busy);

    QNetworkAccessManager *m_net = nullptr;
    QLineEdit *m_url = nullptr, *m_user = nullptr, *m_password = nullptr;
    QLabel *m_status = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_detect = nullptr, *m_fetch = nullptr, *m_login = nullptr;
    Form m_form;
    bool m_ready = false;
    bool m_autoSubmit = false;
    QUrl m_portalUrl;
};
