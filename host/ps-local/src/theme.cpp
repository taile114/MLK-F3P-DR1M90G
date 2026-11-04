#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

void applyTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    p.setColor(QPalette::Window, QColor(0x0f, 0x14, 0x1a));
    p.setColor(QPalette::WindowText, QColor(0xc9, 0xd1, 0xd9));
    p.setColor(QPalette::Base, QColor(0x0b, 0x10, 0x15));
    p.setColor(QPalette::AlternateBase, QColor(0x16, 0x1d, 0x26));
    p.setColor(QPalette::ToolTipBase, QColor(0x16, 0x1d, 0x26));
    p.setColor(QPalette::ToolTipText, QColor(0xc9, 0xd1, 0xd9));
    p.setColor(QPalette::Text, QColor(0xc9, 0xd1, 0xd9));
    p.setColor(QPalette::Button, QColor(0x16, 0x1d, 0x26));
    p.setColor(QPalette::ButtonText, QColor(0xc9, 0xd1, 0xd9));
    p.setColor(QPalette::BrightText, QColor(0xff, 0x7b, 0x72));
    p.setColor(QPalette::Highlight, QColor(0x1f, 0x6f, 0xeb));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6e, 0x76, 0x81));
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: #0f141a; }
        QWidget {
            color: #c9d1d9;
            font-family: "Maple Mono NF CN", "Noto Sans CJK SC", "DejaVu Sans";
            font-size: 13px;
        }
        QGroupBox {
            background: #161d26;
            border: 1px solid #263241;
            border-radius: 8px;
            margin-top: 18px;
            padding: 10px 8px 8px 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 8px;
            color: #8fb3d9;
            font-weight: bold;
        }
        QLabel { background: transparent; }
        QLabel#sensorUnit { color: #8b949e; }
        QLabel#sysDetail { color: #8b949e; font-size: 11px; }
        QLabel#boardTime { color: #8b949e; }
        QLabel#sourceText { color: #8b949e; }
        QPushButton {
            background: #21262d;
            border: 1px solid #30363d;
            border-radius: 6px;
            padding: 6px 14px;
        }
        QPushButton:hover { background: #30363d; }
        QPushButton:pressed { background: #1f6feb; }
        QPushButton:checked { background: #238636; border-color: #2ea043; }
        QProgressBar {
            background: #0b1015;
            border: 1px solid #263241;
            border-radius: 6px;
            text-align: center;
            min-height: 18px;
        }
        QProgressBar::chunk { border-radius: 5px; background: #2ea043; }
        QProgressBar#cpuBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2ea043, stop:1 #3fb950);
        }
        QProgressBar#memBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1f6feb, stop:1 #58a6ff);
        }
        QProgressBar#diskBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #d29922, stop:1 #e3b341);
        }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: #0b1015;
            border: 1px solid #30363d;
            border-radius: 4px;
            padding: 4px;
            selection-background-color: #1f6feb;
        }
        QComboBox QAbstractItemView { background: #161d26; selection-background-color: #1f6feb; }
        QCheckBox { spacing: 6px; }
        QScrollBar:vertical { background: #0b1015; width: 10px; }
        QScrollBar::handle:vertical { background: #30363d; border-radius: 5px; min-height: 24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QStatusBar { background: #0b1015; color: #8b949e; }
    )"));
}
