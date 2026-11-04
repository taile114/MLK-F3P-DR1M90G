#include <QtTest>
#include <QtWidgets>
#include "virtualkeyboard.h"
#include "numberedit.h"

namespace {
QPushButton *key(VirtualKeyboard &keyboard,const QString &text) {
    for (auto *button:keyboard.findChildren<QPushButton *>()) if(button->text()==text) return button;
    return nullptr;
}
void clickNative(QWidget *widget) {
    if (!widget) { QTest::qFail("Expected virtual keyboard key is missing",__FILE__,__LINE__);return; }
    auto *window=widget->window();
    QTest::mouseClick(window->windowHandle(),Qt::LeftButton,Qt::NoModifier,
        widget->mapTo(window,widget->rect().center()));
    QCoreApplication::processEvents();
}
void dragMouse(QWidget *title,QEvent::Type type,const QPoint &global) {
    const auto button=type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton;
    const auto buttons=type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton;
    QMouseEvent event(type,title->mapFromGlobal(global),global,button,buttons,Qt::NoModifier);
    QApplication::sendEvent(title,&event);
}
}

class KeyboardTest:public QObject {
    Q_OBJECT
private slots:
    void passwordAndModalButtons_data() {
        QTest::addColumn<int>("modality");
        QTest::newRow("window-modal")<<int(Qt::WindowModal);
        QTest::newRow("application-modal")<<int(Qt::ApplicationModal);
    }
    void passwordAndModalButtons() {
        QFETCH(int,modality);
        QWidget main;main.resize(400,200);main.show();
        VirtualKeyboard keyboard(nullptr);
        QDialog dialog(&main);dialog.setWindowModality(Qt::WindowModality(modality));
        auto *layout=new QVBoxLayout(&dialog);auto *edit=new QLineEdit(&dialog);
        edit->setEchoMode(QLineEdit::Password);layout->addWidget(edit);
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
        layout->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        QSignalSpy accepted(&dialog,&QDialog::accepted),rejected(&dialog,&QDialog::rejected);
        dialog.show();dialog.activateWindow();edit->setFocus();
        QTRY_VERIFY(keyboard.isVisible());
        QCOMPARE(keyboard.windowHandle()->transientParent(),dialog.windowHandle());
        QVERIFY(key(keyboard,"1"));clickNative(key(keyboard,"1"));
        QCOMPARE(edit->text(),QStringLiteral("1"));
        clickNative(key(keyboard,QStringLiteral("收起")));QTRY_VERIFY(!keyboard.isVisible());
        clickNative(buttons->button(QDialogButtonBox::Cancel));QCOMPARE(rejected.count(),1);
        dialog.show();dialog.activateWindow();edit->setFocus();
        QTRY_VERIFY(keyboard.isVisible());
        clickNative(key(keyboard,"2"));QCOMPARE(edit->text(),QStringLiteral("12"));
        // A keyboard window must not prevent the input dialog's buttons from receiving clicks.
        clickNative(buttons->button(QDialogButtonBox::Ok));QCOMPARE(accepted.count(),1);
    }
    void targetSurvivesWindowFocusChange() {
        QWidget main;auto *layout=new QVBoxLayout(&main);auto *edit=new QLineEdit(&main);layout->addWidget(edit);
        VirtualKeyboard keyboard(nullptr);main.show();main.activateWindow();edit->setFocus();
        QTRY_VERIFY(keyboard.isVisible());
        QApplication::setActiveWindow(&keyboard);QCoreApplication::processEvents();
        // Some framebuffer backends activate the raised tool window despite NoFocus.
        key(keyboard,"7")->click();QCOMPARE(edit->text(),QStringLiteral("7"));
        key(keyboard,"Backspace")->click();QCOMPARE(edit->text(),QString());
    }
    void hideDoesNotReopenOnFocusRestore() {
        QWidget main;auto *layout=new QVBoxLayout(&main);auto *edit=new QLineEdit(&main);layout->addWidget(edit);
        VirtualKeyboard keyboard(nullptr);main.show();main.activateWindow();edit->setFocus();
        QTRY_VERIFY(keyboard.isVisible());keyboard.hideKeyboard();
        QFocusEvent restored(QEvent::FocusIn,Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(edit,&restored);QCoreApplication::processEvents();
        QVERIFY(!keyboard.isVisible());
        clickNative(edit);QTRY_VERIFY(keyboard.isVisible());
    }
    void numberEditAndDialogLifetime() {
        QWidget main;main.show();VirtualKeyboard keyboard(nullptr);
        {
            QDialog dialog(&main);dialog.setWindowModality(Qt::WindowModal);
            auto *layout=new QVBoxLayout(&dialog);auto *number=new mlk::NumberEdit(&dialog);
            number->setRange(0,65535);number->setDecimals(0);number->setValue(8000);layout->addWidget(number);
            dialog.show();dialog.activateWindow();number->setFocus();number->selectAll();
            QTRY_VERIFY(keyboard.isVisible());
            clickNative(key(keyboard,"9"));clickNative(key(keyboard,"0"));
            QCOMPARE(number->text(),QStringLiteral("90"));
            key(keyboard,"Enter")->click();QCOMPARE(number->value(),90.0);
            dialog.hide();
        }
        QCoreApplication::processEvents();QVERIFY(!keyboard.isVisible());
        // Destroying the last editor must not leave a dangling target.
        key(keyboard,"1")->click();
    }
    void cursorRetargetAndModifierReset() {
        QWidget main;auto *layout=new QVBoxLayout(&main);
        auto *first=new QLineEdit("12345",&main);auto *second=new QLineEdit(&main);
        layout->addWidget(first);layout->addWidget(second);
        VirtualKeyboard keyboard(nullptr);main.show();main.activateWindow();first->setFocus();first->setCursorPosition(2);
        QTRY_VERIFY(keyboard.isVisible());
        clickNative(key(keyboard,"7"));QCOMPARE(first->text(),QStringLiteral("127345"));
        main.activateWindow();second->setFocus();QCoreApplication::processEvents();
        clickNative(key(keyboard,"Caps"));clickNative(key(keyboard,"A"));
        clickNative(key(keyboard,"Shift"));clickNative(key(keyboard,"b"));
        clickNative(key(keyboard,"."));QCOMPARE(second->text(),QStringLiteral("Ab."));
        QCOMPARE(first->text(),QStringLiteral("127345"));
        clickNative(key(keyboard,QStringLiteral("收起")));QVERIFY(!keyboard.isVisible());
        main.activateWindow();clickNative(second);QTRY_VERIFY(keyboard.isVisible());
        QVERIFY(key(keyboard,"a"));clickNative(key(keyboard,"a"));
        QVERIFY(second->text().contains('a'));
    }
    void longPressDragAndHide() {
        QWidget main;auto *layout=new QVBoxLayout(&main);auto *edit=new QLineEdit(&main);layout->addWidget(edit);
        VirtualKeyboard keyboard(nullptr);main.show();main.activateWindow();edit->setFocus();
        QTRY_VERIFY(keyboard.isVisible());
        auto *title=keyboard.findChild<QWidget *>(QStringLiteral("vkTitle"));QVERIFY(title);
        const QPoint start=keyboard.pos();
        QPoint press=title->mapToGlobal(QPoint(20,15));
        dragMouse(title,QEvent::MouseButtonPress,press);
        dragMouse(title,QEvent::MouseMove,press+QPoint(-20,-20));
        dragMouse(title,QEvent::MouseButtonRelease,press+QPoint(-20,-20));
        QTest::qWait(40);QCOMPARE(keyboard.pos(),start);
        dragMouse(title,QEvent::MouseButtonPress,press);QTest::qWait(550);
        for (int i=1;i<=10;++i) dragMouse(title,QEvent::MouseMove,press+QPoint(-i*5,-i*4));
        QTRY_COMPARE(keyboard.pos(),start+QPoint(-50,-40));
        dragMouse(title,QEvent::MouseButtonRelease,press+QPoint(-80,-60));
        QCOMPARE(keyboard.pos(),start+QPoint(-80,-60));
        clickNative(key(keyboard,"3"));QCOMPARE(edit->text(),QStringLiteral("3"));
        press=title->mapToGlobal(QPoint(20,15));
        dragMouse(title,QEvent::MouseButtonPress,press);QTest::qWait(550);
        dragMouse(title,QEvent::MouseMove,press+QPoint(100,100));
        const QPoint beforeHide=keyboard.pos();keyboard.hideKeyboard();QTest::qWait(80);
        QCOMPARE(keyboard.pos(),beforeHide);QVERIFY(!keyboard.isVisible());
    }
    void dragRestoresFramebuffer_data() {
        QTest::addColumn<bool>("directMoves");
        QTest::newRow("mouse-burst")<<false;
        QTest::newRow("window-move-burst")<<true;
    }
    void dragRestoresFramebuffer() {
        if (QGuiApplication::platformName()!=QStringLiteral("vnc"))
            QSKIP("Run with the vnc platform to inspect the framebuffer compositor pixels");
        QFETCH(bool,directMoves);
        auto *screen=QGuiApplication::primaryScreen();
        QWidget main;main.setGeometry(screen->availableGeometry());
        main.setAutoFillBackground(true);
        QPalette palette=main.palette();palette.setColor(QPalette::Window,QColor("#7196b0"));main.setPalette(palette);
        QLineEdit edit(&main);edit.setGeometry(10,10,180,30);
        VirtualKeyboard keyboard(nullptr);main.show();edit.setFocus();
        QTRY_VERIFY(keyboard.isVisible());keyboard.hideKeyboard();QTest::qWait(50);
        const QImage background=screen->grabWindow(0).toImage();QVERIFY(!background.isNull());
        clickNative(&edit);QTRY_VERIFY(keyboard.isVisible());QTest::qWait(50);
        const QRect oldRect=keyboard.geometry();
        const auto screenPoint=[screen](QPoint point) { return point-screen->geometry().topLeft(); };
        QVERIFY(screen->grabWindow(0).toImage().pixelColor(screenPoint(oldRect.center()))
            !=background.pixelColor(screenPoint(oldRect.center())));
        auto *title=keyboard.findChild<QWidget *>(QStringLiteral("vkTitle"));QVERIFY(title);
        const QPoint press=title->mapToGlobal(QPoint(20,15));
        dragMouse(title,QEvent::MouseButtonPress,press);QTest::qWait(550);
        // Deliver a burst without processing paints between moves: fbconvenience
        // retains only the last old geometry unless every exposed area is dirtied.
        const QPoint end=screen->geometry().topLeft()+QPoint(60,90);
        const QPoint delta=end-oldRect.topLeft();
        for (int step=1;step<=6;++step) {
            if (directMoves) keyboard.move(oldRect.topLeft()+delta*step/6);
            else dragMouse(title,QEvent::MouseMove,press+delta*step/6);
        }
        dragMouse(title,QEvent::MouseButtonRelease,press+delta);
        QTRY_COMPARE(keyboard.pos(),end);QTest::qWait(80);
        const QImage moved=screen->grabWindow(0).toImage();
        const QRegion exposed=QRegion(oldRect).subtracted(QRegion(keyboard.geometry()));
        int differences=0;
        for (const QRect &rect:exposed)
            for (int y=rect.top();y<=rect.bottom();++y)
                for (int x=rect.left();x<=rect.right();++x) {
                    const QPoint point=screenPoint(QPoint(x,y));
                    differences+=moved.pixel(point)!=background.pixel(point);
                }
        QCOMPARE(differences,0);
        keyboard.hideKeyboard();QTest::qWait(50);
        const QImage hidden=screen->grabWindow(0).toImage();
        QCOMPARE(hidden.pixelColor(screenPoint(end+QPoint(100,100))),
            background.pixelColor(screenPoint(end+QPoint(100,100))));
    }
};
QTEST_MAIN(KeyboardTest)
#include "test_keyboard.moc"
