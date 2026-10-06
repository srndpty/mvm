#include "app/preview/test_window_mode.h"
#include "app/timeline_export.h"
#include "audio_adjustment_job.h"
#include "clip_sample_reader.h"
#include "core/checked_output_timebase.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/audio_adjustment.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "project/timeline_render.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace mvm;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "失敗: %s\n", message);
        std::exit(1);
    }
}

bool pump(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 30000) {
        QGuiApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = visualItem(child, name))
            return found;
    return nullptr;
}

QByteArray ffmpeg(const QStringList& args) {
    QProcess process;
    process.start(QStringLiteral(MVM_AUDIO_ADJUSTMENT_FFMPEG), args);
    require(process.waitForStarted(5000) && process.waitForFinished(20000),
            "UCRT64 FFmpeg の実行期限");
    const auto logs = process.readAllStandardError();
    if (process.exitCode() != 0)
        std::fprintf(stderr, "%s\n", logs.constData());
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            "UCRT64 FFmpeg の正常終了");
    return logs;
}

void addAudio(project::Project& p, const std::filesystem::path& path, const char* id, int track,
              std::int64_t start = 0, std::int64_t in = 0, std::int64_t out = 240) {
    project::MediaItem media;
    media.id = id;
    media.kind = project::MediaKind::Audio;
    media.mediaPath = path;
    media.name = id;
    media.sampleRate = 48000;
    media.durationSamples = 192000;
    p.mediaItems.push_back(media);
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Audio;
    clip.id = id;
    clip.mediaItemId = id;
    clip.mediaPath = path;
    clip.name = id;
    clip.sourceFpsNum = 60;
    clip.sourceFrameCount = 240;
    clip.sourceInFrame = in;
    clip.sourceOutFrame = out;
    clip.timelineStartFrame = start;
    clip.track = {project::TrackKind::Audio, track};
    p.timelineClips.push_back(clip);
}

double independentLufs(const std::filesystem::path& path, double gain, double* peakDb = nullptr) {
    const auto logs = ffmpeg({"-hide_banner", "-i", QString::fromStdWString(path.wstring()), "-af",
                              "volume=" + QString::number(gain, 'f', 8) + "dB,ebur128=peak=true",
                              "-f", "null", "-"});
    QRegularExpression regex(QStringLiteral("I:\\s+(-?[0-9]+\\.[0-9]+) LUFS"));
    auto matches = regex.globalMatch(QString::fromUtf8(logs));
    double result = std::numeric_limits<double>::quiet_NaN();
    while (matches.hasNext())
        result = matches.next().captured(1).toDouble();
    require(std::isfinite(result), "独立した FFmpeg 測定で LUFS を実際に比較");
    if (peakDb) {
        QRegularExpression peakRegex(QStringLiteral("Peak:\\s+(-?[0-9]+\\.[0-9]+) dBFS"));
        auto peaks = peakRegex.globalMatch(QString::fromUtf8(logs));
        *peakDb = std::numeric_limits<double>::quiet_NaN();
        while (peaks.hasNext())
            *peakDb = peaks.next().captured(1).toDouble();
        require(std::isfinite(*peakDb), "独立測定で true peak を実際に比較");
    }
    return result;
}

void pureTests() {
    {
        project::AudioAdjustmentSettings settings;
        std::vector<project::AudioDetectedRange> ranges;
        for (std::int64_t interval = 0; interval < 1200; ++interval)
            ranges.push_back({interval * 144000 + 48000, interval * 144000 + 96000});
        const auto keys = project::makeDuckingKeys(ranges, settings, 0, 216000, 60, 1);
        require(keys.size() == 4802, "1 時間に分散した 1200 発話の全折点を生成する");
        for (std::int64_t interval = 0; interval < 1200; ++interval)
            require(project::evaluateClipKeys(keys, 0, interval * 180 + 51) == 0 &&
                        project::evaluateClipKeys(keys, 0, interval * 180 + 60) == -12 &&
                        project::evaluateClipKeys(keys, 0, interval * 180 + 138) == -12 &&
                        project::evaluateClipKeys(keys, 0, interval * 180 + 174) == 0,
                    "長尺カーブの下降・保持・復帰が最後の発話まで正しい");
    }
    project::AudioAdjustmentSettings settings;
    settings.voiceTracks = {0};
    settings.bgmTrack = 1;
    std::string error;
    require(project::validateAudioAdjustmentSettings(settings, 2, error), "設定の対照群");
    for (int bad = 0; bad < 6; ++bad) {
        auto invalid = settings;
        if (bad == 0)
            invalid.bgmTrack = 0;
        if (bad == 1)
            invalid.voiceTracks = {0, 0};
        if (bad == 2)
            invalid.thresholdDb = std::numeric_limits<double>::quiet_NaN();
        if (bad == 3)
            invalid.attackMs = 0;
        if (bad == 4)
            invalid.releaseMs = -1;
        if (bad == 5)
            invalid.normalize = invalid.duck = false;
        require(!project::validateAudioAdjustmentSettings(invalid, 2, error),
                "新しい設定検査の負例");
    }
    const auto keys = project::makeDuckingKeys({{48000, 96000}}, settings, 0, 240, 60, 1);
    require(std::abs(project::evaluateClipKeys(keys, 0, 51)) < 1e-8 &&
                std::abs(project::evaluateClipKeys(keys, 0, 60) + 12) < 1e-8 &&
                std::abs(project::evaluateClipKeys(keys, 0, 138) + 12) < 1e-8 &&
                std::abs(project::evaluateClipKeys(keys, 0, 156) + 6) < 1e-8 &&
                std::abs(project::evaluateClipKeys(keys, 0, 174)) < 1e-8,
            "独立した時刻と減衰量で attack・hold・release を比較");
    const auto merged =
        project::makeDuckingKeys({{48000, 96000}, {100800, 144000}}, settings, 0, 300, 60, 1);
    require(project::evaluateClipKeys(merged, 0, 124) == -12, "短い間で音量を戻さず二重減衰しない");
    require(project::makeDuckingKeys({}, settings, 0, 240, 60, 1).empty(), "検出 0 件は無減衰");
    const auto cropped = project::makeDuckingKeys({{48000, 96000}}, settings, 90, 30, 60, 1);
    require(cropped.front().value == -12 && cropped.back().frame == 29,
            "減衰中に始まる BGM と clip 境界");
    project::ClipEffects effects;
    effects.normalizationGainDb = 6;
    effects.duckingKeys = keys;
    require(project::validateClipEffects(effects, 240, error) &&
                project::validateEffectKeys(effects, 240, true, error),
            "音声カーブ検査の対照群");
    effects.duckingKeys[0].value = 1;
    require(!project::validateEffectKeys(effects, 240, true, error), "増幅する ducking を拒否");
    effects.duckingKeys = keys;
    require(!project::validateEffectKeys(effects, 240, false, error), "映像の ducking を拒否");
    effects.normalizationGainDb = std::numeric_limits<double>::infinity();
    require(!project::validateClipEffects(effects, 240, error), "非有限の正規化を拒否");
    audio::LoudnessMeter meter;
    audio::LoudnessMeasurement measured;
    require(!meter.finish(measured, error), "音声 0 件の測定を成功にしない");
    require(meter.start(error), "測定器の開始");
    std::vector<float> pcm(960 * 2, 0);
    pcm[0] = std::numeric_limits<float>::quiet_NaN();
    require(!meter.push(pcm.data(), 960, error), "不正な PCM の負例");
    require(meter.start(error), "ピーク精度試験の開始");
    for (int block = 0; block < 50; ++block) {
        for (int sample = 0; sample < 960; ++sample) {
            const auto value = static_cast<float>(
                0.012345 * std::sin(2 * 3.14159265358979323846 * (block * 960 + sample) / 48.0));
            pcm[static_cast<std::size_t>(sample) * 2] = value;
            pcm[static_cast<std::size_t>(sample) * 2 + 1] = value;
        }
        require(meter.push(pcm.data(), 960, error), "ピーク精度試験の PCM を実測");
    }
    require(meter.finish(measured, error) && measured.measurable &&
                std::abs(measured.truePeakDb - 20 * std::log10(0.012345)) < 0.02,
            "metadata の 3 桁丸めによるピーク過小評価を防ぐ");
    require(meter.start(error), "短い音声の測定を開始");
    for (int block = 0; block < 3; ++block)
        require(meter.push(pcm.data(), 960, error), "短い音声の実 PCM を入力");
    require(meter.finish(measured, error) && !measured.measurable && measured.samples == 2880,
            "400 ms 未満の音声を未測定として明示する");
    require(meter.start(error), "末尾ピークの測定を開始");
    for (int block = 0; block < 50; ++block) {
        if (block == 49)
            pcm[pcm.size() - 2] = pcm.back() = 0.6F;
        require(meter.push(pcm.data(), 960, error), "末尾にだけ大きなピークを持つ PCM を入力");
    }
    require(meter.finish(measured, error) && measured.measurable &&
                measured.truePeakDb >= 20 * std::log10(0.6) - 0.02,
            "素材末尾の oversampling 遅延でもピークを取りこぼさない");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    if (argc == 5 && QString::fromLocal8Bit(argv[2]) == "--benchmark") {
        bool validSeconds = false;
        const auto seconds = QString::fromLocal8Bit(argv[4]).toLongLong(&validSeconds);
        require(validSeconds && seconds > 0 && seconds <= 86400, "計測する尺が不正です");
        auto source = project::createDefaultProject();
        source.audioTracks.resize(2);
        source.audioTracks[0].name = "声";
        source.audioTracks[1].name = "BGM";
        const auto path = std::filesystem::path(QString::fromLocal8Bit(argv[3]).toStdWString());
        addAudio(source, path, "voice", 0, 0, 0, seconds * 60);
        addAudio(source, path, "bgm", 1, 0, 0, seconds * 60);
        for (auto& media : source.mediaItems)
            media.durationSamples = seconds * 48000;
        for (auto& clip : source.timelineClips)
            clip.sourceFrameCount = seconds * 60;
        source.timelineClips[1].effects.volumePercent = 30;
        const auto valid = project::validateTimeline(source);
        if (!valid.success)
            std::fprintf(stderr, "%s\n", valid.error.c_str());
        require(valid.success, "計測するプロジェクトが有効である");
        project::AudioAdjustmentSettings settings;
        settings.voiceTracks = {0};
        settings.bgmTrack = 1;
        for (int run = 0; run < 3; ++run) {
            std::atomic<bool> running{true};
            std::atomic<int> progress{0};
            QElapsedTimer timer;
            timer.start();
            const auto result = app::analyzeAudioAdjustment(source, settings, running, progress);
            const auto elapsedMs = timer.elapsed();
            if (!result.success)
                std::fprintf(stderr, "%s\n", result.error.c_str());
            require(result.success && result.clips.size() == 2 && progress == 100,
                    "長尺音声の解析が完了する");
            for (const auto& clip : result.clips)
                require(clip.measurement.samples == seconds * 48000 && clip.measurement.measurable,
                        "長尺音声の全使用区間を実際に測定する");
            QJsonObject measurement{{"run", run + 1},
                                    {"secondsPerClip", seconds},
                                    {"clips", 2},
                                    {"elapsedMs", elapsedMs},
                                    {"voiceLufs", result.clips[0].measurement.integratedLufs},
                                    {"voicePeakDb", result.clips[0].measurement.truePeakDb},
                                    {"ranges", static_cast<qint64>(result.ranges.size())}};
            std::puts(QJsonDocument(measurement).toJson(QJsonDocument::Compact).constData());
        }
        return 0;
    }
    pureTests();
    QTemporaryDir temp;
    require(temp.isValid(), "音声試験の作業フォルダー");
    const auto file = [&](const char* name) {
        return std::filesystem::path(temp.filePath(name).toStdWString());
    };
    for (const auto& fixture :
         {std::pair{"voice.wav", "aevalsrc='0.1*sin(2*PI*1000*t)*between(t,1,2)':s=48000:d=4"},
          std::pair{"bgm.wav", "aevalsrc=0.03*sin(2*PI*220*t):s=48000:d=4"},
          std::pair{"quiet.wav", "aevalsrc=0.0015*sin(2*PI*220*t):s=48000:d=4"},
          std::pair{"second-voice.wav",
                    "aevalsrc='0.08*sin(2*PI*1000*t)*between(t,2.8,3.2)':s=48000:d=4"},
          std::pair{"peak.wav", "aevalsrc='0.02*sin(2*PI*1000*t)+0.7*sin(2*PI*1000*t)*between(t,1,"
                                "1.01)':s=48000:d=4"},
          std::pair{"silent.wav", "anullsrc=r=48000:cl=stereo:d=4"}})
        ffmpeg({"-hide_banner", "-y", "-f", "lavfi", "-i", fixture.second, "-ac", "2", "-c:a",
                "pcm_f32le", temp.filePath(fixture.first)});
    ffmpeg({"-hide_banner", "-y", "-i", temp.filePath("voice.wav"), "-c:a", "pcm_f32le",
            "-output_ts_offset", "5", temp.filePath("delayed.mov")});
    ffmpeg({"-hide_banner", "-y", "-f", "lavfi", "-i", "color=s=16x16:d=0.4", "-an", "-c:v",
            "libx264", temp.filePath("no-audio.mp4")});
    auto p = project::createDefaultProject();
    p.audioTracks.resize(3);
    p.audioTracks[0].name = "声";
    p.audioTracks[1].name = "BGM";
    p.audioTracks[2].name = "無音";
    addAudio(p, file("voice.wav"), "voice", 0);
    addAudio(p, file("bgm.wav"), "bgm", 1);
    addAudio(p, file("silent.wav"), "silent", 2);
    p.timelineClips[0].name =
        "声：長い説明を持つ録音素材の日本語タイトルを狭い画面でも最後まで折り返して確認します";
    p.audioTracks[0].muted = true;
    p.audioTracks[1].solo = true;
    {
        auto audible = p;
        audible.audioTracks[0].muted = false;
        audible.audioTracks[1].solo = false;
        auto subtitled = p;
        project::SubtitleCue cue;
        cue.id = "cue";
        cue.startFrame = 0;
        cue.endFrame = 30;
        cue.content = "字幕本文";
        subtitled.subtitles = project::SubtitleTrack{};
        subtitled.subtitles->cues.push_back(cue);
        auto gained = p;
        gained.audioTracks[1].mixerGainDb = -6;
        require(project::audioAdjustmentInputProjection(audible) ==
                        project::audioAdjustmentInputProjection(p) &&
                    project::audioAdjustmentInputProjection(subtitled) ==
                        project::audioAdjustmentInputProjection(p),
                "mute・solo・字幕は解析入力に含めない");
        require(project::audioAdjustmentInputProjection(gained) !=
                    project::audioAdjustmentInputProjection(p),
                "トラックゲインは解析入力に含める");
    }
    project::AudioAdjustmentSettings settings;
    settings.voiceTracks = {0, 2};
    settings.bgmTrack = 1;
    {
        auto scoped = settings;
        scoped.voiceTracks = {0};
        auto offline = p;
        offline.timelineClips[2].mediaPath = file("対象外で存在しない.wav");
        offline.audioTracks[2].mixerGainDb = -12;
        require(project::audioAdjustmentInputProjection(offline, scoped) ==
                    project::audioAdjustmentInputProjection(p, scoped),
                "対象外トラックの素材とゲインは authority に含めない");
        std::atomic<bool> scopeRunning{true};
        std::atomic<int> scopeProgress{0};
        const auto scopedResult =
            app::analyzeAudioAdjustment(offline, scoped, scopeRunning, scopeProgress);
        require(scopedResult.success && scopedResult.clips.size() == 2 &&
                    scopedResult.files.size() == 2,
                "対象外の offline 素材を開かず選択した声と BGM だけを解析する");
    }
    {
        // 長尺素材を細かく切った構成。保護と内容照合の単位は clip ではなく素材にする。
        const auto voiceCopy = temp.filePath("cut-voice.wav");
        const auto bgmCopy = temp.filePath("cut-bgm.wav");
        require(QFile::copy(temp.filePath("voice.wav"), voiceCopy) &&
                    QFile::copy(temp.filePath("bgm.wav"), bgmCopy),
                "多数 clip 試験の素材を作る");
        constexpr int kCuts = 60;
        auto cut = project::createDefaultProject();
        cut.audioTracks.resize(2);
        cut.audioTracks[0].name = "声";
        cut.audioTracks[1].name = "BGM";
        for (int index = 0; index < kCuts; ++index)
            addAudio(cut, std::filesystem::path(voiceCopy.toStdWString()),
                     ("cut-" + std::to_string(index)).c_str(), 0, index * 4, index * 4,
                     index * 4 + 4);
        addAudio(cut, std::filesystem::path(bgmCopy.toStdWString()), "cut-bgm", 1);
        // Project の素材は 1 ファイル 1 件。切った clip はすべて同じ素材を参照する。
        cut.mediaItems = {cut.mediaItems.front(), cut.mediaItems.back()};
        for (auto& clip : cut.timelineClips)
            if (clip.track.index == 0)
                clip.mediaItemId = cut.mediaItems.front().id;
        project::AudioAdjustmentSettings cutSettings;
        cutSettings.voiceTracks = {0};
        cutSettings.bgmTrack = 1;
        const auto cutValid = project::validateTimeline(cut);
        if (!cutValid.success)
            std::fprintf(stderr, "%s\n", cutValid.error.c_str());
        require(cutValid.success, "多数 clip 試験の timeline が有効である");
        const auto locks = app::audioFileLocksOpenedForTest();
        const auto hashes = app::audioFullHashesStartedForTest();
        std::atomic<bool> cutRunning{true};
        std::atomic<int> cutProgress{0};
        const auto cutResult =
            app::analyzeAudioAdjustment(cut, cutSettings, cutRunning, cutProgress);
        if (!cutResult.success)
            std::fprintf(stderr, "%s\n", cutResult.error.c_str());
        require(cutResult.success && cutResult.clips.size() == kCuts + 1,
                "同じ素材を参照する多数の clip を実際に解析する");
        std::printf("多数 clip の解析: 保護 handle %d、内容 hash %d\n",
                    app::audioFileLocksOpenedForTest() - locks,
                    app::audioFullHashesStartedForTest() - hashes);
        require(app::audioFileLocksOpenedForTest() - locks == 2,
                "保護 handle は clip 数ではなく素材数だけ開く");
        require(app::audioFullHashesStartedForTest() - hashes == 2 * 2,
                "内容 SHA-256 は開始時と終了時に素材数だけ計算する");
        require(cutResult.files.size() == 2, "出自は素材ごとに 1 件だけ記録する");

        // 目標値だけ違う過去の調整が clip ごとに残っていても、対象 track が同じなら射影は 1 回。
        for (std::size_t index = 0; index < cut.timelineClips.size(); ++index) {
            auto& clip = cut.timelineClips[index];
            clip.effects.audioAdjustmentFingerprint = cutResult.fingerprintText;
            clip.effects.audioAdjustmentSettings =
                // 半数は BGM と声の役割を入れ替える。対象 track の集合は同じなので射影も同じ。
                QJsonDocument(QJsonObject{{"bgmTrack", index % 2 == 0 ? 1 : 0},
                                          {"voiceTracks", QJsonArray{index % 2 == 0 ? 0 : 1}},
                                          {"voiceLufs", -10.0 - static_cast<double>(index)}})
                    .toJson(QJsonDocument::Compact)
                    .toStdString();
        }
        const auto cutPath = file("cut-project.mvm");
        const auto cutSaved = project::saveProjectJson(cut, cutPath);
        if (!cutSaved.success)
            std::fprintf(stderr, "%s\n", cutSaved.error.c_str());
        require(cutSaved.success, "多数 clip 試験の Project を保存");
        app::MvmController cutController(cutPath, {}, cut);
        require(pump([&] {
                    return !cutController.audioAdjustmentNeedsRegeneration() &&
                           cutController.audioAdjustmentWorkersIdle();
                }),
                "多数 clip の保存済み調整は読込時の照合で有効になる");
        require(pump([&] { return !cutController.audioAdjustmentPollingForTest(); }),
                "保存済み調整の待機中は 100 ms の poll を止める");
        const auto projections = app::audioProjectionHashesForTest();
        require(cutController.addTimelineMarker(), "射影の計算回数を測る編集");
        const auto projected = app::audioProjectionHashesForTest() - projections;
        std::printf("設定 %d 種類の編集 1 回: 射影 %d 回\n", kCuts + 1, projected);
        require(projected >= 1 && projected <= 2,
                "設定値だけ違う保存済み調整の射影を対象 track の集合ごとに 1 回だけ計算する");
        require(!cutController.audioAdjustmentPollingForTest(),
                "素材に関係しない編集では poll を再開しない");

        // 中央の 1 byte を反転し、size と更新時刻を元に戻す。2 回で元の内容に戻る。
        const auto flipKeepingStat = [&](const QString& target) {
            const auto wide = reinterpret_cast<const wchar_t*>(target.utf16());
            HANDLE handle =
                CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            FILETIME written{};
            require(handle != INVALID_HANDLE_VALUE &&
                        GetFileTime(handle, nullptr, nullptr, &written),
                    "置き換え前の更新時刻を読む");
            CloseHandle(handle);
            QFile flipped(target);
            require(flipped.open(QIODevice::ReadWrite), "内容だけ変える素材を開く");
            auto contents = flipped.readAll();
            require(contents.size() > 8, "内容比較の素材が短すぎる");
            contents[contents.size() / 2] =
                static_cast<char>(contents.at(contents.size() / 2) ^ 0x5a);
            require(flipped.seek(0) && flipped.write(contents) == contents.size(),
                    "中央の 1 byte を書き換える");
            flipped.close();
            handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(handle != INVALID_HANDLE_VALUE &&
                        SetFileTime(handle, nullptr, nullptr, &written),
                    "size を保ったまま更新時刻を戻す");
            CloseHandle(handle);
        };
        // watcher が通知を取りこぼして path を外した間の置き換え。取りこぼし確認だけで検出する。
        cutController.dropAudioFileWatchForTest();
        flipKeepingStat(bgmCopy);
        const auto rechecks = app::audioContentHashJobsStartedForTest();
        cutController.runAudioWatchFallbackForTest();
        require(pump([&] {
                    return app::audioContentHashJobsStartedForTest() > rechecks &&
                           cutController.audioAdjustmentWorkersIdle() &&
                           !cutController.audioAdjustmentPollingForTest();
                }) &&
                    cutController.audioAdjustmentNeedsRegeneration(),
                "監視が外れていた間に同じ size・更新時刻の別内容へ置き換えた素材を照合し直す");
        // 監視中でも size・更新時刻が同じ変更は fileChanged にならない
        // (実測)。戻すときも同じ経路を通す。
        cutController.dropAudioFileWatchForTest();
        flipKeepingStat(bgmCopy);
        cutController.runAudioWatchFallbackForTest();
        require(pump([&] {
                    return !cutController.audioAdjustmentNeedsRegeneration() &&
                           cutController.audioAdjustmentWorkersIdle();
                }),
                "元の内容に戻した素材を照合し直し、保存済み調整を再び有効にする");

        QFile voiceFile(voiceCopy);
        require(voiceFile.open(QIODevice::Append) && voiceFile.write("x", 1) == 1,
                "待機中に素材を書き換える");
        voiceFile.close();
        require(pump([&] { return cutController.audioAdjustmentNeedsRegeneration(); }),
                "poll を止めていても素材の変更を検知して再生成を案内する");

        cutController.shutdown();
        require(!cutController.audioAdjustmentPollingForTest() &&
                    !cutController.audioWatchFallbackActiveForTest(),
                "shutdown で自動音量調整の timer をすべて止める");
        const auto afterShutdown = app::audioContentHashJobsStartedForTest();
        require(voiceFile.open(QIODevice::Append) && voiceFile.write("y", 1) == 1,
                "shutdown 後に素材を書き換える");
        voiceFile.close();
        cutController.runAudioWatchFallbackForTest();
        QElapsedTimer quiet;
        quiet.start();
        require(pump([&] { return quiet.elapsed() >= 500; }) &&
                    !cutController.audioAdjustmentPollingForTest() &&
                    !cutController.audioWatchFallbackActiveForTest() &&
                    app::audioContentHashJobsStartedForTest() == afterShutdown,
                "shutdown 後は取りこぼし確認も素材の照合も始めない");
    }
    std::atomic<bool> running{true};
    std::atomic<int> progress{0};
    {
        app::AudioContentHashJob unreadable({std::filesystem::path(temp.path().toStdWString())});
        require(pump([&] { return unreadable.ready(); }), "読取失敗の内容照合が完了する");
        const auto hashed = unreadable.take();
        require(hashed.status == app::AudioContentHashResult::Status::ReadError &&
                    hashed.files.size() == 1 && !hashed.files[0].hashed,
                "読めない入力を Complete として扱わず失敗の出自を返す");
    }
    auto result = app::analyzeAudioAdjustment(p, settings, running, progress);
    if (!result.success)
        std::fprintf(stderr, "%s\n", result.error.c_str());
    require(result.success && result.clips.size() == 3 && progress == 100,
            "指定トラックを mute・solo に依存せず実際に測定");
    require(!result.clips[2].measurement.measurable && !result.ranges.empty(),
            "無音を測定成功と誤認せず声を検出");
    for (std::size_t i = 0; i < 2; ++i) {
        const double expected = i == 0 ? -16 : -24;
        require(
            std::abs(independentLufs(p.timelineClips[i].mediaPath, result.clips[i].correctionDb) -
                     expected) <= 0.5,
            "独立した二度目の測定で目標 ±0.5 LU 以内");
    }
    require(result.candidate.timelineClips[1].effects.duckingKeys.size() >= 4,
            "BGM の編集可能なカーブを生成");
    auto again = app::analyzeAudioAdjustment(result.candidate, settings, running, progress);
    require(again.success && again.candidate == result.candidate,
            "再生成で正規化も減衰も累積しない");
    auto multipleVoices = p;
    multipleVoices.timelineClips[2].mediaPath = multipleVoices.mediaItems[2].mediaPath =
        file("second-voice.wav");
    auto multiple = app::analyzeAudioAdjustment(multipleVoices, settings, running, progress);
    require(multiple.success && multiple.ranges.size() == 2 &&
                std::abs(multiple.ranges[1].startSample - 134400) <= 960 &&
                std::abs(multiple.ranges[1].endSample - 153600) <= 960,
            "別の声トラックの発音を和集合へ追加する");
    auto handEditedCurve = result.candidate;
    handEditedCurve.timelineClips[1].effects.duckingKeys.front().value = -5;
    require(app::analyzeAudioAdjustment(handEditedCurve, settings, running, progress).candidate ==
                result.candidate,
            "再生成は手直しした自動カーブだけを置き換える");
    auto peak = settings;
    peak.voiceLufs = -5;
    auto peakProject = p;
    peakProject.timelineClips[0].mediaPath = peakProject.mediaItems[0].mediaPath = file("peak.wav");
    auto limited = app::analyzeAudioAdjustment(peakProject, peak, running, progress);
    require(limited.success && limited.clips[0].peakLimited &&
                limited.clips[0].measurement.truePeakDb + limited.clips[0].correctionDb <=
                    -1.0 + 1e-6,
            "ピーク制限を明示し −1 dBTP の余裕を超えない");
    double independentPeak = 0;
    independentLufs(file("peak.wav"), limited.clips[0].correctionDb, &independentPeak);
    require(independentPeak <= -1.0, "補正後の true peak も独立測定で −1 dBTP 以下");
    auto noDuck = settings;
    noDuck.duck = false;
    auto onlyNormalized = app::analyzeAudioAdjustment(p, noDuck, running, progress);
    require(onlyNormalized.success &&
                onlyNormalized.candidate.timelineClips[1].effects.duckingKeys.empty(),
            "正規化のみを独立して実行");
    auto onlyDuck = settings;
    onlyDuck.normalize = false;
    auto ducked = app::analyzeAudioAdjustment(p, onlyDuck, running, progress);
    require(ducked.success && ducked.candidate.timelineClips[0].effects.normalizationGainDb == 0,
            "ダッキングのみを独立して実行");
    running = false;
    require(app::analyzeAudioAdjustment(p, settings, running, progress).cancelled,
            "解析中止は未適用");
    running = true;
    auto missing = p;
    missing.timelineClips[0].mediaPath = file("missing.wav");
    require(!app::analyzeAudioAdjustment(missing, settings, running, progress).success,
            "素材欠損を無音へ置き換えない");
    missing.timelineClips[0].mediaPath = missing.mediaItems[0].mediaPath = file("no-audio.mp4");
    require(!app::analyzeAudioAdjustment(missing, settings, running, progress).success,
            "音声 stream のない素材を測定成功にしない");
    auto invalid = p;
    invalid.timelineClips.clear();
    require(!app::analyzeAudioAdjustment(invalid, settings, running, progress).success,
            "対象 0 件を拒否");
    // 速度・音程保持・トリム・手動ゲインを既存 decoder で測定する。
    auto edited = p;
    edited.timelineClips[0].sourceInFrame = 30;
    edited.timelineClips[0].sourceOutFrame = 180;
    edited.timelineClips[0].speedNum = 2;
    edited.timelineClips[0].preservePitch = true;
    edited.timelineClips[0].effects.volumePercent = 50;
    edited.timelineClips[0].effects.volumeKeys = {{0, 50}, {74, 100}};
    edited.audioTracks[0].mixerGainDb = -3;
    edited.audioTracks[0].mixerPan = 0.5;
    auto speed = app::analyzeAudioAdjustment(edited, settings, running, progress);
    require(speed.success && speed.clips[0].measurement.samples == 60000 &&
                speed.candidate.timelineClips[0].effects.volumeKeys ==
                    edited.timelineClips[0].effects.volumeKeys,
            "トリム・速度・音程保持・手動音量を反映し手動キーを保持");
    // 最適化とは独立に、毎 sample の従来評価と 20 ms 窓で結果を照合する。
    for (const auto rate :
         {core::SupportedFrameRate{60, 1}, core::SupportedFrameRate{30000, 1001}}) {
        auto referenceProject = edited;
        referenceProject.timelineFpsNum = rate.numerator;
        referenceProject.timelineFpsDen = rate.denominator;
        const auto referenceDuration =
            project::timelineClipDuration(referenceProject, referenceProject.timelineClips[0]);
        require(referenceDuration.success, "比較用 clip の尺を求める");
        referenceProject.timelineClips[0].effects.volumeKeys = {{0, 50},
                                                                {referenceDuration.frame - 1, 100}};
        for (auto& track : referenceProject.audioTracks) {
            track.muted = false;
            track.solo = false;
        }
        const auto optimized =
            app::analyzeAudioAdjustment(referenceProject, settings, running, progress);
        require(optimized.success, "小数 frame rate の解析が完了する");
        app::ShuttleAudioPlan referencePlan;
        std::string error;
        require(app::planShuttleAudio(referenceProject, 1, 0, referencePlan, error),
                "比較用の音声計画を生成する");
        const auto& voice = referencePlan.clips.front();
        const auto timebase =
            core::CheckedOutputTimebase::create(rate.numerator, rate.denominator, 48000);
        require(static_cast<bool>(timebase), "比較用の時間軸を生成する");
        app::ClipSampleReaders referenceReader;
        referenceReader.reset(1, 48000);
        audio::LoudnessMeter referenceMeter;
        require(referenceMeter.start(error), "比較用の測定を開始する");
        std::vector<project::AudioDetectedRange> expectedRanges;
        std::vector<float> referencePcm;
        const auto pan = project::audioMixGains(0, voice.segment.mixerPan);
        const auto normalized = std::pow(10.0, optimized.clips.front().correctionDb / 20);
        const auto threshold = std::pow(10.0, settings.thresholdDb / 20);
        for (auto start = voice.timelineStartSample; start < voice.timelineEndSample;
             start += 960) {
            const auto count = std::min<std::int64_t>(960, voice.timelineEndSample - start);
            require(referenceReader.read(voice, 0, start + voice.sourceOffset, count, referencePcm,
                                         running, error),
                    "比較用 PCM を読み出す");
            double leftSquares = 0, rightSquares = 0;
            for (std::int64_t sample = 0; sample < count; ++sample) {
                const auto frame = timebase.value().schedulerOutputFrame(start + sample);
                require(static_cast<bool>(frame), "比較用 sample の frame を求める");
                const auto gain = project::renderSegmentGain(voice.segment, rate.numerator,
                                                             rate.denominator, frame.value());
                require(gain.has_value(), "比較用のゲインを求める");
                const auto at = static_cast<std::size_t>(sample) * 2;
                referencePcm[at] *= static_cast<float>(*gain * pan.first);
                referencePcm[at + 1] *= static_cast<float>(*gain * pan.second);
                leftSquares +=
                    static_cast<double>(referencePcm[at]) * static_cast<double>(referencePcm[at]);
                rightSquares += static_cast<double>(referencePcm[at + 1]) *
                                static_cast<double>(referencePcm[at + 1]);
            }
            require(
                referenceMeter.push(referencePcm.data(), static_cast<std::size_t>(count), error),
                "比較用 PCM を測定する");
            if (std::sqrt(std::max(leftSquares, rightSquares) / static_cast<double>(count)) *
                    normalized >=
                threshold)
                expectedRanges.push_back({start, start + count});
        }
        audio::LoudnessMeasurement measured;
        const auto merged = project::mergeAudioDetectedRanges(std::move(expectedRanges));
        require(referenceMeter.finish(measured, error) && measured.measurable &&
                    std::abs(measured.integratedLufs -
                             optimized.clips.front().measurement.integratedLufs) < 1e-10 &&
                    std::abs(measured.truePeakDb - optimized.clips.front().measurement.truePeakDb) <
                        1e-10 &&
                    measured.samples == optimized.clips.front().measurement.samples &&
                    std::equal(merged.begin(), merged.end(), optimized.ranges.begin(),
                               optimized.ranges.end(),
                               [](const auto& a, const auto& b) {
                                   return a.startSample == b.startSample &&
                                          a.endSample == b.endSample;
                               }),
                "frame 単位の最適化でも LUFS・ピーク・20 ms の検出位置が毎 sample 評価と一致する");
    }
    auto delayedProject = p;
    delayedProject.timelineClips[0].mediaPath = delayedProject.mediaItems[0].mediaPath =
        file("delayed.mov");
    auto delayed = app::analyzeAudioAdjustment(delayedProject, settings, running, progress);
    if (!delayed.success)
        std::fprintf(stderr, "%s\n", delayed.error.c_str());
    require(delayed.success &&
                std::abs(delayed.clips[0].measurement.integratedLufs -
                         result.clips[0].measurement.integratedLufs) < 0.1 &&
                !delayed.ranges.empty() &&
                std::abs(delayed.ranges.front().startSample - result.ranges.front().startSample) <=
                    960,
            "非ゼロ開始 PTS を素材の先頭へ揃えて測定・検出");
    auto crossfadeProject = p;
    crossfadeProject.timelineClips[0].sourceOutFrame = 120;
    auto incoming = crossfadeProject.timelineClips[0];
    incoming.id = "voice-2";
    incoming.sourceInFrame = 120;
    incoming.sourceOutFrame = 240;
    incoming.timelineStartFrame = 120;
    crossfadeProject.timelineClips.push_back(incoming);
    crossfadeProject.timelineTransitions.push_back({"audio-fade", "voice", "voice-2", 6, 6});
    auto crossfade = app::analyzeAudioAdjustment(crossfadeProject, settings, running, progress);
    require(crossfade.success && crossfade.clips.size() == 4 &&
                crossfade.clips[0].measurement.samples == 100800 &&
                crossfade.clips.back().measurement.samples == 100800,
            "クロスフェードの素材余白まで実測する");
    const auto path = file("project.mvm");
    auto json = project::serializeProjectJson(result.candidate, path);
    require(json.success &&
                project::parseProjectJsonText(json.json, path).project == result.candidate,
            "自動補正の JSON 往復");
    auto oldDocument = QJsonDocument::fromJson(
        QByteArray::fromStdString(project::serializeProjectJson(p, path).json));
    auto oldRoot = oldDocument.object();
    oldRoot["schema_version"] = 16;
    auto oldClips = oldRoot["timeline_clips"].toArray();
    for (qsizetype i = 0; i < oldClips.size(); ++i) {
        auto clip = oldClips[i].toObject();
        auto effects = clip["effects"].toObject();
        for (const auto* field : {"normalization_gain_db", "ducking_db", "ducking_keys",
                                  "audio_adjustment_settings", "audio_adjustment_fingerprint"})
            effects.remove(field);
        clip["effects"] = effects;
        oldClips[i] = clip;
    }
    oldRoot["timeline_clips"] = oldClips;
    auto old = QJsonDocument(oldRoot).toJson().toStdString();
    auto legacy = project::parseProjectJsonText(old, path);
    require(legacy.success && legacy.project.schemaVersion == project::kProjectSchemaVersion &&
                legacy.project.timelineClips[0].effects.normalizationGainDb == 0,
            "schema 16 を自動補正なしで読み込み");
    auto bad = json.json;
    const auto keyAt = bad.find("\"normalization_gain_db\"");
    require(keyAt != std::string::npos, "新しい検査の JSON 対照群");
    bad.erase(keyAt, bad.find('\n', keyAt) + 1 - keyAt);
    require(!project::parseProjectJsonText(bad, path).success, "現行 schema の補正欠落を拒否");
    auto split = result.candidate;
    int ids = 0;
    require(project::splitTimelineClips(
                split, {"bgm"}, 100, [&] { return "split-" + std::to_string(++ids); },
                project::LinkMode::Single)
                .success,
            "自動カーブを持つ BGM を分割");
    require(split.timelineClips[1].effects.normalizationGainDb ==
                result.candidate.timelineClips[1].effects.normalizationGainDb,
            "分割で正規化補正を保持");
    for (const auto& clip : split.timelineClips)
        if (clip.track.index == 1)
            for (std::int64_t frame = 0; frame < clip.sourceOutFrame - clip.sourceInFrame; ++frame)
                require(std::abs(project::evaluateClipKeys(clip.effects.duckingKeys,
                                                           clip.effects.duckingDb, frame) -
                                 project::evaluateClipKeys(
                                     result.candidate.timelineClips[1].effects.duckingKeys, 0,
                                     clip.timelineStartFrame + frame)) < 1e-9,
                        "分割後も元の減衰カーブの全 frame が連続する");
    auto trimmed = result.candidate;
    require(project::trimTimelineClip(trimmed, "bgm", project::TrimEdge::Left, 45,
                                      project::LinkMode::Single)
                .success,
            "減衰カーブを持つクリップを左トリム");
    require(
        std::abs(project::evaluateClipKeys(trimmed.timelineClips[1].effects.duckingKeys, 0, 15) -
                 project::evaluateClipKeys(result.candidate.timelineClips[1].effects.duckingKeys, 0,
                                           60)) < 1e-9,
        "トリムでカーブの素材位置を保持");
    auto moved = result.candidate;
    require(project::moveClip(moved, "bgm", {project::TrackKind::Audio, 1}, 300).success &&
                moved.timelineClips[1].effects == result.candidate.timelineClips[1].effects,
            "移動してもクリップ内のカーブと補正を保持する");
    auto copied = result.candidate.timelineClips[1];
    copied.id = "bgm-copy";
    require(
        project::placeTimelineClipAt(moved, copied, {project::TrackKind::Audio, 1}, 600).success &&
            moved.timelineClips.back().effects == result.candidate.timelineClips[1].effects,
        "複製・コピー配置でもカーブと補正を保持する");
    // 書き出しの frame ごとのゲインと preview / scrub の共通評価を全 frame で比較する。
    auto audible = result.candidate;
    for (auto& track : audible.audioTracks) {
        track.muted = false;
        track.solo = false;
    }
    app::TimelineExportRequest request;
    request.outputPath = file("output.mp4");
    request.width = p.outputWidth;
    request.height = p.outputHeight;
    auto exported = app::mapTimelineExportPlan(audible, request);
    require(exported.success, "自動補正を含む書き出し plan");
    std::vector<project::TimelineRenderSegment> segments;
    std::string error;
    require(project::timelineRenderSegments(audible, project::TrackKind::Audio, segments, error),
            "比較する描画区間");
    std::size_t compared = 0;
    for (const auto& mapped : exported.clips)
        if (mapped.audio)
            for (const auto& key : mapped.gainKeys) {
                const auto segment =
                    std::find_if(segments.begin(), segments.end(), [&](const auto& value) {
                        return value.clipIndex == mapped.projectClipIndex;
                    });
                require(segment != segments.end(), "比較する audio 区間が存在する");
                const auto gain = project::renderSegmentGain(
                    *segment, 60, 1, mapped.timelineStartFrame + key.localFrame);
                require(gain && std::abs(*gain - key.gain) < 1e-9,
                        "preview と書き出しのゲインが一致");
                ++compared;
            }
    require(compared == 720, "書き出し比較が空振りでない");
    // 非圧縮の実書き出しを、既存 decoder による preview PCM と独立に比較する。
    auto quietProject = p;
    quietProject.timelineClips[1].mediaPath = quietProject.mediaItems[1].mediaPath =
        file("quiet.wav");
    auto quiet = app::analyzeAudioAdjustment(quietProject, settings, running, progress);
    require(quiet.success && quiet.clips[1].correctionDb > 20 &&
                std::abs(independentLufs(file("quiet.wav"), quiet.clips[1].correctionDb) + 24) <=
                    0.5,
            "20 dB を超える補正も目標へ正規化する");
    auto pcmProject = quiet.candidate;
    pcmProject.outputWidth = 320;
    pcmProject.outputHeight = 240;
    request.outputPath = file("lossless.mkv");
    request.width = 320;
    request.height = 240;
    request.losslessAudio = true;
    request.timeoutMs = 30000;
    require(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
            "実書き出しの MLT 初期化");
    const auto rendered = app::exportTimeline(pcmProject, request);
    if (!rendered.success)
        std::fprintf(stderr, "%s\n", rendered.error.c_str());
    require(rendered.success && rendered.frameCount == 240, "自動音量を非圧縮 PCM で実書き出し");
    ffmpeg({"-hide_banner", "-y", "-i", temp.filePath("lossless.mkv"), "-map", "0:a:0", "-ar",
            "48000", "-ac", "2", "-c:a", "pcm_f32le", "-f", "f32le", temp.filePath("output.f32")});
    QFile raw(temp.filePath("output.f32"));
    require(raw.open(QIODevice::ReadOnly), "実書き出しの PCM を読む");
    const auto bytes = raw.readAll();
    require(bytes.size() >= 192000 * 2 * static_cast<qsizetype>(sizeof(float)),
            "PCM の比較 sample が不足していない");
    app::ShuttleAudioPlan pcmPlan;
    require(app::planShuttleAudio(pcmProject, 1, 0, pcmPlan, error) && pcmPlan.clips.size() == 1,
            "BGM 単独の preview 音声を比較");
    app::ClipSampleReaders readers;
    readers.reset(pcmPlan.clips.size(), 48000);
    std::vector<float> previewPcm;
    for (const auto sample : {14400LL, 57600LL, 100800LL, 124800LL, 168000LL}) {
        require(app::mixShuttleBlock(
                    pcmPlan, sample, 2400,
                    [&](std::size_t index, std::int64_t first, std::int64_t count,
                        std::vector<float>& pcm, std::string& readError) {
                        return readers.read(pcmPlan.clips[index], index, first, count, pcm, running,
                                            readError);
                    },
                    previewPcm, error),
                "比較する preview PCM を実際に生成");
        double expectedPower = 0, actualPower = 0;
        for (std::size_t i = 0; i < previewPcm.size(); ++i) {
            float actual = 0;
            std::memcpy(&actual,
                        bytes.constData() + (sample * 2 + static_cast<std::int64_t>(i)) *
                                                static_cast<std::int64_t>(sizeof(float)),
                        sizeof(float));
            expectedPower +=
                static_cast<double>(previewPcm[i]) * static_cast<double>(previewPcm[i]);
            actualPower += static_cast<double>(actual) * static_cast<double>(actual);
        }
        require(std::isfinite(actualPower) && expectedPower > 0 &&
                    std::abs(std::sqrt(actualPower / expectedPower) - 1) < 0.05,
                "正規化・減衰・復帰の実書き出し PCM と preview の振幅が 5% 以内");
    }
    require(project::saveProjectJson(p, path).success, "controller 試験の初期保存");
    app::MvmController controller(path, {}, p);
    QVariantMap options{{"voiceTracks", QVariantList{0, 2}}, {"bgmTrack", 1}};
    require(controller.startAudioAdjustment(options), "バックグラウンド解析の開始");
    require(pump([&] { return !controller.audioAdjusting(); }) &&
                controller.canApplyAudioAdjustment(),
            "解析結果を適用前に確認");
    require(project::loadProjectJson(path).project == p, "解析結果が保存対象へ勝手に反映されない");
    require(!controller.applyAudioAdjustment() &&
                pump([&] { return controller.audioAdjustmentResults().empty(); }) &&
                controller.audioAdjustmentError().isEmpty() && controller.saveProject(),
            "最終内容照合を待って解析を一括適用・保存");
    const auto applied = project::loadProjectJson(path).project;
    require(!applied.timelineClips[1].effects.duckingKeys.empty() &&
                !applied.timelineClips[0].effects.audioAdjustmentFingerprint.empty() &&
                !applied.lastAudioAdjustmentSettings.empty() &&
                !controller.audioAdjustmentNeedsRegeneration(),
            "設定と fingerprint を保存");
    require(controller.undoLastEdit() && controller.saveProject() &&
                project::loadProjectJson(path).project == p,
            "Undo 1 回で全自動調整を戻す");
    require(controller.redoLastEdit() && controller.saveProject() &&
                project::loadProjectJson(path).project == applied,
            "Redo でカーブと設定を復元");
    {
        app::MvmController reopened(path, {}, applied);
        require(pump([&] { return !reopened.audioAdjustmentNeedsRegeneration(); }),
                "保存済み調整は読込時の内容照合で有効になる");
        const auto checks = app::audioContentHashJobsStartedForTest();
        QElapsedTimer quietTimer;
        quietTimer.start();
        require(pump([&] { return quietTimer.elapsed() >= 1200; }) &&
                    app::audioContentHashJobsStartedForTest() == checks,
                "保存済み調整の待機中には全内容 hash を周期実行しない");
    }
    {
        auto missingProject = applied;
        auto& missingClip = missingProject.timelineClips[0];
        const auto source = temp.filePath("missing-after-save.wav");
        require(QFile::copy(QString::fromStdWString(missingClip.mediaPath.wstring()), source),
                "消失試験の素材を作る");
        app::AudioFileIdentity identity;
        require(app::inspectAudioFile(std::filesystem::path(source.toStdWString()), identity, true,
                                      nullptr, error),
                "消失前の内容を照合");
        std::string projection;
        std::vector<app::AudioFileIdentity> files;
        require(app::parseAudioInputFingerprint(missingClip.effects.audioAdjustmentFingerprint,
                                                projection, files),
                "保存済み出自を読む");
        app::AudioFileIdentity originalIdentity;
        require(
            app::inspectAudioFile(missingClip.mediaPath, originalIdentity, false, nullptr, error),
            "消失対象の正規化した path を確認");
        bool replaced = false;
        for (auto& fileIdentity : files)
            if (fileIdentity.key == originalIdentity.key) {
                fileIdentity = identity;
                replaced = true;
            }
        require(replaced, "消失する素材を保存済み fingerprint の比較対象へ含める");
        missingClip.mediaPath = std::filesystem::path(source.toStdWString());
        missingProject.mediaItems[0].mediaPath = missingClip.mediaPath;
        projection = app::audioProjectionHash(missingProject, settings);
        for (auto& clip : missingProject.timelineClips)
            clip.effects.audioAdjustmentFingerprint =
                app::formatAudioInputFingerprint(projection, files);
        const auto missingPath = file("missing-project.json");
        const auto savedMissing = project::saveProjectJson(missingProject, missingPath);
        if (!savedMissing.success)
            std::fprintf(stderr, "%s\n", savedMissing.error.c_str());
        require(savedMissing.success, "消失前の Project を保存");
        {
            app::MvmController closed(missingPath, {}, missingProject);
        }
        require(QFile::remove(source), "Controller 破棄後に素材を削除");
        app::MvmController reopened(missingPath, {}, project::loadProjectJson(missingPath).project);
        const auto checks = app::audioContentHashJobsStartedForTest();
        require(pump([&] { return reopened.audioAdjustmentNeedsRegeneration(); }),
                "素材が消失した保存済み調整は再生成が必要");
        require(pump([&] {
                    return app::audioContentHashJobsStartedForTest() > checks &&
                           reopened.audioAdjustmentWorkersIdle();
                }) &&
                    reopened.audioAdjustmentNeedsRegeneration(),
                "照合後も消失を成功扱いしない");
    }
    require(controller.startAudioAdjustment(options) &&
                pump([&] { return !controller.audioAdjusting(); }),
            "古い結果の検査を準備");
    require(controller.setTrackMuted(QStringLiteral("audio"), 2, true) &&
                controller.canApplyAudioAdjustment(),
            "mute では解析候補を失効させない");
    require(controller.undoLastEdit() && controller.canApplyAudioAdjustment() &&
                !controller.projectForTest()
                     .timelineClips[0]
                     .effects.audioAdjustmentFingerprint.empty(),
            "mute を戻しても解析候補と保存済み fingerprint を保てる");
    require(controller.addTimelineMarker() && controller.canApplyAudioAdjustment(),
            "マーカーでは解析候補を失効させない");
    require(controller.undoLastEdit() && controller.canApplyAudioAdjustment(),
            "マーカーを戻しても解析候補を保てる");
    {
        QVariantMap rejected = options;
        rejected.insert(QStringLiteral("voiceTracks"), QVariantList{});
        require(!controller.startAudioAdjustment(rejected) &&
                    controller.canApplyAudioAdjustment() && !controller.audioAdjusting() &&
                    controller.audioAdjustmentError().contains(QStringLiteral("トラック")),
                "不正な設定では再生停止や候補の破棄をせず解析を始めない");
    }
    {
        const auto voicePath = QString::fromStdWString(p.timelineClips[0].mediaPath.wstring());
        HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(voicePath.utf16()),
                                    FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME written{};
        require(handle != INVALID_HANDLE_VALUE && GetFileTime(handle, nullptr, nullptr, &written),
                "差し替え前の更新時刻を読む");
        CloseHandle(handle);
        QFile voiceFile(voicePath);
        require(voiceFile.open(QIODevice::ReadWrite), "内容だけ変える素材を開く");
        auto contents = voiceFile.readAll();
        require(contents.size() > 8, "内容比較の素材が短すぎる");
        const auto middle = contents.size() / 2;
        const char previous = contents.at(middle);
        contents[middle] = static_cast<char>(previous ^ 0x5a);
        require(voiceFile.seek(0) && voiceFile.write(contents) == contents.size(),
                "中央の 1 byte を書き換える");
        voiceFile.close();
        handle = CreateFileW(reinterpret_cast<const wchar_t*>(voicePath.utf16()),
                             FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE && SetFileTime(handle, nullptr, nullptr, &written),
                "size を保ったまま更新時刻を戻す");
        CloseHandle(handle);
        QFileInfo restored(voicePath);
        require(restored.size() == contents.size(), "1 byte の書き換えで size が変わった");
        const auto beforeApply = controller.projectForTest();
        require(!controller.applyAudioAdjustment() && controller.projectForTest() == beforeApply,
                "イベント処理前の内容差し替え直後には確定しない");
        require(pump([&] { return !controller.canApplyAudioAdjustment(); }),
                "size と更新時刻を戻しても内容が違う素材には古い解析を適用できない");
        require(pump([&] { return controller.audioAdjustmentResults().empty(); }) &&
                    controller.projectForTest() == beforeApply,
                "最終 SHA-256 不一致の結果を確定せず破棄する");
    }
    require(controller.setAudioTrackMix(0, -6, 0) && !controller.applyAudioAdjustment(),
            "解析後の編集で古い結果を拒否");
    require(controller.audioAdjustmentNeedsRegeneration(), "声の音量編集後に再生成を案内");
    require(controller.startAudioAdjustment(options), "解析中の編集試験を開始");
    require(controller.setAudioTrackMix(0, -9, 0) &&
                pump([&] { return !controller.audioAdjusting(); }) &&
                !controller.canApplyAudioAdjustment(),
            "解析中の編集でも結果を失効させる");
    require(controller.startAudioAdjustment(options) &&
                pump([&] { return !controller.audioAdjusting(); }),
            "素材差し替えの検査を準備");
    ffmpeg({"-hide_banner", "-y", "-f", "lavfi", "-i", "aevalsrc=0.05*sin(2*PI*1000*t):s=48000:d=4",
            "-ac", "2", "-c:a", "pcm_s16le", temp.filePath("voice.wav")});
    require(!controller.applyAudioAdjustment(), "素材差し替え後の古い解析結果を拒否");
    controller.cancelAudioAdjustment();
    {
        std::atomic<bool> gate{true};
        app::setAudioAdjustmentOpenGateForTest(&gate);
        require(controller.startAudioAdjustment(options), "取消試験の解析開始");
        require(pump([&] { return app::audioAdjustmentOpenGateWaitersForTest() >= 1; }),
                "decode 前で worker が止まっている");
        const auto lockedSource = QString::fromStdWString(p.timelineClips[0].mediaPath.wstring());
        QFile forbiddenWrite(lockedSource);
        require(!forbiddenWrite.open(QIODevice::ReadWrite) &&
                    !QFile::rename(lockedSource, lockedSource + QStringLiteral(".replacement")),
                "初期 hash 後から decoder open 前までの書換えと差し替えを拒否する");
        QElapsedTimer cancelTime;
        cancelTime.start();
        controller.cancelAudioAdjustment();
        const auto elapsed = cancelTime.elapsed();
        require(elapsed < 500 && !controller.audioAdjusting(),
                "解析の中止が worker の終了を待たない");
        require(app::audioAdjustmentOpenGateWaitersForTest() >= 1,
                "中止したあとも worker は open の前で止まっている");
        gate = false;
        require(pump([&] { return controller.audioAdjustmentWorkersIdle(); }),
                "gate を開けると取消した worker が終わる");
        app::setAudioAdjustmentOpenGateForTest(nullptr);
        require(!controller.canApplyAudioAdjustment(), "取消で結果を閉じる");
    }
    require(controller.startAudioAdjustment(options) &&
                pump([&] { return !controller.audioAdjusting(); }) &&
                controller.canApplyAudioAdjustment(),
            "実描画で測定結果・カーブ・検出区間を確認する準備");
    if (argc == 3 && QString::fromLocal8Bit(argv[2]) == "--audition") {
        require(controller.auditionAudioAdjustment() && controller.audioAdjustmentAuditioning(),
                "適用前の候補を実 audio endpoint で試聴する");
        QElapsedTimer auditionTime;
        auditionTime.start();
        require(pump([&] { return auditionTime.elapsed() >= 200; }) &&
                    controller.audioAdjustmentAuditioning(),
                "試聴開始後も解析結果を保持する");
        controller.stopAudioAdjustmentAudition();
        require(!controller.audioAdjustmentAuditioning() && controller.canApplyAudioAdjustment() &&
                    project::loadProjectJson(path).project == applied,
                "試聴を停止しても保存対象を変更しない");
    }
    // 実際に QML を描画し、狭幅・低い高さで主要操作と内部スクロールを確認する。
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("controller", &controller);
    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings,
                     [&](const QList<QQmlError>& values) { warnings.append(values); });
    QQmlComponent component(&engine);
    const auto uiPath = QUrl::fromLocalFile(QStringLiteral(MVM_AUDIO_ADJUSTMENT_UI_DIR)).toString();
    component.setData(
        ("import QtQuick\nimport QtQuick.Controls\nimport \"" + uiPath +
         "\"\nApplicationWindow { width: 900; height: 850; visible: true; AutoAudioDialog { "
         "mvmController: controller; Component.onCompleted: open() } }")
            .toUtf8(),
        QUrl("file:///audio-adjustment-test.qml"));
    require(pump([&] { return !component.isLoading(); }), "自動調整 UI のロード期限");
    if (component.isError())
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
    std::unique_ptr<QObject> windowObject(component.createWithInitialProperties(
        {{QStringLiteral("flags"), QVariant::fromValue(mvm::app::testBackgroundWindowFlags())}}));
    require(windowObject != nullptr, "自動調整 UI を実生成");
    auto* window = qobject_cast<QQuickWindow*>(windowObject.get());
    require(window != nullptr, "描画対象の window");
    for (const auto& size : {QSize(900, 850), QSize(360, 360)}) {
        window->resize(size);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 200) {
            QGuiApplication::processEvents();
            QThread::msleep(2);
        }
        auto* scroll = windowObject->findChild<QObject*>("autoAudioScroll");
        auto* analyze = windowObject->findChild<QQuickItem*>("autoAudioAnalyze");
        require(scroll && analyze, "最下部の解析ボタンとスクロール領域が存在");
        auto* content = scroll->property("contentItem").value<QObject*>();
        require(content != nullptr, "実スクロール対象");
        const double maximum = std::max(0.0, content->property("contentHeight").toDouble() -
                                                 content->property("height").toDouble());
        content->setProperty("contentY", 0);
        QGuiApplication::processEvents();
        if (argc >= 2)
            window->grabWindow().save(QString::fromLocal8Bit(argv[1]) + "/audio-adjustment-top-" +
                                      QString::number(size.width()) +
                                      (argc == 3 ? "-audition.png" : ".png"));
        content->setProperty("contentY", maximum);
        QGuiApplication::processEvents();
        require(scroll->property("clip").toBool() &&
                    content->property("contentY").toDouble() >= maximum - 1,
                "狭幅・低い高さでもスクロール端へ到達して親範囲で clip");
        const auto image = window->grabWindow();
        require(!image.isNull(), "ロードだけでなく実描画を確認");
        if (argc >= 2)
            image.save(QString::fromLocal8Bit(argv[1]) + "/audio-adjustment-" +
                       QString::number(size.width()) + (argc == 3 ? "-audition.png" : ".png"));
    }
    auto* voiceField = visualItem(window->contentItem(), "autoAudioField_voiceLufs");
    auto* analyzeButton = windowObject->findChild<QObject*>("autoAudioAnalyze");
    require(voiceField && analyzeButton, "負数入力の実 UI を取得");
    voiceField->setProperty("text", "-");
    require(QMetaObject::invokeMethod(voiceField, "textEdited"), "負数の途中入力を通知");
    QGuiApplication::processEvents();
    require(voiceField->property("text").toString() == "-" &&
                !analyzeButton->property("enabled").toBool(),
            "負数の途中入力を NaN へ書き換えず、途中の設定では解析しない");
    voiceField->setProperty("text", "-18");
    require(QMetaObject::invokeMethod(voiceField, "textEdited"), "有効な負数入力を通知");
    QGuiApplication::processEvents();
    require(analyzeButton->property("enabled").toBool(), "有効な負数を入力し終えると解析できる");
    windowObject.reset();
    QQmlComponent emptyComponent(&engine);
    emptyComponent.setData(
        ("import QtQuick\nimport QtQuick.Controls\nimport \"" + uiPath +
         "\"\nApplicationWindow { width: 260; height: 220; visible: true; "
         "QtObject { id: emptyController; property var audioTrackModel: []; "
         "signal audioAdjustmentApplied(); property bool audioAdjustmentApplying: false; "
         "property var savedAudioAdjustmentSettings: ({}); property bool audioAdjusting: false; "
         "property bool audioAdjustmentAuditioning: false; property bool canApplyAudioAdjustment: "
         "false; "
         "property bool audioAdjustmentNeedsRegeneration: true; "
         "property real audioAdjustmentProgress: 0; property var audioAdjustmentResults: []; "
         "property var audioAdjustmentRanges: []; property string audioAdjustmentError: "
         "'素材を読み出せません。対象の音声ファイルと設定を確認してください。'.repeat(12); "
         "function cancelAudioAdjustment() {} } "
         "AutoAudioDialog { mvmController: emptyController; Component.onCompleted: open() } }")
            .toUtf8(),
        QUrl("file:///audio-adjustment-empty-test.qml"));
    require(pump([&] { return !emptyComponent.isLoading(); }), "空一覧 UI のロード期限");
    std::unique_ptr<QObject> emptyWindowObject(emptyComponent.createWithInitialProperties(
        {{QStringLiteral("flags"), QVariant::fromValue(mvm::app::testBackgroundWindowFlags())}}));
    auto* emptyWindow = qobject_cast<QQuickWindow*>(emptyWindowObject.get());
    require(emptyWindow != nullptr, "空一覧・長いエラーを持つ実 window を生成");
    QElapsedTimer emptyPaintTime;
    emptyPaintTime.start();
    require(pump([&] { return emptyPaintTime.elapsed() >= 200; }), "空一覧 UI の描画を待つ");
    auto* emptyScroll = emptyWindowObject->findChild<QObject*>("autoAudioScroll");
    auto* emptyError = emptyWindowObject->findChild<QQuickItem*>("autoAudioError");
    require(emptyScroll && emptyError && emptyError->isVisible() && emptyError->height() > 100 &&
                emptyError->width() < 260,
            "長いエラーが狭い幅で折り返される");
    auto* emptyContent = emptyScroll->property("contentItem").value<QObject*>();
    require(emptyContent != nullptr, "空一覧 UI のスクロール対象");
    const double emptyMaximum = std::max(0.0, emptyContent->property("contentHeight").toDouble() -
                                                  emptyContent->property("height").toDouble());
    emptyContent->setProperty("contentY", emptyMaximum);
    QGuiApplication::processEvents();
    require(emptyScroll->property("clip").toBool() &&
                emptyContent->property("contentY").toDouble() >= emptyMaximum - 1 &&
                !emptyWindow->grabWindow().isNull(),
            "空一覧・長いエラー・極小 window でも末尾へ到達して実描画する");
    if (argc >= 2)
        emptyWindow->grabWindow().save(
            QString::fromLocal8Bit(argv[1]) +
            (argc == 3 ? "/audio-adjustment-empty-audition.png" : "/audio-adjustment-empty.png"));
    emptyWindowObject.reset();
    for (const auto& warning : warnings)
        std::fprintf(stderr, "%s\n", qPrintable(warning.toString()));
    require(warnings.empty(), "自動調整 UI の binding・レイアウト警告がない");
    {
        QVariantMap later = options;
        later.insert(QStringLiteral("voiceTracks"), QVariantList{2});
        later.insert(QStringLiteral("voiceLufs"), -18.0);
        require(controller.startAudioAdjustment(later) &&
                    pump([&] { return !controller.audioAdjusting(); }) &&
                    controller.canApplyAudioAdjustment() && !controller.applyAudioAdjustment() &&
                    pump([&] { return controller.audioAdjustmentResults().empty(); }) &&
                    controller.audioAdjustmentError().isEmpty() && controller.saveProject(),
                "別トラックへ 2 回目の自動調整を適用");
        require(std::abs(controller.savedAudioAdjustmentSettings()
                             .value(QStringLiteral("voiceLufs"))
                             .toDouble() +
                         18) < 1e-6,
                "ダイアログには最後に適用した設定を出す");
        const auto saved = project::loadProjectJson(path).project;
        const auto first =
            QJsonDocument::fromJson(
                QByteArray::fromStdString(saved.timelineClips[0].effects.audioAdjustmentSettings))
                .object();
        const auto last =
            QJsonDocument::fromJson(QByteArray::fromStdString(saved.lastAudioAdjustmentSettings))
                .object();
        require(std::abs(first.value(QStringLiteral("voiceLufs")).toDouble() + 16) < 1e-6 &&
                    std::abs(last.value(QStringLiteral("voiceLufs")).toDouble() + 18) < 1e-6,
                "最初の clip の設定ではなく、最後に適用した設定を保存する");
        require(controller.startAudioAdjustment(later) &&
                    pump([&] { return !controller.audioAdjusting(); }) &&
                    controller.canApplyAudioAdjustment(),
                "最終照合の取消試験を準備する");
        const auto beforeCancel = controller.projectForTest();
        require(!controller.applyAudioAdjustment() && controller.audioAdjustmentApplying(),
                "最終照合中は未確定であることを示す");
        controller.cancelAudioAdjustment();
        require(pump([&] { return controller.audioAdjustmentWorkersIdle(); }) &&
                    controller.projectForTest() == beforeCancel &&
                    !controller.audioAdjustmentApplying(),
                "取消した世代の最終 hash が一致しても Project を確定しない");
    }
    controller.shutdown();
    std::puts("自動音量調整の解析・編集・保存・履歴・失効・実描画の検査に合格しました");
    return 0;
}
