#include "plotwidget.h"
#include <limits>

#include <cmath>

#include <QPainter>
#include <QPainterPath>

#include "fft.h"

namespace mlk {

PlotWidget::PlotWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setSeriesCount(1);
    setMinimumHeight(150);
}

void PlotWidget::ensureSeries(int series)
{
    if (series < 0)
        return;
    while (m_series.size() <= series) {
        m_series.append(QVector<qint32>());
        m_colors.append(QColor(0x5a, 0xa9, 0xe6));
        m_names.append(QString());
    }
}

void PlotWidget::setSeriesCount(int n)
{
    if (n < 1)
        n = 1;
    if (m_series.size() == n)
        return;
    m_series.resize(n);
    while (m_colors.size() < n)
        m_colors.append(QColor(0x5a, 0xa9, 0xe6));
    while (m_names.size() < n)
        m_names.append(QString());
    update();
}

void PlotWidget::setSeriesColor(int series, const QColor &color)
{
    ensureSeries(series);
    m_colors[series] = color;
    update();
}

void PlotWidget::setSeriesName(int series, const QString &name)
{
    ensureSeries(series);
    m_names[series] = name;
    update();
}

void PlotWidget::setDisplayScale(double scale, const QString &unit)
{
    m_displayScale = scale > 0 ? scale : 1.0;
    m_unit = unit;
    update();
}

void PlotWidget::setSeriesLanes(bool on)
{
    if (m_lanes == on)
        return;
    m_lanes = on;
    update();
}

void PlotWidget::setSpectrumData(const QVector<double> &db, double spanHz)
{
    m_spectrumDb = db;
    m_spectrumSpanHz = spanHz > 0 ? spanHz : 0.0;
    update();
}

void PlotWidget::clearSpectrumData()
{
    if (m_spectrumDb.isEmpty())
        return;
    m_spectrumDb.clear();
    m_spectrumSpanHz = 0.0;
    update();
}

void PlotWidget::addSamples(const QVector<qint32> &s)
{
    addSamples(0, s);
}

void PlotWidget::addSamples(int series, const QVector<qint32> &s)
{
    if (s.isEmpty())
        return;
    ensureSeries(series);
    QVector<qint32> &buf = m_series[series];
    buf += s;
    if (buf.size() > m_max)
        buf.remove(0, buf.size() - m_max);
}

void PlotWidget::refresh()
{
    update();
}

void PlotWidget::clear()
{
    for (auto &buf : m_series)
        buf.clear();
    update();
}

void PlotWidget::setMode(Mode mode)
{
    m_mode = mode;
    update();
}

void PlotWidget::setRateHz(int rate)
{
    if (rate > 0)
        m_rateHz = rate;
}

void PlotWidget::setFftPoints(int n)
{
    if (n >= 64)
        m_fftPoints = n;
    update();
}

const QVector<qint32> &PlotWidget::samples() const
{
    return series(0);
}

const QVector<qint32> &PlotWidget::series(int series) const
{
    static const QVector<qint32> empty;
    if (series < 0 || series >= m_series.size())
        return empty;
    return m_series[series];
}

void PlotWidget::paintEvent(QPaintEvent *)
{
    QPainter pr(this);
    pr.fillRect(rect(), QColor(0x11, 0x16, 0x1b));

    const QRectF r = QRectF(rect()).adjusted(6, 6, -6, -44);
    pr.setPen(QColor(0x2a, 0x33, 0x3d));
    pr.drawRect(r);
    for (int i = 1; i < 8; ++i) {
        const qreal x = r.left() + r.width() * i / 8.0;
        pr.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    for (int i = 1; i < 4; ++i) {
        const qreal y = r.top() + r.height() * i / 4.0;
        pr.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }

    m_infoText.clear();
    bool any = false;
    for (const auto &buf : m_series)
        any = any || buf.size() >= 2;

    if (!any) {
        pr.setPen(QColor(0x7a, 0x86, 0x92));
        pr.drawText(r, Qt::AlignCenter, tr("no data"));
    } else if (m_mode == Time) {
        paintTime(pr, r);
    } else {
        paintSpectrum(pr, r);
    }
    if (!m_infoText.isEmpty()) {
        pr.setPen(QColor(0x7a, 0x86, 0x92));
        QFont f = pr.font();
        f.setPointSize(qMax(7, f.pointSize() - 2));
        pr.setFont(f);
        pr.drawText(QPointF(r.left() + 2, r.bottom() + 16), m_infoText);
    }
    paintLegend(pr, rect());
}

void PlotWidget::paintLegend(QPainter &pr, const QRectF &widgetRect)
{
    if (m_names.isEmpty())
        return;
    QFont f = pr.font();
    f.setPointSize(qMax(7, f.pointSize() - 2));
    pr.setFont(f);
    qreal total = 0;
    for (int i = 0; i < m_names.size(); ++i) {
        if (m_names[i].isEmpty())
            continue;
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        total += pr.fontMetrics().horizontalAdvance(m_names[i]) + 14;
#else
        total += pr.fontMetrics().width(m_names[i]) + 14;
#endif
    }
    qreal x = widgetRect.right() - total - 10;
    const qreal y = widgetRect.bottom() - 8;
    for (int i = 0; i < m_names.size(); ++i) {
        if (m_names[i].isEmpty())
            continue;
        pr.setPen(m_colors.value(i, QColor(0x7a, 0x86, 0x92)));
        pr.drawText(QPointF(x, y), m_names[i]);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        x += pr.fontMetrics().horizontalAdvance(m_names[i]) + 14;
#else
        x += pr.fontMetrics().width(m_names[i]) + 14;
#endif
    }
}

void PlotWidget::paintTime(QPainter &pr, const QRectF &r)
{
    if (m_lanes && m_series.size() > 1) {
        paintTimeLanes(pr, r);
        return;
    }

    qreal lo = std::numeric_limits<qint32>::max(), hi = std::numeric_limits<qint32>::min();
    bool have = false;
    for (const auto &buf : m_series) {
        for (qint32 v : buf) {
            lo = qMin<qreal>(lo, v);
            hi = qMax<qreal>(hi, v);
            have = true;
        }
    }
    if (!have)
        return;
    if (hi - lo < 16) {
        const qreal c = (hi + lo) / 2;
        lo = c - 8;
        hi = c + 8;
    }
    const qreal pad = (hi - lo) * 0.1;
    lo -= pad;
    hi += pad;

    for (int s = 0; s < m_series.size(); ++s) {
        const QVector<qint32> &buf = m_series[s];
        const int n = buf.size();
        if (n < 2)
            continue;
        const int stepN = qMax(1, n / qMax(2, int(r.width()) * 2));
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < n; i += stepN) {
            const qreal x = r.left() + r.width() * qreal(i) / (n - 1);
            const qreal y = r.bottom() - r.height() * (qreal(buf[i]) - lo) / (hi - lo);
            if (started)
                path.lineTo(x, y);
            else {
                path.moveTo(x, y);
                started = true;
            }
        }
        if (started)
            path.lineTo(r.right(),
                        r.bottom() - r.height() * (qreal(buf[n - 1]) - lo) / (hi - lo));
        pr.setPen(QPen(m_colors.value(s, QColor(0x37, 0xc0, 0x8a)), 1));
        pr.drawPath(path);
    }

    m_infoText = QStringLiteral("min %1  max %2 %3")
                     .arg(lo * m_displayScale, 0, 'f', 1)
                     .arg(hi * m_displayScale, 0, 'f', 1)
                     .arg(m_unit);
}

void PlotWidget::paintTimeLanes(QPainter &pr, const QRectF &r)
{
    /* 每序列一条等高泳道：基线间距 = r.height()/N，固定；各道按自身中点居中，
     * 但共用同一 px/LSB 增益（取各道范围的最大值），幅度仍可直接比较。 */
    const int count = m_series.size();
    QVector<qreal> lo(count, 0), hi(count, 0), mid(count, 0);
    qreal span = 0;
    bool have = false;
    for (int s = 0; s < count; ++s) {
        qreal l = std::numeric_limits<qint32>::max(), h = std::numeric_limits<qint32>::min();
        for (qint32 v : m_series[s]) {
            l = qMin<qreal>(l, v);
            h = qMax<qreal>(h, v);
            have = true;
        }
        if (h - l < 16) {
            const qreal c = (h + l) / 2;
            l = c - 8;
            h = c + 8;
        }
        lo[s] = l;
        hi[s] = h;
        mid[s] = (l + h) / 2;
        span = qMax(span, h - l);
    }
    if (!have || span <= 0)
        return;

    const qreal laneH = r.height() / count;
    const qreal gain = laneH * 0.8 / span;   /* 每道上下留 10% 余量 */

    for (int s = 0; s < count; ++s) {
        const qreal top = r.top() + laneH * s;
        const qreal center = top + laneH * 0.5;

        if (s > 0) {
            pr.setPen(QColor(0x2a, 0x33, 0x3d));
            pr.drawLine(QPointF(r.left(), top), QPointF(r.right(), top));
        }

        /* 零线（仅在该道范围内时绘制），用于看直流分量。 */
        const qreal zeroY = center + mid[s] * gain;
        if (mid[s] != 0.0 && zeroY > top + laneH * 0.1 && zeroY < top + laneH * 0.9) {
            QPen zp(QColor(0x3a, 0x45, 0x50), 1, Qt::DashLine);
            pr.setPen(zp);
            pr.drawLine(QPointF(r.left(), zeroY), QPointF(r.right(), zeroY));
        }

        QFont f = pr.font();
        f.setPointSize(qMax(7, f.pointSize() - 2));
        pr.setFont(f);
        if (s < m_names.size() && !m_names[s].isEmpty()) {
            /* 道名放左上角并加底色，避免压住居中绘制的曲线。 */
            const qreal lh = qMin<qreal>(16.0, laneH * 0.4);
            const QRectF box(r.left() + 2, top + 2, 52, lh);
            pr.fillRect(box, QColor(0x0d, 0x12, 0x18, 200));
            pr.setPen(m_colors.value(s, QColor(0x7a, 0x86, 0x92)));
            pr.drawText(box.adjusted(4, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, m_names[s]);
        }

        const QVector<qint32> &buf = m_series[s];
        const int n = buf.size();
        if (n < 2)
            continue;
        const int stepN = qMax(1, n / qMax(2, int(r.width()) * 2));
        QPainterPath path;
        bool started = false;
        pr.save();
        pr.setClipRect(QRectF(r.left(), top, r.width(), laneH));
        for (int i = 0; i < n; i += stepN) {
            const qreal x = r.left() + r.width() * qreal(i) / (n - 1);
            const qreal y = center - (qreal(buf[i]) - mid[s]) * gain;
            if (started)
                path.lineTo(x, y);
            else {
                path.moveTo(x, y);
                started = true;
            }
        }
        if (started)
            path.lineTo(r.right(), center - (qreal(buf[n - 1]) - mid[s]) * gain);
        pr.setPen(QPen(m_colors.value(s, QColor(0x37, 0xc0, 0x8a)), 1));
        pr.drawPath(path);
        pr.restore();
    }

    m_infoText = QStringLiteral("%1 道  ±%2 %3")
                     .arg(count)
                     .arg(span * 0.5 * m_displayScale, 0, 'f', 2)
                     .arg(m_unit);
}

void PlotWidget::drawExternalSpectrum(QPainter &pr, const QRectF &r)
{
    const int n = m_spectrumDb.size();
    qreal lo = 0.0, hi = -1.0e9;
    for (double v : m_spectrumDb) {
        if (!std::isfinite(v))
            continue;
        lo = qMin(lo, v);
        hi = qMax(hi, v);
    }
    if (hi < lo)
        hi = lo + 12.0;
    if (hi - lo < 12.0)
        lo = hi - 12.0;

    const int stepN = qMax(1, n / qMax(2, int(r.width()) * 2));
    QPainterPath path;
    bool started = false;
    for (int i = 0; i < n; i += stepN) {
        const double v = std::isfinite(m_spectrumDb[i]) ? m_spectrumDb[i] : lo;
        const qreal x = r.left() + r.width() * qreal(i) / (n - 1);
        const qreal y = r.bottom() - r.height() * (v - lo) / (hi - lo);
        if (started)
            path.lineTo(x, y);
        else {
            path.moveTo(x, y);
            started = true;
        }
    }
    pr.setPen(QPen(m_colors.value(0, QColor(0x5a, 0xa9, 0xe6)), 1));
    pr.drawPath(path);

    const double fmax = m_spectrumSpanHz > 0 ? m_spectrumSpanHz : m_rateHz / 2.0;
    m_infoText = QStringLiteral("PL 频谱 %1 点  0 - %2 Hz  %3 dB").arg(n).arg(fmax, 0, 'f', 1)
                     .arg(hi - lo, 0, 'f', 0);
}

void PlotWidget::paintSpectrum(QPainter &pr, const QRectF &r)
{
    if (m_spectrumDb.size() >= 2) {
        drawExternalSpectrum(pr, r);
        return;
    }
    int nfft = 0;
    qreal lo = -120, hi = -20;
    bool have = false;
    QVector<QVector<double>> spectra;
    for (const auto &buf : m_series) {
        int n = 0;
        const QVector<double> mag = powerSpectrumDb(buf, n, m_fftPoints);
        spectra.append(mag);
        if (!mag.isEmpty()) {
            nfft = qMax(nfft, n);
            for (double v : mag) {
                lo = qMin(lo, v);
                hi = qMax(hi, v);
                have = true;
            }
        }
    }
    if (!have) {
        pr.setPen(QColor(0x7a, 0x86, 0x92));
        pr.drawText(r, Qt::AlignCenter, tr("no data"));
        return;
    }
    if (hi - lo < 12)
        hi = lo + 12;

    for (int s = 0; s < spectra.size(); ++s) {
        const QVector<double> &mag = spectra[s];
        const int n = mag.size();
        if (n < 2)
            continue;
        const int stepN = qMax(1, n / qMax(2, int(r.width()) * 2));
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < n; i += stepN) {
            const qreal x = r.left() + r.width() * qreal(i) / (n - 1);
            const qreal y = r.bottom() - r.height() * (mag[i] - lo) / (hi - lo);
            if (started)
                path.lineTo(x, y);
            else {
                path.moveTo(x, y);
                started = true;
            }
        }
        pr.setPen(QPen(m_colors.value(s, QColor(0x5a, 0xa9, 0xe6)), 1));
        pr.drawPath(path);
    }

    const double fmax = m_rateHz / 2.0;
    m_infoText = QStringLiteral("FFT %1 pts  0 - %2 Hz").arg(nfft).arg(fmax, 0, 'f', 1);
}

} // namespace mlk
