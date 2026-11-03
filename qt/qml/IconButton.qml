import QtQuick
import QtQuick.Controls
Button {
    id: button
    property string glyph: "play"
    property string hint: ""
    property color ink: "#eeeeee"
    implicitWidth: 38; implicitHeight: 38
    hoverEnabled: true
    Accessible.name: hint
    ToolTip.visible: hovered && hint.length > 0
    ToolTip.text: hint; ToolTip.delay: 650
    background: Rectangle {radius: 10; color: button.down ? "#35ffffff" : button.hovered ? "#18ffffff" : "transparent"}
    contentItem: Item { Glyph {anchors.centerIn: parent; width: 22; height: 22; name: button.glyph; color: button.ink} }
}
