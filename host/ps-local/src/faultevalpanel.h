#pragma once

#include <QWidget>

#include "faultevaluator.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QProgressBar;
class QPushButton;
class QTableWidget;

/* 数据集评估页：选择运行包、评估集与后端，逐窗推断后按真值统计分类与
 * 分级准确度，并给出单窗处理速度。按钮只发信号，加载与推断在评估线程完成。 */
class FaultEvalPanel : public QWidget
{
    Q_OBJECT
public:
    explicit FaultEvalPanel(QWidget *parent = nullptr);

    QString packageDirectory() const;
    QString datasetDirectory() const;
    int backend() const;
    void setPackageDirectory(const QString &directory);
    void setDatasetDirectory(const QString &directory);
    void setBackend(int backend);

    void setBusy(bool busy);
    void showState(const QString &detail);
    void showProgress(int done, int total);
    void showReport(const mlk::FaultEvalReport &report);
    void clearReport();

signals:
    void startRequested(const QString &packageRoot, const QString &datasetRoot, int backend);
    void stopRequested();

private:
    void exportReport();

    QLineEdit *m_package = nullptr;
    QLineEdit *m_dataset = nullptr;
    QComboBox *m_backend = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_export = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_speed = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_confusion = nullptr;
    mlk::FaultEvalReport m_report;
    bool m_hasReport = false;
};
