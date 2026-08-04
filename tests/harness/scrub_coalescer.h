/*
 * mvm Phase 0 / S6 - scrub の要求 coalescing 状態機械
 *
 * 位置づけ:
 *   Phase 0 の検証用。MLT にも Qt にも依存しない純粋なロジックであり、
 *   スレッドも時間も使わない。決定論的に単体テストできることが目的である。
 *   本番の Project Model や IMediaEngine へ広げないこと。
 *
 * なぜ分離したか:
 *   修正前は scrub のループ内に判定が直接書かれており、
 *   「decode 中に新しい要求が来た古い結果」を accept していた。
 *   実時間に依存する形のままでは、この誤りを決定論的に再現できない。
 *
 * 判定の契約 (latestSubmittedGeneration が基準):
 *   result.generation <  latestSubmittedGeneration -> RejectStale
 *   result.generation == latestSubmittedGeneration -> Accept
 *   result.generation >  latestSubmittedGeneration -> InvalidFutureGeneration
 *                                                    (内部契約違反。fail-closed)
 *
 *   修正前は lastAcceptedGeneration と比較していた。consumer が 1 件ずつ順に
 *   処理する限り結果は常に generation 昇順で返るため、その比較では
 *   stale が一度も成立しなかった。
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
    Accept,                  // 最新要求の結果。表示してよい
    RejectStale,             // 古い結果。表示してはいけない
    InvalidFutureGeneration, // 未投入の generation。契約違反
    NotInFlight,             // in-flight でない generation の complete (二重 complete 含む)
    DecodeFailed,            // decode 自体が失敗した
};

struct SubmitResult {
    bool accepted = false;          // done 後は受理しない
    bool supersededPending = false; // 未処理の pending を置き換えた
};

/*
 * producer 側は submit() で要求を投入する。
 * consumer 側は takePending() で 1 件取り出し、処理後に complete() を呼ぶ。
 *
 * pending は常に最新 1 件だけ保持する。新しい要求が来たら古い pending は
 * supersede される。処理中 (in-flight) のものは中断しない。
 */
class ScrubCoalescer {
public:
    SubmitResult submit(const ScrubRequest& request) {
        SubmitResult out;
        if (done_)
            return out; // done 後は受理しない
        if (hasPending_) {
            superseded_++;
            out.supersededPending = true;
        }
        pending_ = request;
        hasPending_ = true;
        latestSubmitted_ = request.generation;
        submitted_++;
        out.accepted = true;
        return out;
    }

    // pending を 1 件取り出して in-flight にする。無ければ nullopt。
    std::optional<ScrubRequest> takePending() {
        if (!hasPending_)
            return std::nullopt;
        if (hasInFlight_)
            return std::nullopt; // 同時に 1 件しか処理しない
        hasPending_ = false;
        inFlight_ = pending_;
        hasInFlight_ = true;
        return inFlight_;
    }

    ScrubResultDecision complete(const ScrubResult& result) {
        // in-flight と一致しない complete は契約違反 (二重 complete を含む)
        if (!hasInFlight_ || inFlight_.generation != result.generation) {
            contractViolations_++;
            return ScrubResultDecision::NotInFlight;
        }
        // 未投入の generation は受け取ってはいけない
        if (result.generation > latestSubmitted_) {
            hasInFlight_ = false;
            contractViolations_++;
            return ScrubResultDecision::InvalidFutureGeneration;
        }

        hasInFlight_ = false;
        decoded_++;

        if (!result.decodeOk) {
            decodeFailed_++;
            return ScrubResultDecision::DecodeFailed;
        }
        if (result.generation < latestSubmitted_) {
            staleRejected_++;
            return ScrubResultDecision::RejectStale;
        }
        accepted_++;
        finalDisplayedGeneration_ = result.generation;
        finalDisplayedFrame_ = result.frame;
        return ScrubResultDecision::Accept;
    }

    void markDone() { done_ = true; }

    bool isDone() const { return done_; }

    bool hasPending() const { return hasPending_; }

    bool hasInFlight() const { return hasInFlight_; }

    long long latestSubmittedGeneration() const { return latestSubmitted_; }

    long long finalDisplayedGeneration() const { return finalDisplayedGeneration_; }

    long long finalDisplayedFrame() const { return finalDisplayedFrame_; }

    long long submitted() const { return submitted_; }

    long long superseded() const { return superseded_; }

    long long decoded() const { return decoded_; }

    long long accepted() const { return accepted_; }

    long long staleRejected() const { return staleRejected_; }

    long long decodeFailed() const { return decodeFailed_; }

    long long contractViolations() const { return contractViolations_; }

    // submitted == superseded + decoded
    //   投入された要求は、supersede されるか decode されるかのどちらか。
    //   ただし pending / in-flight が残っている間は成立しない (drain 前提)。
    bool countersBalanced() const {
        return !hasPending_ && !hasInFlight_ && submitted_ == superseded_ + decoded_;
    }

    // decoded == accepted + stale_rejected + decode_failed
    bool decodeCountersBalanced() const {
        return decoded_ == accepted_ + staleRejected_ + decodeFailed_;
    }

private:
    ScrubRequest pending_{};
    ScrubRequest inFlight_{};
    bool hasPending_ = false;
    bool hasInFlight_ = false;
    bool done_ = false;

    long long latestSubmitted_ = -1;
    long long finalDisplayedGeneration_ = -1;
    long long finalDisplayedFrame_ = -1;

    long long submitted_ = 0;
    long long superseded_ = 0;
    long long decoded_ = 0;
    long long accepted_ = 0;
    long long staleRejected_ = 0;
    long long decodeFailed_ = 0;
    long long contractViolations_ = 0;
};

} // namespace bench

#endif // MVM_SCRUB_COALESCER_H
