import QtQuick
import QtQuick.Controls
Slider {
    id: slider
    implicitHeight: 28
    property color activeColor: "#eeeeee"
    background: Rectangle {
        x: slider.leftPadding; y: (slider.height-height)/2
        width: slider.availableWidth; height: 4; radius: 2; color: "#35ffffff"
        Rectangle {width: slider.visualPosition*parent.width; height: parent.height; radius: 2; color: slider.activeColor}
    }
    handle: Rectangle {
        x: slider.leftPadding + slider.visualPosition*(slider.availableWidth-width)
        y: (slider.height-height)/2; width: slider.pressed ? 16 : 12; height: width; radius: width/2
        color: slider.activeColor
    }
}
