#include "networksettingsdialog.h"

#include "ifaddr.h"
#include "captiveportal.h"
#include "portaldialog.h"
#include "wifidialog.h"

#include <QtWidgets>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>

namespace {

const char *kNetworkDir = "/sys/class/net";
const char *kApnPath = "/var/lib/mlk/4g.conf";
const char *kPortalPath = "/var/lib/mlk/ethernet.conf";
const char *kPppoePath = "/var/lib/mlk/pppoe.conf";

QString readLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(file.readAll()).trimmed();
}

bool isWireless(const QString &iface)
{
    return QFile::exists(QStringLiteral("%1/%2/wireless").arg(QLatin1String(kNetworkDir), iface));
}

bool carrierUp(const QString &iface)
{
    return readLine(QStringLiteral("%1/%2/carrier").arg(QLatin1String(kNetworkDir), iface))
        == QStringLiteral("1");
}

QStringList ethernetInterfaces()
{
    QStringList result;
    for (const auto &name : QDir(QLatin1String(kNetworkDir))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (isWireless(name)) continue;
        if (name.startsWith(QLatin1String("eth")) || name.startsWith(QLatin1String("en")))
            result << name;
    }
    return result;
}

QStringList wirelessInterfaces()
{
    QStringList result;
    for (const auto &name : QDir(QLatin1String(kNetworkDir))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (isWireless(name)) result << name;
    return result;
}

QString driverOf(const QString &iface)
{
    const QString link = QStringLiteral("%1/%2/device/driver").arg(QLatin1String(kNetworkDir), iface);
    return QFileInfo(link).symLinkTarget().section(QLatin1Char('/'), -1);
}

QStringList cellularInterfaces()
{
    QStringList result;
    for (const auto &name : QDir(QLatin1String(kNetworkDir))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (name.startsWith(QLatin1String("wwan")) || name.startsWith(QLatin1String("ppp"))
            || name.startsWith(QLatin1String("rmnet"))) {
            result << name;
            continue;
        }
        if (!name.startsWith(QLatin1String("usb"))) continue;
        const auto driver = driverOf(name);
        if (driver.compare(QLatin1String("GobiNet"), Qt::CaseInsensitive) == 0
            || driver == QLatin1String("qmi_wwan") || driver == QLatin1String("cdc_mbim"))
            result << name;
    }
    return result;
}

QString resolveProgram(const QString &name)
{
    if (name.contains(QLatin1Char('/'))) return QFile::exists(name) ? name : QString();
    for (const auto &dir : {"/sbin", "/usr/sbin", "/bin", "/usr/bin"}) {
        const QString path = QStringLiteral("%1/%2").arg(QLatin1String(dir), name);
        if (QFile::exists(path)) return path;
    }
    return {};
}

QString runCommand(const QString &program, const QStringList &arguments, int timeoutMs = 8000)
{
    if (program.isEmpty()) return {};
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) return {};
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput() + process.readAllStandardError())
        .trimmed();
}

QString ipv4Address(const QString &iface)
{
    const auto output = runCommand(resolveProgram(QStringLiteral("ip")),
                                   {QStringLiteral("-4"), QStringLiteral("-o"),
                                    QStringLiteral("addr"), QStringLiteral("show"),
                                    QStringLiteral("dev"), iface});
    return mlk::ipv4AddressFromIpOutput(output);
}

bool hasDefaultRoute(const QString &iface)
{
    const auto output = runCommand(resolveProgram(QStringLiteral("ip")),
                                   {QStringLiteral("-4"), QStringLiteral("route"),
                                    QStringLiteral("show"), QStringLiteral("default")});
    return output.contains(QStringLiteral(" dev %1").arg(iface));
}

bool processRunning(const QString &name)
{
    for (const auto &entry : QDir(QStringLiteral("/proc"))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        bool numeric = false;
        entry.toInt(&numeric);
        if (!numeric) continue;
        const auto comm = readLine(QStringLiteral("/proc/%1/comm").arg(entry));
        if (comm == name) return true;
    }
    return false;
}

QString loadApn()
{
    QFile file{QLatin1String(kApnPath)};
    if (!file.open(QIODevice::ReadOnly)) return {};
    for (const auto &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        const auto key = line.section(QLatin1Char('='), 0, 0).trimmed();
        if (key == QLatin1String("apn"))
            return line.section(QLatin1Char('='), 1).trimmed();
    }
    return {};
}

void saveApn(const QString &apn)
{
    QDir().mkpath(QStringLiteral("/var/lib/mlk"));
    QFile file{QLatin1String(kApnPath)};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QStringLiteral("apn=%1\n").arg(apn).toUtf8());
}


struct PortalSettings {
    QString url, user, password;
    bool automatic = false;
};

PortalSettings loadPortalSettings()
{
    PortalSettings settings;
    QFile file{QLatin1String(kPortalPath)};
    if (!file.open(QIODevice::ReadOnly)) return settings;
    for (const auto &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        const auto key = line.section(QLatin1Char('='), 0, 0).trimmed();
        const auto value = line.section(QLatin1Char('='), 1).trimmed();
        if (key == QLatin1String("portal_url")) settings.url = value;
        else if (key == QLatin1String("portal_user")) settings.user = value;
        else if (key == QLatin1String("portal_password")) settings.password = value;
        else if (key == QLatin1String("portal_auto")) settings.automatic = value == QLatin1String("1");
    }
    return settings;
}

void savePortalSettingsFile(const PortalSettings &settings)
{
    QDir().mkpath(QStringLiteral("/var/lib/mlk"));
    QFile file{QLatin1String(kPortalPath)};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QString text;
    text += QStringLiteral("portal_url=%1\n").arg(settings.url);
    text += QStringLiteral("portal_user=%1\n").arg(settings.user);
    text += QStringLiteral("portal_password=%1\n").arg(settings.password);
    text += QStringLiteral("portal_auto=%1\n").arg(settings.automatic ? 1 : 0);
    file.write(text.toUtf8());
}

struct PppoeSettings {
    QString user, password, service, iface;
    bool enabled = false;
};

PppoeSettings loadPppoeSettings()
{
    PppoeSettings settings;
    settings.iface = QStringLiteral("eth0");
    QFile file{QLatin1String(kPppoePath)};
    if (!file.open(QIODevice::ReadOnly)) return settings;
    for (const auto &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        const auto key = line.section(QLatin1Char('='), 0, 0).trimmed();
        const auto value = line.section(QLatin1Char('='), 1).trimmed();
        if (key == QLatin1String("user")) settings.user = value;
        else if (key == QLatin1String("password")) settings.password = value;
        else if (key == QLatin1String("service")) settings.service = value;
        else if (key == QLatin1String("iface") && !value.isEmpty()) settings.iface = value;
        else if (key == QLatin1String("enabled")) settings.enabled = value == QLatin1String("1");
    }
    return settings;
}

void savePppoeSettingsFile(const PppoeSettings &settings)
{
    QDir().mkpath(QStringLiteral("/var/lib/mlk"));
    QFile file{QLatin1String(kPppoePath)};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QString text;
    text += QStringLiteral("user=%1\n").arg(settings.user);
    text += QStringLiteral("password=%1\n").arg(settings.password);
    text += QStringLiteral("service=%1\n").arg(settings.service);
    text += QStringLiteral("iface=%1\n").arg(settings.iface);
    text += QStringLiteral("enabled=%1\n").arg(settings.enabled ? 1 : 0);
    file.write(text.toUtf8());
}

bool gobiNetLoaded()
{
    QFile file(QStringLiteral("/proc/modules"));
    if (!file.open(QIODevice::ReadOnly)) return false;
    return QString::fromUtf8(file.readAll()).contains(QStringLiteral("GobiNet "));
}

QString pppoeInterfaceAddress()
{
    const auto address = ipv4Address(QStringLiteral("ppp0"));
    return address;
}

} // namespace

NetworkSettingsDialog::NetworkSettingsDialog(QWidget *parent)
    : QDialog(parent), m_net(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("networkSettingsDialog"));
    setWindowTitle(QStringLiteral("网络配置"));
    setWindowModality(Qt::WindowModal);
    resize(1100, 720);

    auto *root = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;

    auto *left = new QVBoxLayout;
    auto *sectionTitle = new QLabel(QStringLiteral("网络通道"), this);
    m_sections = new QListWidget(this);
    m_sections->setObjectName(QStringLiteral("networkSectionList"));
    m_sections->addItems({QStringLiteral("千兆网"), QStringLiteral("4G"), QStringLiteral("WiFi")});
    m_sections->setMinimumWidth(190);
    m_sections->setMaximumWidth(250);
    left->addWidget(sectionTitle);
    left->addWidget(m_sections, 1);
    body->addLayout(left);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("networkPages"));
    m_pages->addWidget(createEthernetPage());
    m_pages->addWidget(createCellularPage());
    m_pages->addWidget(createWifiPage());
    body->addWidget(m_pages, 1);
    root->addLayout(body, 1);

    auto *foot = new QHBoxLayout;
    m_priority = new QLabel(this);
    m_priority->setObjectName(QStringLiteral("networkPriority"));
    m_priority->setTextFormat(Qt::PlainText);
    m_priority->setWordWrap(true);
    m_priority->setText(QStringLiteral(
        "连接优先级：千兆网 > 4G > WiFi。高优先级链路获得地址后，其余链路不参与"
        "默认路由；拔掉网线后自动回落到 4G 或 WiFi。"));
    foot->addWidget(m_priority, 1);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setObjectName(QStringLiteral("networkSettingsClose"));
    foot->addWidget(closeButton);
    root->addLayout(foot);

    connect(closeButton, &QPushButton::clicked, this, &NetworkSettingsDialog::accept);
    connect(m_sections, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_sections->setCurrentRow(0);

    m_timer = new QTimer(this);
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &NetworkSettingsDialog::refresh);
    m_timer->start();
    refresh();
}

QWidget *NetworkSettingsDialog::createEthernetPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("ethernetPage"));
    auto *layout = new QVBoxLayout(page);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("网口"), page));
    m_ethInterface = new QComboBox(page);
    m_ethInterface->setObjectName(QStringLiteral("ethernetInterface"));
    m_ethInterface->setMinimumWidth(120);
    row->addWidget(m_ethInterface);
    m_ethEnable = new QPushButton(QStringLiteral("启用并获取地址"), page);
    m_ethEnable->setObjectName(QStringLiteral("ethernetEnable"));
    m_ethRenew = new QPushButton(QStringLiteral("重新获取地址"), page);
    m_ethRenew->setObjectName(QStringLiteral("ethernetRenew"));
    row->addWidget(m_ethEnable);
    row->addWidget(m_ethRenew);
    row->addStretch(1);
    layout->addLayout(row);

    m_ethStatus = new QLabel(page);
    m_ethStatus->setObjectName(QStringLiteral("ethernetStatus"));
    m_ethStatus->setTextFormat(Qt::PlainText);
    m_ethStatus->setWordWrap(true);
    layout->addWidget(m_ethStatus);

    auto *portalRow = new QHBoxLayout;
    m_ethPortal = new QLabel(QStringLiteral("门户：未检测"), page);
    m_ethPortal->setObjectName(QStringLiteral("ethernetPortal"));
    m_ethPortal->setTextFormat(Qt::PlainText);
    m_ethPortal->setWordWrap(true);
    m_portalDetect = new QPushButton(QStringLiteral("检测门户"), page);
    m_portalDetect->setObjectName(QStringLiteral("ethernetPortalDetect"));
    m_portalLogin = new QPushButton(QStringLiteral("门户登录…"), page);
    m_portalLogin->setObjectName(QStringLiteral("ethernetPortalLogin"));
    m_portalLogin->setEnabled(false);
    portalRow->addWidget(m_ethPortal, 1);
    portalRow->addWidget(m_portalDetect);
    portalRow->addWidget(m_portalLogin);
    layout->addLayout(portalRow);

    const auto saved = loadPortalSettings();
    auto *portalForm = new QFormLayout;
    m_portalUrlEdit = new QLineEdit(saved.url, page);
    m_portalUrlEdit->setObjectName(QStringLiteral("ethernetPortalUrl"));
    m_portalUrlEdit->setPlaceholderText(
        QStringLiteral("留空使用默认网关"));
    m_portalUser = new QLineEdit(saved.user, page);
    m_portalUser->setObjectName(QStringLiteral("ethernetPortalUser"));
    m_portalUser->setPlaceholderText(QStringLiteral("手机号或账号"));
    m_portalPassword = new QLineEdit(saved.password, page);
    m_portalPassword->setObjectName(QStringLiteral("ethernetPortalPassword"));
    m_portalPassword->setEchoMode(QLineEdit::Password);
    m_portalAuto = new QCheckBox(QStringLiteral("地址获取后自动登录门户"), page);
    m_portalAuto->setObjectName(QStringLiteral("ethernetPortalAuto"));
    m_portalAuto->setChecked(saved.automatic);
    m_portalSave = new QPushButton(QStringLiteral("保存门户凭据"), page);
    m_portalSave->setObjectName(QStringLiteral("ethernetPortalSave"));
    portalForm->addRow(QStringLiteral("门户地址"), m_portalUrlEdit);
    portalForm->addRow(QStringLiteral("门户账号"), m_portalUser);
    portalForm->addRow(QStringLiteral("门户密码"), m_portalPassword);
    portalForm->addRow(QString(), m_portalAuto);
    portalForm->addRow(QString(), m_portalSave);
    layout->addLayout(portalForm);

    const auto pppoe = loadPppoeSettings();
    auto *pppoeForm = new QFormLayout;
    m_pppoeEnable = new QCheckBox(QStringLiteral("此网口使用 PPPoE 拨号（校园网）"), page);
    m_pppoeEnable->setObjectName(QStringLiteral("pppoeEnable"));
    m_pppoeEnable->setChecked(pppoe.enabled);
    m_pppoeUser = new QLineEdit(pppoe.user, page);
    m_pppoeUser->setObjectName(QStringLiteral("pppoeUser"));
    m_pppoeUser->setPlaceholderText(QStringLiteral("宽带账号（通常为手机号）"));
    m_pppoePassword = new QLineEdit(pppoe.password, page);
    m_pppoePassword->setObjectName(QStringLiteral("pppoePassword"));
    m_pppoePassword->setEchoMode(QLineEdit::Password);
    m_pppoeService = new QLineEdit(pppoe.service, page);
    m_pppoeService->setObjectName(QStringLiteral("pppoeService"));
    m_pppoeService->setPlaceholderText(QStringLiteral("服务名，可留空"));
    pppoeForm->addRow(QString(), m_pppoeEnable);
    pppoeForm->addRow(QStringLiteral("PPPoE 账号"), m_pppoeUser);
    pppoeForm->addRow(QStringLiteral("PPPoE 密码"), m_pppoePassword);
    pppoeForm->addRow(QStringLiteral("服务名"), m_pppoeService);
    layout->addLayout(pppoeForm);

    auto *pppoeRow = new QHBoxLayout;
    m_pppoeStatus = new QLabel(page);
    m_pppoeStatus->setObjectName(QStringLiteral("pppoeStatus"));
    m_pppoeStatus->setTextFormat(Qt::PlainText);
    m_pppoeConnect = new QPushButton(QStringLiteral("PPPoE 连接"), page);
    m_pppoeConnect->setObjectName(QStringLiteral("pppoeConnect"));
    m_pppoeDisconnect = new QPushButton(QStringLiteral("断开"), page);
    m_pppoeDisconnect->setObjectName(QStringLiteral("pppoeDisconnect"));
    pppoeRow->addWidget(m_pppoeStatus, 1);
    pppoeRow->addWidget(m_pppoeConnect);
    pppoeRow->addWidget(m_pppoeDisconnect);
    layout->addLayout(pppoeRow);

    connect(m_pppoeEnable, &QCheckBox::toggled, this, [this](bool) {
        savePppoeSettings();
        refreshPppoe();
    });
    for (auto *edit : {m_pppoeUser, m_pppoePassword, m_pppoeService}) {
        connect(edit, &QLineEdit::editingFinished, this, &NetworkSettingsDialog::savePppoeSettings);
    }
    connect(m_pppoeConnect, &QPushButton::clicked, this, &NetworkSettingsDialog::startPppoe);
    connect(m_pppoeDisconnect, &QPushButton::clicked, this, &NetworkSettingsDialog::stopPppoe);

    auto *note = new QLabel(page);
    note->setWordWrap(true);
    note->setTextFormat(Qt::PlainText);
    note->setText(QStringLiteral(
        "PS 端千兆网口（设备树 eqos0，YT8531 PHY）。门户凭据保存在 "
        "/var/lib/mlk/ethernet.conf，PPPoE 凭据保存在 /var/lib/mlk/pppoe.conf。"));
    layout->addWidget(note);
    layout->addStretch(1);

    connect(m_ethEnable, &QPushButton::clicked, this, &NetworkSettingsDialog::enableEthernet);
    connect(m_ethRenew, &QPushButton::clicked, this, &NetworkSettingsDialog::renewEthernet);
    connect(m_portalDetect, &QPushButton::clicked, this, &NetworkSettingsDialog::detectPortal);
    connect(m_portalLogin, &QPushButton::clicked, this, &NetworkSettingsDialog::openPortalLogin);
    connect(m_portalSave, &QPushButton::clicked, this, [this] {
        savePortalSettings();
        m_ethPortal->setText(QStringLiteral("门户：凭据已保存"));
    });
    connect(m_ethInterface, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refreshEthernet(); });
    return page;
}

QWidget *NetworkSettingsDialog::createCellularPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("cellularPage"));
    auto *layout = new QVBoxLayout(page);

    m_cellStatus = new QLabel(page);
    m_cellStatus->setObjectName(QStringLiteral("cellularStatus"));
    m_cellStatus->setTextFormat(Qt::PlainText);
    m_cellStatus->setWordWrap(true);
    layout->addWidget(m_cellStatus);

    auto *apnRow = new QHBoxLayout;
    apnRow->addWidget(new QLabel(QStringLiteral("APN"), page));
    m_apn = new QLineEdit(loadApn(), page);
    m_apn->setObjectName(QStringLiteral("cellularApn"));
    m_apn->setPlaceholderText(QStringLiteral("留空使用模块默认；例如 cmnet / 3gnet / ctnet"));
    apnRow->addWidget(m_apn, 1);
    layout->addLayout(apnRow);

    auto *buttons = new QHBoxLayout;
    m_dial = new QPushButton(QStringLiteral("拨号"), page);
    m_dial->setObjectName(QStringLiteral("cellularDial"));
    m_hangup = new QPushButton(QStringLiteral("断开"), page);
    m_hangup->setObjectName(QStringLiteral("cellularHangup"));
    buttons->addWidget(m_dial);
    buttons->addWidget(m_hangup);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto *note = new QLabel(page);
    note->setWordWrap(true);
    note->setTextFormat(Qt::PlainText);
    note->setText(QStringLiteral(
        "4G 模块接口 wwan0（GobiNet / QMI）。APN 保存在 /var/lib/mlk/4g.conf。"));
    layout->addWidget(note);
    layout->addStretch(1);

    connect(m_dial, &QPushButton::clicked, this, &NetworkSettingsDialog::dialCellular);
    connect(m_hangup, &QPushButton::clicked, this, &NetworkSettingsDialog::hangupCellular);
    return page;
}

QWidget *NetworkSettingsDialog::createWifiPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("wifiPage"));
    auto *layout = new QVBoxLayout(page);
    auto *title = new QLabel(QStringLiteral("WiFi 配置"), page);
    QFont font = title->font();
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto *note = new QLabel(page);
    note->setWordWrap(true);
    note->setTextFormat(Qt::PlainText);
    note->setText(QStringLiteral(
        "WiFi 接口 wlan0（AIC8800）。凭据由 wpa_supplicant 保存在 "
        "/var/lib/mlk/wpa-wlan0.conf。"));
    layout->addWidget(note);
    m_wifi = new WifiDialog(page, QStringLiteral("/var/run/wpa_supplicant"), {}, true);
    layout->addWidget(m_wifi, 1);
    return page;
}

void NetworkSettingsDialog::refresh()
{
    reloadInterfaces();
    refreshEthernet();
    refreshCellular();
    refreshPppoe();
    refreshSections();
}

void NetworkSettingsDialog::reloadInterfaces()
{
    const auto ethernet = ethernetInterfaces();
    QStringList current;
    for (int i = 0; i < m_ethInterface->count(); ++i) current << m_ethInterface->itemText(i);
    if (current != ethernet) {
        const auto selected = m_ethInterface->currentText();
        m_ethInterface->blockSignals(true);
        m_ethInterface->clear();
        m_ethInterface->addItems(ethernet);
        const int index = ethernet.indexOf(selected);
        m_ethInterface->setCurrentIndex(index >= 0 ? index : 0);
        m_ethInterface->blockSignals(false);
    }
}

QString NetworkSettingsDialog::selectedEthernet() const
{
    return m_ethInterface->currentText().trimmed();
}

QString NetworkSettingsDialog::selectedCellular() const
{
    const auto interfaces = cellularInterfaces();
    return interfaces.isEmpty() ? QString() : interfaces.first();
}

QString NetworkSettingsDialog::selectedWifi() const
{
    const auto interfaces = wirelessInterfaces();
    return interfaces.isEmpty() ? QString() : interfaces.first();
}

void NetworkSettingsDialog::refreshEthernet()
{
    const auto iface = selectedEthernet();
    if (iface.isEmpty()) {
        m_ethStatus->setText(QStringLiteral(
            "未发现以太网口。确认 PS 网口 PHY 已上电、设备树 eqos0 为 okay。"));
        m_ethEnable->setEnabled(false);
        m_ethRenew->setEnabled(false);
        m_portalDetect->setEnabled(false);
        return;
    }
    m_ethEnable->setEnabled(true);
    m_ethRenew->setEnabled(true);
    m_portalDetect->setEnabled(true);
    const bool carrier = carrierUp(iface);
    const auto speed = readLine(QStringLiteral("%1/%2/speed").arg(QLatin1String(kNetworkDir), iface));
    const auto duplex = readLine(QStringLiteral("%1/%2/duplex").arg(QLatin1String(kNetworkDir), iface));
    const auto mac = readLine(QStringLiteral("%1/%2/address").arg(QLatin1String(kNetworkDir), iface));
    const auto ipv4 = ipv4Address(iface);
    QString extra;
    if (carrier && !speed.isEmpty() && speed != QLatin1String("-1"))
        extra = QStringLiteral("（%1 Mb/s%2）").arg(
            speed, duplex == QLatin1String("full") ? QStringLiteral("，全双工") : QString());
    QStringList lines;
    lines << QStringLiteral("网口：%1").arg(iface);
    lines << (carrier
                  ? QStringLiteral("链路：已连接%1").arg(extra)
                  : QStringLiteral("链路：未连接（网线未插或对端未上电）"));
    lines << QStringLiteral("MAC：%1").arg(mac);
    lines << (ipv4.isEmpty() ? QStringLiteral("IPv4：未获取；点击「启用并获取地址」")
                             : QStringLiteral("IPv4：%1").arg(ipv4));
    lines << QStringLiteral("默认路由：%1").arg(hasDefaultRoute(iface) ? QStringLiteral("有")
                                                                  : QStringLiteral("无"));
    if (m_pppoeEnable && m_pppoeEnable->isChecked()) {
        const auto pppoeAddress = pppoeInterfaceAddress();
        if (!pppoeAddress.isEmpty())
            lines << QStringLiteral("PPPoE：已连接 %1").arg(pppoeAddress);
    }
    m_ethStatus->setText(lines.join(QLatin1Char('\n')));
    if (m_pppoeEnable && m_pppoeEnable->isChecked()) {
        m_ethPortal->setText(QStringLiteral("门户：PPPoE 模式跳过门户认证"));
        m_portalLogin->setEnabled(false);
        refreshPppoe();
        return;
    }
    if (ipv4.isEmpty()) {
        m_ethPortal->setText(QStringLiteral("门户：等待地址"));
        m_portalLogin->setEnabled(false);
        m_autoDetected = false;
    } else {
        m_portalLogin->setEnabled(true);
        if (!m_autoDetected && !m_probing) {
            m_autoDetected = true;
            detectPortal();
        }
    }
}

void NetworkSettingsDialog::refreshCellular()
{
    const auto interfaces = cellularInterfaces();
    const auto running = processRunning(QStringLiteral("quectel-CM"));
    const bool moduleLoaded = gobiNetLoaded();
    QStringList lines;
    lines << QStringLiteral("模块：%1").arg(moduleLoaded ? QStringLiteral("GobiNet 已加载")
                                                     : QStringLiteral("GobiNet 未加载"));
    if (interfaces.isEmpty()) {
        lines << QStringLiteral("未发现 4G 网络接口（wwan / ppp / rmnet，或 GobiNet 驱动的 usb 口）。");
    } else {
        const auto &iface = interfaces.first();
        const auto ipv4 = ipv4Address(iface);
        lines << QStringLiteral("接口：%1").arg(interfaces.join(QStringLiteral(", ")));
        lines << (ipv4.isEmpty() ? QStringLiteral("IPv4：未获取")
                                 : QStringLiteral("IPv4：%1").arg(ipv4));
        lines << QStringLiteral("默认路由：%1").arg(hasDefaultRoute(iface) ? QStringLiteral("有")
                                                                    : QStringLiteral("无"));
    }
    lines << QStringLiteral("拨号进程：%1").arg(running ? QStringLiteral("quectel-CM 运行中")
                                                      : QStringLiteral("未运行"));
    m_cellStatus->setText(lines.join(QLatin1Char('\n')));
    m_dial->setEnabled(!running);
    m_hangup->setEnabled(running);
}

void NetworkSettingsDialog::refreshSections()
{
    const auto eth = selectedEthernet();
    const auto cellular = selectedCellular();
    const auto wifi = selectedWifi();
    const auto ethAddress = eth.isEmpty() ? QString() : ipv4Address(eth);
    const auto cellAddress = cellular.isEmpty() ? QString() : ipv4Address(cellular);
    const auto wifiAddress = wifi.isEmpty() ? QString() : ipv4Address(wifi);

    QString ethText = QStringLiteral("千兆网");
    if (eth.isEmpty()) ethText += QStringLiteral("（不可用）");
    else if (!ethAddress.isEmpty()) ethText += QStringLiteral("（已连接）");
    else if (carrierUp(eth)) ethText += QStringLiteral("（无地址）");
    else ethText += QStringLiteral("（未接）");

    QString cellText = QStringLiteral("4G");
    if (cellular.isEmpty())
        cellText += gobiNetLoaded() ? QStringLiteral("（未连接）") : QStringLiteral("（无模块）");
    else if (!cellAddress.isEmpty()) cellText += QStringLiteral("（已连接）");
    else cellText += QStringLiteral("（未拨号）");

    QString wifiText = QStringLiteral("WiFi");
    if (wifi.isEmpty()) wifiText += QStringLiteral("（不可用）");
    else if (!wifiAddress.isEmpty()) wifiText += QStringLiteral("（已连接）");
    else wifiText += QStringLiteral("（未连接）");

    const QStringList texts{ethText, cellText, wifiText};
    for (int i = 0; i < m_sections->count() && i < texts.size(); ++i)
        m_sections->item(i)->setText(texts[i]);

    const QString winner = !ethAddress.isEmpty() ? QStringLiteral("千兆网 %1").arg(eth)
        : !cellAddress.isEmpty() ? QStringLiteral("4G %1").arg(cellular)
        : !wifiAddress.isEmpty() ? QStringLiteral("WiFi %1").arg(wifi)
        : QStringLiteral("未连接");
    m_priority->setText(QStringLiteral("当前生效：%1（优先级 千兆网 > 4G > WiFi）").arg(winner));
}

void NetworkSettingsDialog::enableEthernet()
{
    if (m_pppoeEnable && m_pppoeEnable->isChecked()) { startPppoe(); return; }
    const auto iface = selectedEthernet();
    if (iface.isEmpty()) return;
    runCommand(resolveProgram(QStringLiteral("ip")),
               {QStringLiteral("link"), QStringLiteral("set"),
                QStringLiteral("dev"), iface, QStringLiteral("up")});
    const auto dhcpcd = resolveProgram(QStringLiteral("dhcpcd"));
    if (!dhcpcd.isEmpty() && processRunning(QStringLiteral("dhcpcd"))) {
        runCommand(dhcpcd, {QStringLiteral("-n"), iface});
        m_ethStatus->setText(QStringLiteral("已请求 dhcpcd 更新地址…"));
        QTimer::singleShot(2500, this, &NetworkSettingsDialog::refresh);
        return;
    }
    const auto udhcpc = resolveProgram(QStringLiteral("udhcpc"));
    if (udhcpc.isEmpty()) {
        m_ethStatus->setText(QStringLiteral("未找到 DHCP 客户端，无法获取地址。"));
        return;
    }
    QDir().mkpath(QStringLiteral("/var/run/mlk-net"));
    QProcess::startDetached(udhcpc, {QStringLiteral("-b"), QStringLiteral("-i"), iface,
                                     QStringLiteral("-p"),
                                     QStringLiteral("/var/run/mlk-net/%1.dhcp.pid").arg(iface),
                                     QStringLiteral("-s"),
                                     QStringLiteral("/usr/share/udhcpc/default.script")});
    m_ethStatus->setText(QStringLiteral("已启动 DHCP，等待地址分配…"));
    QTimer::singleShot(2500, this, &NetworkSettingsDialog::refresh);
}

void NetworkSettingsDialog::renewEthernet()
{
    if (m_pppoeEnable && m_pppoeEnable->isChecked()) { startPppoe(); return; }
    const auto iface = selectedEthernet();
    if (iface.isEmpty()) return;
    const auto dhcpcd = resolveProgram(QStringLiteral("dhcpcd"));
    if (!dhcpcd.isEmpty() && processRunning(QStringLiteral("dhcpcd"))) {
        runCommand(dhcpcd, {QStringLiteral("-n"), iface});
        m_ethStatus->setText(QStringLiteral("已请求 dhcpcd 续租地址…"));
        QTimer::singleShot(1500, this, &NetworkSettingsDialog::refresh);
        return;
    }
    const auto pid = readLine(QStringLiteral("/var/run/mlk-net/%1.dhcp.pid").arg(iface));
    if (!pid.isEmpty()) {
        runCommand(resolveProgram(QStringLiteral("kill")),
                   {QStringLiteral("-USR1"), pid});
        m_ethStatus->setText(QStringLiteral("已请求续租地址…"));
        QTimer::singleShot(1500, this, &NetworkSettingsDialog::refresh);
        return;
    }
    enableEthernet();
}

void NetworkSettingsDialog::detectPortal()
{
    if (m_probing) return;
    m_probing = true;
    m_portalDetect->setEnabled(false);
    m_ethPortal->setText(QStringLiteral("门户：检测中…"));
    auto *reply = mlk::startPortalProbe(m_net);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { handlePortalProbe(reply); });
}

void NetworkSettingsDialog::handlePortalProbe(QNetworkReply *reply)
{
    m_probing = false;
    m_portalDetect->setEnabled(true);
    QUrl portalUrl;
    QString detail;
    const auto state = mlk::classifyPortal(reply, &portalUrl, &detail);
    reply->deleteLater();
    switch (state) {
    case mlk::PortalState::Online:
        m_portalUrl = QUrl();
        m_ethPortal->setText(QStringLiteral("门户：已联网，无需认证"));
        m_portalLogin->setEnabled(false);
        break;
    case mlk::PortalState::LoginRequired: {
        m_portalUrl = portalUrl;
        m_ethPortal->setText(QStringLiteral("门户：需要登录（%1）").arg(portalUrl.toString()));
        m_portalLogin->setEnabled(true);
        const auto settings = loadPortalSettings();
        if (settings.automatic && !m_autoLoginTried && !settings.user.isEmpty()
            && !settings.password.isEmpty()) {
            m_autoLoginTried = true;
            autoPortalLogin();
        }
        break;
    }
    case mlk::PortalState::Offline: {
        m_portalUrl = QUrl();
        const auto gateway = gatewayOf(selectedEthernet());
        if (gateway.isEmpty()) {
            m_ethPortal->setText(QStringLiteral("门户：检测失败（%1）").arg(detail));
            m_portalLogin->setEnabled(!ipv4Address(selectedEthernet()).isEmpty());
        } else {
            m_portalUrl = QUrl(QStringLiteral("http://%1/").arg(gateway));
            m_portalLogin->setEnabled(true);
            if (!m_gatewayProbeTried) {
                m_gatewayProbeTried = true;
                m_probing = true;
                m_portalDetect->setEnabled(false);
                m_ethPortal->setText(
                    QStringLiteral("门户：探测网关 %1…").arg(m_portalUrl.toString()));
                auto *probe = mlk::startPortalProbe(m_net, m_portalUrl);
                connect(probe, &QNetworkReply::finished, this,
                        [this, probe] { handlePortalProbe(probe); });
            } else {
                m_ethPortal->setText(
                    QStringLiteral("门户：未探测到跳转（%1）；门户地址 %2")
                        .arg(detail, m_portalUrl.toString()));
            }
        }
        break;
    }
    }
}

QString NetworkSettingsDialog::gatewayOf(const QString &iface) const
{
    if (iface.isEmpty()) return {};
    const auto output = runCommand(resolveProgram(QStringLiteral("ip")),
                                   {QStringLiteral("-4"), QStringLiteral("route"),
                                    QStringLiteral("show"), QStringLiteral("default"),
                                    QStringLiteral("dev"), iface});
    const auto match = QRegularExpression(QStringLiteral("via\\s+([0-9.]+)")).match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

QUrl NetworkSettingsDialog::portalAddress() const
{
    const auto typed = m_portalUrlEdit->text().trimmed();
    if (!typed.isEmpty()) return QUrl::fromUserInput(typed);
    if (!m_portalUrl.isEmpty()) return m_portalUrl;
    const auto gateway = gatewayOf(selectedEthernet());
    if (!gateway.isEmpty()) return QUrl(QStringLiteral("http://%1/").arg(gateway));
    return {};
}

void NetworkSettingsDialog::openPortalLogin()
{
    const auto url = portalAddress();
    if (url.isEmpty()) {
        m_ethPortal->setText(QStringLiteral("门户：请先填写门户地址"));
        return;
    }
    PortalDialog dialog(this);
    dialog.setPortalUrl(url);
    dialog.setCredentials(m_portalUser->text().trimmed(), m_portalPassword->text());
    connect(&dialog, &PortalDialog::portalOnline, this, [this] {
        m_ethPortal->setText(QStringLiteral("门户：已联网"));
        m_portalLogin->setEnabled(false);
        savePortalSettings();
    });
    dialog.exec();
    refresh();
}

void NetworkSettingsDialog::autoPortalLogin()
{
    const auto settings = loadPortalSettings();
    const auto url = portalAddress();
    if (url.isEmpty() || settings.user.isEmpty() || settings.password.isEmpty()) return;
    m_ethPortal->setText(QStringLiteral("门户：使用已保存凭据自动登录…"));
    auto *dialog = new PortalDialog(this);
    dialog->setPortalUrl(url);
    dialog->setCredentials(settings.user, settings.password);
    connect(dialog, &PortalDialog::portalOnline, this, [this, dialog] {
        m_ethPortal->setText(QStringLiteral("门户：自动登录成功"));
        m_portalLogin->setEnabled(false);
        dialog->deleteLater();
    });
    QTimer::singleShot(20000, dialog, [dialog] { dialog->deleteLater(); });
    dialog->autoLogin(url, settings.user, settings.password);
}

void NetworkSettingsDialog::savePortalSettings()
{
    PortalSettings settings;
    settings.url = m_portalUrlEdit->text().trimmed();
    settings.user = m_portalUser->text().trimmed();
    settings.password = m_portalPassword->text();
    settings.automatic = m_portalAuto->isChecked();
    savePortalSettingsFile(settings);
}

void NetworkSettingsDialog::savePppoeSettings()
{
    if (!m_pppoeEnable) return;
    PppoeSettings settings;
    settings.user = m_pppoeUser->text().trimmed();
    settings.password = m_pppoePassword->text();
    settings.service = m_pppoeService->text().trimmed();
    settings.enabled = m_pppoeEnable->isChecked();
    const auto iface = selectedEthernet();
    settings.iface = iface.isEmpty() ? QStringLiteral("eth0") : iface;
    savePppoeSettingsFile(settings);
}

void NetworkSettingsDialog::refreshPppoe()
{
    if (!m_pppoeStatus) return;
    const auto settings = loadPppoeSettings();
    const auto address = pppoeInterfaceAddress();
    const bool running = processRunning(QStringLiteral("pppd"));
    QStringList lines;
    if (!address.isEmpty())
        lines << QStringLiteral("PPPoE：已连接 %1").arg(address);
    else if (running)
        lines << QStringLiteral("PPPoE：拨号中（尚未取得地址）");
    else if (settings.enabled)
        lines << QStringLiteral("PPPoE：未连接，点击「PPPoE 连接」");
    else
        lines << QStringLiteral("PPPoE：未启用；启用后该网口不再取 DHCP 地址");
    m_pppoeStatus->setText(lines.join(QLatin1Char('\n')));
    m_pppoeConnect->setEnabled(true);
    m_pppoeDisconnect->setEnabled(running || !address.isEmpty());
}

void NetworkSettingsDialog::startPppoe()
{
    if (m_pppoeEnable && !m_pppoeEnable->isChecked()) {
        const QSignalBlocker blocker(m_pppoeEnable);
        m_pppoeEnable->setChecked(true);
    }
    savePppoeSettings();
    const auto settings = loadPppoeSettings();
    if (settings.user.isEmpty()) {
        m_pppoeStatus->setText(QStringLiteral("PPPoE：请先填写账号"));
        return;
    }
    const auto tool = resolveProgram(QStringLiteral("mlk_pppoe"));
    if (tool.isEmpty()) {
        m_pppoeStatus->setText(QStringLiteral("PPPoE：镜像未包含 mlk_pppoe"));
        return;
    }
    m_pppoeStatus->setText(QStringLiteral("PPPoE：拨号中…"));
    // pppd 在后台常驻（persist），启动脚本自身最多等待 10 s；
    // 用 detached 启动避免界面阻塞，状态由 2 s 定时刷新跟进。
    if (!QProcess::startDetached(tool, {QStringLiteral("start")})) {
        m_pppoeStatus->setText(QStringLiteral("PPPoE：启动失败"));
        return;
    }
    QTimer::singleShot(1500, this, &NetworkSettingsDialog::refresh);
    QTimer::singleShot(5000, this, &NetworkSettingsDialog::refresh);
}

void NetworkSettingsDialog::stopPppoe()
{
    const auto tool = resolveProgram(QStringLiteral("mlk_pppoe"));
    if (tool.isEmpty()) return;
    runCommand(tool, {QStringLiteral("stop")});
    refresh();
}

void NetworkSettingsDialog::dialCellular()
{
    const auto apn = m_apn->text().trimmed();
    saveApn(apn);
    const auto program = resolveProgram(QStringLiteral("quectel-CM"));
    if (program.isEmpty()) {
        m_cellStatus->setText(QStringLiteral("未找到 quectel-CM，4G 拨号不可用。"));
        return;
    }
    QStringList arguments;
    if (!apn.isEmpty()) arguments << QStringLiteral("-s") << apn;
    if (!QProcess::startDetached(program, arguments)) {
        m_cellStatus->setText(QStringLiteral("4G 拨号进程启动失败。"));
        return;
    }
    m_cellStatus->setText(QStringLiteral("4G 拨号进程已启动；无 SIM 卡时会立即退出。"));
    QTimer::singleShot(2000, this, &NetworkSettingsDialog::refresh);
}

void NetworkSettingsDialog::hangupCellular()
{
    runCommand(resolveProgram(QStringLiteral("killall")), {QStringLiteral("quectel-CM")});
    const auto iface = selectedCellular();
    if (!iface.isEmpty())
        runCommand(resolveProgram(QStringLiteral("ip")),
                   {QStringLiteral("link"), QStringLiteral("set"),
                    QStringLiteral("dev"), iface, QStringLiteral("down")});
    m_cellStatus->setText(QStringLiteral("已断开 4G 拨号。"));
    QTimer::singleShot(1000, this, &NetworkSettingsDialog::refresh);
}
