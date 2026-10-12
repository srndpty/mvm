// P3-5A: 数式 group の transition (src/media/mlt/mvm_mlt_math_group.c) を、検出器の下で MLT と
// 一緒に動かす隔離 harness。Linux の gcc で、MLT 7.36.1 の framework と core module を同じ検出器
// 付きで build したものへ link する (scripts/math-group-p35a-sanitizers.sh)。
//
// 対象: 実物の mvm_mlt_math_group.c と math_group_compose.cpp。
// 対象外: mvm_mlt_export.c (Windows の API と、Qt・FFmpeg の MLT module に依存する)。graph の
// 組み立て・待ち loop・閉じる順は、この file が mvm_mlt_export.c と同じ順で書き直している。
//
// 使い方: math_group_mlt_lifecycle <MLT の module の directory> <scenario> [worker の数]
//   normal          書き出しを同じ process で 3 回。全 frame の画素を 1 thread の参照と比べる
//   cancel          途中で consumer を止める。止めた後に callback が走らないこと
//   error           下地が対象外 (alpha 0)。全 worker が同時に失敗を記録する。最初の失敗の公開
//   init-failure    transition を作る途中の失敗 (途中まで確保した状態の解放)
//   frame-outlives  frame が transition より長く生きる (frame が持つ参照が状態を保つ)
//   control-frame-ref     対照: frame の参照を外してから transition を閉じる → 解放後の参照
//   control-raster-freed  対照: 画像を解放してから合成する → 解放後の参照
//   control-shared-write  対照: 合成の callback が共有の変数へ同期なしで書く → data race
//   mlt-color-race  MLT 7.36.1 の color producer の data race の記録 (下の prewarm を参照)。
//                   worker が 2 本以上のとき、検出器が MLT の中の race を報告する
// 終了コード: 0 = 期待どおり、1 = 不成立。対照は検出器が報告して異常終了する (報告が無ければ 1)。

#include "media/math/math_group_compose.h"

extern "C" {
#include "media/mlt/mvm_mlt_math_group.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" long long mvm_mlt_export_observer_now_ns(void) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

namespace {

using mvm::math::PreparedMathRaster;

constexpr int kWidth = 320;
constexpr int kHeight = 180;
constexpr int kFrames = 240;
constexpr int kLayers = 6;

int failures = 0;

// MLT 7.36.1 の color producer (producer_colour.c) は、画像の cache を「作った」と記録して lock を
// 外してから画素を埋める。同時に来た別の worker は、埋まる前の cache を複製して返すことがある
// (P3-5A で検出器が報告し、ASan の build では実際に下地が未初期化になった)。
// これは数式 group の code の外の問題なので、既定では consumer を始める前に 1 thread で cache を
// 作っておき (prewarm)、数式 group の code だけを見る。scenario mlt-color-race は prewarm を外す。
bool gPrewarmColor = true;

void require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

// 書き出しが MLT へ渡すのと同じ形の入力 (clip ごとの画像と、frame ごとの位置・不透明度)。
struct Scene {
    std::vector<std::unique_ptr<PreparedMathRaster>> rasters;
    std::vector<std::vector<MvmExportOpacityKeyframe>> opacity;
    std::vector<std::vector<MvmExportMotionFrame>> motion;
    std::vector<MvmExportClip> clips;
    std::vector<const MvmExportClip*> members;
};

Scene makeScene() {
    Scene scene;
    for (int layer = 0; layer < kLayers; ++layer) {
        // 字形の代わりの模様: 中央の矩形に、端が半透明の画素。外側は全部 0。
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
        for (int y = 40 + layer * 5; y < 100 + layer * 5; ++y)
            for (int x = 60 + layer * 20; x < 140 + layer * 20; ++x) {
                auto* pixel = &rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4];
                pixel[0] = static_cast<std::uint8_t>(40 * layer + x);
                pixel[1] = static_cast<std::uint8_t>(200 - 30 * layer);
                pixel[2] = static_cast<std::uint8_t>(y * 3);
                pixel[3] = static_cast<std::uint8_t>((x + y) % 7 == 0 ? 90 : 255);
            }
        auto raster = std::make_unique<PreparedMathRaster>();
        std::string error;
        if (!PreparedMathRaster::prepare(rgba.data(), kWidth, kHeight,
                                         static_cast<std::size_t>(kWidth) * 4, *raster, error)) {
            std::fprintf(stderr, "画像を準備できません: %s\n", error.c_str());
            std::exit(2);
        }
        scene.rasters.push_back(std::move(raster));
        auto& opacity = scene.opacity.emplace_back();
        auto& motion = scene.motion.emplace_back();
        // layer ごとに始まりをずらす (見えている member の数が frame で変わる)。
        const int start = layer * 10;
        const int frames = kFrames - start;
        for (int frame = 0; frame < frames; ++frame) {
            opacity.push_back({frame, 0.25 + 0.75 * ((frame * 7 + layer * 13) % 100) / 100.0});
            MvmExportMotionFrame value{};
            value.local_frame = frame;
            value.rect_x = -20.0 + frame * 0.37 + layer * 1.5; // 端数の位置
            value.rect_y = 10.0 - frame * 0.21 + layer * 0.75;
            value.rect_width = kWidth;
            value.rect_height = kHeight;
            motion.push_back(value);
        }
    }
    for (int layer = 0; layer < kLayers; ++layer) {
        MvmExportClip clip{};
        clip.path = "member";
        clip.video_track = layer + 1;
        clip.is_still_image = 1;
        clip.timeline_start_frame = layer * 10;
        clip.timeline_duration_frames = kFrames - layer * 10;
        clip.rect_width = kWidth;
        clip.rect_height = kHeight;
        clip.opacity_keyframes = scene.opacity[static_cast<std::size_t>(layer)].data();
        clip.opacity_keyframe_count = static_cast<int>(clip.timeline_duration_frames);
        clip.motion_frames = scene.motion[static_cast<std::size_t>(layer)].data();
        clip.motion_frame_count = static_cast<int>(clip.timeline_duration_frames);
        clip.math_group_raster = scene.rasters[static_cast<std::size_t>(layer)].get();
        scene.clips.push_back(clip);
    }
    for (const auto& clip : scene.clips)
        scene.members.push_back(&clip);
    return scene;
}

std::uint64_t digest(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t value = 0xcbf29ce484222325ull;
    for (std::size_t index = 0; index < size; ++index)
        value = (value ^ bytes[index]) * 0x100000001b3ull;
    return value;
}

// 参照: 黒 (alpha 255) の下地へ、その frame で見えている member を 1 thread で合成する。
std::uint64_t referenceDigest(const Scene& scene, int frame) {
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
    for (std::size_t index = 3; index < canvas.size(); index += 4)
        canvas[index] = 255;
    std::vector<mvm::math::MathGroupPlacement> placements;
    for (const auto& clip : scene.clips) {
        const long long local = frame - clip.timeline_start_frame;
        if (local < 0 || local >= clip.timeline_duration_frames)
            continue;
        placements.push_back({static_cast<const PreparedMathRaster*>(clip.math_group_raster),
                              clip.motion_frames[local].rect_x, clip.motion_frames[local].rect_y,
                              clip.opacity_keyframes[local].opacity});
    }
    std::string error;
    if (!placements.empty() &&
        !mvm::math::composeMathGroup(canvas.data(), kWidth, kHeight,
                                     static_cast<std::size_t>(kWidth) * 4, placements.data(),
                                     placements.size(), error)) {
        std::fprintf(stderr, "参照を合成できません: %s\n", error.c_str());
        std::exit(2);
    }
    return digest(canvas.data(), canvas.size());
}

// timeline_export.cpp の composeMathGroupForExport と同じ (状態を持たない)。
long gUnsafeShared = 0; // 対照 control-shared-write だけが書く
bool gUnsafeWrite = false;

int composeCallback(void*, unsigned char* rgba, int width, int height, std::size_t stride,
                    const MvmExportMathGroupLayer* layers, int count, int, char* err,
                    std::size_t errSize) {
    if (gUnsafeWrite)
        ++gUnsafeShared; // 対照: render worker どうしで同期なしに書く
    std::vector<mvm::math::MathGroupPlacement> placements(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        placements[static_cast<std::size_t>(index)] = {
            static_cast<const PreparedMathRaster*>(layers[index].raster), layers[index].rect_x,
            layers[index].rect_y, layers[index].opacity};
    std::string error;
    if (mvm::math::composeMathGroup(rgba, width, height, stride, placements.data(),
                                    placements.size(), error))
        return 0;
    if (err && errSize > 0)
        std::snprintf(err, errSize, "%s", error.c_str());
    return 1;
}

MvmExportObserver makeObserver() {
    MvmExportObserver observer{};
    observer.diagnostic_math_group = 1;
    observer.diagnostic_math_group_compose = &composeCallback;
    return observer;
}

// 出力の直前の画素の検査 (mvm_mlt_export.c の RGBA の検査と同じ形の filter)。
struct Audit {
    const std::vector<std::uint64_t>* expected = nullptr;
    std::atomic<long long> observed{0};
    std::atomic<long long> mismatched{0};
    std::atomic<long long> failedImages{0};
    std::vector<std::atomic<int>> seen;

    Audit() : seen(static_cast<std::size_t>(kFrames)) {}
};

int auditImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height,
               int writable) {
    auto* audit = static_cast<Audit*>(mlt_frame_pop_service(frame));
    const auto position = static_cast<long long>(mlt_frame_get_position(frame));
    *format = mlt_image_rgba;
    const int status = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (status || !*image || *format != mlt_image_rgba || *width != kWidth || *height != kHeight) {
        audit->failedImages.fetch_add(1);
        return status ? status : 1;
    }
    audit->observed.fetch_add(1);
    if (position >= 0 && position < kFrames) {
        audit->seen[static_cast<std::size_t>(position)].fetch_add(1);
        if (audit->expected && digest(*image, static_cast<std::size_t>(kWidth) * kHeight * 4) !=
                                   (*audit->expected)[static_cast<std::size_t>(position)])
            audit->mismatched.fetch_add(1);
    }
    return 0;
}

mlt_frame auditProcess(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter->child);
    mlt_frame_push_get_image(frame, auditImage);
    return frame;
}

struct RunResult {
    bool built = false;
    bool stoppedEarly = false;
    long long callbacksAtStop = 0, callbacksAfterWait = 0;
    long long liveAfter = -1, activeAfter = -1;
};

// 1 回の書き出し。mvm_mlt_export.c と同じ順: graph を作る → consumer を始める → 待つ (失敗・取消を
// 見る) → consumer を止めて閉じる → tractor・playlist・producer を閉じる → 残りを確かめる。
// stopAfter >= 0 なら、その数の画像を観測した時点で consumer を止める (取消)。
RunResult runExport(const Scene& scene, MvmExportObserver& observer, Audit& audit, int workers,
                    const char* background, long long stopAfter) {
    RunResult result;
    mlt_profile profile = mlt_profile_init(nullptr);
    profile->width = kWidth;
    profile->height = kHeight;
    profile->frame_rate_num = 60;
    profile->frame_rate_den = 1;
    profile->sample_aspect_num = profile->sample_aspect_den = 1;
    profile->display_aspect_num = kWidth;
    profile->display_aspect_den = kHeight;
    profile->progressive = 1;
    mlt_producer color = mlt_factory_producer(profile, "color", background);
    mlt_tractor tractor = mlt_tractor_new();
    mlt_playlist base = mlt_playlist_new(profile);
    mlt_playlist empty = mlt_playlist_new(profile);
    mlt_consumer consumer = nullptr;
    char error[256] = {};
    if (color && tractor && base && empty) {
        mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(color), "length", kFrames + 1);
        mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(color), "out", kFrames);
        mlt_playlist_append_io(base, color, 0, kFrames - 1);
        mlt_playlist_blank(empty, kFrames - 1); // member は playlist に置かない
        mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(base), 0);
        mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(empty), 1);
        mlt_transition transition =
            mvm_mlt_math_group_new(scene.members.data(), static_cast<int>(scene.members.size()), 1,
                                   kWidth, kHeight, &observer, error, sizeof(error));
        if (transition) {
            mlt_transition_set_tracks(transition, 0, 1);
            result.built =
                mlt_field_plant_transition(mlt_tractor_field(tractor), transition, 0, 1) == 0;
            mlt_transition_close(transition); // 以降は field と frame が参照を持つ
        }
    }
    if (result.built && gPrewarmColor) {
        mlt_frame warm = nullptr;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(color), &warm, 0) == 0 && warm) {
            uint8_t* image = nullptr;
            mlt_image_format format = mlt_image_rgba;
            int width = kWidth, height = kHeight;
            mlt_frame_get_image(warm, &image, &format, &width, &height, 0);
            mlt_frame_close(warm);
        }
    }
    if (result.built) {
        mlt_producer output = MLT_TRACTOR_PRODUCER(tractor);
        mlt_producer_set_in_and_out(output, 0, kFrames - 1);
        mlt_producer_seek(output, 0);
        mlt_filter filter = mlt_filter_new();
        filter->child = &audit;
        filter->process = auditProcess;
        mlt_service_attach(MLT_PRODUCER_SERVICE(output), filter);
        mlt_filter_close(filter);
        consumer = mlt_factory_consumer(profile, "null", nullptr);
        result.built = consumer != nullptr;
        if (consumer) {
            mlt_properties properties = MLT_CONSUMER_PROPERTIES(consumer);
            mlt_properties_set_int(properties, "real_time", -workers);
            mlt_properties_set_int(properties, "terminate_on_pause", 1);
            mlt_properties_set(properties, "mlt_image_format", "rgba");
            result.built = mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(output)) == 0 &&
                           mlt_consumer_start(consumer) == 0;
        }
    }
    if (result.built) {
        while (!mlt_consumer_is_stopped(consumer)) {
            if (__atomic_load_n(&observer.math_group_errors, __ATOMIC_SEQ_CST) != 0 ||
                (stopAfter >= 0 && audit.observed.load() >= stopAfter)) {
                result.stoppedEarly = true;
                mlt_consumer_stop(consumer);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
        // consumer を閉じた後は、callback が走らないこと。
        result.callbacksAtStop = __atomic_load_n(&observer.math_group_callbacks, __ATOMIC_SEQ_CST);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        result.callbacksAfterWait =
            __atomic_load_n(&observer.math_group_callbacks, __ATOMIC_SEQ_CST);
    }
    if (tractor)
        mlt_tractor_close(tractor);
    if (empty)
        mlt_playlist_close(empty);
    if (base)
        mlt_playlist_close(base);
    if (color)
        mlt_producer_close(color);
    mlt_profile_close(profile);
    result.liveAfter = mvm_mlt_math_group_live();
    result.activeAfter = mvm_mlt_math_group_active();
    return result;
}

void requireClean(const RunResult& result) {
    require(result.built, "graph と consumer を作れること");
    require(result.callbacksAtStop == result.callbacksAfterWait,
            "consumer を閉じた後に callback が走らないこと");
    require(result.liveAfter == 0, "graph を閉じた後に transition の状態が残らないこと");
    require(result.activeAfter == 0, "graph を閉じた後に実行中の callback が無いこと");
}

std::vector<std::uint64_t> references(const Scene& scene) {
    std::vector<std::uint64_t> expected;
    for (int frame = 0; frame < kFrames; ++frame)
        expected.push_back(referenceDigest(scene, frame));
    return expected;
}

void scenarioNormal(int workers) {
    const Scene scene = makeScene();
    const auto expected = references(scene);
    for (int repeat = 0; repeat < 3; ++repeat) { // 同じ process で続けて書き出す
        auto observer = makeObserver();
        Audit audit;
        audit.expected = &expected;
        const auto result = runExport(scene, observer, audit, workers, "#000000", -1);
        requireClean(result);
        require(!result.stoppedEarly, "途中で止まらないこと");
        require(observer.math_group_errors == 0, "group の失敗が無いこと");
        require(audit.failedImages.load() == 0, "画像の取得に失敗しないこと");
        require(audit.mismatched.load() == 0, "全 frame の画素が 1 thread の参照と同じこと");
        int missing = 0;
        for (const auto& seen : audit.seen)
            missing += seen.load() == 0;
        require(missing == 0, "全 frame を観測すること");
        require(observer.math_group_frames >= kFrames, "全 frame を合成すること");
        std::printf("normal workers=%d repeat=%d callbacks=%lld composed=%lld observed=%lld\n",
                    workers, repeat, static_cast<long long>(observer.math_group_callbacks),
                    static_cast<long long>(observer.math_group_frames), audit.observed.load());
    }
}

void scenarioCancel(int workers) {
    const Scene scene = makeScene();
    const auto expected = references(scene);
    for (const long long stopAfter : {0LL, 1LL, 8LL, 100LL}) {
        auto observer = makeObserver();
        Audit audit;
        audit.expected = &expected;
        const auto result = runExport(scene, observer, audit, workers, "#000000", stopAfter);
        requireClean(result);
        require(audit.mismatched.load() == 0, "止めるまでに出た frame の画素が参照と同じこと");
        require(observer.math_group_errors == 0, "取消で group の失敗を記録しないこと");
        std::printf("cancel workers=%d after=%lld callbacks=%lld observed=%lld\n", workers,
                    stopAfter, static_cast<long long>(observer.math_group_callbacks),
                    audit.observed.load());
    }
}

void scenarioError(int workers) {
    // 下地が透明 (alpha 0): 合成器が全 frame を拒否する。全 worker が同時に失敗を記録する。
    const Scene scene = makeScene();
    for (int repeat = 0; repeat < 5; ++repeat) {
        auto observer = makeObserver();
        Audit audit;
        const auto result = runExport(scene, observer, audit, workers, "#00000000", -1);
        requireClean(result);
        require(result.stoppedEarly, "失敗を見て consumer を止めること");
        require(observer.math_group_errors > 0, "失敗を数えること");
        require(observer.math_group_error_ready == 1, "最初の失敗を公開すること");
        require(std::strstr(observer.math_group_error, "合成器が拒否") != nullptr,
                "最初の失敗の文が完全であること");
        require(observer.math_group_frames == 0, "拒否した frame を合成済みとして数えないこと");
        std::printf("error workers=%d repeat=%d errors=%lld frame=%lld\n", workers, repeat,
                    static_cast<long long>(observer.math_group_errors),
                    observer.math_group_error_frame);
    }
}

void scenarioInitFailure() {
    Scene scene = makeScene();
    auto observer = makeObserver();
    char error[256] = {};
    // 4 番目の member が対象外 (crop): 3 つ分を確保した後で失敗する。
    scene.clips[3].crop_left = 4;
    require(mvm_mlt_math_group_new(scene.members.data(), kLayers, 1, kWidth, kHeight, &observer,
                                   error, sizeof(error)) == nullptr,
            "対象外の member があれば作らないこと");
    require(mvm_mlt_math_group_live() == 0, "途中まで確保した状態を解放すること");
    scene.clips[3].crop_left = 0;
    // 合成の callback が無い。
    auto noCompose = makeObserver();
    noCompose.diagnostic_math_group_compose = nullptr;
    require(mvm_mlt_math_group_new(scene.members.data(), kLayers, 1, kWidth, kHeight, &noCompose,
                                   error, sizeof(error)) == nullptr,
            "合成の callback が無ければ作らないこと");
    require(mvm_mlt_math_group_new(scene.members.data(), 0, 1, kWidth, kHeight, &observer, error,
                                   sizeof(error)) == nullptr,
            "member が 0 なら作らないこと");
    // 作ってから、使わずに閉じる (graph を作る途中の失敗と同じ道)。
    mlt_transition unused = mvm_mlt_math_group_new(scene.members.data(), kLayers, 1, kWidth,
                                                   kHeight, &observer, error, sizeof(error));
    require(unused != nullptr, "正しい入力なら作れること");
    require(mvm_mlt_math_group_live() == 1, "作った状態を数えること");
    if (unused)
        mlt_transition_close(unused);
    require(mvm_mlt_math_group_live() == 0, "使わずに閉じた状態を解放すること");
    std::printf("init-failure ok\n");
}

// frame を 1 枚だけ手で作り、transition を通す (consumer を使わない)。
// mode 0: frame が transition より長く生きる (正しい使い方。frame の参照が状態を保つ)。
// mode 1: 対照。frame の参照を外してから transition を閉じる。
// mode 2: 対照。画像を解放してから合成する。
void scenarioManual(int mode) {
    Scene scene = makeScene();
    const auto expected = referenceDigest(scene, 0);
    auto observer = makeObserver();
    mlt_profile profile = mlt_profile_init(nullptr);
    profile->width = kWidth;
    profile->height = kHeight;
    profile->sample_aspect_num = profile->sample_aspect_den = 1;
    profile->display_aspect_num = kWidth;
    profile->display_aspect_den = kHeight;
    profile->progressive = 1;
    mlt_producer color = mlt_factory_producer(profile, "color", "#000000");
    char error[256] = {};
    mlt_transition transition = mvm_mlt_math_group_new(scene.members.data(), kLayers, 1, kWidth,
                                                       kHeight, &observer, error, sizeof(error));
    mlt_frame frame = nullptr;
    if (!color || !transition ||
        mlt_service_get_frame(MLT_PRODUCER_SERVICE(color), &frame, 0) != 0 || !frame) {
        std::fprintf(stderr, "frame を作れません\n");
        std::exit(2);
    }
    mlt_frame upper = mlt_frame_init(MLT_PRODUCER_SERVICE(color));
    mlt_transition_process(transition, frame, upper);
    if (mode == 1) {
        // 対照: frame が持つ参照を外す (「frame が参照を持たない」実装と同じ状態)。
        char name[64];
        std::snprintf(name, sizeof(name), "mvm.math_group.%p", static_cast<void*>(transition));
        mlt_properties_set_data(MLT_FRAME_PROPERTIES(frame), name, nullptr, 0, nullptr, nullptr);
    }
    mlt_transition_close(transition); // 呼び出し側の参照を手放す
    if (mode == 0)
        require(mvm_mlt_math_group_live() == 1, "frame が生きている間は状態が残ること");
    if (mode == 2)
        scene.rasters[0].reset(); // 対照: 合成の前に画像を解放する
    uint8_t* image = nullptr;
    mlt_image_format format = mlt_image_rgba;
    int width = kWidth, height = kHeight;
    const int status = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    require(status == 0 && image != nullptr, "画像を取得できること");
    if (status == 0 && image)
        require(digest(image, static_cast<std::size_t>(kWidth) * kHeight * 4) == expected,
                "画素が参照と同じこと");
    mlt_frame_close(upper);
    mlt_frame_close(frame);
    require(mvm_mlt_math_group_live() == 0, "frame を閉じたら状態が解放されること");
    require(mvm_mlt_math_group_active() == 0, "実行中の callback が無いこと");
    mlt_producer_close(color);
    mlt_profile_close(profile);
    std::printf("manual mode=%d finished\n", mode);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <MLT module directory> <scenario> [workers]\n", argv[0]);
        return 2;
    }
    const std::string scenario = argv[2];
    const int workers = argc > 3 ? std::atoi(argv[3]) : 4;
    if (!mlt_factory_init(argv[1])) {
        std::fprintf(stderr, "MLT を初期化できません: %s\n", argv[1]);
        return 2;
    }
    if (scenario == "normal")
        scenarioNormal(workers);
    else if (scenario == "cancel")
        scenarioCancel(workers);
    else if (scenario == "error")
        scenarioError(workers);
    else if (scenario == "init-failure")
        scenarioInitFailure();
    else if (scenario == "frame-outlives")
        scenarioManual(0);
    else if (scenario == "control-frame-ref")
        scenarioManual(1);
    else if (scenario == "control-raster-freed")
        scenarioManual(2);
    else if (scenario == "control-shared-write") {
        gUnsafeWrite = true;
        scenarioNormal(workers);
    } else if (scenario == "mlt-color-race") {
        gPrewarmColor = false;
        scenarioNormal(workers);
    } else {
        std::fprintf(stderr, "不明な scenario: %s\n", scenario.c_str());
        return 2;
    }
    mlt_factory_close();
    if (failures) {
        std::fprintf(stderr, "不成立: %d\n", failures);
        return 1;
    }
    std::printf("%s: 成立\n", scenario.c_str());
    return 0;
}
