#ifndef MVM_TEST_EVENT_WAIT_H
#define MVM_TEST_EVENT_WAIT_H

#include <chrono>
#include <functional>
#include <thread>

#include <QCoreApplication>

namespace mvm::test {

// 条件の成立を観測したら、その結果を返す。成立後にイベントを処理して再判定すると、
// 初回 seek などで一時的に戻る ready を、待ち時間内に成立しなかったものとして扱ってしまう。
inline bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace mvm::test

#endif
