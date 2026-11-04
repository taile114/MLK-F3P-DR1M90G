#pragma once

#include <QList>
#include <QPoint>
#include <QPointer>
#include <QRegion>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QGridLayout;
class QPushButton;

class VirtualKeyboard : public QWidget
{
    Q_OBJECT
public:
    explicit VirtualKeyboard(QWidget *owner);
    ~VirtualKeyboard() override;
    void hideKeyboard();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    enum Action {
        Text,
        Backspace,
        Delete,
        Enter,
        Tab,
        Escape,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        ShiftKey,
        CapsKey,
        CtrlKey,
        AltKey,
        WinKey,
        NumKey,
        FKey,
        Noop
    };

    struct KeySpec {
        QString label;
        QString normal;
        QString shifted;
        Action action;
        int key;
        int row;
        int col;
        int rowSpan;
        int colSpan;
    };

    void build();
    QPushButton *addKey(const KeySpec &spec);
    void handleKey(const KeySpec &spec);
    void sendKey(int key, const QString &text = QString(),
                 Qt::KeyboardModifiers mods = Qt::NoModifier);
    void typeText(const QString &text);
    void showFor(QWidget *target);
    void clearTarget();
    void refreshExposedBackground(const QRegion &globalRegion);
    bool isTextInput(QWidget *w) const;
    QLineEdit *targetLineEdit() const;
    void updateMods();

    QPointer<QWidget> m_target;
    QPointer<QWidget> m_targetWindow;
    QPointer<QWidget> m_suppressedTarget;
    QMetaObject::Connection m_targetDestroyed;
    QWidget *m_title = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_modLabel = nullptr;
    QGridLayout *m_grid = nullptr;
    QList<QPushButton *> m_letterButtons;
    QTimer m_dragTimer;
    QTimer m_moveTimer;
    bool m_dragging = false;
    bool m_userMoved = false;
    bool m_presenting = false;
    bool m_framebufferCompositor = false;
    QPoint m_dragStartPos;
    QPoint m_dragStartGlobal;
    QPoint m_pendingDragPos;
    bool m_shift = false;
    bool m_caps = false;
    bool m_ctrl = false;
    bool m_alt = false;
    bool m_win = false;
    bool m_num = false;
};
