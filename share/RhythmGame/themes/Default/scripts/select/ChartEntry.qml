pragma ValueTypeBehavior: Addressable
import QtQuick
import QtQuick.Layouts
import RhythmGameQml
import "../common/helpers.js" as Helpers

Image {
    id: image

    property string clearType: Helpers.getClearType(scores)
    required property var scores
    property var bestStats: Helpers.getBestStats(scores)
    property var scoreWithBestPoints: Helpers.getScoreWithBestPoints(scores)
    property var scoreWithBestClear: Helpers.getScoreWithBestClear(scores)
    property bool scrollingText: false
    property bool isCurrentItem: false
    readonly property bool isCourse: modelData instanceof course
    readonly property bool isMissing: modelData instanceof entry
    readonly property bool arenaSeated: Rg.arenaSession.state === ArenaSession.InRoom
        || Rg.arenaSession.state === ArenaSession.Reconnecting
    readonly property int arenaAvailability: {
        if (!image.arenaSeated || !(modelData instanceof ChartData)) {
            return ArenaAvailabilityIndex.NotApplicable;
        }
        const availability = Rg.arenaSession.availability;
        const revision = availability.revision;
        return revision >= 0
            ? availability.availabilityFor(modelData.sha256 || "")
            : ArenaAvailabilityIndex.NotApplicable;
    }
    readonly property bool arenaSyncing: arenaAvailability === ArenaAvailabilityIndex.Syncing
    readonly property bool arenaUnavailable: arenaAvailability === ArenaAvailabilityIndex.UnavailableToSome

    asynchronous: true
    source: root.iniImagesUrl + "folders.png/" + (isCourse ? "folder_pink" : "white")

    Image {
        id: clearImage

        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.top: parent.top
        anchors.topMargin: 9
        asynchronous: true
        source: root.iniImagesUrl + "parts.png/C_" + image.clearType
    }
    TextureText {
        id: playlevelText
        visible: !image.isCourse

        function getDiffColorInt(diff) {
            switch (diff) {
                case 1:
                    return "green";
                case 2:
                    return "blue";
                case 3:
                    return "orange";
                case 4:
                    return "red";
                default:
                    return "purple";
            }
        }

        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: -270
        number: Math.min(modelData.playLevel || 0, 99)
        srcBeforeDecimal: root.iniImagesUrl + "parts.png/s_" + getDiffColorInt(modelData.difficulty) + "_"
    }
    Image {
        visible: modelData.lnCount > 0 || modelData.bssCount > 0
        source: root.iniImagesUrl + "folders.png/ln"
        asynchronous: true
        anchors.horizontalCenter: parent.left
        anchors.horizontalCenterOffset: 95
        anchors.verticalCenter: playlevelText.verticalCenter
    }
    NameLabel {
        anchors.right: parent.right
        anchors.rightMargin: 30
        color: {
            if (image.arenaUnavailable) {
                return "red";
            }
            if (image.arenaSyncing) {
                return "dimgray";
            }
            if (image.arenaSeated && image.isCourse) {
                return "dimgray";
            }
            return (modelData instanceof ChartData || modelData instanceof course)
                ? "black"
                : "red";
        }
        height: parent.height
        scrolling: image.isCurrentItem && image.scrollingText
        fontFile: root.themeVars.songListFont
        // newlines replaced with spaces
        text: ((modelData.title || modelData.name || "") + (modelData.subtitle ? (" " + modelData.subtitle) : "")).replace(/\r\n|\n|\r/g, " ")
        width: parent.width * 0.7
    }
    MouseArea {
        anchors.fill: parent

        onClicked: {
            pathView.forceActiveFocus();
            pathView.setNavigationImmediate(index);
            // Attempting to open a missing table song starts its download,
            // mirroring endlessdream's in-game downloader.
            if (image.isMissing) {
                Rg.songDownloader.submitMd5(modelData.md5, modelData.title || "");
                return;
            }
            Qt.callLater(() => pathView.controller.goForward(modelData));
        }
    }
    Rectangle {
        id: downloadChip

        visible: image.isMissing
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: downloadLabel.implicitWidth + 20
        height: 36
        radius: 6
        color: palette.highlight

        Text {
            id: downloadLabel

            anchors.centerIn: parent
            text: qsTr("Download")
            color: palette.highlightedText
            font.pixelSize: 14
        }
        MouseArea {
            anchors.fill: parent
            onClicked: (mouse) => {
                mouse.accepted = true;
                pathView.forceActiveFocus();
                pathView.setNavigationImmediate(index);
                Rg.songDownloader.submitMd5(modelData.md5, modelData.title || "");
            }
        }
    }
    }
}
