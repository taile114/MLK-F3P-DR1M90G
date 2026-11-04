#include "virtualkeyboard.h"

#include <QtWidgets>
#include <QGuiApplication>
#include <QScreen>

VirtualKeyboard::VirtualKeyboard(QWidget *owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                         | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("virtualKeyboard"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QStringLiteral(
        "QWidget#virtualKeyboard { background: #11161b; border: 1px solid #2a333d; }"
        "QFrame#vkTitle { background: #1b232b; border-bottom: 1px solid #2a333d; }"
        "QLabel#vkTitleText { color: #9aa6b2; font-size: 11px; }"
        "QLabel#vkMods { color: #6e7681; font-size: 10px; }"
        "QPushButton { background: #222b34; color: #d8dee6; border: 1px solid #3a4652;"
        "  border-radius: 4px; padding: 1px; }"
        "QPushButton:pressed { background: #3a4a5a; }"));
    build();
    m_dragTimer.setSingleShot(true);
    m_dragTimer.setInterval(500);
    connect(&m_dragTimer, &QTimer::timeout, this, [this] { m_dragging = true; });
    m_moveTimer.setSingleShot(true);
    m_moveTimer.setInterval(33);
    connect(&m_moveTimer, &QTimer::timeout, this, [this] {
        if (m_dragging && isVisible()) move(m_pendingDragPos);
    });
    const auto platform=QGuiApplication::platformName();
    m_framebufferCompositor=platform==QStringLiteral("linuxfb") || platform==QStringLiteral("vnc");
    qApp->installEventFilter(this);
    hide();
}

VirtualKeyboard::~VirtualKeyboard()
{
    qApp->removeEventFilter(this);
}

void VirtualKeyboard::build()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    m_title = new QFrame(this);
    m_title->setObjectName(QStringLiteral("vkTitle"));
    m_title->setFixedHeight(30);
    m_title->setCursor(Qt::SizeAllCursor);
    auto *titleRow = new QHBoxLayout(m_title);
    titleRow->setContentsMargins(8, 0, 6, 0);
    m_titleLabel = new QLabel(QStringLiteral("长按顶部拖动 · 虚拟键盘"), m_title);
    m_titleLabel->setObjectName(QStringLiteral("vkTitleText"));
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_modLabel = new QLabel(m_title);
    m_modLabel->setObjectName(QStringLiteral("vkMods"));
    m_modLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto *close = new QPushButton(QStringLiteral("收起"), m_title);
    close->setObjectName(QStringLiteral("virtualKeyboardHide"));
    close->setFocusPolicy(Qt::NoFocus);
    close->setFixedSize(64, 24);
    connect(close, &QPushButton::clicked, this, &VirtualKeyboard::hideKeyboard);
    titleRow->addWidget(m_titleLabel);
    titleRow->addStretch(1);
    titleRow->addWidget(m_modLabel);
    titleRow->addSpacing(8);
    titleRow->addWidget(close);
    m_title->installEventFilter(this);
    root->addWidget(m_title);

    m_grid = new QGridLayout;
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(3);
    root->addLayout(m_grid, 1);

    auto text = [this](int r, int c, const QString &label, const QString &normal,
                       const QString &shifted = QString(), int rs = 1, int cs = 1) {
        addKey({ label, normal, shifted.isEmpty() ? normal : shifted, Text, 0, r, c, rs, cs });
    };
    auto special = [this](int r, int c, const QString &label, Action action, int key = 0,
                          int rs = 1, int cs = 1) {
        addKey({ label, QString(), QString(), action, key, r, c, rs, cs });
    };
    auto letter = [&](int r, int c, char ch) {
        const QString lower = QString(QChar(ch)).toLower();
        auto *b = addKey({ lower, lower, lower.toUpper(), Text, 0, r, c, 1, 1 });
        b->setProperty("baseChar", lower);
        m_letterButtons.append(b);
    };

    const char *digits = "1234567890";
    for (int i = 0; i < 10; ++i)
        text(0, i, QString(QChar(digits[i])), QString(QChar(digits[i])),
             QString(QChar(digits[i])));
    special(0, 10, QStringLiteral("Backspace"), Backspace, 0, 1, 2);

    const char *qwerty = "QWERTYUIOP";
    for (int i = 0; i < 10; ++i)
        letter(1, i, qwerty[i]);

    const char *asdf = "ASDFGHJKL";
    for (int i = 0; i < 9; ++i)
        letter(2, i, asdf[i]);

    special(3, 0, QStringLiteral("Shift"), ShiftKey, 0, 1, 2);
    const char *zxcv = "ZXCVBNM";
    for (int i = 0; i < 7; ++i)
        letter(3, 2 + i, zxcv[i]);
    special(3, 9, QStringLiteral("Caps"), CapsKey, 0, 1, 2);

    text(4, 0, QStringLiteral("Space"), QStringLiteral(" "), QStringLiteral(" "), 1, 6);
    special(4, 6, QStringLiteral("Enter"), Enter, 0, 1, 2);

    const char *symbols[] = { ".", "/", ":", "-", "_", "@", "*", "+", "=", ",", ";", "#" };
    for (int i = 0; i < 12; ++i) {
        const QString sym = QString::fromLatin1(symbols[i]);
        text(5, i, sym, sym, sym);
    }

    resize(760, 330);
    setMinimumSize(700, 300);
    updateMods();
}

QPushButton *VirtualKeyboard::addKey(const KeySpec &spec)
{
    auto *b = new QPushButton(spec.label, this);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAutoDefault(false);
    b->setDefault(false);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    b->setMinimumSize(28, 32);
    QFont f = b->font();
    f.setPointSize(spec.label.size() > 5 ? 7 : 8);
    b->setFont(f);
    m_grid->addWidget(b, spec.row, spec.col, spec.rowSpan, spec.colSpan);
    connect(b, &QPushButton::clicked, this, [this, spec] { handleKey(spec); });
    return b;
}

void VirtualKeyboard::handleKey(const KeySpec &spec)
{
    if (!isVisible()) return;
    switch (spec.action) {
    case Text: {
        QString value;
        if (spec.normal.size() == 1 && spec.normal.at(0).isLetter())
            value = (m_caps ^ m_shift) ? spec.normal.toUpper() : spec.normal.toLower();
        else
            value = (m_shift && !spec.shifted.isEmpty()) ? spec.shifted : spec.normal;
        if (m_ctrl && value.size() == 1 && value.at(0).isLetter()) {
            sendKey(value.toUpper().at(0).unicode(), value, Qt::ControlModifier);
            m_ctrl = false;
            updateMods();
        } else {
            typeText(value);
        }
        if (m_shift) {
            m_shift = false;
            updateMods();
        }
        return;
    }
    case Backspace: sendKey(Qt::Key_Backspace); return;
    case Delete: sendKey(Qt::Key_Delete); return;
    case Enter: sendKey(Qt::Key_Return, QStringLiteral("\r")); return;
    case Tab: sendKey(Qt::Key_Tab, QStringLiteral("\t")); return;
    case Escape: hideKeyboard(); return;
    case Left: sendKey(Qt::Key_Left); return;
    case Right: sendKey(Qt::Key_Right); return;
    case Up: sendKey(Qt::Key_Up); return;
    case Down: sendKey(Qt::Key_Down); return;
    case Home: sendKey(Qt::Key_Home); return;
    case End: sendKey(Qt::Key_End); return;
    case PageUp: sendKey(Qt::Key_PageUp); return;
    case PageDown: sendKey(Qt::Key_PageDown); return;
    case ShiftKey: m_shift = !m_shift; updateMods(); return;
    case CapsKey: m_caps = !m_caps; updateMods(); return;
    case CtrlKey: m_ctrl = !m_ctrl; updateMods(); return;
    case AltKey: m_alt = !m_alt; updateMods(); return;
    case WinKey: m_win = !m_win; updateMods(); return;
    case NumKey: m_num = !m_num; updateMods(); return;
    case FKey: sendKey(spec.key); return;
    case Noop: return;
    }
}

void VirtualKeyboard::typeText(const QString &text)
{
    QLineEdit *le = targetLineEdit();
    if (le)
        le->insert(text);
}

void VirtualKeyboard::sendKey(int key, const QString &text, Qt::KeyboardModifiers mods)
{
    QPointer<QWidget> w = targetLineEdit();
    if (!w)
        return;
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    if (w) QApplication::sendEvent(w, &release);
}

void VirtualKeyboard::showFor(QWidget *target)
{
    if (!target || !target->isVisible() || !isTextInput(target)) return;
    auto *host = target->window();
    // Do not revive an editor behind a newer modal dialog (e.g. a file picker).
    if (auto *modal=QApplication::activeModalWidget())
        if (modal!=host && !modal->isAncestorOf(target)) return;
    if (m_target != target) {
        disconnect(m_targetDestroyed);
        m_target=target;
        m_targetDestroyed=connect(target,&QObject::destroyed,this,[this] { clearTarget(); });
    }
    m_targetWindow=host;
    // Keep QObject ownership independent of stack-allocated dialogs, while
    // making the tool window part of the active dialog's modal hierarchy.
    m_presenting=true;
    host->winId();
    winId();
    if (windowHandle()->transientParent()!=host->windowHandle())
        windowHandle()->setTransientParent(host->windowHandle());
    if (!m_userMoved) {
        QScreen *s = QGuiApplication::primaryScreen();
        if (s) {
            const QRect g = s->availableGeometry();
            move(g.center().x() - width() / 2, g.bottom() - height() - 12);
        }
    }
    show();
    raise();
    m_presenting=false;
}

void VirtualKeyboard::hideKeyboard()
{
    // Hiding a tool window can restore editor focus. Suppress that FocusIn
    // until an explicit editor click, rather than immediately reopening.
    m_suppressedTarget=m_target;
    m_dragTimer.stop();
    m_moveTimer.stop();
    m_dragging = false;
    m_shift = m_caps = m_ctrl = m_alt = m_win = m_num = false;
    updateMods();
    hide();
}

void VirtualKeyboard::clearTarget()
{
    disconnect(m_targetDestroyed);
    m_target.clear();
    m_targetWindow.clear();
    hideKeyboard();
    m_suppressedTarget.clear();
}

void VirtualKeyboard::refreshExposedBackground(const QRegion &globalRegion)
{
    if (!m_framebufferCompositor || globalRegion.isEmpty()) return;
    // Qt 5.13 fbconvenience keeps only one old geometry until backing-store
    // flush. Several moves before that flush can leave earlier positions on
    // screen. Queue every exposed region on the underlying windows so that
    // their backing-store flush also invalidates those compositor pixels.
    for (auto *window:QApplication::topLevelWidgets()) {
        if (window==this || !window->isVisible() || window->isMinimized()) continue;
        const QRegion local=globalRegion.translated(-window->mapToGlobal(QPoint()))
            .intersected(window->rect());
        if (!local.isEmpty()) window->update(local);
    }
}

void VirtualKeyboard::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    if (isVisible())
        refreshExposedBackground(QRegion(QRect(e->oldPos(),size())).subtracted(geometry()));
}

void VirtualKeyboard::hideEvent(QHideEvent *e)
{
    m_dragTimer.stop();
    m_moveTimer.stop();
    m_dragging=false;
    QWidget::hideEvent(e);
    refreshExposedBackground(geometry());
}

bool VirtualKeyboard::isTextInput(QWidget *w) const
{
    if (!w || !w->isEnabled())
        return false;
    if (auto *le = qobject_cast<QLineEdit *>(w))
        return !le->isReadOnly();
    if (qobject_cast<QAbstractSpinBox *>(w))
        return true;
    if (auto *cb = qobject_cast<QComboBox *>(w))
        return cb->isEditable();
    return false;
}

QLineEdit *VirtualKeyboard::targetLineEdit() const
{
    QWidget *w = m_target.data();
    if (!w || !w->isVisible() || !isTextInput(w))
        return nullptr;
    if (auto *modal=QApplication::activeModalWidget())
        if (modal!=w->window() && !modal->isAncestorOf(w)) return nullptr;
    if (auto *le = qobject_cast<QLineEdit *>(w))
        return le;
    if (auto *sb = qobject_cast<QAbstractSpinBox *>(w))
        return sb->findChild<QLineEdit *>();
    if (auto *cb = qobject_cast<QComboBox *>(w))
        return cb->lineEdit();
    return nullptr;
}

void VirtualKeyboard::updateMods()
{
    const bool upper = m_caps ^ m_shift;
    for (auto *b : m_letterButtons) {
        const QString base = b->property("baseChar").toString();
        if (!base.isEmpty())
            b->setText(upper ? base.toUpper() : base.toLower());
    }
    m_modLabel->setText(QStringLiteral("Shift %1 · Caps %2")
                            .arg(m_shift ? QStringLiteral("开") : QStringLiteral("关"))
                            .arg(m_caps ? QStringLiteral("开") : QStringLiteral("关")));
}

bool VirtualKeyboard::eventFilter(QObject *obj, QEvent *e)
{
    if (obj==m_target && e->type()==QEvent::FocusOut) {
        const auto reason=static_cast<QFocusEvent *>(e)->reason();
        if ((reason==Qt::ActiveWindowFocusReason || reason==Qt::PopupFocusReason)
            && (m_presenting || QApplication::activeWindow()==this
                || (windowHandle() && QGuiApplication::focusWindow()==windowHandle()))) {
            // Raising the keyboard is not an editor commit. In particular,
            // NumberEdit::focusOutEvent must not reformat text/reset selection.
            return true;
        }
    }
    if (obj==m_targetWindow && (e->type()==QEvent::Hide || e->type()==QEvent::Close
        || e->type()==QEvent::WindowBlocked)) clearTarget();

    if (obj == m_title) {
        if (e->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
            auto *me = static_cast<QMouseEvent *>(e);
            m_moveTimer.stop();
            m_dragging=false;
            m_dragStartPos = pos();
            m_dragStartGlobal = me->globalPos();
            m_dragTimer.start();
            return true;
        }
        if (e->type() == QEvent::MouseMove && m_dragging
            && (static_cast<QMouseEvent *>(e)->buttons() & Qt::LeftButton)) {
            auto *me = static_cast<QMouseEvent *>(e);
            m_pendingDragPos=m_dragStartPos + (me->globalPos() - m_dragStartGlobal);
            if (!m_moveTimer.isActive()) m_moveTimer.start();
            m_userMoved = true;
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(e)->button()==Qt::LeftButton) {
            m_dragTimer.stop();
            m_moveTimer.stop();
            if (m_dragging) {
                const auto *me=static_cast<QMouseEvent *>(e);
                const QPoint finalPos=m_dragStartPos+(me->globalPos()-m_dragStartGlobal);
                move(finalPos);
                if (finalPos!=m_dragStartPos) m_userMoved=true;
            }
            m_dragging = false;
            return true;
        }
    }

    if (e->type() == QEvent::FocusIn || e->type() == QEvent::MouseButtonPress) {
        if (auto *w = qobject_cast<QWidget *>(obj)) {
            if (isTextInput(w) && !isAncestorOf(w)) {
                if (e->type()==QEvent::MouseButtonPress) {
                    m_suppressedTarget.clear();
                    showFor(w);
                } else if (w!=m_suppressedTarget) {
                    // Let the dialog complete activation before raising the
                    // keyboard, so the new tool window cannot steal FocusIn.
                    QPointer<QWidget> editor=w;
                    QTimer::singleShot(0,this,[this,editor] {
                        if (editor && editor!=m_suppressedTarget) showFor(editor);
                    });
                }
            }
        }
    }
    return QWidget::eventFilter(obj, e);
}
