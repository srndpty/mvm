// Explorer表示ヘルパの試験。Explorer は開かない。
// - 負例 (空・存在しない・directory) は、Shell を呼ぶ前に失敗する。
// - P3-7A: Shell API の境界 (mvm_reveal_set_shell_for_test) で SHOpenFolderAndSelectItems だけを
//   差し替え、SHParseDisplayName は実際の API を使う。QML の FileDialog から来る / 区切りの path
//   (日本語・空白を含む) が、正規化されて Shell に解釈されることを確かめる。

#include "util/mvm_reveal_in_explorer.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <shlobj.h>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool rejects(const wchar_t* path) {
    char error[512] = {};
    return mvm_reveal_in_explorer(path, error, sizeof(error)) != 0 && std::strlen(error) > 0;
}

// 差し替えた Shell の境界が見たもの。
struct ShellRecord {
    std::wstring observed;
    int observeCalls = 0;
    int parseCalls = 0;
    int openCalls = 0;
    bool openSawItem = false;
    long parseResult = S_OK; // 差し替えた parse が返す値 (parse を差し替えるときだけ使う)
    long openResult = S_OK;  // 差し替えた open が返す値
};

void observe(const wchar_t* path, void* opaque) {
    auto* record = static_cast<ShellRecord*>(opaque);
    record->observed = path;
    ++record->observeCalls;
}

long fakeParse(const wchar_t*, void** item, void* opaque) {
    auto* record = static_cast<ShellRecord*>(opaque);
    ++record->parseCalls;
    *item = nullptr;
    return record->parseResult;
}

long fakeOpen(const void* item, void* opaque) {
    auto* record = static_cast<ShellRecord*>(opaque);
    ++record->openCalls;
    record->openSawItem = item != nullptr;
    return record->openResult;
}

std::wstring withSlashes(std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return path;
}

bool hasSlash(const std::wstring& path) {
    return path.find(L'/') != std::wstring::npos;
}

// 実際の SHParseDisplayName の結果 (Explorer は開かない)。
HRESULT realParse(const std::wstring& path) {
    PIDLIST_ABSOLUTE item = nullptr;
    const HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
    if (item)
        CoTaskMemFree(item);
    return hr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);

    check(rejects(nullptr), "NULL pathを受理しました");
    check(rejects(L""), "空pathを受理しました");
    check(rejects((directory / L"存在しない出力.mp4").c_str()), "存在しないファイルを受理しました");
    check(rejects(directory.c_str()), "directoryをファイルとして受理しました");

    // 失敗理由のHRESULTが実際の原因と一致すること。
    char error[512] = {};
    mvm_reveal_in_explorer((directory / L"存在しない出力.mp4").c_str(), error, sizeof(error));
    check(std::strstr(error, "0x80070002") != nullptr,
          "存在しないファイルのHRESULTがERROR_FILE_NOT_FOUNDではありません");
    // directoryではGetFileAttributesWが成功するため、直前に残った無関係な
    // last errorをHRESULTへ流してはならない。わざと別の値を残して検査する。
    SetLastError(ERROR_ACCESS_DENIED);
    mvm_reveal_in_explorer(directory.c_str(), error, sizeof(error));
    check(std::strstr(error, "0x80070057") != nullptr &&
              std::strstr(error, "0x80070005") == nullptr,
          "directoryのHRESULTが直前のlast errorを流用しています");
    if (failures != 0)
        std::fprintf(stderr, "  last error message: %s\n", error);

    // --- P3-7A: / 区切りの path (QUrl::toLocalFile の形) を正規化して Shell へ渡す ---
    const std::filesystem::path unicodeDirectory = directory / L"日本語 フォルダ" / L"出力 先";
    std::filesystem::create_directories(unicodeDirectory);
    const std::filesystem::path output = unicodeDirectory / L"書き出し 結果 ①.mp4";
    {
        std::ofstream(output, std::ios::binary) << "x";
    }
    // 比べる値は Windows の正規の形 (\ 区切り)。CTest が渡す directory は / 区切りで来る。
    const std::wstring native = std::filesystem::absolute(output).make_preferred().wstring();
    const std::wstring slashed = withSlashes(native);

    // 原因の確認: 実際の SHParseDisplayName は / 区切りを E_INVALIDARG
    // で拒否し、\ 区切りは受け付ける。
    const HRESULT comState = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(realParse(slashed) == E_INVALIDARG,
          "SHParseDisplayName が / 区切りを拒否する (修正の前提)");
    check(SUCCEEDED(realParse(native)),
          "SHParseDisplayName が \\ 区切りの日本語・空白の path を受け付ける");
    if (SUCCEEDED(comState))
        CoUninitialize();

    ShellRecord record;
    MvmRevealShell shell{&observe, nullptr, &fakeOpen, &record};
    mvm_reveal_set_shell_for_test(&shell);
    error[0] = '\0';
    const int slashedResult = mvm_reveal_in_explorer(slashed.c_str(), error, sizeof(error));
    check(slashedResult == 0 && error[0] == '\0',
          "/ 区切りの日本語・空白の path を表示できる (実際の SHParseDisplayName が解釈する)");
    check(record.observeCalls == 1 && !hasSlash(record.observed) && record.observed == native,
          "Shell へ渡す path は \\ 区切りの絶対 path");
    check(record.openCalls == 1 && record.openSawItem, "解釈した item で Explorer を開く");
    if (slashedResult != 0)
        std::fprintf(stderr, "  slashed: %s\n", error);

    record = ShellRecord{};
    check(mvm_reveal_in_explorer(native.c_str(), error, sizeof(error)) == 0 &&
              record.observed == native && record.openCalls == 1,
          "\\ 区切りの path も表示できる");

    // 相対 path: 親の directory が現在の directory。絶対 path に直してから渡す。
    {
        wchar_t previous[MAX_PATH * 4] = {};
        GetCurrentDirectoryW(MAX_PATH * 4, previous);
        SetCurrentDirectoryW(unicodeDirectory.c_str());
        record = ShellRecord{};
        check(mvm_reveal_in_explorer(L"書き出し 結果 ①.mp4", error, sizeof(error)) == 0 &&
                  record.observed == native,
              "相対 path を現在の directory から解決する");
        SetCurrentDirectoryW(previous);
    }

    // 存在しない file: Shell を呼ばずに失敗する (親の directory はある)。
    record = ShellRecord{};
    check(mvm_reveal_in_explorer(withSlashes((unicodeDirectory / L"無い.mp4").wstring()).c_str(),
                                 error, sizeof(error)) != 0 &&
              std::strstr(error, "0x80070002") != nullptr && record.openCalls == 0 &&
              record.observeCalls == 0,
          "存在しない file は Shell を呼ばずに失敗する");

    // Explorer を開けない (Shell の失敗): 失敗の理由と HRESULT をそのまま返す。
    record = ShellRecord{};
    record.openResult = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    check(mvm_reveal_in_explorer(slashed.c_str(), error, sizeof(error)) != 0 &&
              std::strstr(error, "Explorerでファイルを表示できません") != nullptr &&
              std::strstr(error, "0x80070005") != nullptr && record.openCalls == 1,
          "Explorer を開けないときは、その HRESULT を返す");

    // path の解釈の失敗: Explorer を開かずに、解釈の失敗として返す。
    record = ShellRecord{};
    record.parseResult = E_INVALIDARG;
    MvmRevealShell parseFails{&observe, &fakeParse, &fakeOpen, &record};
    mvm_reveal_set_shell_for_test(&parseFails);
    check(mvm_reveal_in_explorer(slashed.c_str(), error, sizeof(error)) != 0 &&
              std::strstr(error, "解釈できません") != nullptr &&
              std::strstr(error, "0x80070057") != nullptr && record.parseCalls == 1 &&
              record.openCalls == 0,
          "path を解釈できないときは Explorer を開かずに、その HRESULT を返す");
    mvm_reveal_set_shell_for_test(&shell);

    // くり返し: 毎回同じ結果。COM の初期化を、呼び出し側の thread に残さない。
    record = ShellRecord{};
    int repeatedOk = 0;
    for (int index = 0; index < 50; ++index)
        repeatedOk += mvm_reveal_in_explorer(slashed.c_str(), error, sizeof(error)) == 0 ? 1 : 0;
    check(repeatedOk == 50 && record.openCalls == 50, "50 回くり返しても毎回表示できる");
    const HRESULT after = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(after == S_OK, "呼び出しの後に COM の初期化が残っていない");
    if (SUCCEEDED(after))
        CoUninitialize();

    // 呼び出し側が別の mode (MTA) で COM を初期化していても使え、その状態を変えない。
    const HRESULT mta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    record = ShellRecord{};
    check(SUCCEEDED(mta) && mvm_reveal_in_explorer(slashed.c_str(), error, sizeof(error)) == 0 &&
              record.openCalls == 1,
          "呼び出し側が MTA でも表示できる");
    check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == RPC_E_CHANGED_MODE,
          "呼び出し側の COM の mode を変えない");
    if (SUCCEEDED(mta))
        CoUninitialize();
    mvm_reveal_set_shell_for_test(nullptr);

    if (failures != 0) {
        std::fprintf(stderr, "reveal in explorer: FAIL (%d 件)\n", failures);
        return 1;
    }
    std::puts("reveal in explorer: PASS (Explorer は開いていない)");
    return 0;
}
