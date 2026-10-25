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
                    text: qsTr("During HDR streams, only while fullscreen and in front")
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
                          qsTr("\"Only while fullscreen and in front\" also switches back 2 seconds after the stream leaves fullscreen or the front (overview, another workspace or window), and to HDR 2 seconds after it returns.")
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
    }
}
