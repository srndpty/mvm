#ifndef MVM_PROJECT_EQUATION_SEQUENCE_H
#define MVM_PROJECT_EQUATION_SEQUENCE_H

#include "project/math_clip.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mvm::project {
template<class Tag>
struct EquationId {
    std::string value;
    bool operator==(const EquationId&) const = default;
};
using StateId = EquationId<struct EquationStateTag>;
using TransitionId = EquationId<struct EquationTransitionTag>;
using ActionId = EquationId<struct EquationActionTag>;
using PartId = EquationId<struct EquationPartTag>;
enum class BindingStatus { Bound, Invalid };
enum class EquationOperation { Outline, Pulse };
enum class EquationTargetStatus { Present, Missing };

struct SourceBinding {
    std::string revision;
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::string expectedText;
    BindingStatus status = BindingStatus::Bound;
    bool operator==(const SourceBinding&) const = default;
};

struct SemanticPart {
    PartId id;
    std::string label;
    SourceBinding binding;
    bool operator==(const SemanticPart&) const = default;
};

struct EquationState {
    StateId id;
    MathClipData equation;
    std::string revision;
    std::int64_t holdFrames = 1;
    std::vector<SemanticPart> parts;
    bool operator==(const EquationState&) const = default;
};

struct PartPair {
    PartId from;
    PartId to;
    bool operator==(const PartPair&) const = default;
};

struct EquationStepTransition {
    TransitionId id;
    StateId from;
    StateId to;
    std::int64_t frames = 1;
    std::vector<PartPair> correspondence;
    bool operator==(const EquationStepTransition&) const = default;
};

struct EquationAction {
    ActionId id;
    StateId state;
    PartId target;
    EquationTargetStatus targetStatus = EquationTargetStatus::Present;
    std::int64_t start = 0;
    std::int64_t duration = 1;
    EquationOperation operation = EquationOperation::Outline;
    bool operator==(const EquationAction&) const = default;
};

struct EquationSequenceClipData {
    std::vector<EquationState> states;
    std::vector<EquationStepTransition> transitions;
    std::vector<EquationAction> actions;
    bool operator==(const EquationSequenceClipData&) const = default;
};

struct EquationInterval {
    bool transition = false;
    std::size_t index = 0;
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

// 時間の正は整数の区間と progressNumerator / progressDenominator。浮動小数は持たない。
struct EquationEvaluation {
    bool transition = false;
    StateId state;
    StateId toState;
    TransitionId transitionId;
    std::int64_t localFrame = 0;
    std::int64_t frames = 0;
    std::int64_t progressNumerator = 0;
    std::int64_t progressDenominator = 1;
    std::optional<ActionId> activeAction;
};

bool equationIntervals(const EquationSequenceClipData&, std::vector<EquationInterval>&,
                       std::int64_t& length, std::string& error);
bool validateEquationSequence(const EquationSequenceClipData&, int outputHeight,
                              std::string& error);
// 現在の source / revision に対する Bound の証人検査を domain と compiler で共有する。
bool equationBindingMatchesSource(const EquationState&, const SourceBinding&);
std::optional<EquationEvaluation> evaluateEquationSequence(const EquationSequenceClipData&,
                                                           std::int64_t sourceFrame,
                                                           std::string& error);
// ID 発行は有限回で失敗する。実在 ID と保持中の欠落 ID を両方予約する。
bool remapEquationSequenceIds(EquationSequenceClipData&, const std::function<std::string()>&,
                              std::string& error);
bool insertEquationState(EquationSequenceClipData&, std::size_t position, EquationState,
                         const std::vector<EquationStepTransition>& newEdges, int outputHeight,
                         std::string& error);
bool deleteEquationState(EquationSequenceClipData&, StateId,
                         const std::optional<EquationStepTransition>& newEdge, int outputHeight,
                         std::string& error);
bool changeEquationHold(EquationSequenceClipData&, StateId, std::int64_t frames, int outputHeight,
                        std::string& error);
bool changeEquationTransition(EquationSequenceClipData&, TransitionId, std::int64_t frames,
                              int outputHeight, std::string& error);
bool replaceEquationSource(EquationSequenceClipData&, StateId, std::string source,
                           std::string revision, int outputHeight, std::string& error);
bool deleteEquationPart(EquationSequenceClipData&, StateId, PartId, int outputHeight,
                        std::string& error);
} // namespace mvm::project
#endif
