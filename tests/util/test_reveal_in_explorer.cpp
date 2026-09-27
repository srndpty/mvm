// Explorer表示ヘルパの負例。成功経路は実際にExplorerを開くため自動testでは踏まない。

#include "util/mvm_reveal_in_explorer.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

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

    if (failures != 0) {
        std::fprintf(stderr, "reveal in explorer: FAIL (%d 件)\n", failures);
        return 1;
    }
    std::puts("reveal in explorer: PASS");
    return 0;
}
