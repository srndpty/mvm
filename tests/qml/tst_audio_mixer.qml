import QtQuick
import QtQuick.Controls
import QtTest
import "../../apps/mvm" as App

TestCase {
    id: test
    name: "AudioMixer"
    visible: true
    when: windowShown
    width: 500; height: 400
    property real lastValue: 0
    property bool committed: false
    Component { id: panComponent; App.AudioPanKnob { onValueEdited: (value, commit) => { test.lastValue = value; test.committed = commit; } } }
    Component { id: faderComponent; App.AudioFader { width: 43; height: 240; onValueEdited: (value, commit) => { test.lastValue = value; test.committed = commit; } } }
    Component { id: numberComponent; App.AudioValueField { width: 70; onValueEdited: value => test.lastValue = value } }
    Component { id: meterComponent; App.AudioMeter { width: 57; height: 240; onClipCleared: clipped = false } }
    QtObject {
        id: controllerFake
        property string mixerName: "音声1"
        property real masterVolume: 1
        property real audioMeterDbLeft: -96
        property real audioMeterDbRight: -96
        property bool audioMeterClipped: false
        property var audioTrackModel: []
        function audioTrackMeter(index) { return {left: -96, right: -96, clipped: false}; }
        function setAudioMixerName(index, name) {
            if (name.trim() === "") return false;
            mixerName = name.trim(); return true;
        }
    }
    Component {
        id: mixerComponent
        App.AudioMixerPanel { width: 70; height: 350; mvmController: controllerFake }
    }
    Component {
        id: implicitMixerComponent
        App.AudioMixerPanel { width: 180; mvmController: controllerFake }
    }
    Component {
        id: gestureComponent
        ScrollView {
            width: 180; height: 300
            contentWidth: 700; contentHeight: 300
            Row {
                width: 700; height: 300
                App.AudioPanKnob {
                    id: livePan
                    value: 0.6
                    onValueEdited: (next, commit) => { value = next; test.lastValue = next; test.committed = commit; }
                    onEditCanceled: { value = 0.6; test.lastValue = 0.6; }
                }
                App.AudioFader {
                    width: 43; height: 240; value: -12
                    onValueEdited: (next, commit) => { value = next; test.lastValue = next; test.committed = commit; }
                    onEditCanceled: { value = -12; test.lastValue = -12; }
                }
            }
        }
    }
    Component {
        id: stripComponent
        App.AudioMixerStrip {
            width: 111; height: 330
            mvmController: controllerFake
            trackIndex: 0
            trackName: "A1"
            mixerName: controllerFake.mixerName
        }
    }
    function test_mixerNameCommitCancelAndBinding() {
        controllerFake.mixerName = "音声1";
        const strip = createTemporaryObject(stripComponent, test);
        verify(strip);
        const editor = findChild(strip, "audioMixerName0");
        mouseClick(editor, 30, 12);
        editor.selectAll(); keyClick(Qt.Key_A); keyClick(Qt.Key_Return);
        compare(controllerFake.mixerName, "a");
        editor.selectAll(); keyClick(Qt.Key_Backspace); keyClick(Qt.Key_Return);
        compare(controllerFake.mixerName, "a"); compare(editor.text, "a");
        editor.selectAll(); keyClick(Qt.Key_B); keyClick(Qt.Key_Escape);
        compare(controllerFake.mixerName, "a"); compare(editor.text, "a");
        controllerFake.mixerName = "戻した名前";
        compare(editor.text, "戻した名前"); compare(strip.trackName, "A1");
    }
    function init() { lastValue = 0; committed = false; }
    function test_mixerImplicitHeightAndResize() {
        failOnWarning(/.*Binding loop detected.*/);
        const mixer = createTemporaryObject(implicitMixerComponent, test);
        verify(mixer); wait(50);
        compare(mixer.height, 295);
        compare(mixer.contentHeight, 295);
        mixer.height = 420; wait(50);
        compare(mixer.contentHeight, mixer.availableHeight);
        mixer.height = 150; wait(50);
        compare(mixer.contentHeight, 295);
    }
    function test_panResetPersists() {
        const view = createTemporaryObject(gestureComponent, test);
        verify(view);
        const pan = findChild(view, "audioPanDrag");
        mouseDoubleClickSequence(pan, 30, 30);
        wait(50); compare(lastValue, 0); verify(committed);
    }
    function test_panDragOutsideScrollView() {
        const view = createTemporaryObject(gestureComponent, test);
        verify(view);
        const pan = findChild(view, "audioPanDrag");
        mouseDrag(pan, 30, 30, 110, 0, Qt.LeftButton, Qt.NoModifier);
        wait(50); compare(lastValue, 1); verify(committed);
        compare(view.contentItem.contentX, 0);
    }
    function test_faderResetPersists() {
        const view = createTemporaryObject(gestureComponent, test);
        verify(view);
        const fader = findChild(view, "audioFaderDrag");
        mouseDoubleClickSequence(fader, 32, 100);
        wait(50); compare(lastValue, 0); verify(committed);
    }
    function test_faderDragOutsideScrollView() {
        const view = createTemporaryObject(gestureComponent, test);
        verify(view);
        const fader = findChild(view, "audioFaderDrag");
        mouseDrag(fader, 32, 100, 0, 170, Qt.LeftButton, Qt.NoModifier);
        wait(50); verify(lastValue < -48); verify(committed);
    }
    function test_wheelScrollsHorizontally() {
        const mixer = createTemporaryObject(mixerComponent, test);
        verify(mixer); wait(50);
        verify(mixer.contentWidth > mixer.availableWidth);
        mouseWheel(mixer, 35, 150, 0, -120, Qt.NoButton);
        tryVerify(() => mixer.contentItem.contentX > 0);
        compare(mixer.contentItem.contentY, 0);
        mouseWheel(mixer, 35, 150, 0, 120, Qt.NoButton);
        tryCompare(mixer.contentItem, "contentX", 0);
        mixer.width = 400; wait(50);
        mouseWheel(mixer, 35, 150, 0, -120, Qt.NoButton);
        compare(mixer.contentItem.contentX, 0);
    }
    function test_panDragAndCtrl() {
        const knob = createTemporaryObject(panComponent, test, {value: 0});
        verify(knob);
        const area = findChild(knob, "audioPanDrag");
        mouseDrag(area, 30, 30, 20, 0, Qt.LeftButton, Qt.NoModifier);
        verify(lastValue > 0.1); verify(committed);
        const normal = lastValue;
        mouseDrag(area, 30, 30, 20, 0, Qt.LeftButton, Qt.ControlModifier);
        verify(lastValue > 0 && lastValue < normal / 2);
        mouseDrag(area, 30, 40, 0, -20, Qt.LeftButton, Qt.NoModifier);
        verify(lastValue > 0.1);
        mouseDoubleClickSequence(area, 30, 30);
        compare(lastValue, 0);
    }
    function test_faderDragCtrlAndReset() {
        const fader = createTemporaryObject(faderComponent, test, {value: 0});
        verify(fader);
        compare(fader.dbFor(1), -96); compare(fader.dbFor(0), 15);
        compare(fader.positionFor(0), 0.25);
        const area = findChild(fader, "audioFaderDrag");
        mouseDrag(area, 32, 100, 0, 30, Qt.LeftButton, Qt.NoModifier);
        verify(lastValue < -5); verify(committed);
        const normal = lastValue;
        mouseDrag(area, 32, 100, 0, 30, Qt.LeftButton, Qt.ControlModifier);
        verify(lastValue < 0 && lastValue > normal / 2);
        mouseDoubleClickSequence(area, 32, 100);
        compare(lastValue, 0);
    }
    function test_directNumberAndInvalid() {
        const field = createTemporaryObject(numberComponent, test, {value: 0});
        verify(field);
        mouseClick(field, 25, 12);
        const editor = findChild(field, "audioValueEditor");
        verify(editor.visible);
        editor.text = "-inf"; field.finishEditing(); compare(lastValue, -96);
        field.beginEditing(); editor.text = "16"; field.finishEditing(); verify(editor.visible); compare(lastValue, -96);
        editor.text = "4foo"; field.finishEditing(); verify(editor.visible); compare(lastValue, -96);
        editor.text = "3.5"; field.finishEditing(); compare(lastValue, 3.5); verify(!editor.visible);
    }
    function test_rollingPeakAndClip() {
        const meter = createTemporaryObject(meterComponent, test);
        verify(meter);
        meter.dbLeft = -3; meter.dbRight = -9; meter.sample(1000);
        meter.dbLeft = -20; meter.sample(1500); compare(meter.heldLeft, -3);
        meter.sample(2001); compare(meter.heldLeft, -20);
        meter.dbLeft = 0.1; meter.clipped = true; meter.sample(2100); verify(meter.clipped);
        meter.dbLeft = -96; meter.sample(3101); verify(meter.clipped);
        mouseClick(findChild(meter, "audioClipLamp"), 10, 3); verify(!meter.clipped);
        meter.dbLeft = 0.1; meter.sample(3200); verify(!meter.clipped);
    }
}

