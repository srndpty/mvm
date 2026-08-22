#include "core/presentation_opportunity_mapper.h"

#include <algorithm>
#include <cstddef>

namespace mvm::core {
namespace {

std::uint8_t saturatedAdd(std::uint8_t left, std::uint8_t right) {
    return static_cast<std::uint8_t>(std::min(2, static_cast<int>(left) + right));
}

bool candidate(const OpportunityCandidateMatrix& input, std::size_t record,
               std::size_t opportunity) {
    const auto opportunityCount = input.opportunityOrdinals.size();
    return input.admissible[record * opportunityCount + opportunity] != 0;
}

} // namespace

bool buildObservableCandidateMatrix(const ObservableMappingInput& input,
                                    OpportunityCandidateMatrix& output) {
    output = {};
    if (input.syncInterval != 1 || input.measurementStartQpc < 0 ||
        input.measurementEndQpcExclusive <= input.measurementStartQpc ||
        input.vblankSamples.size() < 2 || input.callbackQpc.empty())
        return false;
    if (input.vblankSamples.front().qpc != input.measurementStartQpc ||
        input.vblankSamples.back().qpc != input.measurementEndQpcExclusive)
        return false;
    for (std::size_t index = 1; index < input.vblankSamples.size(); ++index) {
        if (input.vblankSamples[index].ordinal != input.vblankSamples[index - 1].ordinal + 1 ||
            input.vblankSamples[index].qpc <= input.vblankSamples[index - 1].qpc)
            return false;
    }
    for (std::size_t index = 0; index < input.callbackQpc.size(); ++index) {
        const auto qpc = input.callbackQpc[index];
        if (qpc < input.measurementStartQpc || qpc >= input.measurementEndQpcExclusive ||
            (index > 0 && qpc <= input.callbackQpc[index - 1]))
            return false;
    }

    const auto opportunityCount = input.vblankSamples.size() - 1;
    output.recordCount = input.callbackQpc.size();
    output.opportunityOrdinals.reserve(opportunityCount);
    output.admissible.assign(output.recordCount * opportunityCount, 0);
    for (std::size_t opportunity = 0; opportunity < opportunityCount; ++opportunity) {
        output.opportunityOrdinals.push_back(input.vblankSamples[opportunity].ordinal);
        for (std::size_t record = 0; record < output.recordCount; ++record) {
            if (input.vblankSamples[opportunity].qpc <= input.callbackQpc[record])
                output.admissible[record * opportunityCount + opportunity] = 1;
        }
    }
    return true;
}

MappingSolution solveOpportunityMapping(const OpportunityCandidateMatrix& input) {
    MappingSolution result;
    const auto recordCount = input.recordCount;
    const auto opportunityCount = input.opportunityOrdinals.size();
    if (recordCount == 0 || opportunityCount == 0 ||
        input.admissible.size() != recordCount * opportunityCount)
        return result;
    for (std::size_t index = 1; index < opportunityCount; ++index) {
        if (input.opportunityOrdinals[index] <= input.opportunityOrdinals[index - 1])
            return result;
    }

    std::vector<std::uint8_t> dp((recordCount + 1) * (opportunityCount + 1), 0);
    const auto at = [opportunityCount, &dp](std::size_t record,
                                            std::size_t opportunity) -> std::uint8_t& {
        return dp[record * (opportunityCount + 1) + opportunity];
    };
    for (std::size_t opportunity = 0; opportunity <= opportunityCount; ++opportunity)
        at(0, opportunity) = 1;
    for (std::size_t record = 1; record <= recordCount; ++record) {
        for (std::size_t opportunity = 1; opportunity <= opportunityCount; ++opportunity) {
            const auto skipped = at(record, opportunity - 1);
            const std::uint8_t selected = candidate(input, record - 1, opportunity - 1)
                                              ? at(record - 1, opportunity - 1)
                                              : std::uint8_t{0};
            at(record, opportunity) = saturatedAdd(skipped, selected);
        }
    }

    result.saturatedSolutionCount = at(recordCount, opportunityCount);
    if (result.saturatedSolutionCount == 0)
        return result;

    std::vector<std::uint8_t> suffix((recordCount + 1) * (opportunityCount + 1), 0);
    const auto suffixAt = [opportunityCount, &suffix](std::size_t record,
                                                      std::size_t opportunity) -> std::uint8_t& {
        return suffix[record * (opportunityCount + 1) + opportunity];
    };
    for (std::size_t opportunity = 0; opportunity <= opportunityCount; ++opportunity)
        suffixAt(recordCount, opportunity) = 1;
    for (auto record = recordCount; record-- > 0;) {
        for (auto opportunity = opportunityCount; opportunity-- > 0;) {
            const auto skipped = suffixAt(record, opportunity + 1);
            const std::uint8_t selected = candidate(input, record, opportunity)
                                              ? suffixAt(record + 1, opportunity + 1)
                                              : std::uint8_t{0};
            suffixAt(record, opportunity) = saturatedAdd(skipped, selected);
        }
    }

    for (std::size_t record = 0; record < recordCount; ++record) {
        std::size_t feasibleCount = 0;
        std::int64_t consensus = -1;
        for (std::size_t opportunity = 0; opportunity < opportunityCount; ++opportunity) {
            if (candidate(input, record, opportunity) && at(record, opportunity) != 0 &&
                suffixAt(record + 1, opportunity + 1) != 0) {
                ++feasibleCount;
                consensus = input.opportunityOrdinals[opportunity];
            }
        }
        if (feasibleCount != 1)
            break;
        result.consensusPrefix.push_back(consensus);
    }
    if (result.saturatedSolutionCount >= 2) {
        result.solutionClass = MappingSolutionClass::Ambiguous;
        return result;
    }

    result.solutionClass = MappingSolutionClass::Unique;
    result.assignment.resize(recordCount);
    auto record = recordCount;
    auto opportunity = opportunityCount;
    while (record > 0) {
        if (opportunity == 0) {
            result = {};
            return result;
        }
        const auto skipped = at(record, opportunity - 1);
        const std::uint8_t selected = candidate(input, record - 1, opportunity - 1)
                                          ? at(record - 1, opportunity - 1)
                                          : std::uint8_t{0};
        if (selected == 1 && skipped == 0) {
            result.assignment[record - 1] = input.opportunityOrdinals[opportunity - 1];
            --record;
            --opportunity;
        } else if (skipped == 1 && selected == 0) {
            --opportunity;
        } else {
            result = {};
            return result;
        }
    }
    return result;
}

IncrementalOpportunityMapper::IncrementalOpportunityMapper(int syncInterval)
    : syncInterval_(syncInterval) {
    if (syncInterval_ != 1)
        snapshot_.error = IncrementalMappingError::InvalidInput;
}

bool IncrementalOpportunityMapper::observeVBlank(MapperVBlankSample sample) {
    if (snapshot_.finalized || snapshot_.error != IncrementalMappingError::None || sample.qpc < 0 ||
        (!vblankSamples_.empty() && (sample.ordinal != vblankSamples_.back().ordinal + 1 ||
                                     sample.qpc <= vblankSamples_.back().qpc))) {
        if (snapshot_.error == IncrementalMappingError::None)
            snapshot_.error = IncrementalMappingError::InvalidInput;
        return false;
    }
    vblankSamples_.push_back(sample);
    return update(false);
}

bool IncrementalOpportunityMapper::observeCallback(std::int64_t qpc) {
    if (snapshot_.finalized || snapshot_.error != IncrementalMappingError::None || qpc < 0 ||
        (!callbackQpc_.empty() && qpc <= callbackQpc_.back())) {
        if (snapshot_.error == IncrementalMappingError::None)
            snapshot_.error = IncrementalMappingError::InvalidInput;
        return false;
    }
    callbackQpc_.push_back(qpc);
    snapshot_.observedCallbackCount = callbackQpc_.size();
    return update(false);
}

bool IncrementalOpportunityMapper::finish() {
    if (snapshot_.finalized || snapshot_.error != IncrementalMappingError::None)
        return false;
    snapshot_.finalized = true;
    return update(true);
}

const IncrementalMappingSnapshot& IncrementalOpportunityMapper::snapshot() const {
    return snapshot_;
}

bool IncrementalOpportunityMapper::update(bool finalizing) {
    snapshot_.observedCallbackCount = callbackQpc_.size();
    if (vblankSamples_.size() < 2 || callbackQpc_.empty()) {
        if (finalizing && !callbackQpc_.empty())
            snapshot_.error = IncrementalMappingError::UnclosedCallback;
        return snapshot_.error == IncrementalMappingError::None;
    }

    const auto boundaryQpc = vblankSamples_.back().qpc;
    const auto closedEnd = std::lower_bound(callbackQpc_.begin(), callbackQpc_.end(), boundaryQpc);
    const auto closedCount = static_cast<std::size_t>(closedEnd - callbackQpc_.begin());
    snapshot_.closedRecordCount = closedCount;
    snapshot_.hasClosedRecords = closedCount != 0;
    if (closedCount == 0) {
        if (finalizing)
            snapshot_.error = IncrementalMappingError::UnclosedCallback;
        return snapshot_.error == IncrementalMappingError::None;
    }

    ObservableMappingInput input;
    input.measurementStartQpc = vblankSamples_.front().qpc;
    input.measurementEndQpcExclusive = boundaryQpc;
    input.syncInterval = syncInterval_;
    input.vblankSamples = vblankSamples_;
    input.callbackQpc.assign(callbackQpc_.begin(), closedEnd);
    OpportunityCandidateMatrix candidates;
    if (!buildObservableCandidateMatrix(input, candidates)) {
        snapshot_.error = IncrementalMappingError::InvalidInput;
        return false;
    }
    const auto solution = solveOpportunityMapping(candidates);
    snapshot_.solutionClass = solution.solutionClass;
    snapshot_.saturatedSolutionCount = solution.saturatedSolutionCount;
    if (solution.solutionClass == MappingSolutionClass::NoSolution) {
        snapshot_.error = IncrementalMappingError::NoSolution;
        return false;
    }
    if (solution.consensusPrefix.size() < snapshot_.committedAssignment.size() ||
        !std::equal(snapshot_.committedAssignment.begin(), snapshot_.committedAssignment.end(),
                    solution.consensusPrefix.begin())) {
        snapshot_.error = IncrementalMappingError::CommitRegression;
        return false;
    }
    snapshot_.committedAssignment = solution.consensusPrefix;

    if (!finalizing)
        return true;
    if (closedCount != callbackQpc_.size()) {
        snapshot_.error = IncrementalMappingError::UnclosedCallback;
        return false;
    }
    if (solution.solutionClass == MappingSolutionClass::Ambiguous) {
        snapshot_.error = IncrementalMappingError::AmbiguousMapping;
        return false;
    }
    return true;
}

const char* mappingSolutionClassName(MappingSolutionClass value) {
    switch (value) {
    case MappingSolutionClass::NoSolution:
        return "NO_SOLUTION";
    case MappingSolutionClass::Unique:
        return "UNIQUE";
    case MappingSolutionClass::Ambiguous:
        return "AMBIGUOUS";
    }
    return "NO_SOLUTION";
}

const char* incrementalMappingErrorName(IncrementalMappingError value) {
    switch (value) {
    case IncrementalMappingError::None:
        return "NONE";
    case IncrementalMappingError::InvalidInput:
        return "INVALID_INPUT";
    case IncrementalMappingError::NoSolution:
        return "NO_SOLUTION";
    case IncrementalMappingError::AmbiguousMapping:
        return "AMBIGUOUS_MAPPING";
    case IncrementalMappingError::UnclosedCallback:
        return "UNCLOSED_CALLBACK";
    case IncrementalMappingError::CommitRegression:
        return "COMMIT_REGRESSION";
    }
    return "INVALID_INPUT";
}

} // namespace mvm::core
