#include "plsettingsdialog.h"

#include <QtWidgets>
#include <QHostAddress>
#include <QScreen>

#include "securechannel.h"
#include "numberedit.h"
#include "sharedconfig.h"
#include "capturestore.h"

namespace {
const char *kNames[4] = { "温度", "电流", "振动", "转速" };

// 数值输入框：点哪改哪的文本编辑语义，见 numberedit.h。
mlk::NumberEdit *makeSpin(QWidget *parent, double lo, double hi, int decimals)
{
    auto *edit = new mlk::NumberEdit(parent);
    edit->setRange(lo, hi);
    edit->setDecimals(decimals);
    edit->setMaximumWidth(120);
    edit->setMinimumWidth(96);
    return edit;
}
} // namespace

PlSettingsDialog::PlSettingsDialog(mlk::AppConfig &cfg, QWidget *parent, bool remote, bool networkTime)
    : QDialog(parent)
    , m_cfg(cfg)
    , m_remote(remote)
    , m_networkTime(networkTime)
{
    setWindowTitle(QStringLiteral("参数配置"));

    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll);
    auto *root = new QVBoxLayout(body);
    scroll->setWidget(body); outer->addWidget(scroll);
    resize(1000, qMin(900, QGuiApplication::primaryScreen()->availableGeometry().height() - 80));

    auto *chBox = new QGroupBox(QStringLiteral("通道标定与告警"), this);
    auto *chGrid = new QGridLayout(chBox);
    chGrid->addWidget(new QLabel(QStringLiteral("通道"), chBox), 0, 0);
    chGrid->addWidget(new QLabel(QStringLiteral("k"), chBox), 0, 1);
    chGrid->addWidget(new QLabel(QStringLiteral("b"), chBox), 0, 2);
    chGrid->addWidget(new QLabel(QStringLiteral("下限"), chBox), 0, 3);
    chGrid->addWidget(new QLabel(QStringLiteral("上限"), chBox), 0, 4);
    chGrid->addWidget(new QLabel(QStringLiteral("告警"), chBox), 0, 5);
    for (int i = 0; i < 4; ++i) {
        m_k[i] = makeSpin(chBox, 0.0, 1e6, 6);
        m_b[i] = makeSpin(chBox, -1e9, 1e9, 3);
        m_lo[i] = makeSpin(chBox, -1e9, 1e9, 3);
        m_hi[i] = makeSpin(chBox, -1e9, 1e9, 3);
        m_en[i] = new QCheckBox(chBox);
        m_k[i]->setValue(m_cfg.ch[i].k);
        m_b[i]->setValue(m_cfg.ch[i].b);
        m_lo[i]->setValue(m_cfg.ch[i].alarmLo);
        m_hi[i]->setValue(m_cfg.ch[i].alarmHi);
        m_en[i]->setChecked(m_cfg.ch[i].alarmEnabled);
        chGrid->addWidget(new QLabel(QString::fromUtf8(kNames[i]), chBox), i + 1, 0);
        chGrid->addWidget(m_k[i], i + 1, 1);
        chGrid->addWidget(m_b[i], i + 1, 2);
        chGrid->addWidget(m_lo[i], i + 1, 3);
        chGrid->addWidget(m_hi[i], i + 1, 4);
        chGrid->addWidget(m_en[i], i + 1, 5);
    }
    m_currentZero = makeSpin(chBox, -32768, 32767, 0);
    m_currentGain = makeSpin(chBox, 0, 1000000, 0);
    m_currentZero->setObjectName(QStringLiteral("currentZero"));
    m_currentGain->setObjectName(QStringLiteral("currentGain"));
    m_currentZero->setValue(m_cfg.currentZero);
    m_currentGain->setValue(m_cfg.currentGain);
    chGrid->addWidget(new QLabel(QStringLiteral("电流零点 / LSB"), chBox), 5, 0);
    chGrid->addWidget(m_currentZero, 5, 1);
    chGrid->addWidget(new QLabel(QStringLiteral("电流增益 / µA/LSB"), chBox), 5, 2);
    chGrid->addWidget(m_currentGain, 5, 3);
    auto *currentNote = new QLabel(QStringLiteral("ACS712-20A：默认零点 20000、增益 1250；停机归零可校正偏移。电流与转速单位固定，k/b 仅用于温度、振动。"), chBox);
    currentNote->setWordWrap(true);
    chGrid->addWidget(currentNote, 6, 0, 1, 6);
    m_priority = new QComboBox(this);
    m_priority->setObjectName(QStringLiteral("configPriority"));
    m_priority->addItem(QStringLiteral("ps-local 优先"), false);
    m_priority->addItem(QStringLiteral("remote-pc 优先"), true);
    m_priority->setCurrentIndex(m_cfg.remotePriority ? 1 : 0);
    m_priority->setEnabled(!remote);
    auto updateOwner = [=](int) {
        /* 共享参数的编辑权限与链路、密钥无关：板端始终可改本地值；上位机端
         * 只在板端授予 remote-pc 控制权时编辑，连不上时先存本机，连上后再同步。 */
        const bool editable = remote ? m_cfg.remotePriority : true;
        for (int i = 0; i < 4; ++i)
            for (QWidget *w : {static_cast<QWidget *>(m_k[i]), static_cast<QWidget *>(m_b[i]),
                 static_cast<QWidget *>(m_lo[i]), static_cast<QWidget *>(m_hi[i]), static_cast<QWidget *>(m_en[i])})
                w->setEnabled(editable);
        m_currentZero->setEnabled(editable && !remote);
        m_currentGain->setEnabled(editable && !remote);
        if (remote) {
            m_currentZero->setToolTip(QStringLiteral("电流零点由板端停机归零维护"));
            m_currentGain->setToolTip(QStringLiteral("电流增益由板端传感器与 ADS1115 量程确定"));
        }
        for (int i : {1, 3}) { m_k[i]->setEnabled(false); m_b[i]->setEnabled(false); }
    };
    connect(m_priority, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateOwner);
    updateOwner(m_priority->currentIndex());
    auto *priorityRow = new QFormLayout;
    priorityRow->addRow(QStringLiteral("共享参数控制权（板端决定）"), m_priority);
    root->addLayout(priorityRow);
    auto *priorityNote = new QLabel(remote
        ? QStringLiteral("remote-pc 优先时，本机保存的共享参数在连接后写入板端并覆盖板端值；ps-local 优先时共享参数由板端决定。")
        : QStringLiteral("ps-local 优先时远端写入被拒绝；remote-pc 优先时板端仍可修改，上位机连接后以其配置覆盖。链路、密钥与共享参数分别保存。"),
        this);
    priorityNote->setWordWrap(true);
    priorityNote->setStyleSheet(QStringLiteral("color: #8b949e; font-size: 11px;"));
    root->addWidget(priorityNote);
    root->addWidget(chBox);

    auto *ana = new QGroupBox(QStringLiteral("分析 / 本地记录"), this);
    auto *form = new QFormLayout(ana);
    m_fft = new QComboBox(ana);
    for (int n : { 256, 512, 1024, 2048, 4096 })
        m_fft->addItem(QString::number(n), n);
    m_fft->setCurrentText(QString::number(m_cfg.fftPoints));
    m_fps = new QComboBox(ana);
    for (int f : { 2, 5, 10, 30 })
        m_fps->addItem(QStringLiteral("%1 fps").arg(f), f);
    m_fps->setCurrentIndex(qMax(0, m_fps->findData(m_cfg.uiFps)));
    m_segment = new QComboBox(ana);
    for (int sec : { 10, 30, 60, 300 })
        m_segment->addItem(QStringLiteral("%1 s").arg(sec), sec);
    m_segment->setCurrentIndex(qMax(0, m_segment->findData(m_cfg.recordSegmentSec)));
    m_segment->setToolTip(QStringLiteral("按时间分段保存四传感器全部接收样本；保留原生采样节奏"));
    m_dir = new QLineEdit(m_cfg.storageDir, ana);
    auto *browse = new QPushButton(QStringLiteral("浏览"), ana);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("本地记录目录"),
                                                            m_dir->text());
        if (!d.isEmpty())
            m_dir->setText(d);
    });
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    m_enc = new QCheckBox(QStringLiteral("寿命结果加密（采集记录始终加密）"), ana);
    m_enc->setChecked(m_cfg.encrypt);
    m_pass = new QLineEdit(m_cfg.passphrase, ana);
    m_pass->setEchoMode(QLineEdit::Password);
    m_retention = new mlk::NumberEdit(ana);
    m_retention->setRange(0, 120);
    m_retention->setDecimals(0);
    m_retention->setMaximumWidth(120);
    m_retention->setMinimumWidth(96);
    m_retention->setValue(m_cfg.retentionMonths);
    m_retention->setToolTip(QStringLiteral("0 = 不按时间清理；记录每 60 s 一个文件"));
    m_minFree = new mlk::NumberEdit(ana);
    m_minFree->setRange(0, 100000);
    m_minFree->setDecimals(0);
    m_minFree->setMaximumWidth(120);
    m_minFree->setMinimumWidth(96);
    m_minFree->setValue(m_cfg.minFreeMb);
    m_minFree->setToolTip(QStringLiteral("剩余空间低于此值时删除最旧样本；0 = 不限制"));
    m_settingsPass = new QLineEdit(m_cfg.settingsPassword, ana);
    m_settingsPass->setEchoMode(QLineEdit::Password);
    m_settingsPass->setToolTip(QStringLiteral("进入配置的密码；初始 root；留空表示不修改"));
    form->addRow(QStringLiteral("FFT 点数"), m_fft);
    form->addRow(QStringLiteral("界面刷新率"), m_fps);
    form->addRow(QStringLiteral("记录分段"), m_segment);
    form->addRow(QStringLiteral("本地记录目录"), dirRow);
    form->addRow(QString(), m_enc);
    form->addRow(QStringLiteral("加密口令"), m_pass);
    form->addRow(QStringLiteral("最长保留（月，0 = 不限制）"), m_retention);
    form->addRow(QStringLiteral("最低剩余（MB，0 = 不限制）"), m_minFree);
    form->addRow(QStringLiteral("配置密码"), m_settingsPass);
    root->addWidget(ana);

    auto *recordBox = new QGroupBox(QStringLiteral("记录管理"), this);
    auto *rbox = new QVBoxLayout(recordBox);
    auto *recordNote = new QLabel(QStringLiteral("仅删除「本地记录目录」中的采集与故障波形文件；故障工单、标签和寿命结果保留。删除后不可恢复。"), recordBox);
    recordNote->setWordWrap(true);
    recordNote->setStyleSheet(QStringLiteral("color: #8b949e; font-size: 11px;"));
    m_removeAll = new QPushButton(QStringLiteral("清理所有记录"), recordBox);
    m_removeAll->setObjectName(QStringLiteral("cleanupAllRecords"));
    m_removeSelected = new QPushButton(QStringLiteral("删除选定记录…"), recordBox);
    m_removeSelected->setObjectName(QStringLiteral("deleteSelectedRecords"));
    m_recordStatus = new QLabel(recordBox);
    m_recordStatus->setTextFormat(Qt::PlainText);
    auto *recordRow = new QHBoxLayout;
    recordRow->addWidget(m_removeAll);
    recordRow->addWidget(m_removeSelected);
    recordRow->addStretch(1);
    rbox->addWidget(recordNote);
    rbox->addLayout(recordRow);
    rbox->addWidget(m_recordStatus);
    connect(m_removeAll, &QPushButton::clicked, this, &PlSettingsDialog::removeAllRecords);
    connect(m_removeSelected, &QPushButton::clicked, this, &PlSettingsDialog::removeSelectedRecords);
    connect(m_dir, &QLineEdit::textChanged, this, [this] { updateRecordStatus(); });
    root->addWidget(recordBox);

    if (!m_remote) {
        auto *timeBox = new QGroupBox(QStringLiteral("板端时间"), this);
        auto *timeForm = new QFormLayout(timeBox);
        m_timeState = new QLabel(m_networkTime
            ? QStringLiteral("网络校时可用：由 NTP 校时，手动设置已禁用")
            : QStringLiteral("网络校时不可用：可手动设置板端时间"), timeBox);
        m_timeState->setWordWrap(true);
        m_timeState->setObjectName(QStringLiteral("boardTimeState"));
        m_manualTime = new QDateTimeEdit(QDateTime::currentDateTime(), timeBox);
        m_manualTime->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        m_manualTime->setCalendarPopup(true);
        m_manualTime->setObjectName(QStringLiteral("manualTime"));
        m_manualTime->setEnabled(!m_networkTime);
        m_setTime = new QPushButton(QStringLiteral("设置板端时间"), timeBox);
        m_setTime->setObjectName(QStringLiteral("setBoardTimeButton"));
        m_setTime->setEnabled(!m_networkTime);
        connect(m_setTime, &QPushButton::clicked, this, [this] {
            const auto target = m_manualTime->dateTime();
            if (QMessageBox::question(this, QStringLiteral("设置板端时间"),
                    QStringLiteral("把板端时间设为 %1？").arg(
                        target.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))))
                != QMessageBox::Yes) return;
            emit timeSetRequested(target);
        });
        timeForm->addRow(QString(), m_timeState);
        timeForm->addRow(QStringLiteral("手动设置"), m_manualTime);
        timeForm->addRow(QString(), m_setTime);
        root->addWidget(timeBox);

        auto *systemBox = new QGroupBox(QStringLiteral("系统"), this);
        auto *systemRow = new QHBoxLayout(systemBox);
        auto *reboot = new QPushButton(QStringLiteral("重启核心板"), systemBox);
        reboot->setObjectName(QStringLiteral("rebootBoard"));
        connect(reboot, &QPushButton::clicked, this, [this] {
            if (QMessageBox::warning(this, QStringLiteral("重启核心板"),
                    QStringLiteral("重启会中断采集、记录与显示，确定重启核心板？"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            emit rebootRequested();
        });
        systemRow->addWidget(reboot);
        systemRow->addStretch(1);
        root->addWidget(systemBox);
    }
    updateRecordStatus();

    m_selfPriv = m_cfg.selfPrivHex;
    auto *sec = new QGroupBox(QStringLiteral("安全（传输加密）"), this);
    auto *sform = new QFormLayout(sec);
    m_secMode = new QComboBox(sec);
    m_secMode->setObjectName(QStringLiteral("securityMode"));
    m_secMode->addItem(QStringLiteral("关闭"), 0);
    m_secMode->addItem(QStringLiteral("PSK"), 1);
    m_secMode->addItem(QStringLiteral("ECDH"), 2);
    m_secMode->setCurrentIndex(qMax(0, m_secMode->findData(m_cfg.secMode)));
    m_psk = new QLineEdit(m_cfg.pskHex, sec);
    m_psk->setObjectName(QStringLiteral("transportPsk"));
    auto *genPsk = new QPushButton(QStringLiteral("生成"), sec);
    connect(genPsk, &QPushButton::clicked, this, [this] {
        m_psk->setText(mlk::crypto::randomBytes(32).toHex());
    });
    auto *pskRow = new QHBoxLayout;
    pskRow->addWidget(m_psk, 1);
    pskRow->addWidget(genPsk);
    m_selfPub = new QLineEdit(m_cfg.selfPubHex, sec);
    m_selfPub->setReadOnly(true);
    m_fp = new QLabel(sec);
    if (!m_cfg.selfPubHex.isEmpty()) {
        m_fp->setText(QStringLiteral("指纹 %1")
                          .arg(QString::fromLatin1(mlk::SecureChannel::fingerprint(
                              QByteArray::fromHex(m_cfg.selfPubHex.toLatin1())))));
    }
    auto *genId = new QPushButton(QStringLiteral("生成身份密钥"), sec);
    genId->setObjectName(QStringLiteral("generateIdentity"));
    connect(genId, &QPushButton::clicked, this, [this] {
        QByteArray pub;
        QByteArray priv;
        if (!mlk::SecureChannel::generateIdentity(pub, priv)) {
            QMessageBox::warning(this, QStringLiteral("身份密钥"), QStringLiteral("密钥生成失败")); return;
        }
        m_selfPub->setText(QString::fromLatin1(pub.toHex()));
        m_selfPriv = QString::fromLatin1(priv.toHex());
        m_fp->setText(QStringLiteral("指纹 %1")
                          .arg(QString::fromLatin1(mlk::SecureChannel::fingerprint(pub))));
    });
    auto *idRow = new QHBoxLayout;
    idRow->addWidget(m_selfPub, 1);
    idRow->addWidget(genId);
    m_peerPub = new QLineEdit(m_cfg.peerPubHex, sec);
    m_selfPub->setObjectName(QStringLiteral("identityPublicKey"));
    m_peerPub->setObjectName(QStringLiteral("peerPublicKey"));
    auto *exportKey = new QPushButton(QStringLiteral("导出本机公钥"), sec);
    auto *importKey = new QPushButton(QStringLiteral("导入对端公钥"), sec);
    connect(exportKey, &QPushButton::clicked, this, [this, remote] {
        if (m_selfPub->text().size() != 64) {
            QMessageBox::warning(this, QStringLiteral("公钥"), QStringLiteral("请先生成身份密钥")); return;
        }
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出本机公钥"),
            remote ? QStringLiteral("remote-pc.pub") : QStringLiteral("ps-local.pub"), QStringLiteral("公钥文件 (*.pub)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto bytes = m_selfPub->text().toLatin1() + '\n';
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("公钥"), QStringLiteral("公钥文件保存失败"));
    });
    connect(importKey, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入对端公钥"), {}, QStringLiteral("公钥文件 (*.pub)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 128) return;
        const auto key = QString::fromLatin1(file.readAll()).trimmed();
        static const QRegularExpression valid(QStringLiteral("^[0-9a-fA-F]{64}$"));
        if (!valid.match(key).hasMatch()) {
            QMessageBox::warning(this, QStringLiteral("公钥"), QStringLiteral("公钥需为 64 个十六进制字符")); return;
        }
        m_peerPub->setText(key);
        QMessageBox::information(this, QStringLiteral("对端公钥"), QStringLiteral("对端指纹 %1；请与对端界面核对后保存。")
            .arg(QString::fromLatin1(mlk::SecureChannel::fingerprint(QByteArray::fromHex(key.toLatin1())))));
    });
    auto *keyFiles = new QHBoxLayout;
    keyFiles->addWidget(exportKey); keyFiles->addWidget(importKey);
    sform->addRow(QStringLiteral("模式"), m_secMode);
    sform->addRow(QStringLiteral("PSK（hex）"), pskRow);
    sform->addRow(QStringLiteral("身份公钥"), idRow);
    sform->addRow(QString(), m_fp);
    sform->addRow(QStringLiteral("对端公钥（hex）"), m_peerPub);
    sform->addRow(QStringLiteral("公钥交换"), keyFiles);
    auto *secNote = new QLabel(QStringLiteral("链路与密钥仅用于两端通信，与通道标定、告警、记录和模型设置相互独立；未填写不影响本地保存。"), sec);
    secNote->setWordWrap(true);
    secNote->setStyleSheet(QStringLiteral("color: #8b949e; font-size: 11px;"));
    sform->addRow(QString(), secNote);
    root->addWidget(sec);

    auto *net = new QGroupBox(QStringLiteral("传输（可选 UDP）"), this);
    auto *nform = new QFormLayout(net);
    m_addr = new QLineEdit(m_cfg.addr, net);
    m_port = new mlk::NumberEdit(net);
    m_port->setRange(1, 65535);
    m_port->setDecimals(0);
    m_port->setValue(m_cfg.port);
    m_target = new QLineEdit(m_cfg.targetAddr, net);
    m_targetPort = new mlk::NumberEdit(net);
    m_targetPort->setRange(1, 65535);
    m_targetPort->setDecimals(0);
    m_targetPort->setMaximumWidth(120);
    m_targetPort->setMinimumWidth(96);
    m_targetPort->setValue(m_cfg.targetPort);
    auto *netNote = new QLabel(QStringLiteral("首次配对后可自动发现同网段设备；固定地址作为首选目标。4G 跨网需可达地址或 VPN。"), net);
    netNote->setWordWrap(true);
    netNote->setStyleSheet(QStringLiteral("color: #8b949e; font-size: 11px;"));
    auto *listenRow = new QHBoxLayout;
    listenRow->addWidget(m_addr, 1);
    listenRow->addWidget(new QLabel(QStringLiteral("端口"), net));
    listenRow->addWidget(m_port);
    auto *targetRow = new QHBoxLayout;
    targetRow->addWidget(m_target, 1);
    targetRow->addWidget(new QLabel(QStringLiteral("端口"), net));
    targetRow->addWidget(m_targetPort);
    nform->addRow(QStringLiteral("监听地址"), listenRow);
    nform->addRow(QStringLiteral("上报目标"), targetRow);
    m_autoDiscover = new QCheckBox(QStringLiteral("自动发现已配对设备（同一局域网）"), net);
    m_autoDiscover->setObjectName(QStringLiteral("autoDiscover"));
    m_autoDiscover->setChecked(m_cfg.autoDiscover);
    nform->addRow(QString(), m_autoDiscover);
    if (remote) {
        m_autoConnect = new QCheckBox(QStringLiteral("启动后自动监听并连接"), net);
        m_autoConnect->setObjectName(QStringLiteral("autoConnect"));
        m_autoConnect->setChecked(m_cfg.autoConnect); nform->addRow(QString(), m_autoConnect);
    }
    nform->addRow(QString(), netNote);
    root->addWidget(net);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &PlSettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(bb);
}

void PlSettingsDialog::accept()
{
    // 先提交尚未回车的编辑，避免读取到上一次的数值。
    for (int i = 0; i < 4; ++i) {
        m_k[i]->commit();
        m_b[i]->commit();
        m_lo[i]->commit();
        m_hi[i]->commit();
    }
    m_retention->commit();
    m_currentZero->commit();
    m_currentGain->commit();
    m_minFree->commit();
    m_port->commit();
    m_targetPort->commit();

    for (int i = 0; i < 4; ++i) {
        m_cfg.ch[i].k = m_k[i]->value();
        m_cfg.ch[i].b = m_b[i]->value();
        m_cfg.ch[i].alarmLo = m_lo[i]->value();
        m_cfg.ch[i].alarmHi = m_hi[i]->value();
        m_cfg.ch[i].alarmEnabled = m_en[i]->isChecked();
    }
    if (!m_remote) m_cfg.remotePriority = m_priority->currentData().toBool();
    m_cfg.currentZero = qint32(m_currentZero->value());
    m_cfg.currentGain = qint32(m_currentGain->value());
    m_cfg.fftPoints = m_fft->currentData().toInt();
    m_cfg.uiFps = m_fps->currentData().toInt();
    m_cfg.storageDir = m_dir->text();
    m_cfg.encrypt = m_enc->isChecked();
    m_cfg.passphrase = m_pass->text();
    m_cfg.retentionMonths = m_retention->value();
    m_cfg.minFreeMb = m_minFree->value();
    m_cfg.recordSegmentSec = m_segment->currentData().toInt();
    if (!m_settingsPass->text().isEmpty())
        m_cfg.settingsPassword = m_settingsPass->text();
    m_cfg.secMode = m_secMode->currentData().toInt();
    m_cfg.pskHex = m_psk->text();
    m_cfg.selfPubHex = m_selfPub->text();
    m_cfg.selfPrivHex = m_selfPriv;
    m_cfg.peerPubHex = m_peerPub->text();
    m_cfg.addr = m_addr->text();
    m_cfg.port = m_port->value();
    m_cfg.targetAddr = m_target->text();
    m_cfg.targetPort = m_targetPort->value();
    m_cfg.autoDiscover = m_autoDiscover->isChecked();
    if (m_autoConnect) m_cfg.autoConnect = m_autoConnect->isChecked();
    mlk::AppConfig checked = m_cfg;
    if (!mlk::readSharedConfig(mlk::sharedConfig(m_cfg), checked)) {
        QMessageBox::warning(this, QStringLiteral("参数配置"), QStringLiteral("共享参数无效；告警下限不得高于上限"));
        return;
    }
    /* 仅校验实际填写的字段：链路、密钥与本地参数分别保存，修改通道标定等
     * 设置不要求先配好 IP 和密钥。空值表示未配置，由连接状态显示原因。 */
    const auto validHex = [](const QString &s, int bytes) {
        static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]+$"));
        return s.size() == bytes * 2 && hex.match(s).hasMatch();
    };
    const auto malformed = [&](const QString &s, int bytes) {
        return !s.isEmpty() && !validHex(s, bytes);
    };
    if (malformed(m_cfg.pskHex, 32) || malformed(m_cfg.selfPubHex, 32)
        || malformed(m_cfg.selfPrivHex, 32) || malformed(m_cfg.peerPubHex, 32)) {
        QMessageBox::warning(this, QStringLiteral("参数配置"),
                             QStringLiteral("密钥需为完整的十六进制字符串（PSK 32 字节，公钥 / 私钥 32 字节）"));
        return;
    }
    if (m_cfg.secMode == 1 && m_cfg.pskHex.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("参数配置"),
                             QStringLiteral("PSK 模式需填写 32 字节 PSK；也可先改用 ECDH 或关闭加密"));
        return;
    }
    if (m_cfg.secMode == 2 && (m_cfg.selfPubHex.isEmpty() || m_cfg.selfPrivHex.isEmpty())) {
        QMessageBox::warning(this, QStringLiteral("参数配置"),
                             QStringLiteral("ECDH 模式需先生成本机身份密钥；对端公钥可留空等待配对"));
        return;
    }
    if ((!m_cfg.addr.isEmpty() && QHostAddress(m_cfg.addr).isNull())
        || (!m_cfg.targetAddr.isEmpty() && QHostAddress(m_cfg.targetAddr).isNull())) {
        QMessageBox::warning(this, QStringLiteral("参数配置"),
                             QStringLiteral("IP 地址格式无效；留空表示不绑定监听地址或不设置上报目标"));
        return;
    }
    QDialog::accept();
}

void PlSettingsDialog::updateRecordStatus()
{
    if (!m_recordStatus)
        return;
    QDir dir(m_dir->text().trimmed());
    const int captures = dir.entryList({QStringLiteral("capture_*.mlkc")}, QDir::Files).size();
    const int faults = dir.entryList({QStringLiteral("fault_*.mlkf")}, QDir::Files).size();
    m_recordStatus->setText(QStringLiteral("当前目录：采集记录 %1 个 · 故障波形 %2 个")
                                .arg(captures).arg(faults));
    if (m_removeAll)
        m_removeAll->setEnabled(captures + faults > 0);
}

void PlSettingsDialog::removeAllRecords()
{
    const QString directory = m_dir->text().trimmed();
    QDir dir(directory);
    const auto names = dir.entryList({QStringLiteral("capture_*.mlkc"), QStringLiteral("fault_*.mlkf")},
                                     QDir::Files);
    if (names.isEmpty()) {
        updateRecordStatus();
        return;
    }
    if (QMessageBox::warning(this, QStringLiteral("清理所有记录"),
            QStringLiteral("删除当前目录中的 %1 个记录文件？\n该操作不可恢复；故障工单、标签和寿命结果保留。")
                .arg(names.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QStringList failed;
    const int removed = mlk::removeCaptureFiles(directory, names, &failed);
    updateRecordStatus();
    emit recordsRemoved();
    QMessageBox::information(this, QStringLiteral("清理所有记录"),
        failed.isEmpty()
            ? QStringLiteral("已删除 %1 个记录文件").arg(removed)
            : QStringLiteral("已删除 %1 个；%2 个删除失败：%3")
                  .arg(removed).arg(failed.size()).arg(failed.join(QStringLiteral(", "))));
}

void PlSettingsDialog::removeSelectedRecords()
{
    const QString directory = m_dir->text().trimmed();
    const auto paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要删除的记录"), directory,
        QStringLiteral("采集与故障波形 (*.mlkc *.mlkf);;全部文件 (*)"));
    if (paths.isEmpty())
        return;
    if (QMessageBox::warning(this, QStringLiteral("删除选定记录"),
            QStringLiteral("删除选定的 %1 个记录文件？\n该操作不可恢复；仅本目录内的采集 / 故障波形可删除。")
                .arg(paths.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QStringList failed;
    const int removed = mlk::removeCaptureFiles(directory, paths, &failed);
    updateRecordStatus();
    emit recordsRemoved();
    QMessageBox::information(this, QStringLiteral("删除选定记录"),
        failed.isEmpty()
            ? QStringLiteral("已删除 %1 个记录文件").arg(removed)
            : QStringLiteral("已删除 %1 个；%2 个未删除（目录不符或文件名无效）：%3")
                  .arg(removed).arg(failed.size()).arg(failed.join(QStringLiteral(", "))));
}
