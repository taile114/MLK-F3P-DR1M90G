#pragma once

#include "../remote-pc/src/frame.h"
#include "../remote-pc/src/appconfig.h"
#include <QJsonArray>
#include <QMap>
#include <array>

namespace mlk {

// Every received sample is retained at its native channel cadence. tsUs is the
// first sample's estimated UTC time; no equal-rate padding is stored on disk.
Frame timestampWaveform(Frame frame, quint64 receivedUs = 0);
quint64 captureNowUs();

struct Capture {
    QString id;
    QString source;
    quint64 startUs = 0, endUs = 0; // half-open acquisition/receipt interval
    double k[4] = {0.1, 1, 0.0039, 0.1};
    double b[4] = {};
    QVector<Frame> blocks;
    qint64 sampleCount() const;
};

struct CaptureEntry {
    QString id, path;
    quint64 startUs = 0, endUs = 0;
};

struct CapturePoint { quint64 tsUs; double value; };
using CaptureSeries = std::array<QVector<CapturePoint>, 6>; // temp,current,speed,x,y,z
struct CaptureRow {
    quint64 tsUs = 0;
    std::array<double, 7> value;
    quint8 validMask = 0;
};

class CaptureStore {
public:
    CaptureStore(const QString &directory, const QString &password);
    bool save(const Capture &capture, QString *path = nullptr, QString *error = nullptr) const;
    bool load(const QString &path, Capture &capture, QString *error = nullptr) const;
    QList<CaptureEntry> list() const;
    QString pathForId(const QString &id) const;
    bool prune(int months, qint64 minFreeBytes, int maxFiles, qint64 incomingBytes,
               QString *error = nullptr) const;
    bool labels(QJsonArray &out, QString *error = nullptr) const;
    bool putLabel(const QJsonObject &label, QString *error = nullptr) const;
    static bool validLabel(const QJsonObject &label);
    static QJsonArray matchingLabels(const QJsonArray &labels, quint64 startUs, quint64 endUs,
                                    bool abnormalOnly = false);
    static CaptureSeries series(const Capture &capture);
    static QVector<CaptureRow> rows(const Capture &capture, quint64 stepUs = 20000);
    static QString csvHeader();
    static QString csvRow(const CaptureRow &row);
    bool exportCsv(const Capture &capture, const QString &output, quint64 stepUs = 20000,
                   QString *error = nullptr) const;
    QString directory() const { return m_directory; }
private:
    QString m_directory, m_password;
};

// Owns a bounded, lossless segment. A failed disk write leaves the segment intact.
class CaptureRecorder {
public:
    void begin(const AppConfig &config, const QString &source);
    bool append(const Frame &frame);
    bool full(const Frame &next) const;
    bool flush(QString *path = nullptr, QString *error = nullptr, CaptureEntry *entry = nullptr);
    bool empty() const { return m_capture.blocks.isEmpty(); }
    bool configMatches(const AppConfig &config) const;
private:
    Capture m_capture;
    AppConfig m_config;
    qint64 m_samples = 0;
};

// Delete capture_*.mlkc / fault_*.mlkf files. Names may be plain file names or
// paths inside directory; any other name or directory is rejected. Returns the
// number removed and appends rejected or failed paths to failed.
int removeCaptureFiles(const QString &directory, const QStringList &paths,
                       QStringList *failed = nullptr);

// Local socket / offline CLI share the same labeling contract. The socket never
// exports plaintext files on behalf of a client; export is an explicit CLI/UI action.
QJsonObject captureRequest(const AppConfig &config, const QJsonObject &request);
QJsonObject faultCaptureLabel(const QJsonObject &order);

} // namespace mlk
