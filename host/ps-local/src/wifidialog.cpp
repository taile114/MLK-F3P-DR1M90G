#include "wifidialog.h"
#include <QtWidgets>
#include <QSocketNotifier>
#include <QRandomGenerator>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

namespace {
QByteArray decodeSsid(const QByteArray &encoded) {
    QByteArray decoded;
    for (int i=0;i<encoded.size();++i) {
        if (encoded[i] != '\\' || i+1>=encoded.size()) { decoded.append(encoded[i]); continue; }
        const char escaped=encoded[++i];
        if (escaped=='x' && i+2<encoded.size()) {
            bool ok=false; const auto value=encoded.mid(i+1,2).toUInt(&ok,16);
            if (ok) { decoded.append(char(value)); i+=2; continue; }
        }
        if (escaped=='n') decoded.append('\n');
        else if (escaped=='r') decoded.append('\r');
        else if (escaped=='t') decoded.append('\t');
        else if (escaped=='e') decoded.append(char(27));
        else decoded.append(escaped);
    }
    return decoded;
}
}

WifiDialog::WifiDialog(QWidget *parent, const QString &controlDirectory, const QStringList &interfaces,
                       bool embedded)
    : QDialog(parent), m_controlDirectory(controlDirectory) {
    setWindowTitle(QStringLiteral("WiFi 配置"));
    if (embedded) setWindowFlags(Qt::Widget); else setWindowModality(Qt::WindowModal);
    resize(620, 480);
    auto *layout = new QVBoxLayout(this);
    auto *row = new QHBoxLayout;
    m_interface = new QComboBox(this);
    for (const auto &name : QDir(QStringLiteral("/sys/class/net")).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFile::exists(QStringLiteral("/sys/class/net/%1/wireless").arg(name))) m_interface->addItem(name);
    if (!interfaces.isEmpty()) { m_interface->clear(); m_interface->addItems(interfaces); }
    auto *search = new QPushButton(QStringLiteral("搜索 WiFi"), this);
    row->addWidget(m_interface); row->addWidget(search); layout->addLayout(row);
    m_results = new QListWidget(this); layout->addWidget(m_results,1);
    m_ssid = new QLineEdit(this); m_password = new QLineEdit(this); m_password->setEchoMode(QLineEdit::Password);
    m_ssid->setObjectName(QStringLiteral("wifiSsid")); m_password->setObjectName(QStringLiteral("wifiPassword"));
    auto *form = new QFormLayout; form->addRow(QStringLiteral("SSID"),m_ssid);
    form->addRow(QStringLiteral("密码（开放网络留空）"),m_password); layout->addLayout(form);
    m_status = new QLabel(QStringLiteral("等待搜索"),this); m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("wifiStatus"));
    m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    if (!embedded) {
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,this);
        auto *joinButton = buttons->addButton(QStringLiteral("连接"),QDialogButtonBox::ActionRole);
        joinButton->setObjectName(QStringLiteral("wifiJoin"));
        layout->addWidget(buttons); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
        connect(joinButton,&QPushButton::clicked,this,&WifiDialog::join);
    } else {
        auto *joinButton = new QPushButton(QStringLiteral("连接"),this);
        joinButton->setObjectName(QStringLiteral("wifiJoin"));
        layout->addWidget(joinButton);
        connect(joinButton,&QPushButton::clicked,this,&WifiDialog::join);
    }
    connect(search,&QPushButton::clicked,this,&WifiDialog::scan);
    connect(m_results,&QListWidget::currentItemChanged,this,[this](QListWidgetItem *item) {
        if (item) m_ssid->setText(item->data(Qt::UserRole).toString());
    });
    m_timeout = new QTimer(this); m_timeout->setSingleShot(true);
    m_joinTimeout = new QTimer(this); m_joinTimeout->setSingleShot(true);
    connect(m_joinTimeout,&QTimer::timeout,this,&WifiDialog::failedJoin);
    connect(m_timeout,&QTimer::timeout,this,[this] {
        m_status->setText(QStringLiteral("WiFi 服务超时；请关闭窗口后重试"));
        m_queue.clear(); m_busy = false; m_joining = false; m_joinTimeout->stop();
        if (m_notifier) m_notifier->setEnabled(false);
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    });
    auto *poll = new QTimer(this); poll->setInterval(2000);
    connect(poll,&QTimer::timeout,this,&WifiDialog::status); poll->start();
    QTimer::singleShot(0,this,&WifiDialog::scan);
}
WifiDialog::~WifiDialog() {
    if (m_fd >= 0) ::close(m_fd);
    if (!m_path.isEmpty()) ::unlink(m_path.constData());
}
void WifiDialog::command(const QByteArray &text, std::function<void(QByteArray)> done) {
    if (m_fd < 0) {
        m_status->setText(QStringLiteral("WiFi 不可用；检查 HOST 跳线、AIC 驱动和 wpa_supplicant 服务")); return;
    }
    if (m_queue.size() > 16) return;
    m_queue.enqueue({text,std::move(done)}); next();
}
void WifiDialog::next() {
    if (m_busy || m_queue.isEmpty() || m_fd < 0) return;
    m_busy = true;
    const auto &text = m_queue.head().text;
    if (::send(m_fd,text.constData(),size_t(text.size()),0) != text.size()) {
        m_busy = false; m_queue.clear(); m_joining = false; m_joinTimeout->stop(); m_status->setText(QStringLiteral("无法发送 WiFi 控制请求")); return;
    }
    m_timeout->start(3000);
}
void WifiDialog::receive() {
    char buffer[32768]; const auto n = ::recv(m_fd,buffer,sizeof(buffer),0);
    if (n <= 0 || !m_busy || m_queue.isEmpty()) return;
    m_timeout->stop(); auto request = m_queue.dequeue(); m_busy = false;
    request.done(QByteArray(buffer,int(n)).trimmed()); next();
}
void WifiDialog::scan() {
    if (m_joining || m_busy) return;
    if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
    if (m_fd >= 0) ::close(m_fd);
    if (!m_path.isEmpty()) ::unlink(m_path.constData());
    m_fd = ::socket(AF_UNIX,SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0);
    sockaddr_un local{}, peer{}; local.sun_family = peer.sun_family = AF_UNIX;
    m_path = QStringLiteral("/tmp/mlk-wifi-%1-%2").arg(::getpid()).arg(QRandomGenerator::global()->generate()).toLocal8Bit();
    std::strncpy(local.sun_path,m_path.constData(),sizeof(local.sun_path)-1);
    const auto endpoint = QDir(m_controlDirectory).filePath(m_interface->currentText()).toLocal8Bit();
    std::strncpy(peer.sun_path,endpoint.constData(),sizeof(peer.sun_path)-1);
    if (m_fd < 0 || ::bind(m_fd,reinterpret_cast<sockaddr *>(&local),sizeof(local)) < 0
        || ::connect(m_fd,reinterpret_cast<sockaddr *>(&peer),sizeof(peer)) < 0) {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
        m_status->setText(QStringLiteral("WiFi 服务不可用；检查驱动、无线接口和 wpa_supplicant")); return;
    }
    m_notifier = new QSocketNotifier(m_fd,QSocketNotifier::Read,this);
    connect(m_notifier,&QSocketNotifier::activated,this,[this] { receive(); });
    command("SCAN",[this](const QByteArray &reply) {
        if (reply != "OK") { m_status->setText(QStringLiteral("扫描失败或正在扫描，请重试")); return; }
        m_status->setText(QStringLiteral("正在搜索 WiFi…"));
        QTimer::singleShot(3000,this,[this] {
            command("SCAN_RESULTS",[this](const QByteArray &data) {
                m_results->clear(); const auto lines = data.split('\n');
                for (int i=1;i<lines.size();++i) {
                    const auto columns=lines[i].split('\t'); if(columns.size()<5) continue;
                    const auto ssid=QString::fromUtf8(decodeSsid(columns[4])); if(ssid.isEmpty()) continue;
                    auto *item=new QListWidgetItem(QStringLiteral("%1  %2 dBm  %3").arg(ssid,QString::fromLatin1(columns[2]),QString::fromLatin1(columns[3])),m_results);
                    item->setData(Qt::UserRole,ssid);
                }
                m_status->setText(QStringLiteral("选择网络或输入 SSID；支持开放网络和 WPA/WPA2-PSK"));
            });
        });
    });
}
void WifiDialog::join() {
    if (m_joining || m_busy || m_fd < 0) return;
    const auto ssid=m_ssid->text().toUtf8(), password=m_password->text().toUtf8();
    if (ssid.isEmpty() || ssid.size()>32 || (!password.isEmpty() && (password.size()<8 || password.size()>63))) {
        m_status->setText(QStringLiteral("SSID 需为 1–32 字节；密码需为 8–63 字节")); return;
    }
    QByteArray psk(32,'\0');
    if (!password.isEmpty() && PKCS5_PBKDF2_HMAC_SHA1(password.constData(),password.size(),
        reinterpret_cast<const uchar *>(ssid.constData()),ssid.size(),4096,32,reinterpret_cast<uchar *>(psk.data())) != 1) return;
    m_joining=true; m_previous.clear();
    command("STATUS",[this,ssid,password,psk](const QByteArray &data) {
        for (const auto &line:data.split('\n')) if(line.startsWith("id=")) m_previous=line.mid(3);
        command("ADD_NETWORK",[this,ssid,password,psk](const QByteArray &id) {
            bool ok=false; id.toUInt(&ok); if(!ok) { failedJoin(); return; } m_network=id;
            command("SET_NETWORK "+id+" ssid "+ssid.toHex(),[this,password,psk](const QByteArray &r) {
                if(r!="OK") { failedJoin(); return; }
                const auto setting=password.isEmpty()?QByteArray(" key_mgmt NONE"):QByteArray(" psk ")+psk.toHex();
                command("SET_NETWORK "+m_network+setting,[this](const QByteArray &r) {
                    if(r!="OK") { failedJoin(); return; }
                    command("SELECT_NETWORK "+m_network,[this](const QByteArray &r) {
                        if(r!="OK") { failedJoin(); return; }
                        m_password->clear(); m_status->setText(QStringLiteral("正在认证并获取 IP 地址…")); m_joinTimeout->start(30000);
                    });
                });
            });
        });
    });
}
void WifiDialog::failedJoin() {
    m_joining=false; m_joinTimeout->stop();
    if(!m_network.isEmpty()) command("REMOVE_NETWORK "+m_network,[](QByteArray){});
    if(!m_previous.isEmpty()) command("SELECT_NETWORK "+m_previous,[](QByteArray){});
    m_network.clear(); m_status->setText(QStringLiteral("连接失败；检查密码、认证类型和 DHCP，已恢复原网络"));
}
void WifiDialog::status() {
    if(m_fd<0 || m_busy) return;
    command("STATUS",[this](const QByteArray &data) {
        QMap<QByteArray,QByteArray> values;
        for(const auto &line:data.split('\n')) { const int i=line.indexOf('='); if(i>0) values[line.left(i)]=line.mid(i+1); }
        if(m_joining && values.value("id")==m_network && values.value("wpa_state")=="COMPLETED"
            && !values.value("ip_address").isEmpty()) {
            m_joining=false; m_joinTimeout->stop();
            command("SAVE_CONFIG",[this](const QByteArray &r) {
                if(r!="OK") m_status->setText(QStringLiteral("已连接，保存网络配置失败"));
            });
        }
        if(values.value("wpa_state")=="COMPLETED") m_status->setText(QStringLiteral("已认证：%1 · IP：%2").arg(
            QString::fromUtf8(decodeSsid(values.value("ssid"))),values.value("ip_address").isEmpty()?QStringLiteral("等待 DHCP"):QString::fromLatin1(values.value("ip_address"))));
    });
}

void WifiDialog::reject() {
    if (m_joining) {
        m_status->setText(QStringLiteral("正在连接；认证完成或超时恢复后可关闭")); return;
    }
    QDialog::reject();
}
