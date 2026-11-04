#include "injectdialog.h"

#include <QtWidgets>

namespace {
const char *kNames[4] = { "温度", "电流", "振动", "转速" };
const char *kUnits[4] = { "℃", "A", "g", "rpm" };
} // namespace

InjectDialog::InjectDialog(double temp, double current, double vib, double speed, int durationMs,
                           QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("模拟数据注入"));

    auto *root = new QVBoxLayout(this);

    auto *box = new QGroupBox(QStringLiteral("注入值（物理量）"), this);
    auto *form = new QFormLayout(box);
    const double init[4] = { temp, current, vib, speed };
    for (int i = 0; i < 4; ++i) {
        m_v[i] = new QDoubleSpinBox(box);
        m_v[i]->setRange(-1e6, 1e6);
        m_v[i]->setDecimals(3);
        m_v[i]->setSuffix(QStringLiteral(" %1").arg(QString::fromUtf8(kUnits[i])));
        m_v[i]->setValue(init[i]);
        form->addRow(QString::fromUtf8(kNames[i]), m_v[i]);
    }
    m_dur = new QSpinBox(box);
    m_dur->setRange(100, 60000);
    m_dur->setSuffix(QStringLiteral(" ms"));
    m_dur->setValue(durationMs);
    form->addRow(QStringLiteral("注入时长"), m_dur);
    root->addWidget(box);

    auto *tip = new QLabel(QStringLiteral("上传后 PS 暂停采集、以注入值送 NPU，诊断工单回传本机。"),
                           this);
    tip->setWordWrap(true);
    root->addWidget(tip);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("上传"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);
}

double InjectDialog::value(int i) const
{
    return m_v[i]->value();
}

int InjectDialog::durationMs() const
{
    return m_dur->value();
}
