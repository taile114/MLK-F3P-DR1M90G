#pragma once

/* 网络配置：左侧选择链路（千兆网 / 4G / WiFi），右侧显示对应配置，布局与
 * 「回放」一致。三页共用一份状态刷新定时器；WiFi 页内嵌原 WiFi 配置对话框。
 *
 * 优先级：千兆网 > 4G > WiFi。高优先级链路获得地址后，其余链路不参与默认
 * 路由；该策略同时由板端 mlk_net_watch 强制执行。 */

#include <QDialog>
#include <QUrl>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QStackedWidget;
class QTimer;
class WifiDialog;

class NetworkSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NetworkSettingsDialog(QWidget *parent = nullptr);

private:
    QWidget *createEthernetPage();
    QWidget *createCellularPage();
    QWidget *createWifiPage();
    void refresh();
    void refreshSections();
    void refreshEthernet();
    void refreshCellular();
    void reloadInterfaces();
    QString selectedEthernet() const;
    QString selectedCellular() const;
    QString selectedWifi() const;
    void enableEthernet();
    void renewEthernet();
    void detectPortal();
    void handlePortalProbe(QNetworkReply *reply);
    void openPortalLogin();
    void autoPortalLogin();
    void savePortalSettings();
    QString gatewayOf(const QString &iface) const;
    QUrl portalAddress() const;
    void dialCellular();
    void hangupCellular();
    void startPppoe();
    void stopPppoe();
    void savePppoeSettings();
    void refreshPppoe();

    QListWidget *m_sections = nullptr;
    QStackedWidget *m_pages = nullptr;
    QComboBox *m_ethInterface = nullptr;
    QLabel *m_ethStatus = nullptr, *m_ethPortal = nullptr, *m_cellStatus = nullptr;
    QLabel *m_priority = nullptr;
    QLineEdit *m_apn = nullptr;
    QLineEdit *m_portalUrlEdit = nullptr, *m_portalUser = nullptr, *m_portalPassword = nullptr;
    QCheckBox *m_portalAuto = nullptr;
    QPushButton *m_portalSave = nullptr;
    QPushButton *m_ethEnable = nullptr, *m_ethRenew = nullptr;
    QPushButton *m_portalDetect = nullptr, *m_portalLogin = nullptr;
    QPushButton *m_dial = nullptr, *m_hangup = nullptr;
    QCheckBox *m_pppoeEnable = nullptr;
    QLineEdit *m_pppoeUser = nullptr, *m_pppoePassword = nullptr, *m_pppoeService = nullptr;
    QPushButton *m_pppoeConnect = nullptr, *m_pppoeDisconnect = nullptr;
    QLabel *m_pppoeStatus = nullptr;
    WifiDialog *m_wifi = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QUrl m_portalUrl;
    bool m_probing = false;
    bool m_autoDetected = false;
    bool m_autoLoginTried = false;
    bool m_gatewayProbeTried = false;
};
