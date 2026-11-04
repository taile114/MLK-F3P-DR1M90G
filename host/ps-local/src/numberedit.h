#ifndef MLK_NUMBEREDIT_H
#define MLK_NUMBEREDIT_H

#include <QDoubleValidator>
#include <QFocusEvent>
#include <QLineEdit>
#include <QLocale>
#include <QString>
#include <QWidget>

namespace mlk {

// 数值输入框：纯文本编辑语义。
//
// 点击把光标放到点到的字符处，键入在光标处插入，方向键与 Backspace 按字符
// 移动/删除；数值在回车或失焦时解析、裁剪到 [lo, hi] 并按 decimals 重新
// 格式化。
//
// 不使用 QAbstractSpinBox：它把文本切成若干“数字段”，点击只选中光标所在的
// 段、键入替换整段、输入数字后还会跳到下一段。与看板的虚拟键盘（按键走
// QLineEdit::insert()）叠加后表现为“点哪都只能整段替换、改不了中间某一位”。
// 本机无物理键盘，数值编辑统一改用本控件。
class NumberEdit : public QLineEdit
{
public:
    explicit NumberEdit(QWidget *parent = nullptr)
        : QLineEdit(parent)
    {
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rebuildValidator();
        connect(this, &QLineEdit::editingFinished, this, &NumberEdit::commit);
    }

    void setRange(double lo, double hi)
    {
        m_lo = lo;
        m_hi = hi;
        rebuildValidator();
        setValue(m_value);
    }

    void setDecimals(int decimals)
    {
        m_decimals = qMax(0, decimals);
        rebuildValidator();
        setValue(m_value);
    }

    double value() const { return m_value; }

    void setValue(double value)
    {
        m_value = clamp(value);
        setText(format(m_value));
    }

    // 提交尚未回车的编辑内容；对话框读取 value() 之前调用。
    void commit()
    {
        const QString text = this->text().trimmed();
        if (!text.isEmpty()) {
            bool ok = false;
            const double parsed = QLocale::c().toDouble(text, &ok);
            if (ok)
                m_value = clamp(parsed);
        }
        setText(format(m_value));
    }

protected:
    void focusOutEvent(QFocusEvent *event) override
    {
        commit();
        QLineEdit::focusOutEvent(event);
    }

private:
    void rebuildValidator()
    {
        if (m_validator) {
            setValidator(nullptr);
            delete m_validator;
        }
        m_validator = new QDoubleValidator(m_lo, m_hi, m_decimals, this);
        m_validator->setNotation(QDoubleValidator::StandardNotation);
        m_validator->setLocale(QLocale::c());
        setValidator(m_validator);
    }

    double clamp(double value) const
    {
        return value < m_lo ? m_lo : (value > m_hi ? m_hi : value);
    }

    QString format(double value) const
    {
        return QString::number(value, 'f', m_decimals);
    }

    QDoubleValidator *m_validator = nullptr;
    double m_lo = -1e9;
    double m_hi = 1e9;
    double m_value = 0.0;
    int m_decimals = 3;
};

} // namespace mlk

#endif // MLK_NUMBEREDIT_H
