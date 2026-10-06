#ifndef MVM_APPS_MVM_MATH_EQUATION_SEQUENCE_ARTIFACT_H
#define MVM_APPS_MVM_MATH_EQUATION_SEQUENCE_ARTIFACT_H

// Equation Sequence (P3-3) の disk の artifact。Qt に依存しない。MathRasterCache が
// 静止・Write・P2 変形と同じ worker・権限・世代・publish の gate の下で呼ぶ。
//
// disk の形 (cacheDirectory からの相対):
//   equation-sequence/<key>.txt              provenance (確定の印。最後に atomic に書く)
//   equation-sequence/<key>/t<k>/00000.a8    変形 k の frame (切り出した A8 の被覆)
//   equation-sequence/<key>/a<k>/base.a8     action k の base 層 (区間中に変わらない)
//   equation-sequence/<key>/a<k>/00000.a8    action k の accent 層の frame
// 状態の hold は通常の静止の artifact (mvm-math-static) をそのまま使い、ここには置かない。
// provenance は状態の静止の key と大きさを記録し、読むたびに今の静止と照合する。
//
// 合成の順序: 各 frame の色は provenance に記録する (合成の側が計算し直さない)。
//   変形   frame i の色 = mathTransformColorAt(前の状態の色, 後の状態の色, i, N)
//   outline base = 状態の色、accent (線) = kEquationActionAccentArgb。base の上に accent
//   pulse   base (対象以外) = 状態の色、accent (拡大する対象) =
//           mathTransformColorAt(状態の色, kEquationActionAccentArgb, 重みの分子, 分母)
//           公開の前に、base と通常の大きさ・位置の対象 (backend の作業 directory の照合用の層。
//           保存しない) を composeEquationCoverage で重ねると状態の静止と全画素一致することを
//           確かめる (PulseBaseMismatch)。読むときは SHA-256 で公開時の base と同じことを確かめる。
// preview・書き出しへの組み込み (P3-4 / P3-5) はまだ行わない。

#include "media/math/equation_sequence_render.h"
#include "media/math/math_backend.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mvm::app {

// provenance file の 1 行目。形を変えたら上げる。
inline constexpr char kEquationSequenceArtifactFormat[] = "mvm-equation-sequence-artifact/1";

struct EquationArtifactFrame {
    std::filesystem::path path;
    std::string sha256;
    std::uint32_t colorArgb = 0;
    bool operator==(const EquationArtifactFrame&) const = default;
};

struct EquationTransitionArtifact {
    int width = 0; // 切り出した frame の大きさ
    int height = 0;
    int sourceX = 0; // 切り出した座標での前後の状態の静止の左上
    int sourceY = 0;
    int targetX = 0;
    int targetY = 0;
    std::vector<EquationArtifactFrame> frames; // N 枚 (frame i は進み具合 i / N)
};

struct EquationActionArtifact {
    math::EquationRenderOperation operation = math::EquationRenderOperation::Outline;
    int width = 0;
    int height = 0;
    int staticX = 0; // 切り出した座標での状態の静止の左上
    int staticY = 0;
    EquationArtifactFrame base;
    std::vector<EquationArtifactFrame> accent; // N 枚
};

struct EquationSequenceArtifact {
    std::vector<std::string> stateStaticKeys;
    std::vector<EquationTransitionArtifact> transitions;
    std::vector<EquationActionArtifact> actions;
    // backend の構造検証の結果 (診断・provenance。Project には書かない)。
    std::vector<math::EquationSegmentOwnership> ownership;
};

// frame を読み、大きさと SHA-256 を provenance と照合する。
bool loadEquationArtifactFrame(const EquationArtifactFrame& frame, int width, int height,
                               std::vector<std::uint8_t>& coverage, std::string& error);

// worker が 1 件の sequence に使う値 (起動の時点で固定する)。
struct EquationSequenceJob {
    std::filesystem::path directory; // cache directory
    std::filesystem::path jobs;      // 作業 directory の親
    std::string key;
    math::EquationSequenceRenderSpec spec;
    math::MathRenderBackend backend;
    std::chrono::milliseconds timeout{300000};
    std::uint64_t ticket = 0;
    // 各状態の今の静止 (同じ key の静止の artifact を decode したもの) と、その key。
    std::vector<math::MathCoverage> stateStatics;
    std::vector<std::string> stateStaticKeys;
    std::shared_ptr<std::mutex> publishGate;
    std::function<void(const std::filesystem::path& provenance)> beforePublish;
};

std::filesystem::path equationSequenceProvenancePath(const std::filesystem::path& directory,
                                                     const std::string& key);
std::filesystem::path equationSequenceDirectory(const std::filesystem::path& directory,
                                                const std::string& key);

enum class EquationSequenceDiskLoad { Ready, Missing, Cancelled };

// disk の artifact を確かめる: provenance の正準形 (identity は job から組み直して byte 単位で
// 比べる)・数値の整合・各 frame の大きさと SHA-256・端点の静止との一致。合わなければ
// (removeInvalid なら gate の下で消して) Missing。
EquationSequenceDiskLoad loadEquationSequenceArtifact(const EquationSequenceJob& job,
                                                      const std::atomic<bool>* cancel,
                                                      EquationSequenceArtifact& artifact,
                                                      bool removeInvalid = true);

struct EquationSequenceOutcome {
    bool cancelled = false;
    bool ready = false;
    math::MathRenderStatus status = math::MathRenderStatus::Failed;
    math::EquationBackendFailure backendFailure = math::EquationBackendFailure::NotValidated;
    std::string message;
    std::string log;
    EquationSequenceArtifact artifact;
};

// disk に検証済みの artifact があればそれを、無ければ backend で描き、一時的な作業 directory で
// 全て検証してから公開する。失敗・取消は Ready にならず、provenance を書かない。
EquationSequenceOutcome renderEquationSequenceJob(const EquationSequenceJob& job,
                                                  const std::atomic<bool>* cancel);

} // namespace mvm::app

#endif // MVM_APPS_MVM_MATH_EQUATION_SEQUENCE_ARTIFACT_H
