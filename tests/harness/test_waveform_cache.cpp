// 波形 cache の identity と eviction。decode は fake に差し替え、呼び出し回数と
// どの呼び出しの結果が残ったかを sampleRate に刻んで追跡する。

#include "waveform_cache.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

using mvm::app::WaveformCache;

std::atomic<int> decodeCalls{0};
// true の間、"slow" を含む path の decode を止める。
std::atomic<bool> holdSlow{false};

constexpr std::int64_t kFakePeakCount = 1000;

mvm::core::WaveformPeaks fakePeaks(int marker) {
    mvm::core::WaveformPeaks peaks;
    peaks.sampleRate = marker;
    peaks.channels = 1;
    mvm::core::WaveformPeakLevel level;
    level.samplesPerPeak = 1;
    level.peakCount = kFakePeakCount;
    level.minimum.assign(kFakePeakCount, 0);
    level.maximum.assign(kFakePeakCount, 0);
    peaks.levels.push_back(std::move(level));
    return peaks;
}

mvm::audio::AudioWaveformResult fakeDecode(const std::string& utf8Path, const std::atomic<bool>*) {
    // cancel は見ない。差し替え後に古い job が結果を返しても、cache 側で捨てること。
    const int marker = ++decodeCalls;
    if (utf8Path.find("slow") != std::string::npos) {
        while (holdSlow.load())
            QThread::msleep(1);
    }
    mvm::audio::AudioWaveformResult result;
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
    if (!std::filesystem::is_regular_file(path)) {
        result.error = "missing";
        return result;
    }
    result.success = true;
    result.peaks = fakePeaks(marker);
    return result;
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

QString qpath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

// 更新時刻は Win32 で直接読み書きする。std::filesystem の実装は秒精度のことがあり、
// 100ns 単位の差を作れない。
std::uint64_t writeTime(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    FILETIME time{};
    GetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool setWriteTime(const std::filesystem::path& path, std::uint64_t value) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    FILETIME time{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    const BOOL ok = SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
    return ok != FALSE && writeTime(path) == value;
}

// revalidateAll の worker と完了通知を処理しきるまで待つ。
void settle() {
    waitUntil([] { return false; }, 200);
}

bool ready(WaveformCache& cache, const QString& path) {
    return cache.request(path).state == WaveformCache::State::Ready;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto directory = std::filesystem::temp_directory_path() /
                           ("mvm_waveform_cache_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);

    // --- 同じ素材は 1 度だけ decode する。path の表記揺れも同じ素材として扱う ---
    {
        WaveformCache cache(WaveformCache::kDefaultBudgetBytes, fakeDecode);
        const auto a = directory / "a.wav";
        writeFile(a, "aaaa");
        decodeCalls = 0;
        check(cache.request(qpath(a)).state == WaveformCache::State::Loading,
              "初回の request が Loading でない");
        check(waitUntil([&] { return ready(cache, qpath(a)); }), "波形が Ready にならない");
        const auto first = cache.request(qpath(a)).peaks;
        const QString variant =
            qpath(directory / "." / "A.WAV").replace(QLatin1Char('\\'), QLatin1Char('/'));
        check(cache.request(variant).peaks == first, "表記揺れの path で別の波形を返した");
        check(decodeCalls == 1, "同じ素材を 2 回 decode した");

        // --- 同じ path の素材が差し替えられたら作り直す ---
        writeFile(a, "aaaaaaaa");
        check(cache.request(qpath(a)).state == WaveformCache::State::Loading,
              "差し替えた素材で古い波形を返した");
        check(waitUntil([&] { return ready(cache, qpath(a)); }), "差し替え後に Ready にならない");
        check(decodeCalls == 2 && cache.request(qpath(a)).peaks != first,
              "差し替え後の波形が作り直されていない");
    }

    // --- 失敗した素材も、後から file が現れたら作り直す ---
    {
        WaveformCache cache(WaveformCache::kDefaultBudgetBytes, fakeDecode);
        const auto late = directory / "late.wav";
        check(waitUntil(
                  [&] { return cache.request(qpath(late)).state == WaveformCache::State::Failed; }),
              "存在しない素材が Failed にならない");
        writeFile(late, "late");
        check(cache.request(qpath(late)).state == WaveformCache::State::Loading,
              "file が現れても Failed を返し続けた");
        check(waitUntil([&] { return ready(cache, qpath(late)); }),
              "現れた素材が Ready にならない");
    }

    // --- 生成中に差し替えられたら、古い job の結果で上書きしない ---
    {
        WaveformCache cache(WaveformCache::kDefaultBudgetBytes, fakeDecode);
        const auto slow = directory / "slow.wav";
        writeFile(slow, "old");
        holdSlow = true;
        decodeCalls = 0;
        cache.request(qpath(slow));
        check(waitUntil([] { return decodeCalls.load() == 1; }), "1 回目の decode が始まらない");
        writeFile(slow, "new-content");
        holdSlow = false;
        cache.request(qpath(slow));
        check(waitUntil([&] { return ready(cache, qpath(slow)); }),
              "差し替え後に Ready にならない");
        // 古い job の完了通知も確実に処理させてから検査する。
        waitUntil([] { return false; }, 100);
        const auto entry = cache.request(qpath(slow));
        check(entry.peaks && entry.peaks->sampleRate == 2, "古い job の結果が残っている");
    }

    // --- 更新時刻は 100ns 単位で比べる。ms へ丸めると同じ size の差し替えを見逃す ---
    {
        WaveformCache cache(WaveformCache::kDefaultBudgetBytes, fakeDecode);
        const auto path = directory / "tick.wav";
        writeFile(path, "aaaa");
        const std::uint64_t base = 133000000000000000ULL; // 100ns 単位、ms 境界ちょうど
        check(setWriteTime(path, base), "更新時刻を設定できない");
        check(waitUntil([&] { return ready(cache, qpath(path)); }), "tick が Ready にならない");
        writeFile(path, "bbbb");
        check(setWriteTime(path, base + 1), "更新時刻を 100ns 進められない");
        check(cache.request(qpath(path)).state == WaveformCache::State::Loading,
              "100ns だけ違う更新時刻の差し替えを見逃した");
        check(waitUntil([&] { return ready(cache, qpath(path)); }),
              "差し替え後に Ready にならない");
    }

    // --- size と更新時刻が一致したまま中身だけ変わった素材は、再検証で作り直す ---
    {
        WaveformCache cache(WaveformCache::kDefaultBudgetBytes, fakeDecode);
        const auto small = directory / "same-stamp.wav";
        const auto large = directory / "same-stamp-large.wav";
        const auto untouched = directory / "untouched.wav";
        std::string largeContent(200 * 1024, 'x');
        writeFile(small, "aaaa");
        writeFile(large, largeContent);
        writeFile(untouched, "keep");
        check(waitUntil([&] { return ready(cache, qpath(small)); }), "small が Ready にならない");
        check(waitUntil([&] { return ready(cache, qpath(large)); }), "large が Ready にならない");
        check(waitUntil([&] { return ready(cache, qpath(untouched)); }),
              "untouched が Ready にならない");
        const auto smallBefore = cache.request(qpath(small)).peaks;
        const auto largeBefore = cache.request(qpath(large)).peaks;
        const auto untouchedBefore = cache.request(qpath(untouched)).peaks;

        const std::uint64_t smallTime = writeTime(small);
        writeFile(small, "bbbb");
        check(setWriteTime(small, smallTime), "small の更新時刻を戻せない");
        // 大きい file は末尾だけを変える (先頭 64KiB は同じ)。
        const std::uint64_t largeTime = writeTime(large);
        largeContent.back() = 'y';
        writeFile(large, largeContent);
        check(setWriteTime(large, largeTime), "large の更新時刻を戻せない");

        // 通常の request は size と更新時刻しか見ない (file の中身は読まない)。
        check(cache.request(qpath(small)).peaks == smallBefore,
              "通常の request で中身まで読んでいる");

        decodeCalls = 0;
        cache.revalidateAll();
        check(waitUntil([&] {
                  const auto s = cache.request(qpath(small));
                  const auto l = cache.request(qpath(large));
                  return s.state == WaveformCache::State::Ready && s.peaks != smallBefore &&
                         l.state == WaveformCache::State::Ready && l.peaks != largeBefore;
              }),
              "同じ size・更新時刻で中身だけ変わった素材を作り直さない");
        settle();
        // 対照群: 中身の変わらない素材は再検証しても decode し直さない。
        check(cache.request(qpath(untouched)).peaks == untouchedBefore && decodeCalls == 2,
              "中身の変わらない素材まで作り直した");
    }

    // --- hard link の別 path は同じ素材として 1 度だけ decode する ---
    {
        WaveformCache cache(WaveformCache::kDefaultBudgetBytes, fakeDecode);
        const auto original = directory / "linked.wav";
        const auto alias = directory / "linked-alias.wav";
        writeFile(original, "link");
        std::error_code linkError;
        std::filesystem::create_hard_link(original, alias, linkError);
        check(!linkError, "hard link を作れない");
        decodeCalls = 0;
        check(waitUntil([&] { return ready(cache, qpath(original)); }),
              "linked が Ready にならない");
        check(cache.request(qpath(alias)).peaks == cache.request(qpath(original)).peaks &&
                  decodeCalls == 1,
              "hard link の別 path を別素材として decode した");
        check(WaveformCache::sourceKey(qpath(alias)) == WaveformCache::sourceKey(qpath(original)),
              "hard link の別 path が別の key になった");
        // 存在しない file は path の字面で区別し、大文字小文字は畳まない
        // (case-sensitive directory では別 file になり得る)。
        check(WaveformCache::sourceKey(qpath(directory / "ghost.wav")) !=
                  WaveformCache::sourceKey(qpath(directory / "GHOST.wav")),
              "存在しない file の大文字小文字を畳んだ");
    }

    // --- budget を超えたら、表示されていない波形から捨てる ---
    {
        const std::size_t one = mvm::core::waveformPeaksMemoryBytes(fakePeaks(0));
        WaveformCache cache(one + one / 2, fakeDecode);
        const auto a = directory / "evict-a.wav";
        const auto b = directory / "evict-b.wav";
        const auto c = directory / "evict-c.wav";
        writeFile(a, "a");
        writeFile(b, "b");
        writeFile(c, "c");
        check(waitUntil([&] { return ready(cache, qpath(a)); }), "a が Ready にならない");
        check(waitUntil([&] { return ready(cache, qpath(b)); }), "b が Ready にならない");
        check(cache.entryCount() == 1 && cache.readyBytes() <= one + one / 2,
              "参照されていない a が捨てられていない");

        // b を view が表示中 (Entry を保持) なら、budget を超えても捨てない。
        const auto heldB = cache.request(qpath(b));
        check(waitUntil([&] { return ready(cache, qpath(c)); }), "c が Ready にならない");
        check(cache.entryCount() == 2 && heldB.peaks, "表示中の b を捨てた");

        // 捨てた a は次の request で作り直す。
        decodeCalls = 0;
        check(cache.request(qpath(a)).state == WaveformCache::State::Loading,
              "捨てた a を Ready のまま返した");
        check(waitUntil([&] { return ready(cache, qpath(a)); }) && decodeCalls == 1,
              "捨てた a を作り直さない");
    }

    std::filesystem::remove_all(directory, ignored);
    if (failures != 0) {
        std::fprintf(stderr, "waveform cache: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("waveform cache: PASS");
    return 0;
}
