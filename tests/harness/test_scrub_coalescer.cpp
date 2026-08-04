// mvm Phase 0 / S6 - ScrubCoalescer の単体テスト
//
// 純粋な状態機械だけを対象にする。スレッドも実時間も使わないので、
// 「decode 中に新しい要求が来た結果」を決定論的に再現できる。
// スレッドを使う統合テストは test_scrub_threaded.cpp にある。
//
// 各 decision 分岐を実際に通ったかをカウンタで数え、最後に検査する。
// 「テスト済み」と言えるのは、分岐を実際に通った証拠がある場合だけである。

#include "scrub_coalescer.h"

#include <cstdio>
#include <map>
#include <string>

namespace {

int gFailures = 0;
int gChecks = 0;

// どの decision を何回通ったか。到達していない分岐を検出する。
std::map<bench::ScrubResultDecision, int> gDecisionHits;

const char* decisionName(bench::ScrubResultDecision d) {
    using D = bench::ScrubResultDecision;
    switch (d) {
    case D::DisplayLatest:
        return "DisplayLatest";
    case D::DisplayLagging:
        return "DisplayLagging";
    case D::RejectRegression:
        return "RejectRegression";
    case D::InvalidFutureGeneration:
        return "InvalidFutureGeneration";
    case D::NotInFlight:
        return "NotInFlight";
    case D::DecodeFailed:
        return "DecodeFailed";
    }
    return "?";
}

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

// complete をラップして decision の到達を記録する
ScrubResultDecision doComplete(ScrubCoalescer& c, const ScrubResult& r) {
    auto d = c.complete(r);
    gDecisionHits[d]++;
    return d;
}

// --------------------------------------------------------------------------
// decode 中に新要求が来た結果は DisplayLagging として表示する
// --------------------------------------------------------------------------
void testInFlightLagging() {
    std::fprintf(stderr, "[in-flight 中の新要求 -> DisplayLagging]\n");
    ScrubCoalescer c;

    auto s0 = c.submit(100);
    checkEq(s0.generation, 0LL, "gen0 が採番される");
    check(s0.accepted, "gen0 受理");

    auto taken0 = c.takePending();
    check(taken0.has_value() && taken0->generation == 0, "gen0 を take");

    auto s1 = c.submit(200);
    checkEq(s1.generation, 1LL, "gen1 が採番される");
    check(!s1.supersededPending, "pending は空なので supersede なし");

    auto s2 = c.submit(300);
    checkEq(s2.generation, 2LL, "gen2 が採番される");
    check(s2.supersededPending, "gen2 が未処理の gen1 を supersede");
    checkEq(c.latestSubmittedGeneration(), 2LL, "latestSubmitted = 2");

    // gen0 の結果。latest ではないが、表示中 (-1) より新しいので表示する。
    auto d0 = doComplete(c, {0, 100, true});
    check(d0 == ScrubResultDecision::DisplayLagging, "gen0 は DisplayLagging (捨てない)");
    checkEq(c.lastDisplayedGeneration(), 0LL, "表示は gen0 へ進む");
    checkEq(c.lastDisplayedFrame(), 100LL, "表示フレームは 100");

    // gen2 を処理すると最新なので DisplayLatest
    auto taken2 = c.takePending();
    check(taken2.has_value() && taken2->generation == 2, "gen2 を take");
    auto d2 = doComplete(c, {2, 300, true});
    check(d2 == ScrubResultDecision::DisplayLatest, "gen2 は DisplayLatest");

    checkEq(c.submitted(), 3LL, "submitted = 3");
    checkEq(c.supersededPending(), 1LL, "superseded_pending = 1");
    checkEq(c.decoded(), 2LL, "decoded = 2");
    checkEq(c.displayLagging(), 1LL, "display_lagging = 1");
    checkEq(c.displayLatest(), 1LL, "display_latest = 1");
    checkEq(c.displayedTotal(), 2LL, "displayed_total = 2");
    checkEq(c.rejectRegression(), 0LL, "reject_regression = 0");
    checkEq(c.lastDisplayedGeneration(), c.latestSubmittedGeneration(),
            "final displayed == latest submitted");
    check(c.countersBalanced(), "submitted == superseded_pending + decoded");
    check(c.decodeCountersBalanced(), "decoded == latest + lagging + regression + failed");
    checkEq(c.contractViolations(), 0LL, "契約違反なし");
}

// 表示は決して巻き戻らない
void testNoRegression() {
    std::fprintf(stderr, "[表示は巻き戻らない]\n");
    ScrubCoalescer c;
    c.submit(10); // gen0
    auto t0 = c.takePending();
    c.submit(20); // gen1
    auto d1 = doComplete(c, {0, 10, true});
    check(d1 == ScrubResultDecision::DisplayLagging, "gen0 表示");
    (void)t0;

    // gen1 を take して complete -> DisplayLatest
    auto t1 = c.takePending();
    check(t1.has_value(), "gen1 take");
    auto d2 = doComplete(c, {1, 20, true});
    check(d2 == ScrubResultDecision::DisplayLatest, "gen1 は最新なので DisplayLatest");

    // 既に表示した gen1 と同じものをもう一度 complete しようとする
    auto d3 = doComplete(c, {1, 20, true});
    check(d3 == ScrubResultDecision::NotInFlight, "二重 complete は NotInFlight");
    checkEq(c.lastDisplayedGeneration(), 1LL, "表示は変わらない");
}

// 表示中以下の generation は RejectRegression
void testRejectRegression() {
    std::fprintf(stderr, "[RejectRegression]\n");
    ScrubCoalescer c;
    // gen0 を take して in-flight のまま、gen1 を submit -> take できない
    c.submit(10); // gen0
    auto t0 = c.takePending();
    check(t0.has_value(), "gen0 take");
    c.submit(20); // gen1
    c.submit(30); // gen2 (gen1 を supersede)

    // 先に gen2 を処理したい状況を作る: gen0 を complete して表示を gen0 へ
    doComplete(c, {0, 10, true});
    auto t2 = c.takePending();
    check(t2.has_value() && t2->generation == 2, "gen2 take");
    doComplete(c, {2, 30, true}); // 表示 gen2

    // ここで gen1 相当の古い結果が来たとする (in-flight ではないので NotInFlight)
    auto dOld = doComplete(c, {1, 20, true});
    check(dOld == ScrubResultDecision::NotInFlight, "in-flight でない古い結果は NotInFlight");

    // in-flight 経由で「表示中以下」を作る:
    // gen3 を submit / take したあと、表示を先に gen4 へ進めることはできないので、
    // 同じ generation を再表示する経路を使う。
    ScrubCoalescer c2;
    c2.submit(1); // gen0
    auto a = c2.takePending();
    check(a.has_value(), "take");
    doComplete(c2, {0, 1, true}); // 表示 gen0
    // gen0 をもう一度 in-flight にすることはできないため、
    // RejectRegression は「表示済みと同じ generation が in-flight で再完了」する
    // 経路でしか起きない。実運用では起きない防御的分岐であることを記録する。
    checkEq(c2.rejectRegression(), 0LL, "通常経路では RejectRegression は起きない");
}

// 未投入の generation は fail-closed。future 判定は in-flight 判定より先。
void testFutureGeneration() {
    std::fprintf(stderr, "[未来の generation -> InvalidFutureGeneration]\n");
    ScrubCoalescer c;
    c.submit(10); // gen0
    auto t = c.takePending();
    check(t.has_value(), "gen0 take");

    // latestSubmitted は 0。gen5 の結果は未投入なので future。
    auto d = doComplete(c, {5, 50, true});
    check(d == ScrubResultDecision::InvalidFutureGeneration,
          "未投入 generation は InvalidFutureGeneration");
    checkEq(c.contractViolations(), 1LL, "契約違反として記録される");
    checkEq(c.decoded(), 0LL, "decoded に数えない");
    check(c.hasInFlight(), "in-flight は保持されたまま (bogus な結果で壊さない)");

    // in-flight の gen0 は正しく完了できる
    auto d0 = doComplete(c, {0, 10, true});
    check(d0 == ScrubResultDecision::DisplayLatest, "gen0 は正常に完了する");
}

// generation の逆行 submit は拒否する
void testRegressionSubmit() {
    std::fprintf(stderr, "[generation 逆行 submit]\n");
    ScrubCoalescer c;
    c.submitWithGeneration({5, 50});
    checkEq(c.latestSubmittedGeneration(), 5LL, "latestSubmitted = 5");

    auto s = c.submitWithGeneration({3, 30});
    check(!s.accepted, "逆行 submit は拒否");
    checkEq(c.latestSubmittedGeneration(), 5LL, "latestSubmitted は巻き戻らない");
    checkEq(c.submitted(), 1LL, "submitted は増えない");
    checkEq(c.contractViolations(), 1LL, "契約違反として記録");

    auto dup = c.submitWithGeneration({5, 55});
    check(!dup.accepted, "同一 generation の重複も拒否");
    checkEq(c.contractViolations(), 2LL, "契約違反 2 件");
}

// pending の supersede
void testSupersedePending() {
    std::fprintf(stderr, "[pending supersede]\n");
    ScrubCoalescer c;
    for (int i = 0; i < 10; i++)
        c.submit(i * 10);
    checkEq(c.supersededPending(), 9LL, "9 件が decode 前に置換された");
    auto t = c.takePending();
    check(t.has_value() && t->generation == 9, "残るのは最新の gen9");
}

// pending が無いときの take
void testTakeWithoutPending() {
    std::fprintf(stderr, "[pending なしで take]\n");
    ScrubCoalescer c;
    check(!c.takePending().has_value(), "pending が無ければ nullopt");
    c.submit(10);
    check(c.takePending().has_value(), "1 件目は取れる");
    check(!c.takePending().has_value(), "in-flight 中は取れない");
}

// done 後は submit を受理しない
void testSubmitAfterDone() {
    std::fprintf(stderr, "[done 後の submit]\n");
    ScrubCoalescer c;
    c.submit(10);
    c.takePending();
    doComplete(c, {0, 10, true});
    c.markDone();
    auto s = c.submit(20);
    check(!s.accepted, "done 後は受理しない");
    checkEq(c.submitted(), 1LL, "submitted は増えない");
    checkEq(c.latestSubmittedGeneration(), 0LL, "latest も変わらない");
}

// decode 失敗は表示にも regression にも数えない
void testDecodeFailed() {
    std::fprintf(stderr, "[decode 失敗]\n");
    ScrubCoalescer c;
    c.submit(10);
    c.takePending();
    auto d = doComplete(c, {0, 10, false});
    check(d == ScrubResultDecision::DecodeFailed, "DecodeFailed");
    checkEq(c.decodeFailed(), 1LL, "decode_failed = 1");
    checkEq(c.displayedTotal(), 0LL, "表示に数えない");
    checkEq(c.rejectRegression(), 0LL, "regression にも数えない");
    checkEq(c.lastDisplayedGeneration(), -1LL, "表示は進まない");
    check(c.decodeCountersBalanced(), "decode counter 整合");
}

// 表示 generation は単調増加し、最終要求は必ず DisplayLatest
void testMonotonicAndFinalCatchup() {
    std::fprintf(stderr, "[表示は単調増加 / 最終要求は DisplayLatest]\n");
    ScrubCoalescer c;
    long long prevDisplayed = -1;
    bool monotonic = true;

    // 投入が decode より速い状況を模す。
    // take してから追加で submit することで、完了時点では最新でない
    // (= DisplayLagging になる) 状況を作る。
    for (int i = 0; i < 50; i++) {
        c.submit(i * 3);
        auto t = c.takePending();
        if (!t)
            continue;
        // decode 中に新しい要求が 2 件来たとする
        c.submit(i * 3 + 1);
        c.submit(i * 3 + 2);
        auto d = doComplete(c, {t->generation, t->frame, true});
        if (d == ScrubResultDecision::DisplayLatest || d == ScrubResultDecision::DisplayLagging) {
            if (c.lastDisplayedGeneration() <= prevDisplayed)
                monotonic = false;
            prevDisplayed = c.lastDisplayedGeneration();
        }
    }
    check(monotonic, "表示 generation は巻き戻らない");
    check(c.displayLagging() > 0, "DisplayLagging が実際に発生している");

    // 入力停止後に drain すると最終要求が DisplayLatest になる
    c.markDone();
    while (c.hasPending()) {
        auto t = c.takePending();
        if (!t)
            break;
        doComplete(c, {t->generation, t->frame, true});
    }
    checkEq(c.lastDisplayedGeneration(), c.latestSubmittedGeneration(),
            "final displayed == latest submitted");
    check(c.countersBalanced(), "counter invariant");
    check(c.decodeCountersBalanced(), "decode counter invariant");
}

} // namespace

int main() {
    testInFlightLagging();
    testNoRegression();
    testRejectRegression();
    testFutureGeneration();
    testRegressionSubmit();
    testSupersedePending();
    testTakeWithoutPending();
    testSubmitAfterDone();
    testDecodeFailed();
    testMonotonicAndFinalCatchup();

    // 分岐の到達を検査する。到達していない分岐を「テスト済み」と呼ばない。
    std::fprintf(stderr, "\n[decision 到達回数]\n");
    using D = ScrubResultDecision;
    const D required[] = {D::DisplayLatest, D::DisplayLagging, D::InvalidFutureGeneration,
                          D::NotInFlight, D::DecodeFailed};
    for (auto d : required) {
        int hits = gDecisionHits.count(d) ? gDecisionHits[d] : 0;
        std::fprintf(stderr, "  %-24s %d 回\n", decisionName(d), hits);
        check(hits > 0, std::string(decisionName(d)) + " の分岐を実際に通った");
    }
    // RejectRegression は防御的分岐であり、通常経路では到達しない。
    // 到達しないこと自体を記録する (テスト済みとは呼ばない)。
    int rr = gDecisionHits.count(D::RejectRegression) ? gDecisionHits[D::RejectRegression] : 0;
    std::fprintf(stderr, "  %-24s %d 回 (防御的分岐。通常経路では到達しない)\n",
                 decisionName(D::RejectRegression), rr);

    std::fprintf(stderr, "\n%d 検査中 %d 件失敗\n", gChecks, gFailures);
    if (gFailures == 0)
        std::fprintf(stderr, "ScrubCoalescer 単体テスト: 全て通過\n");
    return gFailures == 0 ? 0 : 1;
}
