// CameraController — the single QML-facing object (lives on the GUI thread).
//
// It owns the worker QThread, mirrors device state into Q_PROPERTYs that QML
// binds to, forwards user actions to the worker as queued calls, and persists
// settings/presets. It never makes a blocking SDK call itself.
//
// Design-honesty responsibilities implemented here:
//   * AI toggle revert-on-failure + resync for undecoded modes (CODE_REVIEW #5/#7)
//   * pending-suppression so a status push can't fight an in-flight AI toggle (#8)
//   * capability gating: only expose controls backed by a verified Tiny 3 SDK call
//   * deterministic shutdown of the worker thread (#1/#2/#3/#9)
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>

#include "Settings.h"

class CameraWorker;
class PreviewEngine;
class QTimer;
class QProcess;

class CameraController : public QObject {
    Q_OBJECT

    // ----- connection / identity -----
    Q_PROPERTY(int connState READ connState NOTIFY connStateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connStateChanged)
    Q_PROPERTY(bool discovering READ discovering NOTIFY connStateChanged)
    Q_PROPERTY(QString product MEMBER m_product NOTIFY identityChanged)
    Q_PROPERTY(QString sn MEMBER m_sn NOTIFY identityChanged)
    Q_PROPERTY(QString firmware MEMBER m_firmware NOTIFY identityChanged)
    Q_PROPERTY(QString mode MEMBER m_mode NOTIFY identityChanged)
    Q_PROPERTY(int enumId MEMBER m_enumId NOTIFY identityChanged)

    // ----- live device state -----
    Q_PROPERTY(int runState READ runState NOTIFY statusChanged)
    Q_PROPERTY(bool asleep READ asleep NOTIFY statusChanged)
    Q_PROPERTY(double zoom MEMBER m_zoom NOTIFY zoomChanged)
    Q_PROPERTY(bool zoomValid MEMBER m_zoomValid NOTIFY zoomChanged)
    Q_PROPERTY(int aiModeRaw MEMBER m_aiModeRaw NOTIFY aiChanged)
    Q_PROPERTY(QString aiModeName MEMBER m_aiModeName NOTIFY aiChanged)
    // AI Track = Human tracking (gimbal follows a person; LED blue). This is the
    // real, working "face/person tracking" on Tiny3. Face focus
    // (cameraSetFaceFocusR, autofocus-only, no gimbal motion) is a separate SDK
    // call, independent of tracking. (A "Face Track"/PortraitTrack mode was tried
    // and removed: on Tiny3 in landscape the device accepted then reverted it —
    // it is portrait-orientation tracking, not face tracking.)
    Q_PROPERTY(bool aiTracking READ aiTracking NOTIFY aiChanged)
    Q_PROPERTY(bool faceFocus READ faceFocus NOTIFY aiChanged)
    Q_PROPERTY(bool gesture READ gesture NOTIFY aiChanged)
    Q_PROPERTY(bool hdrOn READ hdrOn NOTIFY imageChanged)
    Q_PROPERTY(bool aiPending MEMBER m_aiPending NOTIFY aiChanged)

    // ----- persisted app settings (writable from QML) -----
    Q_PROPERTY(int moveStepDeg READ moveStepDeg WRITE setMoveStepDeg NOTIFY settingsChanged)
    Q_PROPERTY(int speedMode READ speedMode WRITE setSpeedMode NOTIFY settingsChanged)
    Q_PROPERTY(int fovIndex READ fovIndex WRITE setFovIndex NOTIFY settingsChanged)
    Q_PROPERTY(int startupPreset READ startupPreset WRITE setStartupPreset NOTIFY settingsChanged)
    Q_PROPERTY(int aiReturnPreset READ aiReturnPreset WRITE setAiReturnPreset NOTIFY settingsChanged)
    Q_PROPERTY(int previewResIndex READ previewResIndex WRITE setPreviewResIndex NOTIFY settingsChanged)
    Q_PROPERTY(QString previewRes READ previewRes NOTIFY settingsChanged)   // human-readable, for STATUS
    Q_PROPERTY(bool sleepOnExit READ sleepOnExit WRITE setSleepOnExit NOTIFY settingsChanged)
    // Experimental, OPT-IN: slow the status cadence while gesture control is on
    // (frequent polling suppresses the camera's gesture recognizer). Off by
    // default so normal behavior is unchanged; kept toggleable for A/B testing
    // across firmware updates.
    Q_PROPERTY(bool gestureLowTraffic READ gestureLowTraffic WRITE setGestureLowTraffic NOTIFY settingsChanged)
    // Device power/sleep behavior. Index 0 = "Device" (don't manage). SDK docs
    // omit the Tiny 3 for both underlying calls — micSleepDevice is the honest
    // device-reported readback (-1 until the first status push).
    Q_PROPERTY(int autoSleepIndex READ autoSleepIndex WRITE setAutoSleepIndex NOTIFY settingsChanged)
    Q_PROPERTY(int micSleepIndex READ micSleepIndex WRITE setMicSleepIndex NOTIFY settingsChanged)
    Q_PROPERTY(int micSleepDevice MEMBER m_micSleepDevice NOTIFY statusChanged)
    Q_PROPERTY(int autoSleepDevice MEMBER m_autoSleepDevice NOTIFY statusChanged)
    Q_PROPERTY(int fps MEMBER m_fps NOTIFY statusChanged)   // current video stream fps

    // ----- app meta -----
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

    // ----- image params (0–100, live device values) -----
    Q_PROPERTY(int brightness MEMBER m_brightness NOTIFY imageChanged)
    Q_PROPERTY(int contrast MEMBER m_contrast NOTIFY imageChanged)
    Q_PROPERTY(int saturation MEMBER m_saturation NOTIFY imageChanged)
    Q_PROPERTY(int sharpness MEMBER m_sharpness NOTIFY imageChanged)
    // White balance. wbMin/wbMax/wbStep are the DEVICE's reported range, not
    // constants — the UI binds the slider to them, so a camera with different
    // limits gets the right control instead of a hardcoded one.
    Q_PROPERTY(bool wbAuto MEMBER m_wbAuto NOTIFY whiteBalanceChanged)
    Q_PROPERTY(int wbKelvin MEMBER m_wbKelvin NOTIFY whiteBalanceChanged)
    Q_PROPERTY(int wbMin MEMBER m_wbMin NOTIFY whiteBalanceChanged)
    Q_PROPERTY(int wbMax MEMBER m_wbMax NOTIFY whiteBalanceChanged)
    Q_PROPERTY(int wbStep MEMBER m_wbStep NOTIFY whiteBalanceChanged)
    // Exposure compensation: DevAEEvBiasType index 0..18, 9 == 0 EV. The bounds
    // are the SDK's own enum, not a device-reported range.
    Q_PROPERTY(int evBias MEMBER m_evBias NOTIFY exposureChanged)

    // ----- capability gating -----
    Q_PROPERTY(bool capAi READ capAi CONSTANT)
    Q_PROPERTY(bool capFace READ capFace CONSTANT)
    Q_PROPERTY(bool capFov READ capFov CONSTANT)
    Q_PROPERTY(bool capImage READ capImage CONSTANT)      // brightness/contrast/sat/sharp (0–100)
    Q_PROPERTY(bool capGesture READ capGesture CONSTANT)
    Q_PROPERTY(bool capHdr READ capHdr NOTIFY imageChanged)   // dynamic: device reports hdr_support
    Q_PROPERTY(bool capTrackingAdvanced READ capTrackingAdvanced CONSTANT)
    Q_PROPERTY(QString capUnverifiedReason READ capUnverifiedReason CONSTANT)
    // Runtime probe, not a constant: true once the camera answered
    // cameraGetRangeWhiteBalanceR + cameraGetWhiteBalanceR on connect.
    Q_PROPERTY(bool capWhiteBalance MEMBER m_capWhiteBalance NOTIFY whiteBalanceChanged)
    // Runtime probe: the camera answered the exposure getters. The SDK marks
    // these "tail air" but a Tiny 3 answers them — hence a probe, not a constant.
    Q_PROPERTY(bool capExposure MEMBER m_capExposure NOTIFY exposureChanged)
    // Click-to-white-balance: a convergence run is in flight, and the last thing
    // it has to say. The message is user-facing (why a pick was rejected, or
    // where it landed), not a log line.
    Q_PROPERTY(bool wbPicking MEMBER m_wbPicking NOTIFY wbPickChanged)
    Q_PROPERTY(QString wbPickMessage MEMBER m_wbPickMessage NOTIFY wbPickChanged)

    // ----- presets + external-preview fallback -----
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    // ffplay fallback (kept alongside the embedded preview): external window path
    // for when QtMultimedia misbehaves on a given box.
    Q_PROPERTY(bool previewAvailable READ previewAvailable CONSTANT)

    // ----- wireless mic (OBSBOT Vox SE) -----
    // One map per transmitter slot (TX1/TX2) for the Mic page's Repeater — built
    // exactly like presets(): { tx, online, battery, charging, muted }. `online`
    // is the camera's own tx_state bit from the status push; the rest come from
    // Device::cameraGetTWSInfoR and are only meaningful while capMicButton.
    Q_PROPERTY(QVariantList micTxList READ micTxList NOTIFY micChanged)
    // Link state, also straight from the status push: BT-TWS vs the Vox SE's
    // 2.4 GHz mode, and whether the camera is currently pairing/scanning.
    Q_PROPERTY(bool micTwsMode MEMBER m_micTwsMode NOTIFY micChanged)
    Q_PROPERTY(bool micPairing MEMBER m_micPairing NOTIFY micChanged)
    Q_PROPERTY(bool micScanning MEMBER m_micScanning NOTIFY micChanged)
    // A pair/clear command of OURS is in flight (distinct from micPairing, which
    // is the camera's own state). Drives the busy cue on the Mic page.
    Q_PROPERTY(bool micPairBusy MEMBER m_micPairBusy NOTIFY micChanged)
    // Device::cameraGetAudioSelectR's has_pair_record: 1 = a mic has been paired
    // to this camera at some point, 0 = never, -1 = unknown/unavailable.
    Q_PROPERTY(int micPairRecord MEMBER m_micPairRecord NOTIFY micChanged)
    // The mic's multi-function button assignment (Device::DevTWSKeyType). The
    // CAMERA stores this, so it is read back from the device rather than
    // persisted here — no app-side copy means no UI-vs-device drift.
    // micButtonAction falls back to 0 while unknown so the selector still has a
    // valid index; micButtonActionName renders "—" until the device answers.
    Q_PROPERTY(int micButtonAction READ micButtonAction WRITE setMicButtonAction NOTIFY micChanged)
    Q_PROPERTY(QString micButtonActionName READ micButtonActionName NOTIFY micChanged)
    // Capability gate, resolved at RUNTIME (hence NOTIFY, not CONSTANT): true
    // once the bound camera answers Device::cameraGetTWSInfoR. That call is
    // exported by libdev but absent from the public SDK header (see
    // ObsbotTwsCompat.h), so a camera/firmware that ignores it must show the
    // mic extras as unavailable rather than pretend.
    Q_PROPERTY(bool capMicButton READ capMicButton NOTIFY micChanged)
    // Second, INDEPENDENT mic gate: true when this build can reach the per-mic
    // mute/gain entry points (Device::cameraTXSet/GetAudio{Mute,Gain}R) AND the
    // camera answers cameraGetTWSInfoR. Without it the mic cards still SHOW mute
    // and gain — cameraGetTWSInfoR reports both — they just cannot be changed.
    Q_PROPERTY(bool capMicGain READ capMicGain NOTIFY micChanged)
    // Per-mic NOISE REDUCTION is report-only, everywhere and always: libdev
    // exports no per-mic setter. Exposed so the page can say so in one place
    // instead of hard-coding the claim in QML.
    Q_PROPERTY(bool capMicNoiseReduce READ capMicNoiseReduce CONSTANT)

    // ----- the camera's own microphone (the Tiny 3's mic array) -----
    // Every value is -1 while unknown — including when the camera does not
    // answer the camera-audio API — so the UI can show "—" instead of an
    // invented 0/off. audioVolume is 0–100, audioNoiseLevel 1–10, and
    // audioMuted / audioNoiseReduce / audioAgc / audioAuto are 0/1.
    Q_PROPERTY(int audioVolume MEMBER m_audioVolume NOTIFY audioChanged)
    Q_PROPERTY(int audioMuted MEMBER m_audioMuted NOTIFY audioChanged)
    Q_PROPERTY(int audioNoiseReduce MEMBER m_audioNoiseReduce NOTIFY audioChanged)
    Q_PROPERTY(int audioNoiseLevel MEMBER m_audioNoiseLevel NOTIFY audioChanged)
    Q_PROPERTY(int audioAgc MEMBER m_audioAgc NOTIFY audioChanged)
    // Which input the camera is using (Device::DevAudioSourceType — 0 built-in,
    // 3 = a wireless Vox SE) and the mic array's pickup pattern
    // (Device::AudioModeType). Both are READ BACK from the status push's
    // tiny.audio_mode; neither has an exported getter. audioSource/audioMode
    // carry the optimistic in-flight pick so a selector does not spring back
    // while the push catches up, while audioSourceName/audioModeName always
    // report what the DEVICE said — the two are shown side by side on the page.
    Q_PROPERTY(int audioSource READ audioSource NOTIFY audioChanged)
    Q_PROPERTY(QString audioSourceName READ audioSourceName NOTIFY audioChanged)
    Q_PROPERTY(int audioMode READ audioMode NOTIFY audioChanged)
    Q_PROPERTY(QString audioModeName READ audioModeName NOTIFY audioChanged)
    // The camera's OWN source arbitration (AudioSelectAttr::is_auto). While this
    // is 1 the camera picks the input itself and overrides any manual choice —
    // see setAudioSource.
    Q_PROPERTY(int audioAuto MEMBER m_audioAuto NOTIFY audioChanged)
    // Capability gate, resolved at RUNTIME like capMicButton: true once the
    // bound camera answers Device::cameraGetAudioVolumeR (exported by libdev,
    // absent from the public header — see ObsbotTwsCompat.h).
    Q_PROPERTY(bool capAudio READ capAudio NOTIFY audioChanged)

    // ----- device event push (Device::setDevEventNotifyCallbackFunc) -----
    // UNPROVEN ON THIS HARDWARE. dev.hpp documents that callback "@category tail
    // air"; whether a Tiny 3 ever fires it is exactly what devEventsSeen exists
    // to answer. Everything below is therefore additive: it appears when an event
    // arrives and is simply absent otherwise. No control is gated on it, nothing
    // waits for it, and no timeout treats its silence as a failure.
    //
    // deviceFault latches on kEvtErrAiComm: the camera's AI subsystem has failed,
    // which matters because the Tiny 3 then accepts tracking commands and
    // silently ignores them while cameraStatus() still reports everything normal
    // — the app's Track toggle looks fine and does nothing.
    Q_PROPERTY(bool deviceFault MEMBER m_deviceFault NOTIFY faultChanged)
    Q_PROPERTY(QString deviceFaultReason MEMBER m_deviceFaultReason NOTIFY faultChanged)
    // Has this camera EVER pushed a device event? Purely diagnostic honesty: it
    // lets the UI say "no device events have arrived from this camera" instead of
    // showing an empty cue area that implies the feature is working.
    Q_PROPERTY(bool devEventsSeen MEMBER m_devEventsSeen NOTIFY faultChanged)

public:
    enum ConnState { Disconnected = 0, Discovering = 1, Connected = 2 };
    Q_ENUM(ConnState)
    enum RunState { RunUnknown = 0, Awake = 1, Asleep = 2 };
    Q_ENUM(RunState)

    explicit CameraController(QObject *parent = nullptr);
    ~CameraController() override;

    void start(int waitMs);
    // Wired in main.cpp. Only used for click-to-white-balance, which needs to
    // read pixels; nothing else in the controller touches the preview.
    void setPreviewEngine(PreviewEngine *p) { m_preview = p; }   // begin discovery (called from main after QML loads)

    // property getters
    int connState() const { return m_connState; }
    bool connected() const { return m_connState == Connected; }
    bool discovering() const { return m_connState == Discovering; }
    int runState() const { return m_runState; }
    bool asleep() const { return m_runState == Asleep; }
    bool aiTracking() const;
    bool faceFocus() const;
    bool gesture() const;
    bool hdrOn() const { return m_hdrOn; }
    int moveStepDeg() const { return m_settings.moveStepDeg; }
    int speedMode() const { return m_settings.speedMode; }
    int fovIndex() const { return m_settings.fovIndex; }
    int startupPreset() const { return m_settings.startupPreset; }
    int aiReturnPreset() const { return m_settings.aiReturnPreset; }
    int previewResIndex() const { return m_settings.previewResIndex; }
    QString previewRes() const;   // e.g. "1080p60"
    bool sleepOnExit() const { return m_settings.sleepOnExit; }
    bool gestureLowTraffic() const { return m_settings.gestureLowTraffic; }
    int autoSleepIndex() const { return m_settings.autoSleepIdx; }
    int micSleepIndex() const { return m_settings.micSleepIdx; }
    QString appVersion() const;

    bool capAi() const { return true; }
    bool capFace() const { return true; }
    bool capFov() const { return true; }
    bool capImage() const { return true; }          // brightness/contrast/sat/sharp: SDK 0–100
    bool capGesture() const { return true; }        // aiSetGestureCtrlIndividualR (rc reported honestly)
    bool capHdr() const { return m_hdrSupport; }    // device-reported hdr_support for the current mode
    bool capTrackingAdvanced() const { return false; }   // framing/sensitivity/zone unverified
    QString capUnverifiedReason() const {
        return QStringLiteral("Not verified against the Tiny 3 SDK — disabled to stay honest.");
    }

    QVariantList presets() const;
    bool previewAvailable() const { return m_previewAvailable; }

    QVariantList micTxList() const;
    // While a change is in flight, show what the user picked: the camera lags a
    // command by up to a status cycle, so reporting the device value straight
    // away makes the selector snap back and look broken. Mirrors the AI-toggle
    // pending idiom (aiTracking()).
    int micButtonAction() const {
        if (m_micButtonTarget >= 0) return m_micButtonTarget;
        return m_micButtonDevice < 0 ? 0 : m_micButtonDevice;
    }
    QString micButtonActionName() const;
    bool capMicButton() const { return m_capMicButton; }
    bool capMicGain() const { return m_capMicGain && m_capMicButton; }
    bool capMicNoiseReduce() const { return false; }   // no exported per-mic NR setter

    // Same optimistic-while-in-flight idiom as micButtonAction: source and mode
    // are only confirmed by the next status push (2–3 s, or the next duty window
    // in low-traffic mode), so the selector shows the user's pick until the
    // camera agrees — or until the confirm timer gives up and the device value
    // takes over again. The *Name getters never do this: they are the "what the
    // camera actually reports" row.
    int audioSource() const { return m_audioSourceTarget >= 0 ? m_audioSourceTarget : m_audioSourceDevice; }
    int audioMode() const { return m_audioModeTarget >= 0 ? m_audioModeTarget : m_audioModeDevice; }
    QString audioSourceName() const;
    QString audioModeName() const;
    bool capAudio() const { return m_capAudio; }

public slots:
    // property setters (persist)
    void setMoveStepDeg(int deg);
    void setSpeedMode(int mode);
    void setFovIndex(int idx);
    void setStartupPreset(int p);
    void setAiReturnPreset(int p);
    void setPreviewResIndex(int idx);
    void setSleepOnExit(bool on);
    void setGestureLowTraffic(bool on);
    void setAutoSleepIndex(int idx);
    void setMicSleepIndex(int idx);
    void resetImageDefaults();     // set brightness/contrast/saturation/sharpness to 50

    // user actions (forwarded to the worker)
    void wake();
    void sleep();
    void center();
    void nudge(int dir);            // 0=up,1=down,2=left,3=right
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void setAiTracking(bool on);      // Human tracking on/off (the real Tiny3 tracking)
    void setFaceFocus(bool on);       // face autofocus on/off (independent — no gimbal motion)
    void setGesture(bool on);         // gesture control on/off
    void gestureQuietTest();          // 60 s SDK-traffic pause (gesture diagnostic)
    void setHdr(bool on);             // HDR/WDR on/off (only when capHdr)
    void setImageParam(const QString &param, int value);  // brightness/contrast/saturation/sharpness
    // White balance. Kelvin is clamped to the DEVICE-reported range and snapped
    // to its step, so the UI cannot ask for a value the camera would reject.
    void setWhiteBalanceAuto(bool on);
    void setWhiteBalanceKelvin(int kelvin);
    // ev is the DevAEEvBiasType index 0..18 (9 == 0 EV), clamped here.
    void setEvBias(int ev);
    // Grey-point white balance. nx/ny are normalised coordinates INSIDE the
    // video image. The camera takes only a Kelvin value, so this is a closed
    // loop — sample, correct, re-measure — not a formula. See the .cpp.
    void pickWhiteBalance(qreal nx, qreal ny);
    void rescan();
    void launchPreview();   // FALLBACK: (re)launch the external ffplay preview
    void stopPreview();     // terminate the ffplay preview (also called on shutdown)
    void copyToClipboard(const QString &text);   // e.g. "Copy" on the Log page

    // VELOCITY (hold-to-move) PTZ — see CameraWorker for the safety-stop design.
    // pitchFrac/yawFrac in [-1,1]; scaled internally by the current speed setting.
    void gimbalVelocity(double pitchFrac, double yawFrac);
    void gimbalStop();   // idempotent; also wired to window-blur (stop-on-blur) in Main.qml

    // presets
    void savePreset(int idx);
    void goPreset(int idx);
    void clearPreset(int idx);
    void renamePreset(int idx, const QString &name);
    void saveCurrentToNextEmpty();

    // wireless mic (Vox SE) — tx is 1-based (1 or 2), which is also the SDK's
    // DevTXType value. The worker wakes the camera before pairing: a sleeping
    // Tiny 3 ACKs the command and never scans.
    // Open pairing. No slot argument: the camera picks the slot itself (see the
    // implementation note) — offering a per-slot choice would be inventing a
    // capability the hardware does not have.
    void micPair();
    void micClearPairing(int tx);    // forget the mic linked to a slot
    // Per-mic mute and gain. Distinct from setAudioMute/setAudioVolume, which act
    // on whichever SOURCE the camera is listening to: these are the Vox SE's own
    // settings, the same ones its button changes.
    //
    // GAIN IS RELATIVE, on purpose. The SDK documents no range for it, so the UI
    // steps the value the CAMERA reported rather than driving a 0–100 slider that
    // would assert a scale nobody has verified. micGainStep is the step size in
    // the device's own units.
    void setMicMute(int tx, bool muted);
    void nudgeMicGain(int tx, int delta);
    // Assign the multi-function button. Persists always, and pushes to the
    // device when the runtime probe says it will listen.
    void setMicButtonAction(int idx);

    // The camera's own microphone. One command per user action — a rapid burst
    // of audio/AI/sleep writes has been observed to put the camera into an AI
    // fault (solid red LED, everything silently ignored), so nothing here is
    // batched and nothing is re-sent on connect.
    void setAudioVolume(int volume);      // 0–100
    void setAudioMute(bool muted);
    void setAudioNoiseReduce(bool on);    // keeps the current level
    void setAudioNoiseLevel(int level);   // 1–10, keeps the current on/off state
    void setAudioAgc(bool on);
    void setAudioMode(int mode);          // pickup pattern, Device::AudioModeType
    // Pinning a source IMPLIES turning the camera's own arbitration off: with it
    // on, the pick is accepted and then silently reverted (hardware finding).
    // The worker does both in one command and logs that it did.
    void setAudioSource(int source);      // Device::DevAudioSourceType
    void setAudioAuto(bool on);           // hand source selection back to the camera

signals:
    void connStateChanged();
    void identityChanged();
    void statusChanged();
    void zoomChanged();
    void aiChanged();
    void imageChanged();
    void whiteBalanceChanged();
    void exposureChanged();
    void wbPickChanged();
    void settingsChanged();
    void presetsChanged();
    void logLine(const QString &kind, const QString &message);
    void commandResult(const QString &action, bool ok, const QString &message);
    void discoveryFinished(bool found);   // one-shot, used by --self-test
    void micChanged();
    void audioChanged();   // the camera's own microphone (kept apart from micChanged)
    void faultChanged();   // device event push: AI fault latch / "any event seen yet"

private slots:
    void onConnectionResolved(bool found, const QString &product, const QString &sn,
                              const QString &fw, const QString &mode, int enumId);
    void onDeviceLost(const QString &reason);
    void onStatusUpdate(int runState, int aiModeRaw, double zoom, bool zoomValid);
    void onAuxStatus(bool faceFocus, bool hdrOn, bool hdrSupport, int fps, int sleepMicro, int autoSleepSec);
    void onZoomUpdate(double zoom, bool valid);
    void onImageParams(int brightness, int contrast, int saturation, int sharpness);
    void onWhiteBalance(bool supported, bool autoMode, int kelvin, int kmin, int kmax, int kstep);
    void onExposureState(bool supported, int ev);
    // One iteration of the grey-point loop: sample, decide, either converge or
    // write the next temperature.
    void wbPickStep();
    void onWorkerResult(const QString &action, bool ok, int rc, const QString &message);
    void onPresetCaptured(int idx, double pitch, double yaw, double zoom, int fov);
    void onMicStatus(bool tx1Online, bool tx2Online, bool twsMode, bool pairing, bool scanning,
                     int audioSource, int audioMode);
    void onTwsInfo(bool supported, int keyCmd,
                   int batt1, bool charging1, bool muted1,
                   int batt2, bool charging2, bool muted2);
    void onTwsMicAudio(bool ctlSupported, int gain1, int ns1, int nsLevel1,
                       int gain2, int ns2, int nsLevel2);
    void onMicAudioSelect(int hasPairRecord, int isAuto, int supportAuto);
    void onAudioState(bool supported, int volume, int muted, int noiseReduce, int noiseLevel, int agc);
    void onDeviceEvent(int eventType, const QString &name);
    void onDeviceFault(bool faulted, const QString &reason);
    void onMicTip(int slot, const QString &what);

private:
    double speedValue() const;   // speedMode -> gimbal reference speed
    QString decodeAiMode(int raw) const;
    void persist();
    // Move to the configured startup preset, after a short delay so the gimbal's
    // power-on self-centering (which runs when the camera connects or wakes)
    // finishes first — otherwise the centering overrides the preset move. `why`
    // is just a log label ("startup" / "wake").
    void scheduleStartupPreset(const QString &why);
    // Delayed, guarded push of the managed power/sleep settings (see impl).
    void applyPowerSettings(const QString &why);
    // Raise the in-flight cue for a mic pair/clear, with a safety timeout.
    void micPairBusyCue(int timeoutMs);
    // Shared guard for the built-in-audio setters. QML already disables those
    // controls without capAudio, so getting here means something raced (the
    // device went away mid-click) — say so instead of swallowing the action.
    bool audioReady(const QString &what);

    QThread m_thread;
    CameraWorker *m_worker = nullptr;

    int m_connState = Discovering;
    int m_runState = RunUnknown;
    // True once a device has been bound and its startup preset applied; stays
    // true across a still-connected Rescan (re-bind) so the startup preset does
    // NOT re-fire and move the gimbal. Cleared only on a real device loss.
    bool m_hadDevice = false;
    QString m_product, m_sn, m_firmware, m_mode;
    int m_enumId = -1;
    double m_zoom = 1.0;
    bool m_zoomValid = false;
    int m_aiModeRaw = 0;
    QString m_aiModeName = QStringLiteral("Off");

    // Confirmed AI state (what the device actually is, per success/status).
    bool m_aiTracking = false;   // ai_mode == AiWorkModeHuman (owns gimbal)
    bool m_faceFocus = false;    // face autofocus engaged (independent, does NOT own gimbal)
    bool m_gesture = false;      // gesture control (intent; SDK has no clean status readback)
    bool m_hdrOn = false;        // from status tiny.hdr
    bool m_hdrSupport = false;   // from status tiny.hdr_support (drives capHdr)
    // Pending window for the HDR toggle: while set, ignore the ~2-3s status push
    // so it can't clobber the optimistic value before the device applies it
    // (same CODE_REVIEW #8 race the AI-track path guards with m_aiPending).
    bool m_hdrPending = false;
    // Optimistic targets during a toggle (applied on success, discarded on failure).
    bool m_targetTracking = false;
    bool m_targetFace = false;
    bool m_targetGesture = false;
    bool m_targetHdr = false;
    bool m_aiPending = false;
    int m_aiInFlight = 0;        // outstanding AI-track/face-focus toggle legs in flight
    QTimer *m_pendingTimer = nullptr;
    // Started when an AI-Track ON command is confirmed. If the device then
    // disengages on its own shortly after (status push back to None), we log an
    // honest hint — the Tiny 3 accepts the command but silently drops tracking
    // when it can't find a person in view (Rex: "AI track don't work… after
    // some fiddling with presets [re-aiming the camera at me] it worked").
    QElapsedTimer m_aiEngageTime;
    // FOV carried by an in-flight preset recall; applied to the selector in
    // onWorkerResult only when the device accepted the move (never on refusal).
    int m_pendingGoFov = -1;

    // Live image params (0–100), mirrored from the device on connect + on change.
    int m_brightness = 50, m_contrast = 50, m_saturation = 50, m_sharpness = 50;
    // White balance. The range starts EMPTY on purpose: until the camera
    // reports one, capWhiteBalance is false and the UI shows no slider, so
    // there is never a moment where a plausible-looking but invented range is
    // on screen.
    // Click-to-WB state. m_wbPickTimer drives the loop; everything else is the
    // run in progress.
    QTimer *m_wbPickTimer = nullptr;
    bool m_wbPicking = false;
    QString m_wbPickMessage;
    qreal m_wbPickX = 0.5, m_wbPickY = 0.5;
    int m_wbPickTriesLeft = 0;
    int m_wbPickKelvin = 0;
    PreviewEngine *m_preview = nullptr;
    void finishWbPick(const QString &message);
    bool m_capExposure = false;
    int m_evBias = 9;               // DevAEEvBias_0
    bool m_capWhiteBalance = false;
    bool m_wbAuto = true;
    int m_wbKelvin = 0;
    int m_wbMin = 0, m_wbMax = 0, m_wbStep = 100;
    int m_fps = 0;               // current video stream fps (from status)
    int m_micSleepDevice = -1;   // device-reported mic-during-sleep (readback; -1 unknown)
    int m_autoSleepDevice = -1;  // device-reported auto-sleep seconds (readback; -1 unknown, 0 never)

    // Wireless mic (Vox SE) transmitter state, one entry per slot (TX1/TX2).
    // `online` is pushed by the camera (tiny.wireless_mic.tx_state); battery/
    // charging/muted come from Device::cameraGetTWSInfoR. battery -1 = unknown.
    // NOTE: transmitter name and firmware are deliberately absent — the public
    // SDK has no source for them (the old cameraTXGet* getters this feature was
    // first written against do not exist in the official header).
    // gain is SIGNED and in the device's own undocumented units, so it needs a
    // separate gainKnown flag — unlike battery, -1 is a perfectly legal gain and
    // cannot double as "unknown". ns/nsLevel are report-only (no exported per-mic
    // setter). tip is the last device-event cue for this slot, blanked again by
    // m_micTipTimer; it stays empty forever on a camera that never pushes events.
    struct MicTx {
        bool online = false;
        int battery = -1;
        bool charging = false;
        bool muted = false;
        int gain = 0;
        bool gainKnown = false;
        // Optimistic gain while a step is in flight. Without it, two quick
        // presses both compute from the same device-reported value and the
        // second sends an identical command — the "every other press does
        // nothing" the user hit. Cleared when the camera confirms, or by
        // m_micGainTimer.
        int gainTarget = 0;
        bool gainPending = false;
        int ns = -1;
        int nsLevel = -1;
        QString tip;      // last button press seen for this mic — kept, not cleared
        QString tipAt;    // when it happened
        bool tipRecent = false;   // drives the highlight only
    } m_micTx[2];
    bool m_micTwsMode = false;   // status push: BT TWS mode instead of 2.4G mic mode
    bool m_micPairing = false;   // status push: the camera is in pairing mode
    bool m_micScanning = false;  // status push: the camera is scanning for a mic
    bool m_micPairBusy = false;
    // Bumped on every new cue so a stale timeout cannot cancel a newer wait.
    quint64 m_micPairCueGen = 0;  // OUR pair/clear command is in flight
    int m_micPairRecord = -1;    // has_pair_record (0/1, -1 unknown)
    int m_micButtonDevice = -1;  // device-reported DevTWSKeyType (-1 unknown)
    // Optimistic value while a set is in flight (-1 = none). Cleared when the
    // device confirms it, or by m_micButtonTimer so a lost/ignored command can
    // never leave the selector lying indefinitely.
    int m_micButtonTarget = -1;
    QTimer *m_micButtonTimer = nullptr;
    QTimer *m_micGainTimer = nullptr;   // gives up on an unconfirmed gain step
    bool m_capMicButton = false; // runtime probe: cameraGetTWSInfoR answered OK
    bool m_capMicGain = false;   // build probe: the per-mic set/get symbols resolved
    QTimer *m_micTipTimer = nullptr;   // blanks the transient per-mic event cues

    // Device event push. m_deviceFault only ever becomes true because the camera
    // SAID so (kEvtErrAiComm) — it is never inferred from a command that failed
    // to take effect, which would be a guess dressed up as a diagnosis.
    bool m_deviceFault = false;
    QString m_deviceFaultReason;
    bool m_devEventsSeen = false;

    // The camera's own microphone. -1 everywhere means "unknown" — the value the
    // UI renders as "—" rather than guessing.
    int m_audioVolume = -1;       // 0–100
    int m_audioMuted = -1;        // 0/1
    int m_audioNoiseReduce = -1;  // 0/1
    int m_audioNoiseLevel = -1;   // 1–10
    int m_audioAgc = -1;          // 0/1
    int m_audioAuto = -1;         // AudioSelectAttr::is_auto (0/1)
    // From the status push's tiny.audio_mode — the ONLY readback either of these
    // has (no exported getter), which is why both carry an optimistic in-flight
    // target with a give-up timer, exactly like the mic-button assignment.
    int m_audioSourceDevice = -1;
    int m_audioModeDevice = -1;
    int m_audioSourceTarget = -1;
    int m_audioModeTarget = -1;
    QTimer *m_audioSourceTimer = nullptr;
    QTimer *m_audioModeTimer = nullptr;
    bool m_capAudio = false;      // runtime probe: cameraGetAudioVolumeR answered OK

    bool m_previewAvailable = false;
    // Managed ffplay preview process (NOT detached) so it is killed when the app
    // closes — a detached ffplay would linger holding /dev/video0 and spew
    // "/dev/video0: error while seeking" after the app is gone. Also lets a
    // resolution change reload the preview.
    QProcess *m_previewProc = nullptr;

    AppSettings m_settings;
};
