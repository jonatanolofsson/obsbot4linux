// ObsbotTwsCompat — access to three wireless-mic (TWS) entry points that the
// OBSBOT libdev shared library EXPORTS but the public SDK header does not
// DECLARE.
//
// Why this file exists
// -------------------
// The Vox SE wireless-mic feature needs a handful of calls that are missing from
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
//   Device::cameraTXSetPairEnabled(Device::DevTXType, bool)
//       _ZN6Device22cameraTXSetPairEnabledENS_9DevTXTypeEb
//   Device::cameraTXClearPairedInfo(Device::DevTXType)
//       _ZN6Device23cameraTXClearPairedInfoENS_9DevTXTypeE
//   Device::setBlePairingEnable(bool, unsigned char)
//       _ZN6Device19setBlePairingEnableEbh
//   Device::setBlePairingExit()
//       _ZN6Device17setBlePairingExitEv
//
// The vendored SDK is kept PRISTINE — an unmodified official drop — so these
// prototypes cannot simply be pasted into dev.hpp. Instead the app binds them
// by their exact exported symbol name using GCC/Clang `__asm__` labels. The
// argument TYPES (Device::DevTWSInfo, Device::DevTWSKeyType,
// Device::AudioSelectAttr, Device::DevTXType) all live in the stock public
// header, so only the entry points need re-declaring here.
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
// `obsbot_cameraGetTWSInfoR(dev, &info)` declared below. All three symbols are
// plain text symbols (`T` in nm), not vtable slots, so no thunk/adjustment is
// involved and the raw Device* from std::shared_ptr::get() is the correct
// `this`.
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

/// Device::setBlePairingEnable / setBlePairingExit — the camera's generic BLE
/// pairing window. Belt-and-braces around the TX pairing call: both return rc=0
/// on a Tiny 3 but pairing has also been observed to work without them, so
/// treat them as optional, NOT load-bearing.
int32_t obsbot_setBlePairingEnable(Device *self, bool enable, unsigned char type)
    __asm__("_ZN6Device19setBlePairingEnableEbh") __attribute__((weak));
int32_t obsbot_setBlePairingExit(Device *self)
    __asm__("_ZN6Device17setBlePairingExitEv") __attribute__((weak));

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

} // namespace ObsbotTws
