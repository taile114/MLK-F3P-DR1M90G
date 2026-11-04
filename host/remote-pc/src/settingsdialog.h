#pragma once

#include <QDialog>

#include "appconfig.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(mlk::AppConfig &cfg, QWidget *parent = nullptr);

public slots:
    void accept() override;

private:
    mlk::AppConfig &m_cfg;
    QDoubleSpinBox *m_k[4];
    QDoubleSpinBox *m_b[4];
    QDoubleSpinBox *m_lo[4];
    QDoubleSpinBox *m_hi[4];
    QCheckBox *m_en[4];
    QComboBox *m_fft;
    QLineEdit *m_addr;
    QSpinBox *m_port;
    QLineEdit *m_target;
    QSpinBox *m_targetPort;
    QSpinBox *m_dur;
    QLineEdit *m_dir;
    QCheckBox *m_enc;
    QLineEdit *m_pass;
    QComboBox *m_secMode;
    QLineEdit *m_psk;
    QLineEdit *m_selfPub;
    QLineEdit *m_peerPub;
    QLabel *m_fp;
    QString m_selfPriv;
};
