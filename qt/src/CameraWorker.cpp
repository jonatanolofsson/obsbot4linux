#include "CameraWorker.h"

#include <QMetaObject>
#include <QTimer>

#include <cmath>
#include <string>

#include <dev/devs.hpp>

#include "ObsbotTwsCompat.h"

namespace {

// Gimbal safety clamps (degrees) — identical bounds to the validated GTK PoC.
constexpr float kPitchMin = -90.0f, kPitchMax = 90.0f;
constexpr float kYawMin = -120.0f, kYawMax = 120.0f;
// Stale-AI-status grace after a confirmed AI-off (device pushes lag 2–3 s).
constexpr qint64 kAiOffGraceMs = 4000;
// Gesture-friendly status cadence: one status refresh per this interval while
// gesture control is on. The camera's gesture recognizer is suppressed by the
// SDK's ~2–3 s status polling (hardware-confirmed via the quiet-test), so the
// refresh becomes a short enable→one push→disable duty cycle, leaving the USB
// control channel quiet ~90+% of the time.
constexpr int kStatusDutyMs = 15000;
// Wireless-mic DETAIL refresh cadence. Deliberately slow: mic presence arrives
// free with the status push, so this single cameraGetTWSInfoR call only exists
// to keep battery/mute fresh. Same recognizer concern that drives the
// gesture-friendly cadence — keep the control channel quiet.
constexpr int kTwsInfoPollMs = 60000;
// Settle delay between waking the camera and issuing a pairing command. A
// sleeping Tiny 3 answers rc=0 but never powers its mic radio (hardware
// finding), so pairing always runs awake — this gives the wake a moment to
// take effect without blocking the worker's event loop.
constexpr int kPairWakeSettleMs = 1200;
// Confirming a mic-button assignment. HARDWARE FINDING: cameraSetTWSKeyTypeR
// returns rc=0 immediately, but cameraGetTWSInfoR keeps answering the OLD
// key_cmd for a while afterwards — measured at 316 ms and 2983 ms on two
// consecutive writes to an AWAKE Tiny 3 (fw 6.6.8.25). The write itself never
// failed. Reading back once, right after the set, therefore misses the change
// almost every time: the UI gave up, sprang back to the previous segment, and
// the user had to click twice — the second click "succeeding" only because it
// read back what the FIRST click had already applied.
//
// So poll instead of asking once, and give it a budget comfortably past the
// worst observed latency rather than the ~3 s that was nearly hit.
constexpr int kKeyConfirmIntervalMs = 250;
constexpr int kKeyConfirmBudgetMs   = 6000;
// Per-mic gain bounds. NOT a documented range — the SDK gives none. This is the
// only bound anyone can defend: DevTWSInfo carries the device's own reported gain
// in an int8_t, so a value outside it could never be read back. The camera's real
// limits are discovered empirically, by cmdSetTxGain's readback.
constexpr int kTxGainMin = -128;
constexpr int kTxGainMax = 127;
// Out-of-int8 sentinel for "the camera has not reported a gain" — gain is signed,
// so -1 is a legitimate value and cannot double as "unknown".
constexpr int kTxGainUnknown = -1000;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// RunState enum mirrored by CameraController: 0=Unknown, 1=Awake, 2=Asleep.
int runStateFromDev(int devStatus) {
    switch (devStatus) {
    case Device::DevStatusRun:   return 1;   // Awake
    case Device::DevStatusSleep: return 2;   // Asleep
    default:                     return 0;   // Unknown / Privacy
    }
}

const char *productName(ObsbotProductType t) {
    switch (t) {
    case ObsbotProdTiny:      return "Tiny";
    case ObsbotProdTiny4k:    return "Tiny4K";
    case ObsbotProdTiny2:     return "Tiny2";
    case ObsbotProdTiny2Lite: return "Tiny2Lite";
    case ObsbotProdTinySE:    return "TinySE";
    case ObsbotProdTiny3:     return "Tiny3";
    case ObsbotProdTiny3Lite: return "Tiny3Lite";
    case ObsbotProdMeet:      return "Meet";
    case ObsbotProdMeet4k:    return "Meet4K";
    case ObsbotProdTailAir:   return "TailAir";
    default:                  return "Unknown";
    }
}

// Device::DevAudioSourceType, for log lines. Bluetooth(3) is what a Vox SE
// wireless mic shows up as — the camera has no separate "2.4 GHz mic" source.
const char *audioSourceName(int s) {
    switch (s) {
    case Device::DevAudioSourceTypeBuildIn:   return "built-in mic array";
    case Device::DevAudioSourceTypeAuxLine:   return "aux line in";
    case Device::DevAudioSourceTypeAuxMic:    return "aux mic in";
    case Device::DevAudioSourceTypeBluetooth: return "wireless mic";
    case Device::DevAudioSourceTypeUsbC:      return "USB-C audio";
    default:                                  return "unknown source";
    }
}

// Device event decode. The values come from Device::RmEventType (dev.hpp) and are
// referenced BY ENUMERATOR, never by a copied-out number, so a future SDK
// renumbering cannot silently shift them. `micSlot` is 1/2 when the event names a
// transmitter and 0 when it does not — several TWS tips genuinely do not say
// which mic, and guessing would be worse than admitting it.
//
// Only the events this app acts on (plus the neighbours that make a log line
// readable) are decoded; anything else logs as its raw number, which is the point
// of the exercise — the SDK marks this whole callback "@category tail air", so
// the numbers a Tiny 3 actually emits, if any, are what we are trying to find out.
struct DevEventInfo {
    const char *name;   // nullptr => unknown to us; log the raw number
    int micSlot;        // 1/2 when the event names a transmitter, else 0
};

DevEventInfo devEventInfo(int t) {
    switch (t) {
    // Errors — kEvtErrAiComm is the one that changes app behaviour.
    case Device::kEvtErrAiComm:        return {"AI communication error", 0};
    case Device::kEvtErrGimbalComm:    return {"gimbal communication error", 0};
    case Device::kEvtErrLensComm:      return {"lens communication error", 0};
    case Device::kEvtErrSensor:        return {"sensor error", 0};
    case Device::kEvtErrMedia:         return {"media error", 0};
    case Device::kEvtErrBluetooth:     return {"bluetooth error", 0};
    case Device::kEvtErrDevTempHigh:   return {"device temperature too high", 0};
    // The "info" counterparts of the error events — the recovery edge.
    case Device::kEvtInfoAiComm:       return {"AI communication restored", 0};
    case Device::kEvtInfoGimbalComm:   return {"gimbal communication restored", 0};
    case Device::kEvtInfoLensComm:     return {"lens communication restored", 0};
    case Device::kEvtInfoBluetooth:    return {"bluetooth info", 0};
    case Device::kEvtInfoDevTemp:      return {"device temperature normal", 0};
    case Device::kEvtInfoTargetLoss:   return {"tracking target lost", 0};
    case Device::kEvtWarnNoAudioInput: return {"no audio input", 0};
    // Wireless-mic button / status tips.
    case Device::kEvtTipsTWSFirstConnect:     return {"mic paired for the first time", 0};
    case Device::kEvtTipsTWSTipConnect1:      return {"connected", 1};
    case Device::kEvtTipsTWSTipConnect2:      return {"connected", 2};
    case Device::kEvtTipsTWSTipElectric1:     return {"battery report", 1};
    case Device::kEvtTipsTWSTipElectric2:     return {"battery report", 2};
    case Device::kEvtTipsTWSTipMute1:         return {"mute button", 1};
    case Device::kEvtTipsTWSTipMute2:         return {"mute button", 2};
    case Device::kEvtTipsTWSTipMuteQuiet1:    return {"mute button (silent cue)", 1};
    case Device::kEvtTipsTWSTipMuteQuiet2:    return {"mute button (silent cue)", 2};
    case Device::kEvtTipsTWSTipDenoiseOn:     return {"noise reduction on", 0};
    case Device::kEvtTipsTWSTipDenoiseOff:    return {"noise reduction off", 0};
    case Device::kEvtTipsTWSTipRecordOn:      return {"record on", 0};
    case Device::kEvtTipsTWSTipRecordOff:     return {"record off", 0};
    case Device::kEvtTipsTWSTipHumanTrackOn:  return {"track on", 0};
    case Device::kEvtTipsTWSTipHumanTrackOff: return {"track off", 0};
    case Device::kEvtTipsTWSTipSingleTrack:   return {"single-person track", 0};
    case Device::kEvtTipsTWSTipMultipleTrack: return {"multi-person track", 0};
    default:                                  return {nullptr, 0};
    }
}

// True for the kEvtTipsTWS* block — the events that describe a wireless mic.
// Range-checked rather than re-listed so a tip we have no decode for still
// refreshes the mic detail instead of being dropped.
bool isTwsTip(int t) {
    return t >= Device::kEvtTipsTWSFirstConnect && t <= Device::kEvtTipsTWSTipMultipleTrack;
}

const char *devModeName(Device::DevMode m) {
    switch (m) {
    case Device::DevModeUvc: return "UVC";
    case Device::DevModeNet: return "Net";
    case Device::DevModeMtp: return "MTP";
    case Device::DevModeBle: return "BLE";
    default:                 return "Unknown";
    }
}

} // namespace

CameraWorker::CameraWorker(QObject *parent) : QObject(parent) {}

CameraWorker::~CameraWorker() = default;

void CameraWorker::init() {
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(200);
    connect(m_pollTimer, &QTimer::timeout, this, &CameraWorker::pollTick);

    // Deadman stop: if no fresh velocity command arrives within this window
    // (e.g. the UI thread stalls, or the drag handler fails to deliver a stop),
    // halt the gimbal automatically. Restarted on every cmdGimbalVelocity call.
    m_velocityWatchdog = new QTimer(this);
    m_velocityWatchdog->setSingleShot(true);
    m_velocityWatchdog->setInterval(400);
    connect(m_velocityWatchdog, &QTimer::timeout, this, [this]() {
        if (m_velocityActive) {
            emit logLine("warn", QStringLiteral("ptz velocity: deadman timeout — auto-stopped"));
            cmdGimbalStop();
        }
    });

    // Gesture-friendly duty cycle: periodically open a one-push status window
    // (see setGestureFriendly / onSdkStatus for the close).
    m_statusDutyTimer = new QTimer(this);
    m_statusDutyTimer->setInterval(kStatusDutyMs);
    connect(m_statusDutyTimer, &QTimer::timeout, this, [this]() {
        if (m_shuttingDown || !m_dev || m_quiet) return;
        m_awaitingDutyPush = true;
        m_dev->enableDevStatusCallback(true);
    });

    // Wireless-mic detail refresh: one cameraGetTWSInfoR per minute while a
    // device is bound AND the camera answered the probe. Silent (no logLine) —
    // it's a background read, not a user command. Started in bindDevice (after
    // the probe succeeds), stopped in shutdown.
    m_twsInfoTimer = new QTimer(this);
    m_twsInfoTimer->setInterval(kTwsInfoPollMs);
    connect(m_twsInfoTimer, &QTimer::timeout, this, [this]() {
        if (m_shuttingDown || !m_dev || m_quiet || !m_twsSupported) return;
        cmdReadTwsInfo();
    });
}

// ---------------------------------------------------------------------------
// Discovery (non-blocking poll on the worker's own event loop)
// ---------------------------------------------------------------------------
void CameraWorker::startDiscovery(int waitMs) {
    if (m_shuttingDown) return;
    m_pollTimeoutMs = waitMs > 0 ? waitMs : 6000;
    m_pollElapsedMs = 0;

    Devices &devs = Devices::get();
    devs.setEnableMdnsScan(false);   // USB only for this app

    // One-time device plug/unplug callback (hot-unplug handling, CODE_REVIEW #6).
    if (!m_devChangedRegistered) {
        devs.setDevChangedCallback(
            [this](std::string sn, bool plugged, void *) {
                const QString qsn = QString::fromStdString(sn);
                QMetaObject::invokeMethod(
                    this, [this, qsn, plugged]() { onDevChanged(qsn, plugged); },
                    Qt::QueuedConnection);
            },
            this);
        m_devChangedRegistered = true;
    }

    emit logLine("net", QStringLiteral("discovery: started (USB, timeout %1 ms)").arg(m_pollTimeoutMs));
    m_pollTimer->start();
}

void CameraWorker::rescan(int waitMs) {
    if (m_shuttingDown) return;
    emit logLine("net", QStringLiteral("rescan requested"));
    startDiscovery(waitMs);
}

void CameraWorker::pollTick() {
    if (m_shuttingDown) { m_pollTimer->stop(); return; }

    std::shared_ptr<Device> found;
    for (const auto &d : Devices::get().getDevList()) {
        if (d && !d->devSn().empty()) { found = d; break; }
    }
    if (found) {
        m_pollTimer->stop();
        bindDevice(found);
        return;
    }

    m_pollElapsedMs += m_pollTimer->interval();
    if (m_pollElapsedMs % 1000 == 0) {
        emit logLine("net", QStringLiteral("discovery: polling for device … (%1/%2 ms)")
                                .arg(m_pollElapsedMs).arg(m_pollTimeoutMs));
    }
    if (m_pollElapsedMs >= m_pollTimeoutMs) {
        m_pollTimer->stop();
        emit logLine("warn", QStringLiteral("discovery: timeout after %1 ms — no device").arg(m_pollTimeoutMs));
        emit connectionResolved(false, {}, {}, {}, {}, -1);
    }
}

void CameraWorker::bindDevice(const std::shared_ptr<Device> &d) {
    m_dev = d;
    m_sn = QString::fromStdString(d->devSn());
    m_twsSupported = false;   // re-probed below; never carried over from a previous device
    m_twsProbed = false;
    m_audioSupported = false;
    m_audioProbed = false;

    const QString product = productName(d->productType());
    const QString fw = QString::fromStdString(d->devVersion());
    const QString mode = devModeName(d->devMode());
    const int enumId = static_cast<int>(d->productType());

    // Subscribe to the periodic status push (~2–3 s) for run/AI/zoom state.
    d->setDevStatusCallbackFunc(&CameraWorker::sdkStatusTrampoline, this);
    d->enableDevStatusCallback(true);
    // …and to the device EVENT push, right beside it. Unlike the status push this
    // one is not periodic — the camera pushes when something happens (an AI fault,
    // a mic button) — and, crucially, the SDK header marks it "@category tail
    // air", so a Tiny 3 may never send a single event. Registering costs nothing
    // and every consumer is written to degrade silently; see onDevEvent.
    setDevEventCallback(true);

    emit logLine("ok", QStringLiteral("connected: %1  (SN %2, fw %3, %4)")
                           .arg(product, m_sn, fw, mode));
    emit connectionResolved(true, product, m_sn, fw, mode, enumId);

    // Read real zoom + current image params once now (blocking getters, safe here).
    refreshZoom();
    cmdReadImageParams();

    // Wireless mic: probe the undocumented TWS API once (cmdReadTwsInfo sets
    // m_twsSupported from the rc and emits the result either way), then keep
    // battery/mute fresh on the slow timer if the camera answered. Presence
    // itself needs nothing here — it rides the status push.
    cmdReadTwsInfo();
    if (m_twsSupported) m_twsInfoTimer->start();

    cmdReadAudioSelect();
    // The camera's own mic array: one read pass, which is also the capability
    // probe. Nothing is WRITTEN on connect — a rapid burst of audio/AI/sleep
    // writes has been observed to drive the camera into an AI fault (solid red
    // LED, everything silently ignored), so the app never "syncs all settings".
    cmdReadAudio();
}

// ---------------------------------------------------------------------------
// Status push: SDK thread -> hop to worker thread -> emit to GUI thread
// ---------------------------------------------------------------------------
void CameraWorker::sdkStatusTrampoline(void *param, const void *data) {
    if (!param || !data) return;
    auto *self = static_cast<CameraWorker *>(param);
    const auto *st = static_cast<const Device::CameraStatus *>(data);
    const int run = st->tiny.dev_status;
    const int ai = st->tiny.ai_mode;
    const int face = st->tiny.face_auto_focus;   // face autofocus 0/1
    const int hdr = st->tiny.hdr;                // hdr 0/1
    const int hdrSup = st->tiny.hdr_support;     // hdr supported in current mode 0/1
    const int fps = st->tiny.fps;                // current video stream fps
    const int sleepMicro = st->tiny.sleep_micro; // mic during sleep 0/1 (readback for cmdSetMicSleep)
    const int autoSleep = st->tiny.auto_sleep_time; // seconds, 0=never (readback for cmdSetAutoSleep)
    // Wireless mic (Vox SE) — the AUTHORITATIVE presence signal, and public/
    // documented, so no undocumented call is needed for it. Copied out of the
    // callback's buffer in the device's own bit order (see the header):
    // bit0 is_tws_mode, bits1-2 tx_state, bit3 is_pairing, bit4 is_scanning.
    const auto &wm = st->tiny.wireless_mic;
    const int micBits = (wm.is_tws_mode ? 1 << 0 : 0)
                        | ((wm.tx_state & 0x3) << 1)
                        | (wm.is_pairing ? 1 << 3 : 0)
                        | (wm.is_scanning ? 1 << 4 : 0);
    // The camera's OWN audio, from the neighbouring public bitfield: which input
    // it is using (DevAudioSourceType — 3 is the wireless Vox SE) and the mic
    // array's pickup pattern (AudioModeType). This is the READBACK for both
    // cmdSetAudioSource and cmdSetAudioMode: neither has an exported getter, and
    // neither command's rc is proof that the setting took.
    const auto &am = st->tiny.audio_mode;
    const int audioBits = (am.source & 0x7) | ((am.mode & 0x1f) << 3);
    // Do NOT touch SDK/Qt state here — just marshal onto the worker thread.
    QMetaObject::invokeMethod(
        self, [self, run, ai, face, hdr, hdrSup, fps, sleepMicro, autoSleep, micBits, audioBits]() { self->onSdkStatus(run, ai, face, hdr, hdrSup, fps, sleepMicro, autoSleep, micBits, audioBits); },
        Qt::QueuedConnection);
}

void CameraWorker::onSdkStatus(int runStatus, int aiMode, int faceFocus, int hdr, int hdrSupport, int fps, int sleepMicro, int autoSleepSec, int micBits, int audioBits) {
    if (m_shuttingDown || !m_dev) return;
    // Gesture-friendly cadence: this push is the duty cycle's one shot — close
    // the window again so the control channel goes quiet for the recognizer.
    if (m_gestureFriendly) {
        m_dev->enableDevStatusCallback(false);
        m_awaitingDutyPush = false;
    }
    if (m_quiet) return;   // gesture diagnostic: no reads, no signals, no traffic
    // aiMode>0 means some AI mode is engaged (includes AiWorkModeSwitching=6).
    // Treating "switching" as tracking is the safe choice for the gimbal guard.
    //
    // Stale-push suppression, ONE push, both directions. The device's status
    // lags a confirmed AI command by up to a push cycle (2–3 s):
    //  * after AI-OFF, a stale push still reports the old AI mode — taking it
    //    at face value re-arms the AI-owns-gimbal guard and blocks the moves
    //    users expect right after turning AI off (incl. the auto return-preset);
    //  * after AI-ON, a stale push still reports None — taking it at face value
    //    flickered the chip off and fired a bogus "device disengaged" hint.
    // Each grace window swallows exactly ONE contradicting push and then closes
    // (review finding: a blanket 4 s rewrite also swallowed a GENUINE palm
    // re-engage within the window, releasing the gimbal guard while the device
    // was really tracking — one-shot bounds that to a single push).
    if (aiMode > Device::AiWorkModeNone
        && m_aiOffGrace.isValid() && m_aiOffGrace.elapsed() < kAiOffGraceMs) {
        aiMode = Device::AiWorkModeNone;   // stale on-push right after our off
        m_aiOffGrace.invalidate();
    } else if (aiMode <= Device::AiWorkModeNone
               && m_aiOnGrace.isValid() && m_aiOnGrace.elapsed() < kAiOffGraceMs) {
        aiMode = m_lastAiOnMode;           // stale off-push right after our on
        m_aiOnGrace.invalidate();
    } else if (aiMode <= Device::AiWorkModeNone) {
        m_aiOffGrace.invalidate();         // device caught up; graces settled
    } else {
        m_aiOnGrace.invalidate();
    }
    m_aiTracking = (aiMode > Device::AiWorkModeNone);

    // Refresh real zoom from the getter (1.0–2.0). Runs on the worker thread,
    // serialized with commands — never on the SDK callback thread.
    float z = 0.0f;
    const bool zok = (m_dev->cameraGetZoomAbsoluteR(z) == RM_RET_OK);
    emit statusUpdate(runStateFromDev(runStatus), aiMode, z, zok);
    emit auxStatus(faceFocus != 0, hdr != 0, hdrSupport != 0, fps, sleepMicro, autoSleepSec);

    // Wireless-mic presence. tx_state is a 2-bit mask: bit0 => slot 1 online,
    // bit1 => slot 2 online (00 none, 11 both).
    const int txState = (micBits >> 1) & 0x3;
    emit micStatus((txState & 0x1) != 0, (txState & 0x2) != 0,
                   (micBits & (1 << 0)) != 0,
                   (micBits & (1 << 3)) != 0,
                   (micBits & (1 << 4)) != 0,
                   audioBits & 0x7, (audioBits >> 3) & 0x1f);
}

// ---------------------------------------------------------------------------
// Device event push (Device::setDevEventNotifyCallbackFunc)
//
// The SDK calls this on ITS OWN thread, exactly like the status push and the
// plug/unplug callback, so the lambda touches nothing but a queued invoke — the
// same shape setDevChangedCallback already uses.
//
// OPEN QUESTION, and the reason nothing here is load-bearing: dev.hpp documents
// the callback "@category tail air". Whether a Tiny 3 emits anything at all is
// unknown until someone watches the log with hardware attached. So every event
// is logged with its NUMERIC type (that log is the experiment), and the features
// built on top — the AI-fault banner, the mic button cues — simply never appear
// if the camera stays silent. Nothing waits on an event, nothing times out on
// one, and no control is disabled because one failed to arrive.
// ---------------------------------------------------------------------------
void CameraWorker::setDevEventCallback(bool on) {
    if (!m_dev) { m_devEventRegistered = false; return; }
    if (on) {
        m_dev->setDevEventNotifyCallbackFunc(
            [this](void *, int32_t eventType, const void *) {
                // SDK thread. Do NOT touch Qt or SDK state here, and do NOT read
                // `result`: its type is event-specific, undocumented for every
                // event this app uses, and it points into the SDK's own buffer.
                const int t = static_cast<int>(eventType);
                QMetaObject::invokeMethod(
                    this, [this, t]() { onDevEvent(t); }, Qt::QueuedConnection);
            },
            this);
        if (!m_devEventRegistered) {
            m_devEventRegistered = true;
            // Say plainly that this is a listening post, not a feature: it is the
            // only way anyone reading the log can tell "no events happened" from
            // "this camera does not send events".
            emit logLine("sys", QStringLiteral(
                "device events: listening (setDevEventNotifyCallbackFunc). The SDK documents "
                "this push for the Tail Air only, so a Tiny 3 may never send one — every event "
                "received is logged with its numeric type."));
        }
        return;
    }
    // Hand the callback back. Deliberately a NO-OP LAMBDA, never an empty
    // std::function: libdev is closed source and may well invoke it without a
    // null check, and invoking an empty std::function is std::bad_function_call
    // — a crash during teardown, which is precisely what the deterministic
    // shutdown design exists to prevent.
    m_dev->setDevEventNotifyCallbackFunc([](void *, int32_t, const void *) {}, nullptr);
    m_devEventRegistered = false;
}

void CameraWorker::onDevEvent(int eventType) {
    if (m_shuttingDown) return;
    const DevEventInfo info = devEventInfo(eventType);
    const QString name = info.name ? QString::fromLatin1(info.name) : QString();
    // Log EVERY event, decoded or not, with the raw number. On a Tiny 3 this line
    // is the whole point: it is the only way to learn which events (if any) this
    // camera actually pushes.
    emit logLine("sys", name.isEmpty()
                            ? QStringLiteral("device event: %1 (not in the SDK enum we decode)").arg(eventType)
                            : QStringLiteral("device event: %1 (%2)").arg(eventType).arg(name));
    emit deviceEvent(eventType, name);

    // The AI subsystem. This is the event worth wiring: in an AI fault the Tiny 3
    // shows a solid red ring and silently ignores tracking commands while
    // cameraStatus() keeps reporting everything as normal — so the app's Track
    // toggle looks healthy and does nothing.
    if (eventType == Device::kEvtErrAiComm) {
        emit logLine("warn", QStringLiteral(
            "device fault: the camera reports an AI communication error. Tracking commands "
            "will be accepted and silently ignored while this lasts (the ring LED goes solid "
            "red). Unplug the camera and plug it back in to clear it."));
        emit deviceFault(true, QStringLiteral("AI communication error — tracking is ignored until "
                                              "the camera is power-cycled"));
        return;
    }
    if (eventType == Device::kEvtInfoAiComm) {
        emit logLine("ok", QStringLiteral("device fault cleared: AI communication restored"));
        emit deviceFault(false, QString());
        return;
    }

    // Wireless-mic tips. Two jobs: a transient cue on the mic card, and — more
    // useful — a trigger to re-read DevTWSInfo, since a mute button press changes
    // state the app would otherwise not see for up to 60 s.
    if (isTwsTip(eventType)) {
        emit micTip(info.micSlot, name.isEmpty() ? QStringLiteral("event %1").arg(eventType) : name);
        if (m_twsSupported) cmdReadTwsInfo();
    }
}

void CameraWorker::onDevChanged(const QString &sn, bool plugged) {
    if (m_shuttingDown) return;
    if (!plugged) {
        // Unplug. If it's our device (or we can no longer see any), drop it.
        if (m_dev && (sn == m_sn || sn.isEmpty())) {
            emit logLine("warn", QStringLiteral("device unplugged (SN %1)").arg(m_sn));
            if (m_dev) m_dev->enableDevStatusCallback(false);
            setDevEventCallback(false);   // let go of `this` before the handle drops
            m_dev.reset();
            m_aiTracking = false;
            m_twsSupported = false;
            m_audioSupported = false;
            if (m_twsInfoTimer) m_twsInfoTimer->stop();
            emit deviceLost(QStringLiteral("USB unplug"));
        }
    } else if (!m_dev) {
        emit logLine("net", QStringLiteral("device plugged in (SN %1) — reconnecting").arg(sn));
        startDiscovery(m_pollTimeoutMs);
    }
}

// ---------------------------------------------------------------------------
// Command helpers
// ---------------------------------------------------------------------------
bool CameraWorker::requireDevice(const QString &action) {
    if (m_dev) return true;
    emit commandResult(action, false, -1, QStringLiteral("no device connected"));
    emit logLine("warn", action + QStringLiteral(": no device connected"));
    return false;
}

bool CameraWorker::aiOwnsGimbal(const QString &action) {
    if (!m_aiTracking) return false;
    // Honest: the device keeps the gimbal under AI control, so a manual move
    // would be ignored. Block it and say so — never log a fake success.
    const QString msg = QStringLiteral("blocked: AI tracking owns the gimbal — turn AI off first");
    emit commandResult(action, false, -1, msg);
    emit logLine("warn", action + QStringLiteral(": ") + msg);
    return true;
}

void CameraWorker::refreshZoom() {
    if (!m_dev) return;
    float z = 0.0f;
    const bool ok = (m_dev->cameraGetZoomAbsoluteR(z) == RM_RET_OK);
    emit zoomUpdate(z, ok);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void CameraWorker::cmdWake() {
    const QString a = QStringLiteral("wake");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ wake"));
    const int rc = m_dev->cameraSetDevRunStatusR(Device::DevStatusRun);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? QStringLiteral("awake") : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("wake  rc=%1").arg(rc));
    statusPulse();   // wake edge must reach the UI now, not at the next duty tick
}

void CameraWorker::cmdSleep() {
    const QString a = QStringLiteral("sleep");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ sleep"));
    const int rc = m_dev->cameraSetDevRunStatusR(Device::DevStatusSleep);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? QStringLiteral("asleep") : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("sleep  rc=%1").arg(rc));
    statusPulse();
}

void CameraWorker::cmdCenter() {
    const QString a = QStringLiteral("center");
    if (!requireDevice(a)) return;
    if (aiOwnsGimbal(a)) return;                 // CODE_REVIEW #4
    emit logLine("cmd", QStringLiteral("→ center"));
    const int rc = m_dev->gimbalRstPosR();       // bounded reset to home
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? QStringLiteral("centered") : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("center  rc=%1").arg(rc));
    statusPulse();   // refresh guard/UI state promptly in low-traffic mode
}

void CameraWorker::cmdNudge(int dir, double stepDeg, double speed) {
    const char *names[] = {"ptz up", "ptz down", "ptz left", "ptz right"};
    const QString a = QString::fromLatin1(dir >= 0 && dir < 4 ? names[dir] : "ptz");
    if (!requireDevice(a)) return;
    if (aiOwnsGimbal(a)) return;                 // CODE_REVIEW #4

    // Direction → axis sign. INVERTED vs the GTK PoC per Rex's hardware test
    // (Rex finding #1): up = -pitch, down = +pitch, left = -yaw, right = +yaw.
    float pitchSign = 0.0f, yawSign = 0.0f;
    switch (dir) {
    case 0: pitchSign = -1.0f; break;  // up
    case 1: pitchSign = +1.0f; break;  // down
    case 2: yawSign = -1.0f; break;    // left
    case 3: yawSign = +1.0f; break;    // right
    }

    const float step = static_cast<float>(stepDeg);
    const float spd = static_cast<float>(speed);
    emit logLine("cmd", QStringLiteral("→ %1 (%2°, spd %3)").arg(a).arg(step, 0, 'f', 0).arg(spd, 0, 'f', 0));

    float xyz[3] = {0, 0, 0};   // roll, pitch, pan(yaw)
    const int rc = m_dev->gimbalGetAttitudeInfoR(xyz);
    if (rc != RM_RET_OK) {
        emit commandResult(a, false, rc, QStringLiteral("read attitude failed"));
        emit logLine("warn", a + QStringLiteral(": read attitude failed rc=%1").arg(rc));
        return;
    }
    const float pitch = xyz[1], yaw = xyz[2];
    const float np = clampf(pitch + pitchSign * step, kPitchMin, kPitchMax);
    const float ny = clampf(yaw + yawSign * step, kYawMin, kYawMax);
    const int rc2 = m_dev->gimbalSetSpeedPositionR(0.0f, np, ny, 0.0f, spd, spd);
    const bool ok = (rc2 == RM_RET_OK);
    emit commandResult(a, ok, rc2,
                       QStringLiteral("pitch %1→%2, yaw %3→%4")
                           .arg(pitch, 0, 'f', 1).arg(np, 0, 'f', 1)
                           .arg(yaw, 0, 'f', 1).arg(ny, 0, 'f', 1));
    emit logLine(ok ? "ok" : "warn",
                 QStringLiteral("%1: pitch %2→%3, yaw %4→%5  rc=%6")
                     .arg(a).arg(pitch, 0, 'f', 1).arg(np, 0, 'f', 1)
                     .arg(yaw, 0, 'f', 1).arg(ny, 0, 'f', 1).arg(rc2));
    statusPulse();
}

void CameraWorker::cmdZoom(double delta, bool absolute, double absVal, const QString &action) {
    if (!requireDevice(action)) return;
    emit logLine("cmd", QStringLiteral("→ %1").arg(action));

    float cur = 1.0f;
    const bool haveCur = (m_dev->cameraGetZoomAbsoluteR(cur) == RM_RET_OK);

    float target;
    if (absolute) {
        // CODE_REVIEW #10: the 1x reset must set 1.0 even if the getter fails —
        // the target is a constant and does not depend on the current reading.
        target = static_cast<float>(absVal);
    } else {
        if (!haveCur) {
            emit commandResult(action, false, -1, QStringLiteral("read zoom failed"));
            emit logLine("warn", action + QStringLiteral(": read zoom failed"));
            return;
        }
        target = cur + static_cast<float>(delta);
    }
    target = clampf(target, 1.0f, 2.0f);

    const int rc = m_dev->cameraSetZoomAbsoluteR(target);
    const bool ok = (rc == RM_RET_OK);

    // Re-read the ACTUAL zoom after the command so the UI never sticks (Rex #6).
    refreshZoom();

    emit commandResult(action, ok, rc,
                       QStringLiteral("%1x → %2x")
                           .arg(haveCur ? cur : target, 0, 'f', 2).arg(target, 0, 'f', 2));
    emit logLine(ok ? "ok" : "warn",
                 QStringLiteral("%1: → %2x  rc=%3").arg(action).arg(target, 0, 'f', 2).arg(rc));
}

void CameraWorker::cmdSetFov(int fovType, const QString &label) {
    const QString a = QStringLiteral("fov");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ fov %1").arg(label));
    const int rc = m_dev->cameraSetFovU(static_cast<Device::FovType>(fovType));
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, label);
    emit logLine(ok ? "ok" : "warn", QStringLiteral("fov %1  rc=%2").arg(label).arg(rc));
    statusPulse();
}

void CameraWorker::cmdSetAi(int mode, int subMode, const QString &action) {
    if (!requireDevice(action)) return;
    emit logLine("cmd", QStringLiteral("→ %1").arg(action));
    // For Human tracking, subMode is the AiSubModeType framing (Normal/Upper/Close-up).
    const int rc = m_dev->cameraSetAiModeU(static_cast<Device::AiWorkModeType>(mode), subMode);
    const bool ok = (rc == RM_RET_OK);
    if (ok) {
        m_aiTracking = (mode != Device::AiWorkModeNone);   // optimistic; status push confirms
        // Open the matching one-shot stale-push grace window (see onSdkStatus).
        if (mode == Device::AiWorkModeNone) {
            m_aiOffGrace.start();
            m_aiOnGrace.invalidate();
        } else {
            m_aiOnGrace.start();
            m_lastAiOnMode = mode;
            m_aiOffGrace.invalidate();
        }
    }
    emit commandResult(action, ok, rc, ok ? QStringLiteral("applied") : QStringLiteral("rejected"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("%1  rc=%2").arg(action).arg(rc));
    statusPulse();   // AI mode change should reach the UI promptly in low-traffic mode
}

void CameraWorker::cmdSetFace(bool on) {
    const QString a = QStringLiteral("face focus");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ face focus %1").arg(on ? "on" : "off"));
    const int rc = m_dev->cameraSetFaceFocusR(on);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on") : QStringLiteral("off"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("face focus %1  rc=%2").arg(on ? "on" : "off").arg(rc));
}

void CameraWorker::cmdSetGesture(bool on, bool lowTraffic) {
    const QString a = QStringLiteral("gesture");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ gesture %1").arg(on ? "on" : "off"));
    // The Tiny 3 answers the "tail2 and later" DevGestureParaType API (master
    // switch + target-select), so both are set here. BUT the legacy
    // aiSetGestureCtrlIndividualR is ALSO driven unconditionally: it was the
    // ONLY call sent in v0.1.0–v0.2.5 — the era when gesture actually worked
    // on this device (flaky but real) — and v0.2.6 demoted it to a fallback
    // that never fired (the para calls return OK). If the recognizer reads
    // the legacy config store, that starved it. The device may keep TWO
    // stores — write both, read both back, every time.
    int rc = m_dev->aiSetGestureParaR(Device::DevGestureParaTypeGesture, on);
    const int rcSel = m_dev->aiSetGestureParaR(Device::DevGestureParaTypeTargetSelection, on);
    const int rcLegacy = m_dev->aiSetGestureCtrlIndividualR(0, on);
    // Also drive the legacy ZOOM (1) and DYNAMIC-ZOOM (2) switches: the round-7
    // readbacks showed the legacy store at dynamic_zoom=0 while the para store
    // said on — because this app had only ever written legacy type 0. The
    // quiet-test proved the autonomous path follows the LEGACY store, so the
    // L-form zoom gesture needs these two flipped here as well.
    m_dev->aiSetGestureCtrlIndividualR(1, on);
    m_dev->aiSetGestureCtrlIndividualR(2, on);
    // Honest readback of the WHOLE gesture parameter table — rc=0 alone proved
    // meaningless for gesture on this device, and the flaky history (works in
    // some sessions, not others, same code) means we need to SEE the full
    // device-side state every time, not just the two switches we set.
    static const char *kParaNames[] = {"function", "target-select", "zoom",
                                       "dynamic-zoom", "record", "snapshot",
                                       "rolling", "mirror"};
    bool paraFn = false, paraTarget = false;
    bool paraFnOk = false, paraTargetOk = false;
    QStringList parts;
    for (int t = Device::DevGestureParaTypeGesture; t <= Device::DevGestureParaTypeMirror; ++t) {
        bool v = false;
        if (m_dev->aiGetGestureParaR(static_cast<Device::DevGestureParaType>(t), v) == RM_RET_OK) {
            parts << QStringLiteral("%1=%2").arg(kParaNames[t], v ? "on" : "off");
            if (t == Device::DevGestureParaTypeGesture) { paraFn = v; paraFnOk = true; }
            if (t == Device::DevGestureParaTypeTargetSelection) { paraTarget = v; paraTargetOk = true; }
        }
    }
    float zf = 0.0f;
    if (m_dev->aiGetGestureParaR(Device::DevGestureParaTypeZoomFactor, zf) == RM_RET_OK)
        parts << QStringLiteral("zoom-factor=%1").arg(double(zf));
    if (!parts.isEmpty())
        emit logLine("sys", QStringLiteral("gesture readback: %1").arg(parts.join(QStringLiteral(", "))));
    // Legacy store readback (AiStatus) — if this ever disagrees with the para
    // table above, the two-store theory is confirmed and we know which store
    // the recognizer actually honors.
    Device::AiStatus ai{};
    const bool legacyReadable = (m_dev->aiGetAiStatusR(&ai) == RM_RET_OK);
    if (legacyReadable)
        emit logLine("sys", QStringLiteral("gesture legacy store: target=%1, zoom=%2, dynamic-zoom=%3, mirror=%4, zoom-factor=%5")
                                .arg(ai.gesture_target ? "on" : "off",
                                     ai.gesture_zoom ? "on" : "off",
                                     ai.gesture_dynamic_zoom ? "on" : "off",
                                     ai.gesture_mirror ? "on" : "off")
                                .arg(double(ai.gesture_zoom_factor)));
    else
        emit logLine("sys", QStringLiteral("gesture legacy store: not readable on this device"));
    // Success = the DEVICE says the state matches the intent (review finding:
    // OR-ing the three write rcs confirmed the chip ON even when the master
    // switch write failed). Readback is the ground truth; the raw rcs are the
    // fallback only when nothing is readable.
    const bool paraMatch = paraFnOk && paraTargetOk && (paraFn == on) && (paraTarget == on);
    const bool legacyMatch = legacyReadable && (ai.gesture_target == on);
    const bool ok = (paraFnOk || paraTargetOk || legacyReadable)
                        ? (paraMatch || legacyMatch)
                        : (rc == RM_RET_OK || rcSel == RM_RET_OK || rcLegacy == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on") : QStringLiteral("off"));
    emit logLine(ok ? "ok" : "warn",
                 QStringLiteral("gesture %1  rc=%2/%3 (para fn/target), rc=%4 (legacy)%5")
                     .arg(on ? "on" : "off").arg(rc).arg(rcSel).arg(rcLegacy)
                     .arg(ok ? QString() : QStringLiteral(" — device state does NOT match")));
    // Recognizer-suppression mitigation (hardware-confirmed by the quiet-test):
    // OPT-IN low-traffic status cadence while gesture is on. Gated on the user
    // setting so default behavior stays unchanged (Rex: keep it toggleable to
    // A/B test across firmware updates). Gesture off always restores normal.
    if (ok)
        setGestureFriendly(on && lowTraffic);
}

void CameraWorker::cmdSetHdr(bool on) {
    const QString a = QStringLiteral("hdr");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ hdr %1").arg(on ? "on" : "off"));
    // DevWdrMode: None(0)=off, Dol2TO1(1)=HDR on.
    const int rc = m_dev->cameraSetWdrR(on ? 1 : 0);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on") : QStringLiteral("off"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("hdr %1  rc=%2").arg(on ? "on" : "off").arg(rc));
}

void CameraWorker::cmdSetAutoSleep(int seconds, const QString &label) {
    const QString a = QStringLiteral("auto sleep");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ auto sleep: %1").arg(label));
    // <=0 disables automatic sleep entirely (SDK: "negative value or 0").
    const int rc = m_dev->cameraSetSuspendTimeU(seconds);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, label);
    emit logLine(ok ? "ok" : "warn", QStringLiteral("auto sleep %1  rc=%2%3")
                                         .arg(label).arg(rc)
                                         .arg(ok ? QString()
                                                 : QStringLiteral(" — device may not support this (SDK docs omit the Tiny 3)")));
    statusPulse();   // fetch the auto_sleep_time readback promptly in low-traffic mode
}

void CameraWorker::cmdSetMicSleep(bool on) {
    const QString a = QStringLiteral("mic in sleep");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ mic during sleep: %1").arg(on ? "on" : "muted"));
    const int rc = m_dev->cameraSetMicrophoneDuringSleepU(on ? 1 : 0);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on") : QStringLiteral("muted"));
    // The status push's sleep_micro field is the honest readback — watch the
    // "device reports" line on the Presets page / next status refresh.
    emit logLine(ok ? "ok" : "warn", QStringLiteral("mic during sleep %1  rc=%2%3")
                                         .arg(on ? "on" : "muted").arg(rc)
                                         .arg(ok ? QString()
                                                 : QStringLiteral(" — device may not support this (SDK docs omit the Tiny 3)")));
    statusPulse();   // fetch the sleep_micro readback promptly in low-traffic mode
}

void CameraWorker::cmdSetImage(const QString &param, int value) {
    if (!requireDevice(param)) return;
    const int v = value < 0 ? 0 : (value > 100 ? 100 : value);   // SDK range 0–100
    int rc = RM_RET_ERR;
    if (param == QLatin1String("brightness"))      rc = m_dev->cameraSetImageBrightnessR(v);
    else if (param == QLatin1String("contrast"))   rc = m_dev->cameraSetImageContrastR(v);
    else if (param == QLatin1String("saturation")) rc = m_dev->cameraSetImageSaturationR(v);
    else if (param == QLatin1String("sharpness"))  rc = m_dev->cameraSetImageSharpR(v);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(param, ok, rc, QString::number(v));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("%1 = %2  rc=%3").arg(param).arg(v).arg(rc));
}

void CameraWorker::cmdReadImageParams() {
    if (!m_dev) return;
    int b = 50, c = 50, s = 50, sh = 50;
    m_dev->cameraGetImageBrightnessR(b);
    m_dev->cameraGetImageContrastR(c);
    m_dev->cameraGetImageSaturationR(s);
    m_dev->cameraGetImageSharpR(sh);
    emit imageParams(b, c, s, sh);
}

void CameraWorker::cmdPresetCapture(int idx) {
    const QString a = QStringLiteral("preset %1 save").arg(idx + 1);
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ %1").arg(a));
    float xyz[3] = {0, 0, 0};
    if (m_dev->gimbalGetAttitudeInfoR(xyz) != RM_RET_OK) {
        emit commandResult(a, false, -1, QStringLiteral("read attitude failed"));
        emit logLine("warn", a + QStringLiteral(": read attitude failed"));
        return;
    }
    float z = 1.0f;
    m_dev->cameraGetZoomAbsoluteR(z);   // best-effort; falls back to 1.0
    // fov = -1 → CameraController fills in its current FOV index.
    emit presetCaptured(idx, xyz[1], xyz[2], z, -1);
    emit commandResult(a, true, 0, QStringLiteral("captured pitch %1, yaw %2, %3x")
                                       .arg(xyz[1], 0, 'f', 1).arg(xyz[2], 0, 'f', 1).arg(z, 0, 'f', 2));
    emit logLine("ok", a + QStringLiteral(": captured"));
}

void CameraWorker::cmdPresetGo(int idx, double pitch, double yaw, double zoom, int fov, double speed) {
    const QString a = QStringLiteral("preset %1 go").arg(idx + 1);
    if (!requireDevice(a)) return;
    if (aiOwnsGimbal(a)) return;                 // CODE_REVIEW #4
    emit logLine("cmd", QStringLiteral("→ %1").arg(a));
    const float np = clampf(static_cast<float>(pitch), kPitchMin, kPitchMax);
    const float ny = clampf(static_cast<float>(yaw), kYawMin, kYawMax);
    const int rc = m_dev->gimbalSetSpeedPositionR(0.0f, np, ny, 0.0f,
                                                  static_cast<float>(speed), static_cast<float>(speed));
    if (fov >= Device::FovType86 && fov <= Device::FovType65)   // never cast junk into the enum
        m_dev->cameraSetFovU(static_cast<Device::FovType>(fov));
    const float tz = clampf(static_cast<float>(zoom), 1.0f, 2.0f);
    m_dev->cameraSetZoomAbsoluteR(tz);
    refreshZoom();
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc,
                       QStringLiteral("pitch %1, yaw %2, %3x").arg(np, 0, 'f', 1).arg(ny, 0, 'f', 1).arg(tz, 0, 'f', 2));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("%1  rc=%2").arg(a).arg(rc));
    statusPulse();
}

// ---------------------------------------------------------------------------
// Wireless mic (OBSBOT Vox SE)
//
// Presence is handled entirely by onSdkStatus (tiny.wireless_mic). What is left
// is the detail read and the button assignment, both of which go through
// ObsbotTwsCompat.h — libdev exports them, the public header does not declare
// them, so they are bound by symbol name and probed at runtime.
// ---------------------------------------------------------------------------

// SILENT — no requireDevice, no logLine: this runs on bind and on a 60 s timer.
// Doubles as the capability probe: rc == RM_RET_OK means this firmware answers
// the undocumented TWS API, and only then are the extras advertised to the UI.
void CameraWorker::cmdReadTwsInfo() { readTwsInfo(); }

// The body of cmdReadTwsInfo, plus the one thing the confirm poller needs back:
// the key_cmd the camera just reported (-1 when it could not be read). Kept as a
// plain helper rather than giving the slot a return value, so the queued
// invocations elsewhere stay ordinary void calls.
int CameraWorker::readTwsInfo() {
    if (!m_dev) return -1;
    Device::DevTWSInfo info{};
    const int rc = ObsbotTws::getInfo(m_dev.get(), info);
    const bool ok = (rc == RM_RET_OK);
    if (!m_twsProbed || ok != m_twsSupported) {
        m_twsProbed = true;
        m_twsSupported = ok;
        emit logLine(ok ? "sys" : "warn",
                     ok ? QStringLiteral("wireless mic: extended mic API available (cameraGetTWSInfoR)")
                        : QStringLiteral("wireless mic: extended mic API unavailable on this camera/firmware "
                                         "(cameraGetTWSInfoR rc=%1) — battery and button assignment disabled").arg(rc));
    }
    if (!ok) {
        emit twsInfo(false, -1, -1, false, false, -1, false, false);
        emit twsMicAudio(false, kTxGainUnknown, -1, -1, kTxGainUnknown, -1, -1);
        return -1;
    }
    emit twsInfo(true, static_cast<int>(info.key_cmd),
                 static_cast<int>(info.mic1_batt_level), info.mic1_chg_status != 0, info.mic_info.mic1_mute != 0,
                 static_cast<int>(info.mic2_batt_level), info.mic2_chg_status != 0, info.mic_info.mic2_mute != 0);
    // Same struct, second signal — see the header for why these are kept apart.
    // The gain and ns_level bytes are passed through EXACTLY as the device
    // reported them (both are int8_t): no rescaling, no "helpful" clamp to a
    // range nobody has verified.
    emit twsMicAudio(ObsbotTws::txAudioLinked(),
                     static_cast<int>(info.mic1_gain), info.mic_info.mic1_ns != 0 ? 1 : 0,
                     static_cast<int>(info.mic1_ns_level),
                     static_cast<int>(info.mic2_gain), info.mic_info.mic2_ns != 0 ? 1 : 0,
                     static_cast<int>(info.mic2_ns_level));
    return static_cast<int>(info.key_cmd);
}

// PER-MIC mute. Distinct from cmdSetAudioMute, which mutes whichever SOURCE the
// camera is listening to: this is the Vox SE's own mute, the same state the mic's
// button toggles, and it survives a source change.
void CameraWorker::cmdSetTxMute(int tx, bool muted) {
    const QString a = QStringLiteral("mic mute tx%1").arg(tx);
    if (!requireDevice(a)) return;
    if (tx != Device::DevTX1 && tx != Device::DevTX2) {
        emit commandResult(a, false, -1, QStringLiteral("invalid transmitter slot"));
        emit logLine("warn", a + QStringLiteral(": invalid transmitter slot %1 (expected 1 or 2)").arg(tx));
        return;
    }
    emit logLine("cmd", QStringLiteral("→ %1 (%2)").arg(a, muted ? "muted" : "live"));
    const auto slot = static_cast<Device::DevTXType>(tx);   // DevTXType is ONE-based
    const int rc = ObsbotTws::setTxMute(m_dev.get(), slot, muted);
    const bool ok = (rc == RM_RET_OK);
    // rc is not proof — ask the device what it actually is now.
    bool nowMuted = muted;
    const bool readOk = (ObsbotTws::getTxMute(m_dev.get(), slot, nowMuted) == RM_RET_OK);
    emit commandResult(a, ok, rc,
                       ok ? (readOk ? (nowMuted ? QStringLiteral("muted") : QStringLiteral("live"))
                                    : QStringLiteral("sent — the mic did not report back"))
                          : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("%1  rc=%2%3").arg(a).arg(rc)
                                         .arg(ok && readOk && nowMuted != muted
                                                  ? QStringLiteral(" — but the mic reports %1")
                                                        .arg(nowMuted ? "muted" : "live")
                                                  : QString()));
    cmdReadTwsInfo();   // the mic cards read from DevTWSInfo, so refresh that too
}

// PER-MIC gain, in the device's OWN units.
//
// The SDK documents no range for this and the app must not invent one: `gain` is
// whatever the camera last reported for this slot, plus or minus the UI's step,
// clamped ONLY to the int8 that DevTWSInfo::micN_gain can carry back. The
// readback below is what makes that honest — if the camera clamps the value to
// its real limits, the log says where it actually landed instead of the UI
// pretending the request took.
void CameraWorker::cmdSetTxGain(int tx, int gain) {
    const QString a = QStringLiteral("mic gain tx%1").arg(tx);
    if (!requireDevice(a)) return;
    if (tx != Device::DevTX1 && tx != Device::DevTX2) {
        emit commandResult(a, false, -1, QStringLiteral("invalid transmitter slot"));
        emit logLine("warn", a + QStringLiteral(": invalid transmitter slot %1 (expected 1 or 2)").arg(tx));
        return;
    }
    const int g = gain < kTxGainMin ? kTxGainMin : (gain > kTxGainMax ? kTxGainMax : gain);
    emit logLine("cmd", QStringLiteral("→ %1 = %2 (device units — the SDK documents no range)").arg(a).arg(g));
    const auto slot = static_cast<Device::DevTXType>(tx);
    const int rc = ObsbotTws::setTxGain(m_dev.get(), slot, g);
    const bool ok = (rc == RM_RET_OK);
    int nowGain = g;
    const bool readOk = (ObsbotTws::getTxGain(m_dev.get(), slot, nowGain) == RM_RET_OK);
    emit commandResult(a, ok, rc,
                       ok ? (readOk ? QString::number(nowGain) : QStringLiteral("sent — no readback"))
                          : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn",
                 QStringLiteral("%1 = %2  rc=%3%4").arg(a).arg(g).arg(rc)
                     .arg(ok && readOk && nowGain != g
                              ? QStringLiteral(" — the mic settled at %1 (a real bound of the "
                                               "undocumented range)").arg(nowGain)
                              : QString()));
    cmdReadTwsInfo();
}

// Assign the mic's multi-function button. Device::DevTWSKeyType maps 1:1 onto
// the UI order (Track / Switch mode / Zoom 1x / Record), so the index IS the
// enum value — no translation table, just a range check so nothing but a valid
// enumerator is ever cast.
void CameraWorker::cmdSetMicButtonAction(int idx) {
    const QString a = QStringLiteral("mic button");
    if (!requireDevice(a)) return;
    if (idx < Device::DevTWSKeyTrack || idx > Device::DevTWSKeyRecord) {
        emit commandResult(a, false, -1, QStringLiteral("invalid action %1").arg(idx));
        emit logLine("warn", a + QStringLiteral(": invalid action %1 (expected 0..3)").arg(idx));
        return;
    }
    emit logLine("cmd", QStringLiteral("→ mic button = %1").arg(idx));
    const int rc = ObsbotTws::setKeyType(m_dev.get(), static_cast<Device::DevTWSKeyType>(idx));
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? QStringLiteral("applied") : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("mic button = %1  rc=%2").arg(idx).arg(rc));
    statusPulse();
    // Confirm from the device rather than trusting rc alone — but POLL for it.
    // The camera goes on reporting the old assignment for anywhere between a
    // third of a second and three seconds (see kKeyConfirmIntervalMs), so a
    // single read here is a coin flip that almost always lands wrong.
    if (ok) startKeyConfirm(idx);
    else    cmdReadTwsInfo();
}

// Poll cameraGetTWSInfoR until the camera reports `target`, or the budget runs
// out. Every tick emits the usual twsInfo signal, so the UI clears its
// optimistic override the moment the device agrees — and a late-but-successful
// write is still caught instead of being reported as a failure.
void CameraWorker::startKeyConfirm(int target) {
    m_keyConfirmTarget = target;
    m_keyConfirmTicksLeft = kKeyConfirmBudgetMs / kKeyConfirmIntervalMs;
    if (!m_keyConfirmTimer) {
        m_keyConfirmTimer = new QTimer(this);
        m_keyConfirmTimer->setInterval(kKeyConfirmIntervalMs);
        connect(m_keyConfirmTimer, &QTimer::timeout, this, &CameraWorker::onKeyConfirmTick);
    }
    m_keyConfirmTimer->start();
}

void CameraWorker::onKeyConfirmTick() {
    if (m_shuttingDown || !m_dev) { m_keyConfirmTimer->stop(); return; }
    const int got = readTwsInfo();
    if (got == m_keyConfirmTarget) {
        m_keyConfirmTimer->stop();
        return;
    }
    if (--m_keyConfirmTicksLeft <= 0) {
        m_keyConfirmTimer->stop();
        emit logLine("warn", QStringLiteral("mic button: camera still reports %1 after %2 ms "
                                            "(asked for %3)")
                                 .arg(got).arg(kKeyConfirmBudgetMs).arg(m_keyConfirmTarget));
    }
}

// Two readbacks in one cheap call. has_pair_record == 0 means this camera has
// never had a wireless mic paired to it, which is the usual reason both slots
// read "not connected" forever. is_auto is the camera's source ARBITRATION flag
// — load-bearing for the Audio section, since while it is on a manual source
// pick is accepted and then reverted. Silent (no logLine on the failure path) —
// it runs on bind and after pair/clear/source changes, never on a timer.
void CameraWorker::cmdReadAudioSelect() {
    if (!m_dev) return;
    Device::AudioSelectAttr audio{};
    if (ObsbotTws::getAudioSelect(m_dev.get(), audio) != RM_RET_OK) {
        emit micAudioSelect(-1, -1, -1);
        return;
    }
    emit micAudioSelect(audio.has_pair_record ? 1 : 0, audio.is_auto ? 1 : 0, audio.support_auto ? 1 : 0);
    // Second opinion on the source, for the log only. The UI is driven by the
    // status push's tiny.audio_mode.source; this getter is the independent way
    // to tell a genuine "built-in" from a push field the firmware never fills
    // in, without putting a second reader on the UI path.
    unsigned char selected = 0;
    const bool selOk = (ObsbotTws::getSelectedAudioSource(m_dev.get(), selected) == RM_RET_OK);
    emit logLine("sys", QStringLiteral("audio: source %1 (auto source-select %2, supported %3); "
                                       "wireless-mic pairing record %4")
                            .arg(selOk ? QString::fromLatin1(audioSourceName(selected))
                                       : QStringLiteral("unreported"),
                                 audio.is_auto ? QStringLiteral("on") : QStringLiteral("off"),
                                 audio.support_auto ? QStringLiteral("yes") : QStringLiteral("no"),
                                 audio.has_pair_record
                                     ? QStringLiteral("present")
                                     : QStringLiteral("NONE — no mic has ever been paired to this camera")));
}

// The camera must be AWAKE for a pairing command to do anything: asleep it
// answers rc=0 and never powers its mic radio (hardware finding — the reason
// pairing was first, wrongly, written off as unsupported). Returns TRUE if the
// camera was ALREADY awake; FALSE means a wake was just issued and the caller
// should let it settle before touching the radio.
bool CameraWorker::wakeForMic(const QString &action) {
    if (!m_dev) return true;   // caller already bailed via requireDevice
    if (m_dev->cameraStatus().tiny.dev_status == Device::DevStatusRun) return true;
    emit logLine("warn", action + QStringLiteral(": camera was asleep — woke it first "
                                                 "(a sleeping Tiny 3 accepts pairing commands it never acts on)"));
    const int rc = m_dev->cameraSetDevRunStatusR(Device::DevStatusRun);
    if (rc != RM_RET_OK)
        emit logLine("warn", action + QStringLiteral(": wake rc=%1 — pairing may not take").arg(rc));
    statusPulse();
    return false;
}

// Put a transmitter slot into pairing mode. tx is 1-based and IS the SDK's
// DevTXType value (DevTX1 = 1). Wakes the camera first when needed, then runs
// the real command after a short settle — never blocking the worker's loop.
void CameraWorker::cmdTxPair(int tx, bool enable) {
    const QString a = QStringLiteral("mic pair tx%1").arg(tx);
    if (!requireDevice(a)) return;
    if (tx != Device::DevTX1 && tx != Device::DevTX2) {
        emit commandResult(a, false, -1, QStringLiteral("invalid transmitter slot"));
        emit logLine("warn", a + QStringLiteral(": invalid transmitter slot %1 (expected 1 or 2)").arg(tx));
        return;
    }
    emit logLine("cmd", QStringLiteral("→ %1 (%2)").arg(a, enable ? "enable" : "disable"));
    if (wakeForMic(a)) {
        sendTxPair(tx, enable);
    } else {
        // Give the wake a moment before the radio command. Context object is
        // `this`, so a torn-down worker cancels the pending fire.
        QTimer::singleShot(kPairWakeSettleMs, this, [this, tx, enable]() {
            if (m_shuttingDown || !m_dev) return;
            sendTxPair(tx, enable);
        });
    }
}

void CameraWorker::sendTxPair(int tx, bool enable) {
    const QString a = QStringLiteral("mic pair tx%1").arg(tx);
    if (!requireDevice(a)) return;
    const auto slot = static_cast<Device::DevTXType>(tx);   // DevTXType is ONE-based
    // Belt-and-braces: open/close the camera's generic BLE pairing window around
    // the TX call. Both return rc=0 on a Tiny 3 but pairing has been observed to
    // work without them, so their result is logged, never acted on.
    if (enable) ObsbotTws::blePairingEnable(m_dev.get(), true, 1);
    const int rc = ObsbotTws::setPairEnabled(m_dev.get(), slot, enable);
    if (!enable) ObsbotTws::blePairingExit(m_dev.get());
    const bool ok = (rc == RM_RET_OK);
    // rc is NOT proof: the honest confirmation is wireless_mic.tx_state going
    // non-zero in the status push, which lands in micStatus a few seconds later.
    emit commandResult(a, ok, rc,
                       ok ? (enable ? QStringLiteral("pairing open — hold the mic's button until it flashes green")
                                    : QStringLiteral("pairing closed"))
                          : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("%1  rc=%2%3").arg(a).arg(rc)
                                         .arg(ok && enable ? QStringLiteral(" — waiting for the camera to report the link")
                                                           : QString()));
    statusPulse();       // pull the tx_state readback forward in low-traffic mode
    cmdReadAudioSelect();
}

void CameraWorker::cmdTxClear(int tx) {
    const QString a = QStringLiteral("mic clear tx%1").arg(tx);
    if (!requireDevice(a)) return;
    if (tx != Device::DevTX1 && tx != Device::DevTX2) {
        emit commandResult(a, false, -1, QStringLiteral("invalid transmitter slot"));
        emit logLine("warn", a + QStringLiteral(": invalid transmitter slot %1 (expected 1 or 2)").arg(tx));
        return;
    }
    emit logLine("cmd", QStringLiteral("→ %1").arg(a));
    wakeForMic(a);   // same asleep-ACKs-but-ignores trap as pairing
    const int rc = ObsbotTws::clearPairedInfo(m_dev.get(), static_cast<Device::DevTXType>(tx));
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? QStringLiteral("cleared") : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("%1  rc=%2").arg(a).arg(rc));
    statusPulse();
    cmdReadAudioSelect();
}

// ---------------------------------------------------------------------------
// The camera's OWN microphone (Tiny 3 mic array)
//
// Two readback paths, and it matters which is which:
//   * SOURCE and pickup MODE come from the status push (tiny.audio_mode) — a
//     public, documented bitfield. Neither has an exported getter, so the push
//     IS the confirmation and it lags a command by up to one push cycle.
//   * volume / mute / noise reduction / AGC come from getters on the control
//     channel and answer immediately, which is why cmdReadAudio runs right
//     after every successful set: rc == 0 is not proof that a setting took.
//
// Nothing here is polled. The gesture work established that periodic control-
// channel traffic suppresses the camera's recognizer, and none of these values
// changes behind the app's back, so a timer would cost recognition for nothing.
// ---------------------------------------------------------------------------

// SILENT — no requireDevice, no logLine on the happy path: this runs on bind and
// after every successful audio set. Doubles as the capability probe, exactly
// like cmdReadTwsInfo: rc == RM_RET_OK from cameraGetAudioVolumeR means this
// firmware answers the camera-audio API, and only then does the UI enable the
// Audio section. Each sub-getter reports independently — one failing leg shows
// as "—" instead of poisoning the whole read.
void CameraWorker::cmdReadAudio() {
    if (!m_dev) return;
    short vol = 0;
    const int rc = ObsbotTws::getAudioVolume(m_dev.get(), vol);
    const bool ok = (rc == RM_RET_OK);
    if (!m_audioProbed || ok != m_audioSupported) {
        m_audioProbed = true;
        m_audioSupported = ok;
        emit logLine(ok ? "sys" : "warn",
                     ok ? QStringLiteral("audio: camera-audio API available (cameraGetAudioVolumeR)")
                        : QStringLiteral("audio: camera-audio API unavailable on this camera/firmware "
                                         "(cameraGetAudioVolumeR rc=%1) — the Audio controls are disabled").arg(rc));
    }
    if (!ok) {
        emit audioState(false, -1, -1, -1, -1, -1);
        return;
    }
    bool muted = false;
    const bool muteOk = (ObsbotTws::getAudioMute(m_dev.get(), muted) == RM_RET_OK);
    bool nrOn = false;
    int nrLevel = 0;
    const bool nrOk = (ObsbotTws::getAudioNoiseReduce(m_dev.get(), nrOn, nrLevel) == RM_RET_OK);
    bool agcOn = false;
    const bool agcOk = (ObsbotTws::getAudioAgc(m_dev.get(), agcOn) == RM_RET_OK);
    const int v = vol < 0 ? -1 : (vol > 100 ? 100 : static_cast<int>(vol));
    emit audioState(true, v,
                    muteOk ? (muted ? 1 : 0) : -1,
                    nrOk ? (nrOn ? 1 : 0) : -1,
                    nrOk ? nrLevel : -1,
                    agcOk ? (agcOn ? 1 : 0) : -1);
}

void CameraWorker::cmdSetAudioVolume(int volume) {
    const QString a = QStringLiteral("audio volume");
    if (!requireDevice(a)) return;
    const int v = volume < 0 ? 0 : (volume > 100 ? 100 : volume);   // SDK range 0–100
    emit logLine("cmd", QStringLiteral("→ audio volume = %1").arg(v));
    const int rc = ObsbotTws::setAudioVolume(m_dev.get(), static_cast<short>(v));
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, QString::number(v));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("audio volume = %1  rc=%2").arg(v).arg(rc));
    // No statusPulse: this value is NOT in the status push. The getter below is
    // the readback, and it answers on the same round trip.
    cmdReadAudio();
}

void CameraWorker::cmdSetAudioMute(bool muted) {
    const QString a = QStringLiteral("audio mute");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ audio %1").arg(muted ? "mute" : "unmute"));
    const int rc = ObsbotTws::setAudioMute(m_dev.get(), muted);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, muted ? QStringLiteral("muted") : QStringLiteral("live"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("audio %1  rc=%2").arg(muted ? "muted" : "live").arg(rc));
    cmdReadAudio();
}

void CameraWorker::cmdSetAudioNoiseReduce(bool on, int level) {
    const QString a = QStringLiteral("audio noise reduction");
    if (!requireDevice(a)) return;
    const int lv = level < 1 ? 1 : (level > 10 ? 10 : level);   // documented range [1-10]
    emit logLine("cmd", QStringLiteral("→ noise reduction %1 (level %2)").arg(on ? "on" : "off").arg(lv));
    const int rc = ObsbotTws::setAudioNoiseReduce(m_dev.get(), on, lv);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on, level %1").arg(lv) : QStringLiteral("off"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("noise reduction %1 level %2  rc=%3")
                                         .arg(on ? "on" : "off").arg(lv).arg(rc));
    cmdReadAudio();
}

void CameraWorker::cmdSetAudioAgc(bool on) {
    const QString a = QStringLiteral("audio agc");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ automatic gain %1").arg(on ? "on" : "off"));
    const int rc = ObsbotTws::setAudioAgc(m_dev.get(), on);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on") : QStringLiteral("off"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("automatic gain %1  rc=%2").arg(on ? "on" : "off").arg(rc));
    cmdReadAudio();
}

// Pickup pattern. Device::AudioMode's `source` field is annotated "use 0 for
// now" in the public header, so ObsbotTws::setAudioMode pins it to 0 and only
// `mode` carries meaning. There is no getter: the readback is the status push's
// tiny.audio_mode.mode, hence statusPulse rather than a re-read.
void CameraWorker::cmdSetAudioMode(int mode) {
    const QString a = QStringLiteral("audio pickup");
    if (!requireDevice(a)) return;
    if (mode < Device::AudioModeOmni || mode >= Device::AudioModeButt) {
        emit commandResult(a, false, -1, QStringLiteral("invalid pattern %1").arg(mode));
        emit logLine("warn", a + QStringLiteral(": invalid pattern %1 (expected 0..%2)")
                                     .arg(mode).arg(Device::AudioModeButt - 1));
        return;
    }
    emit logLine("cmd", QStringLiteral("→ audio pickup pattern = %1").arg(mode));
    const int rc = ObsbotTws::setAudioMode(m_dev.get(), mode);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? QStringLiteral("applied") : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("audio pickup pattern = %1  rc=%2").arg(mode).arg(rc));
    statusPulse();   // the honest confirmation is tiny.audio_mode.mode in the next push
}

// Pin the input source.
//
// HARDWARE FINDING, the whole reason this is one command and not two: while the
// camera's own arbitration is on (AudioSelectAttr::is_auto == 1), a source pick
// is ACCEPTED with rc=0 and then silently reverted a moment later. Selecting a
// source therefore IMPLIES turning automatic selection off — done here, in this
// order, and said out loud in the log so the user is never surprised by a
// setting they did not knowingly change.
void CameraWorker::cmdSetAudioSource(int source) {
    const QString a = QStringLiteral("audio source");
    if (!requireDevice(a)) return;
    if (source < Device::DevAudioSourceTypeBuildIn || source > Device::DevAudioSourceTypeUsbC) {
        emit commandResult(a, false, -1, QStringLiteral("invalid source %1").arg(source));
        emit logLine("warn", a + QStringLiteral(": invalid source %1 (expected 0..%2)")
                                     .arg(source).arg(static_cast<int>(Device::DevAudioSourceTypeUsbC)));
        return;
    }
    const QString name = QString::fromLatin1(audioSourceName(source));
    emit logLine("cmd", QStringLiteral("→ audio source = %1 (automatic selection turned off first — "
                                       "with it on the camera accepts a manual pick and then reverts it)")
                            .arg(name));
    const int autoRc = ObsbotTws::setAudioSelect(m_dev.get(), false);
    if (autoRc != RM_RET_OK)
        emit logLine("warn", a + QStringLiteral(": could not turn automatic selection off (rc=%1) — "
                                                "the camera may revert this pick").arg(autoRc));
    const int rc = ObsbotTws::setAudioSource(m_dev.get(), source);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, ok ? name : QStringLiteral("failed"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("audio source = %1  rc=%2%3").arg(name).arg(rc)
                                         .arg(ok ? QStringLiteral(" — waiting for the camera to report it")
                                                 : QString()));
    statusPulse();          // tiny.audio_mode.source is the honest confirmation
    cmdReadAudioSelect();   // …and this confirms the auto flag really cleared
}

void CameraWorker::cmdSetAudioAuto(bool on) {
    const QString a = QStringLiteral("audio auto source");
    if (!requireDevice(a)) return;
    emit logLine("cmd", QStringLiteral("→ automatic audio-source selection %1").arg(on ? "on" : "off"));
    const int rc = ObsbotTws::setAudioSelect(m_dev.get(), on);
    const bool ok = (rc == RM_RET_OK);
    emit commandResult(a, ok, rc, on ? QStringLiteral("on") : QStringLiteral("off"));
    emit logLine(ok ? "ok" : "warn", QStringLiteral("automatic audio-source selection %1  rc=%2")
                                         .arg(on ? "on" : "off").arg(rc));
    statusPulse();          // the camera may switch source as a result
    cmdReadAudioSelect();   // is_auto readback — a direct getter, answers now
}

// ---------------------------------------------------------------------------
// PTZ velocity (hold-to-move) — see the header doc for the four safety stops.
// ---------------------------------------------------------------------------
void CameraWorker::cmdGimbalVelocity(double pitchSpeed, double yawSpeed) {
    if (!m_dev) return;   // silent: this is called on a repeating timer while dragging
    if (m_aiTracking) {
        // stop-on-(AI-owns-gimbal): honest, but only log once per drag, not per tick.
        if (!m_velocityBlockedLogged) {
            emit logLine("warn", QStringLiteral("ptz velocity: blocked — AI tracking owns the gimbal"));
            m_velocityBlockedLogged = true;
        }
        return;
    }
    m_velocityBlockedLogged = false;

    const int rc = m_dev->gimbalSpeedCtrlR(pitchSpeed, yawSpeed, 0.0);
    if (rc != RM_RET_OK) {
        // stop-on-error: never keep sending velocity after a rejected command.
        emit logLine("warn", QStringLiteral("ptz velocity: rc=%1 — stopping").arg(rc));
        m_dev->gimbalSpeedCtrlR(0.0, 0.0, 0.0);
        m_velocityActive = false;
        m_velocityWatchdog->stop();
        return;
    }
    if (!m_velocityActive) {
        m_velocityActive = true;
        emit logLine("cmd", QStringLiteral("→ ptz velocity: moving"));
    }
    m_velocityWatchdog->start();   // deadman: refreshed by every fresh command (restarts the interval)
}

void CameraWorker::cmdGimbalStop() {
    m_velocityWatchdog->stop();
    if (!m_velocityActive) return;   // avoid redundant calls/log spam
    m_velocityActive = false;
    if (m_dev) {
        const int rc = m_dev->gimbalSpeedCtrlR(0.0, 0.0, 0.0);
        emit logLine(rc == RM_RET_OK ? "ok" : "warn", QStringLiteral("ptz velocity: stop rc=%1").arg(rc));
    }
    statusPulse();   // post-drag: refresh guard/UI state promptly in low-traffic mode
}

void CameraWorker::statusPulse() {
    if (m_gestureFriendly && m_dev && !m_quiet && !m_shuttingDown) {
        m_awaitingDutyPush = true;
        m_dev->enableDevStatusCallback(true);   // onSdkStatus closes the window
    }
}

void CameraWorker::setGestureFriendly(bool on) {
    if (on == m_gestureFriendly) return;
    m_gestureFriendly = on;
    if (on) {
        // Close the always-on push now; the duty timer reopens it briefly every
        // kStatusDutyMs (and any user command still refreshes state as usual).
        if (m_dev && !m_quiet) m_dev->enableDevStatusCallback(false);
        m_statusDutyTimer->start();
        emit logLine("sys", QStringLiteral(
            "gesture low-traffic mode ACTIVE — status refresh slowed to one per %1 s "
            "(the normal polling suppresses the camera's gesture recognizer)")
                                .arg(kStatusDutyMs / 1000));
    } else {
        m_statusDutyTimer->stop();
        m_awaitingDutyPush = false;
        if (m_dev && !m_quiet) m_dev->enableDevStatusCallback(true);
        emit logLine("sys", QStringLiteral("status refresh back to normal cadence"));
    }
}

void CameraWorker::cmdQuietMode(int seconds) {
    const QString a = QStringLiteral("gesture quiet test");
    if (!requireDevice(a)) return;
    if (m_quiet) { emit logLine("warn", QStringLiteral("quiet test already running")); return; }
    m_quiet = true;
    m_dev->enableDevStatusCallback(false);   // stop the push; onSdkStatus also gates
    emit logLine("cmd", QStringLiteral(
        "quiet test: ALL periodic SDK traffic paused for %1 s — try palm gestures NOW "
        "(status/zoom display will freeze meanwhile)").arg(seconds));
    QTimer::singleShot(seconds * 1000, this, [this]() {
        m_quiet = false;
        // Resume to the cadence that matches the current mode: gesture-friendly
        // stays quiet (its duty timer reopens the window), normal re-enables.
        if (!m_shuttingDown && m_dev && !m_gestureFriendly)
            m_dev->enableDevStatusCallback(true);
        emit logLine("sys", QStringLiteral("quiet test: over — status traffic resumed"));
    });
}

// ---------------------------------------------------------------------------
// Deterministic teardown (CODE_REVIEW #1/#2/#3/#9)
// ---------------------------------------------------------------------------
void CameraWorker::shutdown() {
    m_shuttingDown = true;
    if (m_pollTimer) m_pollTimer->stop();
    if (m_statusDutyTimer) m_statusDutyTimer->stop();
    if (m_twsInfoTimer) m_twsInfoTimer->stop();
    if (m_velocityActive && m_dev) m_dev->gimbalSpeedCtrlR(0.0, 0.0, 0.0);   // stop any in-flight motion
    if (m_velocityWatchdog) m_velocityWatchdog->stop();
    if (m_dev) {
        // Disable the status push BEFORE dropping the device so no further
        // sdkStatusTrampoline fires against state we are tearing down. Same for
        // the device event callback, which captures `this` (setDevEventCallback
        // swaps in a no-op rather than an empty std::function — see its comment).
        m_dev->enableDevStatusCallback(false);
        setDevEventCallback(false);
        m_dev.reset();
    }
    // Stop the SDK's discovery task and drop the plug/unplug callback.
    Devices::get().close();
    emit logLine("sys", QStringLiteral("shutdown: SDK released"));
}
