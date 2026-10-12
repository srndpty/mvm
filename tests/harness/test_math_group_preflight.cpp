// 数式 group を使ってよいかの、書き出しの前の判定 (P3-3D・P3-4)。
//
// 判定は画素も MLT も使わない関数 (mvm_mlt_export_math_group_preflight) なので、clip の表だけで
// 確かめる。ELIGIBLE と言ってよいのは「member と同じ frame に見える V1 の clip が、どれも
// stage した静止画で、変形が無く、全画素の alpha が 1 以上だと証明されている」ときだけである。
// 証明が欠けている・範囲外・静止画でない clip に付いている場合は、ELIGIBLE にしてはならない。

#include "media/mlt/mvm_mlt_export.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

MvmExportClip still(long long start, long long frames, int proof) {
    MvmExportClip clip{};
    clip.path = "v1.png";
    clip.video_track = 0;
    clip.is_still_image = 1;
    clip.timeline_start_frame = start;
    clip.timeline_duration_frames = frames;
    clip.math_group_base_alpha_proof = proof;
    return clip;
}

MvmExportClip overlay(int track, long long start, long long frames) {
    MvmExportClip clip{};
    clip.path = "math.png";
    clip.video_track = track;
    clip.is_still_image = 1;
    clip.timeline_start_frame = start;
    clip.timeline_duration_frames = frames;
    return clip;
}

struct Verdict {
    int decision = -1;
    std::string reason;
    int lowest = -2;
    long long checked = -1;
};

// groups は clip と同じ長さ。group の数は最大の番号。
Verdict judge(const std::vector<MvmExportClip>& clips, const std::vector<int>& groups,
              int ignoreProof = 0, int forgeProof = 0) {
    int count = 0;
    for (const int id : groups)
        count = id > count ? id : count;
    Verdict verdict;
    char reason[96] = {};
    mvm_mlt_export_math_group_preflight(clips.data(), static_cast<int>(clips.size()), groups.data(),
                                        count, ignoreProof, forgeProof, &verdict.decision, reason,
                                        sizeof(reason), &verdict.lowest, &verdict.checked);
    verdict.reason = reason;
    return verdict;
}

void expect(const char* name, const Verdict& verdict, int decision, const char* reason, int lowest,
            long long checked) {
    const bool ok = verdict.decision == decision && verdict.reason == reason &&
                    verdict.lowest == lowest && verdict.checked == checked;
    if (!ok) {
        ++failures;
        std::fprintf(stderr,
                     "FAIL: %s: 判定 %d (%s) alpha %d clip %lld、期待 %d (%s) alpha %d clip %lld\n",
                     name, verdict.decision, verdict.reason.c_str(), verdict.lowest,
                     verdict.checked, decision, reason, lowest, checked);
    }
}

constexpr int kEligible = MVM_MATH_GROUP_DECISION_ELIGIBLE;
constexpr int kIneligible = MVM_MATH_GROUP_DECISION_INELIGIBLE;
constexpr int kUnknown = MVM_MATH_GROUP_DECISION_UNKNOWN;

} // namespace

int main() {
    // --- 証明がある・無い・alpha 0 ---
    expect("member が無い", judge({still(0, 120, 256), overlay(1, 0, 120)}, {0, 0}), kIneligible,
           "no-eligible-math-clip", -1, 0);
    expect("不透明だと証明", judge({still(0, 120, 256), overlay(1, 0, 120)}, {0, 1}), kEligible,
           "base-alpha-proven", 255, 1);
    expect("alpha 1 だと証明", judge({still(0, 120, 2), overlay(1, 0, 120)}, {0, 1}), kEligible,
           "base-alpha-proven", 1, 1);
    expect("証明が無い", judge({still(0, 120, 0), overlay(1, 0, 120)}, {0, 1}), kUnknown,
           "base-alpha-unproven", -1, 1);
    expect("alpha 0 がある", judge({still(0, 120, 1), overlay(1, 0, 120)}, {0, 1}), kIneligible,
           "base-alpha-zero", 0, 1);
    expect("最小値は clip の中の最小",
           judge({still(0, 60, 256), still(60, 60, 2), overlay(1, 0, 120)}, {0, 0, 1}), kEligible,
           "base-alpha-proven", 1, 2);

    // --- 範囲外の証明 ---
    for (const int proof : {257, 1000, 100000, -1, -256})
        expect("範囲外の証明", judge({still(0, 120, proof), overlay(1, 0, 120)}, {0, 1}), kUnknown,
               "base-alpha-unproven", -1, 1);

    // --- stage した静止画でない clip に付いた証明は使わない ---
    {
        auto sequence = still(0, 120, 256);
        sequence.is_image_sequence = 1;
        auto video = still(0, 120, 256);
        video.is_still_image = 0;
        auto hold = still(0, 120, 256);
        hold.is_frame_hold = 1;
        const std::vector<int> groups{0, 1};
        expect("連番に付いた証明", judge({sequence, overlay(1, 0, 120)}, groups), kUnknown,
               "base-alpha-unproven", -1, 1);
        expect("動画に付いた証明", judge({video, overlay(1, 0, 120)}, groups), kUnknown,
               "base-alpha-unproven", -1, 1);
        expect("frame hold に付いた証明", judge({hold, overlay(1, 0, 120)}, groups), kUnknown,
               "base-alpha-unproven", -1, 1);
    }

    // --- V1 の clip に変形がある (出す画素を確かめていない) ---
    {
        const std::vector<int> groups{0, 1};
        auto effects = still(0, 120, 256);
        effects.effects_enabled = 1;
        auto crop = still(0, 120, 256);
        crop.crop_right = 4;
        auto motion = still(0, 120, 256);
        motion.motion_frame_count = 120;
        auto backdrop = still(0, 120, 256);
        backdrop.opaque_backdrop = 1;
        auto region = still(0, 120, 256);
        region.source_width = 100;
        region.source_height = 50;
        for (const auto& clip : {effects, crop, motion, backdrop, region})
            expect("変形のある V1", judge({clip, overlay(1, 0, 120)}, groups), kUnknown,
                   "base-clip-transformed", -1, 1);
        // 変形があり、証明が alpha 0 でも、INELIGIBLE (事実) ではなく UNKNOWN。
        auto effectsZero = still(0, 120, 1);
        effectsZero.effects_enabled = 1;
        expect("変形のある V1 (alpha 0)", judge({effectsZero, overlay(1, 0, 120)}, groups),
               kUnknown, "base-clip-transformed", -1, 1);
    }

    // --- V1 の隙間 (黒の color producer。alpha 255 として扱う) ---
    expect("V1 に clip が無い", judge({overlay(1, 0, 120)}, {1}), kEligible, "base-alpha-proven",
           -1, 0);

    // --- 区間: member と同じ frame に見える V1 の clip だけを調べる (端は半開区間) ---
    expect("証明の無い clip が member の直前で終わる",
           judge({still(0, 60, 0), overlay(1, 60, 60)}, {0, 1}), kEligible, "base-alpha-proven", -1,
           0);
    expect("証明の無い clip が member と 1 frame 重なる (前)",
           judge({still(0, 61, 0), overlay(1, 60, 60)}, {0, 1}), kUnknown, "base-alpha-unproven",
           -1, 1);
    expect("証明の無い clip が member の直後に始まる",
           judge({still(120, 60, 0), overlay(1, 60, 60)}, {0, 1}), kEligible, "base-alpha-proven",
           -1, 0);
    expect("証明の無い clip が member と 1 frame 重なる (後)",
           judge({still(119, 60, 0), overlay(1, 60, 60)}, {0, 1}), kUnknown, "base-alpha-unproven",
           -1, 1);
    expect("alpha 0 の clip が member と重ならない",
           judge({still(0, 60, 1), still(60, 60, 256), overlay(1, 60, 60)}, {0, 0, 1}), kEligible,
           "base-alpha-proven", 255, 1);
    expect("alpha 0 の clip が 2 つ目の member と重なる",
           judge({still(0, 60, 1), still(60, 60, 256), overlay(1, 60, 60), overlay(2, 59, 2)},
                 {0, 0, 1, 2}),
           kIneligible, "base-alpha-zero", 0, 1);
    // member でない layer (従来の affine) の区間は、判定の対象ではない。
    expect("証明の無い clip に重なるのが member でない layer だけ",
           judge({still(0, 60, 0), still(60, 60, 256), overlay(1, 0, 60), overlay(2, 60, 60)},
                 {0, 0, 0, 1}),
           kEligible, "base-alpha-proven", 255, 1);

    // --- 優先: alpha 0 が実際にあれば、ほかに分からない clip があっても INELIGIBLE ---
    expect("証明なしの後に alpha 0",
           judge({still(0, 60, 0), still(60, 60, 1), overlay(1, 0, 120)}, {0, 0, 1}), kIneligible,
           "base-alpha-zero", 0, 2);
    expect("証明ありの後に証明なし (途中の最小値を証明として返さない)",
           judge({still(0, 60, 200), still(60, 60, 0), overlay(1, 0, 120)}, {0, 0, 1}), kUnknown,
           "base-alpha-unproven", -1, 2);

    // --- V1 でないもの・音声は V1 の clip として数えない ---
    {
        auto audio = still(0, 120, 0);
        audio.is_audio = 1;
        expect("音声", judge({audio, still(0, 120, 256), overlay(1, 0, 120)}, {0, 0, 1}), kEligible,
               "base-alpha-proven", 255, 1);
    }

    // --- 対照用の入力 ---
    expect("証明を無いものとして扱う", judge({still(0, 120, 256), overlay(1, 0, 120)}, {0, 1}, 1),
           kUnknown, "base-alpha-unproven", -1, 1);
    expect("証明を範囲外に差し替える",
           judge({still(0, 120, 256), overlay(1, 0, 120)}, {0, 1}, 0, 999), kUnknown,
           "base-alpha-unproven", -1, 1);
    expect("証明を alpha 0 に差し替える",
           judge({still(0, 120, 256), overlay(1, 0, 120)}, {0, 1}, 0, 1), kIneligible,
           "base-alpha-zero", 0, 1);

    // --- 入力が欠けている ---
    {
        Verdict verdict;
        char reason[96] = {};
        mvm_mlt_export_math_group_preflight(nullptr, 3, nullptr, 2, 0, 0, &verdict.decision, reason,
                                            sizeof(reason), &verdict.lowest, &verdict.checked);
        verdict.reason = reason;
        expect("clip の配列が無い", verdict, kUnknown, "base-alpha-unproven", -1, 0);
    }

    if (failures) {
        std::fprintf(stderr, "不成立: %d\n", failures);
        return 1;
    }
    std::printf("math_group_preflight: 全部成立\n");
    return 0;
}
