#include "portaldialog.h"
#include "captiveportal.h"

#include <QtWidgets>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {

QString attribute(const QString &tag, const QString &name)
{
    const QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))")
                                    .arg(name),
                                QRegularExpression::CaseInsensitiveOption);
    const auto match = re.match(tag);
    if (!match.hasMatch()) return {};
    return match.captured(1) + match.captured(2) + match.captured(3);
}

} // namespace

PortalDialog::PortalDialog(QWidget *parent)
    : QDialog(parent), m_net(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("portalDialog"));
    setWindowTitle(QStringLiteral("门户登录"));
    setWindowModality(Qt::WindowModal);
    resize(680, 560);
    auto *root = new QVBoxLayout(this);
    auto *hint = new QLabel(QStringLiteral(
        "千兆网获得地址后若不能直接访问外网，需要先登录网络门户。"
        "本窗口抓取门户页中的登录表单并提交账号密码。"), this);
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    root->addWidget(hint);

    auto *form = new QFormLayout;
    m_url = new QLineEdit(this);
    m_url->setObjectName(QStringLiteral("portalUrl"));
    m_url->setPlaceholderText(QStringLiteral("留空则自动探测"));
    m_user = new QLineEdit(this);
    m_user->setObjectName(QStringLiteral("portalUser"));
    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("portalPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("门户地址"), m_url);
    form->addRow(QStringLiteral("账号"), m_user);
    form->addRow(QStringLiteral("密码"), m_password);
    root->addLayout(form);

    auto *buttons = new QHBoxLayout;
    m_detect = new QPushButton(QStringLiteral("检测连通性"), this);
    m_detect->setObjectName(QStringLiteral("portalDetect"));
    m_fetch = new QPushButton(QStringLiteral("获取登录页"), this);
    m_fetch->setObjectName(QStringLiteral("portalFetch"));
    m_login = new QPushButton(QStringLiteral("登录"), this);
    m_login->setObjectName(QStringLiteral("portalSubmit"));
    m_login->setEnabled(false);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    buttons->addWidget(m_detect);
    buttons->addWidget(m_fetch);
    buttons->addWidget(m_login);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);

    m_status = new QLabel(QStringLiteral("门户：未检测"), this);
    m_status->setObjectName(QStringLiteral("portalStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("portalLog"));
    m_log->setReadOnly(true);
    m_log->setPlaceholderText(QStringLiteral("门户交互记录"));
    root->addWidget(m_log, 1);

    connect(m_detect, &QPushButton::clicked, this, &PortalDialog::detect);
    connect(m_fetch, &QPushButton::clicked, this, &PortalDialog::fetchPage);
    connect(m_login, &QPushButton::clicked, this, &PortalDialog::submit);
    connect(closeButton, &QPushButton::clicked, this, &PortalDialog::accept);
    connect(m_url, &QLineEdit::returnPressed, this, &PortalDialog::fetchPage);
    connect(m_password, &QLineEdit::returnPressed, this, [this] {
        if (m_ready) submit();
    });
    QTimer::singleShot(0, this, &PortalDialog::detect);
}

void PortalDialog::setPortalUrl(const QUrl &url)
{
    if (!url.isEmpty()) m_url->setText(url.toString());
}

void PortalDialog::setCredentials(const QString &user, const QString &password)
{
    m_user->setText(user);
    m_password->setText(password);
}

void PortalDialog::autoLogin(const QUrl &url, const QString &user, const QString &password)
{
    setPortalUrl(url);
    setCredentials(user, password);
    m_autoSubmit = true;
    fetchPage();
}

void PortalDialog::setBusy(bool busy)
{
    m_detect->setEnabled(!busy);
    m_fetch->setEnabled(!busy);
    m_login->setEnabled(!busy && m_ready);
}

void PortalDialog::log(const QString &text)
{
    m_log->appendPlainText(text);
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void PortalDialog::detect()
{
    m_status->setText(QStringLiteral("门户：检测中…"));
    probe();
}

void PortalDialog::probe()
{
    setBusy(true);
    auto *reply = mlk::startPortalProbe(m_net);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleProbe(reply); });
}

void PortalDialog::handleProbe(QNetworkReply *reply)
{
    QUrl portalUrl;
    QString detail;
    const auto state = mlk::classifyPortal(reply, &portalUrl, &detail);
    reply->deleteLater();
    setBusy(false);
    switch (state) {
    case mlk::PortalState::Online:
        m_status->setText(QStringLiteral("门户：已联网，无需认证"));
        log(QStringLiteral("探测通过（HTTP 204），网络可直接使用"));
        emit portalOnline();
        break;
    case mlk::PortalState::LoginRequired:
        m_portalUrl = portalUrl;
        if (m_url->text().trimmed().isEmpty()) m_url->setText(portalUrl.toString());
        m_status->setText(QStringLiteral("门户：需要登录"));
        log(QStringLiteral("探测被门户接管：%1").arg(portalUrl.toString()));
        break;
    case mlk::PortalState::Offline:
        m_status->setText(QStringLiteral("门户：链路或地址不可用"));
        log(QStringLiteral("探测失败：%1").arg(detail));
        break;
    }
}

void PortalDialog::fetchPage()
{
    QUrl url = QUrl::fromUserInput(m_url->text().trimmed());
    if (url.isEmpty() || !url.isValid()) {
        m_status->setText(QStringLiteral("门户：地址无效"));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "mlk-portal/1");
    setBusy(true);
    m_status->setText(QStringLiteral("门户：正在获取登录页…"));
    auto *reply = m_net->get(request);
    QTimer::singleShot(10000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handlePage(reply); });
}

void PortalDialog::handlePage(QNetworkReply *reply)
{
    const QUrl url = reply->url();
    const QByteArray body = reply->readAll();
    const bool failed = reply->error() != QNetworkReply::NoError;
    const QString error = reply->errorString();
    reply->deleteLater();
    setBusy(false);
    if (failed && body.isEmpty()) {
        m_status->setText(QStringLiteral("门户：登录页获取失败"));
        log(QStringLiteral("获取登录页失败：%1").arg(error));
        return;
    }
    if (body.isEmpty()) {
        m_status->setText(QStringLiteral("门户：登录页为空"));
        log(QStringLiteral("登录页返回空内容：%1").arg(url.toString()));
        return;
    }
    formFrom(QString::fromUtf8(body), url);
    if (!m_form.hasPassword || m_form.action.isEmpty()) {
        m_ready = false;
        m_login->setEnabled(false);
        m_status->setText(QStringLiteral("门户：未识别到可提交的登录表单"));
        log(QStringLiteral("已获取 %1（%2 字节），但未找到含密码输入的表单；"
                           "该门户可能需要 JavaScript 或专用客户端认证")
                .arg(url.toString()).arg(body.size()));
        return;
    }
    m_ready = true;
    m_login->setEnabled(true);
    m_portalUrl = url;
    if (m_autoSubmit) {
        m_autoSubmit = false;
        log(QStringLiteral("自动登录：提交已保存的账号密码"));
        submit();
        return;
    }
    m_status->setText(QStringLiteral("门户：已识别登录表单，可提交账号密码"));
    QStringList names;
    for (const auto &field : m_form.fields) names << field.name;
    log(QStringLiteral("表单地址：%1（%2）").arg(m_form.action.toString(), m_form.method.toUpper()));
    log(QStringLiteral("表单字段：%1").arg(names.join(QStringLiteral(", "))));
}

void PortalDialog::formFrom(const QString &html, const QUrl &base)
{
    m_form = {};
    static const QRegularExpression formRe(
        QStringLiteral("<form\\b([^>]*)>([\\s\\S]*?)</form>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression inputRe(
        QStringLiteral("<input\\b([^>]*)>"), QRegularExpression::CaseInsensitiveOption);
    auto forms = formRe.globalMatch(html);
    Form fallback;
    while (forms.hasNext()) {
        const auto match = forms.next();
        Form form;
        const QString action = attribute(match.captured(1), QStringLiteral("action")).trimmed();
        form.action = action.isEmpty() ? base : base.resolved(QUrl(action));
        form.method = attribute(match.captured(1), QStringLiteral("method")).toLower();
        if (form.method != QStringLiteral("get")) form.method = QStringLiteral("post");
        auto inputs = inputRe.globalMatch(match.captured(2));
        while (inputs.hasNext()) {
            const QString tag = inputs.next().captured(1);
            const QString type = attribute(tag, QStringLiteral("type")).toLower();
            const QString name = attribute(tag, QStringLiteral("name")).trimmed();
            if (name.isEmpty()) continue;
            if (type == QStringLiteral("submit") || type == QStringLiteral("button")
                || type == QStringLiteral("image") || type == QStringLiteral("reset"))
                continue;
            if (type == QStringLiteral("password")) form.hasPassword = true;
            form.fields.append({name, attribute(tag, QStringLiteral("value")), type});
        }
        if (form.fields.isEmpty()) continue;
        if (form.hasPassword) { m_form = form; return; }
        if (fallback.fields.isEmpty()) fallback = form;
    }
    m_form = fallback;
}

void PortalDialog::submit()
{
    if (!m_ready || m_form.action.isEmpty()) return;
    static const QRegularExpression userRe(
        QStringLiteral("user|account|uname|login|name|email|phone|mobile|id"),
        QRegularExpression::CaseInsensitiveOption);
    QUrlQuery query;
    bool userUsed = false;
    for (const auto &field : m_form.fields) {
        QString value = field.value;
        const bool passwordField = field.type == QStringLiteral("password")
            || field.name.contains(QStringLiteral("pass"), Qt::CaseInsensitive);
        const bool textField = field.type == QStringLiteral("text")
            || field.type == QStringLiteral("email") || field.type == QStringLiteral("tel")
            || field.type == QStringLiteral("search") || field.type.isEmpty();
        if (passwordField && !field.type.isEmpty()) {
            value = m_password->text();
        } else if (passwordField) {
            value = m_password->text();
        } else if (textField && !userUsed && userRe.match(field.name).hasMatch()) {
            value = m_user->text();
            userUsed = true;
        } else if (textField && !userUsed && field.name.contains(QStringLiteral("user"),
                                                                 Qt::CaseInsensitive)) {
            value = m_user->text();
            userUsed = true;
        }
        query.addQueryItem(field.name, value);
    }
    QUrl target = m_form.action;
    QNetworkRequest request(target);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "mlk-portal/1");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    const QByteArray body = query.toString(QUrl::FullyEncoded).toUtf8();
    setBusy(true);
    m_status->setText(QStringLiteral("门户：正在提交…"));
    log(QStringLiteral("提交登录表单：%1 字段 %2 个").arg(target.toString()).arg(m_form.fields.size()));
    QNetworkReply *reply = nullptr;
    if (m_form.method == QStringLiteral("get")) {
        target.setQuery(query);
        reply = m_net->get(QNetworkRequest(target));
    } else {
        reply = m_net->post(request, body);
    }
    QTimer::singleShot(12000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleSubmit(reply); });
}

void PortalDialog::handleSubmit(QNetworkReply *reply)
{
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString error = reply->errorString();
    const QUrl finalUrl = reply->url();
    reply->deleteLater();
    log(QStringLiteral("提交结果：HTTP %1，%2 字节%3")
            .arg(code).arg(body.size())
            .arg(error.isEmpty() ? QString() : QStringLiteral("，") + error));
    if (finalUrl.isValid()) m_url->setText(finalUrl.toString());
    QTimer::singleShot(600, this, &PortalDialog::probe);
}
