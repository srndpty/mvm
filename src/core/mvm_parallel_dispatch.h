#ifndef MVM_CORE_PARALLEL_DISPATCH_H
#define MVM_CORE_PARALLEL_DISPATCH_H

namespace mvm::core {

// 2 sourceへのrequestを完了してからcompletion pollへ進む順序だけを追跡する。
// threadの同時実行やexecution intervalの重なりは、この契約の対象ではない。
class ParallelDispatchOrder final {
public:
    bool begin(long long qpc) {
        if (stage_ != Stage::Idle || qpc <= 0)
            return invalidate();
        requestStartQpc_ = qpc;
        stage_ = Stage::Started;
        return true;
    }

    bool requestA(long long qpc) {
        if (stage_ != Stage::Started || qpc < requestStartQpc_)
            return invalidate();
        stage_ = Stage::ARequested;
        return true;
    }

    bool requestB(long long qpc) {
        if (stage_ != Stage::ARequested || qpc < requestStartQpc_)
            return invalidate();
        stage_ = Stage::BRequested;
        return true;
    }

    bool dispatchComplete(long long qpc) {
        if (stage_ != Stage::BRequested || qpc < requestStartQpc_)
            return invalidate();
        stage_ = Stage::Complete;
        return true;
    }

    bool completionPoll() {
        if (stage_ != Stage::Complete && stage_ != Stage::Polling)
            return invalidate();
        stage_ = Stage::Polling;
        return true;
    }

    bool valid() const { return valid_; }

private:
    enum class Stage { Idle, Started, ARequested, BRequested, Complete, Polling, Invalid };

    bool invalidate() {
        valid_ = false;
        stage_ = Stage::Invalid;
        return false;
    }

    Stage stage_ = Stage::Idle;
    long long requestStartQpc_ = 0;
    bool valid_ = true;
};

} // namespace mvm::core

#endif
