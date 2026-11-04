#include "capturestore.h"
#include "crypto.h"
#include "faulttypes.h"
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace mlk {
namespace {
constexpr qint64 maxBytes = 64 * 1024 * 1024;
constexpr qint64 maxSamples = 2000000; // scalar values, including all XYZ axes
constexpr quint64 maxTime = 253402300799999999ULL;
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
bool safeId(const QString &id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(id).hasMatch();
}
/* 标签 note 上限 512 字节；按 UTF-8 字符边界截断，不切断多字节字符。 */
QString clippedUtf8(const QString &text, int maxBytes) {
    const auto utf8 = text.toUtf8();
    if (utf8.size() <= maxBytes) return text;
    int size = maxBytes;
    while (size > 0 && (uchar(utf8.at(size)) & 0xc0) == 0x80) --size;
    return QString::fromUtf8(utf8.constData(), size);
}
QByteArray keyFor(const QString &password, const QByteArray &salt) {
    QByteArray key(32, '\0'); const auto p = password.toUtf8();
    if (password.isEmpty() || salt.size() != 16 || PKCS5_PBKDF2_HMAC(p.constData(), p.size(),
        reinterpret_cast<const uchar *>(salt.constData()), 16, 100000, EVP_sha256(), 32,
        reinterpret_cast<uchar *>(key.data())) != 1) return {};
    return key;
}
bool writeEncrypted(const QString &path, const QByteArray &magic, const QByteArray &plain,
                    const QString &password, QString *error) {
    if (plain.size()>maxBytes-50) return fail(error,QStringLiteral("加密文件超出容量限制"));
    const auto salt = crypto::randomBytes(16), nonce = crypto::randomBytes(12);
    const auto key = keyFor(password, salt);
    if (salt.size() != 16 || nonce.size() != 12 || key.size() != 32)
        return fail(error, QStringLiteral("记录口令为空或密钥不可用"));
    const auto header = magic + QByteArray("\1\1", 2) + salt + nonce;
    const auto cipher = crypto::seal(key, nonce, header, plain);
    if (cipher.isEmpty()) return fail(error, QStringLiteral("记录加密失败"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto data = header + cipher;
    if (file.write(data) != data.size() || !file.commit()) return fail(error, file.errorString());
    return true;
}
bool readEncrypted(const QString &path, const QByteArray &magic, const QString &password,
                   QByteArray &plain, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    if (file.size() < 50 || file.size() > maxBytes) return fail(error, QStringLiteral("记录长度无效"));
    const auto data = file.readAll();
    if (data.left(4) != magic || data[4] != 1 || data[5] != 1)
        return fail(error, QStringLiteral("记录类型、版本或加密标志无效"));
    const auto key = keyFor(password, data.mid(6,16));
    if (key.isEmpty() || !crypto::open(key, data.mid(22,12), data.left(34), data.mid(34), plain))
        return fail(error, QStringLiteral("记录口令错误或文件认证失败"));
    return true;
}
bool validFrame(const Frame &f) {
    return f.ch < 4 && f.type == kTypeWaveform && f.tsUs > 0 && f.tsUs <= maxTime
        && f.rateHz > 0 && !f.samples.isEmpty() && f.samples.size() <= 65536
        && ((f.samplesY.isEmpty() && f.samplesZ.isEmpty()) || (f.ch == kVibration
            && f.samplesY.size() == f.samples.size() && f.samplesZ.size() == f.samples.size()));
}
qint64 sizeOf(const Frame &f) { return f.samples.size() + f.samplesY.size() + f.samplesZ.size(); }
quint64 timeValue(const QJsonObject &o, const char *key) {
    bool ok = false; const auto value = o.value(key).toString().toULongLong(&ok);
    return ok && value <= maxTime ? value : 0;
}
void configureStream(QDataStream &stream) {
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}
}

quint64 captureNowUs() { return quint64(QDateTime::currentMSecsSinceEpoch()) * 1000; }
Frame timestampWaveform(Frame f, quint64 receivedUs) {
    const auto span = f.rateHz && !f.samples.isEmpty()
        ? quint64(f.samples.size()-1) * 1000000 / f.rateHz : 0;
    // v4 timestamps were send-side UTC. v1/v2 used a monotonic clock and
    // therefore need a receive-time estimate when importing legacy packets.
    if (f.ver==4 && f.tsUs) f.tsUs=f.tsUs>span?f.tsUs-span:1;
    if (!f.tsUs || f.ver==1 || f.ver==2) {
        const auto now = receivedUs ? receivedUs : captureNowUs();
        f.tsUs = now > span ? now-span : 1;
    }
    f.ver=kWaveformVersion;
    return f;
}
qint64 Capture::sampleCount() const {
    qint64 n=0; for (const auto &f : blocks) n += sizeOf(f); return n;
}
CaptureStore::CaptureStore(const QString &directory, const QString &password)
    : m_directory(directory), m_password(password) {}

bool CaptureStore::save(const Capture &c, QString *path, QString *error) const {
    if (!safeId(c.id) || c.source.isEmpty() || c.source.toUtf8().size()>128 || !c.startUs || c.endUs <= c.startUs
        || c.endUs > maxTime || c.blocks.isEmpty() || c.sampleCount() > maxSamples
        || c.blocks.size() > 100000) return fail(error, QStringLiteral("采集记录元数据无效"));
    QByteArray plain; QDataStream ds(&plain, QIODevice::WriteOnly); configureStream(ds);
    QJsonArray scales;
    for (int ch=0; ch<4; ++ch) {
        if (!std::isfinite(c.k[ch]) || !std::isfinite(c.b[ch]))
            return fail(error, QStringLiteral("标定参数无效"));
        scales.append(QJsonObject{{"k",c.k[ch]},{"b",c.b[ch]}});
    }
    const QJsonObject meta{{"schema","mlk.capture.v2"},{"id",c.id},{"source",c.source},
        {"startUs",QString::number(c.startUs)},{"endUs",QString::number(c.endUs)},
        {"timebase","estimated_utc_first_sample"},{"channels",scales}};
    const auto json=QJsonDocument(meta).toJson(QJsonDocument::Compact);
    if(json.size()>8192) return fail(error,QStringLiteral("采集元数据过长"));
    ds << quint32(json.size()); ds.writeRawData(json.constData(),json.size());
    ds << quint32(c.blocks.size());
    for (const auto &f : c.blocks) {
        if (!validFrame(f)) return fail(error, QStringLiteral("采集样本块无效"));
        const quint8 axes=f.samplesY.isEmpty()?1:3;
        ds << f.ch << axes << f.rateHz << f.seq << f.cnt << f.tsUs << quint32(f.samples.size());
        const bool wide = f.ch == kCurrent || f.ch == kSpeed;
        for (const auto *axis : {&f.samples, &f.samplesY, &f.samplesZ}) {
            for (auto v : *axis) {
                if (wide) ds << qint32(v);
                else {
                    if (v < -32768 || v > 32767) return fail(error, QStringLiteral("采集原始码超出 signed16 范围"));
                    ds << qint16(v);
                }
            }
        }
    }
    if (ds.status()!=QDataStream::Ok || plain.size()>maxBytes-50)
        return fail(error, QStringLiteral("采集记录超出长度限制"));
    QDir dir(m_directory);
    if (!dir.mkpath(".")) return fail(error, QStringLiteral("无法创建采集目录"));
    const auto name=QStringLiteral("capture_%1_%2_%3.mlkc").arg(c.startUs).arg(c.endUs).arg(c.id);
    const auto output=dir.filePath(name);
    if (QFile::exists(output)) return fail(error, QStringLiteral("采集记录已存在"));
    if (!writeEncrypted(output,"MLKC",plain,m_password,error)) return false;
    if (path) *path=output;
    return true;
}

bool CaptureStore::load(const QString &path, Capture &capture, QString *error) const {
    QByteArray plain;
    if (!readEncrypted(path,"MLKC",m_password,plain,error)) return false;
    QDataStream ds(plain); configureStream(ds);
    quint32 length=0; ds >> length;
    if (length>8192 || length>quint32(plain.size()-4)) return fail(error,QStringLiteral("采集头长度无效"));
    QByteArray json(int(length),'\0'); ds.readRawData(json.data(),int(length));
    const auto meta=QJsonDocument::fromJson(json).object(); Capture c;
    c.id=meta.value("id").toString(); c.source=meta.value("source").toString();
    c.startUs=timeValue(meta,"startUs"); c.endUs=timeValue(meta,"endUs");
    const auto scales=meta.value("channels").toArray();
    const bool extended = meta.value("schema")=="mlk.capture.v2";
    if ((!extended && meta.value("schema")!="mlk.capture.v1") || !safeId(c.id) || c.source.isEmpty()
        || !c.startUs || c.endUs<=c.startUs || scales.size()!=4)
        return fail(error,QStringLiteral("采集元数据无效"));
    for (int i=0;i<4;++i) {
        const auto scale=scales[i].toObject();
        if (!scale.value("k").isDouble() || !scale.value("b").isDouble())
            return fail(error,QStringLiteral("采集标定无效"));
        c.k[i]=scale.value("k").toDouble(); c.b[i]=scale.value("b").toDouble();
    }
    quint32 count=0; ds >> count;
    if (!count || count>100000) return fail(error,QStringLiteral("采集块数无效"));
    qint64 total=0;
    for (quint32 block=0;block<count;++block) {
        Frame f; f.ver=kWaveformVersion; f.type=kTypeWaveform; quint8 axes=0; quint32 n=0;
        ds >> f.ch >> axes >> f.rateHz >> f.seq >> f.cnt >> f.tsUs >> n;
        total+=qint64(n)*axes;
        const bool wide = extended && (f.ch == kCurrent || f.ch == kSpeed);
        const int width = wide ? 4 : 2;
        if (!n || n>65536 || total>maxSamples || (axes!=1 && axes!=3)
            || qint64(n)*axes*width>ds.device()->bytesAvailable())
            return fail(error,QStringLiteral("采集样本长度无效"));
        f.samples.resize(int(n));
        if (axes==3) { f.samplesY.resize(int(n)); f.samplesZ.resize(int(n)); }
        for (auto *axis : {&f.samples, &f.samplesY, &f.samplesZ}) {
            for (auto &v : *axis) {
                if (wide) ds >> v;
                else { qint16 value = 0; ds >> value; v = value; }
            }
        }
        if (!validFrame(f)) return fail(error,QStringLiteral("采集样本字段无效"));
        c.blocks.append(f);
    }
    if (ds.status()!=QDataStream::Ok || !ds.atEnd()) return fail(error,QStringLiteral("采集载荷损坏"));
    capture=std::move(c); return true;
}
QList<CaptureEntry> CaptureStore::list() const {
    QList<CaptureEntry> out;
    static const QRegularExpression re(QStringLiteral("^capture_([0-9]+)_([0-9]+)_([A-Za-z0-9_-]{1,64})\\.mlkc$"));
    for (const auto &file:QDir(m_directory).entryInfoList({"capture_*.mlkc"},QDir::Files,QDir::Name|QDir::Reversed)) {
        const auto match=re.match(file.fileName()); if (!match.hasMatch()) continue;
        CaptureEntry entry; entry.startUs=match.captured(1).toULongLong(); entry.endUs=match.captured(2).toULongLong();
        entry.id=match.captured(3); entry.path=file.absoluteFilePath(); out.append(entry);
    }
    return out;
}
QString CaptureStore::pathForId(const QString &id) const {
    if (!safeId(id)) return {};
    for (const auto &entry:list()) if (entry.id==id) return entry.path;
    return {};
}
bool CaptureStore::prune(int months, qint64 minFreeBytes, int maxFiles, qint64 incoming,
                         QString *error) const {
    QDir dir(m_directory); if (!dir.exists() && !dir.mkpath(".")) return fail(error,QStringLiteral("无法创建采集目录"));
    auto files=dir.entryInfoList({"capture_*.mlkc"},QDir::Files,QDir::Time|QDir::Reversed);
    QStorageInfo storage(dir.absolutePath());
    while (!files.isEmpty()) {
        storage.refresh();
        const auto file=files.first();
        const bool expired=months>0 && file.lastModified().addMonths(months)<QDateTime::currentDateTime();
        const bool low=storage.isValid() && storage.bytesAvailable()<minFreeBytes+incoming;
        if (!expired && !low && (maxFiles<=0 || files.size()<maxFiles)) break;
        if (!QFile::remove(file.absoluteFilePath())) return fail(error,QStringLiteral("无法清理采集记录 %1").arg(file.fileName()));
        files.removeFirst();
    }
    storage.refresh();
    return !storage.isValid() || storage.bytesAvailable()>=minFreeBytes+incoming
        || fail(error,QStringLiteral("采集记录可用空间不足"));
}
bool CaptureStore::validLabel(const QJsonObject &o) {
    const auto start=timeValue(o,"startUs"), end=timeValue(o,"endUs");
    return o.value("api")==1 && safeId(o.value("id").toString()) && start>0 && end>start
        && o.value("abnormal").isBool() && o.value("label").isString()
        && !o.value("label").toString().trimmed().isEmpty()
        && o.value("label").toString().toUtf8().size()<=128
        && o.value("author").isString() && o.value("author").toString().toUtf8().size()<=128
        && o.value("note").isString() && o.value("note").toString().toUtf8().size()<=512;
}
bool CaptureStore::labels(QJsonArray &out, QString *error) const {
    out={}; const auto path=QDir(m_directory).filePath("capture_labels.mlkl");
    if (!QFile::exists(path)) return true;
    QByteArray plain;
    if (!readEncrypted(path,"MLKL",m_password,plain,error)) return false;
    const auto doc=QJsonDocument::fromJson(plain);
    if (!doc.isArray() || doc.array().size()>100000) return fail(error,QStringLiteral("标签库格式无效"));
    for (const auto &value:doc.array()) if (!value.isObject() || !validLabel(value.toObject()))
        return fail(error,QStringLiteral("标签字段无效"));
    out=doc.array(); return true;
}
bool CaptureStore::putLabel(const QJsonObject &label, QString *error) const {
    if (!validLabel(label)) return fail(error,QStringLiteral("标签字段无效"));
    if (!QDir().mkpath(m_directory)) return fail(error,QStringLiteral("无法创建标签目录"));
    QLockFile lock(QDir(m_directory).filePath("capture_labels.lock"));
    if (!lock.tryLock(1000)) return fail(error,QStringLiteral("标签库正在写入，请重试"));
    QJsonArray all; if (!labels(all,error)) return false;
    bool found=false;
    for (int i=0;i<all.size();++i) if (all[i].toObject().value("id")==label.value("id")) {
        all[i]=label; found=true; break;
    }
    if (!found) all.append(label);
    if (all.size()>100000 || !QDir().mkpath(m_directory)) return fail(error,QStringLiteral("标签库容量或目录无效"));
    return writeEncrypted(QDir(m_directory).filePath("capture_labels.mlkl"),"MLKL",
        QJsonDocument(all).toJson(QJsonDocument::Compact),m_password,error);
}
QJsonArray CaptureStore::matchingLabels(const QJsonArray &labels, quint64 start, quint64 end, bool abnormal) {
    QJsonArray out;
    for (const auto &value:labels) {
        const auto o=value.toObject();
        if (timeValue(o,"startUs")<end && timeValue(o,"endUs")>start
            && (!abnormal || o.value("abnormal").toBool())) out.append(o);
    }
    return out;
}
CaptureSeries CaptureStore::series(const Capture &c) {
    CaptureSeries result;
    const int slot[4]={0,1,3,2};
    for (const auto &f:c.blocks) {
        for (int i=0;i<f.samples.size();++i) {
            const auto ts=f.tsUs+quint64(i)*1000000/f.rateHz;
            const auto physical=[&](qint32 raw) { return double(raw)*c.k[f.ch]+c.b[f.ch]; };
            result[slot[f.ch]].append({ts,physical(f.samples[i])});
            if (!f.samplesY.isEmpty()) {
                result[4].append({ts,physical(f.samplesY[i])});
                result[5].append({ts,physical(f.samplesZ[i])});
            }
        }
    }
    for (auto &values:result) std::stable_sort(values.begin(),values.end(),
        [](const CapturePoint &a,const CapturePoint &b) { return a.tsUs<b.tsUs; });
    return result;
}
QVector<CaptureRow> CaptureStore::rows(const Capture &c, quint64 step) {
    QVector<CaptureRow> out;
    if (step<1000 || c.endUs<=c.startUs || (c.endUs-c.startUs+step-1)/step>1000000) return out;
    const auto values=series(c); std::array<int,6> index{};
    // Hold only preceding samples. Missing/stale channels remain NaN and have a
    // cleared mask bit, so offline sensors never become fabricated zeroes.
    const quint64 age[6]={6000000,2000000,2000000,500000,500000,500000};
    for (auto ts=c.startUs;ts<c.endUs;ts+=step) {
        CaptureRow row; row.tsUs=ts; row.value.fill(std::numeric_limits<double>::quiet_NaN());
        for (int ch=0;ch<6;++ch) {
            const auto &v=values[ch]; auto &i=index[ch];
            while (i<v.size() && v[i].tsUs<=ts) ++i;
            if (i>0 && ts-v[i-1].tsUs<=age[ch]) {
                row.value[ch]=v[i-1].value; row.validMask|=quint8(1<<ch);
            }
        }
        if ((row.validMask & 0x38)==0x38) {
            row.value[6]=std::sqrt(row.value[3]*row.value[3]+row.value[4]*row.value[4]+row.value[5]*row.value[5]);
            row.validMask|=0x40;
        }
        out.append(row);
    }
    return out;
}
QString CaptureStore::csvHeader() {
    return QStringLiteral("timestamp_us,temperature_c,current_ma,speed_rpm,vibration_x_g,vibration_y_g,vibration_z_g,vibration_magnitude_g,valid_mask");
}
QString CaptureStore::csvRow(const CaptureRow &row) {
    QStringList cells{QString::number(row.tsUs)};
    for (double value:row.value) cells.append(std::isfinite(value)?QString::number(value,'g',12):QString());
    cells.append(QString::number(row.validMask)); return cells.join(',');
}
bool CaptureStore::exportCsv(const Capture &c, const QString &output, quint64 step, QString *error) const {
    const auto data=rows(c,step);
    if (data.isEmpty()) return fail(error,QStringLiteral("导出间隔或记录区间无效（最多 1000000 行）"));
    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly)) return fail(error,file.errorString());
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    const auto header=(csvHeader()+'\n').toUtf8();
    if (file.write(header)!=header.size()) return fail(error,file.errorString());
    for (const auto &row:data) {
        const auto line=(csvRow(row)+'\n').toUtf8();
        if (file.write(line)!=line.size()) return fail(error,file.errorString());
    }
    return file.commit() || fail(error,file.errorString());
}
void CaptureRecorder::begin(const AppConfig &config, const QString &source) {
    m_config=config; m_capture=Capture{}; m_samples=0;
    m_capture.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_capture.source=source; m_capture.startUs=captureNowUs();
    for (int i=0;i<4;++i) { m_capture.k[i]=config.ch[i].k; m_capture.b[i]=config.ch[i].b; }
    // The data source already converts ADS1115 codes into mA.
    m_capture.k[kCurrent]=1; m_capture.b[kCurrent]=0;
    m_capture.k[kSpeed]=0.1; m_capture.b[kSpeed]=0;
}
bool CaptureRecorder::full(const Frame &f) const {
    return m_samples+sizeOf(f)>maxSamples || m_capture.blocks.size()>=100000;
}
bool CaptureRecorder::configMatches(const AppConfig &c) const {
    if (c.storageDir!=m_config.storageDir || c.passphrase!=m_config.passphrase
        || c.recordSegmentSec!=m_config.recordSegmentSec || c.retentionMonths!=m_config.retentionMonths
        || c.minFreeMb!=m_config.minFreeMb) return false;
    for (int i=0;i<4;++i) if(c.ch[i].k!=m_config.ch[i].k || c.ch[i].b!=m_config.ch[i].b) return false;
    return true;
}
bool CaptureRecorder::append(const Frame &frame) {
    const auto f=timestampWaveform(frame);
    if (!validFrame(f) || full(f)) return false;
    m_capture.startUs=qMin(m_capture.startUs,f.tsUs);
    m_capture.blocks.append(f); m_samples+=sizeOf(f); return true;
}
bool CaptureRecorder::flush(QString *path, QString *error, CaptureEntry *entry) {
    if (empty()) return true;
    m_capture.endUs=qMax(captureNowUs(),m_capture.startUs+1);
    for (const auto &f:m_capture.blocks)
        m_capture.endUs=qMax(m_capture.endUs,f.tsUs+quint64(f.samples.size()-1)*1000000/f.rateHz+1);
    CaptureStore store(m_config.storageDir,m_config.passphrase);
    const int maxFiles=m_config.retentionMonths>0
        ? int(qMin<qint64>(std::numeric_limits<int>::max(),qint64(m_config.retentionMonths)*30*86400/qMax(1,m_config.recordSegmentSec))) : 0;
    QString savedPath;
    if (!store.prune(m_config.retentionMonths,qint64(m_config.minFreeMb)*1024*1024,
        maxFiles,m_samples*2+m_capture.blocks.size()*28+8192,error) || !store.save(m_capture,&savedPath,error)) return false;
    if (path) *path = savedPath;
    if (entry) *entry = CaptureEntry{m_capture.id,savedPath,m_capture.startUs,m_capture.endUs};
    const auto source=m_capture.source; begin(m_config,source); return true;
}
QJsonObject faultCaptureLabel(const QJsonObject &order) {
    const auto ts=order.value("faultTimeUs").toString().toULongLong();
    const int classIndex=order.value("diagnosis").toObject().value("classIndex").toInt(kFaultUnavailable);
    // Model window labels describe the diagnosed interval, not a physical onset annotation.
    const bool specific=order.value("schema").toString()==QLatin1String(kFaultOrderSchema)
        && classIndex>=kFaultOverload && classIndex<=kFaultMountTilt;
    const auto label=specific
        ? QStringLiteral("%1_grade_%2").arg(faultName(classIndex)).arg(order.value("grade").toInt())
        : QStringLiteral("class_%1_grade_%2").arg(order.value("class").toInt()).arg(order.value("grade").toInt());
    const auto author=specific
        ? QStringLiteral("model:%1").arg(QString::fromLatin1(kFaultTask))
        : QStringLiteral("model");
    const auto diagnosis=order.value("diagnosis").toObject();
    const quint64 begin=diagnosis.value("startUs").toString().toULongLong();
    const quint64 end=diagnosis.value("endUs").toString().toULongLong();
    const bool window=specific && begin>0 && end>begin;
    QJsonObject result{{"api",1},{"id",order.value("id")},
        {"startUs",QString::number(window?begin:ts)},
        {"endUs",QString::number(window?end:ts+1)},{"abnormal",true},{"label",label},
        {"author",author},{"note",clippedUtf8(order.value("message").toString(),512)}};
    if (specific) {
        result.insert("scope",window?"model_window":"event");
        result.insert("eventUs",QString::number(ts));
        result.insert("classIndex",classIndex); result.insert("grade",order.value("grade"));
        result.insert("diagnosis",diagnosis);
    }
    return result;
}
int removeCaptureFiles(const QString &directory, const QStringList &paths, QStringList *failed)
{
    static const QRegularExpression re(
        QStringLiteral("^(?:capture|fault)_[A-Za-z0-9_.-]{1,160}\\.(?:mlkc|mlkf)$"));
    const QDir dir(directory);
    const QString root = dir.exists() ? dir.canonicalPath() : dir.absolutePath();
    int removed = 0;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        const QString target = info.isAbsolute() ? info.absoluteFilePath()
                                                 : dir.filePath(path);
        const QFileInfo resolved(target);
        const bool inside = resolved.absolutePath() == root || resolved.absoluteFilePath().startsWith(root + QLatin1Char('/'));
        if (!inside || !re.match(resolved.fileName()).hasMatch()) {
            if (failed) failed->append(resolved.fileName().isEmpty() ? path : resolved.fileName());
            continue;
        }
        if (QFile::remove(resolved.absoluteFilePath())) ++removed;
        else if (failed) failed->append(resolved.fileName());
    }
    return removed;
}

QJsonObject captureRequest(const AppConfig &cfg, const QJsonObject &request) {    CaptureStore store(cfg.storageDir,cfg.passphrase); QString error; QJsonArray all;
    auto bad=[](const QString &text) { return QJsonObject{{"ok",false},{"error",text}}; };
    if (request.value("api")!=1) return bad(QStringLiteral("unsupported API"));
    const auto op=request.value("op").toString();
    if (op=="label") {
        if (!store.putLabel(request.value("label").toObject(),&error)) return bad(error);
        return {{"ok",true}};
    }
    if (!store.labels(all,&error)) return bad(error);
    if (op=="labels") return {{"ok",true},{"labels",all}};
    if (op!="list") return bad(QStringLiteral("unsupported operation"));
    const int offset=request.value("offset").toInt(0), limit=request.value("limit").toInt(100);
    if (offset<0 || limit<1 || limit>1000) return bad(QStringLiteral("invalid pagination"));
    QJsonArray records; int total=0;
    for (const auto &entry:store.list()) {
        const auto labels=CaptureStore::matchingLabels(all,entry.startUs,entry.endUs,true);
        if (request.value("abnormalOnly").toBool() && labels.isEmpty()) continue;
        if (total>=offset && records.size()<limit) records.append(QJsonObject{{"id",entry.id},
            {"startUs",QString::number(entry.startUs)},{"endUs",QString::number(entry.endUs)},
            {"abnormal",!labels.isEmpty()},{"labels",labels}});
        ++total;
    }
    return {{"ok",true},{"records",records},{"total",total}};
}
} // namespace mlk
