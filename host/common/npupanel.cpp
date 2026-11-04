#include "npupanel.h"
#include "plotwidget.h"

#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <cmath>

namespace mlk {
NpuPanel::NpuPanel(QVBoxLayout *layout) : QObject(layout)
{
    auto *parent = layout->parentWidget();
    layout->addWidget(new QLabel(QStringLiteral("NPU"), parent));
    m_text = new QLabel(parent);
    m_text->setObjectName(QStringLiteral("npuStatus"));
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::PlainText);
    m_text->setToolTip(QStringLiteral(
        "驱动就绪要求硬核、软核和 CMA 均为字符设备。\n"
        "中断速率来自板端 /proc/interrupts，不代表百分比使用率或推理成功；无中断不能判定硬件空闲。\n"
        "累计值可以包含其他进程；FF6 使用硬核，软核为 0 属于正常情况。"));
    layout->addWidget(m_text);
    m_bar = new QProgressBar(parent);
    m_bar->setObjectName(QStringLiteral("npuBar"));
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setEnabled(false);
    layout->addWidget(m_bar);
    m_plot = new PlotWidget(parent);
    m_plot->setObjectName(QStringLiteral("npuPlot"));
    m_plot->setSeriesCount(2);
    m_plot->setSeriesName(0, QStringLiteral("硬核"));
    m_plot->setSeriesName(1, QStringLiteral("软核"));
    m_plot->setSeriesColor(0, QColor(0xbc, 0x8c, 0xff));
    m_plot->setSeriesColor(1, QColor(0xe6, 0xc0, 0x37));
    m_plot->setDisplayScale(0.1, QStringLiteral("次/s"));
    m_plot->setRateHz(1);
    m_plot->setMinimumHeight(60);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_plot->setToolTip(QStringLiteral("最近 60 个有效状态样本的板端 NPU 中断速率（次/s）；统计失效时清除对应曲线。"));
    layout->addWidget(m_plot, 1);
    setUnavailable(QStringLiteral("等待板端 NPU 状态"));
}

void NpuPanel::setUnavailable(const QString &reason)
{
    m_text->setText(reason);
    m_bar->setFormat(QStringLiteral("使用率：不可用"));
    for (auto &rates : m_rates) rates.clear();
    m_plot->clear();
}

void NpuPanel::updateStats(const NpuStats &stats)
{
    auto text = stats.text();
    text.truncate(text.lastIndexOf(QLatin1Char('\n')));
    m_text->setText(text);
    m_bar->setFormat(QStringLiteral("使用率：驱动未提供"));
    const double rates[] = {stats.hardInterruptsPerSec, stats.softInterruptsPerSec};
    const bool valid[] = {stats.hardReady && stats.hardIrqAvailable,
                          stats.softReady && stats.softIrqAvailable};
    double peak = 0;
    for (int i = 0; i < 2; ++i) {
        if (!valid[i] || !std::isfinite(rates[i]) || rates[i] < 0) m_rates[i].clear();
        else {
            m_rates[i].append(rates[i]);
            if (m_rates[i].size() > 60) m_rates[i].removeFirst();
        }
        for (double value : m_rates[i]) peak = qMax(peak, value);
    }
    // Keep full rates in the history and scale only the plotting buffer.
    const double scale = qMax(0.1, peak / 30000.0);
    m_plot->setDisplayScale(scale, QStringLiteral("次/s"));
    m_plot->clear();
    for (int i = 0; i < 2; ++i) {
        QVector<qint32> samples;
        for (double value : m_rates[i]) samples.append(qint16(qRound(value / scale)));
        m_plot->addSamples(i, samples);
    }
    m_plot->refresh();
}
} // namespace mlk
