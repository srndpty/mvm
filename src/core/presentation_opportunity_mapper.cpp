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

} // namespace mvm::core
