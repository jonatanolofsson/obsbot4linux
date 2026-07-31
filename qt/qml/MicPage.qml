// Wireless Mic — OBSBOT Vox SE pairing and status on the Tiny 3.
//
// The Vox SE's receiver IS the camera. Pairing is a two-sided handshake: the mic
// goes into pairing mode from its own button (hold ~6 s until the light flashes
// green) while the camera holds a transmitter slot open — that's what Pair does
// here. The camera must be AWAKE for it: asleep it accepts the command and never
// turns its radio on, so the app wakes it first.
//
// Presence comes from what the camera reports (CameraStatus.tiny.wireless_mic),
// never from the pair command's return code. Battery/mute and the button
// assignment come from the extended mic API, which is probed on connect and
// shown as unavailable if this camera/firmware does not answer.
// Audio itself needs nothing here — it arrives on the camera's USB-audio input.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Obsbot

Item {
    id: root

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
