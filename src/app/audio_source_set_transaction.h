#ifndef MVM_APP_AUDIO_SOURCE_SET_TRANSACTION_H
#define MVM_APP_AUDIO_SOURCE_SET_TRANSACTION_H

#include <utility>
#include <vector>

namespace mvm::app {

enum class SourceSetReplaceResult {
    Success,
    OperationFailedRestored,
    OperationFailedReset,
    OperationFailedResetFailed,
};

// remove/addしか提供しないbackend上でsource setを差し替える。
// 途中失敗時は呼出前のsetを再構築し、それも失敗した場合だけbackendをresetする。
template<typename Entry, typename Remove, typename Add, typename Reset>
SourceSetReplaceResult replaceSourceSet(std::vector<Entry>& current,
                                        const std::vector<Entry>& desired, Remove remove, Add add,
                                        Reset reset) {
    const std::vector<Entry> original = current;
    const auto apply = [&](const std::vector<Entry>& target) {
        while (!current.empty()) {
            if (!remove(current.back()))
                return false;
            current.pop_back();
        }
        for (const auto& requested : target) {
            Entry installed = requested;
            if (!add(requested, installed))
                return false;
            current.push_back(std::move(installed));
        }
        return true;
    };

    if (apply(desired))
        return SourceSetReplaceResult::Success;
    if (apply(original))
        return SourceSetReplaceResult::OperationFailedRestored;
    if (reset()) {
        current.clear();
        return SourceSetReplaceResult::OperationFailedReset;
    }
    return SourceSetReplaceResult::OperationFailedResetFailed;
}

} // namespace mvm::app

#endif // MVM_APP_AUDIO_SOURCE_SET_TRANSACTION_H
