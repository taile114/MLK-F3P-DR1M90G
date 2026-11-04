#include "faultpreprocess.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPair>
#include <algorithm>
#include <cmath>
#include <vector>

namespace mlk {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTfChannels = 8;
constexpr int kTfColumns = 48;
constexpr int kTfCells = 47;
constexpr int kStats = 12;
constexpr int kFastCells = 40;
constexpr int kSlowCells = 60;
constexpr int kSlowReal = 4;
constexpr double kFastCellUs = 100000.0;
constexpr double kSlowCellUs = 1000000.0;
constexpr double kFastMaxAgeMs = 500.0;
constexpr double kSlowMaxAgeMs = 500.0;
constexpr double kSlowTemperatureMaxAgeMs = 6000.0;
constexpr double kSlowMinSpanMs = 200.0;
constexpr double kPsdFloor = 1e-10;
constexpr double kGravityTolerance = 0.25;

bool fail(QString *error, const QString &text)
{
    if (error) *error = text;
    return false;
}

int nextPow2(int n)
{
    int value = 1;
    while (value < n) value <<= 1;
    return value;
}

std::vector<double> hannWindow(int n)
{
    std::vector<double> w(static_cast<size_t>(n));
    if (n <= 1) {
        if (n == 1) w[0] = 1.0;
        return w;
    }
    for (int i = 0; i < n; ++i)
        w[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n - 1));
    return w;
}

void fft(std::vector<double> &re, std::vector<double> &im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / double(len);
        const double wlenRe = std::cos(angle), wlenIm = std::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            double wr = 1.0, wi = 0.0;
            for (size_t j = 0; j < len / 2; ++j) {
                const double ur = re[i + j], ui = im[i + j];
                const double vr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
                const double vi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr;
                im[i + j + len / 2] = ui - vi;
                const double nextWr = wr * wlenRe - wi * wlenIm;
                wi = wr * wlenIm + wi * wlenRe;
                wr = nextWr;
            }
        }
    }
}

double median(QVector<double> values)
{
    if (values.isEmpty()) return 0.0;
    std::sort(values.begin(), values.end());
    const int n = values.size();
    return n % 2 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

/* 频点线性插值：目标网格落在源频率范围内才取插值，其余记 0。 */
QVector<double> resample(const QVector<double> &freq, const QVector<double> &power,
                         const QVector<double> &grid)
{
    QVector<double> out(grid.size(), 0.0);
    const int n = freq.size();
    if (n < 2 || power.size() != n) return out;
    int cursor = 0;
    for (int i = 0; i < grid.size(); ++i) {
        const double f = grid[i];
        if (f < freq.first() || f > freq.last()) continue;
        while (cursor + 2 < n && freq[cursor + 1] < f) ++cursor;
        const double f0 = freq[cursor], f1 = freq[cursor + 1];
        const double span = f1 - f0;
        const double t = span > 0 ? (f - f0) / span : 0.0;
        out[i] = power[cursor] + (power[cursor + 1] - power[cursor]) * qBound(0.0, t, 1.0);
    }
    return out;
}

QVector<double> linearGrid(int count, double maximum)
{
    QVector<double> grid(count);
    for (int i = 0; i < count; ++i)
        grid[i] = maximum * double(i) / double(count - 1);
    return grid;
}

} // namespace

QJsonObject FaultQuality::toJson() const
{
    return {{QStringLiteral("blocks"), blocks},
            {QStringLiteral("gapBlocks"), gapBlocks},
            {QStringLiteral("tfRatio"), tfRatio},
            {QStringLiteral("fastRatio"), fastRatio},
            {QStringLiteral("slowRatio"), slowRatio},
            {QStringLiteral("poseAgeMs"), poseAgeMs},
            {QStringLiteral("coverage"), coverage},
            {QStringLiteral("gravityOk"), gravityOk},
            {QStringLiteral("temperatureC"), temperatureValid ? QJsonValue(temperatureC) : QJsonValue()},
            {QStringLiteral("overtemperatureMasked"), overtemperatureMasked}};
}

void FaultPreprocessor::blockSpectrum(const QVector<float> &samples, double fs,
                                      QVector<double> &freqHz, QVector<double> &power)
{
    freqHz.clear();
    power.clear();
    const int n = samples.size();
    if (n < 32 || !(fs > 0.0)) return;
    const int nfft = std::max(512, nextPow2(n));
    const int half = nfft / 2;
    std::vector<double> re(size_t(nfft), 0.0), im(size_t(nfft), 0.0);
    double mean = 0.0;
    for (float v : samples) mean += double(v);
    mean /= double(n);
    const auto window = hannWindow(n);
    double windowSq = 0.0;
    for (double w : window) windowSq += w * w;
    if (!(windowSq > 0.0)) return;
    for (int i = 0; i < n; ++i)
        re[size_t(i)] = (double(samples[i]) - mean) * window[size_t(i)];
    fft(re, im);
    freqHz.resize(half + 1);
    power.resize(half + 1);
    for (int k = 0; k <= half; ++k) {
        const double magnitude = re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)];
        double density = magnitude / (fs * windowSq);
        if (k > 0 && k < half) density *= 2.0;   /* 单边谱内部频点乘二 */
        freqHz[k] = double(k) * fs / double(nfft);
        power[k] = density;
    }
}

bool FaultPreprocessor::readArray(const QJsonObject &norm, const char *key, int expected,
                                  QVector<double> &out, QString *error) const
{
    const auto value = norm.value(QLatin1String(key));
    if (!value.isArray() || value.toArray().size() != expected)
        return fail(error, QStringLiteral("preprocess.json 的 %1 长度不符").arg(QLatin1String(key)));
    QVector<double> parsed(expected);
    const auto array = value.toArray();
    for (int i = 0; i < expected; ++i) {
        if (!array[i].isDouble()) return fail(error, QStringLiteral("preprocess.json 数值无效"));
        parsed[i] = array[i].toDouble();
    }
    out = parsed;
    return true;
}

bool FaultPreprocessor::loadAssets(const QString &root, QString *error)
{
    m_loaded = false;
    const QString path = QDir(root).filePath(QStringLiteral("preprocess.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("缺少 preprocess.json：%1").arg(path));
    if (file.size() <= 0 || file.size() > 4 * 1024 * 1024) {
        file.close();
        return fail(error, QStringLiteral("preprocess.json 长度无效"));
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(error, QStringLiteral("preprocess.json 无法解析"));
    const auto o = doc.object();
    if (o.value(QStringLiteral("schema")).toString() != QStringLiteral("mlk.fault.preprocess.v1")
        || o.value(QStringLiteral("preprocess")).toString() != QLatin1String(kFaultPreprocess))
        return fail(error, QStringLiteral("预处理资产身份不符"));

    const auto frequencies = o.value(QStringLiteral("frequencies")).toObject();
    m_bands = frequencies.value(QStringLiteral("bands")).toInt(64);
    m_tfFreqs = frequencies.value(QStringLiteral("tf")).toInt(193);
    m_psdFreqs = frequencies.value(QStringLiteral("psd")).toInt(769);
    m_maxHz = frequencies.value(QStringLiteral("maxHz")).toDouble(600.0);
    if (m_bands <= 0 || m_tfFreqs < 2 || m_psdFreqs < 2 || !(m_maxHz > 0.0))
        return fail(error, QStringLiteral("预处理频率网格无效"));

    const auto bands = o.value(QStringLiteral("bands")).toObject();
    const auto weights = bands.value(QStringLiteral("weights")).toArray();
    if (weights.size() != m_bands) return fail(error, QStringLiteral("频带权重数量不符"));
    m_bandWeights.resize(m_bands * m_tfFreqs);
    for (int b = 0; b < m_bands; ++b) {
        const auto row = weights[b].toArray();
        if (row.size() != m_tfFreqs) return fail(error, QStringLiteral("频带权重长度不符"));
        for (int f = 0; f < m_tfFreqs; ++f) {
            if (!row[f].isDouble()) return fail(error, QStringLiteral("频带权重无效"));
            m_bandWeights[b * m_tfFreqs + f] = row[f].toDouble();
        }
    }

    const auto norm = o.value(QStringLiteral("norm")).toObject();
    const auto tf = norm.value(QStringLiteral("tf")).toObject();
    const auto psd = norm.value(QStringLiteral("psd")).toObject();
    const auto stats = norm.value(QStringLiteral("stats")).toObject();
    const auto poseNorm = norm.value(QStringLiteral("pose")).toObject();
    const auto fastNorm = norm.value(QStringLiteral("fast")).toObject();
    const auto slowNorm = norm.value(QStringLiteral("slow")).toObject();
    if (!readArray(tf, "mean", kTfChannels * m_bands, m_tfMean, error)
        || !readArray(tf, "std", kTfChannels * m_bands, m_tfStd, error)
        || !readArray(psd, "mean", 3 * m_psdFreqs, m_psdMean, error)
        || !readArray(psd, "std", 3 * m_psdFreqs, m_psdStd, error)
        || !readArray(stats, "mean", kStats, m_statsMean, error)
        || !readArray(stats, "std", kStats, m_statsStd, error)
        || !readArray(poseNorm, "mean", 5, m_poseMean, error)
        || !readArray(poseNorm, "std", 5, m_poseStd, error)
        || !readArray(fastNorm, "mean", 6, m_fastMean, error)
        || !readArray(fastNorm, "std", 6, m_fastStd, error)
        || !readArray(slowNorm, "mean", 9, m_slowMean, error)
        || !readArray(slowNorm, "std", 9, m_slowStd, error))
        return false;

    const auto reference = o.value(QStringLiteral("pose")).toObject()
                               .value(QStringLiteral("reference")).toArray();
    if (reference.size() != 3) return fail(error, QStringLiteral("水平参考无效"));
    for (int i = 0; i < 3; ++i) m_reference[i] = reference[i].toDouble();

    const auto quality = o.value(QStringLiteral("quality")).toObject();
    m_minBlocks = quality.value(QStringLiteral("minBlocks")).toInt(12);
    m_minBlockSamples = quality.value(QStringLiteral("minBlockSamples")).toInt(64);
    m_minVibrationRateHz = quality.value(QStringLiteral("minVibrationRateHz")).toInt(1600);
    m_tfMinRatio = quality.value(QStringLiteral("tfRatio")).toDouble(0.45);
    m_fastMinRatio = quality.value(QStringLiteral("fastRatio")).toDouble(0.75);
    m_slowMinRatio = quality.value(QStringLiteral("slowRatio")).toDouble(0.60);
    m_poseMaxAgeMs = quality.value(QStringLiteral("poseAgeMs")).toDouble(1500.0);
    m_loaded = true;
    return true;
}

double FaultPreprocessor::standardize(const QVector<double> &scale, const QVector<double> &shift,
                                      int index, double value) const
{
    const double mean = index < shift.size() ? shift[index] : 0.0;
    const double stddev = index < scale.size() ? scale[index] : 1.0;
    return (value - mean) / (stddev != 0.0 ? stddev : 1.0);
}

bool FaultPreprocessor::usableVibration(const NativeBlock &block) const
{
    return block.dataValid && block.axes == 3 && block.count >= quint32(m_minBlockSamples)
        && block.rateHz >= quint32(m_minVibrationRateHz)
        && block.values.size() == int(block.count * block.axes);
}

void FaultPreprocessor::buildTimeFrequency(const FaultWindowInput &window, QVector<double> &values,
                                           FaultQuality &quality) const
{
    const int stride = m_bands * kTfColumns;
    values.fill(0.0, kTfChannels * stride);
    QVector<double> sum(3 * stride, 0.0), weight(3 * kTfCells, 0.0);
    const auto grid = linearGrid(m_tfFreqs, m_maxHz);
    const double cellUs = double(kFaultWindowUs) / double(kTfCells);

    for (const auto &block : window.vibration) {
        if (!usableVibration(block))
            continue;
        const double fs = double(block.rateHz);
        const double seconds = block.seconds();
        const double center = double(block.t0Us + (block.endUs() + 1 - block.t0Us) / 2);
        const double relative = (center - double(window.startUs)) / cellUs;
        const int cell = qBound(0, int(qBound(0.0, relative, double(kTfCells - 1))),
                                kTfCells - 1);
        for (int axis = 0; axis < 3; ++axis) {
            QVector<float> samples(int(block.count));
            for (quint32 i = 0; i < block.count; ++i)
                samples[int(i)] = block.values[int(i * 3 + quint32(axis))];
            QVector<double> freq, power;
            blockSpectrum(samples, fs, freq, power);
            const auto spectrum = resample(freq, power, grid);
            for (int band = 0; band < m_bands; ++band) {
                double energy = 0.0;
                const int base = band * m_tfFreqs;
                for (int f = 0; f < m_tfFreqs; ++f)
                    energy += m_bandWeights[base + f] * spectrum[f];
                sum[(axis * m_bands + band) * kTfColumns + cell] += double(float(energy)) * seconds;
            }
            weight[axis * kTfCells + cell] += seconds;
        }
    }

    int used = 0;
    for (int axis = 0; axis < 3; ++axis)
        for (int cell = 0; cell < kTfCells; ++cell)
            if (weight[axis * kTfCells + cell] > 0.0) ++used;
    quality.tfRatio = double(used) / double(3 * kTfCells);

    for (int axis = 0; axis < 3; ++axis) {
        for (int band = 0; band < m_bands; ++band) {
            const double mean = m_tfMean[axis * m_bands + band];
            const double stddev = m_tfStd[axis * m_bands + band];
            for (int cell = 0; cell < kTfCells; ++cell) {
                const int index = (axis * m_bands + band) * kTfColumns + cell;
                const double total = weight[axis * kTfCells + cell];
                values[axis * stride + band * kTfColumns + cell] = total > 0.0
                    ? (std::log10(sum[index] / total + kPsdFloor) - mean)
                        / (stddev != 0.0 ? stddev : 1.0) : 0.0;
            }
        }
    }
    /* 第 48 列与五个空通道保持零：它们只是固定布局的占位。 */
}

void FaultPreprocessor::buildSpectrum(const FaultWindowInput &window, QVector<double> &values) const
{
    const auto grid = linearGrid(m_psdFreqs, m_maxHz);
    values.fill(0.0, 3 * m_psdFreqs);
    for (int axis = 0; axis < 3; ++axis) {
        QVector<double> accumulated(m_psdFreqs, 0.0);
        double total = 0.0;
        for (const auto &block : window.vibration) {
            if (!usableVibration(block))
                continue;
            QVector<float> samples(int(block.count));
            for (quint32 i = 0; i < block.count; ++i)
                samples[int(i)] = block.values[int(i * 3 + quint32(axis))];
            QVector<double> freq, power;
            blockSpectrum(samples, double(block.rateHz), freq, power);
            const auto spectrum = resample(freq, power, grid);
            const double weight = block.seconds();
            for (int f = 0; f < m_psdFreqs; ++f) accumulated[f] += double(float(spectrum[f])) * weight;
            total += weight;
        }
        for (int f = 0; f < m_psdFreqs; ++f) {
            const double mean = total > 0.0 ? accumulated[f] / total : 0.0;
            const double level = std::log10(mean + kPsdFloor);
            values[axis * m_psdFreqs + f] =
                standardize(m_psdStd, m_psdMean, axis * m_psdFreqs + f, level);
        }
    }
}

void FaultPreprocessor::buildStatistics(const FaultWindowInput &window, QVector<double> &values) const
{
    values.fill(0.0, kStats);
    for (int axis = 0; axis < 3; ++axis) {
        double count = 0.0, variance = 0.0, fourth = 0.0;
        double low = 0.0, high = 0.0, peak = 0.0;
        bool first = true;
        for (const auto &block : window.vibration) {
            if (!usableVibration(block)) continue;
            double blockMean = 0.0;
            for (quint32 i = 0; i < block.count; ++i)
                blockMean += block.values[int(i * 3 + quint32(axis))];
            blockMean /= block.count;
            for (quint32 i = 0; i < block.count; ++i) {
                const double v = double(block.values[int(i * 3 + quint32(axis))]) - blockMean;
                if (!std::isfinite(v)) continue;
                variance += v * v;
                fourth += v * v * v * v;
                if (first) { low = high = v; peak = std::abs(v); first = false; }
                low = qMin(low, v); high = qMax(high, v); peak = qMax(peak, std::abs(v));
                ++count;
            }
        }
        if (count <= 0.0) {
            for (int s = 0; s < 4; ++s)
                values[axis * 4 + s] = 0.0;
            continue;
        }
        variance /= count;
        fourth /= count;
        const double rms = std::sqrt(variance);
        const double crest = peak / std::max(1e-10, rms);
        const double kurt = fourth / std::max(1e-10, variance * variance);
        const double physical[4] = {rms, high - low, crest, kurt};
        for (int s = 0; s < 4; ++s)
            values[axis * 4 + s] =
                standardize(m_statsStd, m_statsMean, axis * 4 + s, physical[s]);
    }
}

void FaultPreprocessor::buildPose(const FaultWindowInput &window, QVector<double> &values,
                                  FaultQuality &quality) const
{
    values.fill(0.0, 8);
    if (window.vibration.isEmpty()) return;
    QVector<QVector<double>> means(3);
    QVector<QPair<quint64, quint64>> intervals;
    quint64 lastEnd = 0;
    for (const auto &block : window.vibration) {
        if (!usableVibration(block)) continue;
        for (int axis = 0; axis < 3; ++axis) {
            double sum = 0.0;
            for (quint32 i = 0; i < block.count; ++i)
                sum += block.values[int(i * 3 + quint32(axis))];
            means[axis].append(sum / double(block.count));
        }
        intervals.append(qMakePair(block.t0Us, block.endUs() + 1));
        lastEnd = qMax(lastEnd, block.endUs() + 1);
    }
    if (means[0].isEmpty()) return;
    const double gravity[3] = {median(means[0]), median(means[1]), median(means[2])};
    const double referenceNorm = std::sqrt(m_reference[0] * m_reference[0]
        + m_reference[1] * m_reference[1] + m_reference[2] * m_reference[2]);
    const double norm = std::sqrt(gravity[0] * gravity[0] + gravity[1] * gravity[1]
        + gravity[2] * gravity[2]);
    double tilt = 0.0;
    if (norm > 0.0 && referenceNorm > 0.0) {
        const double dot = gravity[0] * m_reference[0] + gravity[1] * m_reference[1]
            + gravity[2] * m_reference[2];
        const double cross[3] = {m_reference[1] * gravity[2] - m_reference[2] * gravity[1],
            m_reference[2] * gravity[0] - m_reference[0] * gravity[2],
            m_reference[0] * gravity[1] - m_reference[1] * gravity[0]};
        tilt = std::atan2(std::sqrt(cross[0]*cross[0] + cross[1]*cross[1] + cross[2]*cross[2]), dot) * 180.0 / kPi;
    }
    /* 覆盖率按真实时间区间并集计算：重叠块不重复增加覆盖率。 */
    std::sort(intervals.begin(), intervals.end());
    quint64 covered = 0;
    quint64 runStart = 0, runEnd = 0;
    bool open = false;
    for (const auto &interval : intervals) {
        const quint64 begin = qMax(interval.first, window.startUs);
        const quint64 end = qMin(interval.second, window.endUs);
        if (end <= begin) continue;
        if (!open) { runStart = begin; runEnd = end; open = true; continue; }
        if (begin <= runEnd) {
            runEnd = qMax(runEnd, end);
        } else {
            covered += runEnd - runStart;
            runStart = begin;
            runEnd = end;
        }
    }
    if (open) covered += runEnd - runStart;
    const double span = double(window.endUs - window.startUs);
    quality.coverage = span > 0.0 ? qBound(0.0, double(covered) / span, 1.0) : 0.0;
    quality.poseAgeMs = lastEnd && window.endUs > lastEnd
        ? double(window.endUs - lastEnd) / 1000.0 : 0.0;
    quality.gravityOk = std::abs(norm - 1.0) <= kGravityTolerance;
    quality.blocks = 0;
    for (const auto &block : window.vibration)
        if (usableVibration(block))
            ++quality.blocks;

    const double physical[5] = {gravity[0], gravity[1], gravity[2], tilt, norm};
    for (int i = 0; i < 5; ++i)
        values[i] = standardize(m_poseStd, m_poseMean, i, physical[i]);
    values[5] = quality.coverage;
    values[6] = 1.0;   /* 有效位：至少一个完整块参与 */
    /* 年龄按训练侧语义归一化到 [0,1]：age_s / pose_max_age_s，超限截断。 */
    values[7] = m_poseMaxAgeMs > 0.0
        ? qBound(0.0, quality.poseAgeMs / m_poseMaxAgeMs, 1.0) : 1.0;
}

void FaultPreprocessor::buildFastState(const FaultWindowInput &window, QVector<double> &values,
                                       FaultQuality &quality) const
{
    values.fill(0.0, 6 * kFastCells);
    for (int cell = 0; cell < kFastCells; ++cell) {
        values[4 * kFastCells + cell] = 1.0;
        values[5 * kFastCells + cell] = 1.0;
    }
    int validCurrent = 0, validSpeed = 0;
    auto lastSample = [](const QVector<NativeBlock> &blocks, quint64 edgeUs, quint64 *t0Us) {
        double value = 0.0;
        bool found = false;
        for (const auto &block : blocks) {
            if (!block.dataValid || block.count == 0 || block.t0Us > edgeUs) continue;
            /* 单点块取块内最后样本的时间；多采样块按采样间隔外推末样本时刻。 */
            const quint64 last = block.endUs();
            if (last >= edgeUs) continue;
            if (!found || last >= *t0Us) {
                *t0Us = last;
                value = block.values.last();
                found = true;
            }
        }
        return found ? qMakePair(true, value) : qMakePair(false, 0.0);
    };
    for (int cell = 0; cell < kFastCells; ++cell) {
        const quint64 edge = window.startUs + quint64(cell * kFastCellUs);
        quint64 seenUs = 0;
        const auto current = lastSample(window.current, edge, &seenUs);
        if (current.first && double(edge - seenUs) / 1000.0 <= kFastMaxAgeMs) {
            values[0 * kFastCells + cell] =
                standardize(m_fastStd, m_fastMean, 0, current.second);
            values[2 * kFastCells + cell] = 1.0;
            /* 年龄与训练侧一致归一化到 [0,1]，不写毫秒原值。 */
            values[4 * kFastCells + cell] =
                qBound(0.0, double(edge - seenUs) / 1000.0 / kFastMaxAgeMs, 1.0);
            ++validCurrent;
        }
        const auto speed = lastSample(window.speed, edge, &seenUs);
        if (speed.first && double(edge - seenUs) / 1000.0 <= kFastMaxAgeMs) {
            values[1 * kFastCells + cell] =
                standardize(m_fastStd, m_fastMean, 1, speed.second);
            values[3 * kFastCells + cell] = 1.0;
            values[5 * kFastCells + cell] =
                qBound(0.0, double(edge - seenUs) / 1000.0 / kFastMaxAgeMs, 1.0);
            ++validSpeed;
        }
    }
    const double ratioCurrent = double(validCurrent) / kFastCells;
    const double ratioSpeed = double(validSpeed) / kFastCells;
    quality.fastRatio = qMin(ratioCurrent, ratioSpeed);
}

void FaultPreprocessor::buildSlowState(const FaultWindowInput &window, QVector<double> &values,
                                       FaultQuality &quality) const
{
    values.fill(0.0, 9 * kSlowCells);
    /* 前 56 格不是当前短历史的观测范围：物理值与有效位为零，年龄为 1。 */
    for (int cell = 0; cell < kSlowCells - kSlowReal; ++cell) {
        values[6 * kSlowCells + cell] = 1.0;
        values[7 * kSlowCells + cell] = 1.0;
        values[8 * kSlowCells + cell] = 1.0;
    }
    int validTemperature = 0, validCurrent = 0, validSpeed = 0;
    for (int k = 0; k < kSlowReal; ++k) {
        const int cell = kSlowCells - kSlowReal + k;
        const quint64 cellEnd = window.startUs + quint64((k + 1) * kSlowCellUs);
        const quint64 cellStart = cellEnd - quint64(kSlowCellUs);

        /* 温度：取网格以前最近一次观测，最大年龄 6 s。 */
        bool haveTemperature = false;
        quint64 temperatureAt = 0;
        double temperature = 0.0;
        for (const auto &block : window.temperature) {
            if (!block.dataValid || block.count == 0) continue;
            const quint64 last = block.endUs();
            if (last >= cellEnd) continue;
            if (!haveTemperature || last >= temperatureAt) {
                temperatureAt = last;
                temperature = block.values.last();
                haveTemperature = true;
            }
        }
        if (haveTemperature && double(cellEnd - temperatureAt) / 1000.0
                <= kSlowTemperatureMaxAgeMs) {
            values[0 * kSlowCells + cell] =
                standardize(m_slowStd, m_slowMean, 0, temperature);
            values[3 * kSlowCells + cell] = 1.0;
            /* 温度年龄按 6 s 归一化；其余慢状态按 0.5 s 归一化。 */
            values[6 * kSlowCells + cell] =
                qBound(0.0, double(cellEnd - temperatureAt) / 1000.0 / kSlowTemperatureMaxAgeMs, 1.0);
            ++validTemperature;
        } else {
            values[6 * kSlowCells + cell] = 1.0;   /* 不可用：年龄取上界 1 */
        }

        auto couple = [&](const QVector<NativeBlock> &blocks, int channelIndex, int validIndex,
                          int ageIndex) {
            int count = 0;
            quint64 first = 0, last = 0;
            double sum = 0.0;
            for (const auto &block : blocks) {
                if (!block.dataValid || block.count == 0) continue;
                const quint64 begin = block.t0Us;
                const quint64 end = block.endUs();
                if (begin < cellStart || end >= cellEnd) continue;
                if (!count) first = begin;
                last = end;
                ++count;
                sum += block.values.last();
            }
            if (count >= 2 && double(last - first) / 1000.0 >= kSlowMinSpanMs
                && double(cellEnd - last) / 1000.0 <= kSlowMaxAgeMs) {
                values[channelIndex * kSlowCells + cell] =
                    standardize(m_slowStd, m_slowMean, channelIndex, sum / double(count));
                values[validIndex * kSlowCells + cell] = 1.0;
                values[ageIndex * kSlowCells + cell] =
                    qBound(0.0, double(cellEnd - last) / 1000.0 / kSlowMaxAgeMs, 1.0);
                return 1;
            }
            values[ageIndex * kSlowCells + cell] = 1.0;   /* 不可用：年龄取上界 1 */
            return 0;
        };
        validCurrent += couple(window.current, 1, 4, 7);
        validSpeed += couple(window.speed, 2, 5, 8);
    }
    const double ratioTemperature = double(validTemperature) / kSlowReal;
    const double ratioCurrent = double(validCurrent) / kSlowReal;
    const double ratioSpeed = double(validSpeed) / kSlowReal;
    quality.slowRatio = qMin(ratioTemperature, qMin(ratioCurrent, ratioSpeed));
    quint64 latest = 0;
    for (const auto &block : window.temperature) {
        if (!block.dataValid || block.values.isEmpty() || block.t0Us < window.startUs
            || block.endUs() >= window.endUs || block.endUs() < latest) continue;
        latest = block.endUs();
        quality.temperatureC = block.values.last();
    }
    quality.temperatureValid = latest && std::isfinite(quality.temperatureC)
        && window.endUs - latest <= quint64(kSlowTemperatureMaxAgeMs * 1000);
    quality.overtemperatureMasked = quality.temperatureValid && quality.temperatureC <= 35.0;
}

bool FaultPreprocessor::build(const FaultWindowInput &window, FaultWindowOutput &out,
                              QString *error) const
{
    out = FaultWindowOutput{};
    if (!m_loaded) return fail(error, QStringLiteral("预处理资产未加载"));
    if (window.endUs <= window.startUs || window.endUs - window.startUs != kFaultWindowUs)
        return fail(error, QStringLiteral("窗口长度不符"));

    QVector<double> tf, psd, stats, pose, fast, slow;
    buildTimeFrequency(window, tf, out.quality);
    buildSpectrum(window, psd);
    buildStatistics(window, stats);
    buildPose(window, pose, out.quality);
    buildFastState(window, fast, out.quality);
    buildSlowState(window, slow, out.quality);

    /* 窗口内首个块之前的缺口属于窗口外上下文，不据此判废；窗口内部缺口才计入。 */
    for (int i = 1; i < window.vibration.size(); ++i)
        if (window.vibration[i].gapBefore) ++out.quality.gapBlocks;

    auto reject = [&](const QString &reason) {
        out.ok = false;
        out.reason = reason;
        if (error) *error = QStringLiteral("窗口质量不足：%1").arg(reason);
        return false;
    };
    if (out.quality.blocks < m_minBlocks) return reject(QStringLiteral("waiting_context"));
    /* 冻结 native-block 模型按实际时间格处理缺失，不要求连续四秒无丢块。
     * gapBlocks 保留用于诊断；是否可推断由运行包中的覆盖率等质量门槛决定。
     * 不拼接缺口、不填充原始采样，也不把一个缺块扩展成全窗不可用。 */
    if (out.quality.tfRatio < m_tfMinRatio) return reject(QStringLiteral("tf_coverage"));
    if (out.quality.fastRatio < m_fastMinRatio) return reject(QStringLiteral("missing_state"));
    if (out.quality.slowRatio < m_slowMinRatio) return reject(QStringLiteral("slow_state"));
    if (out.quality.poseAgeMs < 0.0 || out.quality.poseAgeMs > m_poseMaxAgeMs)
        return reject(QStringLiteral("pose_age"));
    if (!out.quality.gravityOk) return reject(QStringLiteral("gravity"));

    QVector<float> input;
    input.reserve(MLK_FAULT_INPUT_FLOATS);
    auto append = [&input](const QVector<double> &values) {
        for (double v : values) input.append(std::isfinite(v) ? float(v) : 0.0f);
    };
    append(tf);
    append(psd);
    append(stats);
    append(pose);
    append(fast);
    append(slow);
    if (input.size() != MLK_FAULT_INPUT_FLOATS)
        return fail(error, QStringLiteral("输入布局长度不符：%1").arg(input.size()));
    out.input = input;
    out.ok = true;
    return true;
}

} // namespace mlk
