#pragma once
#include <QDialog>
#include <QQueue>
#include <QStringList>
#include <functional>
class QComboBox;
class QLineEdit;
class QLabel;
class QListWidget;
class QSocketNotifier;
class QTimer;
class WifiDialog : public QDialog {
    Q_OBJECT
public:
    explicit WifiDialog(QWidget *parent = nullptr,
                        const QString &controlDirectory = QStringLiteral("/var/run/wpa_supplicant"),
                        const QStringList &interfaces = {},
                        bool embedded = false);
    ~WifiDialog() override;
public slots:
    void reject() override;
private:
    void command(const QByteArray &text, std::function<void(QByteArray)> done);
    void next();
    void receive();
    void scan();
    void join();
    void status();
    void failedJoin();
    int m_fd = -1;
    QByteArray m_path;
    QString m_controlDirectory;
    QComboBox *m_interface;
    QLineEdit *m_ssid, *m_password;
    QListWidget *m_results;
    QLabel *m_status;
    QTimer *m_timeout, *m_joinTimeout;
    QSocketNotifier *m_notifier = nullptr;
    struct Request { QByteArray text; std::function<void(QByteArray)> done; };
    QQueue<Request> m_queue;
    bool m_busy = false, m_joining = false;
    QByteArray m_network, m_previous;
};
