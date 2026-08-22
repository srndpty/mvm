#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvm::core {

enum class MappingSolutionClass { NoSolution, Unique, Ambiguous };

struct OpportunityCandidateMatrix {
    std::vector<std::int64_t> opportunityOrdinals;
    std::size_t recordCount = 0;
    // record-major。0は非合法、1は合法。
    std::vector<std::uint8_t> admissible;
};

struct MappingSolution {
    MappingSolutionClass solutionClass = MappingSolutionClass::NoSolution;
    // 0 / 1 / 2。2は「2個以上」へのsaturation。
    std::uint8_t saturatedSolutionCount = 0;
    std::vector<std::int64_t> assignment;
};

struct MapperVBlankSample {
    std::int64_t ordinal = -1;
    std::int64_t qpc = -1;
};

struct ObservableMappingInput {
    std::int64_t measurementStartQpc = -1;
    std::int64_t measurementEndQpcExclusive = -1;
    int syncInterval = 0;
    std::vector<MapperVBlankSample> vblankSamples;
    std::vector<std::int64_t> callbackQpc;
};

// R4で事前固定したadmissibility relation。
// 各VBlank sample[i]をopportunity iの開始、sample[i+1]を終了境界とする。
// callbackはdisplay後に遅延し得るというB0/B0.5仮説に基づき、開始QPCが
// callback QPC以下の全opportunityを候補にする。hidden oracle、元callback、
// synthetic delayは入力に含めない。
bool buildObservableCandidateMatrix(const ObservableMappingInput& input,
                                    OpportunityCandidateMatrix& output);

// strict monotone / injective / order-preserving assignmentをexact DPで数える。
// 複数解から代表解を選ばず、assignmentはUNIQUEの場合だけ返す。
MappingSolution solveOpportunityMapping(const OpportunityCandidateMatrix& input);

const char* mappingSolutionClassName(MappingSolutionClass value);

} // namespace mvm::core
