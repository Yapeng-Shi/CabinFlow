import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "cockpitScene"
    property bool stateKnown: false
    property bool climateOn: false
    property bool leftFrontWindowOpen: false
    property bool pending: false
    readonly property real glassLevel: 1 - windowGlass.lowered
    readonly property bool glassVisible: stateKnown
    readonly property bool breezeVisible: stateKnown && climateOn
    readonly property bool musicAvailable: musicController.enabled && !musicController.busy && !voiceClient.busy
    implicitHeight: 730
    color: "#111a23"
    radius: 18
    border.color: "#2b3b48"
    clip: true

    component MusicButton: Button {
        id: control
        implicitHeight: 36
        font.pixelSize: 12
        contentItem: Text {
            text: control.text
            font: control.font
            color: control.enabled ? "#edf5f7" : "#6b7b8c"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 7
            color: control.down ? "#3a5b69" : (control.hovered ? "#284652" : "#223441")
            border.color: control.enabled ? "#4b6575" : "#2b3b48"
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 3
                Text { text: "前排全景"; color: "#eef4f7"; font.pixelSize: 19; font.bold: true }
                Text { text: "CABIN · MUSIC · VOICE"; color: "#8ea3b3"; font.pixelSize: 10; font.letterSpacing: 2 }
            }
            Item { Layout.fillWidth: true }
            Text { text: "模拟座舱"; color: "#9fd6c9"; font.pixelSize: 12 }
        }

        Item {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 475

            Canvas {
                id: cabin
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                function polygon(ctx, points, fill, stroke, lineWidth) {
                    ctx.beginPath()
                    ctx.moveTo(points[0][0], points[0][1])
                    for (var i = 1; i < points.length; ++i)
                        ctx.lineTo(points[i][0], points[i][1])
                    ctx.closePath()
                    ctx.fillStyle = fill
                    ctx.fill()
                    if (stroke) {
                        ctx.strokeStyle = stroke
                        ctx.lineWidth = lineWidth
                        ctx.stroke()
                    }
                }
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.clearRect(0, 0, width, height)
                    ctx.scale(width / 1000, height / 660)
                    var sky = ctx.createLinearGradient(0, 80, 0, 300)
                    sky.addColorStop(0, "#7795a5")
                    sky.addColorStop(0.65, "#ccd8d6")
                    sky.addColorStop(1, "#809b9e")
                    polygon(ctx, [[68,80],[932,80],[877,285],[120,285]], sky, "#435765", 3)
                    polygon(ctx, [[90,235],[230,199],[318,222],[439,199],[560,226],[687,203],[814,225],[901,203],[881,279],[117,279]], "#8da6a8", "", 0)
                    polygon(ctx, [[99,257],[905,257],[883,287],[116,287]], "#758e8e", "", 0)
                    polygon(ctx, [[0,128],[87,96],[111,281],[0,347]], "#879fa3", "#415564", 3)
                    polygon(ctx, [[912,97],[1000,128],[1000,347],[889,280]], "#879fa3", "#415564", 3)
                    polygon(ctx, [[0,0],[1000,0],[1000,110],[931,78],[70,78],[0,113]], "#111921", "#33424e", 2)
                    polygon(ctx, [[165,39],[399,39],[417,71],[170,68]], "#19232c", "#394651", 2)
                    polygon(ctx, [[599,39],[835,39],[829,68],[584,71]], "#19232c", "#394651", 2)
                    polygon(ctx, [[438,30],[563,30],[577,67],[422,67]], "#242f39", "#3e4e59", 2)
                    polygon(ctx, [[482,66],[519,66],[517,104],[484,104]], "#17222b", "", 0)
                    polygon(ctx, [[451,96],[550,96],[542,123],[459,123]], "#172630", "#4e606c", 2)
                    polygon(ctx, [[0,65],[66,79],[139,283],[101,302],[51,159]], "#25323c", "#526473", 2)
                    polygon(ctx, [[935,78],[1000,66],[949,163],[899,300],[861,282]], "#25323c", "#526473", 2)
                    var dash = ctx.createLinearGradient(0, 272, 0, 432)
                    dash.addColorStop(0, "#4c5e68")
                    dash.addColorStop(0.32, "#2b3b47")
                    dash.addColorStop(0.34, "#17252f")
                    dash.addColorStop(0.72, "#273946")
                    dash.addColorStop(1, "#0d1821")
                    polygon(ctx, [[114,266],[884,266],[1000,354],[968,429],[32,429],[0,354]], dash, "#516371", 2)
                    ctx.beginPath()
                    ctx.moveTo(29,348)
                    ctx.bezierCurveTo(260,316,720,322,970,349)
                    ctx.strokeStyle = "#94b4bc"
                    ctx.lineWidth = 3
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(51,378)
                    ctx.bezierCurveTo(269,363,743,363,948,378)
                    ctx.strokeStyle = "#527e85"
                    ctx.lineWidth = 2
                    ctx.stroke()
                    polygon(ctx, [[133,291],[364,291],[377,352],[119,352]], "#0b151e", "#627985", 2)
                    for (var gauge = 0; gauge < 2; ++gauge) {
                        ctx.beginPath()
                        ctx.arc(199 + gauge * 92,329,30,Math.PI * 1.05,Math.PI * 1.95)
                        ctx.strokeStyle = "#87afba"
                        ctx.lineWidth = 4
                        ctx.stroke()
                    }
                    polygon(ctx, [[36,324],[97,309],[113,337],[44,356]], "#0f1b24", "#4c6573", 2)
                    polygon(ctx, [[765,301],[876,301],[925,336],[765,336]], "#0e1b24", "#4c6573", 2)
                    for (var slat = 0; slat < 4; ++slat) {
                        ctx.beginPath()
                        ctx.moveTo(772,309 + slat * 6)
                        ctx.lineTo(875 + slat * 7,309 + slat * 6)
                        ctx.strokeStyle = "#4b626f"
                        ctx.lineWidth = 2
                        ctx.stroke()
                    }
                    polygon(ctx, [[0,350],[118,398],[134,525],[0,602]], "#1c2b36", "#425664", 2)
                    polygon(ctx, [[1000,350],[889,398],[866,525],[1000,602]], "#1c2b36", "#425664", 2)
                    polygon(ctx, [[17,413],[75,419],[80,442],[12,438]], "#63828d", "#8da5ad", 2)
                    polygon(ctx, [[923,420],[983,413],[988,438],[922,442]], "#63828d", "#8da5ad", 2)
                    polygon(ctx, [[403,419],[681,419],[716,660],[346,660]], "#17242e", "#495b68", 2)
                    polygon(ctx, [[442,440],[641,440],[660,522],[423,522]], "#233640", "#546975", 2)
                    polygon(ctx, [[429,534],[656,534],[683,659],[398,659]], "#2c3c47", "#61737e", 2)
                    ctx.beginPath()
                    ctx.moveTo(447,553)
                    ctx.lineTo(640,553)
                    ctx.strokeStyle = "#6c7e87"
                    ctx.lineWidth = 2
                    ctx.stroke()
                    polygon(ctx, [[87,535],[293,535],[357,660],[11,660]], "#1e2d38", "#3d5160", 2)
                    polygon(ctx, [[735,535],[920,535],[990,660],[692,660]], "#1e2d38", "#3d5160", 2)
                    ctx.save()
                    ctx.translate(246,409)
                    ctx.scale(1,0.87)
                    ctx.beginPath()
                    ctx.arc(0,0,109,0,Math.PI * 2)
                    ctx.strokeStyle = "#0a1117"
                    ctx.lineWidth = 25
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.arc(0,0,111,0,Math.PI * 2)
                    ctx.strokeStyle = "#5a6d78"
                    ctx.lineWidth = 3
                    ctx.stroke()
                    polygon(ctx, [[-99,-15],[-35,-12],[35,-12],[99,-15],[90,17],[34,23],[17,100],[-17,100],[-34,23],[-90,17]], "#273a46", "#536b79", 2)
                    ctx.beginPath()
                    ctx.ellipse(-47,-35,94,72)
                    ctx.fillStyle = "#172732"
                    ctx.fill()
                    ctx.strokeStyle = "#6c838e"
                    ctx.lineWidth = 2
                    ctx.stroke()
                    ctx.restore()
                }
            }

            Canvas {
                id: windowGlass
                objectName: "leftFrontWindowGlass"
                anchors.fill: parent
                visible: root.glassVisible
                property real lowered: root.leftFrontWindowOpen ? 1 : 0
                // 玻璃与风效仅接受已确认回执；未知状态不推断为关闭。
                Behavior on lowered { enabled: root.stateKnown; NumberAnimation { duration: 650; easing.type: Easing.InOutCubic } }
                onLoweredChanged: requestPaint()
                onVisibleChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.clearRect(0,0,width,height)
                    ctx.scale(width / 1000,height / 660)
                    cabin.polygon(ctx, [[0,130],[83,100],[108,277],[0,338]], "rgba(0,0,0,0)", "", 0)
                    ctx.clip()
                    var top = 98 + lowered * 245
                    var glass = ctx.createLinearGradient(0,top,110,343)
                    glass.addColorStop(0,"rgba(175,224,235,0.66)")
                    glass.addColorStop(1,"rgba(175,224,235,0.17)")
                    ctx.fillStyle = glass
                    ctx.fillRect(0,top,115,250)
                    ctx.beginPath()
                    ctx.moveTo(0,top + 30)
                    ctx.lineTo(115,top)
                    ctx.strokeStyle = "#c9e7ec"
                    ctx.lineWidth = 2
                    ctx.stroke()
                }
            }

            Canvas {
                id: breeze
                objectName: "climateBreeze"
                anchors.fill: parent
                visible: root.breezeVisible
                property real phase: 0
                NumberAnimation { target: breeze; property: "phase"; from: 0; to: 1; duration: 1800; loops: Animation.Infinite; running: breeze.visible }
                onPhaseChanged: requestPaint()
                onVisibleChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.clearRect(0,0,width,height)
                    ctx.scale(width / 1000,height / 660)
                    for (var i = 0; i < 4; ++i) {
                        var offset = ((phase + i * 0.23) % 1) * 30
                        ctx.beginPath()
                        ctx.moveTo(788 + i * 27,303 - offset)
                        ctx.bezierCurveTo(769 + i * 27,280 - offset,804 + i * 27,258 - offset,788 + i * 27,237 - offset)
                        ctx.strokeStyle = "#8be0d4"
                        ctx.globalAlpha = 0.3 + 0.45 * (1 - offset / 30)
                        ctx.lineWidth = 2.5
                        ctx.stroke()
                    }
                }
            }

            Text { x: view.width * 0.222; y: view.height * 0.615; text: "CF"; color: "#a2b8c2"; font.pixelSize: 14; font.letterSpacing: 2 }
            Text { x: view.width * 0.237; y: view.height * 0.466; text: "P"; color: "#9ed7ca"; font.pixelSize: 12 }

            Rectangle {
                id: musicScreen
                objectName: "musicScreen"
                x: view.width * 0.455
                y: view.height * 0.19
                width: view.width * 0.50
                height: 346
                radius: 11
                color: "#0e1b28"
                border.color: "#617887"
                border.width: 3

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 13
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "在线音乐"; color: "#e9f3f7"; font.pixelSize: 16; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Text { text: musicController.enabled ? "ONLINE" : "未启用"; color: musicController.enabled ? "#8ce0ce" : "#a0adb8"; font.pixelSize: 10 }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        TextField {
                            id: searchInput
                            objectName: "musicSearchInput"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: 36
                            font.pixelSize: 12
                            placeholderText: "歌曲 / 歌手"
                            placeholderTextColor: "#768da0"
                            color: "#e7f0f5"
                            enabled: root.musicAvailable
                            selectByMouse: true
                            background: Rectangle { radius: 7; color: "#152735"; border.color: searchInput.activeFocus ? "#8fd9ca" : "#354d60" }
                            onAccepted: if (root.musicAvailable && text.trim().length > 0) musicController.search(text.trim())
                        }
                        MusicButton { objectName: "musicSearchButton"; text: "搜索"; implicitWidth: 54; enabled: root.musicAvailable && searchInput.text.trim().length > 0; onClicked: musicController.search(searchInput.text.trim()) }
                    }
                    Text {
                        objectName: "musicStatus"
                        Layout.fillWidth: true
                        text: !musicController.enabled ? "在线音乐未启用" : musicController.status
                        color: musicController.busy ? "#edc17d" : "#9db5c7"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    ListView {
                        id: searchResults
                        objectName: "musicResults"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 62
                        clip: true
                        spacing: 3
                        model: musicController.results.slice(0, 10)
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: ItemDelegate {
                            id: songResult
                            required property var modelData
                            width: searchResults.width
                            height: 43
                            enabled: root.musicAvailable
                            onClicked: musicController.select(modelData.index)
                            contentItem: RowLayout {
                                spacing: 8
                                Text { text: songResult.modelData.index; color: "#88cdbf"; font.pixelSize: 12; Layout.preferredWidth: 17 }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text { text: songResult.modelData.title; textFormat: Text.PlainText; color: "#e1edf3"; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: songResult.modelData.artist; textFormat: Text.PlainText; color: "#809aab"; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                                }
                                Text { text: songResult.modelData.playable ? "选择" : "受限"; color: "#839bad"; font.pixelSize: 10 }
                            }
                            background: Rectangle { radius: 5; color: songResult.down ? "#325260" : (songResult.hovered ? "#223b4b" : "#172b39") }
                        }
                        Text { anchors.centerIn: parent; width: parent.width; visible: searchResults.count === 0; text: musicController.enabled ? "搜索后选择编号歌曲" : "配置在线音乐后可搜索"; horizontalAlignment: Text.AlignHCenter; color: "#6d8598"; font.pixelSize: 11; wrapMode: Text.Wrap }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#2c4253" }
                    Text { objectName: "musicTitle"; Layout.fillWidth: true; text: musicController.title || "尚未选择歌曲"; textFormat: Text.PlainText; color: "#e8f1f5"; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; text: musicController.artist || ""; textFormat: Text.PlainText; color: "#8ea6b7"; font.pixelSize: 11; elide: Text.ElideRight; visible: text.length > 0 }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        MusicButton { objectName: "musicPreviousButton"; Layout.fillWidth: true; text: "上一首"; enabled: root.musicAvailable && musicController.canPrevious; onClicked: musicController.previous() }
                        MusicButton { objectName: "musicPlayButton"; Layout.fillWidth: true; text: musicController.playing ? "暂停" : "播放"; enabled: root.musicAvailable && (musicController.playing || musicController.canPlay); onClicked: musicController.playing ? musicController.pause() : musicController.play() }
                        MusicButton { objectName: "musicNextButton"; Layout.fillWidth: true; text: "下一首"; enabled: root.musicAvailable && musicController.canNext; onClicked: musicController.next() }
                    }
                    Text { objectName: "musicError"; Layout.fillWidth: true; text: musicController.error; textFormat: Text.PlainText; visible: text.length > 0; color: "#ff9d91"; font.pixelSize: 11; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight }
                }
            }
        }

        Text {
            objectName: "receiptStatus"
            Layout.fillWidth: true
            text: root.pending ? (root.stateKnown ? "等待本次结果 · 下方保留最近回执" : "等待本次结果 · 车辆状态未知") : (root.stateKnown ? "最近已确认回执" : "车辆状态未知 · 等待有效回执")
            color: root.pending ? "#edc17d" : "#a3b5c1"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 60
                radius: 10
                color: root.stateKnown && root.climateOn ? "#213e3d" : "#202f3b"
                Column {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text { text: "空调"; color: "#97acba"; font.pixelSize: 11 }
                    Text { objectName: "climateStatus"; text: root.stateKnown ? (root.climateOn ? "已开启" : "已关闭") : "未知"; color: root.stateKnown && root.climateOn ? "#9de8d6" : "#e4edf1"; font.pixelSize: 16; font.bold: true }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 60
                radius: 10
                color: root.stateKnown && root.leftFrontWindowOpen ? "#243e48" : "#202f3b"
                Column {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text { text: "左前车窗"; color: "#97acba"; font.pixelSize: 11 }
                    Text { objectName: "leftFrontWindowStatus"; text: root.stateKnown ? (root.leftFrontWindowOpen ? "已全开" : "已关闭") : "未知"; color: root.stateKnown && root.leftFrontWindowOpen ? "#a7ddea" : "#e4edf1"; font.pixelSize: 16; font.bold: true }
                }
            }
        }
        Text { Layout.fillWidth: true; text: "模拟 / 非真实车控 · 状态以执行回执为准"; color: "#8299a9"; font.pixelSize: 11; wrapMode: Text.Wrap }
    }
}
