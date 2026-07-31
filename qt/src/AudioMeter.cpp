#include "AudioMeter.h"

#include <QAudioSource>
#include <QIODevice>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {

// UI refresh cadence. 30 Hz is the ceiling the design asks for: fast enough that
// the bar tracks speech, slow enough that it costs nothing. The AUDIO is read as
// fast as the device delivers it (readyRead); only the repaint is throttled, so
// no transient is missed — it lands in the window's peak.
constexpr int kUiIntervalMs = 33;
// Display floor. A linear 0..1 bar off raw RMS is useless — normal speech sits
// around 0.05 and looks like silence — so the bar is dBFS-mapped: -60 dBFS reads
// as empty, 0 dBFS as full. This is a DISPLAY choice and is stated in the UI; the
// underlying numbers are not rescaled anywhere else.
constexpr qreal kFloorDb = -60.0;
// Peak-hold decay, in display units per UI frame (~0.6/s at 30 Hz). Slow enough
// to see a transient, fast enough that the marker never lies about the present.
constexpr qreal kPeakDecay = 0.02;
// A sample at or above this normalized magnitude counts as clipping. Not 1.0:
// integer full scale rounds to just under it after normalization.
constexpr float kClipLevel = 0.999f;
// Capture buffer. Deliberately modest — the meter needs latency, not history, and
// a large buffer just delays the bar. ~20 ms at the device's own rate.
constexpr int kBufferMs = 20;

// Match on the device DESCRIPTION (what a user sees in their sound settings).
// The camera enumerates as e.g. "OBSBOT Tiny 3 Analog Stereo" via PipeWire/PULSE;
// the vendor string is the only part that is stable across backends and models.
bool looksLikeObsbot(const QAudioDevice &d) {
    return d.description().contains(QStringLiteral("OBSBOT"), Qt::CaseInsensitive)
           || QString::fromUtf8(d.id()).contains(QStringLiteral("OBSBOT"), Qt::CaseInsensitive);
}

// dBFS-mapped display value in 0..1 (see kFloorDb).
qreal toDisplay(double amplitude) {
    if (amplitude <= 0.0) return 0.0;
    const qreal db = 20.0 * std::log10(amplitude);
    if (db <= kFloorDb) return 0.0;
    if (db >= 0.0) return 1.0;
    return (db - kFloorDb) / (0.0 - kFloorDb);
}

} // namespace

AudioMeter::AudioMeter(QObject *parent) : QObject(parent) {
    m_uiTimer = new QTimer(this);
    m_uiTimer->setInterval(kUiIntervalMs);
    connect(m_uiTimer, &QTimer::timeout, this, &AudioMeter::emitLevel);

    // Hotplug: the camera can be plugged in after the app starts, and a default
    // input can appear or vanish at any time. Re-pick, and restart if the meter
    // is currently running on a device that is no longer the right one.
    connect(&m_devices, &QMediaDevices::audioInputsChanged, this, [this]() {
        const QAudioDevice was = m_device;
        refreshDevice();
        if (m_active && m_device != was) {
            stop();
            if (available()) start();
        }
    });

    refreshDevice();
}

AudioMeter::~AudioMeter() { teardownSource(); }

QString AudioMeter::unavailableReason() const {
    if (available()) return QString();
    if (!m_devicesEverSeen)
        return QStringLiteral("No audio input devices at all — is a sound server running?");
    return QStringLiteral("No OBSBOT audio input, and no default input either.");
}

// Pick the input to listen to: an OBSBOT-named device if there is one, otherwise
// the system default. The distinction is kept (m_usingFallback) rather than
// smoothed over, because a meter reading a laptop's built-in microphone while the
// page says "camera input" would be worse than no meter.
void AudioMeter::refreshDevice() {
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    m_devicesEverSeen = !inputs.isEmpty();

    QAudioDevice picked;
    for (const QAudioDevice &d : inputs) {
        if (looksLikeObsbot(d)) { picked = d; break; }
    }
    const bool fallback = picked.isNull();
    if (fallback) picked = QMediaDevices::defaultAudioInput();

    if (picked == m_device && fallback == m_usingFallback) return;

    m_device = picked;
    m_usingFallback = fallback && !picked.isNull();
    m_deviceName = picked.isNull() ? QString() : picked.description();

    if (picked.isNull()) {
        emit logLine("warn", QStringLiteral("input level: %1").arg(unavailableReason()));
    } else if (m_usingFallback) {
        emit logLine("warn", QStringLiteral(
            "input level: no OBSBOT audio input found — falling back to the system default "
            "input \"%1\". The bar shows THAT microphone, not the camera.").arg(m_deviceName));
    } else {
        emit logLine("sys", QStringLiteral("input level: using audio input \"%1\"").arg(m_deviceName));
    }
    emit availabilityChanged();
}

void AudioMeter::setActive(bool on) {
    if (on == m_active) return;
    if (on) start();
    else stop();
}

void AudioMeter::start() {
    if (m_active) return;
    // Honest, and quiet: the page already shows the meter as unavailable with the
    // reason, so opening the Mic page must not spam the log about it.
    if (!available()) return;

    // Start from the device's own preferred format — the one the backend can give
    // us without resampling — and only insist on the parts the arithmetic needs.
    QAudioFormat fmt = m_device.preferredFormat();
    if (fmt.sampleFormat() == QAudioFormat::Unknown || fmt.channelCount() < 1
        || fmt.sampleRate() < 1 || !m_device.isFormatSupported(fmt)) {
        // Fall back to the one format every backend supports.
        fmt.setSampleFormat(QAudioFormat::Int16);
        fmt.setChannelCount(1);
        fmt.setSampleRate(48000);
    }
    if (!m_device.isFormatSupported(fmt)) {
        emit logLine("warn", QStringLiteral("input level: \"%1\" accepts no format this meter can "
                                            "read — meter disabled").arg(m_deviceName));
        return;
    }

    m_format = fmt;
    m_source = new QAudioSource(m_device, fmt, this);
    // Latency, not history — a big buffer would just delay the bar.
    m_source->setBufferSize(std::max(1024, fmt.bytesPerFrame() * fmt.sampleRate() * kBufferMs / 1000));

    // Stop honestly on any device-side failure instead of freezing the last level.
    connect(m_source, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state != QAudio::StoppedState || !m_source) return;
        const QAudio::Error err = m_source->error();
        if (err == QAudio::NoError) return;
        emit logLine("warn", QStringLiteral("input level: capture stopped (audio error %1) — "
                                            "the device may be held by another application")
                                 .arg(static_cast<int>(err)));
        stop();
    });

    m_io = m_source->start();
    if (!m_io) {
        emit logLine("warn", QStringLiteral("input level: could not open \"%1\" for capture — "
                                            "another application may hold it").arg(m_deviceName));
        teardownSource();
        return;
    }
    connect(m_io, &QIODevice::readyRead, this, &AudioMeter::drain);

    m_active = true;
    m_uiTimer->start();
    // Name the device every time capture starts, and say plainly when it is not
    // the camera — refreshDevice's own log can happen before the app has a log to
    // write to (it runs in the constructor), so this line is the reliable one.
    emit logLine(m_usingFallback ? "warn" : "sys",
                 QStringLiteral("input level: listening to \"%1\" (%2 Hz, %3 ch)%4")
                     .arg(m_deviceName).arg(m_format.sampleRate()).arg(m_format.channelCount())
                     .arg(m_usingFallback
                              ? QStringLiteral(" — the SYSTEM DEFAULT input, not the camera: no "
                                               "OBSBOT audio device was found")
                              : QString()));
    emit activeChanged();
}

void AudioMeter::stop() {
    const bool was = m_active;
    teardownSource();
    m_uiTimer->stop();
    m_sumSq = 0.0;
    m_sampleCount = 0;
    m_windowPeak = 0.0f;
    m_windowClipped = false;
    // Fall back to a resting bar rather than leaving the last reading frozen on
    // screen, which would read as "the mic is still at this level".
    if (m_level != 0.0 || m_peak != 0.0 || m_clipping) {
        m_level = 0.0;
        m_peak = 0.0;
        m_clipping = false;
        emit levelChanged();
    }
    if (was) {
        m_active = false;
        emit logLine("sys", QStringLiteral("input level: released the audio input"));
        emit activeChanged();
    }
}

void AudioMeter::teardownSource() {
    if (!m_source) return;
    // Clear m_source FIRST, then drop every connection, and only then stop the
    // source. QAudioSource::stop() emits stateChanged synchronously, and the
    // handler for that calls stop() → teardownSource() again: without this order
    // the outer call would return into a m_source that the inner one had already
    // nulled and deleted. Detaching before stopping makes the re-entry impossible
    // rather than merely unlikely.
    QAudioSource *src = m_source;
    m_source = nullptr;
    if (m_io) m_io->disconnect(this);
    m_io = nullptr;
    src->disconnect(this);
    src->stop();
    src->deleteLater();
}

// Fold every sample that has arrived into the current UI window. Runs on the GUI
// thread from readyRead; the work is one pass of multiply-add over a few hundred
// samples per callback, which is why this can afford to live here.
void AudioMeter::drain() {
    if (!m_io || !m_active) return;
    const int bps = m_format.bytesPerSample();
    if (bps <= 0) return;

    const QByteArray block = m_io->readAll();
    const int samples = static_cast<int>(block.size()) / bps;
    const char *p = block.constData();
    for (int i = 0; i < samples; ++i, p += bps) {
        const float v = std::fabs(m_format.normalizedSampleValue(p));
        m_sumSq += static_cast<double>(v) * v;
        if (v > m_windowPeak) m_windowPeak = v;
        if (v >= kClipLevel) m_windowClipped = true;
    }
    m_sampleCount += static_cast<quint64>(samples);
}

void AudioMeter::emitLevel() {
    // No audio arrived this frame: decay rather than hold. A stalled device must
    // not leave a bar sitting at half scale looking live.
    const double rms = m_sampleCount > 0 ? std::sqrt(m_sumSq / static_cast<double>(m_sampleCount)) : 0.0;
    const qreal newLevel = toDisplay(rms);
    const qreal windowPeak = toDisplay(m_windowPeak);
    const bool clipped = m_windowClipped;

    m_sumSq = 0.0;
    m_sampleCount = 0;
    m_windowPeak = 0.0f;
    m_windowClipped = false;

    // Peak-hold with decay: rises instantly, falls slowly, so a transient stays
    // visible for a moment without ever exceeding what was actually measured.
    const qreal decayed = std::max<qreal>(0.0, m_peak - kPeakDecay);
    const qreal newPeak = std::max(windowPeak, decayed);

    if (qFuzzyCompare(newLevel + 1.0, m_level + 1.0)
        && qFuzzyCompare(newPeak + 1.0, m_peak + 1.0) && clipped == m_clipping)
        return;   // nothing moved; don't churn QML bindings 30 times a second
    m_level = newLevel;
    m_peak = newPeak;
    m_clipping = clipped;
    emit levelChanged();
}
