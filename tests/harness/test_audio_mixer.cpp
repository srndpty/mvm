#include "media/audio_preview/audio_frame_queue.h"
#include "project/project.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "project/timeline_render.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;

void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "失敗: %s\n", message);
        ++failures;
    }
}

bool near(double a, double b) {
    return std::abs(a - b) < 0.00001;
}
} // namespace

int main() {
    using namespace mvm;
    check(project::isValidAudioMix(-96, -1) && project::isValidAudioMix(15, 1),
          "音量・パンの境界値を拒否しました");
    check(!project::isValidAudioMix(15.1, 0) && !project::isValidAudioMix(-96.1, 0) &&
              !project::isValidAudioMix(0, 1.01) && !project::isValidAudioMix(0, -1.01) &&
              !project::isValidAudioMix(std::numeric_limits<double>::quiet_NaN(), 0),
          "範囲外・非有限の値を受理しました");
    const auto center = project::audioMixGains(0, 0);
    const auto right = project::audioMixGains(0, 1);
    const auto left = project::audioMixGains(0, -1);
    check(center.first == 1 && center.second == 1 && right.first == 0 && right.second == 1 &&
              left.first == 1 && left.second == 0,
          "中央・左右端のステレオバランスが違います");
    check(project::audioMixGains(-96, 0).first == 0 &&
              near(project::audioMixGains(15, 0).first, 5.6234132519),
          "無音または+15 dBの振幅が違います");

    auto project = project::createDefaultProject();
    project.audioTracks[0].mixerName = "ナレーション";
    project.audioTracks[0].mixerGainDb = 6;
    project.audioTracks[0].mixerPan = -0.5;
    const auto json = project::serializeProjectJson(project, "mixer.mvm");
    check(json.success, "対照群のJSONを生成できません");
    const auto loaded = project::parseProjectJsonText(json.json, "mixer.mvm");
    check(loaded.success && loaded.project.audioTracks == project.audioTracks &&
              loaded.project.audioTracks[0].name == "A1",
          "ミキサー設定の保存・読込またはタイムライン名の独立性が壊れました");
    for (const auto& change : {std::pair{"\"mixer_gain_db\": 6", "\"mixer_gain_db\": 16"},
                               std::pair{"\"mixer_pan\": -0.5", "\"mixer_pan\": -2"}}) {
        auto bad = json.json;
        const auto at = bad.find(change.first);
        check(at != std::string::npos, "負例で変更するfieldがありません");
        if (at != std::string::npos)
            bad.replace(at, std::string(change.first).size(), change.second);
        const auto rejected = project::parseProjectJsonText(bad, "mixer.mvm");
        check(!rejected.success && rejected.error.find("範囲外") != std::string::npos,
              "負例が音量・パンの違反箇所で落ちません");
    }
    for (const auto* field : {"mixer_name", "mixer_gain_db", "mixer_pan"}) {
        auto duplicate = json.json;
        const std::string key = std::string("\"") + field + "\": ";
        const auto at = duplicate.find(key);
        check(at != std::string::npos, "重複fieldの負例を作れません");
        if (at != std::string::npos) {
            const auto end = duplicate.find_first_of(",}", at);
            duplicate.insert(end, ", " + duplicate.substr(at, end - at));
        }
        const auto rejected = project::parseProjectJsonText(duplicate, "mixer.mvm");
        check(!rejected.success && rejected.error.find("重複") != std::string::npos,
              "ミキサー設定の重複を拒否しません");
    }
    auto invalid = project;
    invalid.audioTracks[0].mixerPan = 2;
    check(!project::validateTimeline(invalid).success &&
              !project::serializeProjectJson(invalid, "mixer.mvm").success,
          "不正な内部データを保存しました");
    project::TimelineRenderSegment segment;
    segment.original.sourceFpsNum = 60;
    segment.original.sourceFrameCount = 60;
    segment.original.sourceOutFrame = 60;
    segment.clip = segment.original;
    segment.mixerGainDb = 6;
    const auto gain = project::renderSegmentGain(segment, 60, 1, 0);
    check(gain && near(*gain, 1.99526231497),
          "書き出し・シャトル共有の音量評価がトラック音量を反映しません");

    auto bus = std::make_shared<audio::AudioMixerBus>();
    audio::AudioFrameQueue queue({1}, {1});
    queue.setMixerBus(bus);
    queue.setGainAtSample([](std::int64_t) { return 0.5F; });
    audio::AudioChunk chunk;
    chunk.sourceId = {1};
    chunk.sourceGeneration = {1};
    chunk.resourceEpoch = {1};
    chunk.startSample = 0;
    chunk.sampleCount = 2;
    chunk.sampleRate = 48000;
    chunk.channels = 2;
    chunk.pcm =
        std::make_shared<std::vector<float>>(std::initializer_list<float>{0.8F, -0.6F, 0.4F, 0.2F});
    check(queue.push(chunk) == audio::AudioQueuePushResult::Accepted,
          "PCMの対照群を受理できません");
    bus->leftGain.store(2);
    bus->rightGain.store(0);
    float output[4]{};
    check(queue.consume(output, 0, 1, {1}).audioSamples == 1 && near(output[0], 0.8) &&
              output[1] == 0,
          "クリップ音量とトラック音量・パンの適用が違います");
    // 既存のキューを作り直さず、次の消費へ制御値が反映される。
    bus->leftGain.store(0);
    bus->rightGain.store(1);
    check(queue.consume(output, 1, 1, {1}).audioSamples == 1 && output[0] == 0 &&
              near(output[1], 0.1),
          "再生中の制御値変更が反映されません");
    const float a[]{0.8F, 0.2F}, b[]{0.4F, -0.2F};
    bus->beginBlock(2);
    bus->addBlock(a, 2);
    bus->addBlock(b, 2);
    bus->publishBlock();
    check(near(bus->peakLeft.exchange(0), 1.2) && bus->peakRight.exchange(0) == 0 &&
              bus->clipped.load(),
          "同じトラックの合算ピーク・位相相殺・0 dB超過を測れません");
    bus->beginBlock(2);
    bus->publishBlock();
    check(bus->peakLeft.load() == 0 && bus->clipped.load(),
          "無音でピークを残すかクリップ表示を自動解除しました");
    bus->clipped.store(false);
    check(!bus->clipped.load(), "クリップ表示を解除できません");
    if (failures == 0)
        std::puts("音量・パン、永続化と負例、PCM適用、トラック合算とクリップを確認しました");
    return failures ? 1 : 0;
}
