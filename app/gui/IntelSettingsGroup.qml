import QtQuick 2.9
import QtQuick.Controls 2.2

import IntelPreferences 1.0

// moonlight-intel: every fork option lives in this group. Values are saved as they change.
GroupBox {
    id: intelSettingsGroupBox
    width: (parent.width - (parent.leftPadding + parent.rightPadding))
    padding: 12
    title: "<font color=\"skyblue\">moonlight-intel</font>"
    font.pointSize: 12

    Column {
        anchors.fill: parent
        spacing: 5

        Label {
            width: parent.width
            id: desktopHdrTitle
            text: qsTr("Switch desktop to HDR during HDR streams")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        AutoResizingComboBox {
            // ignore setting the index at first, and actually set it when the component is loaded
            Component.onCompleted: {
                var savedMode = IntelPreferences.desktopHdrMode
                currentIndex = 0
                for (var i = 0; i < desktopHdrListModel.count; i++) {
                    if (savedMode === desktopHdrListModel.get(i).val) {
                        currentIndex = i
                        break
                    }
                }
                activated(currentIndex)
            }

            id: desktopHdrComboBox
            enabled: IntelPreferences.desktopHdrAvailable
            hoverEnabled: true
            textRole: "text"
            model: ListModel {
                id: desktopHdrListModel
                ListElement {
                    text: qsTr("Off")
                    val: IntelPreferences.DHM_OFF
                }
                ListElement {
                    text: qsTr("During HDR streams")
                    val: IntelPreferences.DHM_HDR_STREAMS
                }
                ListElement {
                    text: qsTr("During HDR streams (fullscreen focused)")
                    val: IntelPreferences.DHM_HDR_STREAMS_FULLSCREEN
                }
            }
            // ::onActivated must be used, as it only listens for when the index is changed by a human
            onActivated: {
                if (enabled) {
                    IntelPreferences.desktopHdrMode = desktopHdrListModel.get(currentIndex).val
                }
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("GNOME only. The display switches to HDR when the host starts sending HDR, and back to its previous mode (e.g. SDR Native) when the stream ends. Each switch blanks the screen briefly.") + "\n\n" +
                          qsTr("\"Fullscreen focused\" also switches back 2 seconds after the stream leaves fullscreen or loses focus (another workspace or window; the overview first has to close), and to HDR 2 seconds after it returns.")
        }

        Label {
            width: parent.width
            id: desktopHdrUnavailableLabel
            visible: !IntelPreferences.desktopHdrAvailable && IntelPreferences.desktopHdrUnavailableReason !== ""
            text: IntelPreferences.desktopHdrUnavailableReason
            font.pointSize: 9
            wrapMode: Text.Wrap
        }

        CheckBox {
            id: logGpuTimingCheck
            width: parent.width
            hoverEnabled: true
            text: qsTr("Log GPU render timing")
            font.pointSize: 12
            checked: IntelPreferences.logGpuTiming
            onCheckedChanged: {
                IntelPreferences.logGpuTiming = checked
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Writes libplacebo GPU timing to the log every 5 seconds. The stats overlay always shows it.")
        }

        CheckBox {
            id: vulkanForSdrCheck
            width: parent.width
            hoverEnabled: true
            text: qsTr("Use Vulkan (libplacebo) renderer for SDR streams")
            font.pointSize: 12
            checked: IntelPreferences.vulkanForSdr
            onCheckedChanged: {
                IntelPreferences.vulkanForSdr = checked
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Renders SDR streams through libplacebo, like HDR streams, so the moonlight-intel overlay readouts and upscaler apply. Off = upstream behaviour (EGL). Takes effect at the next stream.")
        }

        Label {
            width: parent.width
            id: upscalerTitle
            text: qsTr("Upscaler")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        AutoResizingComboBox {
            // ignore setting the index at first, and actually set it when the component is loaded
            Component.onCompleted: {
                var savedUpscaler = IntelPreferences.upscaler
                currentIndex = 0
                for (var i = 0; i < upscalerListModel.count; i++) {
                    if (savedUpscaler === upscalerListModel.get(i).val) {
                        currentIndex = i
                        break
                    }
                }
                activated(currentIndex)
            }

            id: upscalerComboBox
            hoverEnabled: true
            textRole: "text"
            model: ListModel {
                id: upscalerListModel
                ListElement {
                    text: qsTr("Bilinear (off)")
                    val: IntelPreferences.SC_BILINEAR
                }
                ListElement {
                    text: qsTr("Spline36")
                    val: IntelPreferences.SC_SPLINE36
                }
                ListElement {
                    text: qsTr("Lanczos")
                    val: IntelPreferences.SC_LANCZOS
                }
                ListElement {
                    text: qsTr("EWA Lanczos Sharp")
                    val: IntelPreferences.SC_EWA_LANCZOSSHARP
                }
                ListElement {
                    text: qsTr("FSR 1 (EASU + RCAS)")
                    val: IntelPreferences.SC_FSR1
                }
                ListElement {
                    text: qsTr("NIS (NVIDIA Image Scaling)")
                    val: IntelPreferences.SC_NIS
                }
                ListElement {
                    text: qsTr("RAVU-Zoom-AR r2")
                    val: IntelPreferences.SC_RAVU_ZOOM_AR_R2
                }
            }
            // ::onActivated must be used, as it only listens for when the index is changed by a human
            onActivated: {
                IntelPreferences.upscaler = upscalerListModel.get(currentIndex).val
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("How the stream is scaled up to the window. Applies only with the Vulkan (libplacebo) renderer: HDR streams, or SDR streams with \"Use Vulkan (libplacebo) renderer for SDR streams\". EWA Lanczos Sharp is the most expensive.")
        }

        Label {
            width: parent.width
            id: fsrSharpnessTitle
            visible: IntelPreferences.upscaler === IntelPreferences.SC_FSR1
            text: qsTr("FSR 1 sharpness: %1").arg(fsrSharpnessSlider.value.toFixed(2))
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        Slider {
            id: fsrSharpnessSlider
            visible: fsrSharpnessTitle.visible
            width: parent.width
            from: 0
            to: 1
            stepSize: 0.05
            snapMode: "SnapAlways"
            value: IntelPreferences.fsrSharpness
            onMoved: {
                IntelPreferences.fsrSharpness = value
            }
        }

        Label {
            width: parent.width
            id: nisSharpnessTitle
            visible: IntelPreferences.upscaler === IntelPreferences.SC_NIS
            text: qsTr("NIS sharpness: %1").arg(nisSharpnessSlider.value.toFixed(2))
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        Slider {
            id: nisSharpnessSlider
            visible: nisSharpnessTitle.visible
            width: parent.width
            from: 0
            to: 1
            stepSize: 0.05
            snapMode: "SnapAlways"
            value: IntelPreferences.nisSharpness
            onMoved: {
                IntelPreferences.nisSharpness = value
            }
        }

        CheckBox {
            id: debandingCheck
            width: parent.width
            hoverEnabled: true
            text: qsTr("Debanding (libplacebo)")
            font.pointSize: 12
            checked: IntelPreferences.debanding
            onCheckedChanged: {
                IntelPreferences.debanding = checked
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Smooths banding in dark gradients (sky, fog) before upscaling, at some GPU cost. Vulkan (libplacebo) renderer only.")
        }

        Label {
            width: parent.width
            id: upscalerHotkeysLabel
            text: qsTr("In-stream hotkeys, with Ctrl+Alt+Shift: U / Y next / previous upscaler, B A/B against bilinear, [ / ] softer / sharper, T benchmark sweep. Hotkey changes last for the current stream only.")
            font.pointSize: 9
            wrapMode: Text.Wrap
        }
    }
}
