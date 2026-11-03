import QtQuick

SeekSlider {
    id: slider
    property real mediaPosition: 0
    property bool followingPlayback: true
    signal seekRequested(real target)
    live: false

    Binding {
        target: slider; property: "value"; value: slider.mediaPosition
        when: slider.followingPlayback
        restoreMode: Binding.RestoreNone
    }
    onPressedChanged: {
        if (pressed) followingPlayback = false
        else {
            seekRequested(valueAt(position))
            // 松手事件完成后恢复绑定，避免旧播放进度抢先覆盖用户选择。
            Qt.callLater(function() { slider.followingPlayback = true })
        }
    }
    onMoved: if (!pressed && followingPlayback) seekRequested(value)
}
