#include "CameraController.h"
#include "CameraWorker.h"
#include "PreviewFormats.h"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {
// Mirror of the SDK's AiWorkModeType values we use (see dev.hpp:554). Kept as
// local constants so this GUI-thread TU doesn't need the SDK headers.
constexpr int kAiNone = 0;    // AiWorkModeNone
constexpr int kAiHuman = 2;   // AiWorkModeHuman (single-person tracking)
constexpr int kAiSwitching = 6;

// NOTE: contains a UTF-8 degree sign — convert with fromUtf8, never fromLatin1
// (fromLatin1 renders it as the "78Â°" mojibake Rex saw in the log).
const char *kFovLabels[] = {"Wide 86°", "Medium 78°", "Narrow 65°"};
// (Preview resolutions live in PreviewFormats.h — shared with PreviewEngine.)

// Settle delay before the automatic "return to preset after AI off" move: the
// gimbal keeps disengaging from AI for ~1 s after the off command is accepted,
// and an immediate position move gets eaten or truncated.
constexpr int kAiReturnDelayMs = 1500;
// Window after a confirmed AI-Track ON in which a device-side drop back to None
// is reported as "couldn't lock on" (the Tiny 3 accepts the command but silently
// disengages when no person is in view). Longer than the gesture-mode status
// duty period (15 s) so the hint can't miss just because the telltale push
// arrived at the next duty tick (review finding).
constexpr qint64 kAiDisengageHintMs = 20000;
// Safety net for the mic pair/clear busy cue. Longer than the worker's
// wake-settle delay plus a round trip, so a normal command always clears the cue
// via its result and this only fires if one is never answered.
constexpr int kMicPairCueMaxMs = 12000;    // clear: the camera answers on its own
constexpr int kMicPairWaitMaxMs = 90000;   // pair: long enough to fetch the mic and hold its button
// Give-up window for an in-flight audio source / pickup-pattern pick. Their only
// readback is the status push, so this must outlast one push cycle (2–3 s) plus
// the pulse the worker opens in low-traffic mode. When it fires, the optimistic
// value is dropped and the device's own value drives the selector again.
constexpr int kAudioConfirmMs = 6000;
// Default noise-reduction strength used when the user turns NR on before the
// camera has ever reported a level (documented range [1-10]; 5 = middle).
constexpr int kAudioNoiseLevelDefault = 5;
// Per-mic gain step, in the DEVICE's own units. The SDK documents no range for
// wireless-mic gain, so the UI adjusts the value the camera reported by this much
// per click rather than pretending to know a 0–100 scale. 1 is the smallest step
// the int8 the device reports back can express — deliberately conservative, since
// a wrong guess about the unit size is a wrong guess about how loud the mic gets.
constexpr int kMicGainStep = 1;
// How long a device-event cue ("mute button", "track on") stays on a mic card.
// Long enough to notice, short enough that a stale cue never reads as state.
constexpr int kMicTipMs = 4000;
} // namespace

CameraController::CameraController(QObject *parent) : QObject(parent) {
    m_settings = Settings::load();
    m_aiModeName = QStringLiteral("Off");

    // ffplay fallback availability: ffplay present AND a /dev/video0 node exists.
    // (The embedded preview has its own by-name device detection in PreviewEngine.)
    m_previewAvailable = !QStandardPaths::findExecutable("ffplay").isEmpty()
                         && QFileInfo::exists("/dev/video0");

    m_pendingTimer = new QTimer(this);
    m_pendingTimer->setSingleShot(true);
    m_pendingTimer->setInterval(4000);   // safety: never leave AI 'pending' stuck
    connect(m_pendingTimer, &QTimer::timeout, this, [this]() {
        m_aiInFlight = 0;
        if (m_aiPending) { m_aiPending = false; emit aiChanged(); }
    });

    // Same safety net for the mic-button selector: if the camera never confirms
    // the new assignment, stop showing the optimistic value and fall back to
    // whatever the device actually reports.
    //
    // This MUST outlast the worker's confirm poll (kKeyConfirmBudgetMs, 6 s) or
    // the UI declares failure while the answer is still on its way. It used to
    // be 4 s against a camera measured taking up to ~3 s to report a write it
    // had already applied — close enough that the give-up fired first and the
    // selector sprang back on a change that had in fact succeeded.
    m_micButtonTimer = new QTimer(this);
    m_micButtonTimer->setSingleShot(true);
    m_micButtonTimer->setInterval(7000);
    connect(m_micButtonTimer, &QTimer::timeout, this, [this]() {
        if (m_micButtonTarget >= 0) {
            emit logLine("warn", QStringLiteral("mic button: camera did not confirm the new assignment"));
            m_micButtonTarget = -1;
            emit micChanged();
        }
    });

    // Give-up net for an in-flight gain step: if the camera never reports the
    // value we asked for, stop showing it and fall back to what it does report.
    m_micGainTimer = new QTimer(this);
    m_micGainTimer->setSingleShot(true);
    m_micGainTimer->setInterval(4000);
    connect(m_micGainTimer, &QTimer::timeout, this, [this]() {
        bool any = false;
        for (MicTx &t : m_micTx)
            if (t.gainPending) { t.gainPending = false; any = true; }
        if (any) {
            emit logLine("warn", QStringLiteral("mic gain: the camera did not confirm the new value — "
                                                "showing what it reports again"));
            emit micChanged();
        }
    });

    // Same net again for the audio source / pickup-pattern selectors. Their only
    // readback is the status push, so an ignored command would otherwise leave
    // the segment showing a choice the camera never made.
    m_audioSourceTimer = new QTimer(this);
    m_audioSourceTimer->setSingleShot(true);
    m_audioSourceTimer->setInterval(kAudioConfirmMs);
    connect(m_audioSourceTimer, &QTimer::timeout, this, [this]() {
        if (m_audioSourceTarget >= 0) {
            emit logLine("warn", QStringLiteral("audio source: the camera did not report the new source — "
                                                "showing what it actually reports again"));
            m_audioSourceTarget = -1;
            emit audioChanged();
        }
    });
    m_audioModeTimer = new QTimer(this);
    m_audioModeTimer->setSingleShot(true);
    m_audioModeTimer->setInterval(kAudioConfirmMs);
    connect(m_audioModeTimer, &QTimer::timeout, this, [this]() {
        if (m_audioModeTarget >= 0) {
            emit logLine("warn", QStringLiteral("audio pickup: the camera did not report the new pattern — "
                                                "showing what it actually reports again"));
            m_audioModeTarget = -1;
            emit audioChanged();
        }
    });

    // Blanks the transient per-mic event cues. One shared timer, not one per
    // slot: the cues are momentary and a second press simply restarts the window.
    m_micTipTimer = new QTimer(this);
    m_micTipTimer->setSingleShot(true);
    m_micTipTimer->setInterval(kMicTipMs);
    connect(m_micTipTimer, &QTimer::timeout, this, [this]() {
        // Only the highlight expires. Clearing the text made the row claim "no
        // press seen yet" moments after reporting one, which is simply false —
        // and it threw away the single most useful thing the row can say.
        bool dirty = false;
        for (MicTx &t : m_micTx) {
            if (t.tipRecent) { t.tipRecent = false; dirty = true; }
        }
        if (dirty) emit micChanged();
    });

    m_worker = new CameraWorker;   // no parent: it will live on m_thread
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_worker, &CameraWorker::init);

    m_gesture = m_settings.gesture;   // restore last gesture intent

    connect(m_worker, &CameraWorker::logLine, this, &CameraController::logLine);
    connect(m_worker, &CameraWorker::connectionResolved, this, &CameraController::onConnectionResolved);
    connect(m_worker, &CameraWorker::deviceLost, this, &CameraController::onDeviceLost);
    connect(m_worker, &CameraWorker::statusUpdate, this, &CameraController::onStatusUpdate);
    connect(m_worker, &CameraWorker::auxStatus, this, &CameraController::onAuxStatus);
    connect(m_worker, &CameraWorker::zoomUpdate, this, &CameraController::onZoomUpdate);
    connect(m_worker, &CameraWorker::imageParams, this, &CameraController::onImageParams);
    connect(m_worker, &CameraWorker::commandResult, this, &CameraController::onWorkerResult);
    connect(m_worker, &CameraWorker::presetCaptured, this, &CameraController::onPresetCaptured);
    connect(m_worker, &CameraWorker::micStatus, this, &CameraController::onMicStatus);
    connect(m_worker, &CameraWorker::twsInfo, this, &CameraController::onTwsInfo);
    connect(m_worker, &CameraWorker::twsMicAudio, this, &CameraController::onTwsMicAudio);
    connect(m_worker, &CameraWorker::micAudioSelect, this, &CameraController::onMicAudioSelect);
    connect(m_worker, &CameraWorker::audioState, this, &CameraController::onAudioState);
    connect(m_worker, &CameraWorker::deviceEvent, this, &CameraController::onDeviceEvent);
    connect(m_worker, &CameraWorker::deviceFault, this, &CameraController::onDeviceFault);
    connect(m_worker, &CameraWorker::micTip, this, &CameraController::onMicTip);

    m_thread.start();
}

CameraController::~CameraController() {
    stopPreview();   // kill the external ffplay so it doesn't linger after the app closes
    // Optional "sleep on exit": issue it BLOCKING on the worker (which is still
    // running its event loop here) so the command completes before we tear the
    // worker down. Guarded by connected() so we never touch a dead handle.
    if (m_worker && connected() && m_settings.sleepOnExit)
        QMetaObject::invokeMethod(m_worker, "cmdSleep", Qt::BlockingQueuedConnection);
    // Deterministic teardown (CODE_REVIEW #1/#2/#3/#9): disable the SDK callback
    // and release the device on the worker thread while its event loop is still
    // alive, THEN quit and join. No detached threads, no post-loop UAF.
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
    m_worker = nullptr;
}

void CameraController::start(int waitMs) {
    m_connState = Discovering;
    emit connStateChanged();
    QMetaObject::invokeMethod(m_worker, "startDiscovery", Qt::QueuedConnection, Q_ARG(int, waitMs));
}

// ---------------------------------------------------------------------------
// Derived getters
// ---------------------------------------------------------------------------
bool CameraController::aiTracking() const { return m_aiPending ? m_targetTracking : m_aiTracking; }
bool CameraController::faceFocus() const { return m_aiPending ? m_targetFace : m_faceFocus; }
bool CameraController::gesture() const { return m_gesture; }

QString CameraController::previewRes() const {
    const int i = m_settings.previewResIndex;
    return (i >= 0 && i < kPreviewResCount) ? QString::fromLatin1(kPreviewRes[i].label)
                                            : QStringLiteral("—");
}

QString CameraController::appVersion() const {
#ifdef APP_VERSION
    return QStringLiteral(APP_VERSION);
#else
    return QStringLiteral("0.0.0");
#endif
}

double CameraController::speedValue() const {
    // Reference gimbal speed (deg/s) for step moves (gimbalSetSpeedPositionR) and
    // the joystick velocity scale. Speed = HOW FAST the gimbal travels; the move
    // DISTANCE is set solely by the Step selection. Medium was 40 deg/s, which is
    // so fast for a small step (~0.12s) that the gimbal's accel/decel made the
    // move stutter and undershoot; 30 keeps it clearly faster than Slow but
    // smooth. (Tuning value — refine from hardware feel.)
    return m_settings.speedMode == 1 ? 30.0   // Medium
                                     : 20.0;  // Slow
}

QString CameraController::decodeAiMode(int raw) const {
    switch (raw) {
    case 0:  return QStringLiteral("Off");
    case 1:  return QStringLiteral("Group track");
    case 2:  return QStringLiteral("Human track");
    case 3:  return QStringLiteral("Hand track");
    case 4:  return QStringLiteral("Whiteboard");
    case 5:  return QStringLiteral("Desk");
    case 6:  return QStringLiteral("Switching…");
    case 7:  return QStringLiteral("Speech");
    case 14: return QStringLiteral("Portrait track");
    case 15: return QStringLiteral("Customize");
    default: return QStringLiteral("Mode %1").arg(raw);   // honest, never blank (#7)
    }
}

QVariantList CameraController::presets() const {
    QVariantList out;
    for (int i = 0; i < 3; ++i) {
        const PresetData &p = m_settings.presets[i];
        QVariantMap m;
        m["index"] = i;
        m["set"] = p.set;
        m["name"] = p.name.isEmpty() ? QStringLiteral("Preset %1").arg(i + 1) : p.name;
        m["pan"] = p.pan;
        m["tilt"] = p.tilt;
        m["zoom"] = p.zoom;
        m["fov"] = p.fov;
        m["summary"] = p.set ? QStringLiteral("pan %1° · tilt %2° · %3x")
                                   .arg(p.pan, 0, 'f', 0).arg(p.tilt, 0, 'f', 0).arg(p.zoom, 0, 'f', 2)
                             : QStringLiteral("Empty slot");
        out.append(m);
    }
    return out;
}

// One map per transmitter slot for the Mic page's Repeater — built exactly like
// presets() above. tx is 1-based (index+1); battery -1 means "unknown". No name
// or firmware key: the official SDK exposes no source for either (see MicTx).
QVariantList CameraController::micTxList() const {
    QVariantList out;
    for (int i = 0; i < 2; ++i) {
        const MicTx &t = m_micTx[i];
        QVariantMap m;
        m["tx"] = i + 1;
        m["online"] = t.online;
        m["battery"] = t.battery;
        m["charging"] = t.charging;
        m["muted"] = t.muted;
        // Gain is signed and in the device's own undocumented units, so it needs
        // its own known-flag: -1 is a legal gain, not "unknown".
        m["gain"] = t.gainPending ? t.gainTarget : t.gain;
        m["gainKnown"] = t.gainKnown || t.gainPending;
        // Report-only (no exported per-mic setter): 0/1 and the device's raw
        // level byte, -1 when the camera has not said.
        m["ns"] = t.ns;
        m["nsLevel"] = t.nsLevel;
        // Last device-event cue for this slot; empty on a camera that never
        // pushes events, which is the expected case on a Tiny 3.
        m["tip"] = t.tip;
        m["tipAt"] = t.tipAt;
        m["tipRecent"] = t.tipRecent;

        out.append(m);
    }
    return out;
}

// The index IS Device::DevTWSKeyType — these are the official header's own
// descriptions of DevTWSKeyTrack / TrackSwitch / ZoomX1 / Record.
static QString micButtonName(int idx) {
    switch (idx) {
    case 0:  return QStringLiteral("Trigger tracking");
    case 1:  return QStringLiteral("Switch tracking target");
    case 2:  return QStringLiteral("Zoom to 1.0×");
    case 3:  return QStringLiteral("PC recording");
    default: return QStringLiteral("Action %1").arg(idx);   // honest, never blank
    }
}

QString CameraController::micButtonActionName() const {
    return m_micButtonDevice < 0 ? QStringLiteral("—") : micButtonName(m_micButtonDevice);
}

// Device::DevAudioSourceType, spelled the way the page talks about it. The
// camera reports a wireless Vox SE as Bluetooth(3) — it has no separate
// "2.4 GHz mic" source — so that is what gets named here.
static QString audioSourceLabel(int src) {
    switch (src) {
    case 0:  return QStringLiteral("built-in mic array");
    case 1:  return QStringLiteral("aux line in");
    case 2:  return QStringLiteral("aux mic in");
    case 3:  return QStringLiteral("wireless mic (Vox SE)");
    case 4:  return QStringLiteral("USB-C audio");
    default: return QStringLiteral("source %1").arg(src);   // honest, never blank
    }
}

// Device::AudioModeType, using the header's own descriptions.
static QString audioModeLabel(int mode) {
    switch (mode) {
    case 0:  return QStringLiteral("Omnidirectional");
    case 1:  return QStringLiteral("Stereo");
    case 2:  return QStringLiteral("Forward pointing");
    case 3:  return QStringLiteral("Backward pointing");
    case 4:  return QStringLiteral("Forward and backward");
    case 5:  return QStringLiteral("Music");
    default: return QStringLiteral("pattern %1").arg(mode);
    }
}

// Deliberately built from the DEVICE value, never the optimistic one: this is
// the "what the camera actually reports" row next to the selector.
QString CameraController::audioSourceName() const {
    return m_audioSourceDevice < 0 ? QStringLiteral("—") : audioSourceLabel(m_audioSourceDevice);
}

QString CameraController::audioModeName() const {
    return m_audioModeDevice < 0 ? QStringLiteral("—") : audioModeLabel(m_audioModeDevice);
}

void CameraController::persist() {
    if (!Settings::save(m_settings))
        emit logLine("warn", QStringLiteral("settings: failed to write %1").arg(Settings::configPath()));
}

// ---------------------------------------------------------------------------
// Persisted setters
// ---------------------------------------------------------------------------
void CameraController::setMoveStepDeg(int deg) {
    if (deg == m_settings.moveStepDeg) return;
    m_settings.moveStepDeg = deg;
    persist();
    emit settingsChanged();
}
void CameraController::setSpeedMode(int mode) {
    if (mode == m_settings.speedMode) return;
    m_settings.speedMode = mode;
    persist();
    emit settingsChanged();
}
void CameraController::setStartupPreset(int p) {
    if (p < 0 || p > 3 || p == m_settings.startupPreset) return;
    m_settings.startupPreset = p;
    persist();
    emit settingsChanged();
}
namespace {
// autoSleepIdx → SDK seconds (<=0 disables) and human label. Index 0 ("Device")
// is not in the table: it means "don't manage" and is never sent.
const struct { int secs; const char *label; } kAutoSleep[] = {
    {0, "Device"}, {0, "Never"}, {120, "2 min"}, {300, "5 min"}, {600, "10 min"}, {1200, "20 min"},
};
} // namespace

void CameraController::setAutoSleepIndex(int idx) {
    if (idx < 0 || idx > 5 || idx == m_settings.autoSleepIdx) return;
    m_settings.autoSleepIdx = idx;
    persist();
    emit settingsChanged();
    if (idx > 0 && connected())
        QMetaObject::invokeMethod(m_worker, "cmdSetAutoSleep", Qt::QueuedConnection,
                                  Q_ARG(int, kAutoSleep[idx].secs),
                                  Q_ARG(QString, QString::fromLatin1(kAutoSleep[idx].label)));
    else if (idx == 0)
        emit logLine("sys", QStringLiteral("auto sleep: not managed by this app (camera keeps its CURRENT setting — nothing is restored)"));
}

void CameraController::setMicSleepIndex(int idx) {
    if (idx < 0 || idx > 2 || idx == m_settings.micSleepIdx) return;
    m_settings.micSleepIdx = idx;
    persist();
    emit settingsChanged();
    if (idx > 0 && connected())
        QMetaObject::invokeMethod(m_worker, "cmdSetMicSleep", Qt::QueuedConnection,
                                  Q_ARG(bool, idx == 2));
    else if (idx == 0)
        emit logLine("sys", QStringLiteral("mic during sleep: not managed by this app (camera keeps its CURRENT setting — nothing is restored)"));
}

// The index IS Device::DevTWSKeyType (0=Track, 1=Switch track, 2=Zoom 1x,
// 3=Record). The CAMERA owns this setting, so there is nothing to persist: we
// just send it, and the resulting DevTWSInfo readback updates the UI. Sending
// unconditionally is deliberate — re-picking the current segment is the user's
// way of saying "make it so" when the device disagrees with the selector.
void CameraController::setMicButtonAction(int idx) {
    idx = idx < 0 ? 0 : (idx > 3 ? 3 : idx);   // clamp to the fixed action table (0..3)
    if (!connected() || !m_capMicButton) {
        emit logLine("warn", QStringLiteral("mic button: camera does not expose the mic API"));
        return;
    }
    // Show the pick immediately. The camera's own readback trails the command,
    // so without this the selector springs back to the old segment and the user
    // has to click several times before it appears to take.
    m_micButtonTarget = idx;
    m_micButtonTimer->start();
    emit micChanged();
    QMetaObject::invokeMethod(m_worker, "cmdSetMicButtonAction", Qt::QueuedConnection,
                              Q_ARG(int, idx));
}

// ---------------------------------------------------------------------------
// The camera's own microphone (Tiny 3 mic array)
//
// None of this is persisted: the CAMERA owns every one of these settings and
// reports them back, so there is exactly one source of truth and no app-side
// copy to drift out of sync. Nothing is re-sent on connect either — one command
// per user action, per the AI-fault finding (a rapid burst of audio/AI/sleep
// writes put a Tiny 3 into a solid-red-LED fault where it ignored everything).
//
// Volume / mute / noise reduction / AGC answer their own getters immediately, so
// they have no optimistic override: the worker re-reads right after the set and
// the readback drives the UI. Source and pickup pattern have NO getter — their
// only readback is the status push — so those two carry the optimistic
// in-flight value with a give-up timer.
// ---------------------------------------------------------------------------
bool CameraController::audioReady(const QString &what) {
    if (connected() && m_capAudio) return true;
    emit logLine("warn", what + QStringLiteral(": the camera does not expose the audio API"));
    return false;
}

void CameraController::setAudioVolume(int volume) {
    if (!audioReady(QStringLiteral("audio volume"))) return;
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioVolume", Qt::QueuedConnection,
                              Q_ARG(int, volume < 0 ? 0 : (volume > 100 ? 100 : volume)));
}

void CameraController::setAudioMute(bool muted) {
    if (!audioReady(QStringLiteral("audio mute"))) return;
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioMute", Qt::QueuedConnection, Q_ARG(bool, muted));
}

// The SDK writes on/off and strength together, so each half of the UI sends the
// pair — keeping whatever the camera last reported for the other half. Before
// the camera has ever reported a level, use the middle of the documented range
// rather than 0, which is outside it.
void CameraController::setAudioNoiseReduce(bool on) {
    if (!audioReady(QStringLiteral("audio noise reduction"))) return;
    const int level = m_audioNoiseLevel > 0 ? m_audioNoiseLevel : kAudioNoiseLevelDefault;
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioNoiseReduce", Qt::QueuedConnection,
                              Q_ARG(bool, on), Q_ARG(int, level));
}

void CameraController::setAudioNoiseLevel(int level) {
    if (!audioReady(QStringLiteral("audio noise reduction"))) return;
    // Changing the strength must not silently switch noise reduction ON — if the
    // camera says it is off, the level is written but the state is preserved.
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioNoiseReduce", Qt::QueuedConnection,
                              Q_ARG(bool, m_audioNoiseReduce == 1),
                              Q_ARG(int, level < 1 ? 1 : (level > 10 ? 10 : level)));
}

void CameraController::setAudioAgc(bool on) {
    if (!audioReady(QStringLiteral("audio agc"))) return;
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioAgc", Qt::QueuedConnection, Q_ARG(bool, on));
}

void CameraController::setAudioMode(int mode) {
    if (!audioReady(QStringLiteral("audio pickup"))) return;
    if (mode < 0 || mode > 5) return;   // Device::AudioModeType 0..5 (AudioModeButt = 6)
    // Show the pick now; the camera only confirms it on the next status push.
    m_audioModeTarget = mode;
    m_audioModeTimer->start();
    emit audioChanged();
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioMode", Qt::QueuedConnection, Q_ARG(int, mode));
}

// Pinning a source turns the camera's own arbitration OFF — the worker does both
// in one command because doing only the second half is silently undone by the
// hardware. Reflect that here too, so the "Auto" segment lets go immediately
// instead of staying lit next to a manual choice.
void CameraController::setAudioSource(int source) {
    if (!audioReady(QStringLiteral("audio source"))) return;
    if (source < 0 || source > 4) return;   // Device::DevAudioSourceType 0..4
    m_audioSourceTarget = source;
    m_audioSourceTimer->start();
    m_audioAuto = 0;   // corrected by the cameraGetAudioSelectR readback moments later
    emit audioChanged();
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioSource", Qt::QueuedConnection, Q_ARG(int, source));
}

// Hand source selection back to the camera. No optimistic value: is_auto has a
// real getter and the worker re-reads it straight after the set.
void CameraController::setAudioAuto(bool on) {
    if (!audioReady(QStringLiteral("audio auto source"))) return;
    // A manual pick that is still in flight is now moot — the camera is about to
    // choose for itself, so stop claiming the user's source will stick.
    if (on && m_audioSourceTarget >= 0) {
        m_audioSourceTarget = -1;
        m_audioSourceTimer->stop();
    }
    QMetaObject::invokeMethod(m_worker, "cmdSetAudioAuto", Qt::QueuedConnection, Q_ARG(bool, on));
}

// Push the MANAGED power/sleep settings ("Device"=0 is never sent) to the
// camera after the usual settle delay — the device ACKs-but-ignores config
// sent too early after connect or while asleep. Called from a genuine connect
// and from every Asleep→Awake edge (review finding #1: the connect-only leg
// silently missed the launched-while-dozing case, the most common state for a
// camera whose owner cares about mic-during-sleep).
void CameraController::applyPowerSettings(const QString &why) {
    if (m_settings.autoSleepIdx <= 0 && m_settings.micSleepIdx <= 0) return;
    QTimer::singleShot(kAiReturnDelayMs, this, [this, why]() {
        if (!connected()) return;
        if (asleep()) return;   // the wake edge will call us again
        const int as = m_settings.autoSleepIdx;
        if (as > 0 && as <= 5)
            QMetaObject::invokeMethod(m_worker, "cmdSetAutoSleep", Qt::QueuedConnection,
                                      Q_ARG(int, kAutoSleep[as].secs),
                                      Q_ARG(QString, QString::fromLatin1(kAutoSleep[as].label)));
        if (m_settings.micSleepIdx > 0)
            QMetaObject::invokeMethod(m_worker, "cmdSetMicSleep", Qt::QueuedConnection,
                                      Q_ARG(bool, m_settings.micSleepIdx == 2));
        emit logLine("sys", QStringLiteral("power settings re-applied (%1)").arg(why));
    });
}

void CameraController::resetImageDefaults() {
    setImageParam(QStringLiteral("brightness"), 50);
    setImageParam(QStringLiteral("contrast"), 50);
    setImageParam(QStringLiteral("saturation"), 50);
    setImageParam(QStringLiteral("sharpness"), 50);
    emit logLine("cmd", QStringLiteral("image: reset to defaults (50)"));
}
void CameraController::setFovIndex(int idx) {
    if (idx < 0 || idx > 2) return;
    if (idx != m_settings.fovIndex) {
        m_settings.fovIndex = idx;
        persist();
        emit settingsChanged();
    }
    if (connected())
        QMetaObject::invokeMethod(m_worker, "cmdSetFov", Qt::QueuedConnection,
                                  Q_ARG(int, idx), Q_ARG(QString, QString::fromUtf8(kFovLabels[idx])));
}
void CameraController::setAiReturnPreset(int p) {
    if (p < 0 || p > 3 || p == m_settings.aiReturnPreset) return;
    m_settings.aiReturnPreset = p;
    persist();
    emit settingsChanged();
}
void CameraController::setPreviewResIndex(int idx) {
    if (idx < 0 || idx >= kPreviewResCount || idx == m_settings.previewResIndex) return;
    m_settings.previewResIndex = idx;
    persist();
    // The EMBEDDED preview restarts at the new mode via main.cpp syncing
    // PreviewEngine off this signal.
    emit settingsChanged();
    // The ffplay FALLBACK (if open) is reloaded here — it can't switch mid-stream.
    if (m_previewProc)
        launchPreview();
}
void CameraController::setSleepOnExit(bool on) {
    if (on == m_settings.sleepOnExit) return;
    m_settings.sleepOnExit = on;
    persist();
    emit settingsChanged();
}

void CameraController::setGestureLowTraffic(bool on) {
    if (on == m_settings.gestureLowTraffic) return;
    m_settings.gestureLowTraffic = on;
    persist();
    emit settingsChanged();
    // Live-apply: if gesture control is currently on, switch the worker's
    // cadence immediately (no need to re-toggle the gesture chip).
    if (connected())
        QMetaObject::invokeMethod(m_worker, "setGestureFriendly", Qt::QueuedConnection,
                                  Q_ARG(bool, m_gesture && on));
    emit logLine("sys", on
        ? QStringLiteral("gesture low-traffic mode ENABLED (experimental) — applies while gesture control is on")
        : QStringLiteral("gesture low-traffic mode disabled — normal status cadence"));
}

// ---------------------------------------------------------------------------
// User actions
// ---------------------------------------------------------------------------
void CameraController::wake() { QMetaObject::invokeMethod(m_worker, "cmdWake", Qt::QueuedConnection); }
void CameraController::sleep() { QMetaObject::invokeMethod(m_worker, "cmdSleep", Qt::QueuedConnection); }
void CameraController::center() { QMetaObject::invokeMethod(m_worker, "cmdCenter", Qt::QueuedConnection); }

void CameraController::nudge(int dir) {
    QMetaObject::invokeMethod(m_worker, "cmdNudge", Qt::QueuedConnection,
                              Q_ARG(int, dir),
                              Q_ARG(double, static_cast<double>(m_settings.moveStepDeg)),
                              Q_ARG(double, speedValue()));
}

void CameraController::zoomIn() {
    QMetaObject::invokeMethod(m_worker, "cmdZoom", Qt::QueuedConnection,
                              Q_ARG(double, 0.1), Q_ARG(bool, false), Q_ARG(double, 0.0),
                              Q_ARG(QString, QStringLiteral("zoom in")));
}
void CameraController::zoomOut() {
    QMetaObject::invokeMethod(m_worker, "cmdZoom", Qt::QueuedConnection,
                              Q_ARG(double, -0.1), Q_ARG(bool, false), Q_ARG(double, 0.0),
                              Q_ARG(QString, QStringLiteral("zoom out")));
}
void CameraController::zoomReset() {
    // Absolute reset — succeeds even if the getter momentarily fails (#10).
    QMetaObject::invokeMethod(m_worker, "cmdZoom", Qt::QueuedConnection,
                              Q_ARG(double, 0.0), Q_ARG(bool, true), Q_ARG(double, 1.0),
                              Q_ARG(QString, QStringLiteral("zoom 1x")));
}

void CameraController::setAiTracking(bool on) {
    if (on == aiTracking()) { emit aiChanged(); return; }
    m_targetTracking = on;
    m_targetFace = faceFocus();      // preserve the independent face-focus leg's display
    ++m_aiInFlight;
    m_aiPending = true;              // suppress status-push fighting until confirmed (#8)
    m_pendingTimer->start();
    QMetaObject::invokeMethod(m_worker, "cmdSetAi", Qt::QueuedConnection,
                              Q_ARG(int, on ? kAiHuman : kAiNone), Q_ARG(int, 0),
                              Q_ARG(QString, on ? QStringLiteral("ai track on")
                                                : QStringLiteral("ai track off")));
    emit aiChanged();
}

void CameraController::setFaceFocus(bool on) {
    if (on == m_faceFocus && !m_aiPending) { emit aiChanged(); return; }
    m_targetFace = on;
    m_targetTracking = aiTracking();     // preserve the AI-track leg's display
    ++m_aiInFlight;
    m_aiPending = true;
    m_pendingTimer->start();
    QMetaObject::invokeMethod(m_worker, "cmdSetFace", Qt::QueuedConnection, Q_ARG(bool, on));
    emit aiChanged();
}

void CameraController::gimbalVelocity(double pitchFrac, double yawFrac) {
    pitchFrac = pitchFrac < -1.0 ? -1.0 : (pitchFrac > 1.0 ? 1.0 : pitchFrac);
    yawFrac = yawFrac < -1.0 ? -1.0 : (yawFrac > 1.0 ? 1.0 : yawFrac);
    const double maxSpeed = speedValue();   // reuse the Slow/Medium reference speed, deg/s
    QMetaObject::invokeMethod(m_worker, "cmdGimbalVelocity", Qt::QueuedConnection,
                              Q_ARG(double, pitchFrac * maxSpeed), Q_ARG(double, yawFrac * maxSpeed));
}

void CameraController::gimbalStop() {
    QMetaObject::invokeMethod(m_worker, "cmdGimbalStop", Qt::QueuedConnection);
}

void CameraController::setGesture(bool on) {
    m_targetGesture = on;
    m_gesture = on;   // optimistic; reverted in onWorkerResult on failure
    m_settings.gesture = on;
    persist();
    QMetaObject::invokeMethod(m_worker, "cmdSetGesture", Qt::QueuedConnection,
                              Q_ARG(bool, on), Q_ARG(bool, m_settings.gestureLowTraffic));
    emit aiChanged();
}

void CameraController::setHdr(bool on) {
    if (!m_hdrSupport) {
        emit logLine("warn", QStringLiteral("hdr: device reports HDR not supported in this mode"));
        return;
    }
    m_targetHdr = on;
    m_hdrOn = on;        // optimistic; onWorkerResult confirms/reverts
    m_hdrPending = true; // suppress the status push clobbering the optimistic value (#8)
    QMetaObject::invokeMethod(m_worker, "cmdSetHdr", Qt::QueuedConnection, Q_ARG(bool, on));
    emit imageChanged();
}

void CameraController::setImageParam(const QString &param, int value) {
    // Update the local mirror immediately for responsive UI; the device call
    // follows. (These params are not in the status push, so the mirror is the
    // source of truth; a getter re-read on connect keeps it honest.)
    if (param == QLatin1String("brightness")) m_brightness = value;
    else if (param == QLatin1String("contrast")) m_contrast = value;
    else if (param == QLatin1String("saturation")) m_saturation = value;
    else if (param == QLatin1String("sharpness")) m_sharpness = value;
    emit imageChanged();
    QMetaObject::invokeMethod(m_worker, "cmdSetImage", Qt::QueuedConnection,
                              Q_ARG(QString, param), Q_ARG(int, value));
}

void CameraController::gestureQuietTest() {
    // Diagnostic (hardware finding): the camera does gestures autonomously with
    // no app attached but goes gesture-deaf while this app runs. This pauses
    // ALL periodic SDK traffic for 60 s to find out whether the traffic or the
    // mere session suppresses the recognizer.
    QMetaObject::invokeMethod(m_worker, "cmdQuietMode", Qt::QueuedConnection, Q_ARG(int, 60));
}

void CameraController::rescan() {
    m_connState = Discovering;
    emit connStateChanged();
    QMetaObject::invokeMethod(m_worker, "rescan", Qt::QueuedConnection, Q_ARG(int, 6000));
}

void CameraController::launchPreview() {
    // FALLBACK path: real frames in a SEPARATE ffplay window. Kept alongside the
    // embedded preview (PreviewEngine) for boxes where QtMultimedia misbehaves.
    // Occupies the UVC node — conflicts with the embedded preview and with
    // browser/Meet/OBS camera use, exactly like any other capture client.
    if (!m_previewAvailable) {
        emit logLine("warn", QStringLiteral("preview: ffplay or /dev/video0 not available"));
        return;
    }
    stopPreview();   // kill any existing preview first (also used to reload on res change)

    const int ri = (m_settings.previewResIndex >= 0 && m_settings.previewResIndex < kPreviewResCount)
                       ? m_settings.previewResIndex : 0;
    const PreviewRes &pr = kPreviewRes[ri];

    // Run ffplay directly (not via a shell, not detached) so the process is a
    // managed child we can terminate cleanly. -loglevel error silences the
    // harmless per-frame MJPEG/swscaler warnings ("EOI missing", "deprecated
    // pixel format") the camera's MJPEG stream produces.
    m_previewProc = new QProcess(this);
    connect(m_previewProc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (m_previewProc) { m_previewProc->deleteLater(); m_previewProc = nullptr; }
        emit logLine("sys", QStringLiteral("preview: ffplay closed"));
    });
    const QStringList args = {
        "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer",
        "-f", "v4l2", "-input_format", "mjpeg",
        "-video_size", QStringLiteral("%1x%2").arg(pr.w).arg(pr.h),
        "-framerate", QString::number(pr.fps),
        "-window_title", QStringLiteral("OBSBOT preview (%1)").arg(QString::fromLatin1(pr.label)),
        "/dev/video0"};
    m_previewProc->start(QStringLiteral("ffplay"), args);
    emit logLine("cmd", QStringLiteral("preview: launched ffplay at %1 (external window, fallback)")
                            .arg(QString::fromLatin1(pr.label)));
}

void CameraController::copyToClipboard(const QString &text) {
    if (auto *cb = QGuiApplication::clipboard()) {
        cb->setText(text);
        emit logLine("sys", QStringLiteral("log copied to clipboard (%1 chars)").arg(text.size()));
    }
}

void CameraController::stopPreview() {
    if (!m_previewProc) return;
    QProcess *p = m_previewProc;
    m_previewProc = nullptr;
    disconnect(p, nullptr, this, nullptr);   // don't let the finished-lambda re-fire during teardown
    p->terminate();
    if (!p->waitForFinished(800)) {
        p->kill();
        // Wait for the kill to land too — callers may start the EMBEDDED
        // preview immediately after this returns, and a still-dying ffplay
        // would hold the UVC node and bounce it with "device busy".
        p->waitForFinished(500);
    }
    p->deleteLater();
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
void CameraController::savePreset(int idx) {
    if (idx < 0 || idx > 2) return;
    QMetaObject::invokeMethod(m_worker, "cmdPresetCapture", Qt::QueuedConnection, Q_ARG(int, idx));
}
void CameraController::goPreset(int idx) {
    if (idx < 0 || idx > 2) return;
    const PresetData &p = m_settings.presets[idx];
    if (!p.set) {
        emit logLine("warn", QStringLiteral("preset %1: empty — Save first").arg(idx + 1));
        return;
    }
    QMetaObject::invokeMethod(m_worker, "cmdPresetGo", Qt::QueuedConnection,
                              Q_ARG(int, idx), Q_ARG(double, p.tilt), Q_ARG(double, p.pan),
                              Q_ARG(double, p.zoom), Q_ARG(int, p.fov), Q_ARG(double, speedValue()));
    // The preset re-applies its SAVED FOV on the device (cmdPresetGo →
    // cameraSetFovU) — keep the FOV selector in sync instead of silently
    // diverging. Rex's "Wide/Med show the same" bug: the startup/wake/AI-return
    // preset had restored the preset's FOV behind the selector's back, so the
    // first click on the already-active FOV looked like a no-op and only the
    // second click (a real change) visibly worked.
    // Synced in onWorkerResult ON SUCCESS ONLY (review finding: syncing here,
    // before knowing whether cmdPresetGo refuses — AI owns gimbal, no device —
    // desynced AND persisted a FOV the camera never got).
    m_pendingGoFov = (p.fov >= 0 && p.fov <= 2) ? p.fov : -1;
}
void CameraController::clearPreset(int idx) {
    if (idx < 0 || idx > 2) return;
    m_settings.presets[idx] = PresetData{};
    persist();
    emit presetsChanged();
    emit logLine("cmd", QStringLiteral("preset %1 cleared").arg(idx + 1));
}
void CameraController::renamePreset(int idx, const QString &name) {
    if (idx < 0 || idx > 2) return;
    m_settings.presets[idx].name = name;
    persist();
    emit presetsChanged();
}
void CameraController::saveCurrentToNextEmpty() {
    for (int i = 0; i < 3; ++i) {
        if (!m_settings.presets[i].set) { savePreset(i); return; }
    }
    emit logLine("warn", QStringLiteral("presets: all slots full — overwrite one instead"));
}

// ---------------------------------------------------------------------------
// Wireless mic (Vox SE) — pairing
//
// The Vox SE links to the CAMERA, not to this app: the mic must be held in its
// own pairing mode (button ~6 s, light flashing green) while the camera has the
// slot open. The worker also wakes the camera first — a sleeping Tiny 3 ACKs the
// pair command with rc=0 and never scans, which is what made this look
// unsupported at first. Success shows up as the slot going online in the status
// push, not as the command's rc.
// ---------------------------------------------------------------------------
// Start pairing. There is deliberately NO slot argument: hardware testing showed
// the camera ignores the DevTXType it is given and links the incoming mic to
// whichever slot it likes (asking to re-pair slot 1 put the mic in slot 2). The
// UI therefore offers one "Pair a mic" action rather than pretending the choice
// exists, and we pass DevTX1 purely because the SDK call requires some value.
void CameraController::micPair() {
    micPairBusyCue(kMicPairWaitMaxMs);
    QMetaObject::invokeMethod(m_worker, "cmdTxPair", Qt::QueuedConnection,
                              Q_ARG(int, 1), Q_ARG(bool, true));
}

void CameraController::micClearPairing(int tx) {
    if (tx != 1 && tx != 2) return;
    micPairBusyCue(kMicPairCueMaxMs);
    QMetaObject::invokeMethod(m_worker, "cmdTxClear", Qt::QueuedConnection, Q_ARG(int, tx));
}

// Per-mic mute. No optimistic value: the worker re-reads the device on the same
// round trip (cameraTXGetAudioMuteR plus a fresh DevTWSInfo), so the readback
// lands well inside the time a user could notice — unlike the source/pattern
// pickers, whose only readback is the 2–3 s status push.
void CameraController::setMicMute(int tx, bool muted) {
    if (tx != 1 && tx != 2) return;
    if (!connected() || !capMicGain()) {
        emit logLine("warn", QStringLiteral("mic mute tx%1: the camera does not expose the per-mic "
                                            "mic API — mute is shown, but cannot be changed here").arg(tx));
        return;
    }
    QMetaObject::invokeMethod(m_worker, "cmdSetTxMute", Qt::QueuedConnection,
                              Q_ARG(int, tx), Q_ARG(bool, muted));
}

// Per-mic gain, RELATIVE to what the camera reported.
//
// This is deliberately a stepper and not a slider. The SDK documents no range for
// wireless-mic gain — the only hard fact is that DevTWSInfo carries it back in an
// int8_t — so a 0–100 slider would be inventing a scale, and worse, would jump
// the mic to an arbitrary level the moment it was touched. Stepping the device's
// own value keeps every number on screen one the camera actually said.
//
// Refusing to step before the camera has reported a gain is the same honesty: we
// would otherwise have to guess a starting point.
void CameraController::nudgeMicGain(int tx, int delta) {
    if ((tx != 1 && tx != 2) || delta == 0) return;
    if (!connected() || !capMicGain()) {
        emit logLine("warn", QStringLiteral("mic gain tx%1: the camera does not expose the per-mic "
                                            "mic API — gain is shown, but cannot be changed here").arg(tx));
        return;
    }
    const MicTx &t = m_micTx[tx - 1];
    if (!t.online) {
        emit logLine("warn", QStringLiteral("mic gain tx%1: no mic connected to that slot").arg(tx));
        return;
    }
    if (!t.gainKnown) {
        emit logLine("warn", QStringLiteral("mic gain tx%1: the camera has not reported a gain yet — "
                                            "nothing to step from").arg(tx));
        return;
    }
    // Step from the value the user can SEE (the pending one if a step is still
    // in flight), so held or rapid presses accumulate instead of each recomputing
    // from the last value the camera happened to report.
    MicTx &slot = m_micTx[tx - 1];
    const int base = slot.gainPending ? slot.gainTarget : slot.gain;
    slot.gainTarget = base + delta * kMicGainStep;
    slot.gainPending = true;
    m_micGainTimer->start();
    emit micChanged();
    QMetaObject::invokeMethod(m_worker, "cmdSetTxGain", Qt::QueuedConnection,
                              Q_ARG(int, tx), Q_ARG(int, slot.gainTarget));
}

// Busy cue for an in-flight pair/clear.
//
// For PAIRING the wait is genuinely long: the camera accepts the request in
// milliseconds, but the link only forms once the user has fetched the mic and
// held its button for ~6 s. Clearing the cue on the command's result therefore
// made the button look dead — it blinked once and gave up while the camera was
// still listening. The cue now survives until a slot actually comes online (see
// onMicStatus), a failure comes back, or the long timeout expires.
void CameraController::micPairBusyCue(int timeoutMs) {
    m_micPairBusy = true;
    ++m_micPairCueGen;
    const quint64 gen = m_micPairCueGen;
    emit micChanged();
    QTimer::singleShot(timeoutMs, this, [this, gen]() {
        // Only the newest cue may time itself out; a later press supersedes it.
        if (gen != m_micPairCueGen || !m_micPairBusy) return;
        m_micPairBusy = false;
        emit logLine("warn", QStringLiteral("pairing: no mic appeared — hold the mic's button "
                                            "for about 6 s while pairing is open"));
        emit micChanged();
    });
}

// ---------------------------------------------------------------------------
// Worker signal handlers (GUI thread)
// ---------------------------------------------------------------------------
void CameraController::onConnectionResolved(bool found, const QString &product, const QString &sn,
                                            const QString &fw, const QString &mode, int enumId) {
    if (found) {
        m_connState = Connected;
        m_product = product; m_sn = sn; m_firmware = fw; m_mode = mode; m_enumId = enumId;
        emit identityChanged();
        // Align the device's FOV with the restored UI setting (benign, no motion).
        QMetaObject::invokeMethod(m_worker, "cmdSetFov", Qt::QueuedConnection,
                                  Q_ARG(int, m_settings.fovIndex),
                                  Q_ARG(QString, QString::fromUtf8(kFovLabels[m_settings.fovIndex])));
        // Re-apply the persisted gesture-control choice to the DEVICE. Restoring
        // it only into the UI (constructor) left the chip showing ON while the
        // camera actually had gesture off — gestures "didn't detect" all session
        // unless the user re-toggled (Rex's hardware finding). rc is logged.
        // m_targetGesture must match, or the failure leg in onWorkerResult would
        // "revert" to !false and confirm the chip ON — the very bug being fixed.
        // Delayed like the startup preset: right at connect the device still
        // eats commands (rc=0 but no effect) — the same too-early window that
        // forces kAiReturnDelayMs and the 1600 ms preset settle.
        if (m_gesture) {
            QTimer::singleShot(kAiReturnDelayMs, this, [this]() {
                if (!connected() || !m_gesture) return;
                m_targetGesture = true;
                QMetaObject::invokeMethod(m_worker, "cmdSetGesture", Qt::QueuedConnection,
                                          Q_ARG(bool, true), Q_ARG(bool, m_settings.gestureLowTraffic));
            });
        }
        // Re-apply managed power/sleep settings on a GENUINE connect only —
        // a rescan-while-connected re-bind must not re-send them (review
        // finding #4: re-sending the suspend time restarts the camera's idle
        // countdown, so periodic rescans would keep it awake forever).
        if (!m_hadDevice)
            applyPowerSettings(QStringLiteral("connect"));
        // Startup preset: move to the chosen preset on a GENUINE connect only —
        // after a delay so the gimbal's power-on centering finishes first (see
        // scheduleStartupPreset). A Rescan while already connected re-binds the
        // same device and re-emits connectionResolved(true); m_hadDevice guards
        // against re-firing the preset (and moving the gimbal) in that case.
        if (!m_hadDevice)
            scheduleStartupPreset(QStringLiteral("startup"));
        m_hadDevice = true;
    } else {
        m_connState = Disconnected;
    }
    emit connStateChanged();
    emit statusChanged();
    emit discoveryFinished(found);
}

void CameraController::onDeviceLost(const QString &reason) {
    m_connState = Disconnected;
    m_runState = RunUnknown;
    m_aiTracking = false;
    m_faceFocus = false;
    m_hdrOn = false;
    m_hdrSupport = false;
    m_hdrPending = false;
    m_aiPending = false;
    m_aiInFlight = 0;
    m_aiEngageTime.invalidate();
    m_micSleepDevice = -1;    // readback is unknown with no device (honesty)
    m_autoSleepDevice = -1;
    m_hadDevice = false;   // a real loss: the next connect is genuine → preset re-fires
    m_zoomValid = false;
    m_micTx[0] = MicTx{};   // no device: transmitter state is unknown again (honesty)
    m_micTx[1] = MicTx{};
    m_micTwsMode = false;
    m_micPairing = false;
    m_micScanning = false;
    m_micPairBusy = false;
    m_micPairRecord = -1;
    m_micButtonDevice = -1;
    m_micButtonTarget = -1;   // a reconnect must not resurrect a stale optimistic pick
    if (m_micButtonTimer) m_micButtonTimer->stop();
    if (m_micTipTimer) m_micTipTimer->stop();
    m_capMicButton = false;   // re-probed on the next bind, never assumed
    m_capMicGain = false;
    // The AI-fault latch is a property of the DEVICE, so losing the device makes
    // it unknown again — and "unknown" here means "not faulted", because keeping
    // a red banner up for a camera that is no longer attached would be a claim we
    // cannot support. devEventsSeen resets for the same reason: whether the NEXT
    // camera pushes events is a fresh question.
    m_deviceFault = false;
    m_deviceFaultReason.clear();
    m_devEventsSeen = false;
    // Built-in audio: with no device every value is unknown again (honesty), and
    // no optimistic pick may survive into the next connection.
    m_audioVolume = -1;
    m_audioMuted = -1;
    m_audioNoiseReduce = -1;
    m_audioNoiseLevel = -1;
    m_audioAgc = -1;
    m_audioAuto = -1;
    m_audioSourceDevice = -1;
    m_audioModeDevice = -1;
    m_audioSourceTarget = -1;
    m_audioModeTarget = -1;
    if (m_audioSourceTimer) m_audioSourceTimer->stop();
    if (m_audioModeTimer) m_audioModeTimer->stop();
    m_capAudio = false;       // re-probed on the next bind, never assumed
    emit connStateChanged();
    emit statusChanged();
    emit aiChanged();
    emit imageChanged();
    emit zoomChanged();
    emit micChanged();
    emit audioChanged();
    emit faultChanged();
    emit logLine("warn", QStringLiteral("device lost: %1 — controls disabled").arg(reason));
}

void CameraController::onStatusUpdate(int runState, int aiModeRaw, double zoom, bool zoomValid) {
    const int prevRun = m_runState;
    if (runState != m_runState) { m_runState = runState; emit statusChanged(); }
    // Wake edge (Asleep → Awake): re-apply the startup preset. The camera
    // re-centers its gimbal on wake, so this restores the user's chosen position
    // after they wake it — mirroring the on-connect behavior.
    if (prevRun == Asleep && runState == Awake) {
        scheduleStartupPreset(QStringLiteral("wake"));
        // Re-apply gesture control on wake too. Hardware finding (the "flaky
        // gesture" history): the device stops honoring gesture across
        // sleep/wake in some sessions — and in one logged session the camera
        // was still ASLEEP when the connect-time re-apply fired, so the
        // setting was accepted (rc=0, readback on) but never took effect.
        // Same settle delay as the preset; state re-checked at fire time.
        if (m_gesture) {
            QTimer::singleShot(kAiReturnDelayMs, this, [this]() {
                if (!connected() || !m_gesture || asleep()) return;
                m_targetGesture = true;
                QMetaObject::invokeMethod(m_worker, "cmdSetGesture", Qt::QueuedConnection,
                                          Q_ARG(bool, true), Q_ARG(bool, m_settings.gestureLowTraffic));
            });
        }
        // Managed power/sleep settings survive the same ACK-while-asleep trap —
        // re-apply on every wake edge (mic-during-sleep especially: sent while
        // dozing it returns rc=0 and never takes effect).
        applyPowerSettings(QStringLiteral("wake"));
    }

    m_zoom = zoom; m_zoomValid = zoomValid; emit zoomChanged();

    m_aiModeRaw = aiModeRaw;
    m_aiModeName = decodeAiMode(aiModeRaw);
    // Resync confirmed AI Track from the device UNLESS a toggle is in flight (#8)
    // or the device is mid-switch (#5). Any settled ai_mode > None means tracking
    // is engaged (AI Track owns the gimbal).
    if (!m_aiPending && aiModeRaw != kAiSwitching) {
        const bool was = m_aiTracking;
        m_aiTracking = (aiModeRaw > kAiNone);
        // Device-side disengage right after we enabled tracking: the Tiny 3
        // accepts AI-Track ON (rc=0) but silently drops back to None when it
        // can't lock onto a person. Without this hint the chip just flips off
        // and the user is left guessing — say what happened instead.
        if (was && !m_aiTracking
            && m_aiEngageTime.isValid() && m_aiEngageTime.elapsed() < kAiDisengageHintMs) {
            emit logLine("warn", QStringLiteral(
                "ai track: device disengaged itself right after enabling — it needs a "
                "person in view to lock on. Aim the camera at yourself and try again. "
                "If it never engages AND the ring LED is solid red, the camera's AI "
                "subsystem has faulted: unplug it and plug it back in."));
        }
        if (!m_aiTracking) m_aiEngageTime.invalidate();
    }
    emit aiChanged();
}

void CameraController::onAuxStatus(bool faceFocus, bool hdrOn, bool hdrSupport, int fps, int sleepMicro,
                                   int autoSleepSec) {
    // Face autofocus + HDR are reported by the device; sync them honestly (unless
    // an AI toggle is mid-flight, in which case the optimistic target stands).
    if (!m_aiPending) m_faceFocus = faceFocus;
    if (!m_hdrPending) m_hdrOn = hdrOn;   // don't clobber an in-flight HDR toggle (#8)
    m_hdrSupport = hdrSupport;
    if (fps != m_fps) { m_fps = fps; emit statusChanged(); }
    if (sleepMicro != m_micSleepDevice) { m_micSleepDevice = sleepMicro; emit statusChanged(); }
    if (autoSleepSec != m_autoSleepDevice) { m_autoSleepDevice = autoSleepSec; emit statusChanged(); }
    emit aiChanged();
    emit imageChanged();
}

void CameraController::onImageParams(int brightness, int contrast, int saturation, int sharpness) {
    m_brightness = brightness;
    m_contrast = contrast;
    m_saturation = saturation;
    m_sharpness = sharpness;
    emit imageChanged();
}

void CameraController::onZoomUpdate(double zoom, bool valid) {
    m_zoom = zoom; m_zoomValid = valid;
    emit zoomChanged();
}

void CameraController::onWorkerResult(const QString &action, bool ok, int /*rc*/, const QString &message) {
    // AI Track leg confirmation: apply target on success, discard on failure (#5/#7).
    if (action.startsWith("ai track")) {
        if (ok) m_aiTracking = m_targetTracking;
        // Arm the device-disengage detector on a confirmed ON (see onStatusUpdate);
        // a deliberate OFF disarms it so it can't fire a bogus hint.
        if (ok && action.endsWith("on") && m_targetTracking) m_aiEngageTime.start();
        else if (action.endsWith("off"))                     m_aiEngageTime.invalidate();
        if (--m_aiInFlight <= 0) { m_aiInFlight = 0; m_aiPending = false; m_pendingTimer->stop(); }
        emit aiChanged();
        // "After AI off, go to preset": when turning AI Track OFF is confirmed and
        // the user chose a return preset, recall it — after a settle delay. The
        // gimbal is still physically disengaging from AI for ~1 s after the off
        // command is accepted; an immediate move gets eaten or truncated (Rex's
        // hardware finding: "moves a tiny bit" / doesn't reach the preset). Same
        // fix pattern as the startup-preset delay. Re-checked at fire time in
        // case AI was switched back on during the wait.
        const int rp = m_settings.aiReturnPreset;
        if (ok && action.endsWith("off") && !m_targetTracking
            && rp >= 1 && rp <= 3 && m_settings.presets[rp - 1].set) {
            emit logLine("cmd", QStringLiteral("ai off: returning to preset %1 in %2 ms (gimbal settle)")
                                    .arg(rp).arg(kAiReturnDelayMs));
            QTimer::singleShot(kAiReturnDelayMs, this, [this, rp]() {
                if (!connected()) return;
                if (asleep()) {
                    emit logLine("warn", QStringLiteral("ai off: return to preset %1 skipped — camera asleep").arg(rp));
                    return;
                }
                if (aiTracking()) {
                    emit logLine("warn", QStringLiteral("ai off: return to preset %1 skipped — AI is on again").arg(rp));
                    return;
                }
                goPreset(rp - 1);
            });
        }
    } else if (action == QLatin1String("face focus")) {
        if (ok) m_faceFocus = m_targetFace;
        if (--m_aiInFlight <= 0) { m_aiInFlight = 0; m_aiPending = false; m_pendingTimer->stop(); }
        emit aiChanged();
    } else if (action == QLatin1String("gesture")) {
        // Revert the DISPLAY on failure — but keep the persisted intent
        // (review finding: persisting the revert let a failed AUTOMATIC
        // re-apply — the delayed connect/wake timer racing an unplug —
        // silently erase the user's saved gesture=on, so the next connect
        // never re-applied it). setGesture() persists the user's choice at
        // click time; a transient failure must not overwrite it.
        if (!ok) m_gesture = !m_targetGesture;
        emit aiChanged();
    } else if (action.startsWith(QLatin1String("preset")) && action.endsWith(QLatin1String("go"))) {
        // FOV selector sync for a preset recall — on SUCCESS only (see goPreset).
        if (ok && m_pendingGoFov >= 0 && m_pendingGoFov != m_settings.fovIndex) {
            m_settings.fovIndex = m_pendingGoFov;
            persist();
            emit settingsChanged();
        }
        m_pendingGoFov = -1;
    } else if (action == QLatin1String("hdr")) {
        if (ok) m_hdrOn = m_targetHdr;
        else    m_hdrOn = !m_targetHdr;   // revert
        m_hdrPending = false;             // toggle settled: allow status resync again
        emit imageChanged();
    } else if (action.startsWith(QLatin1String("mic pair"))
               || action.startsWith(QLatin1String("mic clear"))) {
        // ok==true only means the camera ACCEPTED the request. For pairing the
        // link is confirmed later, by a slot going online in onMicStatus, so a
        // successful "mic pair" must leave the cue running — otherwise the UI
        // stops indicating anything while the camera is still listening.
        const bool isPair = action.startsWith(QLatin1String("mic pair"));
        if (m_micPairBusy && (!isPair || !ok)) { m_micPairBusy = false; emit micChanged(); }
    }
    emit commandResult(action, ok, message);
}

void CameraController::onPresetCaptured(int idx, double pitch, double yaw, double zoom, int fov) {
    if (idx < 0 || idx > 2) return;
    PresetData &p = m_settings.presets[idx];
    const QString name = p.name.isEmpty() ? QStringLiteral("Preset %1").arg(idx + 1) : p.name;
    p.set = true;
    p.name = name;
    p.pan = yaw;      // pan  = yaw
    p.tilt = pitch;   // tilt = pitch
    p.zoom = zoom;
    p.fov = (fov < 0) ? m_settings.fovIndex : fov;
    persist();
    emit presetsChanged();
}

// Presence, from the camera's own status push (tiny.wireless_mic). Authoritative
// and free — nothing is polled for it.
void CameraController::onMicStatus(bool tx1Online, bool tx2Online, bool twsMode,
                                   bool pairing, bool scanning, int audioSource, int audioMode) {
    // The same push also carries the camera's own audio source and pickup
    // pattern (tiny.audio_mode), which is the ONLY readback either of them has.
    // Handled first and separately: an audio change with no mic change must
    // still reach the UI, and vice versa.
    bool audioDirty = false;
    if (audioSource != m_audioSourceDevice) {
        m_audioSourceDevice = audioSource;
        audioDirty = true;
    }
    if (audioMode != m_audioModeDevice) {
        m_audioModeDevice = audioMode;
        audioDirty = true;
    }
    // The device agreeing with our pick is what ends the optimistic window — the
    // command's rc never was proof (with the camera's own source arbitration on,
    // a source pick is accepted and then reverted).
    if (m_audioSourceTarget >= 0 && m_audioSourceDevice == m_audioSourceTarget) {
        m_audioSourceTarget = -1;
        m_audioSourceTimer->stop();
        audioDirty = true;
    }
    if (m_audioModeTarget >= 0 && m_audioModeDevice == m_audioModeTarget) {
        m_audioModeTarget = -1;
        m_audioModeTimer->stop();
        audioDirty = true;
    }
    if (audioDirty) emit audioChanged();

    if (m_micTx[0].online == tx1Online && m_micTx[1].online == tx2Online
        && m_micTwsMode == twsMode && m_micPairing == pairing && m_micScanning == scanning)
        return;   // status pushes every ~2-3 s; don't churn QML bindings for nothing
    // A slot coming online is the one moment a fresh battery reading is worth a
    // round trip — otherwise the 60 s refresh would leave it blank for a minute.
    const bool cameOnline = (tx1Online && !m_micTx[0].online) || (tx2Online && !m_micTx[1].online);
    // The link forming is what actually ends a pairing wait — not the command's
    // return code, which only says the camera started listening.
    if (cameOnline && m_micPairBusy) {
        m_micPairBusy = false;
        ++m_micPairCueGen;   // supersede the pending timeout
        emit logLine("ok", QStringLiteral("pairing: a mic came online"));
    }
    m_micTx[0].online = tx1Online;
    m_micTx[1].online = tx2Online;
    m_micTwsMode = twsMode;
    m_micPairing = pairing;
    m_micScanning = scanning;
    // A slot that just went offline has no battery, gain or mute state to report
    // any more — and no pending button cue that could still be about it.
    for (MicTx &t : m_micTx) {
        if (!t.online) {
            t.battery = -1;
            t.charging = false;
            t.muted = false;
            t.gainKnown = false;
            t.gain = 0;
            t.ns = -1;
            t.nsLevel = -1;
            t.tip.clear();
        }
    }
    emit micChanged();
    if (cameOnline && m_capMicButton && connected())
        QMetaObject::invokeMethod(m_worker, "cmdReadTwsInfo", Qt::QueuedConnection);
}

// Detail, from Device::cameraGetTWSInfoR (see ObsbotTwsCompat.h). `supported`
// is the runtime capability probe — false means the call is unavailable on this
// build/firmware and the UI must say so instead of showing invented zeroes.
void CameraController::onTwsInfo(bool supported, int keyCmd,
                                 int batt1, bool charging1, bool muted1,
                                 int batt2, bool charging2, bool muted2) {
    m_capMicButton = supported;
    if (!supported) {
        m_micButtonDevice = -1;
        for (MicTx &t : m_micTx) { t.battery = -1; t.charging = false; t.muted = false; }
        emit micChanged();
        return;
    }
    m_micButtonDevice = (keyCmd >= 0 && keyCmd <= 3) ? keyCmd : -1;
    // Once the camera reports the value we asked for, the optimistic override
    // has done its job and the device becomes the single source of truth again.
    if (m_micButtonTarget >= 0 && m_micButtonDevice == m_micButtonTarget) {
        m_micButtonTarget = -1;
        m_micButtonTimer->stop();
    }
    // The battery bytes only mean something for a slot the camera says is
    // online — an empty slot keeps whatever was last written there.
    //
    // A CONNECTED mic reporting 0 % is treated as "unknown", not as a real
    // reading: the camera answers 0 for a slot it has not filled in yet, so
    // right after a (re)pair the UI would otherwise flash a red "0 %" flat-
    // battery alarm for a mic that is actually fine — observed with a mic at
    // 77 %. A genuinely empty mic powers itself off rather than staying
    // connected at zero, so "—" is the honest reading for 0.
    const auto batteryOf = [](bool online, int raw) { return (online && raw > 0) ? raw : -1; };
    m_micTx[0].battery = batteryOf(m_micTx[0].online, batt1);
    m_micTx[0].charging = m_micTx[0].online && charging1;
    m_micTx[0].muted = m_micTx[0].online && muted1;
    m_micTx[1].battery = batteryOf(m_micTx[1].online, batt2);
    m_micTx[1].charging = m_micTx[1].online && charging2;
    m_micTx[1].muted = m_micTx[1].online && muted2;
    emit micChanged();
}

// Per-mic gain and noise reduction, from the same DevTWSInfo read as onTwsInfo.
// ctlSupported is a BUILD fact (did the per-mic set/get symbols resolve), not a
// device one — the camera answering cameraGetTWSInfoR is what capMicGain also
// requires, and the set's own rc is the final word.
//
// Like the battery bytes, these only mean something for a slot the camera says is
// online: an empty slot's gain byte is whatever was last left there.
void CameraController::onTwsMicAudio(bool ctlSupported, int gain1, int ns1, int nsLevel1,
                                     int gain2, int ns2, int nsLevel2) {
    m_capMicGain = ctlSupported;
    const int gains[2] = {gain1, gain2};
    const int nss[2] = {ns1, ns2};
    const int levels[2] = {nsLevel1, nsLevel2};
    for (int i = 0; i < 2; ++i) {
        MicTx &t = m_micTx[i];
        // kTxGainUnknown (the worker's out-of-int8 sentinel) means the TWS read
        // itself failed — not a gain of any kind.
        const bool known = t.online && gains[i] >= -128 && gains[i] <= 127;
        t.gainKnown = known;
        t.gain = known ? gains[i] : 0;
        // The camera has caught up with the step we asked for, so the optimistic
        // value has done its job. Also drop it if the slot went away.
        // No bound is inferred from a mismatch here. "The camera reports a
        // different value than we asked for" cannot be told apart from "the
        // readback has not caught up yet", and treating the latter as a limit
        // invented a floor that then blocked the step it was inferred from.
        // The mic ignoring an out-of-range value is harmless; a wrong limit is
        // not.
        if (t.gainPending && (!t.online || (known && t.gain == t.gainTarget)))
            t.gainPending = false;
        t.ns = t.online ? nss[i] : -1;
        t.nsLevel = t.online ? levels[i] : -1;
    }
    if (!m_micTx[0].gainPending && !m_micTx[1].gainPending)
        m_micGainTimer->stop();
    emit micChanged();
}

// ---------------------------------------------------------------------------
// Device event push — everything here is CONDITIONAL on the camera actually
// sending events, which on a Tiny 3 is unproven (dev.hpp: "@category tail air").
// Silence is the expected case and is not treated as an error anywhere.
// ---------------------------------------------------------------------------
void CameraController::onDeviceEvent(int eventType, const QString &name) {
    Q_UNUSED(eventType);
    Q_UNUSED(name);
    // The worker already logged the event with its numeric type — that log IS the
    // experiment. All this adds is the one bit the UI can honestly state: that
    // this camera pushes events at all.
    if (!m_devEventsSeen) {
        m_devEventsSeen = true;
        emit faultChanged();
    }
}

void CameraController::onDeviceFault(bool faulted, const QString &reason) {
    if (faulted == m_deviceFault && reason == m_deviceFaultReason) return;
    m_deviceFault = faulted;
    m_deviceFaultReason = reason;
    emit faultChanged();
}

// A wireless-mic button/status cue. slot 0 means the event did not say which mic,
// in which case the cue goes on every ONLINE slot rather than being attributed to
// a guess — showing "track on" under mic 1 when mic 2's button was pressed would
// be a fabricated detail.
void CameraController::onMicTip(int slot, const QString &what) {
    bool dirty = false;
    for (int i = 0; i < 2; ++i) {
        const bool mine = (slot == i + 1) || (slot == 0 && m_micTx[i].online);
        if (mine) {
            m_micTx[i].tip = what;
            m_micTx[i].tipAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
            m_micTx[i].tipRecent = true;   // highlight fades; the text stays
            dirty = true;
        }
    }
    if (!dirty) return;
    m_micTipTimer->start();   // restart the window; a fresh press supersedes
    emit micChanged();
}

// has_pair_record is the "this camera has never had a mic paired" diagnostic.
// is_auto is load-bearing for the Audio section: while it is 1 the camera picks
// the input itself and silently reverts any manual choice, so it drives the
// "Automatic" segment and must be readable — and clearable — by the user.
// support_auto only says whether the camera has the feature at all, which
// capAudio and the auto readback already convey, so it stays informational.
void CameraController::onMicAudioSelect(int hasPairRecord, int isAuto, int supportAuto) {
    Q_UNUSED(supportAuto);
    if (isAuto != m_audioAuto) {
        m_audioAuto = isAuto;
        emit audioChanged();
    }
    if (hasPairRecord == m_micPairRecord) return;
    m_micPairRecord = hasPairRecord;
    emit micChanged();
}

// Built-in audio detail (volume / mute / noise reduction / AGC) from the
// worker's one read pass. `supported` is the runtime capability probe — false
// means the camera-audio API is unavailable on this build/firmware, and every
// value must go back to "unknown" rather than showing an invented 0.
void CameraController::onAudioState(bool supported, int volume, int muted,
                                    int noiseReduce, int noiseLevel, int agc) {
    m_capAudio = supported;
    if (!supported) {
        m_audioVolume = -1;
        m_audioMuted = -1;
        m_audioNoiseReduce = -1;
        m_audioNoiseLevel = -1;
        m_audioAgc = -1;
        emit audioChanged();
        return;
    }
    m_audioVolume = volume;
    m_audioMuted = muted;
    m_audioNoiseReduce = noiseReduce;
    m_audioNoiseLevel = noiseLevel;
    m_audioAgc = agc;
    emit audioChanged();
}

void CameraController::scheduleStartupPreset(const QString &why) {
    const int sp = m_settings.startupPreset;
    if (sp < 1 || sp > 3 || !m_settings.presets[sp - 1].set)
        return;
    emit logLine("cmd", QStringLiteral("%1: going to preset %2 (after gimbal settles)").arg(why).arg(sp));
    // Delay so the camera's power-on / wake self-centering completes first —
    // otherwise it overrides the preset move and the gimbal ends up centered
    // instead of at the preset. Context object is `this`, so a destroyed
    // controller cancels the pending fire.
    QTimer::singleShot(1600, this, [this, sp]() {
        if (connected() && !m_aiTracking)
            goPreset(sp - 1);
    });
}
