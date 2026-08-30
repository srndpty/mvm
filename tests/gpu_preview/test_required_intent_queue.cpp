#include "media/gpu_preview/required_intent_queue.h"

#include <cstdio>

namespace {

using Queue = mvm::gpu::RequiredIntentQueue;
using ReserveResult = mvm::gpu::RequiredIntentReserveResult;
using Error = mvm::gpu::RequiredIntentQueueError;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void positiveLifecycleAndDuplicate() {
    Queue queue;
    check(queue.start(3), "required queueを開始できません");
    const auto first = queue.reserveHead();
    const auto duplicate = queue.reserveHead();
    check(first.result == ReserveResult::Reserved && first.reservation.intentOrdinal == 0 &&
              first.reservation.reservationId != 0,
          "queue head 0をreserveできません");
    check(duplicate.result == ReserveResult::Duplicate &&
              duplicate.reservation.reservationId == first.reservation.reservationId &&
              duplicate.reservation.intentOrdinal == first.reservation.intentOrdinal,
          "duplicateが同一reservationを返しません");
    auto snapshot = queue.snapshot();
    check(snapshot.issuedCount == 1 && snapshot.dequeuedCount == 0 &&
              snapshot.activeReservationCount == 1,
          "reserveまたはduplicateがqueueをconsumeしました");

    check(
        queue.markRenderComplete(first.reservation.reservationId, first.reservation.intentOrdinal),
        "matching render completionを記録できません");
    snapshot = queue.snapshot();
    check(snapshot.renderedCount == 1 && snapshot.dequeuedCount == 0,
          "markRenderCompleteがqueueをconsumeしました");
    check(queue.commitQualified(first.reservation.reservationId, first.reservation.intentOrdinal),
          "qualified commitでheadをdequeueできません");
    snapshot = queue.snapshot();
    check(snapshot.qualifiedCommitCount == 1 && snapshot.dequeuedCount == 1 &&
              snapshot.activeReservationCount == 0 && snapshot.conservationValid,
          "qualified commitとexactly-one dequeueが一致しません");

    const auto second = queue.reserveHead();
    check(second.result == ReserveResult::Reserved && second.reservation.intentOrdinal == 1,
          "dequeue後の次headがrequired ordinal 1ではありません");
}

void plannedEndPreservesInflightAndTail() {
    Queue queue;
    check(queue.start(4), "planned-end queueを開始できません");
    const auto active = queue.reserveHead();
    check(queue.markRenderComplete(active.reservation.reservationId,
                                   active.reservation.intentOrdinal),
          "planned-end activeをrenderedへ進められません");
    check(queue.closePlannedWindow(), "PLANNED_WINDOW_ENDでqueueを閉じられません");
    const auto snapshot = queue.snapshot();
    check(snapshot.plannedWindowEnded && snapshot.activeReservationCount == 1 &&
              snapshot.activeReservationRendered && snapshot.dequeuedCount == 0 &&
              snapshot.unissuedTailCount == 3 && snapshot.conservationValid,
          "planned endがactive reservationまたはunissued tailを失いました");
    check(!snapshot.displaySatisfactionImported,
          "FinalState/display authorityをonline queueへ逆輸入しました");
}

void nonNormalCloseIsNotPlannedCompletion() {
    Queue queue;
    check(queue.start(2), "non-normal queueを開始できません");
    check(queue.closeWithoutNormalCompletion(), "non-normal closeに失敗しました");
    check(!queue.snapshot().plannedWindowEnded,
          "PLANNED_WINDOW_END以外をnormal completionにしました");
}

void exhaustedQueueDoesNotInventIdentity() {
    Queue queue;
    check(queue.start(1), "exhaustion queueを開始できません");
    const auto only = queue.reserveHead();
    check(
        queue.markRenderComplete(only.reservation.reservationId, only.reservation.intentOrdinal) &&
            queue.commitQualified(only.reservation.reservationId, only.reservation.intentOrdinal),
        "only required intentをcommitできません");
    const auto exhausted = queue.reserveHead();
    check(exhausted.result == ReserveResult::Exhausted &&
              exhausted.reservation.reservationId == 0 && queue.snapshot().dequeuedCount == 1,
          "empty queueがobserver由来のidentityを生成しました");
}

void mismatchAndUncommittedConsumeNothing() {
    {
        Queue queue;
        check(queue.start(2), "missing-render queueを開始できません");
        const auto value = queue.reserveHead();
        check(!queue.commitQualified(value.reservation.reservationId,
                                     value.reservation.intentOrdinal) &&
                  queue.error() == Error::RenderCompletionMissing &&
                  queue.snapshot().dequeuedCount == 0,
              "render未完了transactionをdequeueしました");
    }
    {
        Queue queue;
        check(queue.start(2), "mismatch queueを開始できません");
        const auto value = queue.reserveHead();
        check(queue.markRenderComplete(value.reservation.reservationId,
                                       value.reservation.intentOrdinal),
              "mismatch前のrender completionに失敗しました");
        check(!queue.commitQualified(value.reservation.reservationId + 1,
                                     value.reservation.intentOrdinal) &&
                  queue.error() == Error::ReservationMismatch &&
                  queue.snapshot().dequeuedCount == 0,
              "mismatched transactionをdequeueしました");
    }
}

void requiredSetRemainsImmutable() {
    Queue queue;
    check(queue.start(3), "immutable-set queueを開始できません");
    const auto before = queue.snapshot().requiredIntentOrdinals;
    const auto active = queue.reserveHead();
    check(queue.markRenderComplete(active.reservation.reservationId,
                                   active.reservation.intentOrdinal) &&
              queue.commitQualified(active.reservation.reservationId,
                                    active.reservation.intentOrdinal),
          "immutable-set transactionに失敗しました");
    check(queue.closePlannedWindow(), "immutable-set planned closeに失敗しました");
    const auto after = queue.snapshot();
    check(after.requiredSetImmutable && after.requiredIntentOrdinals == before &&
              after.requiredCount == 3,
          "required setをissuance/close中に変更しました");
}

} // namespace

int main() {
    positiveLifecycleAndDuplicate();
    plannedEndPreservesInflightAndTail();
    nonNormalCloseIsNotPlannedCompletion();
    exhaustedQueueDoesNotInventIdentity();
    mismatchAndUncommittedConsumeNothing();
    requiredSetRemainsImmutable();
    std::fprintf(stderr, "P2-D5-2/B3-I1 required-intent queue: 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}
