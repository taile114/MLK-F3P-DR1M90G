#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

namespace mlk {

class PlotWidget : public QWidget
{
    Q_OBJECT
public:
    enum Mode {
        Time,
        Spectrum,
    };

    explicit PlotWidget(QWidget *parent = nullptr);

    void addSamples(const QVector<qint32> &s);
    void addSamples(int series, const QVector<qint32> &s);
    void refresh();
    void clear();

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    void setRateHz(int rate);
    void setFftPoints(int n);
    void setSeriesCount(int n);
    void setSeriesColor(int series, const QColor &color);
    void setSeriesName(int series, const QString &name);
    void setDisplayScale(double scale, const QString &unit = QString());
    /* 时域多序列分道显示：每条序列占一条等高等距的泳道，各泳道共用同一
     * px/LSB 增益（按各自中点居中），因此间距固定且幅度仍可直接比较。 */
    void setSeriesLanes(bool on);
    /* 外部频谱：由 PL 频谱窗口（契约 §4.6）算好的相对功率 dB（0 = 峰值），
     * 非空时取代本机 FFT 参与绘制；传空恢复本机计算。 */
    void setSpectrumData(const QVector<double> &db, double spanHz);
    void clearSpectrumData();

    const QVector<qint32> &samples() const;
    const QVector<qint32> &series(int series) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void paintTime(QPainter &pr, const QRectF &r);
    void paintTimeLanes(QPainter &pr, const QRectF &r);
    void paintSpectrum(QPainter &pr, const QRectF &r);
    void drawExternalSpectrum(QPainter &pr, const QRectF &r);
    void paintLegend(QPainter &pr, const QRectF &r);
    void ensureSeries(int series);

    QVector<QVector<qint32>> m_series;
    QVector<QColor> m_colors;
    QVector<QString> m_names;
    QImage m_cache;
    int m_max = 2048;
    int m_rateHz = 200;
    int m_fftPoints = 1024;
    double m_displayScale = 1.0;
    QString m_unit;
    QString m_infoText;
    Mode m_mode = Time;
    bool m_lanes = false;
    QVector<double> m_spectrumDb;
    double m_spectrumSpanHz = 0.0;
};

} // namespace mlk
