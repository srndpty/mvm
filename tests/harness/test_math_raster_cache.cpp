// MathRasterCache (数式の描画結果の cache) を偽の backend で検査する。
//
// - backend の確認 (preflight) が終わるまでは描かない。使えなければ Unavailable
// - key 単位で 1 回だけ描き、disk に置いた結果は次の instance が描かずに読む
// - provenance (toolchain) が違う・画像が壊れている disk の結果は使わずに描き直す
// - 失敗を覚えて自動では描き直さない。forgetFailures / startPreflight で解除する
// - 要求されなくなった key の描画は止め、その結果を残さない
// - 描画中に破棄しても、描画を止めて速やかに返る

#include "math_fake_backend.h"
#include "math_raster_cache.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

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

using mvm::app::MathRasterCache;
namespace math = mvm::math;

bool waitUntil(const std::function<bool()>& condition, int milliseconds = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > milliseconds)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

// 偽の backend (式の内容で振る舞いを変え、描画の回数を数える)。
using FakeBackend = mvm::test::FakeMathBackend;

math::MathRenderSpec spec(const std::string& source) {
    return {"latex", source, 96};
}

std::unique_ptr<MathRasterCache> readyCache(const std::filesystem::path& directory,
                                            const FakeBackend& backend,
                                            const std::string& session = "session-a") {
    auto cache = std::make_unique<MathRasterCache>(session, backend.preflight());
    cache->setAuthority(directory, true);
    check(
        waitUntil([&] { return cache->backendState() != MathRasterCache::BackendState::Checking; }),
        "preflight が終わる");
    return cache;
}

MathRasterCache::Entry waitForResult(MathRasterCache& cache, const math::MathRenderSpec& s) {
    MathRasterCache::Entry entry = cache.request(s);
    waitUntil([&] {
        entry = cache.request(s);
        return entry.state != MathRasterCache::State::Pending;
    });
    return entry;
}

void testUnavailable(const std::filesystem::path& root) {
    MathRasterCache cache("session-a",
                          FakeBackend::unavailable("LaTeX (latex.exe) が PATH に見つかりません"));
    check(cache.request(spec("x")).state == MathRasterCache::State::Unavailable,
          "権限を受け取る前は Unavailable (描かない)");
    cache.setAuthority(root / L"unavailable", true);
    check(waitUntil(
              [&] { return cache.backendState() == MathRasterCache::BackendState::Unavailable; }),
          "使えない backend は Unavailable");
    const auto entry = cache.request(spec("x"));
    check(entry.state == MathRasterCache::State::Unavailable &&
              entry.message.contains(QStringLiteral("latex.exe")),
          "Unavailable の理由を返す");
    check(cache.keyFor(spec("x")).isEmpty(), "backend が無ければ key を作らない");
}

void testRenderAndDisk(const std::filesystem::path& root) {
    const auto directory = root / L"cache 日本語";
    FakeBackend backend;
    {
        auto cache = readyCache(directory, backend);
        const auto entry = waitForResult(*cache, spec("x^2"));
        check(entry.state == MathRasterCache::State::Ready && entry.mask &&
                  entry.mask->width == 3 && entry.mask->height == 2,
              "描いた mask を返す");
        check(entry.mask && entry.mask->rgba[3] == 255 && entry.mask->rgba[7] == 128,
              "mask は PNG の alpha をそのまま持つ");
        check(*backend.renders == 1, "1 回だけ描く");
        check(cache->request(spec("x^2")).state == MathRasterCache::State::Ready &&
                  *backend.renders == 1,
              "同じ key は描き直さない");
        const auto key = cache->keyFor(spec("x^2"));
        const auto artifact = cache->readyArtifact(spec("x^2"));
        check(artifact && std::filesystem::is_regular_file(*artifact) &&
                  artifact->filename() == key.toStdWString() + L".png",
              "PNG を cache directory の <key>.png に置く");
        check(std::filesystem::is_regular_file(directory / (key.toStdWString() + L".txt")),
              "provenance を <key>.txt に置く");
        check(cache->jobsDirectory() == directory / L"jobs" / L"session-a",
              "作業 directory は instance (session) ごとに分ける");
        check(!std::filesystem::exists(cache->jobsDirectory() / (key.toStdWString() + L"-1")),
              "作業 directory を残さない");
        check(!cache->readyArtifact(spec("other")), "未要求の key に artifact は無い");
    }
    {
        // 次の instance (開き直し) は disk から読む。
        auto cache = readyCache(directory, backend);
        const auto entry = waitForResult(*cache, spec("x^2"));
        check(entry.state == MathRasterCache::State::Ready && *backend.renders == 1,
              "disk の結果を描かずに読む");
    }
    {
        FakeBackend updated;
        updated.canonical = "backend=fake\nversion=2\n";
        updated.renders = backend.renders;
        auto cache = readyCache(directory, updated);
        const auto entry = waitForResult(*cache, spec("x^2"));
        check(entry.state == MathRasterCache::State::Ready && *backend.renders == 2,
              "toolchain が変われば (key が変わり) 描き直す");
    }
    {
        auto cache = readyCache(directory, backend);
        const auto key = cache->keyFor(spec("x^2"));
        std::ofstream(directory / (key.toStdWString() + L".png"), std::ios::binary) << "壊れた";
        const auto entry = waitForResult(*cache, spec("x^2"));
        check(entry.state == MathRasterCache::State::Ready && *backend.renders == 3,
              "壊れた PNG は使わずに描き直す");
    }
    {
        auto cache = readyCache(directory, backend);
        const auto key = cache->keyFor(spec("x^2"));
        const auto provenance = directory / (key.toStdWString() + L".txt");
        std::string text;
        {
            std::ifstream input(provenance, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        const auto at = text.find("version=1");
        check(at != std::string::npos, "provenance に toolchain を書く");
        if (at != std::string::npos)
            text.replace(at, 9, "version=9");
        std::ofstream(provenance, std::ios::binary) << text;
        const auto entry = waitForResult(*cache, spec("x^2"));
        check(entry.state == MathRasterCache::State::Ready && *backend.renders == 4,
              "provenance の toolchain が違う disk の結果は使わない");
    }
}

void testFailures(const std::filesystem::path& root) {
    FakeBackend backend;
    auto cache = readyCache(root / L"failures", backend);
    const auto bad = waitForResult(*cache, spec("BAD"));
    check(bad.state == MathRasterCache::State::Failed &&
              bad.status == math::MathRenderStatus::InvalidSource &&
              bad.message == QStringLiteral("Undefined control sequence."),
          "式の誤りは Failed で理由を返す");
    check(!cache->readyArtifact(spec("BAD")), "失敗した key に artifact は無い");
    const int rendersAfterFailure = *backend.renders;
    for (int i = 0; i < 3; ++i)
        cache->request(spec("BAD"));
    waitUntil([] { return false; }, 100);
    check(*backend.renders == rendersAfterFailure, "失敗を覚え、自動では描き直さない");
    cache->forgetFailures();
    waitForResult(*cache, spec("BAD"));
    check(*backend.renders == rendersAfterFailure + 1, "forgetFailures の後は描き直す");

    const auto gone = waitForResult(*cache, spec("GONE"));
    check(gone.state == MathRasterCache::State::Unavailable,
          "描画中に toolchain が無ければ Unavailable");

    cache->startPreflight();
    check(cache->request(spec("BAD")).state == MathRasterCache::State::Pending,
          "再確認中は Pending");
    waitUntil([&] { return cache->backendState() == MathRasterCache::BackendState::Available; });
    waitForResult(*cache, spec("BAD"));
    check(*backend.renders == rendersAfterFailure + 3, "startPreflight で失敗を忘れて描き直す");
}

void testRetainOnlyCancels(const std::filesystem::path& root) {
    FakeBackend backend;
    auto cache = readyCache(root / L"retain", backend);
    check(cache->request(spec("SLOW")).state == MathRasterCache::State::Pending, "描画を始める");
    waitUntil([&] { return backend.slowStarted->load(); });
    // SLOW を要求しなくなり、別の式を要求する。
    const auto keep = cache->keyFor(spec("y"));
    cache->retainOnly({keep});
    const auto y = waitForResult(*cache, spec("y"));
    check(*backend.slowSawCancel, "要求されなくなった描画は cancel で止まる");
    check(y.state == MathRasterCache::State::Ready, "後の要求は描ける");
    check(cache->recordCount() == 1, "要求されなくなった key の record を残さない");
}

// 権限が無い間は、確認 (外部 renderer の起動) も cache directory の掃除もしない。
void testWithoutAuthority(const std::filesystem::path& root) {
    const auto directory = root / L"no authority";
    // 他の instance の作業中の directory に見立てる。
    const auto foreignJob = directory / L"jobs" / L"other-session" / L"key-1";
    std::filesystem::create_directories(foreignJob);
    auto preflights = std::make_shared<std::atomic<int>>(0);
    FakeBackend backend;
    MathRasterCache cache(
        "session-b", [preflights, inner = backend.preflight()](const std::filesystem::path& work,
                                                               const std::atomic<bool>* cancel) {
            ++*preflights;
            return inner(work, cancel);
        });
    cache.setAuthority(directory, false, QStringLiteral("他のプロセスが編集中です"));
    cache.startPreflight();
    waitUntil([] { return false; }, 200);
    check(*preflights == 0, "権限が無ければ確認 (外部 renderer) を始めない");
    check(std::filesystem::exists(foreignJob), "権限が無ければ cache directory を掃除しない");
    check(cache.backendState() == MathRasterCache::BackendState::Unavailable &&
              cache.backendMessage() == QStringLiteral("他のプロセスが編集中です"),
          "権限が無いことを理由付きの Unavailable で示す");
    const auto entry = cache.request(spec("x"));
    check(entry.state == MathRasterCache::State::Unavailable && *backend.renders == 0,
          "権限が無ければ描かない");

    // 対照: 権限を受け取ると確認を始め、前回の作業 directory の残りを掃除する。
    cache.setAuthority(directory, true);
    check(
        waitUntil([&] { return cache.backendState() == MathRasterCache::BackendState::Available; }),
        "権限を受け取ると確認して Available");
    check(*preflights == 1 && !std::filesystem::exists(foreignJob),
          "対照: 権限があれば確認し、残った作業 directory を消す");
}

// 確認の途中で置き場所が変わっても、必ず次の確認が Available / Unavailable に着く。
void testDirectoryChangeDuringPreflight(const std::filesystem::path& root) {
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto firstEntered = std::make_shared<std::atomic<bool>>(false);
    auto releaseFirst = std::make_shared<std::atomic<bool>>(false);
    auto workDirectories = std::make_shared<std::vector<std::filesystem::path>>();
    auto workMutex = std::make_shared<std::mutex>();
    FakeBackend backend;
    MathRasterCache cache(
        "session-c", [=, inner = backend.preflight()](const std::filesystem::path& work,
                                                      const std::atomic<bool>* cancel) {
            const int call = ++*calls;
            {
                std::lock_guard lock(*workMutex);
                workDirectories->push_back(work);
            }
            if (call == 1) {
                firstEntered->store(true);
                // 試験が離すまで (取消を見ずに) 待つ。置き場所の変更がこの確認の途中で起きる。
                while (!releaseFirst->load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            auto result = inner(work, cancel);
            result.backend.fingerprint.canonical = call == 1 ? "first\n" : "second\n";
            return result;
        });
    cache.setAuthority(root / L"directory old", true);
    check(waitUntil([&] { return firstEntered->load(); }), "最初の確認が始まる");
    cache.setAuthority(root / L"directory new", true);
    check(cache.backendState() == MathRasterCache::BackendState::Checking,
          "置き場所の変更で確認をやり直す");
    releaseFirst->store(true);
    check(
        waitUntil([&] { return cache.backendState() != MathRasterCache::BackendState::Checking; }),
        "置き場所を変えても Checking のまま止まらない");
    check(cache.backendState() == MathRasterCache::BackendState::Available &&
              cache.toolchainText() == QStringLiteral("second\n"),
          "後の確認の結果を使い、途中で捨てた確認の結果は使わない");
    std::lock_guard lock(*workMutex);
    check(*calls == 2 && workDirectories->size() == 2 &&
              workDirectories->back().wstring().find(L"directory new") != std::wstring::npos,
          "後の確認は新しい置き場所で行う");
}

void testDestroyWhileRendering(const std::filesystem::path& root) {
    FakeBackend backend;
    auto cache = readyCache(root / L"destroy", backend);
    cache->request(spec("SLOW"));
    check(waitUntil([&] { return backend.slowStarted->load(); }), "描画が始まる");
    const auto started = std::chrono::steady_clock::now();
    cache.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(*backend.slowSawCancel, "破棄は描画を cancel する");
    check(elapsed < std::chrono::seconds(3), "描画中の破棄が速やかに返る");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_math_raster_cache <test-dir>\n");
        return 2;
    }
    const std::filesystem::path root =
        std::filesystem::absolute(QString::fromLocal8Bit(argv[1]).toStdWString());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    testUnavailable(root);
    testRenderAndDisk(root);
    testFailures(root);
    testRetainOnlyCancels(root);
    testWithoutAuthority(root);
    testDirectoryChangeDuringPreflight(root);
    testDestroyWhileRendering(root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
