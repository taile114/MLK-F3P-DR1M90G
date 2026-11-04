#include "sensorpanel.h"

#include <QtWidgets>
#include <cmath>

#include "plotwidget.h"

namespace {

QString fmt(double v)
{
    const double a = std::abs(v);
    if (a >= 1000)
        return QString::number(v, 'f', 0);
    if (a >= 100)
        return QString::number(v, 'f', 1);
    return QString::number(v, 'f', 2);
}

/* 合成矢量曲线的名称与颜色：颜色与 X(0x5aa9e6) / Y(0x37c08a) / Z(0xe6c037) 区分。 */
QString magnitudeName()
{
    return QStringLiteral("合成");
}

QColor magnitudeColor()
{
    return QColor(0xe6, 0x5a, 0x5a);
}

/* 三轴合成 sqrt(x^2+y^2+z^2)，结果仍是原始 LSB 计数，供波形图与统计共用。
 * 量程 ±16 g / 256 LSB/g 时单轴上限 4096，三轴合成上限约 7094，不超出 qint16。 */
qint16 magnitudeCounts(qint16 x, qint16 y, qint16 z)
{
    const double dx = double(x);
    const double dy = double(y);
    const double dz = double(z);
    const long v = std::lround(std::sqrt(dx * dx + dy * dy + dz * dz));
    return qint16(qMin<long>(32767, v));
}

} // namespace

SensorPanel::SensorPanel(const QString &name, const QString &unit, const QColor &accent,
                         QWidget *parent)
    : QGroupBox(name, parent)
    , m_value(new QLabel(QStringLiteral("-"), this))
    , m_laneToggle(new QPushButton(QStringLiteral("分道"), this))
    , m_zero(new QPushButton(QStringLiteral("归零"), this))
    , m_unitLabel(new QLabel(unit, this))
    , m_toggle(new QPushButton(QStringLiteral("频谱"), this))
    , m_plot(new mlk::PlotWidget(this))
    , m_unit(unit)
    , m_accent(accent)
{
    m_unitLabel->setObjectName(QStringLiteral("sensorUnit"));
    auto *top = new QHBoxLayout;
    m_basePointSize = m_value->font().pointSize();
    applyValueFont();
    m_value->setStyleSheet(QStringLiteral("color: %1;").arg(m_accent.name()));
    m_toggle->setCheckable(true);
    /* 与“频谱/时域”按钮同一套交互：按钮写的是点下去会切到哪种显示。
     * 只有同一图里画多条曲线（振动三轴 + 合成）时才有意义，单序列面板隐藏。 */
    m_laneToggle->setText(QStringLiteral("分道"));
    m_laneToggle->setToolTip(QStringLiteral("多条曲线分成等距道显示；再点回到叠加显示"));
    m_laneToggle->setVisible(false);
    m_zero->setToolTip(QStringLiteral("把当前读数记为电流零点，仅扣除霍尔传感器与 ADC 的静态偏移"));
    m_zero->setVisible(false);
    top->addWidget(m_value);
    top->addWidget(m_unitLabel);
    top->addStretch(1);
    /* 右侧按钮组：分道/归零紧挨「频谱」，与振动面板的分道位置一致。 */
    top->addWidget(m_laneToggle);
    top->addWidget(m_zero);
    top->addWidget(m_toggle);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(top);
    lay->addWidget(m_plot, 1);

    setNamedSeriesCount(1);
    connect(m_toggle, &QPushButton::clicked, this, &SensorPanel::toggleMode);
    connect(m_laneToggle, &QPushButton::clicked, this, &SensorPanel::toggleLanes);
    connect(m_zero, &QPushButton::clicked, this, [this] { emit zeroRequested(); });
}

int SensorPanel::plotSeriesCount() const
{
    return m_named + (magnitudeOn() ? 1 : 0);
}

bool SensorPanel::magnitudeOn() const
{
    /* 合成曲线需要三轴同时存在；setVectorMagnitude 只负责标记该面板是否启用。 */
    return m_lsbPerG > 0.0 && m_named >= 3;
}

void SensorPanel::toggleLanes()
{
    m_lanes = !m_lanes;
    m_laneToggle->setText(m_lanes ? QStringLiteral("叠加") : QStringLiteral("分道"));
    m_plot->setSeriesLanes(plotSeriesCount() > 1 && m_lanes);
}

void SensorPanel::syncPlotSeries()
{
    m_plotSeries = plotSeriesCount();
    m_laneToggle->setVisible(m_plotSeries > 1);
    m_plot->setSeriesCount(m_plotSeries);
    /* 多序列默认分道等距显示；单序列仍是整幅叠加。 */
    m_plot->setSeriesLanes(m_plotSeries > 1 && m_lanes);
    for (int i = 0; i < m_named; ++i) {
        if (i < m_names.size())
            m_plot->setSeriesName(i, m_names[i]);
        if (i < m_colors.size())
            m_plot->setSeriesColor(i, m_colors[i]);
    }
    if (magnitudeOn()) {
        m_plot->setSeriesName(m_named, magnitudeName());
        m_plot->setSeriesColor(m_named, magnitudeColor());
    }
}

void SensorPanel::applyValueFont()
{
    QFont f = m_value->font();
    const int base = m_basePointSize > 0 ? m_basePointSize : f.pointSize();
    /* 多序列面板一行并排四组数值，字号取基础字号+1，使半宽面板内不折行；
     * 单序列面板沿用基础字号+4。bold 与强调色对所有面板保持不变。 */
    if (base > 0)
        f.setPointSize(base + (m_named > 1 ? 1 : 4));
    f.setBold(true);
    m_value->setFont(f);
}

QString SensorPanel::axisName(int index) const
{
    if (index >= 0 && index < m_names.size() && !m_names[index].isEmpty())
        return m_names[index];
    static const char *const kAxis[3] = { "X", "Y", "Z" };
    if (index >= 0 && index < 3)
        return QString::fromLatin1(kAxis[index]);
    return QString::number(index + 1);
}

void SensorPanel::setNamedSeriesCount(int n)
{
    if (n < 1)
        n = 1;
    if (m_named == n && m_plotSeries == plotSeriesCount())
        return;
    m_named = n;
    syncPlotSeries();
    m_unitLabel->setText(m_named == 1 ? m_unit : QString());
    applyValueFont();
}

void SensorPanel::setSeriesNames(const QStringList &names)
{
    m_names = names;
    setNamedSeriesCount(names.size());
    /* 序列数不变时名称仍需写入波形图。 */
    syncPlotSeries();
}

void SensorPanel::setSeriesColors(const QList<QColor> &colors)
{
    m_colors = colors;
    setNamedSeriesCount(colors.size());
    syncPlotSeries();
}

void SensorPanel::setSpectrum(const QVector<quint32> &power)
{
    if (power.size() < 2) {
        m_plot->clearSpectrumData();
        return;
    }
    quint32 peak = 1;
    for (quint32 v : power)
        peak = qMax(peak, v);
    const double p0 = double(peak);
    QVector<double> db;
    db.reserve(power.size());
    for (quint32 v : power) {
        const double rel = double(v) / p0;
        db.append(rel > 0.0 ? 10.0 * std::log10(rel) : -120.0);
    }
    m_plot->setSpectrumData(db, m_rateHz > 0 ? m_rateHz / 2.0 : 0.0);
}

void SensorPanel::setVectorMagnitude(double lsbPerG)
{
    const bool was = magnitudeOn();
    m_lsbPerG = lsbPerG > 0.0 ? lsbPerG : 0.0;
    if (magnitudeOn() != was)
        syncPlotSeries();
    m_dirty = true;
}

void SensorPanel::setScale(double k, double b)
{
    m_k = k;
    m_b = b;
    m_plot->setDisplayScale(k, m_unit);
}

void SensorPanel::setAlarm(double lo, double hi, bool enabled)
{
    m_alarmLo = lo;
    m_alarmHi = hi;
    m_alarmEnabled = enabled;
}

void SensorPanel::setFftPoints(int n)
{
    m_plot->setFftPoints(n);
}

void SensorPanel::setPlotMinimumHeight(int h)
{
    m_plot->setMinimumHeight(h);
}

const QVector<qint32> &SensorPanel::buffer() const
{
    return m_plot->samples();
}

const QVector<qint32> &SensorPanel::series(int series) const
{
    return m_plot->series(series);
}

void SensorPanel::addSamples(const QVector<qint32> &s, int rateHz)
{
    addMulti({s}, rateHz);
}

void SensorPanel::addSamples(const QVector<qint32> &x, const QVector<qint32> &y,
                             const QVector<qint32> &z, int rateHz)
{
    addMulti({x, y, z}, rateHz);
}

void SensorPanel::addMulti(const QVector<QVector<qint32>> &series, int rateHz)
{
    if (series.isEmpty() || series[0].isEmpty())
        return;
    setNamedSeriesCount(series.size());
    m_plot->setRateHz(rateHz);

    for (int i = 0; i < series.size(); ++i)
        m_plot->addSamples(i, series[i]);

    if (magnitudeOn() && series.size() >= 3) {
        /* 三轴样本数按最小值对齐，逐样本合成后作为命名序列之后的第 4 条曲线。 */
        const int n = qMin(series[0].size(), qMin(series[1].size(), series[2].size()));
        QVector<qint32> mag;
        mag.reserve(n);
        for (int i = 0; i < n; ++i)
            mag.append(magnitudeCounts(series[0][i], series[1][i], series[2][i]));
        if (!mag.isEmpty())
            m_plot->addSamples(m_named, mag);
    }

    m_have = true;
    m_dirty = true;
    m_latest = series;
    m_rateHz = rateHz;
}

void SensorPanel::refreshUi()
{
    if (!m_have || !m_dirty || m_latest.isEmpty() || m_latest[0].isEmpty())
        return;
    m_dirty = false;
    const QVector<QVector<qint32>> &series = m_latest;
    const double lastX = series[0].back() * m_k + m_b;
    m_lastPhysical = lastX;
    const bool multi = series.size() > 1;
    bool alarm = false;
    QStringList values;
    for (int i = 0; i < series.size(); ++i) {
        const double v = series[i].back() * m_k + m_b;
        alarm = alarm || (m_alarmEnabled && (v < m_alarmLo || v > m_alarmHi));
        if (multi)
            values << QStringLiteral("%1 %2").arg(axisName(i)).arg(fmt(v));
        else
            values << fmt(v);
    }
    if (magnitudeOn() && series.size() >= 3) {
        /* 合成值只作显示，不参与告警判定，保持既有单轴阈值语义。 */
        const qint16 mag = magnitudeCounts(series[0].back(), series[1].back(), series[2].back());
        const double v = double(mag) * m_k + m_b;
        values << QStringLiteral("%1 %2 %3").arg(magnitudeName()).arg(fmt(v)).arg(m_unit);
    }
    /* 多序列面板的数值只占一行（两个空格分隔），避免把下方波形挤小。 */
    if (multi)
        m_value->setText(values.join(QStringLiteral("  ")));
    else
        m_value->setText(values.join(QString()));
    if (alarm != m_alarmShown) {
        m_alarmShown = alarm;
        m_value->setStyleSheet(
            QStringLiteral("color: %1;").arg(alarm ? QStringLiteral("#e65a5a") : m_accent.name()));
    }
    m_plot->refresh();
}

void SensorPanel::setZeroButtonVisible(bool on)
{
    m_zero->setVisible(on);
}

void SensorPanel::setUnavailable(const QString &text)
{
    m_plot->clear();
    m_value->setText(text);
    m_have = false;
    m_dirty = false;
    m_latest.clear();
    /* PlotWidget::clear 只清缓冲，序列布局按命名序列数与合成状态重新同步。 */
    syncPlotSeries();
}

void SensorPanel::clear()
{
    m_plot->clear();
    m_value->setText(QStringLiteral("-"));
    m_have = false;
    m_dirty = false;
    m_latest.clear();
    syncPlotSeries();
}

void SensorPanel::toggleMode()
{
    const bool spectrum = m_toggle->isChecked();
    m_plot->setMode(spectrum ? mlk::PlotWidget::Spectrum : mlk::PlotWidget::Time);
    m_toggle->setText(spectrum ? QStringLiteral("时域") : QStringLiteral("频谱"));
    /* 分道只作用于时域绘制，频谱视图下不适用。 */
    m_laneToggle->setEnabled(!spectrum);
}
