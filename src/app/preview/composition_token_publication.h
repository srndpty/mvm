/*
 * B3-I6B。composition token publicationのRAII gate。
 *
 * production rendererとintegration testが**同じclass**を使う。testがlogicを複製すると
 * 「production destructorのgateが実際に効く」ことを証明できないため、複製は禁止する。
 * Qtにもrenderer stateにも依存させない。sink adapterだけがそれらを知る。
 */
#ifndef MVM_APP_PREVIEW_COMPOSITION_TOKEN_PUBLICATION_H
#define MVM_APP_PREVIEW_COMPOSITION_TOKEN_PUBLICATION_H

#include "app/preview/native_present_hook_abi.h"

namespace mvm::app {

// publication siteが触る外部effectの全体。testはこれを実装して観測する。
class CompositionTokenPublicationSink {
public:
    virtual ~CompositionTokenPublicationSink() = default;

    // callback-localのfail-able pre-Present validationが1つでも失敗したか。
    virtual bool protocolFatalLatched() const = 0;
    virtual bool terminalExitTracking() const = 0;
    virtual void noteDestructorEntered() = 0;
    virtual void noteDestructorComplete() = 0;
    // Qtのthread_local pending tokenへ実際にpublishする唯一の経路。
    virtual bool publishCompositionToken(const MvmNativePresentCompositionToken& token) = 0;
    virtual void notePublicationAttempt(bool succeeded,
                                        const MvmNativePresentCompositionToken& token) = 0;
    virtual void notePublicationFailure() = 0;
    // 抑止はtransport failureではない。専用counterへ記録する。
    virtual void noteSuppressedBeforePresent() = 0;
};

// scope exitでだけpublishする。fatalが立っていればpublishせずQt pending tokenを残さない。
// nearest/latest/QPC/serial推定によるcancelやrecoveryは持たない。
class CompositionTokenPublication {
public:
    CompositionTokenPublication(CompositionTokenPublicationSink& sink, bool captureActive,
                                const MvmNativePresentCompositionToken& token)
        : sink_(sink), token_(token), active_(captureActive) {}

    CompositionTokenPublication(const CompositionTokenPublication&) = delete;
    CompositionTokenPublication& operator=(const CompositionTokenPublication&) = delete;

    ~CompositionTokenPublication() { publishOnScopeExit(); }

    void setTokenValid(bool valid) { valid_ = valid; }

    bool publicationAllowed() const { return active_ && valid_ && !sink_.protocolFatalLatched(); }

private:
    void publishOnScopeExit() {
        if (!active_)
            return;
        const bool terminalExitTracking = sink_.terminalExitTracking();
        if (terminalExitTracking)
            sink_.noteDestructorEntered();
        if (!publicationAllowed()) {
            // fatal transactionはQt pending tokenを残さない。以後のPresentはformal token
            // を持たず、formal join candidateにならない。
            sink_.noteSuppressedBeforePresent();
            if (terminalExitTracking)
                sink_.noteDestructorComplete();
            return;
        }
        const bool succeeded = sink_.publishCompositionToken(token_);
        sink_.notePublicationAttempt(succeeded, token_);
        if (!succeeded)
            sink_.notePublicationFailure();
        if (terminalExitTracking)
            sink_.noteDestructorComplete();
    }

    CompositionTokenPublicationSink& sink_;
    const MvmNativePresentCompositionToken& token_;
    bool active_ = false;
    bool valid_ = false;
};

} // namespace mvm::app

#endif
