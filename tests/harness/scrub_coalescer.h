/*
 * mvm Phase 0 / S6 - scrub の要求 coalescing と表示契約
 *
 * 位置づけ:
 *   Phase 0 の検証用。MLT にも Qt にも依存しない純粋なロジックであり、
 *   スレッドも時間も使わない。決定論的に単体テストできることが目的である。
 *   本番の Project Model や IMediaEngine へ広げないこと。
 *
 * 表示契約 (monotonic display):
 *
 *   result.generation <= lastDisplayedGeneration -> RejectRegression
 *   result.generation >  lastDisplayedGeneration -> 表示する
 *       result.generation == latestSubmittedGeneration -> DisplayLatest
 *       lastDisplayed < result.generation < latestSubmitted -> DisplayLagging
 *
 *   DisplayLagging は捨てない。新しい要求が pending でも、
 *   現在表示中より新しい decode 結果なら表示する。
 *   これによりスクラブ中も画が更新され続ける。
 *
 *   保証するのは「表示が巻き戻らないこと」であって
 *   「常に最新であること」ではない。
 *
 * 以前の strict latest-only 契約 (generation == latestSubmitted のときだけ表示) は
 * 廃止した。あれは投入間隔より decode が遅い間、入力が止まるまで
 * 何も表示されないという挙動になっていた。
 *
 * pending の latest-only coalescing と、in-flight を中断しない契約は維持する。
 * 最終要求は入力停止後に必ず DisplayLatest される。
 */

#ifndef MVM_SCRUB_COALESCER_H
#define MVM_SCRUB_COALESCER_H

#include <optional>

namespace bench {

struct ScrubRequest {
    long long generation = -1;
    long long frame = 0;
};

struct ScrubResult {
    long long generation = -1;
    long long frame = 0;
    bool decodeOk = true;
};

enum class ScrubResultDecision {
    DisplayLatest,           // 完了時点でも最新要求だった
    DisplayLagging,          // 最新ではないが、表示中より新しいので表示する
    RejectRegression,        // 表示中以下の generation。表示すると巻き戻る
    InvalidFutureGeneration, // 未投入の generation。契約違反 (fail-closed)
    NotInFlight,             // in-flight でない generation の complete (二重 complete 含む)
    DecodeFailed,            // decode 自体が失敗した
};

struct SubmitResult {
    bool accepted = false;          // done 後 / generation 逆行では受理しない
    bool supersededPending = false; // 未処理の pending を置き換えた
    long long generation = -1;      // 採番された generation
};

class ScrubCoalescer {
public:
    // 推奨 API。generation は内部で単調採番する。
    SubmitResult submit(long long frame) {
        ScrubRequest r;
        r.generation = nextGeneration_;
        r.frame = frame;
        return submitWithGeneration(r);
    }

    // 外部で generation を持つ場合。逆行・重複は拒否して契約違反に数える。
    // latestSubmitted を巻き戻してはいけない。
    SubmitResult submitWithGeneration(const ScrubRequest& request) {
        SubmitResult out;
        out.generation = request.generation;
        if (done_)
            return out; // done 後は受理しない
        if (request.generation <= latestSubmitted_) {
            contractViolations_++;
            return out; // 逆行・重複は拒否
        }
        if (hasPending_) {
            supersededPending_++;
            out.supersededPending = true;
        }
        pending_ = request;
        hasPending_ = true;
        latestSubmitted_ = request.generation;
        nextGeneration_ = request.generation + 1;
        submitted_++;
        out.accepted = true;
        return out;
    }

    // pending を 1 件取り出して in-flight にする。無ければ nullopt。
    std::optional<ScrubRequest> takePending() {
        if (!hasPending_ || hasInFlight_)
            return std::nullopt;
        hasPending_ = false;
        inFlight_ = pending_;
        hasInFlight_ = true;
        return inFlight_;
    }

    ScrubResultDecision complete(const ScrubResult& result) {
        // future 判定を in-flight 判定より先に行う。
        // 未投入の generation を受け取ること自体が契約違反であり、
        // in-flight の一致より重い異常だからである。
        if (result.generation > latestSubmitted_) {
            contractViolations_++;
            return ScrubResultDecision::InvalidFutureGeneration;
        }
        if (!hasInFlight_ || inFlight_.generation != result.generation) {
            contractViolations_++;
            return ScrubResultDecision::NotInFlight;
        }

        hasInFlight_ = false;
        decoded_++;

        if (!result.decodeOk) {
            decodeFailed_++;
            return ScrubResultDecision::DecodeFailed;
        }

        // 表示中以下なら巻き戻るので表示しない
        if (result.generation <= lastDisplayed_) {
            rejectRegression_++;
            return ScrubResultDecision::RejectRegression;
        }

        lastDisplayed_ = result.generation;
        lastDisplayedFrame_ = result.frame;
        if (result.generation == latestSubmitted_) {
            displayLatest_++;
            return ScrubResultDecision::DisplayLatest;
        }
        displayLagging_++;
        return ScrubResultDecision::DisplayLagging;
    }

    void markDone() { done_ = true; }

    bool isDone() const { return done_; }

    bool hasPending() const { return hasPending_; }

    bool hasInFlight() const { return hasInFlight_; }

    long long latestSubmittedGeneration() const { return latestSubmitted_; }

    long long lastDisplayedGeneration() const { return lastDisplayed_; }

    long long lastDisplayedFrame() const { return lastDisplayedFrame_; }

    long long submitted() const { return submitted_; }

    long long supersededPending() const { return supersededPending_; }

    long long decoded() const { return decoded_; }

    long long displayLatest() const { return displayLatest_; }

    long long displayLagging() const { return displayLagging_; }

    long long displayedTotal() const { return displayLatest_ + displayLagging_; }

    long long rejectRegression() const { return rejectRegression_; }

    long long decodeFailed() const { return decodeFailed_; }

    long long contractViolations() const { return contractViolations_; }

    // submitted == superseded_pending + decoded
    //   投入された要求は、decode 開始前に置換されるか decode されるかのどちらか。
    //   pending / in-flight が残っている間は成立しない (drain 前提)。
    bool countersBalanced() const {
        return !hasPending_ && !hasInFlight_ && submitted_ == supersededPending_ + decoded_;
    }

    // decoded == display_latest + display_lagging + reject_regression + decode_failed
    bool decodeCountersBalanced() const {
        return decoded_ == displayLatest_ + displayLagging_ + rejectRegression_ + decodeFailed_;
    }

private:
    ScrubRequest pending_{};
    ScrubRequest inFlight_{};
    bool hasPending_ = false;
    bool hasInFlight_ = false;
    bool done_ = false;

    long long nextGeneration_ = 0;
    long long latestSubmitted_ = -1;
    long long lastDisplayed_ = -1;
    long long lastDisplayedFrame_ = -1;

    long long submitted_ = 0;
    long long supersededPending_ = 0;
    long long decoded_ = 0;
    long long displayLatest_ = 0;
    long long displayLagging_ = 0;
    long long rejectRegression_ = 0;
    long long decodeFailed_ = 0;
    long long contractViolations_ = 0;
};

} // namespace bench

#endif // MVM_SCRUB_COALESCER_H
