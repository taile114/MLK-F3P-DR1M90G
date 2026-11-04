#pragma once

#include <QObject>
#include <QVector>
#include "npustats.h"

class QLabel;
class QProgressBar;
class QVBoxLayout;

namespace mlk {
class PlotWidget;

// Adds the same title/value/bar/history layout to both resource drawers.
class NpuPanel : public QObject
{
public:
    explicit NpuPanel(QVBoxLayout *layout);
    void updateStats(const NpuStats &stats);
    void setUnavailable(const QString &reason);

private:
    QLabel *m_text;
    QProgressBar *m_bar;
    PlotWidget *m_plot;
    QVector<double> m_rates[2];
};
} // namespace mlk
