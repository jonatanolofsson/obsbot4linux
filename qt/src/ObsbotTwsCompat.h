// ObsbotTwsCompat — access to the wireless-mic (TWS) and camera-audio entry
// points that the OBSBOT libdev shared library EXPORTS but the public SDK header
// does not DECLARE.
//
// Why this file exists
// -------------------
// The Vox SE wireless-mic feature — and the Tiny 3's own built-in mic array —
// need a handful of calls that are missing from
// sdk/libdev_v2.1.0_8/include/dev/dev.hpp and from OBSBOT's own OBSBOT_Sample
// (grep either for "TWS": nothing). They are nonetheless real, exported,
// non-virtual Device member functions in libdev.so — verified with
// `nm -D libdev.so.1.0.3` on BOTH shipped Linux ABIs (x86_64 and arm64):
//
//   Device::cameraGetTWSInfoR(Device::DevTWSInfo&)
//       _ZN6Device17cameraGetTWSInfoRERNS_10DevTWSInfoE
//   Device::cameraSetTWSKeyTypeR(Device::DevTWSKeyType)
//       _ZN6Device20cameraSetTWSKeyTypeRENS_13DevTWSKeyTypeE
//   Device::cameraGetAudioSelectR(Device::AudioSelectAttr&)
//       _ZN6Device21cameraGetAudioSelectRERNS_15AudioSelectAttrE
//   Device::cameraSetAudioSelect(Device::AudioSelectParam&)
//       _ZN6Device20cameraSetAudioSelectERNS_16AudioSelectParamE
//   Device::cameraTXSetPairEnabled(Device::DevTXType, bool)
//       _ZN6Device22cameraTXSetPairEnabledENS_9DevTXTypeEb
//   Device::cameraTXClearPairedInfo(Device::DevTXType)
//       _ZN6Device23cameraTXClearPairedInfoENS_9DevTXTypeE
//   Device::setBlePairingEnable(bool, unsigned char)
//       _ZN6Device19setBlePairingEnableEbh
//   Device::setBlePairingExit()
//       _ZN6Device17setBlePairingExitEv
//   Device::cameraTXSetAudioMuteR(Device::DevTXType, bool)
//       _ZN6Device21cameraTXSetAudioMuteRENS_9DevTXTypeEb
//   Device::cameraTXGetAudioMuteR(Device::DevTXType, bool&)
//       _ZN6Device21cameraTXGetAudioMuteRENS_9DevTXTypeERb
//   Device::cameraTXSetAudioGainR(Device::DevTXType, int)
//       _ZN6Device21cameraTXSetAudioGainRENS_9DevTXTypeEi
//   Device::cameraTXGetAudioGainR(Device::DevTXType, int&)
//       _ZN6Device21cameraTXGetAudioGainRENS_9DevTXTypeERi
//
// …and, for the camera's OWN microphone (the Tiny 3's mic array), a second
// family in exactly the same situation — exported, undeclared:
//
//   Device::cameraGetAudioVolumeR(short&)              [0-100]
//       _ZN6Device21cameraGetAudioVolumeRERs
//   Device::cameraSetAudioVolumeR(short)
//       _ZN6Device21cameraSetAudioVolumeREs
//   Device::cameraGetAudioSourceMuteR(bool&)
//       _ZN6Device25cameraGetAudioSourceMuteRERb
//   Device::cameraSetAudioSourceMuteR(bool)
//       _ZN6Device25cameraSetAudioSourceMuteREb
//   Device::cameraGetAudioNoiseReduceR(bool&, int&)    level [1-10]
//       _ZN6Device26cameraGetAudioNoiseReduceRERbRi
//   Device::cameraSetAudioNoiseReduceR(bool, int)
//       _ZN6Device26cameraSetAudioNoiseReduceREbi
//   Device::cameraGetAudioAGCR(bool&)
//       _ZN6Device18cameraGetAudioAGCRERb
//   Device::cameraSetAudioAGCR(bool)
//       _ZN6Device18cameraSetAudioAGCREb
//   Device::cameraSetAudioModeU(Device::AudioMode)     pickup pattern
//       _ZN6Device19cameraSetAudioModeUENS_9AudioModeE
//   Device::cameraSetAudioSourceR(int)                 DevAudioSourceType
//       _ZN6Device21cameraSetAudioSourceREi
//   Device::cameraGetSelectedAudioSourceR(unsigned char&)
//       _ZN6Device29cameraGetSelectedAudioSourceRERh
//
// There is deliberately NO getter wrapper for the audio SOURCE or the audio
// MODE here: both are pushed by the camera for free in
// CameraStatus::tiny.audio_mode (a public, documented bitfield —
// `source : 3` / `mode : 5`), so the status push is the readback and no extra
// USB round trip is spent on them. cameraGetSelectedAudioSourceR is bound only
// because it is the one call that reports which source the camera's OWN
// arbitration picked; it is not on any hot path.
//
// The vendored SDK is kept PRISTINE — an unmodified official drop — so these
// prototypes cannot simply be pasted into dev.hpp. Instead the app binds them
// by their exact exported symbol name using GCC/Clang `__asm__` labels. The
// argument TYPES (Device::DevTWSInfo, Device::DevTWSKeyType,
// Device::AudioSelectAttr, Device::AudioSelectParam, Device::AudioMode,
// Device::DevTXType) all live in the stock public header, so only the entry
// points need re-declaring here.
//
// Mind Device::DevTXType: it is ONE-BASED in the official header
// (`enum DevTXType { DevTX1 = 1, DevTX2 };`). Never pass a 0-based slot index.
//
// How the binding works
// --------------------
// Itanium C++ ABI (the Linux x86_64/arm64 C++ ABI): a NON-VIRTUAL member
// function is an ordinary function whose implicit first argument is the `this`
// pointer, and whose reference parameters are passed as pointers. So
// `dev->cameraGetTWSInfoR(info)` is ABI-identical to
// `obsbot_cameraGetTWSInfoR(dev, &info)` declared below. Every symbol bound here
// is a plain text symbol (`T` in nm), not a vtable slot, so no thunk/adjustment
// is involved and the raw Device* from std::shared_ptr::get() is the correct
// `this`. Small by-value struct parameters (Device::AudioMode — two uint8_t)
// keep their normal parameter class: the declaration below names the SAME type
// the library was compiled against, so the compiler reproduces the ABI exactly.
//
// Safety / degradation
// -------------------
//  * The declarations are `__attribute__((weak))`: if a future libdev drops the
//    exports, the app still LINKS and LOADS — the symbol addresses are simply
//    null, ObsbotTws::linked() returns false, and the caller's runtime
//    capability probe turns the mic extras off in the UI instead of the process
//    dying at first call.
//  * Even when the symbols are present, the firmware may not implement the
//    command. CameraWorker therefore probes once on bind (call getInfo(), check
//    rc == RM_RET_OK) and only advertises the feature when the DEVICE answers.
//  * This is Linux/Itanium-ABI specific. On any other toolchain the whole thing
//    compiles down to stubs that return RM_RET_ERR, so linked() is false and the
//    feature is reported as unavailable rather than silently broken.
//
// Nothing else in the app may call these symbols directly — go through the
// ObsbotTws:: wrappers so the null/unsupported checks are never skipped.
#pragma once

#include <cstdint>

#include <dev/dev.hpp>

#if defined(__GNUC__) && defined(__linux__)
#define OBSBOT_TWS_COMPAT 1
#else
#define OBSBOT_TWS_COMPAT 0
#endif

#if OBSBOT_TWS_COMPAT

// extern "C" only to keep OUR names unmangled and un-overloadable; the __asm__
// label is what actually selects the C++-mangled libdev symbol. The calling
// convention is identical either way on the platforms this branch covers.
extern "C" {

/// Device::cameraGetTWSInfoR — one shot, everything: per-mic battery/charging/
/// mute/noise-suppression/gain plus the current multi-function key assignment.
int32_t obsbot_cameraGetTWSInfoR(Device *self, Device::DevTWSInfo *info)
    __asm__("_ZN6Device17cameraGetTWSInfoRERNS_10DevTWSInfoE") __attribute__((weak));

/// Device::cameraSetTWSKeyTypeR — assign what a click on the mic's
/// multi-function button triggers on the camera (see Device::DevTWSKeyType).
int32_t obsbot_cameraSetTWSKeyTypeR(Device *self, Device::DevTWSKeyType key)
    __asm__("_ZN6Device20cameraSetTWSKeyTypeRENS_13DevTWSKeyTypeE") __attribute__((weak));

/// Device::cameraGetAudioSelectR — audio-source auto-select attributes. Useful
/// as a diagnostic: has_pair_record == 0 means this camera has never had a mic
/// paired to it.
int32_t obsbot_cameraGetAudioSelectR(Device *self, Device::AudioSelectAttr *attr)
    __asm__("_ZN6Device21cameraGetAudioSelectRERNS_15AudioSelectAttrE") __attribute__((weak));

/// Device::cameraTXSetPairEnabled — put a transmitter SLOT into pairing mode so
/// a Vox SE held in its own pairing mode can link. tx is ONE-based (DevTX1 = 1).
/// HARDWARE NOTE: the camera ACKs this with rc=0 while ASLEEP and then never
/// scans — callers must wake it first and confirm via wireless_mic.tx_state.
int32_t obsbot_cameraTXSetPairEnabled(Device *self, Device::DevTXType tx, bool enabled)
    __asm__("_ZN6Device22cameraTXSetPairEnabledENS_9DevTXTypeEb") __attribute__((weak));

/// Device::cameraTXClearPairedInfo — forget the mic linked to a slot.
int32_t obsbot_cameraTXClearPairedInfo(Device *self, Device::DevTXType tx)
    __asm__("_ZN6Device23cameraTXClearPairedInfoENS_9DevTXTypeE") __attribute__((weak));

/// PER-MIC mute and gain. cameraGetTWSInfoR already REPORTS both (DevTWSInfo's
/// mic_info.micN_mute and micN_gain), so these four are the WRITE side plus a
/// per-slot second opinion on the readback. tx is ONE-based (DevTX1 = 1).
///
/// GAIN RANGE IS UNDOCUMENTED. The setter takes a plain int and the SDK header
/// documents no bounds for it; the only hard fact is that DevTWSInfo stores the
/// value the device reports back in an `int8_t`, so anything outside [-128,127]
/// cannot round-trip. That int8 is the ONLY defensible clamp — callers must not
/// invent a 0-100 scale, and the UI must adjust the value the DEVICE reports
/// rather than assert a range of its own. See CameraWorker::cmdSetTxGain, which
/// reads the value straight back with cameraTXGetAudioGainR and logs it when the
/// camera lands somewhere other than where it was asked to.
///
/// There is deliberately no per-mic NOISE-REDUCTION setter here: libdev exports
/// none. (cameraSetTWSFuncR(DevTWSFuncType, bool, short) can switch the TWS
/// denoise FUNCTION, but it takes no DevTXType — it is not per-mic — so the
/// per-mic ns/ns_level fields of DevTWSInfo are read-only for this app.)
int32_t obsbot_cameraTXSetAudioMuteR(Device *self, Device::DevTXType tx, bool muted)
    __asm__("_ZN6Device21cameraTXSetAudioMuteRENS_9DevTXTypeEb") __attribute__((weak));
int32_t obsbot_cameraTXGetAudioMuteR(Device *self, Device::DevTXType tx, bool *muted)
    __asm__("_ZN6Device21cameraTXGetAudioMuteRENS_9DevTXTypeERb") __attribute__((weak));
int32_t obsbot_cameraTXSetAudioGainR(Device *self, Device::DevTXType tx, int gain)
    __asm__("_ZN6Device21cameraTXSetAudioGainRENS_9DevTXTypeEi") __attribute__((weak));
int32_t obsbot_cameraTXGetAudioGainR(Device *self, Device::DevTXType tx, int *gain)
    __asm__("_ZN6Device21cameraTXGetAudioGainRENS_9DevTXTypeERi") __attribute__((weak));

/// Device::setBlePairingEnable / setBlePairingExit — the camera's generic BLE
/// pairing window. Belt-and-braces around the TX pairing call: both return rc=0
/// on a Tiny 3 but pairing has also been observed to work without them, so
/// treat them as optional, NOT load-bearing.
int32_t obsbot_setBlePairingEnable(Device *self, bool enable, unsigned char type)
    __asm__("_ZN6Device19setBlePairingEnableEbh") __attribute__((weak));
int32_t obsbot_setBlePairingExit(Device *self)
    __asm__("_ZN6Device17setBlePairingExitEv") __attribute__((weak));

// --- the camera's OWN microphone (Tiny 3 mic array) -------------------------

/// Device::cameraGetAudioVolumeR / cameraSetAudioVolumeR — input gain of the
/// SELECTED audio source, range [0-100]. Note the `short`: the getter takes a
/// short&, so passing an int* here would corrupt the stack.
int32_t obsbot_cameraGetAudioVolumeR(Device *self, short *volume)
    __asm__("_ZN6Device21cameraGetAudioVolumeRERs") __attribute__((weak));
int32_t obsbot_cameraSetAudioVolumeR(Device *self, short volume)
    __asm__("_ZN6Device21cameraSetAudioVolumeREs") __attribute__((weak));

/// Device::cameraGetAudioSourceMuteR / cameraSetAudioSourceMuteR — mute of the
/// SELECTED audio source (not the Vox SE's own mute, which lives in DevTWSInfo).
int32_t obsbot_cameraGetAudioSourceMuteR(Device *self, bool *muted)
    __asm__("_ZN6Device25cameraGetAudioSourceMuteRERb") __attribute__((weak));
int32_t obsbot_cameraSetAudioSourceMuteR(Device *self, bool muted)
    __asm__("_ZN6Device25cameraSetAudioSourceMuteREb") __attribute__((weak));

/// Device::cameraGetAudioNoiseReduceR / cameraSetAudioNoiseReduceR — noise
/// suppression on/off plus its strength, range [1-10] (the range the public
/// header documents for DevAudioInputSourceNoiseReduce::level).
int32_t obsbot_cameraGetAudioNoiseReduceR(Device *self, bool *enabled, int *level)
    __asm__("_ZN6Device26cameraGetAudioNoiseReduceRERbRi") __attribute__((weak));
int32_t obsbot_cameraSetAudioNoiseReduceR(Device *self, bool enabled, int level)
    __asm__("_ZN6Device26cameraSetAudioNoiseReduceREbi") __attribute__((weak));

/// Device::cameraGetAudioAGCR / cameraSetAudioAGCR — automatic gain control.
int32_t obsbot_cameraGetAudioAGCR(Device *self, bool *enabled)
    __asm__("_ZN6Device18cameraGetAudioAGCRERb") __attribute__((weak));
int32_t obsbot_cameraSetAudioAGCR(Device *self, bool enabled)
    __asm__("_ZN6Device18cameraSetAudioAGCREb") __attribute__((weak));

/// Device::cameraSetAudioModeU — the mic array's pickup pattern. The struct's
/// `source` field is annotated "暂时用0" ("use 0 for now") in the public header,
/// so only `mode` (a Device::AudioModeType) carries meaning. Readback is the
/// status push's tiny.audio_mode.mode, NOT a getter — there is no exported one.
int32_t obsbot_cameraSetAudioModeU(Device *self, Device::AudioMode mode)
    __asm__("_ZN6Device19cameraSetAudioModeUENS_9AudioModeE") __attribute__((weak));

/// Device::cameraSetAudioSourceR — pick the input, a Device::DevAudioSourceType
/// (0 built-in, 3 Bluetooth = the Vox SE).
/// HARDWARE NOTE: while AudioSelectAttr::is_auto == 1 the camera arbitrates the
/// source itself and this call is ACCEPTED (rc=0) and then silently reverted.
/// Always clear auto with setAudioSelect{is_auto=0} first — see
/// CameraWorker::cmdSetAudioSource.
int32_t obsbot_cameraSetAudioSourceR(Device *self, int source)
    __asm__("_ZN6Device21cameraSetAudioSourceREi") __attribute__((weak));

/// Device::cameraSetAudioSelect — turn the camera's own source arbitration on
/// or off (the write counterpart of cameraGetAudioSelectR).
int32_t obsbot_cameraSetAudioSelect(Device *self, Device::AudioSelectParam *param)
    __asm__("_ZN6Device20cameraSetAudioSelectERNS_16AudioSelectParamE") __attribute__((weak));

/// Device::cameraGetSelectedAudioSourceR — which source the camera is actually
/// using right now (a DevAudioSourceType in a byte). Diagnostic only: the status
/// push carries the same value in tiny.audio_mode.source for free.
int32_t obsbot_cameraGetSelectedAudioSourceR(Device *self, unsigned char *source)
    __asm__("_ZN6Device29cameraGetSelectedAudioSourceRERh") __attribute__((weak));

} // extern "C"

#endif // OBSBOT_TWS_COMPAT

namespace ObsbotTws {

/// True when this build can reach the undocumented entry points at all (right
/// toolchain AND the symbols resolved in the linked libdev). Says nothing about
/// whether the connected camera implements them — probe with getInfo() for that.
inline bool linked() {
#if OBSBOT_TWS_COMPAT
    return obsbot_cameraGetTWSInfoR != nullptr && obsbot_cameraSetTWSKeyTypeR != nullptr;
#else
    return false;
#endif
}

inline int32_t getInfo(Device *dev, Device::DevTWSInfo &info) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetTWSInfoR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetTWSInfoR(dev, &info);
#else
    (void)dev;
    (void)info;
    return RM_RET_ERR;
#endif
}

inline int32_t setKeyType(Device *dev, Device::DevTWSKeyType key) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetTWSKeyTypeR == nullptr) return RM_RET_ERR;
    return obsbot_cameraSetTWSKeyTypeR(dev, key);
#else
    (void)dev;
    (void)key;
    return RM_RET_ERR;
#endif
}

inline int32_t getAudioSelect(Device *dev, Device::AudioSelectAttr &attr) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetAudioSelectR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetAudioSelectR(dev, &attr);
#else
    (void)dev;
    (void)attr;
    return RM_RET_ERR;
#endif
}

/// True when the transmitter pairing/clear entry points resolved. Independent of
/// linked(): a libdev could ship one set and not the other.
inline bool pairingLinked() {
#if OBSBOT_TWS_COMPAT
    return obsbot_cameraTXSetPairEnabled != nullptr && obsbot_cameraTXClearPairedInfo != nullptr;
#else
    return false;
#endif
}

/// tx is ONE-based: Device::DevTX1 / Device::DevTX2.
inline int32_t setPairEnabled(Device *dev, Device::DevTXType tx, bool enabled) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraTXSetPairEnabled == nullptr) return RM_RET_ERR;
    return obsbot_cameraTXSetPairEnabled(dev, tx, enabled);
#else
    (void)dev;
    (void)tx;
    (void)enabled;
    return RM_RET_ERR;
#endif
}

inline int32_t clearPairedInfo(Device *dev, Device::DevTXType tx) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraTXClearPairedInfo == nullptr) return RM_RET_ERR;
    return obsbot_cameraTXClearPairedInfo(dev, tx);
#else
    (void)dev;
    (void)tx;
    return RM_RET_ERR;
#endif
}

/// True when the PER-MIC mute/gain entry points resolved. A third, independent
/// gate: libdev could perfectly well ship cameraGetTWSInfoR (which REPORTS mute
/// and gain) without the cameraTXSet*/Get* family that CHANGES them, and then
/// the mic cards must show those two as read-only rather than offer a control
/// that cannot fire. Says nothing about whether the camera answers — the set's
/// rc and the readback right after it are the honest verdict for that.
inline bool txAudioLinked() {
#if OBSBOT_TWS_COMPAT
    return obsbot_cameraTXSetAudioMuteR != nullptr && obsbot_cameraTXSetAudioGainR != nullptr
           && obsbot_cameraTXGetAudioGainR != nullptr && obsbot_cameraTXGetAudioMuteR != nullptr;
#else
    return false;
#endif
}

/// tx is ONE-based: Device::DevTX1 / Device::DevTX2.
inline int32_t setTxMute(Device *dev, Device::DevTXType tx, bool muted) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraTXSetAudioMuteR == nullptr) return RM_RET_ERR;
    return obsbot_cameraTXSetAudioMuteR(dev, tx, muted);
#else
    (void)dev;
    (void)tx;
    (void)muted;
    return RM_RET_ERR;
#endif
}

inline int32_t getTxMute(Device *dev, Device::DevTXType tx, bool &muted) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraTXGetAudioMuteR == nullptr) return RM_RET_ERR;
    return obsbot_cameraTXGetAudioMuteR(dev, tx, &muted);
#else
    (void)dev;
    (void)tx;
    (void)muted;
    return RM_RET_ERR;
#endif
}

/// gain is in the device's OWN, undocumented units — see the declaration above.
/// Nothing here rescales it; the caller passes through what the device reported
/// (± a step), and the int8 clamp is the only bound anyone can defend.
inline int32_t setTxGain(Device *dev, Device::DevTXType tx, int gain) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraTXSetAudioGainR == nullptr) return RM_RET_ERR;
    return obsbot_cameraTXSetAudioGainR(dev, tx, gain);
#else
    (void)dev;
    (void)tx;
    (void)gain;
    return RM_RET_ERR;
#endif
}

inline int32_t getTxGain(Device *dev, Device::DevTXType tx, int &gain) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraTXGetAudioGainR == nullptr) return RM_RET_ERR;
    return obsbot_cameraTXGetAudioGainR(dev, tx, &gain);
#else
    (void)dev;
    (void)tx;
    (void)gain;
    return RM_RET_ERR;
#endif
}

/// Optional BLE pairing window (see the declarations above) — best effort, the
/// return value is informational only.
inline int32_t blePairingEnable(Device *dev, bool enable, unsigned char type) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_setBlePairingEnable == nullptr) return RM_RET_ERR;
    return obsbot_setBlePairingEnable(dev, enable, type);
#else
    (void)dev;
    (void)enable;
    (void)type;
    return RM_RET_ERR;
#endif
}

inline int32_t blePairingExit(Device *dev) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_setBlePairingExit == nullptr) return RM_RET_ERR;
    return obsbot_setBlePairingExit(dev);
#else
    (void)dev;
    return RM_RET_ERR;
#endif
}

/// Turn the camera's own audio-source arbitration on/off. Must be set to false
/// BEFORE setAudioSource, or the camera reverts the pick (hardware finding).
inline int32_t setAudioSelect(Device *dev, bool isAuto) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioSelect == nullptr) return RM_RET_ERR;
    Device::AudioSelectParam p{};
    p.is_auto = isAuto ? 1 : 0;
    return obsbot_cameraSetAudioSelect(dev, &p);
#else
    (void)dev;
    (void)isAuto;
    return RM_RET_ERR;
#endif
}

// --- the camera's OWN microphone --------------------------------------------

/// True when this build can reach the camera-audio entry points at all. Separate
/// from linked(): a future libdev could ship one family and not the other.
/// Says nothing about whether the CAMERA answers — probe with getVolume().
inline bool audioLinked() {
#if OBSBOT_TWS_COMPAT
    return obsbot_cameraGetAudioVolumeR != nullptr && obsbot_cameraSetAudioVolumeR != nullptr;
#else
    return false;
#endif
}

inline int32_t getAudioVolume(Device *dev, short &volume) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetAudioVolumeR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetAudioVolumeR(dev, &volume);
#else
    (void)dev;
    (void)volume;
    return RM_RET_ERR;
#endif
}

inline int32_t setAudioVolume(Device *dev, short volume) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioVolumeR == nullptr) return RM_RET_ERR;
    return obsbot_cameraSetAudioVolumeR(dev, volume);
#else
    (void)dev;
    (void)volume;
    return RM_RET_ERR;
#endif
}

inline int32_t getAudioMute(Device *dev, bool &muted) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetAudioSourceMuteR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetAudioSourceMuteR(dev, &muted);
#else
    (void)dev;
    (void)muted;
    return RM_RET_ERR;
#endif
}

inline int32_t setAudioMute(Device *dev, bool muted) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioSourceMuteR == nullptr) return RM_RET_ERR;
    return obsbot_cameraSetAudioSourceMuteR(dev, muted);
#else
    (void)dev;
    (void)muted;
    return RM_RET_ERR;
#endif
}

inline int32_t getAudioNoiseReduce(Device *dev, bool &enabled, int &level) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetAudioNoiseReduceR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetAudioNoiseReduceR(dev, &enabled, &level);
#else
    (void)dev;
    (void)enabled;
    (void)level;
    return RM_RET_ERR;
#endif
}

inline int32_t setAudioNoiseReduce(Device *dev, bool enabled, int level) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioNoiseReduceR == nullptr) return RM_RET_ERR;
    return obsbot_cameraSetAudioNoiseReduceR(dev, enabled, level);
#else
    (void)dev;
    (void)enabled;
    (void)level;
    return RM_RET_ERR;
#endif
}

inline int32_t getAudioAgc(Device *dev, bool &enabled) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetAudioAGCR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetAudioAGCR(dev, &enabled);
#else
    (void)dev;
    (void)enabled;
    return RM_RET_ERR;
#endif
}

inline int32_t setAudioAgc(Device *dev, bool enabled) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioAGCR == nullptr) return RM_RET_ERR;
    return obsbot_cameraSetAudioAGCR(dev, enabled);
#else
    (void)dev;
    (void)enabled;
    return RM_RET_ERR;
#endif
}

/// mode is a Device::AudioModeType. source is pinned to 0 — the header says
/// "use 0 for now" and the field carries no meaning yet.
inline int32_t setAudioMode(Device *dev, int mode) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioModeU == nullptr) return RM_RET_ERR;
    Device::AudioMode m{};
    m.source = 0;
    m.mode = static_cast<uint8_t>(mode);
    return obsbot_cameraSetAudioModeU(dev, m);
#else
    (void)dev;
    (void)mode;
    return RM_RET_ERR;
#endif
}

/// source is a Device::DevAudioSourceType. Clear is_auto FIRST (see the
/// declaration's hardware note) or the camera will revert this.
inline int32_t setAudioSource(Device *dev, int source) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraSetAudioSourceR == nullptr) return RM_RET_ERR;
    return obsbot_cameraSetAudioSourceR(dev, source);
#else
    (void)dev;
    (void)source;
    return RM_RET_ERR;
#endif
}

inline int32_t getSelectedAudioSource(Device *dev, unsigned char &source) {
#if OBSBOT_TWS_COMPAT
    if (!dev || obsbot_cameraGetSelectedAudioSourceR == nullptr) return RM_RET_ERR;
    return obsbot_cameraGetSelectedAudioSourceR(dev, &source);
#else
    (void)dev;
    (void)source;
    return RM_RET_ERR;
#endif
}

} // namespace ObsbotTws
