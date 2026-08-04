// mvm Phase 0 / S6 - ScrubCoalescer のスレッド統合テスト
//
// 単体テスト (test_scrub_coalescer.cpp) は状態機械だけを見る。
// こちらは「実際の mutex と condition_variable の下で契約が守られるか」を見る。
//
// sleep を使わない。sleep で待つテストは、遅いマシンで偽陽性を出すか、
// 速いマシンで意図した interleaving を作れずに素通りするかのどちらかになる。
// 代わりに condition_variable による明示的なハンドシェイクで順序を固定する。
//
// scrub-bench (mvm_bench_compose.cpp) の consumer ループと同じ構造を、
// MLT を外して再現している。decode は即座に成功する擬似 decode である。

#include "scrub_coalescer.h"

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool cond, const std::string& what) {
    gChecks++;
    if (!cond) {
        gFailures++;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

using namespace bench;

// --------------------------------------------------------------------------
// 1. ハンドシェイクによる決定論テスト
//
//    consumer を「take した直後」で必ず止める。その間に本体が submit を
//    追加してから complete を許可する。これにより DisplayLagging が
//    起きる interleaving を、タイミングに依存せず確実に作れる。
// --------------------------------------------------------------------------

void testHandshakeLagging() {
    ScrubCoalescer coal;
    std::mutex mu;
    std::condition_variable cv;

    // 単調増加のカウンタで待つ。bool フラグだと consumer が次の要求を
    // take し直した瞬間に本体側が「false の窓」を見逃して永久待機する。
    long long takenCount = 0;     // consumer が take した回数
    long long completedCount = 0; // consumer が complete した回数
    bool mayComplete = false;     // 本体が complete を許可した
    bool stop = false;
    ScrubRequest inFlight{};
    std::vector<ScrubResultDecision> decisions;

    std::thread consumer([&] {
        for (;;) {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return coal.hasPending() || stop; });
            if (stop && !coal.hasPending())
                return;
            auto t = coal.takePending();
            if (!t)
                continue;
            inFlight = *t;
            takenCount++;
            cv.notify_all();

            // decode 中を模す。本体が submit を積むまでここで待つ。
            cv.wait(lk, [&] { return mayComplete; });
            mayComplete = false;
            decisions.push_back(coal.complete({inFlight.generation, inFlight.frame, true}));
            completedCount++;
            cv.notify_all();
        }
    });

    auto submitLocked = [&](long long frame) {
        std::lock_guard<std::mutex> lk(mu);
        coal.submit(frame);
        cv.notify_all();
    };
    auto waitTaken = [&](long long n) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return takenCount >= n; });
    };
    auto allowComplete = [&](long long n) {
        {
            std::lock_guard<std::mutex> lk(mu);
            mayComplete = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return completedCount >= n; });
    };

    // gen0 を take させてから gen1, gen2 を積む -> gen0 完了時は最新でない
    submitLocked(100);
    waitTaken(1);
    submitLocked(200);
    submitLocked(300); // gen1 は decode 前に置換される
    allowComplete(1);

    // gen2 を take させ、追加投入なしで完了させる -> 最新なので DisplayLatest
    waitTaken(2);
    allowComplete(2);

    {
        std::lock_guard<std::mutex> lk(mu);
        stop = true;
    }
    cv.notify_all();
    consumer.join();

    check(decisions.size() == 2, "2 件 complete した");
    if (decisions.size() == 2) {
        check(decisions[0] == ScrubResultDecision::DisplayLagging,
              "take 後に submit が積まれた結果は DisplayLagging");
        check(decisions[1] == ScrubResultDecision::DisplayLatest,
              "追加投入なしの結果は DisplayLatest");
    }
    check(coal.supersededPending() == 1, "pending 置換が 1 回起きた");
    check(coal.lastDisplayedFrame() == 300, "最終要求のフレームが表示されている");
    check(coal.contractViolations() == 0, "契約違反なし");
    check(coal.countersBalanced(), "counter invariant");
    check(coal.decodeCountersBalanced(), "decode counter invariant");
}

// --------------------------------------------------------------------------
// 2. 自由な interleaving での不変条件テスト
//
//    ここでは順序を固定しない。OS のスケジューリングに任せる。
//    そのかわり検査するのはタイミングに依存しない不変条件だけにする。
//    「何回 DisplayLagging したか」のような値は検査しない。
// --------------------------------------------------------------------------

void testFreeInterleavingInvariants(int iteration) {
    ScrubCoalescer coal;
    std::mutex mu;
    std::condition_variable cv;

    const int kRequests = 500;
    long long prevDisplayed = -1;
    bool regressionSeen = false;
    bool violationSeen = false;

    std::thread consumer([&] {
        for (;;) {
            ScrubRequest req;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return coal.hasPending() || coal.isDone(); });
                auto t = coal.takePending();
                if (!t) {
                    if (coal.isDone())
                        return;
                    continue;
                }
                req = *t;
            }
            // decode 相当。ロックの外で行う。
            volatile long long sink = 0;
            for (int i = 0; i < 200; i++)
                sink += i * req.frame;
            (void)sink;
            {
                std::lock_guard<std::mutex> lk(mu);
                auto d = coal.complete({req.generation, req.frame, true});
                if (d == ScrubResultDecision::DisplayLatest ||
                    d == ScrubResultDecision::DisplayLagging) {
                    if (req.generation <= prevDisplayed)
                        regressionSeen = true;
                    prevDisplayed = req.generation;
                } else if (d != ScrubResultDecision::RejectRegression) {
                    violationSeen = true;
                }
            }
            cv.notify_all();
        }
    });

    for (int i = 0; i < kRequests; i++) {
        {
            std::lock_guard<std::mutex> lk(mu);
            coal.submit(i * 7 + iteration);
        }
        cv.notify_all();
    }
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return !coal.hasPending() && !coal.hasInFlight(); });
        coal.markDone();
    }
    cv.notify_all();
    consumer.join();

    check(!regressionSeen, "表示 generation が巻き戻らない");
    check(!violationSeen, "契約違反の decision が出ない");
    check(coal.contractViolations() == 0, "contractViolations が 0");
    check(coal.submitted() == kRequests, "投入数が一致する");
    check(coal.countersBalanced(), "counter invariant");
    check(coal.decodeCountersBalanced(), "decode counter invariant");
    // 入力停止後に drain するので、最終要求は必ず表示される。
    check(coal.lastDisplayedGeneration() == coal.latestSubmittedGeneration(),
          "最終要求が表示されている");
    check(coal.lastDisplayedFrame() == (kRequests - 1) * 7 + iteration,
          "最終要求のフレームが表示されている");
    check(coal.displayedTotal() > 0, "1 件以上表示された");
}

} // namespace

int main() {
    testHandshakeLagging();
    // interleaving を変えるため複数回まわす。sleep は使わない。
    for (int i = 0; i < 8; i++)
        testFreeInterleavingInvariants(i);

    std::fprintf(stderr, "\n%d 検査中 %d 件失敗\n", gChecks, gFailures);
    if (gFailures == 0)
        std::fprintf(stderr, "ScrubCoalescer スレッド統合テスト: 全て通過\n");
    return gFailures == 0 ? 0 : 1;
}
