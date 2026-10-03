// atomic write の置換が、置換先を一時的に開いている reader に負けないこと。
//
// 自動復旧 (recovery) の file は、試験やウイルス対策ソフト・検索の索引が読むことがある。
// Windows では、置換先を FILE_SHARE_DELETE なしで開いている間 MoveFileExW が失敗する
// (std::ifstream はこの開き方になる)。失敗すると自動復旧の再試行は 30 秒後になり、
// その間 recovery は古いまま残る (CI の m7b_4_controller_export_lifecycle で Redo 後の
// recovery が 20 秒以内に更新されなかった)。短い間の共有違反は待って置換し直す。
// いつまでも開かれている場合は、限られた時間で失敗を返す (呼び出し側を止め続けない)。

#include "util/mvm_atomic_write.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool write(const std::filesystem::path& path, const std::string& text, std::string& error) {
    char buffer[256] = {};
    const int code =
        mvm_atomic_write_file(path.c_str(), text.data(), text.size(), buffer, sizeof(buffer));
    error = buffer;
    return code == 0;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool temporaryLeft(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().wstring().find(L".mvmtmp.") != std::wstring::npos)
            return true;
    }
    return false;
}

// 置換先を reader と同じ開き方で holdMs の間開いたまま、置換を試みる。
bool writeWhileOpen(const std::filesystem::path& path, const std::string& text, int holdMs,
                    std::string& error, long long& elapsedMs) {
    std::ifstream reader(path, std::ios::binary);
    if (!reader)
        return false;
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        reader.close();
    });
    const auto started = std::chrono::steady_clock::now();
    const bool written = write(path, text, error);
    elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    closer.join();
    return written;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_atomic_write <作業directory>\n");
        return 2;
    }
    const std::filesystem::path directory = argv[1];
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / L"recovery.json";
    std::string error;

    check(write(path, "v1", error) && read(path) == "v1", "初回の書き込みができません");

    {
        // 短い間だけ開かれている: 待って置換し直し、新しい内容になる。
        long long elapsedMs = 0;
        const bool written = writeWhileOpen(path, "v2", 300, error, elapsedMs);
        std::printf("300ms 開いた置換先: %s (%lld ms) %s\n", written ? "成功" : "失敗", elapsedMs,
                    error.c_str());
        check(written, "短い間だけ開かれた置換先へ書き込めません");
        check(read(path) == "v2", "置換後の内容が新しくありません");
        check(!temporaryLeft(directory), "一時fileが残っています");
    }
    {
        // 開かれ続けている: 限られた時間で失敗し、元の内容と一時fileの後始末を保つ。
        long long elapsedMs = 0;
        const bool written = writeWhileOpen(path, "v3", 6000, error, elapsedMs);
        std::printf("6000ms 開いた置換先: %s (%lld ms) %s\n", written ? "成功" : "失敗", elapsedMs,
                    error.c_str());
        check(!written && !error.empty(), "開かれ続けた置換先への書き込みを成功にしました");
        check(elapsedMs < 4000, "開かれ続けた置換先で長く待ちすぎます");
        check(read(path) == "v2", "失敗した置換で元の内容が変わりました");
        check(!temporaryLeft(directory), "失敗した置換の一時fileが残っています");
    }

    std::filesystem::remove_all(directory);
    if (failures != 0)
        return 1;
    std::printf("PASS\n");
    return 0;
}
