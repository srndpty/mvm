// extendedLengthPath と removeTree を確かめる (P2-8、docs/math-clips.md)。
//
// - 形式: 期待値は手で書いた文字列 (実装から作らない)
// - 260 文字を超える木: extended-length の path で全 file を走査でき、removeTree が返って
//   すべて消す。removeTree が通常の path の remove_all に戻ると返らないので、時間の上限で
//   失敗にする (この試験が hang しない)。
//
//   mvm_test_long_path <作業 directory>

#include "util/mvm_long_path.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::wstring extended(const wchar_t* path) {
    return mvm::util::extendedLengthPath(path).native();
}

std::size_t countFiles(const std::filesystem::path& root) {
    std::size_t count = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end;
         it.increment(error))
        count += it->is_regular_file(error);
    return count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_long_path <作業 directory>\n");
        return 2;
    }
    check(extended(L"C:\\a\\b") == L"\\\\?\\C:\\a\\b", "drive の絶対 path に \\\\?\\ を付ける");
    check(extended(L"C:/a/./b/../c") == L"\\\\?\\C:\\a\\c",
          "/ を \\ にし、. と .. を解決してから付ける");
    const auto unc = extended(L"\\\\server\\share\\x");
    if (unc != L"\\\\?\\UNC\\server\\share\\x")
        std::fwprintf(stderr, L"UNC: %ls\n", unc.c_str());
    check(unc == L"\\\\?\\UNC\\server\\share\\x", "UNC は \\\\?\\UNC\\ にする");
    check(extended(L"\\\\?\\C:\\x") == L"\\\\?\\C:\\x", "既に extended-length ならそのまま");
    check(extended(L"\\\\.\\pipe\\x") == L"\\\\.\\pipe\\x", "device の path はそのまま");
    check(extended(L"").empty(), "空の path は空のまま");
    {
        // 260 文字を超える入力も字句的に変換する (GetFullPathNameW を通さない)。
        std::wstring deep = L"C:\\deep";
        while (deep.size() < 300)
            deep += L"\\0123456789abcdef";
        check(extended((deep + L"\\.\\x\\..\\y").c_str()) == L"\\\\?\\" + deep + L"\\y",
              "260 文字を超える入力も変換する");
    }
    {
        const auto relative = extended(L"rel\\x");
        const auto cwd = std::filesystem::current_path().wstring();
        check(relative == L"\\\\?\\" + cwd + L"\\rel\\x", "相対 path は current directory から");
    }

    const auto base = std::filesystem::absolute(argv[1]) / L"long-path-tree";
    std::error_code error;
    check(mvm::util::removeTree(base, error), "前回の残りを消す: " + error.message());
    const auto write = [](const std::filesystem::path& directory, const std::wstring& name) {
        std::error_code ignored;
        std::filesystem::create_directories(mvm::util::extendedLengthPath(directory), ignored);
        std::ofstream out(mvm::util::extendedLengthPath(directory) / name, std::ios::binary);
        out << "x";
        return static_cast<bool>(out);
    };

    // 1. Manim の作業 directory と同じ形: directory は 260 文字未満、file は 260 文字を超える。
    auto images = base / L"images";
    while (images.native().size() < 230)
        images /= L"0123456789abcdef";
    const std::wstring longName(260 - images.native().size() + 20, L'f');
    constexpr int kFiles = 5;
    bool written = true;
    for (int index = 0; index < kFiles; ++index)
        written = write(images, longName + std::to_wstring(index) + L".png") && written;
    const auto longest = (images / (longName + L"0.png")).native().size();
    std::fprintf(stderr, "directory %zu 文字、file %zu 文字\n", images.native().size(), longest);
    check(written && images.native().size() < 260 && longest > 260,
          "前提: 260 文字未満の directory に 260 文字を超える file を書く");
    const auto plain = countFiles(images);
    const auto found = countFiles(mvm::util::extendedLengthPath(images));
    // 通常の path の件数は環境 (LongPathsEnabled) で変わるので記録だけする。
    std::fprintf(stderr, "走査: 通常の path %zu 件、extended-length %zu 件\n", plain, found);
    check(found == kFiles, "extended-length の path で 260 文字を超える file をすべて見つける");

    // 2. directory 自体が 260 文字を超える木 (列挙できない深さ) も消せる。
    auto deepest = base / L"deep";
    while (deepest.native().size() < 330)
        deepest /= L"0123456789abcdef0123456789abcdef";
    check(write(deepest, L"frame.png") && write(deepest / L"..", L"readonly.png"),
          "前提: 260 文字を超える directory に file を書く");
    SetFileAttributesW((mvm::util::extendedLengthPath(deepest / L"..") / L"readonly.png").c_str(),
                       FILE_ATTRIBUTE_READONLY);

    // removeTree は返り、すべて消す。返らなければ 30 秒で失敗にする。
    std::atomic<bool> done{false};
    bool removed = false;
    std::error_code removeError;
    std::thread remover([&] {
        removed = mvm::util::removeTree(base, removeError);
        done = true;
    });
    const auto start = std::chrono::steady_clock::now();
    while (!done && std::chrono::steady_clock::now() - start < std::chrono::seconds(30))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!done) {
        std::fprintf(stderr, "FAIL: removeTree が 30 秒で返らない\n");
        std::fflush(stderr);
        std::_Exit(1);
    }
    remover.join();
    check(removed && !removeError &&
              GetFileAttributesW(mvm::util::extendedLengthPath(base).c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
          "removeTree は 260 文字を超える木 (読み取り専用の file を含む) をすべて消す: " +
              removeError.message());
    check(mvm::util::removeTree(base, error) && !error, "無い path の removeTree は成功");

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
