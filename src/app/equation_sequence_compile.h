#ifndef MVM_APP_EQUATION_SEQUENCE_COMPILE_H
#define MVM_APP_EQUATION_SEQUENCE_COMPILE_H
#include "media/math/math_tex_segments.h"
#include "project/equation_sequence.h"

namespace mvm::app {
enum class EquationCompileFailure {
    None,
    InvalidBinding,
    MissingPart,
    UnsupportedTexBoundary,
    PartitionConflict,
    InvalidCorrespondencePlan,
    UnsupportedEmptyTarget,
    BackendValidationRequired,
    InvalidSequence
};
enum class EquationSegmentKind { Semantic, Auto };
enum class EquationTargetProof { BackendValidationRequired, Empty };

struct EquationSegmentSpec {
    EquationSegmentKind kind = EquationSegmentKind::Auto;
    std::size_t begin = 0, end = 0;
    std::string text, key;
    EquationTargetProof targetProof = EquationTargetProof::BackendValidationRequired;
    bool operator==(const EquationSegmentSpec&) const = default;
};

// 正準入力は所有 ID / revision / label を含めない。順番と範囲が派生 handle の正。
struct EquationPartitionSpec {
    project::MathClipData equation;
    std::int64_t holdFrames = 0;
    std::vector<EquationSegmentSpec> segments;
    std::string segmenterVersion = math::kMathTexSegmenterVersion;
    bool operator==(const EquationPartitionSpec&) const = default;
};

struct EquationTransitionSpec {
    std::size_t fromState = 0, toState = 0;
    std::int64_t frames = 0;
    math::MathSegmentMatching matching;
    std::string matcherVersion = math::kMathTexMatchingVersion;
    bool operator==(const EquationTransitionSpec&) const = default;
};

struct EquationActionSpec {
    std::size_t state = 0, segment = 0;
    std::int64_t start = 0, duration = 0;
    project::EquationOperation operation = project::EquationOperation::Outline;
    EquationTargetProof targetProof = EquationTargetProof::BackendValidationRequired;
    bool operator==(const EquationActionSpec&) const = default;
};

struct EquationSequenceSpec {
    std::string compilerVersion = "equation-neutral/1";
    std::vector<EquationPartitionSpec> states;
    std::vector<EquationTransitionSpec> transitions;
    std::vector<EquationActionSpec> actions;
    bool operator==(const EquationSequenceSpec&) const = default;
};

struct EquationPartition {
    project::StateId state;
    EquationPartitionSpec spec;
    // semantic の ID のみ保持する。auto は空。正準入力とは別の解決用データ。
    std::vector<std::optional<project::PartId>> owners;
};

struct EquationActionPlan {
    project::StateId state;
    project::PartId part;
    EquationActionSpec spec;
};

template<class T>
struct EquationCompileResult {
    std::optional<T> value;
    EquationCompileFailure failure = EquationCompileFailure::None;
};

EquationCompileResult<EquationPartition> compileEquationPartition(const project::EquationState&);
EquationCompileResult<EquationTransitionSpec>
compileEquationTransition(const EquationPartition&, const EquationPartition&,
                          const project::EquationStepTransition&);
EquationCompileResult<EquationActionPlan> compileEquationAction(const EquationPartition&,
                                                                const project::EquationAction&);
EquationCompileResult<EquationSequenceSpec>
compileEquationSequence(const project::EquationSequenceClipData&);
// P3-2 は glyph の存在を証明しない。backend が非空を確認するまで実行可能としない。
EquationCompileFailure equationTargetReadiness(EquationTargetProof);
} // namespace mvm::app
#endif
