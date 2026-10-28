#include "sonar_qa_demo.h"

#include "sonar_qa_analysis.h"

#include <algorithm>
#include <cmath>

SonarQaDemoDevice::SonarQaDemoDevice(bool realTime, QObject* parent) : SonarQaDevice(parent), realTime_(realTime)
{
    settings = { { "freq", 700 }, { "pulse", 10 }, { "boost", 1 }, { "samples", 1600 }, { "resolution", 20 },
                 { "offset", 0 }, { "period", 100 }, { "chart", 1 }, { "chartZ", 1 }, { "voltage", 0 }, { "temp", 0 } };
    timer_.setInterval(realTime_ ? 100 : 0);
    connect(&timer_, &QTimer::timeout, this, &SonarQaDemoDevice::emitPing);
    timer_.start();
}

SonarQaDevice::Identity SonarQaDemoDevice::identity() const
{
    return { QStringLiteral("NanoSSS (demo)"), m.firmware, m.serial, m.board };
}

void SonarQaDemoDevice::restore(const QVariantMap& s)
{
    restored = s;
    settings = s;
    freq_ = s.value("freq").toInt();
    pulses_ = s.value("pulse").toInt();
    pendingFreq_ = pendingPulses_ = -1;
    if (realTime_) {
        timer_.setInterval(qMax(10, s.value("period").toInt()));
    }
}

void SonarQaDemoDevice::applyRun(int samples, int periodMs, int resolutionMm, int offset, int boost)
{
    settings["samples"] = samples;
    settings["period"] = periodMs;
    settings["resolution"] = resolutionMm;
    settings["offset"] = offset;
    settings["boost"] = boost;
    settings["chart"] = 1;
    settings["chartZ"] = 0;
    settings["voltage"] = 1;
    settings["temp"] = 1;
    if (realTime_) {
        timer_.setInterval(qMax(10, periodMs));
    }
}

void SonarQaDemoDevice::setTransmitter(int freqKhz, int pulses)
{
    pendingFreq_ = freqKhz;
    pendingPulses_ = pulses;
}

double SonarQaDemoDevice::noise()
{
    rng_ = rng_ * 1103515245u + 12345u;
    return double((rng_ >> 16) % 9) - 4.0;   // -4..+4 codes
}

void SonarQaDemoDevice::emitPing()
{
    if (silent) {
        return;
    }
    const int n = qMax(4, settings.value("samples").toInt() / 2);
    QVector<QVector<uint8_t>> sides(2, QVector<uint8_t>(n));
    const int pulses = pulses_ > 0 ? pulses_ : m.zeroPulses;
    const double x = 2.0 * (freq_ - m.f0) / m.bw;
    const double respCodes = 20.0 * std::log10(1.0 / std::sqrt(1.0 + x * x)) * sonarqa::kCodesPerDb;
    for (int s = 0; s < 2; ++s) {
        QVector<double> a(n);
        for (int i = 0; i < n; ++i) {
            a[i] = (pulses > 0 ? m.floorOn : m.floorOff) + noise();
        }
        if (pulses > 0) {
            const double len = m.ringLen * m.ringScale;
            for (int i = 0; i < n && i < len; ++i) {
                a[i] = std::max(a[i], m.ringTop - (m.ringTop - m.floorOn) * i / len);
            }
            for (int i = int(len); i < n && i < len + m.tailLen; ++i) {
                a[i] = m.floorOn + m.tailCodes + noise();
            }
            const double top = m.echoTop + m.gainDb * sonarqa::kCodesPerDb + respCodes - (s ? 3 : 0);
            for (int k = 0; k < 3; ++k) {
                const int pos = m.targetPos * (k + 1);
                for (int d = -6; d <= 6; ++d) {
                    if (pos + d >= 0 && pos + d < n) {
                        a[pos + d] = std::max(a[pos + d], top - k * m.multiLoss - 4.0 * std::abs(d));
                    }
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            sides[s][i] = uint8_t(std::clamp(std::lround(a[i]), 0L, 255L));
        }
        if (m.tagPings) {
            sides[s][n - 2] = uint8_t(freq_ / 5);
            sides[s][n - 1] = uint8_t(pulses_);
        }
    }
    emit ping(sides, settings.value("resolution").toInt() * 0.001f);
    if (++count_ % 5 == 0 && m.sensors) {
        emit supply(float(m.supply + 0.01 * noise()));
        emit temperature(float(m.temp + 0.02 * noise()));
    }
    if (pendingFreq_ >= 0) {
        freq_ = pendingFreq_;
        pulses_ = pendingPulses_;
        pendingFreq_ = pendingPulses_ = -1;
    }
}
