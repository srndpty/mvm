// P2-D5-2/F3-B0.6-R1: Complete Present-ID Oracle。
// mapperを評価する前に、成功した全PresentのIDとDXGI frame statisticsを
// PresentCountで完全にjoinできるrunだけをoracle authorityとする。
#include "media/gpu_preview/qpc_clock.h"
#include "media/gpu_preview/window_output_vblank_observer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

struct StatisticsTransition {
    long long pollQpc = 0;
    long long presentCount = 0;
    long long presentRefreshCount = 0;
    long long syncRefreshCount = 0;
    long long syncQpcTime = 0;
};

struct PresentSubmission {
    long long submissionIndex = -1;
    unsigned int presentId = 0;
    long long renderEndQpc = 0;
    long long presentReturnQpc = 0;
};

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    for (const char c : value) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

void updateMaximum(std::atomic<long long>& destination, long long value) {
    long long current = destination.load(std::memory_order_relaxed);
    while (current < value &&
           !destination.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

std::vector<long long> parseDelayPattern(const std::string& pattern) {
    std::vector<long long> values;
    std::size_t begin = 0;
    while (begin <= pattern.size() && !pattern.empty()) {
        const std::size_t comma = pattern.find(',', begin);
        const std::string token =
            pattern.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        if (!token.empty())
            values.push_back(std::atoll(token.c_str()));
        if (comma == std::string::npos)
            break;
        begin = comma + 1;
    }
    return values;
}

} // namespace

int main(int argc, char** argv) {
    std::string metricsPath;
    int presentCount = 900;
    std::string notifyDelayPattern = "0,100,300,800,1200";
    bool dwmFlushFallback = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--dwm-flush-fallback") == 0) {
            dwmFlushFallback = true;
        } else if (index + 1 >= argc) {
            continue;
        } else if (std::strcmp(argv[index], "--metrics") == 0)
            metricsPath = argv[index + 1];
        else if (std::strcmp(argv[index], "--presents") == 0)
            presentCount = std::atoi(argv[index + 1]);
        else if (std::strcmp(argv[index], "--notify-delay-pattern") == 0)
            notifyDelayPattern = argv[index + 1];
    }
    const auto notifyDelayMilliPeriods = parseDelayPattern(notifyDelayPattern);
    if (metricsPath.empty() || presentCount <= 0 || notifyDelayMilliPeriods.empty() ||
        std::any_of(notifyDelayMilliPeriods.begin(), notifyDelayMilliPeriods.end(),
                    [](long long value) { return value < 0; })) {
        std::fprintf(stderr, "使い方: mvm_p2_present_identity_probe --metrics <json> "
                             "[--presents <n>] [--notify-delay-pattern <milli-period,...>]\n");
        return 2;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MvmPresentIdentityProbe";
    if (!RegisterClassExW(&windowClass))
        return 5;
    const HWND hwnd = CreateWindowExW(
        0, windowClass.lpszClassName, L"mvm P2 complete Present-ID oracle", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!hwnd)
        return 5;
    ShowWindow(hwnd, SW_SHOW);
    pumpMessages();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context)))
        return 5;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return 5;

    // 現在のQt条件を変えない。secondary controlへの切替はこのprobeでは行わない。
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr,
                                               &swapChain)))
        return 5;

    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv)))
        return 5;

    const auto resolved = mvm::gpu::resolveWindowOutput(hwnd);
    mvm::gpu::WindowOutputVBlankObserver observer;
    std::string observerError;
    if (!resolved.ok || !observer.start(hwnd, observerError)) {
        std::fprintf(stderr, "VBlank observerを開始できません: %s\n",
                     resolved.ok ? observerError.c_str() : resolved.error.c_str());
        return 3;
    }

    ComPtr<IDXGIOutput> statisticsOutput;
    if (FAILED(swapChain->GetContainingOutput(&statisticsOutput))) {
        std::fprintf(stderr, "statistics sampler用のDXGI outputを取得できません\n");
        return 3;
    }
    DXGI_OUTPUT_DESC statisticsOutputDesc{};
    if (FAILED(statisticsOutput->GetDesc(&statisticsOutputDesc)))
        return 3;
    const bool statisticsOutputMatchesWindow =
        reinterpret_cast<std::uint64_t>(statisticsOutputDesc.Monitor) ==
        resolved.identity.monitorHandle;

    const long long qpcFrequency = static_cast<long long>(mvm::gpu::qpcFrequency());
    const long long nominalPeriodQpc =
        (qpcFrequency * resolved.identity.refreshDenominator) / resolved.identity.refreshNumerator;
    const long long boundedPollQpc = (nominalPeriodQpc * 3) / 4;

    std::vector<StatisticsTransition> transitions;
    transitions.reserve(static_cast<std::size_t>(presentCount) + 16);
    std::atomic<bool> statisticsStop{false};
    std::atomic<bool> samplerEnabled{false};
    std::atomic<bool> samplerBaselineReady{false};
    std::atomic<int> samplerPriorityState{0};
    std::atomic<long long> samplerVBlankGaps{0};
    std::atomic<long long> statisticsFailures{0};
    std::atomic<long long> statisticsDisjoint{0};
    std::atomic<long long> baselineDisjoint{0};
    std::atomic<long long> maxPollIntervalQpc{0};
    std::atomic<long long> lastObservedPresentId{-1};
    std::thread statisticsThread([&] {
        const bool priorityOk = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
        samplerPriorityState.store(priorityOk ? 1 : 2, std::memory_order_release);
        while (!samplerEnabled.load(std::memory_order_acquire) &&
               !statisticsStop.load(std::memory_order_acquire))
            Sleep(0);
        long long previousPollQpc = 0;
        long long previousPresentCount = -1;
        std::size_t observedVBlankCount = observer.ring().publishedCount();
        while (!statisticsStop.load(std::memory_order_acquire)) {
            const std::size_t currentVBlankCount = observer.ring().publishedCount();
            if (currentVBlankCount == observedVBlankCount) {
                Sleep(0);
                continue;
            }
            if (currentVBlankCount != observedVBlankCount + 1)
                samplerVBlankGaps.fetch_add(
                    static_cast<long long>(currentVBlankCount - observedVBlankCount - 1),
                    std::memory_order_relaxed);
            observedVBlankCount = currentVBlankCount;
            if (dwmFlushFallback)
                DwmFlush();
            const long long pollDeadline = mvm::gpu::qpcTicks() + boundedPollQpc;
            while (!statisticsStop.load(std::memory_order_acquire) &&
                   mvm::gpu::qpcTicks() < pollDeadline &&
                   observer.ring().publishedCount() == observedVBlankCount) {
                const long long pollQpc = mvm::gpu::qpcTicks();
                if (previousPollQpc != 0)
                    updateMaximum(maxPollIntervalQpc, pollQpc - previousPollQpc);
                previousPollQpc = pollQpc;
                DXGI_FRAME_STATISTICS statistics{};
                const HRESULT hr = swapChain->GetFrameStatistics(&statistics);
                if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
                    if (samplerBaselineReady.load(std::memory_order_acquire))
                        statisticsDisjoint.fetch_add(1, std::memory_order_relaxed);
                    else
                        baselineDisjoint.fetch_add(1, std::memory_order_relaxed);
                } else if (FAILED(hr)) {
                    statisticsFailures.fetch_add(1, std::memory_order_relaxed);
                } else {
                    samplerBaselineReady.store(true, std::memory_order_release);
                    if (static_cast<long long>(statistics.PresentCount) != previousPresentCount) {
                        previousPresentCount = statistics.PresentCount;
                        transitions.push_back(
                            {pollQpc, statistics.PresentCount, statistics.PresentRefreshCount,
                             statistics.SyncRefreshCount, statistics.SyncQPCTime.QuadPart});
                        lastObservedPresentId.store(statistics.PresentCount,
                                                    std::memory_order_release);
                    }
                }
                Sleep(0);
            }
        }
    });

    for (int attempt = 0;
         attempt < 500 && samplerPriorityState.load(std::memory_order_acquire) == 0; ++attempt)
        Sleep(1);
    samplerEnabled.store(true, std::memory_order_release);
    for (int attempt = 0; attempt < 500 && !samplerBaselineReady.load(std::memory_order_acquire);
         ++attempt)
        Sleep(1);

    std::vector<PresentSubmission> submissions;
    submissions.reserve(static_cast<std::size_t>(presentCount));
    long long presentFailureCount = 0;
    long long getLastPresentCountFailureCount = 0;
    bool submittedIdsConsecutive = true;
    for (int submissionIndex = 0; submissionIndex < presentCount; ++submissionIndex) {
        pumpMessages();
        const float color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(rtv.Get(), color);
        const long long renderEndQpc = mvm::gpu::qpcTicks();
        const HRESULT presentHr = swapChain->Present(1, 0);
        const long long presentReturnQpc = mvm::gpu::qpcTicks();
        if (FAILED(presentHr)) {
            ++presentFailureCount;
            break;
        }
        UINT presentId = 0;
        if (FAILED(swapChain->GetLastPresentCount(&presentId))) {
            ++getLastPresentCountFailureCount;
            break;
        }
        if (!submissions.empty() && presentId != submissions.back().presentId + 1U)
            submittedIdsConsecutive = false;
        submissions.push_back({submissionIndex, presentId, renderEndQpc, presentReturnQpc});
    }

    // DwmFlushは使わない。最後のsubmitted Present IDが観測されるまでbounded drainする。
    const long long drainDeadline = mvm::gpu::qpcTicks() + qpcFrequency * 5;
    const long long finalSubmittedId =
        submissions.empty() ? -1 : static_cast<long long>(submissions.back().presentId);
    while (mvm::gpu::qpcTicks() < drainDeadline &&
           lastObservedPresentId.load(std::memory_order_acquire) < finalSubmittedId) {
        pumpMessages();
        Sleep(1);
    }
    statisticsStop.store(true, std::memory_order_release);
    if (statisticsThread.joinable())
        statisticsThread.join();
    observer.stop();

    const auto endIdentity = mvm::gpu::resolveWindowOutput(hwnd);
    const auto vblanks = observer.ring().snapshot();
    std::vector<const StatisticsTransition*> joinedTransitions;
    joinedTransitions.reserve(submissions.size());
    if (!submissions.empty()) {
        const long long firstSubmittedId = submissions.front().presentId;
        for (const auto& transition : transitions) {
            if (transition.presentCount >= firstSubmittedId &&
                transition.presentCount <= finalSubmittedId)
                joinedTransitions.push_back(&transition);
        }
    }
    bool observedIdsComplete = joinedTransitions.size() == submissions.size();
    for (std::size_t index = 0; observedIdsComplete && index < submissions.size(); ++index) {
        observedIdsComplete = joinedTransitions[index]->presentCount ==
                              static_cast<long long>(submissions[index].presentId);
    }
    const bool configuredSubmissionsComplete =
        submissions.size() == static_cast<std::size_t>(presentCount);
    const bool finalDrainComplete =
        finalSubmittedId >= 0 &&
        lastObservedPresentId.load(std::memory_order_acquire) == finalSubmittedId;
    // 各VBlank後の3/4周期をpoll窓にするため、0.5周期以上の空白はsampler gap。
    const bool pollIntervalValid =
        maxPollIntervalQpc.load(std::memory_order_acquire) * 2 < nominalPeriodQpc;
    const bool outputStable =
        endIdentity.ok && mvm::gpu::sameWindowOutput(resolved.identity, endIdentity.identity);
    const bool oracleValid =
        configuredSubmissionsComplete && presentFailureCount == 0 &&
        getLastPresentCountFailureCount == 0 && submittedIdsConsecutive && observedIdsComplete &&
        joinedTransitions.size() == submissions.size() && finalDrainComplete &&
        samplerPriorityState.load(std::memory_order_acquire) == 1 &&
        samplerBaselineReady.load(std::memory_order_acquire) &&
        samplerVBlankGaps.load(std::memory_order_acquire) == 0 &&
        statisticsFailures.load(std::memory_order_acquire) == 0 &&
        statisticsDisjoint.load(std::memory_order_acquire) == 0 && pollIntervalValid &&
        observer.waitFailureCount() == 0 && observer.ring().overflowCount() == 0 && outputStable;
    const bool completeOracleValid = oracleValid && statisticsOutputMatchesWindow;
    const bool oracleSamplingGap = !observedIdsComplete || !finalDrainComplete ||
                                   !pollIntervalValid ||
                                   samplerVBlankGaps.load(std::memory_order_acquire) != 0;

    FILE* file = nullptr;
    if (fopen_s(&file, metricsPath.c_str(), "wb") != 0 || !file)
        return 6;
    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"schema\": \"mvm-p2-present-id-oracle-2\",\n");
    std::fprintf(file, "  \"oracle_status\": \"%s\",\n", completeOracleValid ? "VALID" : "INVALID");
    std::fprintf(file, "  \"oracle_valid\": %s,\n", completeOracleValid ? "true" : "false");
    std::fprintf(file, "  \"mapper_proof_status\": \"NOT_YET_EVALUABLE\",\n");
    std::fprintf(file, "  \"flip_discard_frame_discard_claim\": \"NOT_ESTABLISHED\",\n");
    std::fprintf(file, "  \"sampling_gap_code\": \"%s\",\n",
                 oracleSamplingGap ? "ORACLE_SAMPLING_GAP" : "NONE");
    std::fprintf(file, "  \"qpc_frequency\": %lld,\n", qpcFrequency);
    std::fprintf(file, "  \"nominal_period_qpc\": %lld,\n", nominalPeriodQpc);
    std::fprintf(file, "  \"configured_present_count\": %d,\n", presentCount);
    std::fprintf(file, "  \"swap_effect\": \"FLIP_DISCARD\",\n");
    std::fprintf(file, "  \"buffer_count\": 3,\n");
    std::fprintf(file, "  \"sync_interval\": 1,\n");
    std::fprintf(file, "  \"dwm_flush_used\": %s,\n", dwmFlushFallback ? "true" : "false");
    std::fprintf(file, "  \"dwm_flush_mode\": \"%s\",\n",
                 dwmFlushFallback ? "ORACLE_ONLY_FALLBACK" : "DISABLED");
    std::fprintf(file, "  \"configured_submissions_complete\": %s,\n",
                 configuredSubmissionsComplete ? "true" : "false");
    std::fprintf(file, "  \"submitted_ids_consecutive\": %s,\n",
                 submittedIdsConsecutive ? "true" : "false");
    std::fprintf(file, "  \"observed_ids_complete\": %s,\n",
                 observedIdsComplete ? "true" : "false");
    std::fprintf(file, "  \"final_drain_complete\": %s,\n", finalDrainComplete ? "true" : "false");
    std::fprintf(file, "  \"final_submitted_present_id\": %lld,\n", finalSubmittedId);
    std::fprintf(file, "  \"final_observed_present_count\": %lld,\n",
                 lastObservedPresentId.load(std::memory_order_acquire));
    std::fprintf(file, "  \"present_failure_count\": %lld,\n", presentFailureCount);
    std::fprintf(file, "  \"get_last_present_count_failure_count\": %lld,\n",
                 getLastPresentCountFailureCount);
    std::fprintf(file, "  \"sampler_high_priority\": %s,\n",
                 samplerPriorityState.load(std::memory_order_acquire) == 1 ? "true" : "false");
    std::fprintf(file, "  \"sampler_baseline_ready\": %s,\n",
                 samplerBaselineReady.load(std::memory_order_acquire) ? "true" : "false");
    std::fprintf(file, "  \"sampler_vblank_gap_count\": %lld,\n",
                 samplerVBlankGaps.load(std::memory_order_acquire));
    std::fprintf(file, "  \"statistics_failure_count\": %lld,\n",
                 statisticsFailures.load(std::memory_order_acquire));
    std::fprintf(file, "  \"statistics_disjoint_count\": %lld,\n",
                 statisticsDisjoint.load(std::memory_order_acquire));
    std::fprintf(file, "  \"baseline_disjoint_count\": %lld,\n",
                 baselineDisjoint.load(std::memory_order_acquire));
    std::fprintf(file, "  \"max_poll_interval_qpc\": %lld,\n",
                 maxPollIntervalQpc.load(std::memory_order_acquire));
    std::fprintf(file, "  \"poll_interval_valid\": %s,\n", pollIntervalValid ? "true" : "false");
    std::fprintf(file, "  \"window_output_stable\": %s,\n", outputStable ? "true" : "false");
    std::fprintf(file, "  \"statistics_output_matches_window\": %s,\n",
                 statisticsOutputMatchesWindow ? "true" : "false");
    std::fprintf(file, "  \"refresh_numerator\": %lld,\n", resolved.identity.refreshNumerator);
    std::fprintf(file, "  \"refresh_denominator\": %lld,\n", resolved.identity.refreshDenominator);
    std::fprintf(file, "  \"gdi_device_name\": \"%s\",\n",
                 jsonEscape(resolved.identity.gdiDeviceName).c_str());
    std::fprintf(file, "  \"vblank_ring_overflow_count\": %lld,\n",
                 observer.ring().overflowCount());
    std::fprintf(file, "  \"vblank_wait_failure_count\": %lld,\n", observer.waitFailureCount());
    std::fprintf(file, "  \"vblank_samples\": [");
    for (std::size_t index = 0; index < vblanks.size(); ++index)
        std::fprintf(file, "%s{\"ordinal\": %lld, \"qpc\": %lld}", index ? ", " : "",
                     vblanks[index].ordinal, vblanks[index].qpc);
    std::fprintf(file, "],\n");
    std::fprintf(file, "  \"present_submissions\": [");
    for (std::size_t index = 0; index < submissions.size(); ++index) {
        const auto& sample = submissions[index];
        std::fprintf(file,
                     "%s{\"submission_index\": %lld, \"present_id\": %u, "
                     "\"render_end_qpc\": %lld, \"present_return_qpc\": %lld}",
                     index ? ", " : "", sample.submissionIndex, sample.presentId,
                     sample.renderEndQpc, sample.presentReturnQpc);
    }
    std::fprintf(file, "],\n");
    std::fprintf(file, "  \"statistics_transitions\": [");
    for (std::size_t index = 0; index < transitions.size(); ++index) {
        const auto& transition = transitions[index];
        std::fprintf(file,
                     "%s{\"poll_qpc\": %lld, \"present_count\": %lld, "
                     "\"present_refresh_count\": %lld, \"sync_refresh_count\": %lld, "
                     "\"sync_qpc_time\": %lld}",
                     index ? ", " : "", transition.pollQpc, transition.presentCount,
                     transition.presentRefreshCount, transition.syncRefreshCount,
                     transition.syncQpcTime);
    }
    std::fprintf(file, "],\n");
    std::fprintf(file, "  \"oracle_records\": [");
    for (std::size_t index = 0; index < joinedTransitions.size(); ++index) {
        const auto& submission = submissions[index];
        const auto& transition = *joinedTransitions[index];
        std::fprintf(file,
                     "%s{\"submission_index\": %lld, \"present_id\": %u, "
                     "\"present_refresh_count\": %lld}",
                     index ? ", " : "", submission.submissionIndex, submission.presentId,
                     transition.presentRefreshCount);
    }
    std::fprintf(file, "],\n");
    std::fprintf(file, "  \"offline_notification_scenarios\": [");
    const std::size_t offlineScenarioCount =
        completeOracleValid ? notifyDelayMilliPeriods.size() : 0;
    for (std::size_t delayIndex = 0; delayIndex < offlineScenarioCount; ++delayIndex) {
        const long long milliPeriods = notifyDelayMilliPeriods[delayIndex];
        const long long delayQpc = (milliPeriods * nominalPeriodQpc) / 1000;
        std::fprintf(file,
                     "%s{\"delay_milli_periods\": %lld, \"delay_qpc\": %lld, "
                     "\"records\": [",
                     delayIndex ? ", " : "", milliPeriods, delayQpc);
        for (std::size_t index = 0; index < submissions.size(); ++index) {
            const auto& submission = submissions[index];
            std::fprintf(file,
                         "%s{\"submission_index\": %lld, \"present_id\": %u, "
                         "\"notify_qpc\": %lld}",
                         index ? ", " : "", submission.submissionIndex, submission.presentId,
                         submission.presentReturnQpc + delayQpc);
        }
        std::fprintf(file, "]}");
    }
    std::fprintf(file, "]\n}\n");
    std::fclose(file);
    DestroyWindow(hwnd);
    return completeOracleValid ? 0 : 4;
}
