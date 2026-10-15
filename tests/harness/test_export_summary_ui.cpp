// P3-7B: 製品の Main.qml の書き出し設定に出す、入力映像と出力の仕様の要約。
//
//   mvm_test_text_ui_input --export-summary-ui <tests/assets/smoke>
//
// 実 controller・実 window・実際の素材の probe を使う。数式の描画だけ偽の backend。書き出し設定は、
// 製品の FileDialog の accepted から開き、controller の要約 (exportSettingsSummary) と、dialog に
// 実際に出た文の両方を確かめる。書き出しはしない。
//
// 生成する clip (文字・数式) は素材の file を持たないので、「入力ファイルの仕様を確認できない」と
// 数えてはいけない。逆に、動画・静止画の file が無い・壊れている場合は、その警告を出し続ける。

#include "app/preview/test_window_mode.h"
#include "math_fake_backend.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "test_window_focus.h"
#include "waveform_cache.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <QCoreApplication>
#include <QEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUrl>
#include <QtQuickTest/quicktest.h>

namespace {
namespace project = mvm::project;
using mvm::app::MvmController;

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

QUrl urlOf(const std::filesystem::path& path) {
    return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
}

const QString kUnreadableInput = QStringLiteral("入力映像: 仕様を取得できません");
const QString kFailedProbe = QStringLiteral("件の入力ファイルは仕様を確認できませんでした。");
const QString kBlack = QStringLiteral("映像クリップがないため、映像は黒になります。");

project::TimelineClip generatedClip(project::TimelineClipKind kind, const std::string& id,
                                    int track) {
    project::TimelineClip clip;
    clip.kind = kind;
    clip.id = id;
    clip.name = id;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = clip.sourceOutFrame = 60;
    clip.track = {project::TrackKind::Video, track};
    if (kind == project::TimelineClipKind::Math) {
        clip.math.source = "SIZE80x40 a";
        clip.math.fontSize = 96;
        clip.math.color = "#FFFF0000";
    } else {
        clip.text.content = id;
    }
    return clip;
}

// 生成する clip だけを置いた Project。素材は後から製品の操作 (addMediaFileToTimeline) で足す。
project::Project baseProject(bool math, bool text) {
    auto out = project::createDefaultProject();
    out.outputWidth = 640;
    out.outputHeight = 360;
    while (out.videoTracks.size() < 3) {
        auto track = out.videoTracks.front();
        track.name = "V" + std::to_string(out.videoTracks.size() + 1);
        out.videoTracks.push_back(track);
    }
    if (math)
        out.timelineClips.push_back(generatedClip(project::TimelineClipKind::Math, "math", 1));
    if (text)
        out.timelineClips.push_back(generatedClip(project::TimelineClipKind::Text, "title", 2));
    return out;
}

struct Session {
    mvm::test::FakeMathBackend backend;
    std::unique_ptr<MvmController> controller;
    mvm::app::WaveformCache cache;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow* window = nullptr;

    ~Session() {
        engine.reset();
        if (controller)
            controller->shutdown();
    }

    bool open(const std::filesystem::path& path) {
        const auto initial = baseProject(false, false);
        if (!project::saveProjectJson(initial, path).success)
            return false;
        controller = std::make_unique<MvmController>(
            path, std::filesystem::path{}, initial, nullptr, MvmController::ExportRunner{},
            MvmController::ExportThreadFactory{},
            MvmController::FileRevealer{
                [](const std::filesystem::path&, QString&) { return true; }});
        controller->setMathPreflightForTest(backend.preflight());
        engine = std::make_unique<QQmlApplicationEngine>();
        auto properties = mvm::app::testFixedWindowInitialProperties();
        properties.insert(QStringLiteral("mvmController"), QVariant::fromValue(controller.get()));
        properties.insert(QStringLiteral("waveformCache"), QVariant::fromValue(&cache));
        if (!mvm::app::testFixedWindowRequested())
            properties.insert(QStringLiteral("flags"), mvm::test::backgroundWindowFlags());
        engine->setInitialProperties(properties);
        engine->load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
        window = engine->rootObjects().isEmpty()
                     ? nullptr
                     : qobject_cast<QQuickWindow*>(engine->rootObjects().first());
        return window != nullptr;
    }

    // 見えている部品だけを返す (閉じた dialog の部品は返さない)。
    QQuickItem* find(const QString& name) {
        if (QQuickTest::qIsPolishScheduled(window))
            window->grabWindow();
        QQuickItem* found = nullptr;
        const std::function<void(QQuickItem*)> visit = [&](QQuickItem* item) {
            if (found)
                return;
            if (item->objectName() == name && item->isVisible()) {
                found = item;
                return;
            }
            for (auto* child : item->childItems())
                visit(child);
        };
        visit(window->contentItem());
        return found;
    }

    QString shownText(const QString& name) {
        auto* item = find(name);
        return item ? item->property("text").toString() : QString();
    }

    // 書き出し先を指定して、書き出し設定を開く (製品の FileDialog の accepted)。
    bool openSettings(const std::filesystem::path& output) {
        auto* dialog = window->findChild<QObject*>(QStringLiteral("exportFileDialog"));
        if (!dialog || !dialog->setProperty("selectedFile", urlOf(output)) ||
            !QMetaObject::invokeMethod(dialog, "accepted"))
            return false;
        return pumpUntil([&] { return find(QStringLiteral("exportAcceptButton")) != nullptr; },
                         5000);
    }

    bool closeSettings() {
        auto* dialog = window->findChild<QObject*>(QStringLiteral("exportSettingsDialog"));
        if (!dialog || !QMetaObject::invokeMethod(dialog, "close"))
            return false;
        return pumpUntil([&] { return find(QStringLiteral("exportAcceptButton")) == nullptr; },
                         5000);
    }
};

// 1 つの場合の期待。warnings は含むべき文、absent は含んではいけない文。
struct Expected {
    QString input;        // 完全一致
    QString inputPrefix;  // input が空のとき、先頭の一致 (素材の仕様は file で決まる)
    QStringList warnings; // 含む
    QStringList absent;   // 含まない
};

class Runner {
public:
    Runner(Session& session, std::filesystem::path directory, std::filesystem::path smoke)
        : s_(session), directory_(std::move(directory)), smoke_(std::move(smoke)) {}

    // 素材の写しを、この場合の directory に作る (消す・壊す操作が他の場合に及ばない)。
    std::filesystem::path copyMedia(const std::string& name, const std::filesystem::path& source) {
        const auto target = directory_ / "media" / name / source.filename();
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::copy_file(smoke_ / source, target,
                                   std::filesystem::copy_options::overwrite_existing);
        return target;
    }

    bool start(const std::string& name, const project::Project& initial) {
        name_ = name;
        const auto path = directory_ / (name + ".mvm");
        // 前の場合の編集 (素材の追加など) は捨ててから開く。
        if (s_.controller->dirty() && !s_.controller->discardUnsavedChanges())
            check(false, name + ": 前の場合の編集を捨てられません");
        if (!project::saveProjectJson(initial, path).success ||
            !s_.controller->openProject(urlOf(path))) {
            check(false, name + ": Project を開けません");
            return false;
        }
        s_.controller->setMathPreflightForTest(s_.backend.preflight());
        return true;
    }

    std::string add(const std::filesystem::path& media) {
        const auto before = s_.controller->projectForTest().timelineClips.size();
        const bool added = s_.controller->addMediaFileToTimeline(urlOf(media));
        const auto& clips = s_.controller->projectForTest().timelineClips;
        check(added && clips.size() > before,
              name_ + ": 素材を置けません: " + s_.controller->statusText().toStdString());
        for (const auto& clip : clips)
            if (clip.mediaPath == media && clip.kind != project::TimelineClipKind::Audio)
                return clip.id;
        return clips.empty() ? std::string{} : clips.back().id;
    }

    int trackOf(const std::string& clipId) {
        for (const auto& clip : s_.controller->projectForTest().timelineClips)
            if (clip.id == clipId)
                return clip.track.index;
        return -1;
    }

    void verify(const Expected& expected) {
        const auto summary = s_.controller->exportSettingsSummary();
        const QString input = summary.value(QStringLiteral("inputText")).toString();
        const QString output = summary.value(QStringLiteral("outputText")).toString();
        const QString warning = summary.value(QStringLiteral("warningText")).toString();
        std::printf("[%s]\n  %s\n  %s\n  警告: %s\n", name_.c_str(), qUtf8Printable(input),
                    qUtf8Printable(output),
                    warning.isEmpty() ? "(なし)"
                                      : qUtf8Printable(QString(warning).replace(
                                            QLatin1Char('\n'), QStringLiteral(" / "))));
        const std::string label = name_ + ": ";
        if (!expected.input.isEmpty())
            check(input == expected.input, label + "入力映像の文 '" + input.toStdString() +
                                               "' (期待 '" + expected.input.toStdString() + "')");
        else
            check(input.startsWith(expected.inputPrefix) && input != kUnreadableInput,
                  label + "入力映像の文 '" + input.toStdString() + "' (期待の先頭 '" +
                      expected.inputPrefix.toStdString() + "')");
        // 出力の仕様は Project の profile から決まる (生成する clip の有無で変わらない)。
        check(output == QStringLiteral("出力: 640×360 / 60 fps"),
              label + "出力の文 '" + output.toStdString() + "'");
        for (const auto& text : expected.warnings)
            check(warning.contains(text), label + "警告に '" + text.toStdString() + "' が無い: '" +
                                              warning.toStdString() + "'");
        for (const auto& text : expected.absent)
            check(!warning.contains(text) && !input.contains(text),
                  label + "'" + text.toStdString() + "' を出してはいけない: '" +
                      input.toStdString() + "' / '" + warning.toStdString() + "'");

        // 製品の dialog に、同じ文が出る。
        check(s_.openSettings(directory_ / (name_ + ".mp4")), label + "書き出し設定を開く");
        check(s_.shownText(QStringLiteral("exportInputSpecLabel")) == input,
              label + "dialog の入力映像の文 '" +
                  s_.shownText(QStringLiteral("exportInputSpecLabel")).toStdString() + "'");
        check(s_.shownText(QStringLiteral("exportOutputSpecLabel")) == output,
              label + "dialog の出力の文");
        const bool warningShown =
            s_.find(QStringLiteral("exportComparisonWarningLabel")) != nullptr;
        check(warning.isEmpty()
                  ? !warningShown
                  : warningShown &&
                        s_.shownText(QStringLiteral("exportComparisonWarningLabel")) == warning,
              label + "dialog の警告 '" +
                  s_.shownText(QStringLiteral("exportComparisonWarningLabel")).toStdString() + "'");
        check(s_.closeSettings(), label + "書き出し設定を閉じる");
    }

private:
    Session& s_;
    std::filesystem::path directory_;
    std::filesystem::path smoke_;
    std::string name_;
};

void corrupt(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "not a media file";
}

} // namespace

int runExportSummaryUi(const std::filesystem::path& directory, const std::filesystem::path& smoke) {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::filesystem::path video = "_import/vp9_opaque.webm";
    const std::filesystem::path image = "_import/jpg_quadrant.jpg";
    const std::filesystem::path audio = "wav_48k.wav";
    for (const auto& media : {video, image, audio})
        if (!std::filesystem::exists(smoke / media)) {
            std::fprintf(stderr,
                         "FAIL: 素材がありません: %s (scripts/make-testmedia.ps1 -Mode "
                         "Smoke)\n",
                         (smoke / media).string().c_str());
            return 3;
        }

    Session s;
    if (!s.open(directory / "start.mvm"))
        return 3;
    QString reason;
    if (!QTest::qWaitForWindowExposed(s.window) || !mvm::test::focusWithoutForeground(s.window) ||
        !mvm::test::isolatedFromUserInput(s.window, reason)) {
        std::fprintf(stderr, "PROTOCOL_INVALID: 試験を操作から隔離できません: %s\n",
                     qUtf8Printable(reason));
        return 4;
    }
    Runner run(s, directory, smoke);
    const QStringList noProbeWarning = {kUnreadableInput, kFailedProbe};

    // --- 生成する clip だけ: 素材の probe をしない。出力の仕様で描くことを示す ---
    if (run.start("math-only", baseProject(true, false)))
        run.verify({QStringLiteral("入力映像: なし（数式 1件を出力の仕様で描きます）"),
                    {},
                    {},
                    {kUnreadableInput, kFailedProbe, kBlack}});
    if (run.start("text-only", baseProject(false, true)))
        run.verify({QStringLiteral("入力映像: なし（文字 1件を出力の仕様で描きます）"),
                    {},
                    {},
                    {kUnreadableInput, kFailedProbe, kBlack}});
    if (run.start("math-and-text", baseProject(true, true)))
        run.verify({QStringLiteral("入力映像: なし（文字 1件・数式 1件を出力の仕様で描きます）"),
                    {},
                    {},
                    {kUnreadableInput, kFailedProbe, kBlack}});

    // --- 静止画だけ: file は読めることを確かめるが、動画の仕様 (fps) は作らない ---
    if (run.start("image-only", baseProject(false, false))) {
        run.add(run.copyMedia("image-only", image));
        run.verify({QStringLiteral("入力映像: なし（静止画 1件を出力の仕様で描きます）"),
                    {},
                    {},
                    {kUnreadableInput, kFailedProbe, kBlack}});
    }

    // --- 動画だけ: 実際の素材の仕様を出す ---
    if (run.start("video-only", baseProject(false, false))) {
        run.add(run.copyMedia("video-only", video));
        run.verify({{}, QStringLiteral("入力映像: "), {}, {kFailedProbe, kBlack}});
    }

    // --- 動画と生成する clip の混在: 動画の仕様だけを出し、生成する clip を失敗に数えない ---
    if (run.start("mixed-video-math-text", baseProject(true, true))) {
        run.add(run.copyMedia("mixed-video-math-text", video));
        run.verify(
            {{}, QStringLiteral("入力映像: "), {}, {kFailedProbe, kBlack, QStringLiteral("（")}});
    }

    // --- 本当の素材の失敗は出し続ける ---
    if (run.start("missing-video", baseProject(false, false))) {
        const auto copy = run.copyMedia("missing-video", video);
        run.add(copy);
        std::filesystem::remove(copy);
        run.verify({kUnreadableInput, {}, {QStringLiteral("1") + kFailedProbe}, {kBlack}});
    }
    if (run.start("invalid-video", baseProject(false, false))) {
        const auto copy = run.copyMedia("invalid-video", video);
        run.add(copy);
        corrupt(copy);
        run.verify({kUnreadableInput, {}, {QStringLiteral("1") + kFailedProbe}, {kBlack}});
    }
    if (run.start("mixed-math-invalid-video", baseProject(true, false))) {
        const auto copy = run.copyMedia("mixed-math-invalid-video", video);
        run.add(copy);
        corrupt(copy);
        run.verify({kUnreadableInput, {}, {QStringLiteral("1") + kFailedProbe}, {kBlack}});
    }
    if (run.start("missing-image-with-math", baseProject(true, false))) {
        const auto copy = run.copyMedia("missing-image-with-math", image);
        run.add(copy);
        std::filesystem::remove(copy);
        run.verify({QStringLiteral("入力映像: なし（静止画 1件・数式 1件を出力の仕様で描きます）"),
                    {},
                    {QStringLiteral("1") + kFailedProbe},
                    {kUnreadableInput, kBlack}});
    }
    if (run.start("video-and-invalid-video", baseProject(false, false))) {
        run.add(run.copyMedia("video-and-invalid-video", video));
        const auto second = run.copyMedia("video-and-invalid-video-2", video);
        run.add(second);
        corrupt(second);
        run.verify(
            {{}, QStringLiteral("入力映像: "), {QStringLiteral("1") + kFailedProbe}, {kBlack}});
    }

    // --- 書き出しに出ない clip・track は数えない (書き出しと同じ) ---
    if (run.start("disabled-invalid-video", baseProject(false, false))) {
        run.add(run.copyMedia("disabled-invalid-video", video));
        const auto second = run.copyMedia("disabled-invalid-video-2", video);
        const auto id = run.add(second);
        corrupt(second);
        check(s.controller->toggleTimelineClipEnabled(QString::fromStdString(id)),
              "disabled-invalid-video: 壊した clip を無効にする");
        run.verify({{}, QStringLiteral("入力映像: "), {}, {kFailedProbe, kBlack}});
    }
    if (run.start("disabled-math", baseProject(true, false))) {
        check(s.controller->toggleTimelineClipEnabled(QStringLiteral("math")),
              "disabled-math: 数式 clip を無効にする");
        run.verify({QStringLiteral("入力映像: なし"), {}, {kBlack}, noProbeWarning});
    }
    if (run.start("hidden-track-missing-video", baseProject(true, false))) {
        const auto copy = run.copyMedia("hidden-track-missing-video", video);
        const auto id = run.add(copy);
        std::filesystem::remove(copy);
        check(s.controller->setTrackMuted(QStringLiteral("video"), run.trackOf(id), true),
              "hidden-track-missing-video: 動画の track を非表示にする");
        run.verify({QStringLiteral("入力映像: なし（数式 1件を出力の仕様で描きます）"),
                    {},
                    {},
                    {kUnreadableInput, kFailedProbe, kBlack}});
    }

    // --- 映像の clip が無い ---
    if (run.start("empty", baseProject(false, false)))
        run.verify({QStringLiteral("入力映像: なし"), {}, {kBlack}, noProbeWarning});
    if (run.start("audio-only", baseProject(false, false))) {
        run.add(run.copyMedia("audio-only", audio));
        run.verify({QStringLiteral("入力映像: なし"), {}, {kBlack}, noProbeWarning});
    }

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
