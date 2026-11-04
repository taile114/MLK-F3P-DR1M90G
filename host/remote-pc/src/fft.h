#pragma once

#include <QVector>
#include <complex>
#include <cmath>

namespace mlk {

inline QVector<double> hannWindow(int n)
{
    QVector<double> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));
    return w;
}

inline void fftRadix2(QVector<std::complex<double>> &a)
{
    const int n = a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

inline QVector<double> powerSpectrumDb(const QVector<qint32> &samples, int &nfft, int maxPoints = 2048)
{
    int n = 1;
    while (n * 2 <= samples.size() && n < maxPoints)
        n <<= 1;
    nfft = n;
    if (n < 2)
        return {};

    const QVector<double> win = hannWindow(n);
    QVector<std::complex<double>> a(n);
    const int off = samples.size() - n;
    for (int i = 0; i < n; ++i)
        a[i] = std::complex<double>(samples[off + i] * win[i], 0.0);

    fftRadix2(a);

    QVector<double> mag(n / 2);
    for (int i = 0; i < n / 2; ++i) {
        const double m = std::abs(a[i]) / n;
        mag[i] = 20.0 * std::log10(m + 1e-9);
    }
    return mag;
}

} // namespace mlk
