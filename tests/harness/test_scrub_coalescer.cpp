// mvm Phase 0 / S6 - ScrubCoalescer の単体テスト
//
// 純粋な状態機械だけを対象にする。スレッドも実時間も使わないので、
// 「decode 中に新しい要求が来た古い結果」を決定論的に再現できる。
// スレッドを使う統合テストは scrub-bench 側 (--selftest) にある。

#include "scrub_coalescer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool cond, const std::string& what) {
    gChecks++;
    if (!cond) {
        gFailures++;
        std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    }
}

template<typename T>
void checkEq(const T& got, const T& want, const std::string& what) {
    gChecks++;
    if (!(got == want)) {
        gFailures++;
        std::fprintf(stderr, "  FAIL %s : got %lld want %lld\n", what.c_str(), (long long)got,
                     (long long)want);
    }
}

using namespace bench;

// --------------------------------------------------------------------------
// 必須シーケンス: decode 中に新要求が来た古い結果を棄却する
// --------------------------------------------------------------------------
//
//  1. generation 0 を submit
//  2. worker が generation 0 を take し in-flight にする
//  3. generation 1 を submit
//  4. generation 2 を submit し、未処理の generation 1 を supersede
//  5. generation 0 の結果を complete
//  6. generation 0 が stale として棄却される
//  7. generation 2 を take して complete
//  8. generation 2 だけが accept される
void testInFlightStale() {
    std::fprintf(stderr, "[in-flight stale]\n");
    ScrubCoalescer c;

    // 1
    auto s0 = c.submit({0, 100});
    check(s0.accepted, "gen0 submit accepted");
    check(!s0.supersededPending, "gen0 は何も supersede しない");

    // 2
    auto taken0 = c.takePending();
    check(taken0.has_value(), "gen0 take できる");
    checkEq(taken0->generation, 0LL, "take したのは gen0");
    check(c.hasInFlight(), "gen0 が in-flight");

    // 3
    auto s1 = c.submit({1, 200});
    check(s1.accepted, "gen1 submit accepted");
    check(!s1.supersededPending, "gen1 時点で pending は空なので supersede なし");

    // 4
    auto s2 = c.submit({2, 300});
    check(s2.accepted, "gen2 submit accepted");
    check(s2.supersededPending, "gen2 が未処理の gen1 を supersede する");
    checkEq(c.latestSubmittedGeneration(), 2LL, "latestSubmitted は 2");

    // 5, 6
    auto d0 = c.complete({0, 100, true});
    check(d0 == ScrubResultDecision::RejectStale, "gen0 の結果は stale として棄却される");
    checkEq(c.finalDisplayedGeneration(), -1LL, "gen0 は display に反映されない");

    // 7
    auto taken2 = c.takePending();
    check(taken2.has_value(), "gen2 take できる");
    checkEq(taken2->generation, 2LL, "take したのは gen2");
    auto d2 = c.complete({2, 300, true});
    check(d2 == ScrubResultDecision::Accept, "gen2 は accept される");

    // 8 期待値
    checkEq(c.submitted(), 3LL, "submitted = 3");
    checkEq(c.superseded(), 1LL, "superseded = 1");
    checkEq(c.decoded(), 2LL, "decoded = 2");
    checkEq(c.staleRejected(), 1LL, "stale_rejected = 1");
    checkEq(c.accepted(), 1LL, "accepted = 1");
    checkEq(c.finalDisplayedGeneration(), 2LL, "final displayed generation = 2");
    checkEq(c.finalDisplayedFrame(), 300LL, "final displayed frame = 300");

    check(c.countersBalanced(), "submitted == superseded + decoded");
    check(c.decodeCountersBalanced(), "decoded == accepted + stale + failed");
    checkEq(c.contractViolations(), 0LL, "契約違反なし");
}

// 新しい要求が無ければ結果は accept される
void testNoNewerRequestAccepts() {
    std::fprintf(stderr, "[新要求なし -> accept]\n");
    ScrubCoalescer c;
    c.submit({0, 10});
    auto t = c.takePending();
    check(t.has_value(), "take できる");
    auto d = c.complete({0, 10, true});
    check(d == ScrubResultDecision::Accept, "最新のままなので accept");
    checkEq(c.accepted(), 1LL, "accepted = 1");
    checkEq(c.staleRejected(), 0LL, "stale なし");
    check(c.countersBalanced(), "counter 整合");
}

// 同一 generation の二重 complete は明示的に拒否する
void testDoubleComplete() {
    std::fprintf(stderr, "[二重 complete]\n");
    ScrubCoalescer c;
    c.submit({0, 10});
    c.takePending();
    auto d1 = c.complete({0, 10, true});
    check(d1 == ScrubResultDecision::Accept, "1 回目は accept");
    auto d2 = c.complete({0, 10, true});
    check(d2 == ScrubResultDecision::NotInFlight, "2 回目は NotInFlight として拒否");
    checkEq(c.accepted(), 1LL, "accepted は増えない");
    checkEq(c.decoded(), 1LL, "decoded も増えない");
    checkEq(c.contractViolations(), 1LL, "契約違反として記録される");
}

// latest より未来の generation は fail-closed
void testFutureGeneration() {
    std::fprintf(stderr, "[未来の generation]\n");
    ScrubCoalescer c;
    c.submit({0, 10});
    c.takePending();
    // in-flight は gen0 だが、gen5 の結果が来たとする
    auto d = c.complete({5, 50, true});
    check(d == ScrubResultDecision::NotInFlight, "in-flight と違うので拒否");
    checkEq(c.accepted(), 0LL, "accept しない");

    // in-flight の generation 自体が latest を超える人工ケース
    ScrubCoalescer c2;
    c2.submit({3, 30});
    auto t = c2.takePending();
    check(t.has_value(), "take できる");
    // 内部状態を壊さずに未来を作れないので、in-flight と同じ generation で
    // latest だけ巻き戻すことはできない。ここでは in-flight 不一致経路で
    // fail-closed になることを確認済みとする。
    checkEq(c2.contractViolations(), 0LL, "正常系では違反なし");
}

// pending が無いときの take
void testTakeWithoutPending() {
    std::fprintf(stderr, "[pending なしで take]\n");
    ScrubCoalescer c;
    check(!c.takePending().has_value(), "pending が無ければ nullopt");
    c.submit({0, 10});
    check(c.takePending().has_value(), "1 件目は取れる");
    check(!c.takePending().has_value(), "続けて取ろうとしても取れない");
}

// done 後は submit を受理しない
void testSubmitAfterDone() {
    std::fprintf(stderr, "[done 後の submit]\n");
    ScrubCoalescer c;
    c.submit({0, 10});
    c.takePending();
    c.complete({0, 10, true});
    c.markDone();
    auto s = c.submit({1, 20});
    check(!s.accepted, "done 後は受理しない");
    checkEq(c.submitted(), 1LL, "submitted は増えない");
    checkEq(c.latestSubmittedGeneration(), 0LL, "latest も変わらない");
}

// 最終要求は必ず accept される (drain した場合)
void testFinalRequestAccepted() {
    std::fprintf(stderr, "[最終要求は必ず accept]\n");
    ScrubCoalescer c;
    // 連続投入して pending をどんどん置き換える
    for (long long g = 0; g < 10; g++)
        c.submit({g, g * 10});
    // ここまでで take は 0 回。pending は gen9 のみ。
    checkEq(c.superseded(), 9LL, "9 件が supersede された");

    auto t = c.takePending();
    check(t.has_value() && t->generation == 9, "取れるのは最新の gen9");
    auto d = c.complete({9, 90, true});
    check(d == ScrubResultDecision::Accept, "最終要求は accept");
    checkEq(c.finalDisplayedGeneration(), c.latestSubmittedGeneration(),
            "final displayed == latest submitted");
    check(c.countersBalanced(), "submitted(10) == superseded(9) + decoded(1)");
}

// decode 失敗は accepted にも stale にも数えない
void testDecodeFailed() {
    std::fprintf(stderr, "[decode 失敗]\n");
    ScrubCoalescer c;
    c.submit({0, 10});
    c.takePending();
    auto d = c.complete({0, 10, false});
    check(d == ScrubResultDecision::DecodeFailed, "DecodeFailed が返る");
    checkEq(c.decodeFailed(), 1LL, "decode_failed = 1");
    checkEq(c.accepted(), 0LL, "accepted に数えない");
    checkEq(c.staleRejected(), 0LL, "stale にも数えない");
    check(c.decodeCountersBalanced(), "decoded == accepted + stale + failed");
}

// accept された generation は単調増加する
void testAcceptedMonotonic() {
    std::fprintf(stderr, "[accepted は単調増加]\n");
    ScrubCoalescer c;
    long long prev = -1;
    bool monotonic = true;
    for (long long g = 0; g < 20; g++) {
        c.submit({g, g});
        auto t = c.takePending();
        if (!t)
            continue;
        if (c.complete({t->generation, t->frame, true}) == ScrubResultDecision::Accept) {
            if (c.finalDisplayedGeneration() <= prev)
                monotonic = false;
            prev = c.finalDisplayedGeneration();
        }
    }
    check(monotonic, "accept した generation は単調増加");
    checkEq(c.accepted(), 20LL, "全件 accept");
}

} // namespace

int main() {
    testInFlightStale();
    testNoNewerRequestAccepts();
    testDoubleComplete();
    testFutureGeneration();
    testTakeWithoutPending();
    testSubmitAfterDone();
    testFinalRequestAccepted();
    testDecodeFailed();
    testAcceptedMonotonic();

    std::fprintf(stderr, "\n%d 検査中 %d 件失敗\n", gChecks, gFailures);
    if (gFailures == 0)
        std::fprintf(stderr, "ScrubCoalescer 単体テスト: 全て通過\n");
    return gFailures == 0 ? 0 : 1;
}
