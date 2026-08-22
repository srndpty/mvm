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

std::vector<std::vector<long long>> bruteForce(const mvm::core::OpportunityCandidateMatrix& input) {
    std::vector<std::vector<long long>> solutions;
    std::vector<long long> assignment;
    const auto opportunityCount = input.opportunityOrdinals.size();
    std::function<void(std::size_t, std::size_t)> visit = [&](std::size_t record,
                                                              std::size_t nextOpportunity) {
        if (record == input.recordCount) {
            solutions.push_back(assignment);
            return;
        }
        for (auto opportunity = nextOpportunity; opportunity < opportunityCount; ++opportunity) {
            if (input.admissible[record * opportunityCount + opportunity] != 0) {
                assignment.push_back(input.opportunityOrdinals[opportunity]);
                visit(record + 1, opportunity + 1);
                assignment.pop_back();
            }
        }
    };
    visit(0, 0);
    return solutions;
}

mvm::core::MappingSolutionClass expectedClass(int count) {
    if (count == 0)
        return mvm::core::MappingSolutionClass::NoSolution;
    if (count == 1)
        return mvm::core::MappingSolutionClass::Unique;
    return mvm::core::MappingSolutionClass::Ambiguous;
}

std::vector<long long> bruteConsensusPrefix(const std::vector<std::vector<long long>>& solutions) {
    std::vector<long long> result;
    if (solutions.empty())
        return result;
    for (std::size_t record = 0; record < solutions.front().size(); ++record) {
        const auto expected = solutions.front()[record];
        if (std::any_of(solutions.begin(), solutions.end(), [record, expected](const auto& value) {
                return value[record] != expected;
            }))
            break;
        result.push_back(expected);
    }
    return result;
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
                check(actual.solutionClass == expectedClass(static_cast<int>(brute.size())),
                      "DP classがbrute forceと不一致です");
                check(actual.saturatedSolutionCount == std::min<std::size_t>(2, brute.size()),
                      "DP solution count saturationが不一致です");
                check(actual.consensusPrefix == bruteConsensusPrefix(brute),
                      "consensus prefixがbrute forceと不一致です");
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

void incrementalWatermark() {
    mvm::core::IncrementalOpportunityMapper mapper;
    check(mapper.observeVBlank({10, 100}), "最初のVBlankを受理できません");
    check(mapper.observeCallback(150), "最初のcallbackを受理できません");
    check(mapper.observeVBlank({11, 200}), "callback domainを閉じられません");
    check(mapper.snapshot().committedAssignment == std::vector<long long>({10}),
          "一意なconsensus prefixをcommitしていません");
    check(mapper.observeVBlank({12, 300}), "次のVBlankを受理できません");
    check(mapper.observeCallback(350), "曖昧なcallbackを受理できません");
    check(mapper.observeVBlank({13, 400}), "曖昧なcallback domainを閉じられません");
    check(mapper.snapshot().solutionClass == mvm::core::MappingSolutionClass::Ambiguous,
          "複数解をAMBIGUOUSとして保持していません");
    check(mapper.snapshot().committedAssignment == std::vector<long long>({10}),
          "future callbackがcommit watermarkを変更しました");
    check(!mapper.finish() &&
              mapper.snapshot().error == mvm::core::IncrementalMappingError::AmbiguousMapping,
          "measurement endのAMBIGUOUSをfail-closedにしていません");

    mvm::core::IncrementalOpportunityMapper noEarlyFirst;
    check(noEarlyFirst.observeVBlank({20, 100}), "mutation fixtureのVBlank 0が不正です");
    check(noEarlyFirst.observeVBlank({21, 200}), "mutation fixtureのVBlank 1が不正です");
    check(noEarlyFirst.observeCallback(250), "mutation fixtureのcallbackが不正です");
    check(noEarlyFirst.observeVBlank({22, 300}), "mutation fixtureのdomainを閉じられません");
    check(noEarlyFirst.snapshot().solutionClass == mvm::core::MappingSolutionClass::Ambiguous &&
              noEarlyFirst.snapshot().committedAssignment.empty(),
          "ambiguous prefixのfirst solutionを早期commitしています");

    mvm::core::IncrementalOpportunityMapper noSolution;
    check(noSolution.observeVBlank({30, 100}), "NO_SOLUTION fixtureのVBlank 0が不正です");
    check(noSolution.observeCallback(150), "NO_SOLUTION fixtureのcallback 0が不正です");
    check(noSolution.observeCallback(160), "NO_SOLUTION fixtureのcallback 1が不正です");
    check(!noSolution.observeVBlank({31, 200}) &&
              noSolution.snapshot().error == mvm::core::IncrementalMappingError::NoSolution,
          "NO_SOLUTIONを確定時点でfail-closedにしていません");
}

} // namespace

int main() {
    exhaustiveCandidateMatrices();
    observableRelation();
    mutationGuards();
    incrementalWatermark();
    if (failures == 0)
        std::printf("presentation opportunity mapper: PASS\n");
    return failures == 0 ? 0 : 1;
}
