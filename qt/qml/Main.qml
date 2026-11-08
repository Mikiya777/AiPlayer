import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects
import AiPlayer 1.0

ApplicationWindow {
    id: root
    width: 1200; height: 780; minimumWidth: 880; minimumHeight: 580
    visible: true; color: "#0d0e10"; title: player.savingRecording ? "正在保存录制 — AiPlayer" : (playingPage ? mediaTitle + " — AiPlayer" : "AiPlayer")
    property bool playingPage: false
    property string mediaTitle: ""
    property bool gridMode: false
    property string section: "主屏幕"
    property url selectedFolder: ""
    property bool foldersExpanded: true
    property bool fileManagerPage: section === "文件" && !selectedFolder.toString()
    function showFileManager() { selectedFolder = ""; section = "文件"; foldersExpanded = true }
    property var selectedFolderInfo: {
        let folders = mediaLibrary.folders
        for (let folder of folders)
            if (folder.url.toString() === selectedFolder.toString()) return folder
        return null
    }
    property var playlistFiles: {
        let files = mediaLibrary.files // 订阅索引变化，文件夹扫描完成后重新取值。
        return selectedFolder.toString() ? mediaLibrary.FilesForFolder(selectedFolder) : files
    }
    property var visibleFiles: selectedFolder.toString() ? playlistFiles : mediaLibrary.files
    function selectFolder(url) { selectedFolder = url; section = "文件"; foldersExpanded = true }
    function removeFolder(url) {
        if (selectedFolder.toString() === url.toString()) selectedFolder = ""
        mediaLibrary.RemoveFolder(url)
    }
    function renameFolder(url, name) {
        folderRename.folderUrl = url; folderName.text = name; folderRename.open()
    }
    property color accent: "#ff9b28"
    property bool dockMoved: false
    property bool controlsShown: true
    property var player: playbackController
    property bool wasFullScreen: false
    property int currentIndex: {
        let files = root.playlistFiles
        for (let i = 0; i < files.length; ++i)
            if (files[i].url.toString() === player.source.toString()) return i
        return -1
    }
    onVisibilityChanged: {
        if (visibility !== Window.FullScreen && visibility !== Window.Windowed && visibility !== Window.Maximized) return
        let nowFullScreen = visibility === Window.FullScreen
        if (nowFullScreen !== wasFullScreen) {
            wasFullScreen = nowFullScreen
            dockMoved = false
            Qt.callLater(root.placeDock)
            revealControls()
        }
    }
    function revealControls() { controlsShown = true; hideControls.stop() }
    function scheduleHide() { hideControls.restart() }
    onActiveChanged: {
        if (playingPage) {
            if (active && stageHover.hovered) revealControls()
            else scheduleHide()
        }
    }
    onPlayingPageChanged: {
        if (playingPage) { revealControls(); scheduleHide() }
        else { hideControls.stop(); filterPanel.close() }
    }
    Timer {
        id: hideControls; interval: 700
        onTriggered: {
            if (!root.playingPage) return
            if (drag.pressed || seek.pressed || volumeSlider.pressed || rateMenu.visible || playlist.visible || filterPanel.visible) restart()
            else if (!stageHover.hovered || !root.active) root.controlsShown = false
        }
    }
    property string query: search.text.toLowerCase()
    function time(ms) { let s=Math.max(0,Math.floor(ms/1000));return (s>=3600 ? Math.floor(s/3600)+":" : "")+ (s>=3600 ? ("0"+Math.floor(s/60)%60).slice(-2): Math.floor(s/60))+":"+("0"+s%60).slice(-2) }
    function openMedia(url) { mediaLibrary.AddFiles([url]); mediaTitle=mediaLibrary.NameFor(url); player.source=url; playingPage=true; dockMoved=false; placeDock(); player.Play() }
    function placeDock() {
        if (!dock || !stage) return
        dock.x = (stage.width - dock.width) / 2
        dock.y = Math.max(62, Math.min(stage.height - dock.height - 12, stage.height * 0.65 - dock.height / 2))
    }
    function backToLibrary() {playlist.close();player.Stop();playingPage=false;if(visibility===Window.FullScreen)showNormal()}
    function fullScreen() {if(visibility===Window.FullScreen)showNormal();else showFullScreen()}
    function skip(amount) {if(player.recording)return;player.position=Math.max(0,Math.min(player.duration,player.position+amount))}
    function nextFile(delta) {
        let next = currentIndex + delta
        if (currentIndex >= 0 && next >= 0 && next < root.playlistFiles.length)
            openMedia(root.playlistFiles[next].url)
    }
    function removeFromPlaylist(url) {
        let isCurrent = root.playingPage && url.toString() === player.source.toString()
        let index = currentIndex
        let wasPlaying = player.playing
        mediaLibrary.RemoveFile(url)
        if (isCurrent) {
            if (!root.playlistFiles.length) backToLibrary()
            else {
                openMedia(root.playlistFiles[Math.min(index, root.playlistFiles.length - 1)].url)
                if (!wasPlaying) player.Pause()
            }
        }
    }
    function clearPlaylist() { backToLibrary(); mediaLibrary.Clear() }
    function adjustVolume(delta) {
        let volume = player.muted ? 0 : player.volume
        player.volume = Math.max(0, Math.min(1, Math.round((volume + delta) * 100) / 100))
        player.muted = false
        revealControls()
        volumeNotice.restart()
    }
    Timer { id: volumeNotice; interval: 1000 }
    Shortcut {sequence: "Ctrl+O"; onActivated: filePicker.open()}
    Shortcut {sequence: "Space"; enabled: root.playingPage; onActivated: player.TogglePlayback()}
    Shortcut {sequence: "Left"; enabled: root.playingPage; onActivated: root.skip(-10000)}
    Shortcut {sequence: "Up"; enabled: root.playingPage && !rateMenu.visible; onActivated: root.adjustVolume(0.05)}
    Shortcut {sequence: "Down"; enabled: root.playingPage && !rateMenu.visible; onActivated: root.adjustVolume(-0.05)}
    Shortcut {sequence: "Right"; enabled: root.playingPage; onActivated: root.skip(10000)}
    Shortcut {sequence: "Escape"; onActivated: {if(playlist.visible)playlist.close();else if(root.visibility===Window.FullScreen)root.showNormal();else if(root.playingPage)root.backToLibrary()}}
    FileDialog {
        id: filePicker; title: "选择视频或音频"; fileMode: FileDialog.OpenFiles
        nameFilters: ["媒体文件 (*.mp4 *.mkv *.mov *.m4v *.avi *.webm *.mp3 *.flac *.m4a)", "所有文件 (*)"]
        onAccepted: {
            if (!selectedFiles.length) return
            root.selectedFolder = ""; root.section = "主屏幕"
            mediaLibrary.AddFiles(selectedFiles); root.openMedia(selectedFiles[0])
        }
    }
    FileDialog {
        id:recordingPicker;title:"保存录制视频";fileMode:FileDialog.SaveFile
        nameFilters:["MP4 视频 (*.mp4)"];defaultSuffix:"mp4"
        onAccepted:player.StartRecording(selectedFile)
    }
    Connections {
        target:player
        function onRecordingSaved(path) { recordingResult.text="录制已保存：\n"+path;recordingResult.open() }
    }
    Dialog {id:recordingResult;property string text:"";title:"录制完成";anchors.centerIn:parent;modal:true;standardButtons:Dialog.Ok
        contentItem:Text {text:recordingResult.text;wrapMode:Text.Wrap;width:420;color:"#eeeeee"}}
    onClosing:function(close) {
        if(player.recording || player.savingRecording) {close.accepted=false;player.StopRecording();root.revealControls()}
    }
    component FilterCheck: CheckBox {
        id:checkControl
        implicitWidth:224;implicitHeight:40
        indicator:Rectangle {
            x:6;y:(parent.height-height)/2;width:22;height:22;radius:6
            color:checkControl.checked ? "#96e0bd" : "#35353e"
            border.color:checkControl.checked ? "#96e0bd" : "#70707b"
            Text {anchors.centerIn:parent;text:"✓";font.pixelSize:16;font.bold:true;color:"#143d2c";visible:checkControl.checked}
        }
        contentItem:Text {text:checkControl.text;color:"#eeeeef";leftPadding:40;verticalAlignment:Text.AlignVCenter;font.pixelSize:14}
    }
    Popup {
        id:filterPanel;objectName:"filterPanel"
        palette.text:"#eeeeef";palette.windowText:"#eeeeef";palette.buttonText:"#eeeeef"
        onOpened:root.revealControls()
        onClosed:root.scheduleHide()
        parent:filterToggle
        x:parent.width-width;y:parent.height+12;width:260
        // 按钮在父项范围内，点击它不会先触发外部点击关闭再重新打开。
        closePolicy:Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        padding:18; modal:false
        background:Rectangle {color:"#ee24242b";radius:16;border.color:"#55555e"}
        contentItem:Column {
            spacing:8
            Text {text:"视频滤镜";color:"white";font.pixelSize:18}
            FilterCheck {objectName:"flipFilter";text:"垂直翻转";checked:video.verticalFlip;onToggled:video.verticalFlip=checked}
            FilterCheck {objectName:"grayFilter";text:"黑白";checked:video.grayscale;onToggled:video.grayscale=checked}
            FilterCheck {objectName:"invertFilter";text:"颜色反相";checked:video.inverted;onToggled:video.inverted=checked}
            FilterCheck {objectName:"stickerFilter";text:"人脸贴纸";checked:video.faceSticker;onToggled:video.faceSticker=checked}
            Grid {
                id: stickerGrid
                visible: video.faceSticker
                width: 224; columns: 2; spacing: 6
                Repeater {
                    model: video.stickerNames
                    delegate: ActionButton {
                        required property int index
                        required property string modelData
                        objectName: "stickerStyle" + index
                        width: (stickerGrid.width - stickerGrid.spacing) / 2
                        implicitHeight: 32; leftPadding: 6; rightPadding: 6
                        text: modelData; primary: video.stickerStyle === index
                        onClicked: video.stickerStyle = index
                    }
                }
            }
            Text {width:224;text:video.filterError;color:"#ffb5b5";wrapMode:Text.Wrap;visible:text.length>0}
            Text {width:224;text:"可叠加使用 · 贴纸跟随检测到的人脸";color:"#a9a9b2";font.pixelSize:12;wrapMode:Text.Wrap}
            ActionButton {text:"恢复原画";onClicked:{video.verticalFlip=false;video.grayscale=false;video.inverted=false;video.faceSticker=false}}
        }
    }
    FolderDialog {
        id: folderPicker; title: "添加视频文件夹"
        onAccepted: {
            let added = mediaLibrary.AddFolder(selectedFolder)
            if (added.toString()) root.showFileManager()
        }
    }
    Connections {
        target: mediaLibrary
        function onFilesChanged() {
            if (!root.selectedFolder.toString()) return
            for (let folder of mediaLibrary.folders)
                if (folder.url.toString() === root.selectedFolder.toString()) return
            root.selectedFolder = ""
        }
    }
    Dialog {
        id: folderRename; title: "修改文件夹显示名称"; property url folderUrl: ""
        anchors.centerIn: parent; width: 360; modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { folderName.forceActiveFocus(); folderName.selectAll() }
        onAccepted: mediaLibrary.RenameFolder(folderUrl, folderName.text)
        ColumnLayout {
            width: parent.width
            TextField { id: folderName; Layout.fillWidth: true; maximumLength: 80; placeholderText: "文件夹名称" }
            Label { text: "只修改媒体库中的名称，磁盘目录保持原样。"; color: "#9a9ca5"; font.pixelSize: 11 }
        }
    }
    component ContextMenuItem: MenuItem {
        id: menuEntry
        implicitWidth: 240; implicitHeight: 40
        leftPadding: 14; rightPadding: 14
        contentItem: Text {
            text: menuEntry.text; font.pixelSize: 13
            color: menuEntry.enabled ? "#eeeeef" : "#777781"
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 8
            color: menuEntry.down ? "#55555f" : menuEntry.highlighted ? "#41414b" : "transparent"
        }
    }
    component ContextMenu: Menu {
        padding: 6; spacing: 3; width: 252
        delegate: ContextMenuItem {}
        palette.text: "#eeeeef"; palette.windowText: "#eeeeef"
        background: Rectangle { color: "#ee24242b"; radius: 14; border.color: "#55555e" }
    }
    component ContextSeparator: MenuSeparator {
        topPadding: 5; bottomPadding: 5; leftPadding: 8; rightPadding: 8
        contentItem: Rectangle { implicitWidth: 224; implicitHeight: 1; color: "#484852" }
    }
    component FolderMenu: ContextMenu {
        property url folderUrl
        property string folderName
        ContextMenuItem { text: "刷新视频列表"; onTriggered: mediaLibrary.RefreshFolder(folderUrl) }
        ContextMenuItem { text: "修改显示名称"; onTriggered: root.renameFolder(folderUrl, folderName) }
        ContextSeparator {}
        ContextMenuItem { text: "移除文件夹（保留本地文件）"; onTriggered: root.removeFolder(folderUrl) }
    }
    Connections {
        target: player
        function onErrorOccurred() { if (player.errorString) errorBox.open() }
        function onPlaybackFinished() { if (root.playingPage) root.nextFile(1) }
    }
    FileDialog {
        id: playlistPicker; title: "添加到播放列表"; fileMode: FileDialog.OpenFiles
        nameFilters: filePicker.nameFilters
        onAccepted: mediaLibrary.AddFiles(selectedFiles)
    }
    Drawer {
        id: playlist; objectName: "playlistPanel"
        edge: Qt.RightEdge; width: Math.min(360, root.width * 0.45); height: root.height
        modal: false; dim: false; interactive: false
        background: Rectangle { color: "#f21b1c20"; border.color: "#45464a" }
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 16; spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label { text: "播放列表 · " + root.playlistFiles.length; color: "#eeeeee"; font.pixelSize: 18; Layout.fillWidth: true }
                IconButton { objectName: "closePlaylist"; glyph: "close"; hint: "关闭列表"; onClicked: playlist.close() }
            }
            RowLayout {
                ActionButton { text: "添加文件"; onClicked: playlistPicker.open() }
                ActionButton { objectName: "clearPlaylist"; text: "清空"; enabled: root.playlistFiles.length > 0; onClicked: root.clearPlaylist() }
            }
            Label { text: "按列表顺序播放，播完自动切换下一项"; color: "#9b9da5"; font.pixelSize: 11 }
            ListView {
                id: playlistView; objectName: "playlistView"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 6
                model: root.playlistFiles; currentIndex: root.currentIndex
                onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: queueRow
                    required property var modelData
                    required property int index
                    objectName: "playlistRow" + index
                    width: playlistView.width; height: 66; radius: 9
                    color: index === root.currentIndex ? "#493827" : "#292a2e"
                    Button {
                        objectName: "playlistPlay" + queueRow.index
                        anchors.fill: parent; anchors.rightMargin: 38
                        background: Item {}
                        contentItem: Column {
                            spacing: 6
                            Text { width: parent.width; text: (queueRow.index + 1) + ". " + queueRow.modelData.title; elide: Text.ElideRight; color: "#eeeeee" }
                            Text { text: queueRow.index === root.currentIndex ? (player.playing ? "正在播放" : "当前项目") : queueRow.modelData.extension; color: root.accent; font.pixelSize: 11 }
                        }
                        onClicked: root.openMedia(queueRow.modelData.url)
                    }
                    IconButton {
                        objectName: "playlistRemove" + queueRow.index
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        glyph: "close"; hint: "从列表移除"; onClicked: root.removeFromPlaylist(queueRow.modelData.url)
                    }
                }
                Label { anchors.centerIn: parent; visible: root.playlistFiles.length === 0; text: "播放列表为空"; color: "#999ba3" }
            }
        }
    }
    Dialog {
        id: errorBox; objectName: "playbackErrorDialog"; anchors.centerIn: parent; width: 430; modal: true; title: "播放出现问题"
        standardButtons: Dialog.Ok
        Label {width: parent.width; wrapMode: Text.Wrap; text: player.errorString || "请尝试其他媒体文件。"; color: "#eeeeee"}
        background: Rectangle {color: "#252629";radius: 16;border.color: "#45464a"}
    }
    component ActionButton: Button {
        id: action; property bool primary: false
        implicitHeight: 42; leftPadding: 20; rightPadding: 20; hoverEnabled: true
        background: Rectangle {radius: 11; color: action.primary ? (action.hovered ? "#ffb354" : root.accent) : (action.hovered ? "#343539" : "#25262a"); border.color: action.primary ? "transparent" : "#3d3e42"}
        contentItem: Text {text: action.text; color: action.primary ? "#201509" : "#e8e8ea";font.pixelSize: 13;font.weight: Font.DemiBold;horizontalAlignment: Text.AlignHCenter;verticalAlignment: Text.AlignVCenter}
    }
    RowLayout {
        anchors.fill: parent; spacing: 0; visible: !root.playingPage
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 224
            gradient: Gradient {GradientStop {position:0;color:"#222326"} GradientStop {position:1;color:"#1b1b1e"}}
            Rectangle {anchors.right:parent.right;width:1;height:parent.height;color:"#343438"}
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 20; spacing: 10
                RowLayout {
                    Layout.topMargin: 14; Layout.bottomMargin: 27; spacing: 10
                    Rectangle {implicitWidth:34;implicitHeight:34;radius:11;color:root.accent;Glyph {anchors.centerIn:parent;width:20;height:20;name:"play";color:"#211609"}}
                    Text {text:"AiPlayer";color:"#f5f5f7";font.pixelSize:22;font.weight:Font.DemiBold; font.letterSpacing:-0.8}
                }
                Rectangle {
                    Layout.fillWidth:true;implicitHeight:36;radius:10;color:"#151518";border.color:"#3b3b40"
                    Glyph {x:10;anchors.verticalCenter:parent.verticalCenter;width:16;height:16;name:"search";color:"#83858c"}
                    TextField {id:search;anchors.fill:parent;leftPadding:34;placeholderText:"搜索媒体";color:"#f5f5f7";placeholderTextColor:"#85858c";font.pixelSize:12;background:Item{}}
                }
                Text {text:"浏览";color:"#767880";font.pixelSize:11;Layout.topMargin:22;Layout.bottomMargin:4;leftPadding:10}
                Repeater {
                    model: [{label:"主屏幕",icon:"play"},{label:"媒体库",icon:"film"},{label:"文件",icon:"folder"}]
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth:true;implicitHeight:42;radius:10;color:root.section===modelData.label && !root.selectedFolder.toString() ? "#414145" : navMouse.containsMouse ? "#2d2d31" : "transparent"
                        Row {anchors.verticalCenter:parent.verticalCenter;x:12;spacing:12;Glyph {width:20;height:20;name:modelData.icon;color:root.accent}Text{text:modelData.label;color:"#ededf0";font.pixelSize:14}}
                        Text { visible: modelData.label === "文件"; anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: root.foldersExpanded ? "⌄" : "›"; color: "#93959e" }
                        MouseArea {id:navMouse;anchors.fill:parent;hoverEnabled:true;cursorShape:Qt.PointingHandCursor;onClicked: {
                            let wasFileManager = root.fileManagerPage
                            root.selectedFolder = ""
                            root.section = modelData.label
                            if (modelData.label === "文件") root.foldersExpanded = wasFileManager ? !root.foldersExpanded : true
                        }}
                    }
                }
                ListView {
                    id: sidebarFolders; objectName: "sidebarFolders"
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                    model: root.foldersExpanded ? mediaLibrary.folders : []
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        id: sidebarFolder
                        required property var modelData
                        width: sidebarFolders.width; height: 38; radius: 8
                        color: root.selectedFolder.toString() === modelData.url.toString() ? "#414145" : folderMouse.containsMouse ? "#2d2d31" : "transparent"
                        Glyph { x: 18; anchors.verticalCenter: parent.verticalCenter; width: 17; height: 17; name: "folder"; color: root.accent }
                        Text { x: 44; width: parent.width - 76; anchors.verticalCenter: parent.verticalCenter; text: modelData.name; elide: Text.ElideRight; color: "#ededf0"; font.pixelSize: 13 }
                        MouseArea {
                            id: folderMouse; anchors.fill: parent; hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: mouse => { if (mouse.button === Qt.RightButton) sidebarMenu.popup(); else root.selectFolder(sidebarFolder.modelData.url) }
                        }
                        IconButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; width: 28; glyph: "more"; hint: "管理文件夹"; onClicked: sidebarMenu.popup() }
                        FolderMenu { id: sidebarMenu; folderUrl: sidebarFolder.modelData.url; folderName: sidebarFolder.modelData.name }
                        ToolTip.visible: folderMouse.containsMouse
                        ToolTip.text: modelData.path
                        ToolTip.delay: 800
                    }
                }

            }
        }
        ColumnLayout {
            Layout.fillHeight:true;Layout.fillWidth:true;spacing:0
            Rectangle {
                Layout.fillWidth:true;implicitHeight:70;color:"#17181a"
                RowLayout {anchors.fill:parent;anchors.leftMargin:30;anchors.rightMargin:26
                    IconButton { visible: !!root.selectedFolder.toString(); glyph: "back"; hint: "返回文件管理"; onClicked: root.showFileManager() }
                    Text {text:root.selectedFolderInfo ? root.selectedFolderInfo.name : root.section;color:"#f1f1f3";font.pixelSize:17;font.weight:Font.DemiBold}
                    Item {Layout.fillWidth:true}
                    IconButton {objectName:"viewToggle";visible: !root.fileManagerPage && root.section !== "媒体库";glyph:root.gridMode?"list":"grid";hint:"切换列表 / 网格";onClicked:root.gridMode=!root.gridMode}
                    IconButton { visible: !!root.selectedFolderInfo; glyph: "more"; hint: "管理当前文件夹"; onClicked: currentFolderMenu.popup() }
                    FolderMenu { id: currentFolderMenu; folderUrl: root.selectedFolder; folderName: root.selectedFolderInfo ? root.selectedFolderInfo.name : "" }
                    ActionButton {objectName: "openFileSource"; visible: root.fileManagerPage; text:"打开文件";onClicked:filePicker.open()}
                    ActionButton {objectName: "addFolderSource"; visible: root.fileManagerPage; text:"添加文件夹";primary:true;onClicked:folderPicker.open()}
                }
                Rectangle {anchors.bottom:parent.bottom;width:parent.width;height:1;color:"#292a2d"}
            }
            Item {
                Layout.fillHeight:true;Layout.fillWidth:true
                Column {
                    visible:root.section==="主屏幕" && mediaLibrary.files.length===0;anchors.centerIn:parent;anchors.verticalCenterOffset:-24;width:Math.min(parent.width-70,560);spacing:0
                    Item {
                        width:parent.width;height:164
                        Rectangle {anchors.centerIn:parent;width:154;height:154;radius:77;color:"#151719";border.color:"#232527"}
                        Rectangle {anchors.centerIn:parent;width:108;height:108;radius:31;rotation:-9;color:"#242529";border.color:"#3f3f43"}
                        Rectangle {anchors.centerIn:parent;width:100;height:100;radius:28;color:"#292827";border.color:"#6b5031";Glyph {anchors.centerIn:parent;width:44;height:44;name:"play";color:root.accent}}
                    }
                    Text {width:parent.width;text:"你的私人影院，从这里开始";color:"#f3f3f5";font.pixelSize:27;font.weight:Font.DemiBold;horizontalAlignment:Text.AlignHCenter;topPadding:20}
                    Text {width:parent.width;text:"打开一部喜欢的影片，剩下的时间交给故事。";color:"#8c8d96";font.pixelSize:14;horizontalAlignment:Text.AlignHCenter;topPadding:15}
                    Row {anchors.horizontalCenter:parent.horizontalCenter;topPadding:30;ActionButton {text:"前往文件管理";primary:true;onClicked:root.showFileManager()}}
                    Text {width:parent.width;text:"也可以将文件拖放到这里";color:"#62646d";font.pixelSize:12;horizontalAlignment:Text.AlignHCenter;topPadding:24}
                    Text {width:parent.width;text:"MP4   ·   MOV   ·   MKV   ·   更多格式";color:"#4e5059";font.pixelSize:10;horizontalAlignment:Text.AlignHCenter;topPadding:35;font.letterSpacing:1}
                }
                ColumnLayout {
                    anchors.fill:parent;anchors.margins:30;visible:!root.fileManagerPage && root.section!=="媒体库" && (root.selectedFolder.toString() || mediaLibrary.files.length>0);spacing:18
                    RowLayout {Layout.fillWidth:true
                        Column {spacing:6
                            Text {text:root.selectedFolderInfo ? root.selectedFolderInfo.name : "所有媒体";color:"#f4f4f6";font.pixelSize:26;font.weight:Font.DemiBold}
                            Text {text:root.selectedFolderInfo ? (root.selectedFolderInfo.scanning ? "正在扫描视频…" : !root.selectedFolderInfo.available ? "文件夹不可用，请检查磁盘连接后刷新" : root.visibleFiles.length+" 个视频 · 包含子文件夹") : root.visibleFiles.length+" 个文件 · 保存在你的设备上";color:"#787b85";font.pixelSize:12}
                        }
                        Item {Layout.fillWidth:true}
                    }
                    Label { visible: root.visibleFiles.length === 0; text: root.selectedFolderInfo && root.selectedFolderInfo.scanning ? "扫描完成后会显示视频" : "这个文件夹还没有可显示的视频"; color: "#92959f" }
                    ScrollView {
                        id: mediaScroll
                        Layout.fillHeight:true;Layout.fillWidth:true;clip:true
                        contentWidth: availableWidth
                        contentHeight: mediaFlow.implicitHeight
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        Flow {
                            id: mediaFlow; objectName: "mediaFlow"
                            // Use the viewport, never the implicit width of ScrollView's content.
                            width:mediaScroll.availableWidth;spacing:root.gridMode?20:0
                            Repeater {
                                model:root.visibleFiles
                                delegate: Item {
                                    id: mediaRow; objectName: "mediaRow"
                                    required property var modelData
                                    required property int index
                                    property bool matches: modelData.title.toLowerCase().indexOf(root.query)>=0
                                    width: root.gridMode ? Math.max(1, (mediaFlow.width-40)/3) : mediaFlow.width
                                    height: matches ? (root.gridMode?230:134) : 0;visible:matches
                                    Rectangle {anchors.fill:parent;anchors.bottomMargin:root.gridMode?0:1;radius:12;color:rowMouse.containsMouse?"#1c1e22":"transparent"}
                                    Rectangle {
                                        id:poster;x:root.gridMode?0:10;y:10;width:root.gridMode?parent.width:80;height:root.gridMode?150:110;radius:10
                                        gradient:Gradient {GradientStop {position:0;color:["#414951","#4e3d34","#333f50","#454537"][mediaRow.index%4]}GradientStop {position:1;color:"#191c22"}}
                                        Glyph {anchors.centerIn:parent;width:32;height:32;name:rowMouse.containsMouse?"play":"film";color:rowMouse.containsMouse?root.accent:"#8a909b"}
                                        Text {anchors.bottom:parent.bottom;anchors.right:parent.right;anchors.margins:8;text:modelData.extension;color:"#bbbfc8";font.pixelSize:9;font.letterSpacing:1}
                                    }
                                    Column {
                                        x:root.gridMode?2:110;y:root.gridMode?174:29;width:parent.width-x-12;spacing:10
                                        Text {width:parent.width;text:modelData.title;elide:Text.ElideRight;color:"#e9e9ed";font.pixelSize:14;font.weight:Font.DemiBold}
                                        Text {text:modelData.extension+"  ·  "+modelData.size;color:"#848793";font.pixelSize:11}
                                        Text {visible:!root.gridMode;text:modelData.folder;color:"#656975";font.pixelSize:11}
                                    }
                                    Rectangle {visible:!root.gridMode;anchors.bottom:parent.bottom;width:parent.width;height:1;color:"#24262a"}
                                    MouseArea {id:rowMouse;anchors.fill:parent;hoverEnabled:true;acceptedButtons:Qt.LeftButton | Qt.RightButton;cursorShape:Qt.PointingHandCursor;onClicked:mouse=>{if(mouse.button===Qt.RightButton)entryMenu.popup();else root.openMedia(mediaRow.modelData.url)}}
                                    ContextMenu {id:entryMenu;width:200;ContextMenuItem {text:"从媒体库移除";onTriggered:root.removeFromPlaylist(mediaRow.modelData.url)}}
                                }
                            }
                        }
                    }
                }
                ColumnLayout {
                    objectName: "fileManagerPage"
                    anchors.fill: parent; anchors.margins: 30; spacing: 18; visible: root.fileManagerPage
                    Label { text: mediaLibrary.folders.length + " 个文件夹"; color: "#9699a3"; font.pixelSize: 13 }
                    GridView {
                        id: folderSources; objectName: "folderSources"
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        cellWidth: 190; cellHeight: 250
                        model: mediaLibrary.folders
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Item {
                            id: sourceCard
                            required property var modelData
                            width: folderSources.cellWidth; height: folderSources.cellHeight
                            Rectangle {
                                x: 0; y: 0; width: parent.width - 18; height: 196; radius: 14
                                color: sourceMouse.containsMouse ? "#303135" : "#232528"
                                Glyph { anchors.centerIn: parent; width: 54; height: 54; name: "folder"; color: root.accent }
                            }
                            Text { x: 0; y: 208; width: parent.width - 20; text: modelData.name; elide: Text.ElideRight; color: "#eeeeef"; font.pixelSize: 14; font.weight: Font.DemiBold }
                            Text { x: 0; y: 230; width: parent.width - 20; text: modelData.scanning ? "正在扫描…" : !modelData.available ? "文件夹不可用" : modelData.videoCount + " 个视频"; color: "#8c8f99"; font.pixelSize: 11 }
                            MouseArea {
                                id: sourceMouse; anchors.fill: parent; anchors.rightMargin: 18; hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.RightButton; cursorShape: Qt.PointingHandCursor
                                onClicked: mouse => { if (mouse.button === Qt.RightButton) sourceMenu.popup(); else root.selectFolder(sourceCard.modelData.url) }
                            }
                            IconButton { x: parent.width - 58; y: 6; glyph: "more"; hint: "管理文件夹"; onClicked: sourceMenu.popup() }
                            FolderMenu { id: sourceMenu; folderUrl: sourceCard.modelData.url; folderName: sourceCard.modelData.name }
                            ToolTip.visible: sourceMouse.containsMouse
                            ToolTip.text: modelData.path
                            ToolTip.delay: 800
                        }
                        Column {
                            anchors.centerIn: parent; spacing: 14; visible: mediaLibrary.folders.length === 0
                            Glyph { anchors.horizontalCenter: parent.horizontalCenter; width: 58; height: 58; name: "folder"; color: root.accent }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "还没有添加文件夹"; color: "#eeeeef"; font.pixelSize: 20 }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "使用右上角的按钮打开文件或添加文件夹"; color: "#8c8f99"; font.pixelSize: 13 }
                        }
                    }
                }
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 30; spacing: 18; visible: root.section === "媒体库"
                    Text { text: "媒体库概览"; color: "#f4f4f6"; font.pixelSize: 26; font.weight: Font.DemiBold }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        Repeater {
                            model: [{label: "视频总数", value: mediaLibrary.videoCount}, {label: "已添加文件夹", value: mediaLibrary.folders.length}]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true; implicitHeight: 86; radius: 12; color: "#292a2e"
                                Column { x: 18; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                                    Text { text: modelData.value; color: root.accent; font.pixelSize: 27; font.weight: Font.DemiBold }
                                    Text { text: modelData.label; color: "#a5a7b0"; font.pixelSize: 13 }
                                }
                            }
                        }
                    }
                    Label { text: "统计已添加文件夹及子文件夹中的常见视频扩展名，重叠目录按文件去重。"; color: "#858893"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    RowLayout {
                        Text { text: "本地文件夹"; color: "#eeeeef"; font.pixelSize: 16; Layout.fillWidth: true }
                        ActionButton { text: "管理文件来源"; onClicked: root.showFileManager() }
                    }
                    ListView {
                        id: folderOverview; objectName: "folderOverview"
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 10
                        model: mediaLibrary.folders
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            id: overviewRow
                            required property var modelData
                            width: folderOverview.width; height: 80; radius: 10; color: "#25262a"
                            Button {
                                anchors.fill: parent; anchors.rightMargin: 46; leftPadding: 18; background: Item {}
                                contentItem: Column { spacing: 7
                                    Text { text: overviewRow.modelData.name + " · " + (overviewRow.modelData.scanning ? "扫描中…" : overviewRow.modelData.videoCount + " 个视频"); color: "#ededf0"; font.pixelSize: 14 }
                                    Text { width: parent.width; text: (overviewRow.modelData.available ? "" : "不可用 · ") + overviewRow.modelData.path; elide: Text.ElideMiddle; color: "#888b95"; font.pixelSize: 11 }
                                }
                                onClicked: root.selectFolder(overviewRow.modelData.url)
                            }
                            IconButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; glyph: "more"; hint: "管理文件夹"; onClicked: overviewMenu.popup() }
                            FolderMenu { id: overviewMenu; folderUrl: overviewRow.modelData.url; folderName: overviewRow.modelData.name }
                        }
                        Label { anchors.centerIn: parent; visible: mediaLibrary.folders.length === 0; text: "添加一个文件夹，开始整理你的视频"; color: "#91949e" }
                    }
                }
                DropArea {anchors.fill:parent;onDropped:drop=>{if(drop.hasUrls){mediaLibrary.AddFiles(drop.urls);drop.acceptProposedAction()}}
                    Rectangle {anchors.fill:parent;visible:parent.containsDrag;color:"#dd191919";border.width:2;border.color:root.accent;radius:16;Text{anchors.centerIn:parent;text:"松开以添加到媒体库";color:root.accent;font.pixelSize:24}}
                }
            }
        }
    }
    Item {
        id: stage; objectName: "playbackStage"; anchors.fill:parent;visible:root.playingPage
        HoverHandler {
            id: stageHover
            onHoveredChanged: {
                if (hovered) root.revealControls()
                else root.scheduleHide()
            }
        }
        onWidthChanged:{if(!root.dockMoved)root.placeDock();else dock.x=Math.max(12,Math.min(dock.x,width-dock.width-12))}
        onHeightChanged:{if(!root.dockMoved)root.placeDock();else dock.y=Math.max(62,Math.min(dock.y,height-dock.height-12))}
        Item {
            id:videoScene;anchors.fill:parent
            Rectangle {anchors.fill:parent;color:"#050506"}
            VideoItem {id:video;objectName:"filterVideo";anchors.fill:parent;Component.onCompleted:player.videoOutput=video}
            Column {anchors.centerIn:parent;spacing:18;visible:!player.hasVideo
                Glyph {anchors.horizontalCenter:parent.horizontalCenter;width:60;height:60;name:"film";color:"#41434a"}
                Text {anchors.horizontalCenter:parent.horizontalCenter;text:root.mediaTitle;color:"#8d9098";font.pixelSize:20}
            }
        }
        MouseArea {anchors.fill:parent;onDoubleClicked:root.fullScreen();onClicked:{player.TogglePlayback()}}
        Rectangle {opacity:root.controlsShown ? 1 : 0;Behavior on opacity {NumberAnimation {duration:180}}
width:parent.width;height:110;gradient:Gradient {GradientStop {position:0;color:"#b3000000"}GradientStop {position:1;color:"transparent"}}}
        RowLayout {opacity:root.controlsShown ? 1 : 0;enabled:opacity>0;Behavior on opacity {NumberAnimation {duration:180}}
x:22;y:16;width:parent.width-44;spacing:12
            IconButton {glyph:"back";hint:"返回媒体库";onClicked:root.backToLibrary()}
            Text {Layout.fillWidth:true;text:root.mediaTitle;elide:Text.ElideMiddle;color:"#d4d4d8";font.pixelSize:14;horizontalAlignment:Text.AlignHCenter}
            Text {visible:player.recording || player.savingRecording;text:player.savingRecording ? "正在保存…" : (player.playing ? "● 录制中 " : "● 录制已暂停 ")+root.time(player.recordingDuration);color:"#ff8989";font.pixelSize:13}
            ActionButton {objectName:"recordButton";text:player.recording ? "停止录制" : "录制";enabled:player.hasVideo && !player.savingRecording
                onClicked:{if(player.recording)player.StopRecording();else recordingPicker.open()}}
            ActionButton {id:filterToggle;objectName:"filterToggle";text:"滤镜";onClicked:{if(filterPanel.visible)filterPanel.close();else filterPanel.open()}}
            IconButton {glyph:"folder";hint:"打开其他文件";onClicked:filePicker.open()}
        }
        Item {
            id:dock;objectName:"playbackDock";width:Math.min(790,stage.width-64);height:126
            opacity: root.controlsShown ? 1 : 0
            visible: opacity > 0
            enabled: root.controlsShown
            Behavior on opacity { NumberAnimation { duration: 180 } }
            clip:true
            ShaderEffectSource {id:backdrop;sourceItem:videoScene;sourceRect:Qt.rect(dock.x,dock.y,dock.width,dock.height);textureSize:Qt.size(dock.width,dock.height);live:true;hideSource:false;visible:false}
            MultiEffect {anchors.fill:parent;source:backdrop;blurEnabled:true;blurMax:48;blur:0.85;autoPaddingEnabled:false;maskEnabled:true;maskSource:glassMask}
            Item {id:glassMask;width:dock.width;height:dock.height;layer.enabled:true;visible:false;Rectangle{anchors.fill:parent;radius:24;color:"white"}}
            Rectangle {anchors.fill:parent;radius:24;color:"#aa2b2c30";border.color:"#45ffffff";border.width:1}
            Rectangle {x:24;y:1;width:parent.width-48;height:1;color:"#18ffffff"}
            MouseArea {
                id:drag;anchors.fill:parent;cursorShape:pressed?Qt.ClosedHandCursor:Qt.OpenHandCursor
                drag.target:dock;drag.minimumX:12;drag.maximumX:stage.width-dock.width-12;drag.minimumY:62;drag.maximumY:stage.height-dock.height-12
                onPressed:{root.dockMoved=true;root.revealControls()}
                onReleased:root.scheduleHide()
                onCanceled:root.scheduleHide()
            }
            Text { anchors.horizontalCenter: parent.horizontalCenter; y: 14; visible: volumeNotice.running; text: "音量 " + Math.round(player.volume * 100) + "%"; color: "#eeeeee"; font.pixelSize: 11 }
            Rectangle {anchors.horizontalCenter:parent.horizontalCenter;y:9;width:32;height:3;radius:2;color:"#55ffffff"}
            RowLayout {
                x:20;y:22;height:40;spacing:6
                IconButton {glyph:player.muted?"mute":"volume";hint:"静音";onClicked:player.muted=!player.muted}
                SeekSlider {id:volumeSlider;objectName:"volumeSlider";Layout.preferredWidth:100;from:0;to:1;value:player.volume;onMoved:{player.volume=value;player.muted=false;}Accessible.name:"音量"}
            }
            RowLayout {
                id: transport; objectName: "transportControls"
                anchors.horizontalCenter:parent.horizontalCenter
                y:22;height:40;spacing:6
                IconButton {glyph:"previous";objectName:"previousTrack";hint:"上一首";enabled:root.currentIndex>0;onClicked:root.nextFile(-1)}
                IconButton {glyph:"rewind";enabled:!player.recording;hint:"后退 10 秒（←）";onClicked:root.skip(-10000)}
                IconButton {objectName:"playPause";glyph:player.playing?"pause":"play";hint:"播放 / 暂停（空格）";implicitWidth:48;onClicked:player.TogglePlayback()}
                IconButton {glyph:"forward";enabled:!player.recording;hint:"前进 10 秒（→）";onClicked:root.skip(10000)}
                IconButton {glyph:"next";objectName:"nextTrack";hint:"下一首";enabled:root.currentIndex>=0 && root.currentIndex<root.playlistFiles.length-1;onClicked:root.nextFile(1)}
            }
            RowLayout {
                anchors.right:parent.right;anchors.rightMargin:20
                y:22;height:40;spacing:6
                Button {
                    id: rateButton
                    objectName: "rateButton"
                    enabled: !player.recording
                    text: player.playbackRate + "×"
                    onClicked: rateMenu.open()
                    implicitWidth: 46
                    implicitHeight: 36
                    hoverEnabled: true
                    Menu {
                        id: rateMenu
                        objectName: "rateMenu"
                        y: -height - 8
                        width: 116
                        background: Rectangle { color: "#292a2e"; radius: 10; border.color: "#55565a" }
                        Repeater {
                            model: [0.5, 0.75, 1, 1.25, 1.5, 2]
                            delegate: MenuItem {
                                required property real modelData
                                objectName: "rateOption" + modelData
                                text: modelData + "×"
                                implicitHeight: 34
                                onTriggered: player.playbackRate = modelData
                                background: Rectangle { radius: 6; color: parent.highlighted ? "#505157" : "transparent" }
                                contentItem: Text {
                                    text: (player.playbackRate === modelData ? "✓  " : "    ") + parent.text
                                    color: "#eeeeee"
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }
                    background: Rectangle {
                        radius: 9
                        color: rateButton.hovered ? "#20ffffff" : "transparent"
                    }
                    contentItem: Text {
                        text: rateButton.text
                        color: "#eeeeee"
                        font.pixelSize: 13
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                IconButton {objectName:"playlistToggle";glyph:"list";hint:"播放列表";onClicked:playlist.open()}
                IconButton {objectName:"fullscreenToggle";glyph:"full";hint:"切换全屏";onClicked:root.fullScreen()}
            }
            RowLayout {
                x:24;y:77;width:parent.width-48;height:29;spacing:18
                Text {text:root.time(player.position);color:"#eeeeef";font.pixelSize:12;Layout.preferredWidth:48}
                PlaybackSeekSlider {
                    id:seek;objectName:"seekSlider";Layout.fillWidth:true
                    from:0;to:Math.max(1,player.duration)
                    mediaPosition:player.position
                    enabled:player.seekable && !player.recording
                    onSeekRequested:function(target){player.position=Math.round(target)}
                    Accessible.name:"播放进度"
                }
                Text {text:"−"+root.time(player.duration-player.position);color:"#eeeeef";font.pixelSize:12;Layout.preferredWidth:54;horizontalAlignment:Text.AlignRight}
            }
        }
    }
}
