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
#include <thread>
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

const char* discardReasonName(DiscardReason value) {
    switch (value) {
    case DiscardReason::None:
        return "NONE";
    case DiscardReason::BackToBackFlipSuperseded:
        return "BACK_TO_BACK_FLIP_SUPERSEDED";
    case DiscardReason::Win32KTokenNotInFrame:
        return "WIN32K_TOKEN_NOT_IN_FRAME";
    case DiscardReason::DependentPresentSuperseded:
        return "DEPENDENT_PRESENT_SUPERSEDED";
    case DiscardReason::DoNotSequence:
        return "DO_NOT_SEQUENCE";
    case DiscardReason::NotVisible:
        return "NOT_VISIBLE";
    case DiscardReason::BlitCancel:
        return "BLIT_CANCEL";
    case DiscardReason::OtherExplicitDiscard:
        return "OTHER_EXPLICIT_DISCARD";
    case DiscardReason::EarlierSwapChainPresentSuperseded:
        return "EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED";
    }
    return "UNKNOWN";
}

const char* presentModeName(PresentMode value) {
    switch (value) {
    case PresentMode::Unknown:
        return "Unknown";
    case PresentMode::Hardware_Legacy_Flip:
        return "Hardware_Legacy_Flip";
    case PresentMode::Hardware_Legacy_Copy_To_Front_Buffer:
        return "Hardware_Legacy_Copy_To_Front_Buffer";
    case PresentMode::Hardware_Independent_Flip:
        return "Hardware_Independent_Flip";
    case PresentMode::Composed_Flip:
        return "Composed_Flip";
    case PresentMode::Composed_Copy_GPU_GDI:
        return "Composed_Copy_GPU_GDI";
    case PresentMode::Composed_Copy_CPU_GDI:
        return "Composed_Copy_CPU_GDI";
    case PresentMode::Hardware_Composed_Independent_Flip:
        return "Hardware_Composed_Independent_Flip";
    }
    return "Unknown";
}

const char* completionClass(const PresentEvent& event) {
    if (event.IsLost)
        return "LOST";
    if (!event.IsCompleted)
        return "INCOMPLETE_UNKNOWN";
    if (event.FinalState == PresentResult::Presented && !event.Displayed.empty())
        return "PRESENTED";
    if (event.FinalState == PresentResult::Discarded)
        return "DISCARDED";
    return "INCOMPLETE_UNKNOWN";
}

bool parseUnsigned(const wchar_t* value, unsigned long& result) {
    if (!value || *value == L'\0')
        return false;
    wchar_t* end = nullptr;
    result = std::wcstoul(value, &end, 10);
    return end && *end == L'\0';
}

bool pathExists(const wchar_t* path) {
    return path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool writeReadyFile(const wchar_t* path) {
    FILE* file = nullptr;
    if (!path || _wfopen_s(&file, path, L"wb") != 0 || !file)
        return false;
    std::fputs("READY\n", file);
    std::fclose(file);
    return true;
}

int writeOutput(const wchar_t* outputPath, unsigned long targetPid, long long qpcFrequency,
                unsigned long etwEventsLost, unsigned long etwBuffersLost,
                const char* acquisitionMode, PMTraceConsumer& consumer,
                std::vector<std::shared_ptr<PresentEvent>>& events) {
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
    std::fprintf(file, "  \"acquisition_mode\": \"%s\",\n", acquisitionMode);
    std::fprintf(file,
                 "  \"provider_configuration\": "
                 "\"PINNED_PRESENTMON_PMTRACESESSION_ENABLEPROVIDERS\",\n");
    std::fprintf(file, "  \"event_id_filtering\": %s,\n",
                 consumer.mFilteredEvents ? "true" : "false");
    std::fprintf(file, "  \"raw_displayed_qpc\": true,\n");
    std::fprintf(file, "  \"discard_reason_diagnostic\": true,\n");
    std::fprintf(file, "  \"qpc_frequency\": %lld,\n", qpcFrequency);
    std::fprintf(file, "  \"target_process_id\": %lu,\n", targetPid);
    std::fprintf(file, "  \"etw_events_lost\": %lu,\n", etwEventsLost);
    std::fprintf(file, "  \"etw_buffers_lost\": %lu,\n", etwBuffersLost);
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
                     "\"discard_reason\": \"%s\", "
                     "\"dependency_batch_present_start_qpc\": %llu, "
                     "\"completion_class\": \"%s\", "
                     "\"is_completed\": %s, "
                     "\"is_lost\": %s, \"present_mode\": \"%s\", "
                     "\"seen_dxgk_present\": %s, \"seen_win32k_events\": %s, "
                     "\"seen_in_frame_event\": %s, \"wait_for_flip_event\": %s, "
                     "\"wait_for_mpo_flip_event\": %s, \"time_in_present_qpc\": %llu, "
                     "\"ready_qpc\": %llu, \"queue_submit_sequence\": %u, "
                     "\"composition_surface_luid\": \"0x%llx\", "
                     "\"win32k_present_count\": %llu, \"win32k_bind_id\": %llu, "
                     "\"dxgk_present_history_token\": \"0x%llx\", "
                     "\"dxgk_present_history_token_data\": \"0x%llx\", \"displayed\": [",
                     index ? ", " : "", static_cast<unsigned long long>(sequence),
                     static_cast<unsigned long long>(event.PresentStartTime), event.ProcessId,
                     event.ThreadId, static_cast<unsigned long long>(event.SwapChainAddress),
                     static_cast<unsigned long long>(event.Hwnd), event.SyncInterval,
                     event.PresentFlags, resultName(event.FinalState),
                     discardReasonName(event.FinalDiscardReason),
                     static_cast<unsigned long long>(event.DependencyBatchPresentStartTime),
                     completionClass(event),
                     event.IsCompleted ? "true" : "false", event.IsLost ? "true" : "false",
                     presentModeName(event.PresentMode), event.SeenDxgkPresent ? "true" : "false",
                     event.SeenWin32KEvents ? "true" : "false",
                     event.SeenInFrameEvent ? "true" : "false",
                     event.WaitForFlipEvent ? "true" : "false",
                     event.WaitForMPOFlipEvent ? "true" : "false",
                     static_cast<unsigned long long>(event.TimeInPresent),
                     static_cast<unsigned long long>(event.ReadyTime), event.QueueSubmitSequence,
                     static_cast<unsigned long long>(event.CompositionSurfaceLuid),
                     static_cast<unsigned long long>(event.Win32KPresentCount),
                     static_cast<unsigned long long>(event.Win32KBindId),
                     static_cast<unsigned long long>(event.DxgkPresentHistoryToken),
                     static_cast<unsigned long long>(event.DxgkPresentHistoryTokenData));
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
    return etwEventsLost == 0 && etwBuffersLost == 0 && consumer.mNumOverflowedPresents == 0 ? 0
                                                                                             : 5;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* etlPath = nullptr;
    const wchar_t* outputPath = nullptr;
    const wchar_t* readyFile = nullptr;
    const wchar_t* stopFile = nullptr;
    bool live = false;
    unsigned long targetPid = 0;
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--etl") == 0 && index + 1 < argc)
            etlPath = argv[++index];
        else if (std::wcscmp(argv[index], L"--live") == 0)
            live = true;
        else if (std::wcscmp(argv[index], L"--ready-file") == 0 && index + 1 < argc)
            readyFile = argv[++index];
        else if (std::wcscmp(argv[index], L"--stop-file") == 0 && index + 1 < argc)
            stopFile = argv[++index];
        else if (std::wcscmp(argv[index], L"--output") == 0 && index + 1 < argc)
            outputPath = argv[++index];
        else if (std::wcscmp(argv[index], L"--process-id") == 0 && index + 1 < argc) {
            if (!parseUnsigned(argv[++index], targetPid))
                targetPid = 0;
        }
    }
    if ((!live && !etlPath) || (live && (!readyFile || !stopFile)) || !outputPath ||
        targetPid == 0) {
        std::fwprintf(stderr, L"使い方: mvm_present_history_decoder --etl <trace.etl> "
                              L"--process-id <pid> --output <json>\n"
                              L"        mvm_present_history_decoder --live --process-id <pid> "
                              L"--ready-file <path> --stop-file <path> --output <json>\n");
        return 2;
    }

    unsigned long etwEventsLost = 0;
    unsigned long etwBuffersLost = 0;
    if (!live) {
        EVENT_TRACE_LOGFILEW headerProbe{};
        headerProbe.LogFileName = const_cast<wchar_t*>(etlPath);
        const TRACEHANDLE headerHandle = OpenTraceW(&headerProbe);
        if (headerHandle == INVALID_PROCESSTRACE_HANDLE) {
            std::fwprintf(stderr, L"ETL headerを開けません: %lu\n", GetLastError());
            return 3;
        }
        etwEventsLost = headerProbe.LogfileHeader.EventsLost;
        CloseTrace(headerHandle);
    }

    PMTraceConsumer consumer(262144);
    consumer.mTrackDisplay = true;
    consumer.mTrackGPU = false;
    consumer.mTrackInput = false;
    consumer.mFilteredProcessIds = true;
    consumer.mDisableOfflineBackpressure = true;
    consumer.AddTrackedProcessForFiltering(static_cast<std::uint32_t>(targetPid));

    PMTraceSession session;
    session.mPMConsumer = &consumer;
    const ULONG startStatus = session.Start(live ? nullptr : etlPath,
                                            live ? L"MvmP2TargetedPresentHistory"
                                                 : L"MvmP2EtwPresentHistory");
    if (startStatus != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"PresentDataでETLを開けません: %lu\n", startStatus);
        return 3;
    }
    consumer.mDeferralTimeLimit = session.mTimestampFrequency.QuadPart * 2;
    TRACEHANDLE traceHandle = session.mTraceHandle;
    ULONG processStatus = ERROR_SUCCESS;
    std::thread consumerThread;
    if (live) {
        consumerThread = std::thread([&] {
            processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        });
        if (!writeReadyFile(readyFile)) {
            session.Stop();
            consumerThread.join();
            std::fwprintf(stderr, L"live ETW ready fileを作成できません\n");
            return 4;
        }
        while (!pathExists(stopFile))
            Sleep(10);
        session.Stop();
        consumerThread.join();
        etwEventsLost = session.mNumEventsLost;
        etwBuffersLost = session.mNumBuffersLost;
    } else {
        processStatus = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    }
    std::vector<std::shared_ptr<PresentEvent>> events;
    consumer.DequeuePresentEvents(events);
    if (!live)
        session.Stop();
    if (processStatus != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"ETL event処理に失敗しました: %lu\n", processStatus);
        return 3;
    }
    return writeOutput(outputPath, targetPid,
                       static_cast<long long>(session.mTimestampFrequency.QuadPart), etwEventsLost,
                       etwBuffersLost, live ? "CANONICAL_PRESENTMON_LIVE" : "WPR_ETL",
                       consumer, events);
}
