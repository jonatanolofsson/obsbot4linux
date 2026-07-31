// Microphones — the OBSBOT Vox SE wireless mic AND the Tiny 3's own mic array.
//
// WIRELESS. The Vox SE's receiver IS the camera. Pairing is a two-sided
// handshake: the mic goes into pairing mode from its own button (hold ~6 s until
// the light flashes green) while the camera holds a transmitter slot open —
// that's what Pair does here. The camera must be AWAKE for it: asleep it accepts
// the command and never turns its radio on, so the app wakes it first.
// Presence comes from what the camera reports (CameraStatus.tiny.wireless_mic),
// never from the pair command's return code. Battery/mute and the button
// assignment come from the extended mic API, which is probed on connect and
// shown as unavailable if this camera/firmware does not answer.
//
// BUILT-IN. The Audio section below is the camera's own microphone: which input
// it listens to, the mic array's pickup pattern, gain, mute, noise reduction and
// automatic gain. Source and pattern are read back from the status push (the
// camera has no getter for either), the rest from the camera-audio API, which is
// probed exactly like the mic extras. The AUDIO STREAM itself still needs
// nothing here — it arrives on the camera's USB-audio input either way.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Obsbot

Item {
    id: root

    // The input-level meter holds the camera's AUDIO device while it runs, so it
    // only runs while this page is on screen — otherwise the app would sit on the
    // camera's microphone for its whole lifetime and other applications could
    // find it busy. StackLayout already drives `visible` for its pages (only the
    // current one is visible), so that flag is exactly the right signal.
    // setActive is idempotent, so a spurious re-fire costs nothing.
    onVisibleChanged: audioMeter.setActive(visible)
    Component.onCompleted: audioMeter.setActive(visible)

    // A live 0–100 slider for a device value, applied on release (few SDK calls)
    // — the same shape and styling as ImagePage's picture sliders.
    component AudioSlider: RowLayout {
        id: srow
        property string label: ""
        property int boundValue: 0
        signal applied(int value)
        spacing: 10
        Text { text: srow.label; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13; Layout.preferredWidth: 96 }
        Slider {
            id: sl
            Layout.fillWidth: true
            from: 0; to: 100; stepSize: 1
            value: srow.boundValue
            onPressedChanged: if (!pressed) srow.applied(Math.round(value))
            opacity: enabled ? 1 : 0.4

            background: Rectangle {
                x: sl.leftPadding
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: sl.availableWidth; height: 5; radius: 2.5
                color: Qt.rgba(1, 1, 1, 0.12)
                Rectangle {
                    width: sl.position * parent.width; height: parent.height; radius: 2.5
                    color: Theme.accent
                }
            }
            handle: Rectangle {
                x: sl.leftPadding + sl.position * (sl.availableWidth - width)
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: 18; height: 18; radius: 9
                color: sl.pressed ? Theme.accentDeep : Theme.accentSoft
                border.width: 1; border.color: Theme.accentDeep
            }
        }
        Rectangle {
            Layout.preferredWidth: 40; Layout.preferredHeight: 24
            radius: Theme.rControl
            color: sl.pressed ? Theme.accentTint : Qt.rgba(1, 1, 1, 0.04)
            border.width: 1; border.color: sl.pressed ? Theme.accentRing : Theme.border
            Text {
                anchors.centerIn: parent
                // "—" while the camera has not reported a gain: showing 0 would
                // claim the mic is turned all the way down.
                text: srow.boundValue < 0 ? "—" : Math.round(sl.value)
                color: sl.pressed ? Theme.accentSoft : (srow.boundValue < 0 ? Theme.dimmer : Theme.fg)
                font.family: Theme.mono; font.pixelSize: 13
            }
        }
    }

    ColumnLayout {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width, 720)
        spacing: Theme.s3

        SectionLabel { text: "Wireless mic" }

        // Pairing — the camera opens a slot; the mic must be in pairing mode too.
        GlassPanel {
            id: pairPanel
            Layout.fillWidth: true
            implicitHeight: pairCol.implicitHeight + 24
            readonly property bool busy: cam.connected && (cam.micPairBusy || cam.micPairing || cam.micScanning)
            ColumnLayout {
                id: pairCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "Pair a Vox SE"; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                    Item { Layout.fillWidth: true }
                    // Live cue: our command in flight, or the camera itself
                    // reporting that it is pairing/scanning.
                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        width: 9; height: 9; radius: 4.5
                        visible: pairPanel.busy
                        color: Theme.busy
                        SequentialAnimation on opacity {
                            running: pairPanel.busy; loops: Animation.Infinite
                            NumberAnimation { from: 0.3; to: 1.0; duration: 450 }
                            NumberAnimation { from: 1.0; to: 0.3; duration: 450 }
                        }
                    }
                }
                Text {
                    text: "Press Pair, then take the mic out of its charging dock and hold its button for about "
                        + "6 seconds, until the light flashes green. The app wakes the camera automatically; a "
                        + "sleeping Tiny 3 accepts the command but never turns its radio on. The camera links up "
                        + "to two mics, chooses which slot to use itself, and reconnects them afterwards."
                    color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                // One action, because the camera assigns the slot: asking it to
                // pair a specific transmitter is accepted and then ignored.
                ActionButton {
                    Layout.fillWidth: true
                    text: "Pair a mic"
                    variant: "secondary"
                    action: "mic pair tx1"
                    enabled: cam.connected
                    onClicked: cam.micPair()
                }
                KeyValue {
                    Layout.fillWidth: true
                    key: "link"
                    // While our pair request is open the camera is genuinely
                    // waiting on the human, so say so instead of showing the
                    // idle link mode and looking like nothing happened.
                    value: !cam.connected ? "—"
                         : (cam.micPairBusy ? "listening — hold the mic's button for ~6 s"
                         : (cam.micScanning ? "scanning for a mic …"
                         : (cam.micPairing ? "pairing …"
                         : (cam.micTwsMode ? "Bluetooth TWS mode" : "2.4 GHz mic mode"))))
                    unknown: !cam.connected
                    valueColor: cam.micPairBusy ? Theme.busy : Theme.accentSoft
                }
                KeyValue {
                    Layout.fillWidth: true
                    key: "history"
                    value: !cam.connected || cam.micPairRecord < 0 ? "—"
                         : (cam.micPairRecord ? "a mic has been paired to this camera"
                                              : "no mic has ever been paired to this camera")
                    unknown: !cam.connected || cam.micPairRecord < 0
                }
            }
        }

        // One status card per transmitter slot — model mirrors PresetsPage's
        // cam.presets: a list of maps { tx, online, battery, charging, muted }.
        // Presence comes from the camera's status push; battery/mute need the
        // extended mic API (cam.capMicButton) and read "—" without it.
        Repeater {
            model: cam.micTxList
            delegate: GlassPanel {
                id: txCard
                required property var modelData
                readonly property bool live: cam.connected && txCard.modelData.online
                readonly property bool battKnown: txCard.live && txCard.modelData.battery >= 0
                // Shared key column for every label in the card, so the rows line
                // up whichever of them happens to be the longest.
                readonly property int keyWidth: 108
                Layout.fillWidth: true
                implicitHeight: txCol.implicitHeight + 24
                ColumnLayout {
                    id: txCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "MIC " + txCard.modelData.tx; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            width: 8; height: 8; radius: 4
                            color: txCard.live ? Theme.live : Theme.unknown
                            opacity: txCard.live ? 1.0 : 0.5
                        }
                    }
                    // Wider key column than the page default: "noise reduction"
                    // needs it, and every row in the card has to line up.
                    KeyValue {
                        Layout.fillWidth: true
                        keyWidth: txCard.keyWidth
                        key: "status"
                        value: !cam.connected ? "—" : (txCard.modelData.online ? "connected" : "not connected")
                        unknown: !txCard.live
                        valueColor: Theme.accentSoft
                    }
                    KeyValue {
                        Layout.fillWidth: true
                        keyWidth: txCard.keyWidth
                        key: "battery"
                        value: txCard.battKnown
                             ? (txCard.modelData.battery + "%" + (txCard.modelData.charging ? " (on charge)" : ""))
                             : "—"
                        unknown: !txCard.battKnown
                        valueColor: (txCard.battKnown && txCard.modelData.battery <= 15 && !txCard.modelData.charging)
                                  ? Theme.offline : Theme.accentSoft
                    }
                    KeyValue {
                        Layout.fillWidth: true
                        keyWidth: txCard.keyWidth
                        key: "microphone"
                        value: !(txCard.live && cam.capMicButton) ? "—"
                             : (txCard.modelData.muted ? "muted" : "live")
                        unknown: !(txCard.live && cam.capMicButton)
                        valueColor: txCard.modelData.muted ? Theme.degraded : Theme.accentSoft
                    }
                    // Noise reduction is REPORT-ONLY, and says so on the row
                    // itself. libdev exports a per-mic gain setter but no per-mic
                    // noise-reduction setter, so there is nothing honest to offer
                    // here — showing a control that cannot fire would be worse
                    // than showing the value alone.
                    KeyValue {
                        Layout.fillWidth: true
                        keyWidth: txCard.keyWidth
                        key: "noise reduction"
                        value: !(txCard.live && cam.capMicButton) || txCard.modelData.ns < 0 ? "—"
                             : (txCard.modelData.ns === 1
                                    ? "on · level " + txCard.modelData.nsLevel + " (set on the mic)"
                                    : "off (set on the mic)")
                        unknown: !(txCard.live && cam.capMicButton) || txCard.modelData.ns < 0
                    }
                    // Transient cue from the camera's device-event push: the mic's
                    // button was pressed. Absent — permanently — on a camera that
                    // never sends events, which is the expected case here (see the
                    // note under the mic cards), so this row must never be the
                    // only place a piece of state is visible.
                    // Always shown. Hiding it when idle made "not pressed" and
                    // "this camera never reports presses" look identical — and on
                    // a Tiny 3, which sends no events at all, it simply never
                    // appeared. A permanent row says which of the two it is.
                    KeyValue {
                        Layout.fillWidth: true
                        keyWidth: txCard.keyWidth
                        key: "last press"
                        // The text persists — a press that happened stays reported,
                        // with the time it happened. Only the highlight fades.
                        value: txCard.modelData.tip !== ""
                             ? txCard.modelData.tip + " · " + txCard.modelData.tipAt
                             : (!cam.devEventsSeen ? "this camera has not reported any yet"
                                                   : "none on this mic yet")
                        unknown: txCard.modelData.tip === ""
                        valueColor: txCard.modelData.tipRecent ? Theme.busy : Theme.accentSoft
                    }
                    // Per-mic gain, stepped in the DEVICE's own units.
                    //
                    // Not a slider on purpose: the SDK documents no range for
                    // wireless-mic gain, so a 0–100 track would assert a scale
                    // nobody has verified — and would jump the mic somewhere
                    // arbitrary the moment it was touched. Stepping the value the
                    // camera reported keeps every number on screen one it said.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        enabled: txCard.live && cam.capMicGain && txCard.modelData.gainKnown
                        Text {
                            text: "gain"
                            color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12
                            Layout.preferredWidth: txCard.keyWidth
                        }
                        // No decAction/incAction: both buttons issue the same
                        // command, so binding either to it lit BOTH on every
                        // result. The number itself is the feedback — it now
                        // moves on the press rather than waiting for the camera.
                        Stepper {
                            valueText: txCard.modelData.gainKnown ? String(txCard.modelData.gain) : "—"
                            onDecrement: cam.nudgeMicGain(txCard.modelData.tx, -1)
                            onIncrement: cam.nudgeMicGain(txCard.modelData.tx, 1)
                        }
                        Text {
                            // Just the range. An earlier version explained the
                            // units at length and, worse, tried to LEARN the
                            // limits by treating "the camera reported something
                            // other than we asked for" as a refusal — which a
                            // slow readback looks exactly like, so it invented
                            // limits from its own lag. Observed on a Vox SE.
                            text: "0–23"
                            color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        spacing: 8
                        // The mic's OWN mute — the same state its button toggles,
                        // and distinct from the Mute in the Audio section below,
                        // which mutes whichever input the camera is listening to.
                        ToggleChip {
                            text: "Mute mic"
                            tone: Theme.offline
                            enabled: txCard.live && cam.capMicGain
                            checked: txCard.modelData.muted
                            onToggled: (c) => cam.setMicMute(txCard.modelData.tx, c)
                        }
                        Item { Layout.fillWidth: true }
                        // Clear IS per-slot — unlike pairing, this names the slot
                        // to empty. Nothing to clear on an empty slot, so the
                        // button would only be able to fail.
                        ActionButton {
                            text: "Clear"
                            variant: "ghost"
                            action: "mic clear tx" + txCard.modelData.tx
                            enabled: cam.connected && txCard.modelData.online
                            onClicked: cam.micClearPairing(txCard.modelData.tx)
                        }
                    }
                }
            }
        }

        // Two honest footnotes for the cards above, both stated once rather than
        // repeated per slot.
        Text {
            Layout.fillWidth: true
            text: (cam.capMicButton && !cam.capMicGain
                       ? "Mute and gain are shown but cannot be changed from here: this build could not "
                         + "reach the per-mic control calls. "
                       : "")
                + (cam.capMicNoiseReduce
                       ? ""
                       : "Per-mic noise reduction is read-only — the camera exposes no call to "
                         + "change it; use the mic's own button. ")
                + (cam.connected && !cam.devEventsSeen
                       ? "Mic button presses would appear on the card above, but this camera has not "
                         + "sent a single device event so far — the SDK documents that push for the "
                         + "Tail Air, so a Tiny 3 may never send one. Nothing here depends on it."
                       : "")
            color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        SectionLabel { text: "Audio" }

        // Honest capability gate — same idiom as the button-action banner below.
        // The camera-audio calls are exported by libdev but absent from the
        // public SDK header, so they are probed on connect and the whole section
        // is disabled when this camera/firmware does not answer.
        Rectangle {
            visible: !cam.capAudio
            Layout.fillWidth: true
            radius: Theme.rControl
            color: Qt.rgba(Theme.degraded.r, Theme.degraded.g, Theme.degraded.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.degraded.r, Theme.degraded.g, Theme.degraded.b, 0.4)
            implicitHeight: audioBanner.implicitHeight + 20
            Text {
                id: audioBanner
                anchors.fill: parent; anchors.margins: 10
                text: cam.connected
                    ? "The camera-audio API is unavailable on this camera/firmware — source, pickup pattern, gain and processing are disabled."
                    : "Connect the camera to read its microphone settings."
                color: Theme.degraded
                font.family: Theme.mono; font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        // Which input the camera actually listens to.
        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: srcCol.implicitHeight + 24
            ColumnLayout {
                id: srcCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                enabled: cam.capAudio
                Text { text: "Input source"; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                Text {
                    text: "On Automatic the camera chooses the input itself — and it wins: picking a source while "
                        + "Automatic is on is accepted and then silently reverted. Choosing Built-in or Wireless "
                        + "therefore turns Automatic off first, in one command."
                    color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                // No segment is lit while the camera's answer is unknown, or when
                // it reports an input this camera has no control for (aux, USB-C)
                // — better a blank selector than one pointing at the wrong thing.
                Segmented {
                    Layout.alignment: Qt.AlignLeft
                    options: ["Auto", "Built-in", "Wireless"]
                    currentIndex: cam.audioAuto === 1 ? 0
                                : (cam.audioSource === 0 ? 1
                                : (cam.audioSource === 3 ? 2 : -1))
                    enabled: cam.connected && cam.capAudio
                    onActivated: (i) => { if (i === 0) cam.setAudioAuto(true); else cam.setAudioSource(i === 1 ? 0 : 3) }
                }
                // What the DEVICE says, always — never the pick we are still
                // waiting on. The two disagreeing for a moment is the truth.
                KeyValue {
                    Layout.fillWidth: true
                    keyWidth: 118
                    key: "camera reports"
                    value: cam.audioSourceName
                    unknown: cam.audioSourceName === "—"
                }
                KeyValue {
                    Layout.fillWidth: true
                    keyWidth: 118
                    key: "selection"
                    value: !cam.connected || cam.audioAuto < 0 ? "—"
                         : (cam.audioAuto ? "automatic — the camera decides"
                                          : "manual — pinned to the source above")
                    unknown: !cam.connected || cam.audioAuto < 0
                }
            }
        }

        // The built-in mic array's pickup pattern.
        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: patCol.implicitHeight + 24
            ColumnLayout {
                id: patCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                enabled: cam.capAudio
                Text { text: "Pickup pattern"; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                Text {
                    text: "Shapes what the camera's own mic array listens to: Omni hears everything, Front points "
                        + "at you, Back away from you, Both covers a table. Stereo and Music are named but not "
                        + "described by the SDK — try them rather than trust the label. Applies to the built-in "
                        + "array, not to a wireless mic."
                    color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                // Short labels so each fits Segmented's fixed cells; the full name
                // of whatever the camera reports is spelled out in the row below.
                // Wider than Segmented's default 74 px cells so "Front+Back"
                // fits without clipping ("Both" was ambiguous — both what?).
                Segmented {
                    Layout.alignment: Qt.AlignLeft
                    Layout.preferredWidth: 6 * 92
                    options: ["Omni", "Stereo", "Front", "Back", "Front+Back", "Music"]
                    currentIndex: cam.audioMode
                    enabled: cam.connected && cam.capAudio
                    onActivated: (i) => cam.setAudioMode(i)
                }
                KeyValue {
                    Layout.fillWidth: true
                    keyWidth: 118
                    key: "camera reports"
                    value: cam.audioModeName
                    unknown: cam.audioModeName === "—"
                }
            }
        }

        // Gain and the processing chain, all read back from the camera.
        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: lvlCol.implicitHeight + 24
            ColumnLayout {
                id: lvlCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                enabled: cam.capAudio
                Text { text: "Level & processing"; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                Text {
                    text: "These apply to whichever input is selected above. Each change is sent on its own and "
                        + "read straight back from the camera — a command returning \"ok\" is not proof it took."
                    color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                AudioSlider {
                    Layout.fillWidth: true
                    label: "Gain"
                    boundValue: cam.audioVolume
                    enabled: cam.connected && cam.capAudio && cam.audioVolume >= 0
                    onApplied: (v) => cam.setAudioVolume(v)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ToggleChip {
                        Layout.fillWidth: true
                        text: "Mute"; tone: Theme.offline
                        enabled: cam.connected && cam.capAudio && cam.audioMuted >= 0
                        checked: cam.audioMuted === 1
                        onToggled: (c) => cam.setAudioMute(c)
                    }
                    ToggleChip {
                        Layout.fillWidth: true
                        text: "Noise reduction"; tone: Theme.live
                        enabled: cam.connected && cam.capAudio && cam.audioNoiseReduce >= 0
                        checked: cam.audioNoiseReduce === 1
                        onToggled: (c) => cam.setAudioNoiseReduce(c)
                    }
                    ToggleChip {
                        Layout.fillWidth: true
                        text: "Automatic gain"; tone: Theme.live
                        enabled: cam.connected && cam.capAudio && cam.audioAgc >= 0
                        checked: cam.audioAgc === 1
                        onToggled: (c) => cam.setAudioAgc(c)
                    }
                }
                // Strength only means anything while noise reduction is on, so the
                // stepper follows it rather than sitting there looking live.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    enabled: cam.connected && cam.capAudio && cam.audioNoiseReduce === 1
                    Text {
                        text: "NR strength"
                        color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12
                        Layout.preferredWidth: 96
                    }
                    Stepper {
                        valueText: cam.audioNoiseLevel < 0 ? "—" : (cam.audioNoiseLevel + " / 10")
                        onDecrement: cam.setAudioNoiseLevel(cam.audioNoiseLevel - 1)
                        onIncrement: cam.setAudioNoiseLevel(cam.audioNoiseLevel + 1)
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        // Input level.
        //
        // Its own panel rather than a row inside "Level & processing" above,
        // because that whole column is disabled when the camera-audio API does
        // not answer (cam.capAudio) — and this meter does not use that API, or
        // any SDK call. It reads the camera's USB audio input through Qt
        // Multimedia and works whether or not the control channel does.
        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: meterCol.implicitHeight + 24
            ColumnLayout {
                id: meterCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                Text { text: "Input level"; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                Text {
                    // Deliberately NOT called "wireless mic level": the camera
                    // sends one audio stream, and which microphone is behind it
                    // depends on the input selected above.
                    text: "Whatever the camera is sending right now — its built-in array or a wireless "
                        + "mic, whichever input is selected above — measured after the camera's own gain, "
                        + "mute and noise processing. This reads the camera's USB audio device directly, "
                        + "not the control API, and it is only open while this page is showing."
                    color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }

                // No input device at all: say so, rather than showing a bar that
                // sits at zero and looks like silence.
                Text {
                    visible: !audioMeter.available
                    Layout.fillWidth: true
                    text: "Level meter unavailable — " + audioMeter.unavailableReason
                    color: Theme.degraded; font.family: Theme.mono; font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    visible: audioMeter.available
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: "Level"
                        color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13
                        Layout.preferredWidth: 96
                    }
                    Rectangle {
                        id: meterTrack
                        Layout.fillWidth: true
                        Layout.preferredHeight: 16
                        radius: 4
                        color: Qt.rgba(1, 1, 1, 0.06)
                        border.width: 1
                        border.color: Theme.border
                        clip: true

                        Rectangle {
                            width: meterTrack.width * audioMeter.level
                            height: parent.height
                            radius: 4
                            // Green while there is headroom, amber as it closes in
                            // on full scale, rose once a sample actually clipped.
                            color: audioMeter.clipping ? Theme.offline
                                 : (audioMeter.level > 0.9 ? Theme.degraded : Theme.live)
                            Behavior on width { NumberAnimation { duration: 40 } }
                        }
                        // Peak hold — rises instantly, falls slowly, never above
                        // what was measured.
                        Rectangle {
                            visible: audioMeter.peak > 0.001
                            x: Math.max(0, Math.min(meterTrack.width - 2, meterTrack.width * audioMeter.peak - 1))
                            width: 2
                            height: parent.height
                            color: Theme.fg
                            opacity: 0.65
                        }
                        // Resting state: no capture running (the page was just
                        // opened, or the device could not be opened).
                        Text {
                            anchors.centerIn: parent
                            visible: !audioMeter.active
                            text: "not listening"
                            color: Theme.dimmer; font.family: Theme.mono; font.pixelSize: 11
                        }
                    }
                }
                KeyValue {
                    visible: audioMeter.available
                    Layout.fillWidth: true
                    keyWidth: 118
                    key: "listening to"
                    value: audioMeter.deviceName
                         + (audioMeter.usingFallback ? "  — system default, NOT the camera" : "")
                    unknown: !audioMeter.active
                    valueColor: audioMeter.usingFallback ? Theme.degraded : Theme.accentSoft
                }
            }
        }

        SectionLabel { text: "Button action" }

        // Honest capability gate: the button assignment rides an SDK entry point
        // that libdev exports but the public header omits, so it is probed on
        // connect and disabled when the camera does not answer (mirrors the
        // TrackingPage advanced-controls gating).
        Rectangle {
            visible: !cam.capMicButton
            Layout.fillWidth: true
            radius: Theme.rControl
            color: Qt.rgba(Theme.degraded.r, Theme.degraded.g, Theme.degraded.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.degraded.r, Theme.degraded.g, Theme.degraded.b, 0.4)
            implicitHeight: micBanner.implicitHeight + 20
            Text {
                id: micBanner
                anchors.fill: parent; anchors.margins: 10
                text: cam.connected
                    ? "The mic-control API is unavailable on this camera/firmware — battery and button assignment are disabled."
                    : "Connect the camera to read the wireless-mic controls."
                color: Theme.degraded
                font.family: Theme.mono; font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        // What the mic's multi-function button triggers on the camera.
        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: btnCol.implicitHeight + 24
            ColumnLayout {
                id: btnCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                enabled: cam.capMicButton
                Text { text: "Multi-function button"; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13 }
                Text {
                    text: "What a click on a Vox SE's button does on the camera. Applies to connected mics; the "
                        + "camera remembers the assignment."
                    color: Theme.dimmer; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                // Short labels so each segment fits Segmented's fixed cells; the
                // full action name is spelled out in the rows below.
                // These four are the ONLY actions the camera offers — the SDK's
                // DevTWSKeyType has no others and takes no parameter, so there is
                // no zoom level to choose, and no FOV or preset action to add.
                // The press is handled inside the camera; the app is not involved.
                // Wider cells so "Track mode" is not clipped.
                Segmented {
                    Layout.alignment: Qt.AlignLeft
                    Layout.preferredWidth: 4 * 100
                    options: ["Track", "Track mode", "Zoom 1×", "Record"]
                    currentIndex: cam.micButtonAction
                    enabled: cam.connected && cam.capMicButton
                    onActivated: (i) => cam.setMicButtonAction(i)
                }
                // Not redundant with the selector: while a change is in flight the
                // selector shows the pick and this shows what the camera actually
                // reports, so they differ exactly when that matters.
                KeyValue {
                    Layout.fillWidth: true
                    keyWidth: 118
                    key: "camera reports"
                    value: cam.micButtonActionName
                    unknown: cam.micButtonActionName === "—"
                }
                // "Record" means the camera asks the HOST to start recording
                // (the SDK calls it "pc record"); that is OBSBOT Center's job on
                // Windows/macOS. Nothing on Linux answers it, this app included.
                Text {
                    visible: cam.micButtonAction === 3
                    text: "Record asks the computer to start recording — a job for OBSBOT Center, which "
                        + "has no Linux version. Nothing here answers it, so this press will most likely "
                        + "do nothing on this machine."
                    color: Theme.degraded; font.family: Theme.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
            }
        }
    }
}
