// AudioMeter — a live input-level bar for the camera's USB audio input.
//
// WHAT THIS IS NOT: it is not an SDK feature. libdev has nothing to do with it,
// so this object deliberately does NOT live on the CameraWorker thread — it runs
// on the GUI thread and talks only to Qt Multimedia. Mixing it into the worker
// would put an audio device's callbacks behind the SDK's blocking USB calls for
// no reason at all.
//
// WHAT IT MEASURES: whatever the camera is currently sending on its USB audio
// endpoint — the built-in mic array or a paired wireless mic, depending on the
// input the camera has selected, and AFTER the camera's own gain/mute/noise
// processing. It is therefore an honest "is sound getting through" indicator and
// NOT a wireless-mic level meter; the UI must not label it as one.
//
// WHY IT IS NOT ALWAYS ON: opening the camera's audio device holds it. Some
// capture stacks hand the same device to several clients, others do not, and the
// app has no business being the reason a meeting client finds the mic busy. The
// meter is therefore driven by the Mic page's visibility (setActive) and holds
// the device only while the user is actually looking at the bar.
//
// Honesty rules, same as PreviewEngine's:
//   * The OBSBOT input is found by matching the device DESCRIPTION, never by
//     assuming an index, and the chosen device is named in the activity log.
//   * If nothing matches and there is no default input either, the meter reports
//     itself UNAVAILABLE with a reason — it never sits at zero pretending to work.
//   * Every failure (open refused, device vanished, format rejected) stops the
//     meter with the reason in the log rather than freezing the last level.
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QObject>
#include <QPointer>
#include <QString>

class QAudioSource;
class QIODevice;
class QTimer;

class AudioMeter : public QObject {
    Q_OBJECT

    // An input device was found (either an OBSBOT one or the system default).
    // False means there is nothing to listen to and the UI must say so.
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    // Human-readable name of the device the meter uses, for the UI and the log.
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY availabilityChanged)
    // True when the chosen device is the SYSTEM DEFAULT rather than a device
    // whose name says OBSBOT — the meter then shows some other microphone, which
    // the page has to admit instead of implying it is the camera.
    Q_PROPERTY(bool usingFallback READ usingFallback NOTIFY availabilityChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availabilityChanged)
    // Capturing right now. Driven by the Mic page's visibility — see setActive.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // Display levels, 0..1, mapped from dBFS with a kFloorDb floor (see the .cpp):
    // `level` is the RMS of the last window, `peak` a decaying sample peak.
    // Both are 0 while inactive — which the UI distinguishes from unavailable.
    Q_PROPERTY(qreal level READ level NOTIFY levelChanged)
    Q_PROPERTY(qreal peak READ peak NOTIFY levelChanged)
    // True when the last window contained a full-scale sample — the honest cue
    // that the camera's gain is set too high, which is the main reason to look at
    // a level meter at all.
    Q_PROPERTY(bool clipping READ clipping NOTIFY levelChanged)

public:
    explicit AudioMeter(QObject *parent = nullptr);
    ~AudioMeter() override;

    bool available() const { return !m_device.isNull(); }
    QString deviceName() const { return m_deviceName; }
    bool usingFallback() const { return m_usingFallback; }
    QString unavailableReason() const;
    bool active() const { return m_active; }
    qreal level() const { return m_level; }
    qreal peak() const { return m_peak; }
    bool clipping() const { return m_clipping; }

public slots:
    // Driven from QML by the Mic page's visibility. Idempotent: calling it with
    // the value it already has does nothing, so a binding may fire freely.
    void setActive(bool on);
    void start();
    void stop();

signals:
    void availabilityChanged();
    void activeChanged();
    void levelChanged();
    // Same (kind, message) shape as CameraController::logLine; main.cpp chains it
    // into the controller's signal so meter events land in the one activity log.
    void logLine(const QString &kind, const QString &message);

private:
    void refreshDevice();     // (re)pick the input device; logs what it chose
    void drain();             // pull whatever the source has ready and fold it in
    void emitLevel();         // ~30 Hz UI update from the accumulated window
    void teardownSource();    // stop + delete the QAudioSource and its QIODevice

    QMediaDevices m_devices;  // enumeration + hotplug notifications
    QAudioDevice m_device;    // null => unavailable
    QString m_deviceName;
    bool m_usingFallback = false;
    bool m_devicesEverSeen = false;

    QAudioFormat m_format;
    QAudioSource *m_source = nullptr;
    QPointer<QIODevice> m_io;   // owned by m_source; QPointer so a reset is visible
    QTimer *m_uiTimer = nullptr;

    bool m_active = false;
    // Accumulated over one UI frame, then reset by emitLevel.
    double m_sumSq = 0.0;
    quint64 m_sampleCount = 0;
    float m_windowPeak = 0.0f;
    bool m_windowClipped = false;

    qreal m_level = 0.0;
    qreal m_peak = 0.0;
    bool m_clipping = false;
};
