#include "project/clip_effects.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

mvm::project::Project projectWithClip() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Manim,
                                     "clip.mp4",
                                     "clip",
                                     "clip-1",
                                     {},
                                     60,
                                     1,
                                     120,
                                     10,
                                     110,
                                     0,
                                     {},
                                     {},
                                     {}});
    return project;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);

    using namespace mvm::project;
    {
        for (const auto& expected :
             {std::pair{KeyInterpolation::Linear, 25.0}, std::pair{KeyInterpolation::EaseIn, 6.25},
              std::pair{KeyInterpolation::EaseOut, 43.75},
              std::pair{KeyInterpolation::EaseInOut, 15.625}}) {
            std::vector<ClipKeyframe> keys{{0, 0, expected.first}, {100, 100}};
            check(std::abs(evaluateClipKeys(keys, 0, 25) - expected.second) < 1e-9,
                  "補間の独立した期待値");
            const auto original = keys;
            reframeClipKeys(keys, 101, 51, 25);
            for (int frame = 0; frame < 51; ++frame)
                check(std::abs(evaluateClipKeys(keys, 0, frame) -
                               evaluateClipKeys(original, 0, frame + 25)) < 1e-9,
                      "トリムでイーズの動きを保つ");
            keys = original;
            insertClipKey(keys, 25, expected.second);
            for (int frame = 0; frame <= 100; ++frame)
                check(std::abs(evaluateClipKeys(keys, 0, frame) -
                               evaluateClipKeys(original, 0, frame)) < 1e-9,
                      "キー追加で曲線を保つ");
        }
        {
            std::vector<ClipKeyframe> spline{{0, 0, KeyInterpolation::Spline, 0, 1, 0.2, 0.8},
                                             {100, 100}};
            check(std::abs(evaluateClipKeys(spline, 0, 25) - 21.25) < 1e-9,
                  "スプラインの独立した期待値を満たしません");
            const auto original = spline;
            insertClipKey(spline, 25, 21.25);
            for (int frame = 0; frame <= 100; ++frame)
                check(std::abs(evaluateClipKeys(spline, 0, frame) -
                               evaluateClipKeys(original, 0, frame)) < 1e-9,
                      "スプラインへのキー追加で曲線が変わりました");
            spline = original;
            reframeClipKeys(spline, 101, 51, 25);
            for (int frame = 0; frame < 51; ++frame)
                check(std::abs(evaluateClipKeys(spline, 0, frame) -
                               evaluateClipKeys(original, 0, frame + 25)) < 1e-9,
                      "スプラインのトリムで曲線が変わりました");
            const auto controls = clipKeySplineControls(spline.front());
            auto restricted = spline;
            restricted.front().control1 = controls.first;
            restricted.front().control2 = controls.second;
            restricted.front().curveStart = 0;
            restricted.front().curveEnd = 1;
            for (int frame = 0; frame < 51; ++frame)
                check(std::abs(evaluateClipKeys(restricted, 0, frame) -
                               evaluateClipKeys(spline, 0, frame)) < 1e-9,
                      "切り出した区間のハンドルが表示曲線と一致しません");
            auto project = projectWithClip();
            project.timelineClips[0].effects.positionXKeys = {
                {0, -20, KeyInterpolation::Spline, 0, 1, 0.2, 0.8}, {99, 20}};
            const auto path = directory / "spline.mvm";
            check(saveProjectJson(project, path).success, "スプラインを保存できません");
            const auto loaded = loadProjectJson(path);
            check(loaded.success &&
                      loaded.project.timelineClips[0].effects == project.timelineClips[0].effects,
                  "スプラインの保存復元が一致しません");
            std::ifstream file(path);
            const std::string json((std::istreambuf_iterator<char>(file)), {});
            auto bad = json;
            const auto pos = bad.find(",\"control1\":0.2");
            check(pos != std::string::npos, "スプライン負例の対照に制御値がありません");
            if (pos != std::string::npos) {
                bad.erase(pos, std::string(",\"control1\":0.2").size());
                const auto badPath = directory / "spline-missing-control.mvm";
                std::ofstream output(badPath);
                output << bad;
                output.close();
                check(!loadProjectJson(badPath).success, "スプラインの制御値欠落を拒否しません");
            }
            std::string error;
            auto invalid = project.timelineClips[0].effects;
            invalid.positionXKeys.front().control1 = -0.1;
            check(!validateEffectKeys(invalid, 100, false, error),
                  "範囲外のスプライン制御値を拒否しません");
            invalid.positionXKeys.front().control1 = std::numeric_limits<double>::quiet_NaN();
            check(!validateEffectKeys(invalid, 100, false, error),
                  "非数のスプライン制御値を拒否しません");
            invalid = {};
            invalid.cropLeftKeys = {{0, 0, KeyInterpolation::Spline, 0, 1, 1, 1}, {99, 80}};
            invalid.cropRightKeys = {{0, 80, KeyInterpolation::Spline, 0, 1, 0, 0}, {99, 0}};
            check(!validateEffectKeys(invalid, 100, false, error),
                  "スプラインのキー間で不正になるクロップを拒否しません");
        }
        {
            // 曲線上へのキー追加と、同じキーの削除を続けると元の曲線へ戻る (追加の逆)。
            // トリム後の部分曲線 (curveStart/End が 0/1 でない) も同じ。
            const auto sameCurve = [](const std::vector<ClipKeyframe>& actual,
                                      const std::vector<ClipKeyframe>& expected) {
                for (int frame = 0; frame <= 100; ++frame)
                    if (std::abs(evaluateClipKeys(actual, 0, frame) -
                                 evaluateClipKeys(expected, 0, frame)) >= 1e-9)
                        return false;
                return true;
            };
            for (const auto interpolation :
                 {KeyInterpolation::Linear, KeyInterpolation::EaseIn, KeyInterpolation::EaseOut,
                  KeyInterpolation::EaseInOut, KeyInterpolation::Spline}) {
                for (const auto range : {std::pair{0.0, 1.0}, std::pair{0.25, 0.7}}) {
                    const std::vector<ClipKeyframe> original{
                        {0, 0, interpolation, range.first, range.second, 0.2, 0.8}, {100, 100}};
                    auto keys = original;
                    insertClipKey(keys, 25, evaluateClipKeys(original, 0, 25));
                    check(keys.size() == 3 && sameCurve(keys, original),
                          "削除試験の前提: キー追加で曲線を保つ");
                    check(removeClipKeys(keys, {25}) == 1 && keys == original,
                          "追加したキーの削除で元の曲線へ戻りません");
                    // 連続する複数キーの一括削除も、順に結合して元へ戻る。
                    insertClipKey(keys, 25, evaluateClipKeys(original, 0, 25));
                    insertClipKey(keys, 60, evaluateClipKeys(original, 0, 60));
                    check(keys.size() == 4 && sameCurve(keys, original),
                          "削除試験の前提: 2 つのキー追加で曲線を保つ");
                    auto one = keys;
                    check(removeClipKeys(one, {25}) == 1 && one.size() == 3 &&
                              sameCurve(one, original),
                          "片方のキー削除で残りの曲線が変わりました");
                    check(removeClipKeys(keys, {60, 25}) == 2 && keys == original,
                          "連続キーの一括削除で元の曲線へ戻りません");
                }
            }
            // 独立した期待値: EaseInOut 0 -> 100 の f50 は 50。f25 の追加・削除後も 50。
            std::vector<ClipKeyframe> easeInOut{{0, 0, KeyInterpolation::EaseInOut}, {100, 100}};
            insertClipKey(easeInOut, 25, 15.625);
            removeClipKeys(easeInOut, {25});
            check(std::abs(evaluateClipKeys(easeInOut, 0, 50) - 50) < 1e-9,
                  "EaseInOut のキー追加・削除後に f50 が 50 になりません");
            // 対照: 別々に作った区間 (連続部分でない) は結合しない。
            std::vector<ClipKeyframe> separate{
                {0, 0, KeyInterpolation::EaseIn}, {50, 50, KeyInterpolation::EaseOut}, {100, 100}};
            check(removeClipKeys(separate, {50}) == 1 && separate.size() == 2 &&
                      separate.front().curveStart == 0 && separate.front().curveEnd == 1,
                  "連続部分でない区間を結合しました");
            check(removeClipKeys(separate, {42}) == 0 && separate.size() == 2,
                  "存在しないキーの削除で個数を返しました");
        }
        std::string error;
        ClipEffects motion;
        motion.positionXKeys = {{0, -100}, {100, 100}};
        motion.rotationKeys = {{0, -180}, {100, 180}};
        check(validateEffectKeys(motion, 101, false, error), "負の位置と回転の対照群");
        check(evaluateClipEffects(motion, 25).positionXPercent == -50 &&
                  evaluateClipEffects(motion, 25).rotationDegrees == -90,
              "モーションを共通評価する");
        check(!validateEffectKeys(motion, 101, true, error), "音声へモーションを適用しない");
        motion.positionXKeys.push_back({100, 1});
        check(!validateEffectKeys(motion, 101, false, error), "重複キーを拒否する");
        motion.positionXKeys.pop_back();
        motion.positionXKeys[0].interpolation = static_cast<KeyInterpolation>(99);
        check(!validateEffectKeys(motion, 101, false, error), "未知の補間を拒否する");
        motion = {};
        motion.cropLeftKeys = {{0, 0, KeyInterpolation::EaseOut}, {100, 80}};
        motion.cropRightKeys = {{0, 80, KeyInterpolation::EaseIn}, {100, 0}};
        check(!validateEffectKeys(motion, 101, false, error),
              "端は80%でも途中が120%になるクロップを拒否する");
        motion.cropLeftKeys.back().value = 40;
        motion.cropRightKeys.front().value = 40;
        check(validateEffectKeys(motion, 101, false, error), "異なる補間のクロップ対照群");
    }
    {
        auto original = projectWithClip();
        for (const auto& channel : effectChannels()) {
            if (isAudioEffectChannel(channel.kind))
                continue;
            const double from = channel.kind >= ClipKeyKind::CropLeft ? 2 : 10;
            const double to = channel.kind >= ClipKeyKind::CropLeft ? 12 : 80;
            original.timelineClips[0].effects.*
                channel.keys = {{0, from, KeyInterpolation::EaseInOut}, {99, to}};
        }
        const auto checkRemaining = [&](const Project& edited) {
            for (const auto& clip : edited.timelineClips) {
                const auto duration = timelineClipDuration(edited, clip);
                check(duration.success, "編集後のモーション尺を取得できない");
                for (std::int64_t frame = 0; frame < duration.frame; ++frame) {
                    const auto before = evaluateClipEffects(original.timelineClips[0].effects,
                                                            clip.timelineStartFrame + frame);
                    const auto after = evaluateClipEffects(clip.effects, frame);
                    for (const auto& channel : effectChannels())
                        check(std::abs(before.*channel.base - after.*channel.base) < 1e-8,
                              "分割・トリム後の全フレームでモーションを保つ");
                }
            }
        };
        auto edited = original;
        check(splitTimelineClips(
                  edited, {"clip-1"}, 37, [] { return "split-motion"; }, LinkMode::Single)
                  .success,
              "イーズ途中でモーションを分割できない");
        checkRemaining(edited);
        edited = original;
        check(trimTimelineClip(edited, "clip-1", TrimEdge::Left, 23, LinkMode::Single).success,
              "モーションを左トリムできない");
        checkRemaining(edited);
        edited = original;
        check(trimTimelineClip(edited, "clip-1", TrimEdge::Right, -29, LinkMode::Single).success,
              "モーションを右トリムできない");
        checkRemaining(edited);
        edited = original;
        check(rateStretchTimelineClip(edited, "clip-1", TrimEdge::Right, 100, LinkMode::Single)
                  .success,
              "モーションをレートストレッチできない");
        for (const auto& channel : effectChannels()) {
            if (isAudioEffectChannel(channel.kind))
                continue;
            const auto& keys = edited.timelineClips[0].effects.*channel.keys;
            check(keys.size() == 2 && keys.back().frame == 199 &&
                      keys.front().interpolation == KeyInterpolation::EaseInOut,
                  "全モーション項目を端から端へ伸縮する");
        }
    }
    ClipEffects effects;
    check(clipEffectsAreDefault(effects), "既定effectをdefaultと判定する");
    {
        // preview / export は default の clip で effect 処理ごと省く。キーだけの clip
        // を落とさない。
        ClipEffects keyed;
        keyed.opacityKeys = {{0, 100.0}, {10, 0.0}};
        check(!clipEffectsAreDefault(keyed), "不透明度キーだけのeffectをdefaultにしない");
        keyed = ClipEffects{};
        keyed.volumeKeys = {{5, 100.0}};
        check(!clipEffectsAreDefault(keyed), "音量キーだけのeffectをdefaultにしない");
        keyed = ClipEffects{};
        keyed.volumePercent = 50.0;
        check(!clipEffectsAreDefault(keyed), "音量だけのeffectをdefaultにしない");
    }
    {
        // 音量の 1dB 刻み。期待値は 10^(±1/20) を独立に計算した値 (実装の式を呼ばない)。
        const auto near = [](std::optional<double> value, double expected) {
            return value && std::abs(*value - expected) < 1e-9;
        };
        check(near(stepVolumePercentByDb(100.0, 1.0), 112.20184543019634), "音量を+1dBできません");
        check(near(stepVolumePercentByDb(100.0, -1.0), 89.12509381337456), "音量を-1dBできません");
        check(near(stepVolumePercentByDb(0.0, 1.0), 0.11220184543019634),
              "0%から上げると下限(-60dB)から+1dBになりません");
        check(near(stepVolumePercentByDb(0.0, -1.0), 0.0), "0%を下げたときに値を変えました");
        check(near(stepVolumePercentByDb(0.1, -1.0), 0.1), "下限の音量を下限より下げました");
        check(near(stepVolumePercentByDb(0.105, -1.0), 0.1),
              "下限の近くから下げたときに下限で止まりません");
        check(near(stepVolumePercentByDb(190.0, 1.0), 200.0), "音量を上限で止めません");
        check(near(stepVolumePercentByDb(200.0, 1.0), 200.0), "上限の音量を上げました");
        check(!stepVolumePercentByDb(-1.0, 1.0) && !stepVolumePercentByDb(201.0, -1.0) &&
                  !stepVolumePercentByDb(std::nan(""), 1.0) && !stepVolumePercentByDb(100.0, 0.0) &&
                  !stepVolumePercentByDb(100.0, std::nan("")),
              "範囲外の音量・段差を受理しました");
    }
    std::string error;
    check(validateClipEffects(effects, 100, error), "既定effectが有効");
    effects.cropLeftPercent = 10;
    effects.cropTopPercent = 20;
    effects.cropRightPercent = 30;
    effects.cropBottomPercent = 10;
    effects.scaleXPercent = effects.scaleYPercent = 60;
    effects.positionXPercent = 15;
    effects.positionYPercent = -5;
    effects.rotationDegrees = 25;
    effects.opacityPercent = 55;
    effects.fadeInFrames = 12;
    effects.fadeOutFrames = 18;
    check(validateClipEffects(effects, 100, error), "組合せeffectが有効");
    const auto mapping = mapClipEffects(effects);
    check(std::abs(mapping.sourceRect.x - 0.1) < 1e-9 &&
              std::abs(mapping.sourceRect.width - 0.6) < 1e-9,
          "asymmetric cropをsource UVへ写す");
    check(std::abs(mapping.destinationRect.width - 0.36) < 1e-9 &&
              std::abs(mapping.destinationRect.x - 0.37) < 1e-9,
          "post-crop中心を維持してscaleしpositionを加える");
    check(std::abs(mapping.baseOpacity - 0.55) < 1e-9 && mapping.fadeInFrames == 12,
          "opacityとsource-native fadeを写す");

    ClipEffects invalid = effects;
    invalid.cropRightPercent = 90;
    check(!validateClipEffects(invalid, 100, error), "左右crop合計100%以上を拒否する");
    invalid = effects;
    invalid.fadeInFrames = 90;
    invalid.fadeOutFrames = 20;
    check(!validateClipEffects(invalid, 100, error), "source-native fade重複を拒否する");
    invalid = effects;
    invalid.scaleXPercent = invalid.scaleYPercent = std::numeric_limits<double>::quiet_NaN();
    check(!validateClipEffects(invalid, 100, error), "非有限値を拒否する");
    for (const double scale : {0.0, 0.5, 1000.5, std::numeric_limits<double>::infinity()}) {
        invalid = effects;
        invalid.scaleYPercent = scale;
        check(!validateClipEffects(invalid, 100, error), "範囲外の拡大率Yを拒否する");
        invalid = effects;
        invalid.scaleXPercent = scale;
        check(!validateClipEffects(invalid, 100, error), "範囲外の拡大率Xを拒否する");
    }

    // 縦横の拡大率は別々に効く。crop 範囲の中心は保ったまま、幅と高さを別の倍率にする。
    {
        ClipEffects stretched;
        stretched.scaleXPercent = 150;
        stretched.scaleYPercent = 50;
        check(validateClipEffects(stretched, 100, error), "縦横別の拡大率が有効");
        const auto mapped = mapClipEffects(stretched);
        check(std::abs(mapped.destinationRect.width - 1.5) < 1e-9 &&
                  std::abs(mapped.destinationRect.height - 0.5) < 1e-9 &&
                  std::abs(mapped.destinationRect.x + 0.25) < 1e-9 &&
                  std::abs(mapped.destinationRect.y - 0.25) < 1e-9,
              "縦横別の拡大率を中心を保って写す");
    }

    // preview の枠: 見えている矩形 (出力画素) と、その逆 (矩形 -> 位置と拡大率)。
    {
        const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
        const auto full = clipVisualGeometry(ClipEffects{}, 1920, 1080, 1920, 1080);
        check(full.valid && near(full.x, 0) && near(full.y, 0) && near(full.width, 1920) &&
                  near(full.height, 1080) && near(full.pivotX, 960),
              "既定effectの枠が出力全体になりません");
        // 4:3 素材は左右に 240px の余白があり、枠は素材の見えている範囲だけを囲む。
        const auto pillar = clipVisualGeometry(ClipEffects{}, 1440, 1080, 1920, 1080);
        check(pillar.valid && near(pillar.x, 240) && near(pillar.width, 1440),
              "4:3素材の枠がletterboxの余白を含みました");

        ClipEffects moved;
        check(effectsForVisualRect(moved, 1920, 1080, 1920, 1080, 100, 50, 960, 270) &&
                  near(moved.scaleXPercent, 50) && near(moved.scaleYPercent, 25),
              "枠の大きさから縦横の拡大率を決められません");
        const auto movedGeometry = clipVisualGeometry(moved, 1920, 1080, 1920, 1080);
        check(movedGeometry.valid && near(movedGeometry.x, 100) && near(movedGeometry.y, 50) &&
                  near(movedGeometry.width, 960) && near(movedGeometry.height, 270),
              "枠から決めた効果で同じ枠に戻りません");

        // crop・回転・letterbox があっても往復する。回転と crop は変えない。
        ClipEffects complex;
        complex.cropLeftPercent = 5;
        complex.cropTopPercent = 10;
        complex.cropRightPercent = 20;
        complex.rotationDegrees = 30;
        complex.scaleXPercent = 80;
        complex.scaleYPercent = 120;
        const auto before = clipVisualGeometry(complex, 1440, 1080, 1920, 1080);
        ClipEffects rebuilt = complex;
        check(before.valid &&
                  effectsForVisualRect(rebuilt, 1440, 1080, 1920, 1080, 300, 200, 700, 500),
              "crop付きの枠から効果を決められません");
        const auto after = clipVisualGeometry(rebuilt, 1440, 1080, 1920, 1080);
        check(after.valid && near(after.x, 300) && near(after.y, 200) && near(after.width, 700) &&
                  near(after.height, 500) && near(rebuilt.rotationDegrees, 30) &&
                  near(rebuilt.cropLeftPercent, 5) && validateClipEffects(rebuilt, 100, error),
              "crop・回転付きの枠が往復しません");

        // 負例: 大きさ 0 や NaN の枠、余白だけの crop は受け付けず effects を変えない。
        ClipEffects unchanged = complex;
        check(!effectsForVisualRect(unchanged, 1440, 1080, 1920, 1080, 0, 0, 0, 100) &&
                  !effectsForVisualRect(unchanged, 1440, 1080, 1920, 1080,
                                        std::numeric_limits<double>::quiet_NaN(), 0, 10, 10) &&
                  !effectsForVisualRect(unchanged, 0, 1080, 1920, 1080, 0, 0, 10, 10) &&
                  unchanged == complex,
              "不正な枠でeffectsが変わりました");
        ClipEffects marginOnly;
        marginOnly.cropRightPercent = 95; // 4:3 素材の左の余白 (0..12.5%) だけが残る
        check(!clipVisualGeometry(marginOnly, 1440, 1080, 1920, 1080).valid,
              "何も見えないcropで枠を出しました");
        // 対照: 同じ crop でも 16:9 素材なら見えている。
        check(clipVisualGeometry(marginOnly, 1920, 1080, 1920, 1080).valid,
              "見えている素材の枠を出しません (対照群)");
    }

    Project project = projectWithClip();
    effects.scaleYPercent = 45; // X と違う値でも round-trip すること
    effects.opacityKeys = {{20, 80.0, KeyInterpolation::EaseInOut, 0.1, 0.9},
                           {80, 20.123456789123}};
    effects.positionXKeys = {{0, -20, KeyInterpolation::EaseOut}, {90, 30}};
    effects.rotationKeys = {{0, -45}, {90, 90}};
    project.timelineClips.front().effects = effects;
    const auto path = directory / "effects.mvm";
    check(saveProjectJson(project, path).success, "effects付きProjectを保存する");
    const auto loaded = loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips.front().effects == effects,
          "effectsをJSON round-tripする");
    {
        std::ifstream saved(path, std::ios::binary);
        const std::string originalJson((std::istreambuf_iterator<char>(saved)),
                                       std::istreambuf_iterator<char>());
        const auto loadVariant = [&](const char* name, const std::string& text) {
            const auto variantPath = directory / name;
            std::ofstream output(variantPath, std::ios::binary);
            output << text;
            output.close();
            return loadProjectJson(variantPath);
        };
        // 対照: 保存したままの JSON は読める。負例はここから 1 か所だけ変える。
        check(loadVariant("scale-control.mvm", originalJson).success,
              "拡大率の負例の対照群を読み込めません");
        // 旧 schema (10) と、縦横共通の拡大率 (scale_percent) は読み替えずに拒否する。
        auto oldSchema = originalJson;
        const auto versionAt = oldSchema.find("\"schema_version\": 18");
        check(versionAt != std::string::npos, "負例のschema_version位置が保存されていません");
        if (versionAt != std::string::npos) {
            oldSchema.replace(versionAt, std::string("\"schema_version\": 18").size(),
                              "\"schema_version\": 14");
            check(!loadVariant("scale-schema12.mvm", oldSchema).success, "schema 14を拒否する");
        }
        auto uniformScale = originalJson;
        const auto scaleXAt = uniformScale.find("\"scale_x_percent\"");
        check(scaleXAt != std::string::npos, "負例のscale_x_percent位置が保存されていません");
        if (scaleXAt != std::string::npos) {
            uniformScale.replace(scaleXAt, std::string("\"scale_x_percent\"").size(),
                                 "\"scale_percent\"");
            check(!loadVariant("scale-uniform.mvm", uniformScale).success,
                  "縦横共通のscale_percentを拒否する");
        }
        auto malformedJson = originalJson;
        const auto secondKey = malformedJson.find("\"frame\":80");
        check(secondKey != std::string::npos, "負例のキー位置が保存されていません");
        if (secondKey != std::string::npos) {
            malformedJson.replace(secondKey, std::string("\"frame\":80").size(), "\"frame\":20");
            const auto duplicatePath = directory / "duplicate-key.mvm";
            std::ofstream output(duplicatePath, std::ios::binary);
            output << malformedJson;
            output.close();
            check(!loadProjectJson(duplicatePath).success, "JSONの同一frameキーを拒否する");
        }
    }

    const auto legacy = directory / "legacy.mvm";
    std::ofstream legacyFile(legacy);
    legacyFile
        << R"({"schema_version":4,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false,"solo":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","media_item_id":"m-a","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"track_kind":"video","track_index":0}]})";
    legacyFile.close();
    const auto legacyLoaded = loadProjectJson(legacy);
    check(!legacyLoaded.success, "schema 4を拒否する");

    const auto partial = directory / "partial.mvm";
    std::ofstream partialFile(partial);
    partialFile
        << R"({"schema_version":16,"timeline_markers":[],"timeline_transitions":[],"in_frame":null,"out_frame":null,"format":"mvm-project","media_folders":[],"media_items":[{"id":"m-a","kind":"video","media_path":"a.mp4","name":"a","folder_id":"","fps_num":60,"fps_den":1,"frame_count":10,"width":1920,"height":1080,"sample_rate":0,"duration_samples":0}],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false,"solo":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","media_item_id":"m-a","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"speed_num":1,"speed_den":1,"preserve_pitch":false,"enabled":true,"frame_hold":null,"track_kind":"video","track_index":0,"effects":{"scale_x_percent":60}}]})";
    partialFile.close();
    check(!loadProjectJson(partial).success, "部分effects objectをfail-closedで拒否する");

    // schema 7 以降の clip は速度 (speed_num / speed_den) を必須にする。どの負例も対照群から
    // 1 か所だけ変えて作るので、壊した箇所で落ちていることが分かる。
    const auto speedProject = [&](const std::string& version, const std::string& speed) {
        return R"({"schema_version":)" + version +
               R"(,"timeline_markers":[],"timeline_transitions":[],"in_frame":null,"out_frame":null,"format":"mvm-project","media_folders":[],"media_items":[{"id":"m-a","kind":"video","media_path":"a.mp4","name":"a","folder_id":"","fps_num":60,"fps_den":1,"frame_count":10,"width":1920,"height":1080,"sample_rate":0,"duration_samples":0}],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false,"solo":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","media_item_id":"m-a","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,)" +
               speed +
               R"("preserve_pitch":false,"enabled":true,"frame_hold":null,"track_kind":"video","track_index":0}]})";
    };
    const auto loadText = [&](const char* name, const std::string& text) {
        const auto projectPath = directory / name;
        std::ofstream file(projectPath);
        file << text;
        file.close();
        return loadProjectJson(projectPath);
    };
    const auto halfSpeed =
        loadText("speed-half.mvm", speedProject("16", R"("speed_num":1,"speed_den":2,)"));
    check(halfSpeed.success && halfSpeed.project.timelineClips.size() == 1 &&
              halfSpeed.project.timelineClips[0].speedNum == 1 &&
              halfSpeed.project.timelineClips[0].speedDen == 2,
          "対照群: 50%のclipを読めない");
    check(!loadText("schema5.mvm", speedProject("5", R"("speed_num":1,"speed_den":2,)")).success,
          "schema 5を拒否する");
    // schema 8 で image clip を足した。版を上げたので旧版は読まない (互換分岐を持たない)。
    check(!loadText("schema7.mvm", speedProject("7", R"("speed_num":1,"speed_den":2,)")).success,
          "schema 7を拒否する");
    // 旧ファイルは速度の field を持たない。後続の field で落ちる前に、版の違いとして報告する。
    const auto legacyClip = loadText("schema5-legacy.mvm", speedProject("5", ""));
    check(!legacyClip.success &&
              legacyClip.error.find("対応していない schema_version です: 5") != std::string::npos,
          (std::string("schema 5の旧ファイルを版の違いとして報告しない: ") + legacyClip.error)
              .c_str());
    check(!loadText("speed-missing.mvm", speedProject("16", "")).success,
          "速度の無いclipを既定値で受理しない");
    check(
        !loadText("speed-slow.mvm", speedProject("16", R"("speed_num":1,"speed_den":11,)")).success,
        "10%未満の速度を拒否する");
    check(
        !loadText("speed-fast.mvm", speedProject("16", R"("speed_num":11,"speed_den":1,)")).success,
        "1000%を超える速度を拒否する");
    check(
        loadText("speed-edge.mvm", speedProject("16", R"("speed_num":10,"speed_den":1,)"))
                .success &&
            loadText("speed-edge-slow.mvm", speedProject("16", R"("speed_num":1,"speed_den":10,)"))
                .success,
        "10%と1000%ちょうどを受理する");
    check(!loadText("speed-unreduced.mvm", speedProject("16", R"("speed_num":2,"speed_den":4,)"))
               .success,
          "約分されていない速度を拒否する");

    {
        // fps の違う Project へ移した key は同じ秒位置へ来る。0.5 秒 / 1 秒 / 末尾。
        std::vector<ClipKeyframe> keys = {{0, 100.0}, {30, 50.0}, {60, 20.0}, {119, 0.0}};
        check(retimeClipKeys(keys, 60, 1, 30, 1, 60) &&
                  keys == std::vector<ClipKeyframe>{{0, 100.0}, {15, 50.0}, {30, 20.0}, {59, 0.0}},
              "60fps -> 30fps で key を同じ秒位置へ移す (尺を超えた末尾は最終 frame)");
        check(retimeClipKeys(keys, 30, 1, 60, 1, 120) &&
                  keys == std::vector<ClipKeyframe>{{0, 100.0}, {30, 50.0}, {60, 20.0}, {118, 0.0}},
              "30fps -> 60fps で key を同じ秒位置へ移す");
        keys = {{9, 10.0}, {10, 20.0}, {11, 30.0}};
        check(retimeClipKeys(keys, 60, 1, 30, 1, 60) &&
                  keys == std::vector<ClipKeyframe>{{5, 10.0}, {6, 30.0}},
              "同じ frame に重なった key は先の 1 つを残し、ちょうど 1/2 は後ろへ丸める");
        keys = {{30, 40.0}};
        check(retimeClipKeys(keys, 30000, 1001, 30, 1, 60) &&
                  keys == std::vector<ClipKeyframe>{{30, 40.0}},
              "29.97fps -> 30fps で最も近い frame へ移す");
        const std::vector<ClipKeyframe> original = {{3, 50.0}};
        keys = original;
        check(!retimeClipKeys(keys, 0, 1, 30, 1, 60) && keys == original,
              "不正な fps では失敗し、key を変えない");
    }

    return failures == 0 ? 0 : 1;
}
