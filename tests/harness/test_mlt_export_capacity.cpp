// tractor 書き出しの cut 数の計算が overflow しないこと。
//
// clip 数・track 数の上限を外したので、cut 配列の要素数 (clip ごとに 2 + 末尾補完 frame 数)
// の合計は入力しだいでいくらでも大きくなる。signed overflow は未定義動作なので、
// calloc の失敗では防御にならない。加算前に止め、MLT の producer を開く前に失敗させる。
//
// 実際に巨大な配列を確保しないよう、どれも確保の手前で落ちる入力だけを使う。
// エラー文言まで照合し、「確保に失敗した」(修正前の経路) ではなく
// 「表現範囲を超えた」で止まったことを確かめる。

#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_runtime.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

MvmExportClip videoClip(const char* path, int track, long long producerFrames,
                        long long tailFrames) {
    MvmExportClip clip{};
    clip.path = path;
    clip.source_fps_num = 60;
    clip.source_fps_den = 1;
    clip.source_frame_count = 300;
    clip.source_in_frame = 0;
    clip.source_out_frame = producerFrames;
    clip.producer_in_frame = 0;
    clip.producer_out_frame = producerFrames;
    clip.tail_padding_frames = tailFrames;
    clip.speed_num = 1;
    clip.speed_den = 1;
    clip.video_track = track;
    clip.timeline_start_frame = 0;
    clip.timeline_duration_frames = producerFrames + tailFrames;
    return clip;
}

// 失敗し、err に expected を含み、出力を作っていないこと。
void requireRejected(const MvmExportClip* clips, int count, long long totalDuration,
                     const std::filesystem::path& output, const char* expected,
                     const char* message) {
    const MvmExportSpec spec{320, 240, 60, 1, 23, 10000, 4, 0, nullptr, nullptr, 0};
    char error[512] = {};
    const auto outputUtf8 = output.u8string();
    const int status = mvm_mlt_export_two_track(clips, count, totalDuration, &spec,
                                                reinterpret_cast<const char*>(outputUtf8.c_str()),
                                                nullptr, error, sizeof(error));
    if (status == 0 || std::string(error).find(expected) == std::string::npos)
        std::fprintf(stderr, "status=%d error=%s\n", status, error);
    require(status != 0, message);
    require(std::string(error).find(expected) != std::string::npos, message);
    require(!std::filesystem::exists(output), "拒否した書き出しで出力を作りました");
}

} // namespace

int main() {
    require(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
            "MLT runtime を初期化できません");
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "mvm-export-capacity-should-not-exist.mp4";
    std::filesystem::remove(output);

    // 1. 1 本の clip の 2 + 末尾補完が long long を超える (signed overflow の手前で止める)。
    {
        const MvmExportClip clip = videoClip(MVM_CAPACITY_TEST_VIDEO, 0, 1, LLONG_MAX - 1);
        requireRejected(&clip, 1, LLONG_MAX, output, "cut数が表現範囲を超えています",
                        "cut 数の signed overflow を拒否しません");
    }

    // 2. clip ごとには収まるが、合計が確保できる要素数 (SIZE_MAX / 要素の大きさ) を超える。
    //    1 本目 (2^60 + 2) は通り、2 本目で合計が超えることを見る。
    {
        constexpr long long kTail = 1LL << 60;
        MvmExportClip clips[2] = {videoClip(MVM_CAPACITY_TEST_VIDEO, 0, 1, kTail),
                                  videoClip(MVM_CAPACITY_TEST_VIDEO, 1, 1, kTail)};
        const long long duration = clips[1].timeline_duration_frames;
        const MvmExportOpacityKeyframe keys[] = {{0, 1.0}, {duration - 1, 1.0}};
        clips[1].opacity_keyframes = keys;
        clips[1].opacity_keyframe_count = 2;
        clips[1].rect_width = 320;
        clips[1].rect_height = 240;
        requireRejected(clips, 2, duration, output,
                        "tractor clip 1までのcut数が表現範囲を超えています",
                        "clip をまたいだ cut 数の合計の超過を拒否しません");
    }

    // 3. producer 尺 + 末尾補完の検算自体の signed overflow も手前で止める。
    {
        MvmExportClip clip = videoClip(MVM_CAPACITY_TEST_VIDEO, 0, 1, 0);
        clip.tail_padding_frames = LLONG_MAX;
        clip.timeline_duration_frames = LLONG_MAX;
        requireRejected(&clip, 1, LLONG_MAX, output, "tractor clip 0のmappingが不正です",
                        "producer 尺と末尾補完の和の overflow を拒否しません");
    }

    mvm_mlt_runtime_shutdown();
    std::puts("tractor 書き出しの cut 数の overflow を確保前に拒否しました (3 件)");
    return 0;
}
