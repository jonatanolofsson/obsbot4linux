// CameraWorker — owns the OBSBOT SDK Device and runs EVERY blocking SDK call on
// a dedicated QThread. It never touches QML/GUI objects; it only emits Qt
// signals, which are delivered to the CameraController on the GUI thread via
// queued connections. This is the structural fix for the GTK PoC's shutdown
// UAF cluster (CODE_REVIEW #1/#2/#3/#9): there are no detached threads, the SDK
// status callback is disabled deterministically on shutdown(), and the worker
// outlives its own event loop so no queued event ever lands on a dead object.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

class Device;
class QTimer;

class CameraWorker : public QObject {
    Q_OBJECT
public:
    explicit CameraWorker(QObject *parent = nullptr);
    ~CameraWorker() override;

public slots:
    void init();                       // one-time setup, runs on the worker thread
    void startDiscovery(int waitMs);
    void rescan(int waitMs);

    void cmdWake();
    void cmdSleep();
    void cmdCenter();
    // dir: 0=up, 1=down, 2=left, 3=right. Bounded one-shot nudge.
    void cmdNudge(int dir, double stepDeg, double speed);
    void cmdZoom(double delta, bool absolute, double absVal, const QString &action);
    void cmdSetFov(int fovType, const QString &label);
    // AiWorkModeType (Human=2, PortraitTrack=14 — Tiny3-specific "face track" —
    // or None=0). subMode/from is always 0 here: the SDK's sub_mode_or_from
    // parameter of cameraSetAiModeU is documented "for tiny2" for the AiSubModeType
    // framing meaning, and 0 ("Normal case") is the universally-documented-safe
    // value for every other product/mode. See CameraController for why the
    // Framing (Normal/Upper/Close-up) control was removed on Tiny3.
    void cmdSetAi(int mode, int subMode, const QString &action);
    void cmdSetFace(bool on);                          // cameraSetFaceFocusR (independent)
    // Writes BOTH gesture config stores + full readback. lowTraffic gates the
    // experimental 15 s status cadence (opt-in setting — see setGestureFriendly).
    void cmdSetGesture(bool on, bool lowTraffic);
    void cmdSetHdr(bool on);                           // cameraSetWdrR
    // Auto-sleep timer (cameraSetSuspendTimeU; <=0 disables auto-sleep) and
    // mic-during-sleep (cameraSetMicrophoneDuringSleepU). The SDK category
    // docs omit the Tiny 3 for BOTH — rc + the status push's sleep_micro
    // readback are the honest verdict (gesture taught us rc alone can lie).
    void cmdSetAutoSleep(int seconds, const QString &label);
    void cmdSetMicSleep(bool on);
    void cmdSetImage(const QString &param, int value); // brightness/contrast/saturation/sharpness (0–100)
    void cmdReadImageParams();                         // read current image params on connect
    // White balance. Public SDK API (@category includes "tiny"), so no shim.
    // cmdReadWhiteBalance doubles as the capability probe AND supplies the
    // device's own Kelvin range — the UI cannot draw the slider without it, and
    // nothing here hardcodes bounds the SDK does not document. See the .cpp for
    // what the hardware actually reports (param is Kelvin; the *List* getter's
    // bounds are inverted garbage and must not be used).
    void cmdReadWhiteBalance();
    void cmdSetWhiteBalance(bool autoMode, int kelvin);
    // Exposure compensation. cmdReadExposure doubles as the capability probe.
    // ev is the DevAEEvBiasType index 0..18, 9 == 0 EV. See the .cpp for why the
    // P-gear setter is the only one that works on this camera.
    void cmdReadExposure();
    void cmdSetEvBias(int ev);
    void cmdPresetCapture(int idx);
    void cmdPresetGo(int idx, double pitch, double yaw, double zoom, int fov, double speed);

    // Wireless mic (OBSBOT Vox SE).
    //
    // PRESENCE comes for free from the SDK status push (CameraStatus's
    // tiny.wireless_mic) — see onSdkStatus / the micStatus signal. Nothing is
    // polled for it.
    //
    // BATTERY / mute / the current button assignment come from ONE call,
    // Device::cameraGetTWSInfoR, which libdev exports but the public header
    // does not declare — reached through ObsbotTwsCompat.h. cmdReadTwsInfo also
    // doubles as the runtime capability probe (rc == RM_RET_OK => this
    // firmware answers the undocumented API); its `supported` flag drives the
    // UI's capMicButton gate. SILENT: no logLine, it also runs on a slow timer.
    void cmdReadTwsInfo();
    // Assign the mic's multi-function button: idx is the Device::DevTWSKeyType
    // value 1:1 (0=Track, 1=Switch track, 2=Zoom 1x, 3=Record), validated 0..3.
    void cmdSetMicButtonAction(int idx);
    // PAIRING. tx is 1-based and IS Device::DevTXType (DevTX1 = 1 — the enum is
    // one-based). HARDWARE FINDING: a sleeping Tiny 3 ACKs the pair command with
    // rc=0 and never turns its radio on, so cmdTxPair wakes the camera first and
    // says so in the log. rc is NOT proof of success — the honest confirmation is
    // wireless_mic.tx_state going non-zero in the status push (micStatus).
    void cmdTxPair(int tx, bool enable);
    void cmdTxClear(int tx);
    // PER-MIC mute and gain (Device::cameraTXSetAudioMuteR / cameraTXSetAudioGainR
    // — exported, undeclared, see ObsbotTwsCompat.h). tx is 1-based = DevTXType.
    // Both re-read the device straight afterwards rather than trusting rc.
    //
    // GAIN IS IN THE DEVICE'S OWN UNITS. The SDK documents no range for it, so
    // nothing here rescales: the caller passes an absolute value derived from
    // what the camera last REPORTED (DevTWSInfo::micN_gain), clamped only to the
    // int8 that field can hold. cmdSetTxGain logs the value the device answers
    // with when it differs from the request — that readback is the only way this
    // app will ever learn the real bounds.
    void cmdSetTxMute(int tx, bool muted);
    void cmdSetTxGain(int tx, int gain);
    // Audio-source auto-select attributes (Device::cameraGetAudioSelectR),
    // emitted as micAudioSelect. Two jobs: has_pair_record == 0 is the "no mic
    // has ever been paired to this camera" diagnostic, and is_auto is the
    // ARBITRATION flag the Audio section depends on — while it is 1 the camera
    // picks the input itself and reverts any manual choice (see cmdSetAudioSource).
    void cmdReadAudioSelect();

    // The camera's OWN microphone (the Tiny 3's mic array).
    //
    // SOURCE and pickup MODE need no call at all: the SDK status push carries
    // them in CameraStatus's tiny.audio_mode bitfield (source:3 / mode:5), so
    // they ride the existing micStatus signal. Everything below is the part the
    // push does NOT carry, reached through ObsbotTwsCompat.h.
    //
    // cmdReadAudio does ONE pass — volume, mute, noise reduction (+level), AGC —
    // and doubles as the capability probe: if cameraGetAudioVolumeR does not
    // answer RM_RET_OK, `supported` is false and the whole Audio section is
    // shown as unavailable rather than filled with invented zeroes. Called on
    // bind and after a successful set; deliberately NOT on a timer (see the
    // gesture-recognizer traffic finding — nothing here changes behind our back).
    void cmdReadAudio();
    void cmdSetAudioVolume(int volume);              // [0-100]
    void cmdSetAudioMute(bool muted);
    void cmdSetAudioNoiseReduce(bool on, int level); // level [1-10]
    void cmdSetAudioAgc(bool on);
    void cmdSetAudioMode(int mode);                  // Device::AudioModeType 0..5
    // HARDWARE FINDING: while the camera's own source arbitration is on
    // (AudioSelectAttr::is_auto == 1) a source pick is accepted with rc=0 and
    // then silently reverted. cmdSetAudioSource therefore turns arbitration OFF
    // first, in this one command, and says so in the log.
    void cmdSetAudioSource(int source);              // Device::DevAudioSourceType
    void cmdSetAudioAuto(bool on);                   // camera picks the source itself

    // VELOCITY (hold-to-move) PTZ — gated behind four safety stops, per the
    // design handoff: stop-on-release (caller stops sending on pointer-up),
    // stop-on-window-blur (CameraController::gimbalStop() wired to window
    // active-changed), stop-on-error (below: any non-OK rc auto-stops), and a
    // deadman timeout (m_velocityWatchdog — auto-stops if no fresh command
    // arrives within its interval, e.g. if the UI thread stalls).
    // pitchSpeed/yawSpeed are degrees/sec, matching gimbalSpeedCtrlR's units.
    void cmdGimbalVelocity(double pitchSpeed, double yawSpeed);
    void cmdGimbalStop();   // idempotent; safe to call anytime, incl. when not moving

    // GESTURE DIAGNOSTIC (hardware finding: the Tiny 3 executes gestures
    // autonomously when NO app session is attached, but goes gesture-deaf while
    // this app runs). Quiet mode silences ALL periodic SDK traffic (status push
    // subscription + the per-push zoom getter) for `seconds` while staying
    // attached, to determine whether the traffic or the session itself
    // suppresses the recognizer. HARDWARE-CONFIRMED (Rex, quiet-test): it's the
    // TRAFFIC — palm gestures worked during the quiet window.
    void cmdQuietMode(int seconds);

    // The permanent fix built on that finding: while gesture control is ON,
    // drop the status cadence from the SDK's ~2–3 s push to a one-shot
    // enable→push→disable duty cycle every kStatusDutyMs, leaving the USB
    // control channel quiet enough for the camera's recognizer to work.
    // Applied automatically by cmdSetGesture on success.
    void setGestureFriendly(bool on);

    // Deterministic teardown: disable the status callback, drop the device, and
    // stop the SDK discovery task. Invoked BlockingQueued from the controller
    // before the thread is quit — see CameraController::~CameraController.
    void shutdown();

signals:
    void logLine(const QString &kind, const QString &message);
    void connectionResolved(bool found, const QString &product, const QString &sn,
                            const QString &fw, const QString &mode, int enumId);
    void deviceLost(const QString &reason);
    void statusUpdate(int runState, int aiModeRaw, double zoom, bool zoomValid);
    // Extra device state read from the same status push: face autofocus on/off,
    // HDR on/off, whether HDR is supported in the current mode, and current fps.
    // sleepMicro: device-reported mic-during-sleep flag (0/1) from the push —
    // the readback for cmdSetMicSleep.
    void auxStatus(bool faceFocus, bool hdrOn, bool hdrSupport, int fps, int sleepMicro, int autoSleepSec);
    void zoomUpdate(double zoom, bool valid);
    void imageParams(int brightness, int contrast, int saturation, int sharpness);
    // White balance state + the device's OWN range. supported=false means the
    // camera did not answer the range/get pair and the control stays disabled;
    // the remaining fields are meaningless in that case.
    void whiteBalance(bool supported, bool autoMode, int kelvin, int kmin, int kmax, int kstep);
    // supported=false => the camera did not answer the exposure getters and the
    // control stays disabled; ev is the DevAEEvBiasType index (9 == 0 EV).
    void exposureState(bool supported, int ev);
    void commandResult(const QString &action, bool ok, int rc, const QString &message);
    void presetCaptured(int idx, double pitch, double yaw, double zoom, int fov);
    // Wireless-mic PRESENCE, straight from the status push's tiny.wireless_mic.
    // tx1/tx2 are the two transmitter slots the camera reports online; twsMode
    // distinguishes BT TWS mode from the Vox SE's 2.4 GHz mode; pairing/scanning
    // are the camera's own link state.
    // audioSource/audioMode come from the SAME push (tiny.audio_mode): the
    // camera's current input (Device::DevAudioSourceType — 3 = the wireless Vox
    // SE) and its mic-array pickup pattern (Device::AudioModeType). They ride
    // this signal because they are pushed, not polled — no extra USB traffic.
    void micStatus(bool tx1Online, bool tx2Online, bool twsMode, bool pairing, bool scanning,
                   int audioSource, int audioMode);
    // Wireless-mic DETAIL from Device::cameraGetTWSInfoR (see ObsbotTwsCompat.h).
    // supported=false means the call is unavailable on this build/firmware and
    // every other field is meaningless — the UI must show "unavailable", not 0.
    // battery is 0–100 (-1 unknown); keyCmd is the device's DevTWSKeyType.
    // chargingN is "mic_chg_status != 0". The exact encoding is undocumented —
    // a Tiny 3 was observed reporting 2 at 100 % — so the UI says "on charge"
    // rather than claiming a charge RATE it cannot know.
    void twsInfo(bool supported, int keyCmd,
                 int batt1, bool charging1, bool muted1,
                 int batt2, bool charging2, bool muted2);
    // Per-mic AUDIO detail, read from the SAME DevTWSInfo struct as twsInfo but
    // emitted separately because it is gated on a DIFFERENT capability:
    // cameraGetTWSInfoR REPORTS gain/noise-reduction, while CHANGING gain needs
    // the cameraTXSet/GetAudio*R family, which is an independent set of exports
    // (ctlSupported == ObsbotTws::txAudioLinked()). gainN is in the device's own
    // undocumented units; nsN is 0/1; nsLevelN is the device's raw level byte.
    // Every field is meaningless unless twsInfo's `supported` was true.
    //
    // NOISE REDUCTION IS REPORT-ONLY: libdev exports no per-mic NR setter (see
    // ObsbotTwsCompat.h), so the UI shows nsN/nsLevelN and offers no control.
    void twsMicAudio(bool ctlSupported,
                     int gain1, int ns1, int nsLevel1,
                     int gain2, int ns2, int nsLevel2);
    // Device event push (Device::setDevEventNotifyCallbackFunc — public, and the
    // ONE thing here the SDK header actually declares). eventType is the SDK's
    // RmEventType value verbatim; name is our decode, or empty when the value is
    // outside the enum we know. UNPROVEN ON THIS HARDWARE: the header documents
    // this callback "@category tail air", so a Tiny 3 may never fire it — every
    // consumer must degrade silently rather than assume the events arrive.
    void deviceEvent(int eventType, const QString &name);
    // AI-subsystem fault latch, from kEvtErrAiComm (set) / kEvtInfoAiComm
    // (clear). Matters because in this state the camera keeps answering
    // cameraStatus() normally and silently ignores tracking commands — the Track
    // toggle looks fine and does nothing. Only ever emitted if the callback above
    // actually fires.
    void deviceFault(bool faulted, const QString &reason);
    // A wireless-mic button/status tip (the kEvtTipsTWS* range). slot is 1 or 2
    // where the event names one, else 0 ("this happened, but not which mic").
    void micTip(int slot, const QString &what);
    // Device::cameraGetAudioSelectR readback. Each field is -1 when the call is
    // unavailable, else 0/1. hasPairRecord == 0 means no mic has ever been
    // paired to this camera. isAuto is the source ARBITRATION flag — while it is
    // 1 the camera overrides any manual source pick.
    void micAudioSelect(int hasPairRecord, int isAuto, int supportAuto);
    // Built-in-audio readback from cmdReadAudio. supported=false means this
    // build/firmware does not answer the camera-audio API and every other field
    // is meaningless — the UI must say "unavailable", not show 0. volume is
    // 0–100, noiseLevel 1–10; muted/noiseReduce/agc are 0/1, and every field is
    // -1 when unknown.
    void audioState(bool supported, int volume, int muted, int noiseReduce, int noiseLevel, int agc);

private:
    void pollTick();
    void bindDevice(const std::shared_ptr<Device> &d);
    bool requireDevice(const QString &action);
    // Returns true (and logs an honest "blocked" result) if AI tracking currently
    // owns the gimbal, so the caller must NOT issue a manual gimbal move.
    // CODE_REVIEW #4: never log a fake "(ok)" for a move the device will ignore.
    bool aiOwnsGimbal(const QString &action);
    void refreshZoom();
    // Wireless-mic pairing helpers. wakeForMic wakes a sleeping camera (and logs
    // it) because a sleeping Tiny 3 ACKs pairing commands it never acts on; it
    // returns true when the camera was ALREADY awake, false when a wake was just
    // issued and the radio command should wait for it to settle. sendTxPair does
    // the actual pair/unpair once the camera is known to be awake.
    bool wakeForMic(const QString &action);
    void sendTxPair(int tx, bool enable);
    // cmdReadTwsInfo's body, returning the key_cmd the camera reported (-1 if it
    // could not be read) for the confirm poller below.
    int readTwsInfo();
    // Confirming a mic-button assignment takes POLLING, not a single read: the
    // camera keeps answering the previous key_cmd for up to ~3 s after a write
    // that has already succeeded. startKeyConfirm arms the poll; onKeyConfirmTick
    // re-reads (emitting twsInfo each time, so the UI settles as soon as the
    // device agrees) and stops on a match or when the budget expires.
    void startKeyConfirm(int target);
    void onKeyConfirmTick();
    // Gesture-friendly mode: open a one-push status window NOW (closed again by
    // onSdkStatus). Called after state-changing commands (wake/sleep/AI/preset)
    // so their effects reach the UI immediately instead of at the next duty
    // tick — e.g. the wake edge must not fire the startup preset 15 s late.
    // The command itself just made traffic, so this pulse costs nothing extra
    // with respect to recognizer suppression.
    void statusPulse();
    // micBits packs CameraStatus::tiny.wireless_mic (a 1-byte bitfield the SDK
    // callback must not be trusted to keep alive) in the DEVICE's own bit order:
    // bit0 is_tws_mode, bits1-2 tx_state, bit3 is_pairing, bit4 is_scanning.
    // audioBits packs the neighbouring tiny.audio_mode byte the same way:
    // bits0-2 source (DevAudioSourceType), bits3-7 mode (AudioModeType).
    void onSdkStatus(int runStatus, int aiMode, int faceFocus, int hdr, int hdrSupport, int fps, int sleepMicro, int autoSleepSec, int micBits, int audioBits);
    void onDevChanged(const QString &sn, bool plugged);
    // Device event, already marshalled onto the worker thread. `result` from the
    // SDK is deliberately NOT forwarded: its type is event-specific and
    // undocumented for every event this app cares about, and it points into the
    // SDK thread's buffer, so reading it later would be a guess AND a lifetime
    // bug. The numeric event_type is the whole payload.
    void onDevEvent(int eventType);
    // Install (or, with `on == false`, replace with a no-op) the device event
    // callback. Never installs an EMPTY std::function: libdev is closed source
    // and may call it unchecked, which on an empty std::function is
    // std::bad_function_call — i.e. a crash on shutdown. A no-op lambda is the
    // safe way to let go of `this`.
    void setDevEventCallback(bool on);

    static void sdkStatusTrampoline(void *param, const void *data);

    std::shared_ptr<Device> m_dev;
    QString m_sn;
    QTimer *m_pollTimer = nullptr;
    // Slow (60 s) refresh of the wireless-mic DETAIL (battery/mute). Presence
    // rides the status push and needs no polling, so this stays deliberately
    // lazy; ticks are silent and are skipped while shutting down or in quiet
    // mode so it never re-adds the USB traffic the gesture work removed.
    QTimer *m_twsInfoTimer = nullptr;
    // Short-lived poll that runs only while a mic-button assignment is settling.
    QTimer *m_keyConfirmTimer = nullptr;
    int m_keyConfirmTarget = -1;
    int m_keyConfirmTicksLeft = 0;
    // Runtime capability: the connected camera answered cameraGetTWSInfoR.
    // m_twsProbed makes the FIRST verdict always log, including a negative one.
    bool m_twsSupported = false;
    bool m_twsProbed = false;
    // Same first-verdict-always-logs pattern for the white-balance probe.
    bool m_wbSupported = false;
    bool m_wbProbed = false;
    bool m_expSupported = false;
    bool m_expProbed = false;
    // Same pair for the camera-audio API (cameraGetAudioVolumeR answered). Kept
    // separate from m_twsSupported: a camera can perfectly well have a working
    // mic array and no wireless-mic support, or the reverse.
    bool m_audioSupported = false;
    bool m_audioProbed = false;
    // The device event callback is installed on this device (so shutdown/unplug
    // knows to hand it back). NOT a capability flag: registering it says nothing
    // about whether a Tiny 3 ever fires an event.
    bool m_devEventRegistered = false;
    int m_pollElapsedMs = 0;
    int m_pollTimeoutMs = 6000;
    bool m_devChangedRegistered = false;
    std::atomic<bool> m_shuttingDown{false};
    bool m_quiet = false;   // gesture diagnostic: drop status pushes while true

    // Gesture-friendly status cadence (see setGestureFriendly).
    bool m_gestureFriendly = false;
    bool m_awaitingDutyPush = false;   // duty window open, waiting for one push
    QTimer *m_statusDutyTimer = nullptr;

    // Cached from the SDK status push; used only for the honest AI-owns-gimbal
    // guard. Never causes an unwanted move.
    bool m_aiTracking = false;
    // Grace windows after a CONFIRMED AI command (see onSdkStatus): each
    // swallows exactly ONE stale contradicting status push — the device lags a
    // command by up to a push cycle — then closes, so a genuine device-side
    // change (e.g. a palm gesture re-engaging tracking) is at most one push
    // late instead of being rewritten for the whole window.
    QElapsedTimer m_aiOffGrace;   // after AI-off: swallow one stale "on" push
    QElapsedTimer m_aiOnGrace;    // after AI-on: swallow one stale "None" push
    int m_lastAiOnMode = 0;       // the mode we commanded on (forwarded during on-grace)

    // Velocity-mode state (hold-to-move PTZ).
    bool m_velocityActive = false;         // true while a hold-drag is in progress
    bool m_velocityBlockedLogged = false;  // log "blocked by AI" once per drag, not per tick
    QTimer *m_velocityWatchdog = nullptr;  // deadman: auto-stop if no fresh command arrives
};
