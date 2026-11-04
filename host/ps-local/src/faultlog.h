#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/* 每窗故障诊断日志：模型/预处理/质量/耗时逐窗加密落盘，不生成工单，也不把
 * 预测当作监督真值。文件按天滚动，沿用记录口令与保留策略。 */
namespace mlk {

class FaultJournal
{
public:
    /* 追加一条窗口记录。目录为 <storageDir>/fault，文件 fault_YYYYMMDD.mlkf。
     * 每条记录独立封装（文件头带盐，记录带独立 nonce），因此追加不需要重写
     * 整个文件。 */
    bool append(const QJsonObject &record, const QString &directory, const QString &password,
                int retentionMonths, qint64 minFreeMb, QString *error);

    static bool read(const QString &path, const QString &password, QJsonArray &records,
                     QString *error);
    static int prune(const QString &directory, int retentionMonths, qint64 minFreeMb,
                     QString *error);

private:
    QString m_path;
    QByteArray m_salt;
    QByteArray m_key;
    QString m_password;
};

} // namespace mlk
