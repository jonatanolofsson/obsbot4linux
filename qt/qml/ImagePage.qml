// Image & Exposure.
//   * Brightness / Contrast / Saturation / Sharpness — WIRED (SDK 0–100, live
//     values read on connect, applied on release).
//   * HDR — NOT available on Tiny 3 (SDK HDR/WDR is for tiny4k/tiny2/meet/tail-
//     air; Tiny 3 reports hdr_support=0 in every mode). Shown as an honest note.
//   * White balance — WIRED (auto/manual + Kelvin, range read from the device).
//     In auto the camera does NOT report the temperature it is using, so the UI
//     says so instead of showing the stale number it does report.
//   * Exposure — still capability-gated OFF (not verified on the Tiny 3 SDK),
//     shown disabled with an honest hint.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Obsbot

RowLayout {
    id: root
    spacing: Theme.s4

    // A live, coral-styled slider. Applies to the device on release (few SDK
    // calls), and reflects the device's current value via `boundValue`.
    component ImageSlider: RowLayout {
        id: row
        property string label: ""
        // When set, the value is applied with setImageParam(param, v). Leave it
        // empty and handle `applied` instead for a control with its own setter
        // — white balance does that, so it gets this exact look rather than a
        // second slider style drawn by hand.
        property string param: ""
        property int boundValue: 50
        // Range defaults to the 0–100 the image params use; white balance
        // overrides them with the range the DEVICE reported.
        property int fromValue: 0
        property int toValue: 100
        property int stepValue: 1
        property string suffix: ""      // appended in the value pill, e.g. " K"
        // When set, REPLACES the number in the pill. For a control whose value
        // is not meaningful in the current mode — white balance in auto, where
        // the camera reports a stored setting rather than what it is using.
        property string valueText: ""
        property int pillWidth: 40
        signal applied(int value)

        // Keeping the handle on the device's value takes an explicit sync, not a
        // `value: boundValue` binding. Two reasons, both of which produced real
        // bugs here:
        //
        //   * Dragging a Slider assigns `value` imperatively and DESTROYS any
        //     binding on it, so after one drag the handle stops following the
        //     device permanently.
        //   * A Slider clamps `value` to whatever from/to are at that moment.
        //     The white-balance range arrives from the camera AFTER this page is
        //     built, so the first reading was clamped into a 0..0 range and the
        //     slider opened pinned at 2000 K while the camera was at 6000 K —
        //     and never recovered, because boundValue never changed again.
        //     Hence syncing on the RANGE changing too, not just the value.
        //
        // Syncing on CHANGE rather than binding continuously also avoids a bounce
        // on release: the write goes out, the camera confirms ~250 ms later, and
        // only then does boundValue move — to exactly where the handle already
        // is. A continuous binding would instead snap back to the stale value for
        // that quarter second and then jump forward again.
        function syncFromDevice() {
            if (sl.pressed) return              // never fight a drag in progress
            if (row.toValue <= row.fromValue) return   // range not known yet
            sl.value = Math.min(Math.max(row.boundValue, row.fromValue), row.toValue)
        }
        onBoundValueChanged: syncFromDevice()
        onFromValueChanged: syncFromDevice()
        onToValueChanged: syncFromDevice()
        Component.onCompleted: syncFromDevice()

        spacing: 10
        Text { text: row.label; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 13; Layout.preferredWidth: 96 }
        Slider {
            id: sl
            Layout.fillWidth: true
            // `row.enabled` so a caller can gate the slider (WB does, while the
            // camera is in auto) without losing the connected() requirement.
            enabled: cam.connected && row.enabled
            from: row.fromValue; to: row.toValue; stepSize: row.stepValue
            onPressedChanged: {
                if (pressed) return
                const v = Math.round(value)
                if (row.param !== "") cam.setImageParam(row.param, v)
                else row.applied(v)
            }
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
        // Value pill — prominent, coral when dragging, tabular so it doesn't jitter.
        Rectangle {
            Layout.preferredWidth: row.pillWidth; Layout.preferredHeight: 24
            radius: Theme.rControl
            color: sl.pressed ? Theme.accentTint : Qt.rgba(1, 1, 1, 0.04)
            border.width: 1; border.color: sl.pressed ? Theme.accentRing : Theme.border
            Text {
                anchors.centerIn: parent
                text: row.valueText !== "" ? row.valueText
                                             : Math.round(sl.value) + row.suffix
                color: sl.pressed ? Theme.accentSoft : Theme.fg
                font.family: Theme.mono; font.pixelSize: 13
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: 560
        Layout.alignment: Qt.AlignTop
        spacing: Theme.s3

        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: col.implicitHeight + 28
            ColumnLayout {
                id: col
                anchors.fill: parent
                anchors.margins: 14
                spacing: Theme.s3
                RowLayout {
                    Layout.fillWidth: true
                    SectionLabel { text: "Picture" }
                    Item { Layout.fillWidth: true }
                    ActionButton {
                        text: "Reset defaults"; variant: "secondary"
                        enabled: cam.connected
                        onClicked: cam.resetImageDefaults()
                    }
                }
                ImageSlider { label: "Brightness"; param: "brightness"; boundValue: cam.brightness }
                ImageSlider { label: "Contrast";   param: "contrast";   boundValue: cam.contrast }
                ImageSlider { label: "Saturation"; param: "saturation"; boundValue: cam.saturation }
                ImageSlider { label: "Sharpness";  param: "sharpness";  boundValue: cam.sharpness }

                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                // HDR — the SDK's HDR/WDR control is documented for tiny4k/tiny2/
                // meet/tail-air, NOT tiny3, and the Tiny 3 reports hdr_support=0
                // in every mode (incl. 1080p30). So HDR is not exposed on Tiny 3
                // via this SDK; shown as an honest note rather than a dead toggle.
                // (Tip: 1080p30 already looks crisper than 1080p60 simply because
                //  30fps compresses less — that's framerate, not HDR.)
                RowLayout {
                    spacing: 10
                    Text { text: "HDR"; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12; Layout.preferredWidth: 96 }
                    Text {
                        text: "not available on the Tiny 3 (not exposed by the SDK)"
                        color: Theme.dimmer; font.family: Theme.mono; font.pixelSize: 11
                        Layout.fillWidth: true; wrapMode: Text.WordWrap
                    }
                }
            }
        }

        // White balance — LIVE. The camera answers cameraGetRangeWhiteBalanceR,
        // so the range below is the device's own (2000–10000 K step 100 on a
        // Tiny 3), never a constant in this file. If it ever stops answering,
        // capWhiteBalance goes false and the whole panel disappears rather than
        // offering a slider bound to invented limits.
        GlassPanel {
            Layout.fillWidth: true
            visible: cam.capWhiteBalance
            implicitHeight: wbcol.implicitHeight + 28
            ColumnLayout {
                id: wbcol
                anchors.fill: parent
                anchors.margins: 14
                spacing: Theme.s3
                enabled: cam.connected
                RowLayout {
                    spacing: 10
                    Text {
                        text: "White balance"; color: Theme.dim
                        font.family: Theme.mono; font.pixelSize: 12; Layout.preferredWidth: 96
                    }
                    Segmented {
                        options: ["Auto", "Manual"]
                        // Bound to the DEVICE's mode, so an externally-made
                        // change (v4l2, another app) shows up here instead of
                        // the UI insisting on its own last click.
                        currentIndex: cam.wbAuto ? 0 : 1
                        onActivated: (i) => cam.setWhiteBalanceAuto(i === 0)
                    }
                }
                // Same slider as brightness/contrast above — deliberately, so the
                // page reads as one control surface.
                //
                // In AUTO the pill says "auto" instead of a number. The camera
                // does report a Kelvin value in that mode, but it is a stored
                // setting rather than what auto is using: writing manual 2500
                // then switching to auto makes it report 2500, writing 9500 makes
                // it report 9500, and the actual picture is the same either way
                // (measured off captured frames). Showing it would be inventing a
                // reading. Nothing in the SDK or over UVC exposes the real one.
                ImageSlider {
                    Layout.fillWidth: true
                    label: "Temp"
                    enabled: !cam.wbAuto
                    boundValue: cam.wbKelvin
                    fromValue: cam.wbMin
                    toValue: cam.wbMax
                    stepValue: cam.wbStep
                    suffix: " K"
                    valueText: cam.wbAuto ? "auto" : ""
                    pillWidth: 62
                    onApplied: (v) => cam.setWhiteBalanceKelvin(v)
                }
                Text {
                    text: cam.wbAuto
                          ? "The camera is choosing the white balance. It does not report which "
                            + "temperature it settled on, so the slider shows the last manual "
                            + "setting, not what you are seeing — switch to Manual to control it."
                          : "Lower is bluer, higher is warmer — it is the colour of the light you "
                            + "are telling the camera to expect, not the tint it applies. "
                            + cam.wbMin + "–" + cam.wbMax + " K."
                    color: Theme.dimmer; font.family: Theme.mono; font.pixelSize: 11
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                }
            }
        }

    }

    // reference preview — fixed size, roomy enough to judge image tweaks by
    // (300x220 was too tiny; fill-the-page swallowed the controls — don't).
    GlassPanel {
        Layout.preferredWidth: 460
        Layout.maximumWidth: 480
        Layout.preferredHeight: 310
        Layout.alignment: Qt.AlignTop
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8
            SectionLabel { text: "Reference" }
            Viewfinder { Layout.fillWidth: true; Layout.fillHeight: true }
        }
    }
}
