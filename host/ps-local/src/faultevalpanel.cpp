#include "faultevalpanel.h"

#include "faulttypes.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
constexpr int kClassCount = 5;
constexpr int kGradeCount = 4;
}

FaultEvalPanel::FaultEvalPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    auto *packageRow = new QHBoxLayout;
    packageRow->setSpacing(6);
    m_package = new QLineEdit(QStringLiteral("/opt/mlk_fault"), this);
    m_package->setObjectName(QStringLiteral("faultEvalPackage"));
    m_package->setToolTip(QStringLiteral("故障运行包目录：model.json、libmlk_fault.so 与 preprocess.json"));
    m_backend = new QComboBox(this);
    m_backend->setObjectName(QStringLiteral("faultEvalBackend"));
    m_backend->addItem(QStringLiteral("CPU+CNN NPU"), int(mlk::kFaultBackendCpuCnnNpu));
    m_backend->addItem(QStringLiteral("CPU"), int(mlk::kFaultBackendCpu));
    m_backend->setToolTip(QStringLiteral("评估后端；NPU 需要硬 NPU、软 NPU 与 CMA 设备"));
    packageRow->addWidget(new QLabel(QStringLiteral("运行包"), this));
    packageRow->addWidget(m_package, 1);
    packageRow->addWidget(m_backend);
    root->addLayout(packageRow);

    auto *datasetRow = new QHBoxLayout;
    datasetRow->setSpacing(6);
    m_dataset = new QLineEdit(this);
    m_dataset->setObjectName(QStringLiteral("faultEvalDataset"));
    m_dataset->setPlaceholderText(QStringLiteral("含 evalset.json、test_inputs.bin、test_index.csv 的目录"));
    m_start = new QPushButton(QStringLiteral("开始评估"), this);
    m_start->setObjectName(QStringLiteral("faultEvalStart"));
    m_stop = new QPushButton(QStringLiteral("停止"), this);
    m_stop->setObjectName(QStringLiteral("faultEvalStop"));
    m_stop->setEnabled(false);
    m_export = new QPushButton(QStringLiteral("导出报告"), this);
    m_export->setObjectName(QStringLiteral("faultEvalExport"));
    m_export->setEnabled(false);
    datasetRow->addWidget(new QLabel(QStringLiteral("评估集"), this));
    datasetRow->addWidget(m_dataset, 1);
    datasetRow->addWidget(m_start);
    datasetRow->addWidget(m_stop);
    datasetRow->addWidget(m_export);
    root->addLayout(datasetRow);

    auto *progressRow = new QHBoxLayout;
    progressRow->setSpacing(6);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("faultEvalProgress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_state = new QLabel(QStringLiteral("状态：未启动"), this);
    m_state->setObjectName(QStringLiteral("faultEvalState"));
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    m_state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_state, 2);
    root->addLayout(progressRow);

    m_summary = new QLabel(QStringLiteral("样本：-　类别准确率：-　联合准确率：-"), this);
    m_summary->setObjectName(QStringLiteral("faultEvalSummary"));
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    m_speed = new QLabel(QStringLiteral("处理速度：-"), this);
    m_speed->setObjectName(QStringLiteral("faultEvalSpeed"));
    m_speed->setTextFormat(Qt::PlainText);
    m_speed->setWordWrap(true);
    m_speed->setStyleSheet(QStringLiteral("font-size: 11px; color: #c9d1d9;"));
    root->addWidget(m_speed);

    m_table = new QTableWidget(kClassCount, 5, this);
    m_table->setObjectName(QStringLiteral("faultEvalTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("类别"), QStringLiteral("真值窗"),
                                        QStringLiteral("类别正确"), QStringLiteral("类别+等级正确"),
                                        QStringLiteral("联合准确率")});
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFixedHeight(150);
    for (int row = 0; row < kClassCount; ++row) {
        auto *name = new QTableWidgetItem(mlk::faultDisplayName(row));
        m_table->setItem(row, 0, name);
        for (int column = 1; column < 5; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(QStringLiteral("-")));
    }
    root->addWidget(m_table);

    m_confusion = new QLabel(this);
    m_confusion->setObjectName(QStringLiteral("faultEvalConfusion"));
    m_confusion->setTextFormat(Qt::PlainText);
    m_confusion->setStyleSheet(QStringLiteral("font-size: 11px; color: #c9d1d9;"));
    m_confusion->setWordWrap(true);
    m_confusion->setText(QStringLiteral("混淆矩阵（行为真值，列为预测）：-"));
    root->addWidget(m_confusion);

    connect(m_start, &QPushButton::clicked, this, [this] {
        emit startRequested(m_package->text().trimmed(), m_dataset->text().trimmed(), backend());
    });
    connect(m_stop, &QPushButton::clicked, this, [this] { emit stopRequested(); });
    connect(m_export, &QPushButton::clicked, this, [this] { exportReport(); });
}

QString FaultEvalPanel::packageDirectory() const { return m_package->text().trimmed(); }

QString FaultEvalPanel::datasetDirectory() const { return m_dataset->text().trimmed(); }

int FaultEvalPanel::backend() const { return m_backend->currentData().toInt(); }

void FaultEvalPanel::setBackend(int backend)
{
    const int index = m_backend->findData(backend);
    if (index >= 0) m_backend->setCurrentIndex(index);
}

void FaultEvalPanel::setPackageDirectory(const QString &directory)
{
    if (!directory.isEmpty()) m_package->setText(directory);
}

void FaultEvalPanel::setDatasetDirectory(const QString &directory)
{
    if (!directory.isEmpty()) m_dataset->setText(directory);
}

void FaultEvalPanel::setBusy(bool busy)
{
    m_start->setEnabled(!busy);
    m_stop->setEnabled(busy);
    m_package->setEnabled(!busy);
    m_dataset->setEnabled(!busy);
    m_backend->setEnabled(!busy);
}

void FaultEvalPanel::showState(const QString &detail)
{
    m_state->setText(QStringLiteral("状态：%1").arg(detail));
}

void FaultEvalPanel::showProgress(int done, int total)
{
    if (total <= 0) { m_progress->setValue(0); return; }
    m_progress->setRange(0, total);
    m_progress->setValue(done);
}

void FaultEvalPanel::clearReport()
{
    m_hasReport = false;
    m_report = mlk::FaultEvalReport();
    m_export->setEnabled(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_summary->setText(QStringLiteral("样本：-　类别准确率：-　联合准确率：-"));
    m_speed->setText(QStringLiteral("处理速度：-"));
    m_confusion->setText(QStringLiteral("混淆矩阵（行为真值，列为预测）：-"));
    for (int row = 0; row < kClassCount; ++row)
        for (int column = 1; column < 5; ++column)
            m_table->item(row, column)->setText(QStringLiteral("-"));
}

void FaultEvalPanel::showReport(const mlk::FaultEvalReport &report)
{
    m_report = report;
    m_hasReport = true;
    m_export->setEnabled(true);
    const int evaluated = report.evaluated;
    m_summary->setText(QStringLiteral(
        "样本 %1　类别正确 %2/%1（%3%）　类别+等级正确 %4/%1（%5%）　故障窗联合 %6/%7")
        .arg(evaluated)
        .arg(report.classCorrect)
        .arg(QString::number(report.classAccuracy() * 100.0, 'f', 2))
        .arg(report.jointCorrect)
        .arg(QString::number(report.jointAccuracy() * 100.0, 'f', 2))
        .arg(report.faultJointCorrect)
        .arg(report.faultSamples));

    /* 处理速度只含共享库单窗调用时间，不含读盘与界面刷新。 */
    const QString backendText = report.backend == QLatin1String("cpu_cnn_npu")
        ? QStringLiteral("CPU+CNN NPU")
        : report.backend == QLatin1String("cpu") ? QStringLiteral("CPU")
                                                 : QStringLiteral("未知");
    m_speed->setText(QStringLiteral(
        "处理速度（%1）：平均 %2 ms/窗　中位 %3 ms　P95 %4 ms　峰值 %5 ms　约 %6 窗/秒")
        .arg(backendText)
        .arg(QString::number(report.meanInferUs / 1000.0, 'f', 2))
        .arg(QString::number(report.medianInferUs / 1000.0, 'f', 2))
        .arg(QString::number(report.p95InferUs / 1000.0, 'f', 2))
        .arg(QString::number(report.maxInferUs / 1000.0, 'f', 2))
        .arg(QString::number(report.windowsPerSecond(), 'f', 2)));

    for (int truth = 0; truth < kClassCount; ++truth) {
        int rowTotal = 0, rowCorrect = 0;
        for (int predicted = 0; predicted < kClassCount; ++predicted) {
            const int value = truth < report.classConfusion.size()
                              && predicted < report.classConfusion[truth].size()
                                  ? report.classConfusion[truth][predicted] : 0;
            rowTotal += value;
            if (truth == predicted) rowCorrect += value;
        }
        const int rowJoint = truth < report.classJointCorrect.size()
                                 ? report.classJointCorrect[truth] : rowCorrect;
        m_table->item(truth, 1)->setText(QString::number(rowTotal));
        m_table->item(truth, 2)->setText(QString::number(rowCorrect));
        m_table->item(truth, 3)->setText(QString::number(rowJoint));
        m_table->item(truth, 4)->setText(rowTotal
            ? QStringLiteral("%1%").arg(QString::number(100.0 * rowJoint / rowTotal, 'f', 2))
            : QStringLiteral("-"));
    }

    QStringList lines;
    lines << QStringLiteral("混淆矩阵（行为真值，列为预测）：列序 正常/过载/欠压/过热/倾斜");
    for (int truth = 0; truth < report.classConfusion.size(); ++truth) {
        QStringList cells;
        for (int value : report.classConfusion[truth]) cells << QString::number(value);
        lines << QStringLiteral("%1：%2").arg(mlk::faultDisplayName(truth).leftJustified(4),
                                               cells.join(QStringLiteral("　")));
    }
    QStringList gradeCells;
    for (int truth = 0; truth < report.gradeConfusion.size(); ++truth) {
        QStringList cells;
        for (int value : report.gradeConfusion[truth]) cells << QString::number(value);
        gradeCells << QStringLiteral("真值 %1 级：%2").arg(truth).arg(cells.join(QStringLiteral("　")));
    }
    lines << QStringLiteral("分级混淆（行为真值等级 0～3，列为预测等级）：");
    lines << gradeCells;
    m_confusion->setText(lines.join(QLatin1Char('\n')));
    Q_UNUSED(kGradeCount);
}

void FaultEvalPanel::exportReport()
{
    if (!m_hasReport) return;
    const QString directory = m_dataset->text().trimmed();
    if (directory.isEmpty()) return;
    const QString path = QDir(directory).filePath(QStringLiteral("fault_eval_report.json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        showState(QStringLiteral("导出失败：%1").arg(file.errorString()));
        return;
    }
    const auto bytes = QJsonDocument(m_report.toJson()).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        showState(QStringLiteral("导出失败：%1").arg(file.errorString()));
        return;
    }
    showState(QStringLiteral("报告已写入 %1").arg(path));
}
