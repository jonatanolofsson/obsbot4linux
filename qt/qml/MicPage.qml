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
                    KeyValue {
                        Layout.fillWidth: true
                        key: "status"
                        value: !cam.connected ? "—" : (txCard.modelData.online ? "connected" : "not connected")
                        unknown: !txCard.live
                        valueColor: Theme.accentSoft
                    }
                    KeyValue {
                        Layout.fillWidth: true
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
                        key: "microphone"
                        value: !(txCard.live && cam.capMicButton) ? "—"
                             : (txCard.modelData.muted ? "muted" : "live")
                        unknown: !(txCard.live && cam.capMicButton)
                        valueColor: txCard.modelData.muted ? Theme.degraded : Theme.accentSoft
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        spacing: 8
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
                Segmented {
                    Layout.alignment: Qt.AlignLeft
                    options: ["Omni", "Stereo", "Front", "Back", "Both", "Music"]
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
                Segmented {
                    Layout.alignment: Qt.AlignLeft
                    options: ["Track", "Mode", "Zoom", "Record"]
                    currentIndex: cam.micButtonAction
                    enabled: cam.connected && cam.capMicButton
                    onActivated: (i) => cam.setMicButtonAction(i)
                }
                // The camera owns this setting, so there is only one truth to
                // show: what the device reports back.
                KeyValue {
                    Layout.fillWidth: true
                    keyWidth: 118
                    key: "assigned"
                    value: cam.micButtonActionName
                    unknown: cam.micButtonActionName === "—"
                }
            }
        }
    }
}
