#pragma once
#include "capturestore.h"
#include <QDialog>

class QListWidget;
class QComboBox;
class QLabel;
class QSlider;
class QDoubleSpinBox;
class QSpinBox;
class QTableView;
class QTimer;
class QPushButton;
namespace mlk {
class CapturePlot;
class CaptureTable;
class CaptureDialog : public QDialog {
    Q_OBJECT
public:
    explicit CaptureDialog(const AppConfig &config, QWidget *parent = nullptr);
    void configure(const AppConfig &config);
    void refresh();
    void openOrder(const QString &id);
    void openCapture(const QString &path);
    void openLegacy(const QString &path);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    void clearRecord();
    void selectItem();
    void loadSelected();
    void render();
    void exportCsv();
    void labelSelection();
    void message(const QString &text);
    void updateCaptureDetails();
    AppConfig m_config;
    Capture m_capture;
    CaptureSeries m_series;
    QList<CaptureEntry> m_entries;
    QJsonArray m_labels;
    QString m_openItemPath, m_loadedCapturePath;
    bool m_loaded = false;
    quint64 m_eventUs = 0;
    QListWidget *m_list;
    QComboBox *m_filter, *m_related;
    QLabel *m_info, *m_status;
    CapturePlot *m_plots[4];
    CaptureTable *m_tableModel;
    QTableView *m_table;
    QSlider *m_position;
    QDoubleSpinBox *m_span;
    QSpinBox *m_step;
    QPushButton *m_play, *m_export, *m_label;
    QTimer *m_timer;
};
}
