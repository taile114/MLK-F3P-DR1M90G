#pragma once

#include <QGroupBox>

#include "faulttypes.h"
#include "faultresult.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class FaultEvalPanel;

/* 故障诊断区：两个互斥模式共用同一面板。
 *
 * 「实时诊断」按采样的六路原生块做四秒窗口推断；「数据集评估」回放冻结特征
 * 评估集并按真值统计分类与分级准确度。按钮回调只发信号，加载、推断与统计
 * 都在工作线程完成，UI 不阻塞。 */
class FaultPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit FaultPanel(QWidget *parent = nullptr);

    QString directory() const;
    int backend() const;
    void setDirectory(const QString &directory);
    void setBackend(int backend);
    void setBusy(bool busy);
    bool discardNormalRecordings() const;
    void setDiscardNormalRecordings(bool enabled);

    void showState(int state, const QString &detail);
    void showResult(const mlk::FaultResult &result, qint64 ageMs, bool confirmed, bool external);
    void clearResult();

    /* 数据集评估页由本面板持有，目录与结果显示由窗口层驱动。 */
    FaultEvalPanel *evalPanel() const { return m_eval; }

signals:
    void startRequested(const QString &root, int backend);
    void stopRequested();
    void evalStartRequested(const QString &packageRoot, const QString &datasetRoot, int backend);
    void evalStopRequested();

private:
    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_directory = nullptr;
    QComboBox *m_backend = nullptr;
    QCheckBox *m_discardNormal = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_main = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_quality = nullptr;
    FaultEvalPanel *m_eval = nullptr;
};
