#pragma once
#include <QWidget>
#include "../remote-pc/src/frame.h"
class QLabel;
class QLineEdit;
class QComboBox;
class QSpinBox;
namespace mlk {
class ReplayPanel : public QWidget {
    Q_OBJECT
public:
    explicit ReplayPanel(bool local, QWidget *parent = nullptr);
    void showFrame(const Frame &f);
    void setDirectory(const QString &root);
    QString directory() const;
    void showError(const QString &error);
    void setConnected(bool connected);
signals:
    void requested(quint8 action, quint8 backend, quint32 index, quint32 intervalMs);
private:
    QLabel *m_state;
    QLabel *m_details;
    QLineEdit *m_root = nullptr;
    QComboBox *m_backend;
    bool m_backendEdited = false;
    QSpinBox *m_index;
    QSpinBox *m_interval;
    QString m_windowClock; /* 模型窗口结果时间；暂停/停止后定格 */
};
}
