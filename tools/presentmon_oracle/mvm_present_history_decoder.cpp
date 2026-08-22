// clang-format off: evntrace.hはwindows.hの型定義を前提とする。
#include <windows.h>
#include <evntrace.h>
// clang-format on

// PresentMon PresentDataを使い、ETLのPresentEventをraw QPCのままJSONへ出す。
#include "PresentData/PresentMonTraceConsumer.hpp"
#include "PresentData/PresentMonTraceSession.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kPresentMonCommit[] = L"717c5bf14e80a4a06b70cd16415ae8d40a7ce201";

const char* frameTypeName(FrameType value) {
    switch (value) {
    case FrameType::NotSet:
        return "NotSet";
    case FrameType::Unspecified:
        return "Unspecified";
    case FrameType::Application:
        return "Application";
    case FrameType::Repeated:
        return "Repeated";
    case FrameType::Intel_XEFG:
        return "Intel_XEFG";
    case FrameType::AMD_AFMF:
        return "AMD_AFMF";
    }
    return "Unknown";
}

const char* resultName(PresentResult value) {
    switch (value) {
    case PresentResult::Unknown:
        return "Unknown";
    case PresentResult::Presented:
        return "Presented";
    case PresentResult::Discarded:
        return "Discarded";
    }
    return "Unknown";
}

bool parseUnsigned(const wchar_t* value, unsigned long& result) {
    if (!value || *value == L'\0')
        return false;
    wchar_t* end = nullptr;
    result = std::wcstoul(value, &end, 10);
    return end && *end == L'\0';
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* etlPath = nullptr;
    const wchar_t* outputPath = nullptr;
    unsigned long targetPid = 0;
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--etl") == 0 && index + 1 < argc)
            etlPath = argv[++index];
        else if (std::wcscmp(argv[index], L"--output") == 0 && index + 1 < argc)
            outputPath = argv[++index];
        else if (std::wcscmp(argv[index], L"--process-id") == 0 && index + 1 < argc) {
            if (!parseUnsigned(argv[++index], targetPid))
                targetPid = 0;
        }
    }
    if (!etlPath || !outputPath || targetPid == 0) {
        std::fwprintf(stderr, L"使い方: mvm_present_history_decoder --etl <trace.etl> "
                              L"--process-id <pid> --output <json>\n");
        return 2;
    }

    EVENT_TRACE_LOGFILEW headerProbe{};
    headerProbe.LogFileName = const_cast<wchar_t*>(etlPath);
    const TRACEHANDLE headerHandle = OpenTraceW(&headerProbe);
    if (headerHandle == INVALID_PROCESSTRACE_HANDLE) {
        std::fwprintf(stderr, L"ETL headerを開けません: %lu\n", GetLastError());
        return 3;
    }
    const unsigned long etwEventsLost = headerProbe.LogfileHeader.EventsLost;
    CloseTrace(headerHandle);

    PMTraceConsumer consumer(262144);
    consumer.mTrackDisplay = true;
    consumer.mTrackGPU = false;
    consumer.mTrackInput = false;
    consumer.mFilteredProcessIds = true;
    consumer.mDisableOfflineBackpressure = true;
    consumer.AddTrackedProcessForFiltering(static_cast<std::uint32_t>(targetPid));

    PMTraceSession session;
    session.mPMConsumer = &consumer;
    const ULONG startStatus = session.Start(etlPath, L"MvmP2EtwPresentHistory");
    if (startStatus != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"PresentDataでETLを開けません: %lu\n", startStatus);
        return 3;
    }
    consumer.mDeferralTimeLimit = session.mTimestampFrequency.QuadPart * 2;
    TRACEHANDLE traceHandle = session.mTraceHandle;
    const ULONG processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    std::vector<std::shared_ptr<PresentEvent>> events;
    consumer.DequeuePresentEvents(events);
    session.Stop();
    if (processStatus != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"ETL event処理に失敗しました: %lu\n", processStatus);
        return 3;
    }
    std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        return left->PresentStartTime < right->PresentStartTime;
    });

    FILE* file = nullptr;
    if (_wfopen_s(&file, outputPath, L"wb") != 0 || !file) {
        std::fwprintf(stderr, L"出力JSONを作成できません\n");
        return 4;
    }
    std::fprintf(file, "{\n  \"schema\": \"mvm-p2-etw-present-history-1\",\n");
    std::fprintf(file, "  \"presentmon_commit\": \"%ls\",\n", kPresentMonCommit);
    std::fprintf(file, "  \"raw_displayed_qpc\": true,\n");
    std::fprintf(file, "  \"qpc_frequency\": %lld,\n",
                 static_cast<long long>(session.mTimestampFrequency.QuadPart));
    std::fprintf(file, "  \"target_process_id\": %lu,\n", targetPid);
    std::fprintf(file, "  \"etw_events_lost\": %lu,\n", etwEventsLost);
    std::fprintf(file, "  \"etw_buffers_lost\": %lu,\n", etwEventsLost == 0 ? 0UL : 1UL);
    std::fprintf(file, "  \"present_event_overflow_count\": %u,\n",
                 consumer.mNumOverflowedPresents);
    std::fprintf(file, "  \"collision_evidence_mode\": \"ACTUAL_QT\",\n");
    std::fprintf(file, "  \"cadence_diagnostic\": {\"traced_swaps_per_second\": null, "
                       "\"baseline_swaps_per_second\": null, \"ratio\": null, "
                       "\"extreme_change\": null},\n");
    std::fprintf(file, "  \"events\": [");
    std::unordered_map<std::uint64_t, std::uint64_t> sequenceBySwapChain;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = *events[index];
        const std::uint64_t sequence = sequenceBySwapChain[event.SwapChainAddress]++;
        std::fprintf(file,
                     "%s{\"sequence_index\": %llu, \"present_start_qpc\": %llu, "
                     "\"process_id\": %u, \"thread_id\": %u, "
                     "\"swap_chain_address\": \"0x%llx\", "
                     "\"window_handle\": \"0x%llx\", \"sync_interval\": %d, "
                     "\"present_flags\": %u, \"final_state\": \"%s\", "
                     "\"displayed\": [",
                     index ? ", " : "", static_cast<unsigned long long>(sequence),
                     static_cast<unsigned long long>(event.PresentStartTime), event.ProcessId,
                     event.ThreadId, static_cast<unsigned long long>(event.SwapChainAddress),
                     static_cast<unsigned long long>(event.Hwnd), event.SyncInterval,
                     event.PresentFlags, resultName(event.FinalState));
        for (std::size_t displayIndex = 0; displayIndex < event.Displayed.size(); ++displayIndex) {
            const auto& display = event.Displayed[displayIndex];
            std::fprintf(file, "%s{\"frame_type\": \"%s\", \"qpc\": %llu}",
                         displayIndex ? ", " : "", frameTypeName(display.first),
                         static_cast<unsigned long long>(display.second));
        }
        std::fprintf(file, "], \"present_ids\": [");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> presentIds(event.PresentIds.begin(),
                                                                        event.PresentIds.end());
        std::sort(presentIds.begin(), presentIds.end());
        for (std::size_t idIndex = 0; idIndex < presentIds.size(); ++idIndex) {
            std::fprintf(file, "%s{\"vidpn_layer_id\": \"0x%llx\", \"present_id\": %llu}",
                         idIndex ? ", " : "",
                         static_cast<unsigned long long>(presentIds[idIndex].first),
                         static_cast<unsigned long long>(presentIds[idIndex].second));
        }
        std::fprintf(file, "]}");
    }
    std::fprintf(file, "]\n}\n");
    std::fclose(file);
    return etwEventsLost == 0 && consumer.mNumOverflowedPresents == 0 ? 0 : 5;
}
