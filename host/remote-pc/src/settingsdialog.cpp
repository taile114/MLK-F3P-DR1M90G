#include "settingsdialog.h"

#include <QtWidgets>

#include "securechannel.h"

namespace {
const char *kNames[4] = { "温度", "电流", "振动", "转速" };

QDoubleSpinBox *makeSpin(QWidget *parent, double lo, double hi, int decimals)
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setMaximumWidth(120);
    return s;
}
} // namespace

SettingsDialog::SettingsDialog(mlk::AppConfig &cfg, QWidget *parent)
    : QDialog(parent)
    , m_cfg(cfg)
{
    setWindowTitle(QStringLiteral("参数配置"));

    auto *root = new QVBoxLayout(this);

    auto *chBox = new QGroupBox(QStringLiteral("通道（标定 k/b、告警限）"), this);
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
    root->addWidget(chBox);

    auto *ana = new QGroupBox(QStringLiteral("分析 / 链路 / 存储"), this);
    auto *form = new QFormLayout(ana);
    m_fft = new QComboBox(ana);
    for (int n : { 256, 512, 1024, 2048, 4096 })
        m_fft->addItem(QString::number(n), n);
    m_fft->setCurrentText(QString::number(m_cfg.fftPoints));
    m_addr = new QLineEdit(m_cfg.addr, ana);
    m_port = new QSpinBox(ana);
    m_port->setRange(1, 65535);
    m_port->setValue(m_cfg.port);
    m_target = new QLineEdit(m_cfg.targetAddr, ana);
    m_targetPort = new QSpinBox(ana);
    m_targetPort->setRange(1, 65535);
    m_targetPort->setValue(m_cfg.targetPort);
    m_dur = new QSpinBox(ana);
    m_dur->setRange(100, 60000);
    m_dur->setSuffix(QStringLiteral(" ms"));
    m_dur->setValue(m_cfg.injectDurationMs);
    m_dir = new QLineEdit(m_cfg.storageDir, ana);
    auto *browse = new QPushButton(QStringLiteral("浏览"), ana);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("故障波形目录"),
                                                            m_dir->text());
        if (!d.isEmpty())
            m_dir->setText(d);
    });
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    m_enc = new QCheckBox(QStringLiteral("加密存储"), ana);
    m_enc->setChecked(m_cfg.encrypt);
    m_pass = new QLineEdit(m_cfg.passphrase, ana);
    m_pass->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("FFT 点数"), m_fft);
    form->addRow(QStringLiteral("监听地址"), m_addr);
    form->addRow(QStringLiteral("端口"), m_port);
    form->addRow(QStringLiteral("PS 目标地址"), m_target);
    form->addRow(QStringLiteral("PS 目标端口"), m_targetPort);
    form->addRow(QStringLiteral("模拟注入时长"), m_dur);
    form->addRow(QStringLiteral("故障波形目录"), dirRow);
    form->addRow(QString(), m_enc);
    form->addRow(QStringLiteral("加密口令"), m_pass);
    root->addWidget(ana);

    m_selfPriv = m_cfg.selfPrivHex;
    auto *sec = new QGroupBox(QStringLiteral("安全（传输加密）"), this);
    auto *sform = new QFormLayout(sec);
    m_secMode = new QComboBox(sec);
    m_secMode->addItem(QStringLiteral("关闭"), 0);
    m_secMode->addItem(QStringLiteral("PSK"), 1);
    m_secMode->addItem(QStringLiteral("ECDH"), 2);
    m_secMode->setCurrentIndex(qMax(0, m_secMode->findData(m_cfg.secMode)));
    m_psk = new QLineEdit(m_cfg.pskHex, sec);
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
    connect(genId, &QPushButton::clicked, this, [this] {
        QByteArray pub;
        QByteArray priv;
        mlk::SecureChannel::generateIdentity(pub, priv);
        m_selfPub->setText(QString::fromLatin1(pub.toHex()));
        m_selfPriv = QString::fromLatin1(priv.toHex());
        m_fp->setText(QStringLiteral("指纹 %1")
                          .arg(QString::fromLatin1(mlk::SecureChannel::fingerprint(pub))));
    });
    auto *idRow = new QHBoxLayout;
    idRow->addWidget(m_selfPub, 1);
    idRow->addWidget(genId);
    m_peerPub = new QLineEdit(m_cfg.peerPubHex, sec);
    sform->addRow(QStringLiteral("模式"), m_secMode);
    sform->addRow(QStringLiteral("PSK（hex）"), pskRow);
    sform->addRow(QStringLiteral("身份公钥"), idRow);
    sform->addRow(QString(), m_fp);
    sform->addRow(QStringLiteral("对端公钥（hex）"), m_peerPub);
    root->addWidget(sec);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);
}

void SettingsDialog::accept()
{
    for (int i = 0; i < 4; ++i) {
        m_cfg.ch[i].k = m_k[i]->value();
        m_cfg.ch[i].b = m_b[i]->value();
        m_cfg.ch[i].alarmLo = m_lo[i]->value();
        m_cfg.ch[i].alarmHi = m_hi[i]->value();
        m_cfg.ch[i].alarmEnabled = m_en[i]->isChecked();
    }
    m_cfg.fftPoints = m_fft->currentData().toInt();
    m_cfg.addr = m_addr->text();
    m_cfg.port = m_port->value();
    m_cfg.targetAddr = m_target->text();
    m_cfg.targetPort = m_targetPort->value();
    m_cfg.injectDurationMs = m_dur->value();
    m_cfg.storageDir = m_dir->text();
    m_cfg.encrypt = m_enc->isChecked();
    m_cfg.passphrase = m_pass->text();
    m_cfg.secMode = m_secMode->currentData().toInt();
    m_cfg.pskHex = m_psk->text();
    m_cfg.selfPubHex = m_selfPub->text();
    m_cfg.selfPrivHex = m_selfPriv;
    m_cfg.peerPubHex = m_peerPub->text();
    QDialog::accept();
}
