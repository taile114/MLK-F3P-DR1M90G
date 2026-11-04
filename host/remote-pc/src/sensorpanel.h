#pragma once

#include <QColor>
#include <QGroupBox>
#include <QList>
#include <QStringList>
#include <QVector>

class QLabel;
class QPushButton;

namespace mlk {
class PlotWidget;
}

class SensorPanel : public QGroupBox
{
    Q_OBJECT
public:
    SensorPanel(const QString &name, const QString &unit, const QColor &accent,
                QWidget *parent = nullptr);

    void setScale(double k, double b);
    void setAlarm(double lo, double hi, bool enabled);
    void setFftPoints(int n);
    void setPlotMinimumHeight(int h);
    void setSeriesNames(const QStringList &names);
    void setSeriesColors(const QList<QColor> &colors);
    /* 三轴面板的合成矢量幅值：逐样本按 sqrt(x^2+y^2+z^2) 保存在原始计数域，
     * 作为波形图的第 4 条曲线并与三轴数值同行显示；物理量换算沿用 setScale。
     * lsbPerG 标记该面板是否启用合成（传 0 关闭，单轴面板无需设置）。 */
    void setVectorMagnitude(double lsbPerG);
    /* PL 频谱窗口（契约 §4.6）：原始 (|X_k|²)>>8 序列，换算成相对峰值 dB 后
     * 交给绘图；只在频谱模式下显示，时域模式不受影响。 */
    void setSpectrum(const QVector<quint32> &power);
    void addSamples(const QVector<qint32> &s, int rateHz);
    void addSamples(const QVector<qint32> &x, const QVector<qint32> &y,
                    const QVector<qint32> &z, int rateHz);
    void refreshUi();
    void setUnavailable(const QString &text);
    /* 电流面板的「归零」按钮：把当前采样记为 CURRENT_ZERO，只显示偏移后的值。 */
    void setZeroButtonVisible(bool on);
    void clear();

    const QVector<qint32> &buffer() const;
    const QVector<qint32> &series(int series) const;
    double lastPhysical() const { return m_lastPhysical; }

signals:
    void zeroRequested();

private slots:
    void toggleMode();

private:
    void setNamedSeriesCount(int n);
    void addMulti(const QVector<QVector<qint32>> &series, int rateHz);
    /* 波形图序列数 = 命名序列数 m_named + 合成曲线（生效时）。
     * PlotWidget::setSeriesCount 是 resize，序列数变少会丢掉多余缓冲，
     * 因此不能按输入序列数直接设置，否则每帧追加样本时会清掉合成曲线。 */
    int plotSeriesCount() const;
    bool magnitudeOn() const;
    void syncPlotSeries();
    void toggleLanes();
    void applyValueFont();
    QString axisName(int index) const;

    QLabel *m_value;
    QPushButton *m_laneToggle;
    QPushButton *m_zero;
    QLabel *m_unitLabel;
    QPushButton *m_toggle;
    mlk::PlotWidget *m_plot;
    QString m_unit;
    QColor m_accent;
    double m_k = 1.0;
    double m_b = 0.0;
    double m_alarmLo = 0.0;
    double m_alarmHi = 0.0;
    bool m_alarmEnabled = false;
    bool m_lanes = false;    /* 显示方式：false = 叠加（默认），true = 分道等距 */
    int m_plotSeries = 1;    /* 已应用到波形图的序列数 */
    double m_lastPhysical = 0.0;
    double m_lsbPerG = 0.0;
    bool m_have = false;
    bool m_dirty = false;
    bool m_alarmShown = false;
    QVector<QVector<qint32>> m_latest;
    int m_rateHz = 0;
    QStringList m_names;
    QList<QColor> m_colors;
    int m_named = 1;
    int m_basePointSize = 0;
};
