/*
 * B3-I6B。frameSwapped callbackがformal transactionとして扱われる条件。
 *
 * production rendererとintegration testが同じ判定を使う。renderer側で条件式を複製しない。
 */
#ifndef MVM_APP_PREVIEW_FORMAL_PRESENT_JOIN_ADMISSION_H
#define MVM_APP_PREVIEW_FORMAL_PRESENT_JOIN_ADMISSION_H

namespace mvm::app {

struct FormalPresentJoinAdmissionInput {
    bool formalSchedulerEnabled = false;
    bool nativeCaptureActive = false;
    // first protocol fatalが既にlatchされているか。
    bool protocolFatalLatched = false;
    bool domainReached = false;
    bool formalCaptureActive = false;
};

struct FormalPresentJoinAdmission {
    // one-shot receiptをconsumeしてexact recordを引く経路へ入ってよいか。
    bool formalEnvelopeActive = false;
    // bind / commit / queue dequeueまで進めてよいか。
    bool joinCommitAllowed = false;
};

// post-fatalのPresentはformal join candidateにしない。Present自体は抑止しない。
constexpr FormalPresentJoinAdmission
formalPresentJoinAdmission(const FormalPresentJoinAdmissionInput& input) {
    FormalPresentJoinAdmission admission;
    admission.formalEnvelopeActive =
        input.formalSchedulerEnabled && input.nativeCaptureActive && !input.protocolFatalLatched;
    admission.joinCommitAllowed =
        !input.protocolFatalLatched && !input.domainReached && input.formalCaptureActive;
    return admission;
}

} // namespace mvm::app

#endif
