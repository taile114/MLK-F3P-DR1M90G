#pragma once
#include <QObject>
#include <QFile>
#include <QThread>
#include <QTimer>
#include "frame.h"
#include "inferencejournal.h"

namespace mlk {
/* 读取运行包里各回放窗口的实际寿命标签，用于面板显示预测偏差。优先
 * labels.csv（sample_index,rul_cycles），其次 labels_f32.bin（float32 LE，
 * 元素数须等于窗口数）。文件缺失或长度不符时返回空表。 */
QVector<float> loadRulLabels(const QString &root, int windowCount);

class ReplayWorker : public QObject {
    Q_OBJECT
public slots:
    void load(const QString &root, int backend);
    void predict(quint32 index);
    void unload();
    void record(const QByteArray &wire, const QString &directory, bool encrypt,
                const QString &password, int months, int minFreeMb);
signals:
    void loaded(quint32 count, const QString &model, const QString &dataset, const QString &error);
    void predicted(quint32 index, float rul, quint32 us, quint8 backend, float label, const QString &error);
    void released();
    void recordFailed(const QString &error);
private:
    QFile m_input;
    quint32 m_count = 0;
    QVector<float> m_labels;
    InferenceJournal m_journal;
};

class ReplayController : public QObject {
    Q_OBJECT
public:
    explicit ReplayController(QObject *parent = nullptr);
    ~ReplayController() override;
    QString control(quint8 action, quint8 backend, quint32 index, quint32 interval);
    bool setRoot(const QString &root);
    Frame snapshot() const { return m_status; }
signals:
    void changed(const mlk::Frame &frame);
    void resultReady(const mlk::Frame &frame);
    void loadRequested(const QString &root, int backend);
    void predictRequested(quint32 index);
    void unloadRequested();
    void recordRequested(const QByteArray &wire, const QString &directory, bool encrypt,
                         const QString &password, int months, int minFreeMb);
    void recordFailed(const QString &error);
private:
    void publish();
    void next();
    void release();
    QThread m_thread;
    ReplayWorker *m_worker;
    QTimer m_timer;
    QTimer m_heartbeat;
    Frame m_status;
    QString m_root;
    quint32 m_next = 0;
    int m_interval = 1000;
    bool m_busy = false;
    bool m_loaded = false;
    bool m_play = false;
    bool m_step = false;
    bool m_stopping = false;
    /* 偏差累计：一次回放会话内逐窗口求绝对相对偏差的平均值。 */
    double m_deviationSum = 0;
    quint32 m_deviationCount = 0;
};
}
