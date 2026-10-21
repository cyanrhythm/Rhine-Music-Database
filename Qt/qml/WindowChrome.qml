import QtQuick
import QtQuick.Window

// Windows 无边框模式
Item {
    id: chrome
    required property Window window
    property bool dark: false
    readonly property color ink: dark ? "#ebebeb" : "#141613"
    readonly property bool maximized: window.visibility === Window.Maximized
    anchors.fill: parent

    function toggleMaximized() {
        if (maximized) {
            window.showNormal();
        } else {
            window.showMaximized();
        }
    }

    Item {
        id: titleBar
        objectName: "windowTitleBar"
        width: parent.width
        height: 32

        Item {
            width: parent.width - windowButtons.width
            height: parent.height
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: chrome.toggleMaximized()
            }
            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: {
                    if (active) {
                        chrome.window.startSystemMove();
                    }
                }
            }
        }
        Row {
            id: windowButtons
            anchors.right: parent.right
            height: parent.height
            Repeater {
                model: ["minimize", "maximize", "close"]
                delegate: Rectangle {
                    id: button
                    required property string modelData
                    readonly property bool closing: modelData === "close"
                    readonly property string label: {
                        if (closing) {
                            return "关闭";
                        }
                        if (modelData === "minimize") {
                            return "最小化";
                        }
                        return chrome.maximized ? "还原" : "最大化";
                    }
                    readonly property color glyph: {
                        if (!buttonMouse.containsMouse) {
                            return chrome.ink;
                        }
                        return closing ? "#a25f55" : "#777b60";
                    }
                    objectName: "windowButton_" + modelData
                    width: 44
                    height: titleBar.height
                    color: "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: label
                    Accessible.onPressAction: activate()

                    function activate() {
                        if (modelData === "minimize") {
                            chrome.window.showMinimized();
                        } else if (modelData === "maximize") {
                            chrome.toggleMaximized();
                        } else {
                            chrome.window.close();
                        }
                    }

                    // 图标由细线构成，避免依赖系统符号字体和不同 Windows 版本的字形。
                    Rectangle {
                        anchors.centerIn: parent
                        width: 10
                        height: 1
                        color: button.glyph
                        visible: button.modelData === "minimize"
                    }
                    Rectangle {
                        x: 19
                        y: 10
                        width: 9
                        height: 9
                        color: "transparent"
                        border.color: button.glyph
                        visible: button.modelData === "maximize" && chrome.maximized
                    }
                    Rectangle {
                        x: chrome.maximized ? 16 : 17
                        y: chrome.maximized ? 13 : 11
                        width: 10
                        height: 10
                        color: button.color
                        border.color: button.glyph
                        visible: button.modelData === "maximize"
                    }
                    Repeater {
                        model: [-45, 45]
                        delegate: Rectangle {
                            required property int modelData
                            anchors.centerIn: parent
                            width: 13
                            height: 1
                            rotation: modelData
                            color: button.glyph
                            visible: button.closing
                        }
                    }
                    MouseArea {
                        id: buttonMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: button.activate()
                    }
                }
            }
        }
    }

    Repeater {
        model: [Qt.TopEdge | Qt.LeftEdge, Qt.TopEdge, Qt.TopEdge | Qt.RightEdge, Qt.LeftEdge, Qt.RightEdge, Qt.BottomEdge | Qt.LeftEdge, Qt.BottomEdge, Qt.BottomEdge | Qt.RightEdge]
        delegate: MouseArea {
            required property int modelData
            readonly property bool leftEdge: (modelData & Qt.LeftEdge) !== 0
            readonly property bool rightEdge: (modelData & Qt.RightEdge) !== 0
            readonly property bool topEdge: (modelData & Qt.TopEdge) !== 0
            readonly property bool bottomEdge: (modelData & Qt.BottomEdge) !== 0
            readonly property bool corner: (leftEdge || rightEdge) && (topEdge || bottomEdge)
            enabled: !chrome.maximized && chrome.window.visibility !== Window.FullScreen
            x: {
                if (leftEdge) {
                    return 0;
                }
                return rightEdge ? chrome.width - 5 : 5;
            }
            y: {
                if (topEdge) {
                    return 0;
                }
                return bottomEdge ? chrome.height - 5 : 5;
            }
            width: leftEdge || rightEdge ? 5 : chrome.width - 10
            height: topEdge || bottomEdge ? 5 : chrome.height - 10
            cursorShape: {
                if (corner) {
                    return leftEdge === topEdge ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor;
                }
                return leftEdge || rightEdge ? Qt.SizeHorCursor : Qt.SizeVerCursor;
            }
            onPressed: chrome.window.startSystemResize(modelData)
        }
    }
}
