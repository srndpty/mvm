#include "core/presentation_opportunity_mapper.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int bruteForce(const mvm::core::OpportunityCandidateMatrix& input) {
    int count = 0;
    const auto opportunityCount = input.opportunityOrdinals.size();
    std::function<void(std::size_t, std::size_t)> visit = [&](std::size_t record,
                                                              std::size_t nextOpportunity) {
        if (record == input.recordCount) {
            ++count;
            return;
        }
        for (auto opportunity = nextOpportunity; opportunity < opportunityCount; ++opportunity) {
            if (input.admissible[record * opportunityCount + opportunity] != 0)
                visit(record + 1, opportunity + 1);
        }
    };
    visit(0, 0);
    return count;
}

mvm::core::MappingSolutionClass expectedClass(int count) {
    if (count == 0)
        return mvm::core::MappingSolutionClass::NoSolution;
    if (count == 1)
        return mvm::core::MappingSolutionClass::Unique;
    return mvm::core::MappingSolutionClass::Ambiguous;
}

void exhaustiveCandidateMatrices() {
    // generic candidate relationを全列挙し、DPを独立brute forceと比較する。
    for (std::size_t records = 1; records <= 3; ++records) {
        for (std::size_t opportunities = 1; opportunities <= 5; ++opportunities) {
            const auto bits = records * opportunities;
            const auto matrixCount = std::size_t{1} << bits;
            for (std::size_t mask = 0; mask < matrixCount; ++mask) {
                mvm::core::OpportunityCandidateMatrix input;
                input.recordCount = records;
                for (std::size_t value = 0; value < opportunities; ++value)
                    input.opportunityOrdinals.push_back(static_cast<long long>(value));
                input.admissible.resize(bits);
                for (std::size_t bit = 0; bit < bits; ++bit)
                    input.admissible[bit] = (mask >> bit) & 1;
                const auto brute = bruteForce(input);
                const auto actual = mvm::core::solveOpportunityMapping(input);
                check(actual.solutionClass == expectedClass(brute),
                      "DP classがbrute forceと不一致です");
                check(actual.saturatedSolutionCount == std::min(2, brute),
                      "DP solution count saturationが不一致です");
                if (actual.solutionClass == mvm::core::MappingSolutionClass::Unique) {
                    check(actual.assignment.size() == records,
                          "UNIQUE assignmentのrecord数が不一致です");
                    check(std::adjacent_find(actual.assignment.begin(), actual.assignment.end(),
                                             std::greater_equal<>()) == actual.assignment.end(),
                          "UNIQUE assignmentがstrict monotoneではありません");
                } else {
                    check(actual.assignment.empty(), "非UNIQUEで代表解を返してはいけません");
                }
            }
        }
    }
}

void observableRelation() {
    mvm::core::ObservableMappingInput input;
    input.measurementStartQpc = 100;
    input.measurementEndQpcExclusive = 400;
    input.syncInterval = 1;
    input.vblankSamples = {{10, 100}, {11, 200}, {12, 300}, {13, 400}};
    input.callbackQpc = {250, 350};
    mvm::core::OpportunityCandidateMatrix candidates;
    check(mvm::core::buildObservableCandidateMatrix(input, candidates),
          "observable candidate relationを構築できません");
    auto result = mvm::core::solveOpportunityMapping(candidates);
    check(result.solutionClass == mvm::core::MappingSolutionClass::Ambiguous,
          "candidateを1個広げた多解をUNIQUE扱いしてはいけません");

    input.vblankSamples.pop_back();
    input.measurementEndQpcExclusive = 300;
    input.callbackQpc = {250, 260};
    check(mvm::core::buildObservableCandidateMatrix(input, candidates),
          "2 opportunity relationを構築できません");
    result = mvm::core::solveOpportunityMapping(candidates);
    check(result.solutionClass == mvm::core::MappingSolutionClass::Unique &&
              result.assignment == std::vector<long long>({10, 11}),
          "2 record / 2 opportunityの一意解が不正です");

    input.callbackQpc = {250, 250};
    check(!mvm::core::buildObservableCandidateMatrix(input, candidates),
          "同時刻callbackをstrict orderとして受理してはいけません");

    input.callbackQpc = {260, 250};
    check(!mvm::core::buildObservableCandidateMatrix(input, candidates),
          "逆順callbackを並べ替えて受理してはいけません");
}

void mutationGuards() {
    // injective制約を外して同一opportunityを再利用するmutationを検出する。
    mvm::core::OpportunityCandidateMatrix reuse;
    reuse.opportunityOrdinals = {10};
    reuse.recordCount = 2;
    reuse.admissible = {1, 1};
    auto result = mvm::core::solveOpportunityMapping(reuse);
    check(result.solutionClass == mvm::core::MappingSolutionClass::NoSolution,
          "opportunity再利用mutationを検出できません");

    // 最初の合法解をUNIQUEとして返すgreedy mutationを検出する。
    mvm::core::OpportunityCandidateMatrix greedy;
    greedy.opportunityOrdinals = {10, 11, 12};
    greedy.recordCount = 2;
    greedy.admissible = {1, 1, 1, 1, 1, 1};
    result = mvm::core::solveOpportunityMapping(greedy);
    check(result.solutionClass == mvm::core::MappingSolutionClass::Ambiguous &&
              result.assignment.empty(),
          "first-solution mutationを検出できません");

    mvm::core::ObservableMappingInput boundary;
    boundary.measurementStartQpc = 100;
    boundary.measurementEndQpcExclusive = 300;
    boundary.syncInterval = 1;
    boundary.vblankSamples = {{10, 100}, {11, 200}, {12, 300}};
    boundary.callbackQpc = {200};
    mvm::core::OpportunityCandidateMatrix candidates;
    check(mvm::core::buildObservableCandidateMatrix(boundary, candidates),
          "boundary candidate relationを構築できません");
    result = mvm::core::solveOpportunityMapping(candidates);
    check(result.solutionClass == mvm::core::MappingSolutionClass::Ambiguous,
          "<=を<へ変えるboundary mutationを検出できません");

    boundary.callbackQpc = {150};
    check(mvm::core::buildObservableCandidateMatrix(boundary, candidates),
          "future exclusion relationを構築できません");
    result = mvm::core::solveOpportunityMapping(candidates);
    check(result.solutionClass == mvm::core::MappingSolutionClass::Unique &&
              result.assignment == std::vector<long long>({10}),
          "future opportunityを混入するwindow-widen mutationを検出できません");
}

} // namespace

int main() {
    exhaustiveCandidateMatrices();
    observableRelation();
    mutationGuards();
    if (failures == 0)
        std::printf("presentation opportunity mapper: PASS\n");
    return failures == 0 ? 0 : 1;
}
