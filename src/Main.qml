import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 760
    height: 560
    minimumWidth: 560
    minimumHeight: 420
    visible: true
    title: "MKV Clean"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool settingsVisible: false
    property bool quitConfirmVisible: false
    readonly property bool busy: backend.state === "inspecting" || backend.state === "extracting" || backend.state === "remuxing"
    readonly property string viewName: {
        if (backend.scanning)
            return "working"
        if (backend.batch && win.busy)
            return "batch"
        switch (backend.state) {
        case "idle":
        case "inspecting":
            return "idle"
        case "ready":
            return "ready"
        case "review":
            return "review"
        case "extracting":
        case "remuxing":
            return "working"
        case "done":
            return "done"
        default:
            return "error"
        }
    }

    function primaryAction() {
        if (backend.state === "idle")
            backend.openFiles()
        else if (backend.state === "ready")
            backend.clean()
        else if (backend.state === "review")
            backend.startBatch()
        else if (backend.state === "done" || backend.state === "error")
            backend.reset()
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; onActivated: win.settingsVisible = !win.settingsVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: {
        if (win.helpVisible || win.quitConfirmVisible || win.settingsVisible) return
        if (win.busy) win.quitConfirmVisible = true
        else Qt.quit()
    } }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; onActivated: { if (!win.busy) backend.openFiles() } }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; onActivated: { if (!win.busy) backend.openFolder() } }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: {
        if (!win.busy && !win.quitConfirmVisible && !win.helpVisible && !win.settingsVisible) win.primaryAction()
    } }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: {
        if (win.quitConfirmVisible) win.quitConfirmVisible = false
        else if (win.settingsVisible) win.settingsVisible = false
        else if (win.helpVisible) win.helpVisible = false
        else if (win.busy) backend.cancel()
    } }
    Shortcut { sequence: "Y"; context: Qt.ApplicationShortcut; enabled: win.quitConfirmVisible; onActivated: Qt.quit() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.quitConfirmVisible; onActivated: win.quitConfirmVisible = false }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        DropArea {
            id: dropArea
            anchors.fill: parent
            keys: ["text/uri-list"]
            onDropped: drop => {
                if (!win.busy && drop.urls.length > 0) {
                    const paths = []
                    for (const url of drop.urls)
                        paths.push(url.toString())
                    backend.openPaths(paths)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "MKV Clean"
                        font.pixelSize: 22
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        visible: text !== "" && !backend.batch && !backend.scanning
                        text: backend.sourceName
                        color: "#9a9aa2"
                        elide: Label.ElideMiddle
                        Layout.maximumWidth: 400
                    }
                    Label {
                        text: "⚙"
                        font.pixelSize: 20
                        color: gearMouse.containsMouse ? theme.accent : "#9a9aa2"
                        MouseArea {
                            id: gearMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: win.settingsVisible = true
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: ["idle", "ready", "review", "batch", "working", "done", "error"].indexOf(win.viewName)

                    // --- Idle / inspecting: the drop target -------------------
                    Rectangle {
                        radius: theme.radius
                        color: "transparent"
                        border.width: 2
                        border.color: dropArea.containsDrag ? theme.accent : "#2c2c34"

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 14

                            BusyIndicator {
                                Layout.alignment: Qt.AlignHCenter
                                visible: backend.state === "inspecting"
                                running: visible
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: backend.state === "inspecting" ? backend.statusText : qsTr("Drop MKV files here")
                                font.pixelSize: 16
                                color: backend.state === "inspecting" ? "#d8d8de" : "#9a9aa2"
                            }
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 10
                                visible: backend.state === "idle"
                                Button {
                                    text: qsTr("Open files…")
                                    Material.background: theme.accent
                                    Material.foreground: theme.accentForeground
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.openFiles()
                                }
                                Button {
                                    text: qsTr("Open folder…")
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.openFolder()
                                }
                            }
                        }
                    }

                    // --- Ready: the track plan for one file -------------------
                    ColumnLayout {
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            visible: backend.extractCount > 0
                            text: qsTr("%n subtitle track will be extracted first.", "", backend.extractCount)
                            color: "#c8c8d0"
                            wrapMode: Label.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: backend.removedCount > 0
                            text: qsTr("%n other track will be removed.", "", backend.removedCount)
                            color: "#c8c8d0"
                            wrapMode: Label.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("The original file will be replaced — it's moved to the trash if possible.")
                            color: "#d8a050"
                            wrapMode: Label.Wrap
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 6
                            model: backend.tracks

                            delegate: Rectangle {
                                required property var modelData
                                width: ListView.view.width
                                height: trackRow.implicitHeight + 16
                                radius: Math.min(4, theme.radius)
                                color: "#16161b"

                                RowLayout {
                                    id: trackRow
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 10

                                    Label {
                                        Layout.preferredWidth: 78
                                        text: modelData.type === "video" ? qsTr("Video")
                                            : modelData.type === "audio" ? qsTr("Audio") : qsTr("Subtitles")
                                        font.pixelSize: 12
                                        font.bold: true
                                        color: "#9a9aa2"
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: [modelData.codec, modelData.language, modelData.name].filter(part => part !== "").join(" · ")
                                        elide: Label.ElideRight
                                        color: "#d8d8de"
                                    }
                                    Label {
                                        visible: modelData.isDefault
                                        text: qsTr("default")
                                        font.pixelSize: 12
                                        color: "#9a9aa2"
                                    }
                                    Label {
                                        visible: modelData.isForced
                                        text: qsTr("forced")
                                        font.pixelSize: 12
                                        color: "#9a9aa2"
                                    }
                                    Label {
                                        visible: modelData.isSdh
                                        text: "sdh"
                                        font.pixelSize: 12
                                        color: "#9a9aa2"
                                    }
                                    Rectangle {
                                        radius: Math.min(4, theme.radius)
                                        implicitWidth: chipLabel.implicitWidth + 16
                                        implicitHeight: chipLabel.implicitHeight + 6
                                        color: modelData.action === "keep" ? theme.accent
                                             : modelData.action === "extract" ? "#2a2a33" : "#332024"
                                        Label {
                                            id: chipLabel
                                            anchors.centerIn: parent
                                            font.pixelSize: 12
                                            font.bold: modelData.action === "keep"
                                            color: modelData.action === "keep" ? theme.accentForeground
                                                 : modelData.action === "extract" ? "#e8e8ee" : "#d08080"
                                            text: modelData.action === "keep" ? qsTr("Keep")
                                                : modelData.action === "extract" ? qsTr("Extract") : qsTr("Remove")
                                        }
                                    }
                                }
                            }
                        }

                        Button {
                            Layout.alignment: Qt.AlignRight
                            text: qsTr("Extract and clean")
                            Material.background: theme.accent
                            Material.foreground: theme.accentForeground
                            Material.roundedScale: theme.radius
                            onClicked: backend.clean()
                        }
                    }

                    // --- Review: trim the list before cleaning ----------------
                    ColumnLayout {
                        spacing: 12

                        Label {
                            text: qsTr("%n file to clean", "", backend.queueCount)
                            font.pixelSize: 16
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Remove files from the list to leave them untouched.")
                            color: "#9a9aa2"
                            font.pixelSize: 12
                            wrapMode: Label.Wrap
                        }

                        ListView {
                            id: reviewList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 6
                            model: backend.queue
                            property int expandedIndex: -1
                            // Removal re-notifies the queue; without this the list
                            // can snap back to the top while the user trims it.
                            property real pendingRestorePosition: -1.0

                            function requestRemove(index) {
                                reviewList.pendingRestorePosition = reviewList.contentY
                                backend.removeFromQueue(index)
                                Qt.callLater(function () {
                                    if (reviewList.pendingRestorePosition > -1) {
                                        reviewList.contentY = reviewList.pendingRestorePosition
                                        reviewList.pendingRestorePosition = -1.0
                                    }
                                })
                            }

                            Connections {
                                target: backend
                                function onStateChanged() { reviewList.expandedIndex = -1 }
                            }

                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                property bool expanded: index === reviewList.expandedIndex
                                width: ListView.view.width
                                // One deterministic height: title plus, when
                                // expanded, the fixed-size details box. The
                                // delegate's clip produces the slide.
                                height: 16 + titleItem.implicitHeight +
                                        (expanded ? 8 + detailsItem.height : 0)
                                radius: Math.min(4, theme.radius)
                                color: "#16161b"
                                clip: true

                                Behavior on height {
                                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                                }

                                Item {
                                    id: titleItem
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 8
                                    implicitHeight: titleRow.implicitHeight

                                    RowLayout {
                                        id: titleRow
                                        anchors.fill: parent
                                        spacing: 10

                                        Label {
                                            text: reviewList.expandedIndex === index ? "▾" : "▸"
                                            font.pixelSize: 12
                                            color: "#9a9aa2"
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            elide: Label.ElideMiddle
                                            color: "#d8d8de"
                                        }
                                        Label {
                                            text: "✕"
                                            font.pixelSize: 13
                                            color: reviewRemoveMouse.containsMouse ? "#d08080" : "#9a9aa2"
                                            MouseArea {
                                                id: reviewRemoveMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: reviewList.requestRemove(index)
                                            }
                                        }
                                    }

                                    MouseArea {
                                        // Under the row's controls: clicks anywhere
                                        // except the ✕ toggle the details.
                                        anchors.fill: parent
                                        z: -1
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: reviewList.expandedIndex =
                                            reviewList.expandedIndex === index ? -1 : index
                                    }
                                }

                                Item {
                                    id: detailsItem
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: titleItem.bottom
                                    anchors.topMargin: 8
                                    height: detailsBox.height

                                    Rectangle {
                                        id: detailsBox
                                        width: parent.width
                                        height: tracksColumn.implicitHeight + 16
                                        radius: Math.min(4, theme.radius)
                                        color: "#101014"
                                        border.color: "#2c2c34"

                                        ColumnLayout {
                                            id: tracksColumn
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.top: parent.top
                                            anchors.margins: 8
                                            spacing: 4

                                            Repeater {
                                                model: modelData.tracks

                                                delegate: RowLayout {
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    spacing: 8

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: (modelData.type === "video" ? qsTr("Video")
                                                             : modelData.type === "audio" ? qsTr("Audio")
                                                             : qsTr("Subtitles")) + " · " +
                                                              [modelData.codec, modelData.language, modelData.name]
                                                                  .filter(part => part !== "").join(" · ") +
                                                              (modelData.isForced ? " · " + qsTr("forced")
                                                               : modelData.isSdh ? " · sdh" : "")
                                                        elide: Label.ElideRight
                                                        font.pixelSize: 12
                                                        color: "#c8c8d0"
                                                    }
                                                    Label {
                                                        text: modelData.action === "keep" ? qsTr("Keep")
                                                            : modelData.action === "extract" ? qsTr("Extract")
                                                            : qsTr("Remove")
                                                        font.pixelSize: 12
                                                        font.bold: modelData.action === "keep"
                                                        color: modelData.action === "keep" ? theme.accent
                                                             : modelData.action === "extract" ? "#e8e8ee" : "#d08080"
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        RowLayout {
                            spacing: 10
                            Button {
                                text: qsTr("Discard")
                                Material.roundedScale: theme.radius
                                onClicked: backend.reset()
                            }
                            Item { Layout.fillWidth: true }
                            Button {
                                text: qsTr("Clean %n files", "", backend.queueCount)
                                Material.background: theme.accent
                                Material.foreground: theme.accentForeground
                                Material.roundedScale: theme.radius
                                onClicked: backend.startBatch()
                            }
                        }
                    }

                    // --- Batch: the queue chewing through files ---------------
                    ColumnLayout {
                        spacing: 12

                        Label {
                            text: qsTr("File %1 of %2").arg(backend.queueIndex + 1).arg(backend.queueCount)
                            font.pixelSize: 16
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            value: (backend.queueIndex + backend.progress / 100) / backend.queueCount
                        }

                        ListView {
                            id: batchList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 6
                            model: backend.queue

                            // Removing a queued row must not yank the user back
                            // to the top of the list.
                            property real pendingRestorePosition: -1.0

                            function requestRemove(index) {
                                batchList.pendingRestorePosition = batchList.contentY
                                backend.removeFromQueue(index)
                                Qt.callLater(function () {
                                    if (batchList.pendingRestorePosition > -1) {
                                        batchList.contentY = batchList.pendingRestorePosition
                                        batchList.pendingRestorePosition = -1.0
                                    }
                                })
                            }

                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                width: ListView.view.width
                                height: queueRow.implicitHeight + 16
                                radius: Math.min(4, theme.radius)
                                color: "#16161b"

                                RowLayout {
                                    id: queueRow
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 10

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        elide: Label.ElideMiddle
                                        color: modelData.state === "pending" ? "#8a8a92" : "#d8d8de"
                                    }
                                    Label {
                                        visible: modelData.state === "pending"
                                        text: "✕"
                                        font.pixelSize: 13
                                        color: queueRemoveMouse.containsMouse ? "#d08080" : "#6a6a72"
                                        MouseArea {
                                            id: queueRemoveMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: batchList.requestRemove(index)
                                        }
                                    }
                                    Rectangle {
                                        radius: Math.min(4, theme.radius)
                                        implicitWidth: queueChipLabel.implicitWidth + 16
                                        implicitHeight: queueChipLabel.implicitHeight + 6
                                        color: modelData.state === "done" ? theme.accent
                                             : modelData.state === "active" ? "#2a2a33"
                                             : modelData.state === "error" ? "#332024" : "#1c1c22"
                                        Label {
                                            id: queueChipLabel
                                            anchors.centerIn: parent
                                            font.pixelSize: 12
                                            font.bold: modelData.state === "done"
                                            color: modelData.state === "done" ? theme.accentForeground
                                                 : modelData.state === "active" ? "#e8e8ee"
                                                 : modelData.state === "error" ? "#d08080" : "#8a8a92"
                                            text: modelData.state === "active" ? qsTr("Cleaning…")
                                                : modelData.state === "done" ? qsTr("Done")
                                                : modelData.state === "error" ? qsTr("Failed") : qsTr("Queued")
                                        }
                                    }
                                }
                            }
                        }

                        Button {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Cancel")
                            Material.roundedScale: theme.radius
                            onClicked: backend.cancel()
                        }
                    }

                    // --- Working: one file, extraction then the remux ---------
                    Item {
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(560, parent.width - 60)
                            spacing: 14

                            BusyIndicator {
                                Layout.alignment: Qt.AlignHCenter
                                visible: backend.state === "extracting"
                                running: visible
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: backend.statusText
                                font.pixelSize: 16
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                visible: backend.scanning
                                text: qsTr("File %1 of %2").arg(backend.scanIndex).arg(backend.scanTotal)
                                color: "#c8c8d0"
                            }
                            Label {
                                id: scanFileLabel
                                // A fixed box, refreshed once a second: it shows
                                // the scan is alive without the text strobing.
                                Layout.fillWidth: true
                                visible: backend.scanning
                                elide: Label.ElideRight
                                horizontalAlignment: Label.AlignHCenter
                                color: "#9a9aa2"
                                property string currentName: ""
                                text: currentName
                                Timer {
                                    interval: 1000
                                    running: backend.scanning
                                    repeat: true
                                    onRunningChanged: if (running) scanFileLabel.currentName = backend.sourceName
                                    onTriggered: scanFileLabel.currentName = backend.sourceName
                                }
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                indeterminate: backend.state === "extracting"
                                value: backend.progress / 100
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                visible: backend.state === "remuxing"
                                text: backend.progress + "%"
                                color: "#9a9aa2"
                            }
                            Button {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("Cancel")
                                Material.roundedScale: theme.radius
                                onClicked: backend.cancel()
                            }
                        }
                    }

                    // --- Done: what was written -------------------------------
                    Item {
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(640, parent.width - 60)
                            spacing: 10

                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "✓"
                            font.pixelSize: 40
                            color: theme.accent
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: backend.scanFoundNothing ? qsTr("Everything is already clean.")
                                : backend.batchAborted ? qsTr("Stopped early.") : qsTr("Done")
                            font.pixelSize: 20
                            font.bold: true
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            visible: backend.batch
                            text: qsTr("%n file cleaned", "", backend.queueCleanedCount)
                            color: "#c8c8d0"
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            visible: backend.batch && backend.queueFailedCount > 0
                            text: qsTr("%n file failed", "", backend.queueFailedCount)
                            color: "#d08080"
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            visible: !backend.batch && backend.extractedFiles.length > 0
                            text: qsTr("%n subtitle file extracted", "", backend.extractedFiles.length)
                            color: "#c8c8d0"
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.fillWidth: true
                            visible: !backend.batch && backend.outputPath !== ""
                            text: qsTr("Saved as:") + " " + backend.outputPath
                            color: "#9a9aa2"
                            elide: Label.ElideMiddle
                            horizontalAlignment: Label.AlignHCenter
                        }
                        Repeater {
                            model: backend.batch ? backend.queue : []
                            delegate: Label {
                                required property var modelData
                                Layout.alignment: Qt.AlignHCenter
                                visible: modelData.state === "error"
                                height: visible ? implicitHeight : 0
                                text: modelData.name + " — " + modelData.error
                                color: "#9a9aa2"
                                font.pixelSize: 12
                            }
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 10
                            Button {
                                visible: !backend.batch
                                text: qsTr("Open folder")
                                Material.roundedScale: theme.radius
                                onClicked: backend.openOutputFolder()
                            }
                            Button {
                                text: backend.batch ? qsTr("Clean more files") : qsTr("Clean another file")
                                Material.background: theme.accent
                                Material.foreground: theme.accentForeground
                                Material.roundedScale: theme.radius
                                onClicked: backend.reset()
                            }
                        }
                        }
                    }

                    // --- Error: say what's wrong ------------------------------
                    Item {
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(560, parent.width - 60)
                            spacing: 14

                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: "⚠"
                            font.pixelSize: 36
                            color: "#d08080"
                        }
                        Label {
                            Layout.fillWidth: true
                            text: backend.errorText
                            wrapMode: Label.Wrap
                            horizontalAlignment: Label.AlignHCenter
                        }
                        Button {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Open files…")
                            Material.background: theme.accent
                            Material.foreground: theme.accentForeground
                            Material.roundedScale: theme.radius
                            onClicked: backend.openFiles()
                        }
                        }
                    }
                }
            }
        }

        // --- Overlays -------------------------------------------------------

        Rectangle {
            anchors.fill: parent
            visible: win.settingsVisible
            color: "#cc000000"
            onVisibleChanged: { if (visible) backend.refreshServiceStatus() }

            MouseArea {
                anchors.fill: parent
                onClicked: win.settingsVisible = false
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(560, parent.width - 60)
                height: Math.min(settingsColumn.implicitHeight + 40, parent.height - 60)
                radius: theme.radius
                color: "#16161b"
                border.color: "#2c2c34"

                MouseArea { anchors.fill: parent } // keep clicks off the dimmer

                ColumnLayout {
                    id: settingsColumn
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Settings")
                            font.pixelSize: 16
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: "✕"
                            font.pixelSize: 14
                            color: settingsCloseMouse.containsMouse ? theme.accent : "#9a9aa2"
                            MouseArea {
                                id: settingsCloseMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: win.settingsVisible = false
                            }
                        }
                    }

                    Label {
                        text: qsTr("Watched folders")
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The watch service cleans new files that appear in these folders.")
                        color: "#9a9aa2"
                        font.pixelSize: 12
                        wrapMode: Label.Wrap
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 140)
                        clip: true
                        spacing: 4
                        model: settings.watchFolders
                        delegate: RowLayout {
                            required property var modelData
                            width: ListView.view.width
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: modelData
                                elide: Label.ElideMiddle
                                color: "#d8d8de"
                            }
                            Label {
                                text: "✕"
                                font.pixelSize: 12
                                color: removeMouse.containsMouse ? "#d08080" : "#9a9aa2"
                                MouseArea {
                                    id: removeMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: settings.removeWatchFolder(modelData)
                                }
                            }
                        }
                    }

                    RowLayout {
                        spacing: 10
                        Button {
                            text: "+ " + qsTr("Add folder")
                            Material.background: theme.accent
                            Material.foreground: theme.accentForeground
                            Material.roundedScale: theme.radius
                            onClicked: backend.pickWatchFolder()
                        }
                        Button {
                            text: qsTr("Scan now")
                            Material.roundedScale: theme.radius
                            enabled: !win.busy && settings.watchFolders.length > 0
                            onClicked: {
                                win.settingsVisible = false
                                backend.scanFolders(settings.watchFolders)
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        text: qsTr("Excluded folders")
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Folders inside these are never watched or scanned.")
                        color: "#9a9aa2"
                        font.pixelSize: 12
                        wrapMode: Label.Wrap
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 100)
                        clip: true
                        spacing: 4
                        model: settings.excludedFolders
                        delegate: RowLayout {
                            required property var modelData
                            width: ListView.view.width
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: modelData
                                elide: Label.ElideMiddle
                                color: "#d8d8de"
                            }
                            Label {
                                text: "✕"
                                font.pixelSize: 12
                                color: excludeRemoveMouse.containsMouse ? "#d08080" : "#9a9aa2"
                                MouseArea {
                                    id: excludeRemoveMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: settings.removeExcludedFolder(modelData)
                                }
                            }
                        }
                    }

                    Button {
                        text: "+ " + qsTr("Exclude folder")
                        Material.roundedScale: theme.radius
                        onClicked: backend.pickExcludeFolder()
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#2c2c34" }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 8

                        Label { text: qsTr("Audio language") }
                        ComboBox {
                            Layout.fillWidth: true
                            model: settings.languageModel
                            textRole: "name"
                            valueRole: "code"
                            currentIndex: {
                                for (let i = 0; i < settings.languageModel.length; ++i)
                                    if (settings.languageModel[i].code === settings.audioLanguage)
                                        return i
                                return 0
                            }
                            onActivated: settings.setAudioLanguage(currentValue)
                        }

                        Label { text: qsTr("Subtitle language") }
                        ComboBox {
                            Layout.fillWidth: true
                            model: settings.languageModel
                            textRole: "name"
                            valueRole: "code"
                            currentIndex: {
                                for (let i = 0; i < settings.languageModel.length; ++i)
                                    if (settings.languageModel[i].code === settings.subtitleLanguage)
                                        return i
                                return 0
                            }
                            onActivated: settings.setSubtitleLanguage(currentValue)
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#2c2c34" }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Label {
                            text: qsTr("Watch service")
                            font.bold: true
                        }
                        Label {
                            text: backend.watchServiceActive ? qsTr("running") : qsTr("stopped")
                            color: backend.watchServiceActive ? theme.accent : "#9a9aa2"
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: backend.watchServiceActive ? qsTr("Disable") : qsTr("Enable")
                            Material.roundedScale: theme.radius
                            onClicked: backend.setWatchServiceEnabled(!backend.watchServiceActive)
                        }
                    }
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: "#cc000000"

            MouseArea {
                anchors.fill: parent
                onClicked: win.helpVisible = false
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(440, parent.width - 60)
                height: helpColumn.implicitHeight + 40
                radius: theme.radius
                color: "#16161b"
                border.color: "#2c2c34"

                MouseArea { anchors.fill: parent } // keep clicks off the dimmer

                ColumnLayout {
                    id: helpColumn
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 12

                    Label {
                        text: qsTr("Keyboard shortcuts")
                        font.pixelSize: 16
                        font.bold: true
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 24
                        rowSpacing: 8

                        Label { text: "Space"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Extract and clean / continue"); wrapMode: Label.Wrap }
                        Label { text: "O"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Open files"); wrapMode: Label.Wrap }
                        Label { text: "F"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Open folder"); wrapMode: Label.Wrap }
                        Label { text: "S"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Settings"); wrapMode: Label.Wrap }
                        Label { text: "Esc"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Cancel"); wrapMode: Label.Wrap }
                        Label { text: "?"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Keyboard shortcuts"); wrapMode: Label.Wrap }
                        Label { text: "Q"; font.bold: true; color: theme.accent }
                        Label { Layout.fillWidth: true; text: qsTr("Quit"); wrapMode: Label.Wrap }
                    }
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: win.quitConfirmVisible
            color: "#cc000000"

            MouseArea { anchors.fill: parent } // modal: clicks go nowhere

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(440, parent.width - 60)
                height: quitColumn.implicitHeight + 40
                radius: theme.radius
                color: "#16161b"
                border.color: "#2c2c34"

                ColumnLayout {
                    id: quitColumn
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Cancel the current job and quit?")
                        wrapMode: Label.Wrap
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: 10
                        Label {
                            text: "Y / N"
                            color: "#9a9aa2"
                            font.pixelSize: 12
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: qsTr("No")
                            Material.roundedScale: theme.radius
                            onClicked: win.quitConfirmVisible = false
                        }
                        Button {
                            text: qsTr("Yes")
                            Material.background: theme.accent
                            Material.foreground: theme.accentForeground
                            Material.roundedScale: theme.radius
                            onClicked: Qt.quit()
                        }
                    }
                }
            }
        }
    }
}
