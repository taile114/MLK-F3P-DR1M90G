#pragma once

#include <QDialog>

class QDoubleSpinBox;
class QSpinBox;

class InjectDialog : public QDialog
{
    Q_OBJECT
public:
    InjectDialog(double temp, double current, double vib, double speed, int durationMs,
                 QWidget *parent = nullptr);

    double value(int i) const;
    int durationMs() const;

private:
    QDoubleSpinBox *m_v[4];
    QSpinBox *m_dur;
};
