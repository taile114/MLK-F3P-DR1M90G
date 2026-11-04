#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mlk {

struct FaultMeta
{
    quint64 tsUs = 0;
    quint16 rateHz = 0;
    quint8 chCount = 4;
    quint8 cls = 0;
    quint8 grade = 0;
    quint16 conf = 0;
    quint32 rul = 0;
    quint32 n = 0;
    double k[4] = { 1, 1, 1, 1 };
    double b[4] = { 0, 0, 0, 0 };
};

class FaultStore
{
public:
    FaultStore(const QString &dir, bool encrypt, const QString &passphrase,
               int retentionMonths = 0, qint64 minFreeBytes = 0, int maxFiles = 0);

    bool save(const FaultMeta &meta, const QVector<qint16> ch[4], QString *err = nullptr) const;
    int prune(QString *err = nullptr) const;
    int count() const;
    qint64 totalBytes() const;
    QStringList list() const;
    bool load(const QString &path, FaultMeta &meta, QVector<qint16> ch[4],
              QString *err = nullptr) const;

private:
    QString m_dir;
    bool m_encrypt;
    QString m_pass;
    int m_retentionMonths;
    qint64 m_minFreeBytes;
    int m_maxFiles;
};

} // namespace mlk
