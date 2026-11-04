#pragma once

#include <QDateTime>
#include <QDialog>

#include "appconfig.h"

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;

namespace mlk {
class NumberEdit;
}

class PlSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    /* remote=true 时为上位机端：不显示时间同步、重启与配置密码字段。
     * networkTime 表示板端网络时间源当前可用，决定手动设置时间是否开放。 */
    explicit PlSettingsDialog(mlk::AppConfig &cfg, QWidget *parent = nullptr,
                              bool remote = false, bool networkTime = false);

signals:
    void recordsRemoved();
    void timeSetRequested(const QDateTime &local);
    void rebootRequested();

public slots:
    void accept() override;

private:
    void removeAllRecords();
    void removeSelectedRecords();
    void updateRecordStatus();

    mlk::AppConfig &m_cfg;
    mlk::NumberEdit *m_k[4];
    mlk::NumberEdit *m_b[4];
    mlk::NumberEdit *m_lo[4];
    mlk::NumberEdit *m_hi[4];
    QCheckBox *m_en[4];
    mlk::NumberEdit *m_currentZero;
    mlk::NumberEdit *m_currentGain;
    QComboBox *m_priority;
    bool m_remote = false;
    bool m_networkTime = false;
    QComboBox *m_fft;
    QComboBox *m_fps;
    QComboBox *m_segment;
    QLineEdit *m_dir;
    QCheckBox *m_enc;
    QLineEdit *m_pass;
    mlk::NumberEdit *m_retention;
    mlk::NumberEdit *m_minFree;
    QLineEdit *m_settingsPass;
    QComboBox *m_secMode;
    QLineEdit *m_psk;
    QLineEdit *m_selfPub;
    QLineEdit *m_peerPub;
    QLineEdit *m_addr;
    mlk::NumberEdit *m_port;
    QLineEdit *m_target;
    QCheckBox *m_autoDiscover;
    QCheckBox *m_autoConnect = nullptr;
    mlk::NumberEdit *m_targetPort;
    QLabel *m_fp;
    QLabel *m_recordStatus = nullptr;
    QPushButton *m_removeAll = nullptr;
    QPushButton *m_removeSelected = nullptr;
    QDateTimeEdit *m_manualTime = nullptr;
    QPushButton *m_setTime = nullptr;
    QLabel *m_timeState = nullptr;
    QString m_selfPriv;
};
