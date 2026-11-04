#pragma once
#include <QByteArray>
#include <QString>
#include <QElapsedTimer>
#include <QMap>
#include "../remote-pc/src/frame.h"
namespace mlk {
// Each record contains the exact v3 frame, including float RUL, unit and source.
class InferenceJournal {
public:
    bool save(const Frame &frame, const QString &directory, bool encrypt, const QString &password,
              int retentionMonths, int minFreeMb, QString *error);
    static bool load(const QString &path, const QString &password, Frame &frame, QString *error);
private:
    QByteArray m_key;
    QByteArray m_salt;
    QString m_password;
    QString m_indexDirectory;
    QMultiMap<qint64, QString> m_records;
    QElapsedTimer m_indexAge;
};
}
