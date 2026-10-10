#pragma once

// 準備の健全性と再生の時間契約を分離する。記録は worker 上では出力せず、メモリへ蓄積する。
#include "math_raster_cache.h"

#include <windows.h>
#include <condition_variable>
#include <fstream>

#include <QEventLoop>
#include <QTimer>

namespace mvm::test {
struct PreparationTrace {
    using Clock = std::chrono::steady_clock;

    struct Record {
        app::MathRasterCache::TransformPreparationEvent event;
        std::int64_t wallUs;
        std::uint64_t cpu100ns;
        DWORD thread;
        bool cpuAvailable;
    };

    Clock::time_point start = Clock::now();
    Clock::time_point last = start;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Record> records;
    bool release = false;
    bool workerReady = false;
    bool workerError = false;
    bool cancelled = false;
    bool held = false;
    Clock::time_point completed{};

    PreparationTrace() { records.reserve(4096); }

    void record(const app::MathRasterCache::TransformPreparationEvent& event) {
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool cpuAvailable =
            GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != FALSE;
        const auto value = [](FILETIME t) {
            return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        const auto now = Clock::now();
        std::lock_guard lock(mutex);
        last = now;
        records.push_back(
            {event, std::chrono::duration_cast<std::chrono::microseconds>(now - start).count(),
             value(kernel) + value(user), GetCurrentThreadId(), cpuAvailable});
        if (event.stage == "worker-ready") {
            workerReady = true;
            completed = now;
        }
        if (event.stage == "worker-error")
            workerError = true;
        if (event.stage == "worker-cancelled")
            cancelled = true;
        changed.notify_all();
    }

    void unblock() {
        std::lock_guard lock(mutex);
        release = true;
        changed.notify_all();
    }

    void dump() {
        std::lock_guard lock(mutex);
        for (const auto& r : records)
            std::fprintf(stderr,
                         "準備記録: %s wall_us=%lld cpu_100ns=%llu thread=%lu frame=%lld "
                         "bytes=%llu success=%d cpu_available=%d\n",
                         r.event.stage.c_str(), static_cast<long long>(r.wallUs),
                         static_cast<unsigned long long>(r.cpu100ns),
                         static_cast<unsigned long>(r.thread),
                         static_cast<long long>(r.event.frame),
                         static_cast<unsigned long long>(r.event.bytes), int(r.event.success),
                         int(r.cpuAvailable));
    }
};

enum class PreparationResult { Ready, Error, Cancelled, Stalled, NotificationLost, SafetyCap };

template<class Ready>
PreparationResult
waitForPreparation(PreparationTrace& trace, Ready ready, bool shortControl = false,
                   std::chrono::milliseconds overallCap = std::chrono::seconds(60)) {
    // 15 秒無進捗 / 全体 60 秒は試験の安全境界。製品の速度要件ではない。
    // 故障注入中だけ停止の観測時間を短縮する。通常の準備には適用しない。
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(10);
    PreparationResult result = PreparationResult::SafetyCap;
    const auto begin = PreparationTrace::Clock::now();
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        const bool guiReady = ready();
        const auto now = PreparationTrace::Clock::now();
        std::lock_guard lock(trace.mutex);
        if (trace.workerError)
            result = PreparationResult::Error;
        else if (trace.cancelled)
            result = PreparationResult::Cancelled;
        else if (guiReady && trace.workerReady)
            result = PreparationResult::Ready;
        else if (trace.workerReady && now - trace.completed > std::chrono::seconds(2))
            result = PreparationResult::NotificationLost;
        else if (now - begin > overallCap)
            result = PreparationResult::SafetyCap;
        else if (now - trace.last > (shortControl && trace.held ? std::chrono::milliseconds(500)
                                                                : std::chrono::milliseconds(15000)))
            result = PreparationResult::Stalled;
        else
            return;
        loop.quit();
    });
    timer.start();
    loop.exec();
    return result;
}

inline app::MathRasterCache::TransformPreparationObserver
preparationObserver(std::shared_ptr<PreparationTrace> trace, std::string control) {
    return [trace, control](const auto& event) {
        trace->record(event);
        if ((control == "backend-stall" && event.stage == "backend-start") ||
            (control == "publish-stall" && event.stage == "provenance-start")) {
            std::unique_lock lock(trace->mutex);
            trace->held = true;
            trace->changed.wait(lock, [&] { return trace->release; });
        }
        if (control == "delayed" && event.stage == "provenance-start") {
            std::unique_lock lock(trace->mutex);
            trace->changed.wait_until(
                lock, PreparationTrace::Clock::now() + std::chrono::milliseconds(10500),
                [&] { return trace->release; });
        }
        if (event.frame == 1 && event.stage == "decode-start") {
            if (control == "missing")
                std::filesystem::remove(event.path);
            if (control == "decode-fail" || control == "corrupt") {
                std::ofstream file(event.path, std::ios::binary | std::ios::trunc);
                file << "破損 PNG";
            }
        }
        if ((control == "persist-fail" && event.stage == "persist-start" && event.frame == 1) ||
            (control == "publish-fail" && event.stage == "provenance-start"))
            std::filesystem::create_directory(event.path);
    };
}
} // namespace mvm::test
