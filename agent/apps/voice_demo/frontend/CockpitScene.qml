import QtQuick
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

    color: "#15222d"
    radius: 18
    border.color: "#283b49"
    clip: true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 4
                Text { text: "驾驶员视角"; color: "#edf4f6"; font.pixelSize: 19; font.bold: true }
                Text { text: "2.5D COCKPIT"; color: "#8299a9"; font.pixelSize: 11; font.letterSpacing: 2 }
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                implicitWidth: simulationTag.implicitWidth + 18
                implicitHeight: 27
                radius: 13
                color: "#293834"
                Text { id: simulationTag; anchors.centerIn: parent; text: "模拟"; color: "#a1d8c6"; font.pixelSize: 12 }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            Item {
                id: view
                width: parent.width
                height: Math.min(parent.height, width * 0.66)
                anchors.centerIn: parent
                opacity: root.stateKnown ? 1 : 0.65

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
                        ctx.scale(width / 720, height / 475)
                        var sky = ctx.createLinearGradient(0, 35, 0, 260)
                        sky.addColorStop(0, "#486778")
                        sky.addColorStop(1, "#b3c3bd")
                        polygon(ctx, [[202,57], [644,57], [692,261], [170,268]], sky, "#405363", 4)
                        polygon(ctx, [[202,174], [256,138], [303,163], [358,128], [418,172], [486,136], [551,164], [653,147], [682,251], [181,256]], "#718e8e", "", 0)
                        polygon(ctx, [[192,207], [668,204], [684,257], [179,260]], "#677b78", "", 0)
                        polygon(ctx, [[429,191], [453,191], [571,266], [298,266]], "#53666a", "", 0)
                        polygon(ctx, [[439,205], [443,205], [452,218], [444,218]], "#d4d9bb", "", 0)
                        polygon(ctx, [[451,233], [458,233], [471,253], [459,253]], "#d4d9bb", "", 0)
                        polygon(ctx, [[18,96], [168,78], [158,260], [18,325]], "#78928f", "#3a4d58", 5)
                        polygon(ctx, [[18,236], [161,187], [158,260], [18,325]], "#576e6c", "", 0)
                        polygon(ctx, [[0,54], [179,44], [207,60], [170,277], [148,284], [158,93], [0,109]], "#26343e", "#4b5d68", 2)
                        polygon(ctx, [[182,42], [650,42], [662,62], [205,66]], "#26343e", "#4b5d68", 2)
                        polygon(ctx, [[649,49], [675,57], [720,265], [691,274]], "#283944", "#4b5d68", 2)
                        polygon(ctx, [[13,319], [164,253], [178,271], [37,357], [0,362]], "#344651", "#52616b", 2)
                        polygon(ctx, [[0,360], [148,276], [181,313], [115,475], [0,475]], "#202d36", "#3a4d5b", 2)
                        polygon(ctx, [[172,260], [684,250], [720,325], [687,386], [135,361]], "#263844", "#526574", 2)
                        var dash = ctx.createLinearGradient(0, 280, 0, 465)
                        dash.addColorStop(0, "#344955")
                        dash.addColorStop(1, "#15212b")
                        polygon(ctx, [[163,308], [691,299], [720,405], [720,475], [95,475]], dash, "#4b606c", 2)
                        ctx.beginPath()
                        ctx.moveTo(160,312)
                        ctx.bezierCurveTo(294,327,497,296,699,323)
                        ctx.strokeStyle = "#7fb4ae"
                        ctx.lineWidth = 2
                        ctx.stroke()
                        polygon(ctx, [[191,292], [353,289], [362,342], [186,345]], "#0e1921", "#536b78", 2)
                        for (var g = 0; g < 2; ++g) {
                            ctx.beginPath()
                            ctx.arc(238 + g * 74, 325, 23, Math.PI * 1.1, Math.PI * 1.9)
                            ctx.strokeStyle = "#91a9af"
                            ctx.lineWidth = 3
                            ctx.stroke()
                        }
                        polygon(ctx, [[456,279], [610,279], [623,319], [453,320]], "#101c25", "#617684", 2)
                        for (var slat = 0; slat < 5; ++slat) {
                            ctx.beginPath()
                            ctx.moveTo(466,287 + slat * 6)
                            ctx.lineTo(607 + slat,287 + slat * 6)
                            ctx.strokeStyle = "#4c646e"
                            ctx.lineWidth = 2
                            ctx.stroke()
                        }
                        polygon(ctx, [[125,301], [166,283], [182,311], [134,336]], "#111d25", "#566d79", 2)
                        for (var sideSlat = 0; sideSlat < 4; ++sideSlat) {
                            ctx.beginPath()
                            ctx.moveTo(135,304 + sideSlat * 6)
                            ctx.lineTo(164,291 + sideSlat * 6)
                            ctx.strokeStyle = "#4c646e"
                            ctx.lineWidth = 2
                            ctx.stroke()
                        }
                        polygon(ctx, [[457,349], [613,341], [615,421], [456,430]], "#0d1c25", "#506d7c", 2)
                        polygon(ctx, [[500,449], [624,441], [675,475], [477,475]], "#0e1921", "#3b5361", 2)
                        ctx.save()
                        ctx.translate(266,379)
                        ctx.scale(1,0.84)
                        ctx.beginPath()
                        ctx.arc(0,0,86,0,Math.PI * 2)
                        ctx.strokeStyle = "#0c141b"
                        ctx.lineWidth = 21
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.arc(0,0,88,0,Math.PI * 2)
                        ctx.strokeStyle = "#526672"
                        ctx.lineWidth = 3
                        ctx.stroke()
                        polygon(ctx, [[-77,-12], [-21,-6], [21,-6], [78,-12], [69,12], [25,17], [15,74], [-14,74], [-25,17], [-68,12]], "#283944", "#4a5f6d", 2)
                        ctx.beginPath()
                        ctx.ellipse(-35,-28,70,58)
                        ctx.fillStyle = "#172630"
                        ctx.fill()
                        ctx.strokeStyle = "#4a5f6d"
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
                    // 动画只在已验证回执后表现玻璃变化；未知状态隐藏玻璃，不冒充关闭。
                    Behavior on lowered { enabled: root.stateKnown; NumberAnimation { duration: 650; easing.type: Easing.InOutCubic } }
                    onLoweredChanged: requestPaint()
                    onVisibleChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        ctx.clearRect(0,0,width,height)
                        ctx.scale(width / 720,height / 475)
                        ctx.beginPath()
                        ctx.moveTo(20,99)
                        ctx.lineTo(165,81)
                        ctx.lineTo(155,258)
                        ctx.lineTo(20,319)
                        ctx.closePath()
                        ctx.clip()
                        var top = 81 + lowered * 248
                        var glass = ctx.createLinearGradient(20,top,156,325)
                        glass.addColorStop(0,"rgba(161,217,229,0.6)")
                        glass.addColorStop(1,"rgba(161,217,229,0.17)")
                        ctx.fillStyle = glass
                        ctx.fillRect(15,top,160,250)
                        ctx.beginPath()
                        ctx.moveTo(16,top + 20)
                        ctx.lineTo(171,top)
                        ctx.strokeStyle = "#c3e5eb"
                        ctx.lineWidth = 2
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.moveTo(47,top + 29)
                        ctx.lineTo(86,top + 111)
                        ctx.moveTo(60,top + 20)
                        ctx.lineTo(101,top + 106)
                        ctx.strokeStyle = "rgba(201,233,237,0.4)"
                        ctx.lineWidth = 5
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
                        ctx.scale(width / 720,height / 475)
                        for (var i = 0; i < 4; ++i) {
                            var offset = ((phase + i * 0.23) % 1) * 34
                            ctx.beginPath()
                            ctx.moveTo(474 + i * 33,281 - offset)
                            ctx.bezierCurveTo(452 + i * 33,255 - offset,495 + i * 33,231 - offset,478 + i * 33,210 - offset)
                            ctx.strokeStyle = "#8be0d4"
                            ctx.globalAlpha = 0.32 + 0.45 * (1 - offset / 34)
                            ctx.lineWidth = 2.5
                            ctx.stroke()
                        }
                    }
                }

                Text {
                    x: view.width * 0.655; y: view.height * 0.745
                    width: view.width * 0.2
                    horizontalAlignment: Text.AlignHCenter
                    text: root.stateKnown ? (root.climateOn ? "AC  ON" : "AC  OFF") : "AC  —"
                    color: root.stateKnown && root.climateOn ? "#93e7d6" : "#91a6b2"
                    font.pixelSize: Math.max(11,view.width * 0.022)
                    font.letterSpacing: 1
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
                implicitHeight: 66
                radius: 10
                color: root.stateKnown && root.climateOn ? "#213e3d" : "#202f3b"
                Column {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5
                    Text { text: "空调"; color: "#97acba"; font.pixelSize: 12 }
                    Text { objectName: "climateStatus"; text: root.stateKnown ? (root.climateOn ? "已开启" : "已关闭") : "未知"; color: root.stateKnown && root.climateOn ? "#9de8d6" : "#e4edf1"; font.pixelSize: 17; font.bold: true }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 66
                radius: 10
                color: root.stateKnown && root.leftFrontWindowOpen ? "#243e48" : "#202f3b"
                Column {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5
                    Text { text: "左前车窗"; color: "#97acba"; font.pixelSize: 12 }
                    Text { objectName: "leftFrontWindowStatus"; text: root.stateKnown ? (root.leftFrontWindowOpen ? "已全开" : "已关闭") : "未知"; color: root.stateKnown && root.leftFrontWindowOpen ? "#a7ddea" : "#e4edf1"; font.pixelSize: 17; font.bold: true }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: "模拟 / 非真实车控\n状态以执行回执为准 · 动画仅为展示过渡"
            color: "#8299a9"
            font.pixelSize: 11
            lineHeight: 1.4
            wrapMode: Text.Wrap
        }
    }
}
