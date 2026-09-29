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
    effects.opacityKeys = {{20, 80.0}, {80, 20.123456789123}};
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
        // 旧 schema (9) と、縦横共通の拡大率 (scale_percent) は読み替えずに拒否する。
        auto oldSchema = originalJson;
        const auto versionAt = oldSchema.find("\"schema_version\": 10");
        check(versionAt != std::string::npos, "負例のschema_version位置が保存されていません");
        if (versionAt != std::string::npos) {
            oldSchema.replace(versionAt, std::string("\"schema_version\": 10").size(),
                              "\"schema_version\": 9");
            check(!loadVariant("scale-schema9.mvm", oldSchema).success, "schema 9を拒否する");
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
        << R"({"schema_version":4,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"track_kind":"video","track_index":0}]})";
    legacyFile.close();
    const auto legacyLoaded = loadProjectJson(legacy);
    check(!legacyLoaded.success, "schema 4を拒否する");

    const auto partial = directory / "partial.mvm";
    std::ofstream partialFile(partial);
    partialFile
        << R"({"schema_version":10,"timeline_markers":[],"in_frame":null,"out_frame":null,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"speed_num":1,"speed_den":1,"track_kind":"video","track_index":0,"effects":{"scale_x_percent":60}}]})";
    partialFile.close();
    check(!loadProjectJson(partial).success, "部分effects objectをfail-closedで拒否する");

    // schema 7 以降の clip は速度 (speed_num / speed_den) を必須にする。どの負例も対照群から
    // 1 か所だけ変えて作るので、壊した箇所で落ちていることが分かる。
    const auto speedProject = [&](const std::string& version, const std::string& speed) {
        return R"({"schema_version":)" + version +
               R"(,"timeline_markers":[],"in_frame":null,"out_frame":null,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,)" +
               speed + R"("track_kind":"video","track_index":0}]})";
    };
    const auto loadText = [&](const char* name, const std::string& text) {
        const auto projectPath = directory / name;
        std::ofstream file(projectPath);
        file << text;
        file.close();
        return loadProjectJson(projectPath);
    };
    const auto halfSpeed =
        loadText("speed-half.mvm", speedProject("10", R"("speed_num":1,"speed_den":2,)"));
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
    check(!loadText("speed-missing.mvm", speedProject("10", "")).success,
          "速度の無いclipを既定値で受理しない");
    check(
        !loadText("speed-slow.mvm", speedProject("10", R"("speed_num":1,"speed_den":11,)")).success,
        "10%未満の速度を拒否する");
    check(
        !loadText("speed-fast.mvm", speedProject("10", R"("speed_num":11,"speed_den":1,)")).success,
        "1000%を超える速度を拒否する");
    check(
        loadText("speed-edge.mvm", speedProject("10", R"("speed_num":10,"speed_den":1,)"))
                .success &&
            loadText("speed-edge-slow.mvm", speedProject("10", R"("speed_num":1,"speed_den":10,)"))
                .success,
        "10%と1000%ちょうどを受理する");
    check(!loadText("speed-unreduced.mvm", speedProject("10", R"("speed_num":2,"speed_den":4,)"))
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
