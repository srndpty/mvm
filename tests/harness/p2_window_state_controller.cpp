// F3-C3-A3-T1: target workloadを変更せず、外部window状態だけを固定・記録する。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <dwmapi.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

enum class Mode { Visible, Occluded, ForceDirty, TargetInvalidate, TargetRedrawNow };

struct RectValue {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

struct Sample {
    std::int64_t qpc = 0;
    std::uintptr_t targetHwnd = 0;
    bool visible = false;
    bool iconic = false;
    bool topmost = false;
    unsigned long cloaked = 0;
    RectValue windowRect;
    RectValue clientRect;
    std::uintptr_t monitor = 0;
    std::uintptr_t foregroundHwnd = 0;
    std::uintptr_t occluderHwnd = 0;
    RectValue occluderRect;
    std::int64_t clientArea = 0;
    std::int64_t designatedIntersectionArea = 0;
    std::int64_t unexpectedIntersectionArea = 0;
    std::int64_t largestUnexpectedIntersectionArea = 0;
    std::uintptr_t largestUnexpectedHwnd = 0;
    unsigned long largestUnexpectedProcessId = 0;
    RectValue largestUnexpectedRect;
    std::uint64_t dirtyTickCount = 0;
    // F3-C3-A3-T2-C: target HWNDへdiagnostic-onlyのdamageを注入した回数と、
    // 直後にupdate regionが観測できたかどうか。mvm render pathには何も足さない。
    std::uint64_t targetDamageCount = 0;
    bool targetUpdateRegionPresent = false;
    // AGENTS.md interactive measurement protocol: measurement中のユーザー入力を
    // 記録し、checker側でPROTOCOL_INVALIDへfail-closeさせる。
    unsigned long lastInputTick = 0;
};

struct OcclusionInfo {
    std::int64_t totalArea = 0;
    std::int64_t largestArea = 0;
    HWND largestHwnd = nullptr;
    unsigned long largestProcessId = 0;
    RECT largestRect{};
};

struct FindWindowContext {
    unsigned long processId = 0;
    HWND best = nullptr;
    std::int64_t bestArea = 0;
};

std::int64_t area(RECT const& rect) {
    auto const width = std::max<long>(0, rect.right - rect.left);
    auto const height = std::max<long>(0, rect.bottom - rect.top);
    return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
}

std::int64_t intersectionArea(RECT const& left, RECT const& right) {
    RECT intersection{};
    return IntersectRect(&intersection, &left, &right) ? area(intersection) : 0;
}

RectValue valueOf(RECT const& rect) {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

BOOL CALLBACK findWindowCallback(HWND hwnd, LPARAM parameter) {
    auto* context = reinterpret_cast<FindWindowContext*>(parameter);
    unsigned long processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != context->processId || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return TRUE;
    auto const candidateArea = area(client);
    if (candidateArea > context->bestArea) {
        context->best = hwnd;
        context->bestArea = candidateArea;
    }
    return TRUE;
}

HWND findTargetWindow(unsigned long processId) {
    FindWindowContext context{processId};
    EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&context));
    return context.best;
}

bool fileExists(wchar_t const* path) {
    return path != nullptr && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool writeReadyFile(wchar_t const* path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || file == nullptr)
        return false;
    std::fputs("READY\n", file);
    std::fclose(file);
    return true;
}

LRESULT CALLBACK companionWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        auto const dc = BeginPaint(hwnd, &paint);
        auto const color = static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        auto const brush = CreateSolidBrush(color == 0 ? RGB(0, 0, 0) : RGB(255, 255, 255));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool registerCompanionClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = companionWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"MvmP2WindowStateCompanion";
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool clientRectOnScreen(HWND hwnd, RECT& result) {
    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return false;
    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight))
        return false;
    result = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return true;
}

unsigned long cloakedState(HWND hwnd) {
    unsigned long cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return 0xffffffffUL;
    return cloaked;
}

OcclusionInfo unexpectedOcclusion(HWND target, unsigned long targetProcessId, HWND designated,
                                  HWND dirty, RECT const& client) {
    OcclusionInfo result{};
    for (auto current = GetWindow(target, GW_HWNDPREV); current != nullptr;
         current = GetWindow(current, GW_HWNDPREV)) {
        if (current == designated || current == dirty || !IsWindowVisible(current) ||
            IsIconic(current))
            continue;
        unsigned long processId = 0;
        GetWindowThreadProcessId(current, &processId);
        if (processId == targetProcessId)
            continue;
        auto const cloaked = cloakedState(current);
        if (cloaked != 0 && cloaked != 0xffffffffUL)
            continue;
        RECT rect{};
        if (GetWindowRect(current, &rect)) {
            auto const intersection = intersectionArea(client, rect);
            result.totalArea += intersection;
            if (intersection > result.largestArea) {
                result.largestArea = intersection;
                result.largestHwnd = current;
                result.largestProcessId = processId;
                result.largestRect = rect;
            }
        }
    }
    return result;
}

bool configureTargetWindow(HWND target, RECT& workArea) {
    auto const monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
        return false;
    workArea = monitorInfo.rcWork;
    auto const workWidth = workArea.right - workArea.left;
    auto const workHeight = workArea.bottom - workArea.top;
    // foreground PowerShellのcaret accessibility overlayと重ならない専用領域へ固定する。
    auto const clientWidth = std::max<long>(640, std::min<long>(1280, workWidth - 480));
    auto const clientHeight = std::max<long>(360, std::min<long>(720, workHeight - 320));
    RECT outer{0, 0, clientWidth, clientHeight};
    auto const style = static_cast<DWORD>(GetWindowLongPtrW(target, GWL_STYLE));
    auto const extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(target, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&outer, style, FALSE, extendedStyle))
        return false;
    auto const x = workArea.left + 240;
    auto const y = workArea.top + 160;
    ShowWindow(target, SW_RESTORE);
    if (!SetWindowPos(target, HWND_TOPMOST, x, y, outer.right - outer.left,
                      outer.bottom - outer.top, SWP_SHOWWINDOW))
        return false;
    return true;
}

HWND createCompanion(HINSTANCE instance, RECT const& rect) {
    auto const hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                      L"MvmP2WindowStateCompanion", L"mvm diagnostic companion",
                                      WS_POPUP, rect.left, rect.top, rect.right - rect.left,
                                      rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (hwnd != nullptr) {
        SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left,
                     rect.bottom - rect.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

unsigned long lastInputTick() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    return GetLastInputInfo(&info) ? info.dwTime : 0;
}

bool writeJson(wchar_t const* outputPath, Mode mode, unsigned long processId,
               std::int64_t qpcFrequency, HWND target, HWND occluder, HWND dirty,
               std::uint64_t dirtyTickCount, std::uint64_t targetDamageCount,
               std::uint64_t targetDamageFailureCount,
               std::uint64_t targetUpdateRegionObservedCount, unsigned long readyInputTick,
               std::vector<Sample> const& samples) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, outputPath, L"wb") != 0 || file == nullptr)
        return false;
    auto const modeName =
        mode == Mode::Visible            ? "VISIBLE_UNOCCLUDED"
        : mode == Mode::Occluded         ? "FULLY_OCCLUDED"
        : mode == Mode::ForceDirty       ? "VISIBLE_UNOCCLUDED_FORCE_DIRTY"
        : mode == Mode::TargetInvalidate ? "VISIBLE_UNOCCLUDED_TARGET_INVALIDATE"
                                         : "VISIBLE_UNOCCLUDED_TARGET_REDRAW_NOW";
    std::fprintf(file, "{\n  \"schema\": \"mvm-p2-c3-a3-t1-window-state-1\",\n");
    std::fprintf(file, "  \"mode\": \"%s\",\n", modeName);
    std::fprintf(file, "  \"target_process_id\": %lu,\n", processId);
    std::fprintf(file, "  \"qpc_frequency\": %lld,\n", static_cast<long long>(qpcFrequency));
    std::fprintf(file, "  \"target_hwnd\": \"0x%llx\",\n",
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target)));
    std::fprintf(file, "  \"occluder_hwnd\": \"0x%llx\",\n",
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(occluder)));
    std::fprintf(file, "  \"dirty_companion_hwnd\": \"0x%llx\",\n",
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(dirty)));
    std::fprintf(file, "  \"dirty_tick_count\": %llu,\n",
                 static_cast<unsigned long long>(dirtyTickCount));
    std::fprintf(file, "  \"target_damage_count\": %llu,\n",
                 static_cast<unsigned long long>(targetDamageCount));
    std::fprintf(file, "  \"target_damage_failure_count\": %llu,\n",
                 static_cast<unsigned long long>(targetDamageFailureCount));
    std::fprintf(file, "  \"target_update_region_observed_count\": %llu,\n",
                 static_cast<unsigned long long>(targetUpdateRegionObservedCount));
    std::fprintf(file, "  \"last_input_tick_at_ready\": %lu,\n", readyInputTick);
    std::fprintf(file, "  \"samples\": [");
    for (std::size_t index = 0; index < samples.size(); ++index) {
        auto const& sample = samples[index];
        auto printRect = [file](char const* name, RectValue const& rect) {
            std::fprintf(file,
                         "\"%s\": {\"left\": %ld, \"top\": %ld, \"right\": %ld, "
                         "\"bottom\": %ld}",
                         name, rect.left, rect.top, rect.right, rect.bottom);
        };
        std::fprintf(file,
                     "%s{\"qpc\": %lld, \"target_hwnd\": \"0x%llx\", "
                     "\"visible\": %s, \"iconic\": %s, \"topmost\": %s, "
                     "\"cloaked\": %lu, ",
                     index == 0 ? "" : ",", static_cast<long long>(sample.qpc),
                     static_cast<unsigned long long>(sample.targetHwnd),
                     sample.visible ? "true" : "false", sample.iconic ? "true" : "false",
                     sample.topmost ? "true" : "false", sample.cloaked);
        printRect("window_rect", sample.windowRect);
        std::fprintf(file, ", ");
        printRect("client_rect", sample.clientRect);
        std::fprintf(file,
                     ", \"monitor\": \"0x%llx\", \"foreground_hwnd\": \"0x%llx\", "
                     "\"occluder_hwnd\": \"0x%llx\", ",
                     static_cast<unsigned long long>(sample.monitor),
                     static_cast<unsigned long long>(sample.foregroundHwnd),
                     static_cast<unsigned long long>(sample.occluderHwnd));
        printRect("occluder_rect", sample.occluderRect);
        std::fprintf(file,
                     ", \"client_area\": %lld, \"designated_intersection_area\": %lld, "
                     "\"unexpected_intersection_area\": %lld, "
                     "\"largest_unexpected_intersection_area\": %lld, "
                     "\"largest_unexpected_hwnd\": \"0x%llx\", "
                     "\"largest_unexpected_process_id\": %lu, ",
                     static_cast<long long>(sample.clientArea),
                     static_cast<long long>(sample.designatedIntersectionArea),
                     static_cast<long long>(sample.unexpectedIntersectionArea),
                     static_cast<long long>(sample.largestUnexpectedIntersectionArea),
                     static_cast<unsigned long long>(sample.largestUnexpectedHwnd),
                     sample.largestUnexpectedProcessId);
        printRect("largest_unexpected_rect", sample.largestUnexpectedRect);
        std::fprintf(file,
                     ", \"dirty_tick_count\": %llu, \"target_damage_count\": %llu, "
                     "\"target_update_region_present\": %s, \"last_input_tick\": %lu}",
                     static_cast<unsigned long long>(sample.dirtyTickCount),
                     static_cast<unsigned long long>(sample.targetDamageCount),
                     sample.targetUpdateRegionPresent ? "true" : "false", sample.lastInputTick);
    }
    std::fprintf(file, "]\n}\n");
    std::fclose(file);
    return true;
}

bool parseUnsigned(wchar_t const* value, unsigned long& result) {
    if (value == nullptr || *value == L'\0')
        return false;
    wchar_t* end = nullptr;
    result = std::wcstoul(value, &end, 10);
    return end != nullptr && *end == L'\0' && result != 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    unsigned long processId = 0;
    wchar_t const* modeValue = nullptr;
    wchar_t const* outputPath = nullptr;
    wchar_t const* readyPath = nullptr;
    wchar_t const* stopPath = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--process-id") == 0 && index + 1 < argc)
            parseUnsigned(argv[++index], processId);
        else if (std::wcscmp(argv[index], L"--mode") == 0 && index + 1 < argc)
            modeValue = argv[++index];
        else if (std::wcscmp(argv[index], L"--output") == 0 && index + 1 < argc)
            outputPath = argv[++index];
        else if (std::wcscmp(argv[index], L"--ready-file") == 0 && index + 1 < argc)
            readyPath = argv[++index];
        else if (std::wcscmp(argv[index], L"--stop-file") == 0 && index + 1 < argc)
            stopPath = argv[++index];
    }
    Mode mode{};
    if (modeValue != nullptr && std::wcscmp(modeValue, L"VISIBLE_UNOCCLUDED") == 0)
        mode = Mode::Visible;
    else if (modeValue != nullptr && std::wcscmp(modeValue, L"FULLY_OCCLUDED") == 0)
        mode = Mode::Occluded;
    else if (modeValue != nullptr && std::wcscmp(modeValue, L"VISIBLE_UNOCCLUDED_FORCE_DIRTY") == 0)
        mode = Mode::ForceDirty;
    else if (modeValue != nullptr &&
             std::wcscmp(modeValue, L"VISIBLE_UNOCCLUDED_TARGET_INVALIDATE") == 0)
        mode = Mode::TargetInvalidate;
    else if (modeValue != nullptr &&
             std::wcscmp(modeValue, L"VISIBLE_UNOCCLUDED_TARGET_REDRAW_NOW") == 0)
        mode = Mode::TargetRedrawNow;
    else {
        std::fwprintf(stderr, L"window state modeが不正です\n");
        return 2;
    }
    if (processId == 0 || outputPath == nullptr || readyPath == nullptr || stopPath == nullptr) {
        std::fwprintf(stderr, L"使い方: mvm_p2_window_state_controller --process-id <pid> "
                              L"--mode <mode> --output <json> --ready-file <path> "
                              L"--stop-file <path>\n");
        return 2;
    }

    HWND target = nullptr;
    for (int attempt = 0; attempt < 500 && target == nullptr; ++attempt) {
        target = findTargetWindow(processId);
        if (target == nullptr)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    RECT workArea{};
    if (target == nullptr || !configureTargetWindow(target, workArea)) {
        std::fwprintf(stderr, L"target windowを固定できません\n");
        return 3;
    }
    // Qtが遅延activation時に保存placementを適用するため、共通warmup内だけ
    // 十分長く再固定する。measurement中は操作しない。
    SetForegroundWindow(target);
    for (int settle = 0; settle < 180; ++settle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!configureTargetWindow(target, workArea)) {
            std::fwprintf(stderr, L"target windowのsettleに失敗しました\n");
            return 3;
        }
    }
    auto const instance = GetModuleHandleW(nullptr);
    if (!registerCompanionClass(instance)) {
        std::fwprintf(stderr, L"companion window classを登録できません\n");
        return 3;
    }
    RECT client{};
    if (!clientRectOnScreen(target, client)) {
        std::fwprintf(stderr, L"target client rectを取得できません\n");
        return 3;
    }
    HWND occluder = nullptr;
    HWND dirty = nullptr;
    if (mode == Mode::Occluded) {
        occluder = createCompanion(instance, client);
        if (occluder == nullptr) {
            std::fwprintf(stderr, L"occluder windowを作成できません\n");
            return 3;
        }
    } else if (mode == Mode::ForceDirty) {
        RECT dirtyRect{workArea.right - 96, workArea.bottom - 96, workArea.right - 32,
                       workArea.bottom - 32};
        if (intersectionArea(client, dirtyRect) != 0) {
            dirtyRect = {workArea.right - 96, workArea.top + 32, workArea.right - 32,
                         workArea.top + 96};
        }
        dirty = createCompanion(instance, dirtyRect);
        if (dirty == nullptr || intersectionArea(client, dirtyRect) != 0) {
            std::fwprintf(stderr, L"dirty companionを非重複位置へ作成できません\n");
            return 3;
        }
    }
    auto const readyInputTick = lastInputTick();
    if (!writeReadyFile(readyPath)) {
        std::fwprintf(stderr, L"ready fileを作成できません\n");
        return 4;
    }

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::vector<Sample> samples;
    std::uint64_t dirtyTickCount = 0;
    std::uint64_t targetDamageCount = 0;
    std::uint64_t targetDamageFailureCount = 0;
    std::uint64_t targetUpdateRegionObservedCount = 0;
    bool targetUpdateRegionPresent = false;
    auto nextSample = std::chrono::steady_clock::now();
    auto nextDirty = nextSample;
    auto nextTargetDamage = nextSample;
    MSG message{};
    while (!fileExists(stopPath)) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        auto const now = std::chrono::steady_clock::now();
        if (dirty != nullptr && now >= nextDirty) {
            ++dirtyTickCount;
            SetWindowLongPtrW(dirty, GWLP_USERDATA, static_cast<LONG_PTR>(dirtyTickCount & 1ULL));
            RedrawWindow(dirty, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            nextDirty += std::chrono::milliseconds(16);
        }
        if ((mode == Mode::TargetInvalidate || mode == Mode::TargetRedrawNow) &&
            now >= nextTargetDamage) {
            // client座標の小矩形だけを対象にする。output pixelsは変更しない。
            RECT const damage{0, 0, 8, 8};
            BOOL issued = FALSE;
            if (mode == Mode::TargetInvalidate) {
                // update regionを設定するだけ。paint処理までは進めない。
                issued = InvalidateRect(target, &damage, FALSE);
            } else {
                // invalidate + 同期的なpaint処理まで進める positive control。
                issued = RedrawWindow(target, &damage, nullptr,
                                      RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE |
                                          RDW_NOCHILDREN);
            }
            if (issued)
                ++targetDamageCount;
            else
                ++targetDamageFailureCount;
            RECT updateRect{};
            targetUpdateRegionPresent = GetUpdateRect(target, &updateRect, FALSE) != FALSE;
            if (targetUpdateRegionPresent)
                ++targetUpdateRegionObservedCount;
            nextTargetDamage += std::chrono::milliseconds(16);
        }
        if (now >= nextSample) {
            if (!IsWindow(target))
                break;
            Sample sample{};
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            sample.qpc = qpc.QuadPart;
            sample.targetHwnd = reinterpret_cast<std::uintptr_t>(target);
            sample.visible = IsWindowVisible(target) != FALSE;
            sample.iconic = IsIconic(target) != FALSE;
            sample.topmost = (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            sample.cloaked = cloakedState(target);
            RECT windowRect{};
            GetWindowRect(target, &windowRect);
            clientRectOnScreen(target, client);
            sample.windowRect = valueOf(windowRect);
            sample.clientRect = valueOf(client);
            sample.monitor = reinterpret_cast<std::uintptr_t>(
                MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST));
            sample.foregroundHwnd = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
            sample.occluderHwnd = reinterpret_cast<std::uintptr_t>(occluder);
            RECT occluderRect{};
            if (occluder != nullptr)
                GetWindowRect(occluder, &occluderRect);
            sample.occluderRect = valueOf(occluderRect);
            sample.clientArea = area(client);
            sample.designatedIntersectionArea =
                occluder != nullptr ? intersectionArea(client, occluderRect) : 0;
            auto const unexpected = unexpectedOcclusion(target, processId, occluder, dirty, client);
            sample.unexpectedIntersectionArea = unexpected.totalArea;
            sample.largestUnexpectedIntersectionArea = unexpected.largestArea;
            sample.largestUnexpectedHwnd = reinterpret_cast<std::uintptr_t>(unexpected.largestHwnd);
            sample.largestUnexpectedProcessId = unexpected.largestProcessId;
            sample.largestUnexpectedRect = valueOf(unexpected.largestRect);
            sample.dirtyTickCount = dirtyTickCount;
            sample.targetDamageCount = targetDamageCount;
            sample.targetUpdateRegionPresent = targetUpdateRegionPresent;
            sample.lastInputTick = lastInputTick();
            samples.push_back(sample);
            nextSample += std::chrono::milliseconds(100);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (occluder != nullptr)
        DestroyWindow(occluder);
    if (dirty != nullptr)
        DestroyWindow(dirty);
    if (samples.empty() ||
        !writeJson(outputPath, mode, processId, frequency.QuadPart, target, occluder, dirty,
                   dirtyTickCount, targetDamageCount, targetDamageFailureCount,
                   targetUpdateRegionObservedCount, readyInputTick, samples)) {
        std::fwprintf(stderr, L"window state JSONを作成できません\n");
        return 4;
    }
    return 0;
}
