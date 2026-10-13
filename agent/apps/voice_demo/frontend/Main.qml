import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    visible: true
    width: 1440; height: 900
    minimumWidth: 720; minimumHeight: 580
    title: "CabinFlow · 座舱语音交互 Demo"
    color: "#0d1720"
    onClosing: function(event) { event.accepted = false; voiceClient.requestClose(); }

    component ActionButton: Button {
        id: actionButton
        font.pixelSize: 13
        implicitHeight: 34
        contentItem: Text {
            text: actionButton.text
            color: actionButton.enabled ? "#dce9ee" : "#687b89"
            font: actionButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 7
            color: actionButton.down ? "#365561" : (actionButton.hovered ? "#304654" : "#263946")
            border.color: actionButton.enabled ? "#405968" : "#293d49"
        }
    }

    FileDialog {
        id: wavPicker
        title: "选择 16 kHz mono PCM16 WAV"
        nameFilters: ["WAV audio (*.wav)"]
        onAccepted: if (!voiceClient.busy && !musicController.busy) voiceClient.startWav(selectedFile)
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: window.width < 850 ? 14 : 22
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 3
                Label { text: "CABINFLOW"; color: "#98dfce"; font.pixelSize: 25; font.bold: true; font.letterSpacing: 2 }
                Label { text: "本地语音 · 座舱交互 Demo"; color: "#8ca2b1"; font.pixelSize: 12 }
            }
            Item { Layout.fillWidth: true }
            ColumnLayout {
                spacing: 3
                Label { Layout.alignment: Qt.AlignRight; text: voiceClient.status; color: voiceClient.busy ? "#edc17d" : "#98dfce"; font.pixelSize: 16 }
                Label { Layout.alignment: Qt.AlignRight; text: voiceClient.connected ? "gRPC 后端已连接" : "gRPC 后端未连接"; color: "#8ca2b1"; font.pixelSize: 11 }
            }
            BusyIndicator { running: voiceClient.busy; visible: running; implicitWidth: 30; implicitHeight: 30 }
        }

        ScrollView {
            id: bodyScroll
            objectName: "cockpitBodyScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            GridLayout {
                width: bodyScroll.availableWidth
                columns: window.width < 1100 ? 1 : 2
                columnSpacing: 12
                rowSpacing: 12
                CockpitScene {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(690, bodyScroll.availableHeight)
                    Layout.preferredWidth: window.width * 0.62
                    Layout.minimumWidth: 285
                    stateKnown: voiceClient.vehicleKnown
                    climateOn: voiceClient.climateOn
                    leftFrontWindowOpen: voiceClient.leftFrontWindowOpen
                    pending: voiceClient.busy
                }
                Rectangle {
                    id: interaction
                    color: "#15222d"
                    border.color: "#283b49"
                    radius: 18
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(620, bodyScroll.availableHeight)
                    Layout.preferredWidth: window.width * 0.35
                    Layout.minimumWidth: 300

                    ScrollView {
                        id: interactionScroll
                        objectName: "interactionScroll"
                        anchors.fill: parent
                        anchors.margins: 16
                        contentWidth: availableWidth
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ColumnLayout {
                            width: interactionScroll.availableWidth
                            spacing: 9
                            Label { text: "语音与文本"; color: "#edf4f6"; font.pixelSize: 19; font.bold: true }
                            Label { text: "开启麦克风后先说“你好，小舱”，看到提示再说指令"; color: "#91a8b7"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Switch {
                                id: wakeSwitch
                                objectName: "wakeSwitch"
                                text: "本地唤醒（待真人验收）· 麦克风"
                                checked: voiceClient.wakeEnabled
                                onToggled: voiceClient.setWakeEnabled(checked)
                                font.pixelSize: 12
                                indicator: Rectangle {
                                    implicitWidth: 38; implicitHeight: 20; radius: 10
                                    y: (wakeSwitch.height - height) / 2
                                    color: wakeSwitch.checked ? "#79cdbb" : "#405968"
                                    Rectangle { width: 14; height: 14; radius: 7; y: 3; x: wakeSwitch.checked ? 21 : 3; color: "#e8f0f4" }
                                }
                                contentItem: Text {
                                    text: wakeSwitch.text; font: wakeSwitch.font; color: "#c4d4de"
                                    leftPadding: wakeSwitch.indicator.width + wakeSwitch.spacing
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                            Label { objectName: "wakeStatus"; text: voiceClient.wakeStatus; color: "#98dfce"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label { text: voiceClient.wakeError; visible: text.length > 0; color: "#ff9d91"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label { text: "5 秒等待开口 · 静音 0.8 秒结束 · 最长 15 秒\n处理和回答播放期间暂停监听；文本 / WAV 为调试输入"; color: "#91a8b7"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            TextField {
                                id: textInput
                                Layout.fillWidth: true
                                implicitHeight: 40
                                placeholderText: "例如：打开左前车窗"
                                color: "#e6eff3"
                                placeholderTextColor: "#748f9f"
                                font.pixelSize: 14
                                selectByMouse: true
                                enabled: !voiceClient.busy && !musicController.busy
                                background: Rectangle { color: "#0f1c26"; radius: 8; border.color: textInput.activeFocus ? "#79cdbb" : "#3b5362" }
                                onAccepted: if (voiceClient.connected && !voiceClient.busy && !musicController.busy && text.length > 0) voiceClient.startText(text)
                            }
                            Label {
                                text: "打开空调 / 关闭空调\n打开左前车窗 / 关闭左前车窗\n搜索周杰伦 / 播放第一首 / 暂停音乐\n播放音乐 / 上一首 / 下一首"
                                color: "#8fb8b8"
                                font.pixelSize: 11
                                lineHeight: 1.45
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton { Layout.fillWidth: true; text: "开始问答"; enabled: voiceClient.connected && !voiceClient.busy && !musicController.busy && textInput.text.length > 0; onClicked: voiceClient.startText(textInput.text) }
                                ActionButton { Layout.fillWidth: true; text: "选择 WAV"; enabled: voiceClient.connected && !voiceClient.busy && !musicController.busy; onClicked: wavPicker.open() }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ActionButton { Layout.fillWidth: true; text: "取消任务"; enabled: voiceClient.busy; onClicked: voiceClient.cancel() }
                                ActionButton { Layout.fillWidth: true; text: "连接后端"; enabled: !voiceClient.connected && !voiceClient.busy; onClicked: voiceClient.connectBackend() }
                            }
                            ActionButton {
                                objectName: "refreshVehicleState"
                                Layout.fillWidth: true
                                text: "刷新车辆状态（虚拟 ECU）"
                                enabled: voiceClient.connected && !voiceClient.busy && !musicController.busy
                                onClicked: voiceClient.refreshVehicleState()
                            }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#30434f"; Layout.topMargin: 3; Layout.bottomMargin: 3 }
                            Label { text: "回答"; color: "#c4d4de"; font.pixelSize: 14; font.bold: true }
                            Label { text: "转写：" + (voiceClient.transcript || "等待输入"); color: "#91a8b7"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(82, interaction.height - 590)
                                clip: true
                                TextArea {
                                    text: voiceClient.answer
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: Text.Wrap
                                    color: "#e8f0f4"
                                    font.pixelSize: 16
                                    background: Rectangle { color: "#101e28"; radius: 8 }
                                    placeholderText: "任务完成后显示回答"
                                    placeholderTextColor: "#748d9c"
                                }
                            }
                            Label { text: voiceClient.error; visible: text.length > 0; color: "#ff9d91"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            RowLayout {
                                objectName: "playbackControls"
                                Layout.fillWidth: true
                                ActionButton { Layout.fillWidth: true; text: "播放回答"; enabled: voiceClient.hasAudio && !voiceClient.busy && !musicController.busy; onClicked: voiceClient.play() }
                                ActionButton { Layout.fillWidth: true; text: "停止播放"; enabled: voiceClient.playbackActive; onClicked: voiceClient.stopPlayback() }
                            }
                            Label { text: voiceClient.playing ? "正在播放回答" : ""; visible: voiceClient.playing; color: "#98dfce"; font.pixelSize: 12 }
                            Label { text: voiceClient.vehicleAction; color: "#8ca2b1"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        }
                    }
                }
            }
        }
        Label { text: "x86 / WSL · 本地推理 · 单任务 · 取消中等待后端实际清理"; color: "#728b9b"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
    }
}
