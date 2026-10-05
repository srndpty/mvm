// MathRasterCache (数式の描画結果の cache) を偽の backend で検査する。
//
// - backend の確認 (preflight) が終わるまでは描かない。使えなければ Unavailable
// - key 単位で 1 回だけ描き、disk に置いた結果は次の instance が描かずに読む
// - provenance (toolchain) が違う・画像が壊れている disk の結果は使わずに描き直す
// - 失敗を覚えて自動では描き直さない。forgetFailures / startPreflight で解除する
// - 要求されなくなった key の描画は止め、その結果を残さない
// - 描画中に破棄しても、描画を止めて速やかに返る
// - Write の連番も同じ worker・権限・世代で扱い、静止と別の key と disk の置き場所を持つ
// - 変形 (P2-4) は両端の今の静止を待ち、切り出した artifact を provenance の後書きで確定する。
//   preview 用の mask は Write と同じ上限を共有する

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

math::MathSequenceSpec writeSpec(const std::string& source, std::int64_t frames) {
    return {spec(source), math::MathAnimationKind::Write, frames};
}

MathRasterCache::SequenceEntry waitForSequence(MathRasterCache& cache,
                                               const math::MathSequenceSpec& s) {
    MathRasterCache::SequenceEntry entry = cache.requestSequence(s);
    waitUntil([&] {
        entry = cache.requestSequence(s);
        return entry.state != MathRasterCache::State::Pending;
    });
    return entry;
}

MathRasterCache::ResidentSequence waitForResident(MathRasterCache& cache,
                                                  const math::MathSequenceSpec& s) {
    auto result = cache.residentSequence(s);
    waitUntil([&] {
        result = cache.residentSequence(s);
        return result.state != MathRasterCache::Residency::Loading;
    });
    return result;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Write の連番: 静止と別の key で描き、disk の結果を次の instance が描かずに読む。
// provenance を最後に書き、合わない・欠けた frame の結果は使わずに描き直す。
void testSequenceRenderAndDisk(const std::filesystem::path& root) {
    const auto directory = root / L"sequence 日本語";
    FakeBackend backend;
    const auto s = writeSpec("WIDE x", 8);
    QString key;
    {
        auto cache = readyCache(directory, backend);
        key = cache->sequenceKeyFor(s);
        check(!key.isEmpty() && key != cache->keyFor(s.still),
              "連番の key は同じ式の静止の key と別");
        const auto entry = waitForSequence(*cache, s);
        check(entry.state == MathRasterCache::State::Ready && entry.width == 64 &&
                  entry.height == 8,
              "disk に 64x8 の連番が揃う");
        check(cache->residentBytes() == 0 && cache->residentLoadCount() == 0,
              "disk に揃っても preview 用の mask は memory に読まない (要求されるまで)");
        const auto resident = waitForResident(*cache, s);
        check(resident.state == MathRasterCache::Residency::Resident && resident.frames &&
                  resident.frames->frames.size() == 8 && resident.frames->width == 64 &&
                  resident.frames->height == 8 && cache->residentBytes() == 4096,
              "preview が要求すると 8 枚の mask (64x8、4096 byte) を memory に読む");
        // frame i は左から 64 * i / 8 = 8i 列 (偽の backend の定義から手で計算)。
        bool coded = resident.frames && resident.frames->frames.size() == 8;
        for (std::size_t index = 0; coded && index < 8; ++index) {
            const auto& frame = resident.frames->frames[index];
            coded = frame.size() == 64U * 8U;
            for (int x = 0; coded && x < 64; ++x)
                coded = frame[static_cast<std::size_t>(x)] ==
                        (x < static_cast<int>(8 * index) ? 255 : 0);
        }
        check(coded, "mask は frame の順に PNG の alpha を 1 byte ずつ持つ");
        check(*backend.sequenceRenders == 1 && *backend.renders == 0,
              "連番だけを 1 回描く (静止の描画は要求しない)");
        const auto artifact = cache->readySequence(s);
        check(artifact && artifact->frames.size() == 8 &&
                  artifact->frames.front() ==
                      directory / L"write" / key.toStdWString() / L"00000.png" &&
                  std::filesystem::is_regular_file(artifact->frames.back()),
              "PNG を write/<key>/00000.png から置く");
        check(
            std::filesystem::is_regular_file(directory / L"write" / (key.toStdWString() + L".txt")),
            "provenance を write/<key>.txt に置く");
        check(cache->readyArtifact(s.still) == std::nullopt, "連番は静止の artifact にならない");
    }
    {
        auto cache = readyCache(directory, backend);
        const auto entry = waitForSequence(*cache, s);
        check(entry.state == MathRasterCache::State::Ready && *backend.sequenceRenders == 1,
              "開き直した instance は disk の連番を描かずに読む");
    }
    {
        // frame を 1 枚消す: provenance の枚数と合わないので描き直す。
        std::filesystem::remove(directory / L"write" / key.toStdWString() / L"00005.png");
        auto cache = readyCache(directory, backend);
        const auto entry = waitForSequence(*cache, s);
        check(entry.state == MathRasterCache::State::Ready && *backend.sequenceRenders == 2,
              "欠けた frame がある連番は使わずに描き直す");
    }
    {
        // frame の中身を壊す (大きさも変わる): provenance の大きさと合わないので描き直す。
        std::ofstream(directory / L"write" / key.toStdWString() / L"00002.png", std::ios::binary)
            << "壊れた";
        auto cache = readyCache(directory, backend);
        waitForSequence(*cache, s);
        check(*backend.sequenceRenders == 3, "壊れた frame がある連番は描き直す");
    }
    {
        // provenance が無い (書く前に止まった) 結果は使わない。
        std::filesystem::remove(directory / L"write" / (key.toStdWString() + L".txt"));
        auto cache = readyCache(directory, backend);
        waitForSequence(*cache, s);
        check(*backend.sequenceRenders == 4, "provenance の無い連番は使わずに描き直す");
        const auto text = readText(directory / L"write" / (key.toStdWString() + L".txt"));
        check(text.rfind("mvm-math-sequence-artifact/1\n", 0) == 0 &&
                  text.find("\nframes=8\n") != std::string::npos &&
                  text.find("\nsequence_template=fake-write/1\n") != std::string::npos,
              "provenance に版・枚数・連番の script を書く");
    }
    {
        FakeBackend changed;
        changed.sequenceTemplate = "fake-write/2";
        changed.sequenceRenders = backend.sequenceRenders;
        changed.renders = backend.renders;
        auto cache = readyCache(directory, changed);
        check(cache->sequenceKeyFor(s) != key, "連番の script が変われば key が変わる");
        waitForSequence(*cache, s);
        waitForResult(*cache, s.still);
        check(*backend.sequenceRenders == 5, "連番の script が変われば描き直す");
        check(cache->keyFor(s.still) == readyCache(directory, backend)->keyFor(s.still),
              "連番の script が変わっても静止の key は変わらない");
    }
}

void testSequenceFailuresAndCancel(const std::filesystem::path& root) {
    FakeBackend backend;
    auto cache = readyCache(root / L"sequence failures", backend);
    const auto bad = waitForSequence(*cache, writeSpec("BAD", 4));
    check(bad.state == MathRasterCache::State::Failed &&
              bad.status == math::MathRenderStatus::InvalidSource,
          "連番の式の誤りは Failed");
    const int afterFailure = *backend.sequenceRenders;
    cache->requestSequence(writeSpec("BAD", 4));
    waitUntil([] { return false; }, 100);
    check(*backend.sequenceRenders == afterFailure, "失敗した連番は自動では描き直さない");
    cache->forgetFailures();
    waitForSequence(*cache, writeSpec("BAD", 4));
    check(*backend.sequenceRenders == afterFailure + 1, "forgetFailures で連番の失敗も忘れる");

    // 長い連番を描いている間に入力中の式の静止が来たら、連番を止めて静止を先に描く。
    check(cache->requestSequence(writeSpec("SLOW_WRITE", 4)).state ==
              MathRasterCache::State::Pending,
          "連番を描き始める");
    check(waitUntil([&] { return backend.slowStarted->load(); }), "連番の描画が始まる");
    cache->cancelPendingAnimations();
    check(cache->sequenceRecordCount() == 1, "描き終えていない連番の要求だけを忘れる (BAD は残る)");
    const auto typed = waitForResult(*cache, spec("typed"));
    check(*backend.slowSawCancel && typed.state == MathRasterCache::State::Ready,
          "取り消した連番は止まり、静止が描ける");
    backend.slowStarted->store(false);
    backend.slowSawCancel->store(false);
    check(cache->requestSequence(writeSpec("SLOW_WRITE", 4)).state ==
              MathRasterCache::State::Pending,
          "取り消した連番は次の要求で要求し直される");
    check(waitUntil([&] { return backend.slowStarted->load(); }), "要求し直した連番が始まる");

    // retainOnly は連番の key にも効く。
    cache->retainOnly({cache->keyFor(spec("typed"))});
    check(waitUntil([&] { return backend.slowSawCancel->load(); }),
          "要求されなくなった連番は cancel で止まる");
    check(cache->sequenceRecordCount() == 0 && cache->recordCount() == 1,
          "要求されなくなった連番の record を残さない");
    check(!std::filesystem::exists(
              root / L"sequence failures" / L"write" /
              (cache->sequenceKeyFor(writeSpec("SLOW_WRITE", 4)).toStdWString())),
          "取り消した連番の directory を作らない");
}

// preview 用の mask は全 clip の合計で上限 (residency) を守る。disk の連番 (書き出し) は
// memory の上限と無関係に Ready のまま。各連番は 64 x 8 x 8 = 4096 byte、上限は 10000 byte
// (2 本まで)。
void testSequenceResidency(const std::filesystem::path& root) {
    const auto directory = root / L"sequence residency";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    cache->setResidentMemoryBudget(10000);
    std::vector<math::MathSequenceSpec> specs;
    for (const char* name : {"WIDE a", "WIDE b", "WIDE c", "WIDE d", "WIDE e", "WIDE f"})
        specs.push_back(writeSpec(name, 8));
    check(cache->residentSequence(specs[0]).state == MathRasterCache::Residency::NotReady,
          "disk の連番が揃う前は memory に読まない");
    for (const auto& s : specs) {
        const auto entry = waitForSequence(*cache, s);
        check(entry.state == MathRasterCache::State::Ready,
              "6 本の連番がすべて disk に揃う: " + s.still.source +
                  " state=" + std::to_string(static_cast<int>(entry.state)) + " " +
                  entry.message.toStdString());
    }
    check(cache->residentBytes() == 0, "6 本が disk に揃っても memory には 1 本も読まない");

    std::size_t peak = 0;
    const auto use = [&](const math::MathSequenceSpec& s) {
        const auto result = waitForResident(*cache, s);
        peak = std::max(peak, cache->residentBytes());
        return result;
    };
    check(use(specs[0]).state == MathRasterCache::Residency::Resident &&
              use(specs[1]).state == MathRasterCache::Residency::Resident &&
              cache->residentBytes() == 8192 && cache->residentLoadCount() == 2,
          "2 本 (8192 byte) は上限に収まり memory に置く");
    check(use(specs[2]).state == MathRasterCache::Residency::Resident &&
              cache->residentBytes() == 8192 && cache->residentSequenceCount() == 2,
          "3 本目は最も長く使っていない a を追い出して置く (合計は 8192 byte のまま)");
    check(cache->residencyOf(specs[0]).state != MathRasterCache::Residency::Resident &&
              cache->residencyOf(specs[1]).state == MathRasterCache::Residency::Resident,
          "追い出すのは最も長く使っていない mask (LRU)");
    // b を使い直してから a を読み直すと、追い出されるのは c。
    use(specs[1]);
    check(use(specs[0]).state == MathRasterCache::Residency::Resident &&
              cache->residentLoadCount() == 4 &&
              cache->residencyOf(specs[2]).state != MathRasterCache::Residency::Resident,
          "追い出した a は disk から読み直し、そのとき最も古い c を追い出す");
    for (const auto& s : specs)
        use(s);
    check(peak <= 10000 && cache->residentBytes() <= 10000 && cache->residentSequenceCount() == 2,
          "6 本を順に使っても memory の合計は上限 (10000 byte) を超えない (最大 " +
              std::to_string(peak) + " byte)");

    // preview engine が mask を持ち続けている間は、cache から外しても memory
    // に残るので上限に数える。
    auto heldE = cache->residentSequence(specs[4]).frames;
    auto heldF = cache->residentSequence(specs[5]).frames;
    check(heldE && heldF, "e と f を使用中として持つ");
    const auto refused = cache->residentSequence(specs[0]);
    check(refused.state == MathRasterCache::Residency::OverBudget &&
              refused.message.contains(QStringLiteral("memory")) && cache->residentBytes() == 8192,
          "使用中の mask で上限が埋まっていれば新しい mask は読まない (上限を超えない)");
    check(cache->residencyOf(specs[0]).state == MathRasterCache::Residency::OverBudget,
          "上限に収まらなかったことを inspector に返せる");
    check(cache->readySequence(specs[0]).has_value(),
          "上限に収まらなくても disk の連番は Ready で、書き出しに使える");
    check(cache->residentSequenceCount() == 2,
          "使用中の mask は追い出さない (外しても memory は空かず、読み直しになるだけ)");
    heldE.reset();
    check(use(specs[0]).state == MathRasterCache::Residency::Resident &&
              cache->residentBytes() == 8192 &&
              cache->residencyOf(specs[4]).state != MathRasterCache::Residency::Resident,
          "使われなくなった e を追い出し、a を memory に置ける");
    heldF.reset();

    // cache が手放した後も preview が持っている mask: 手放されたら、収まらなかった連番を
    // もう一度試させる (entryChanged)。
    auto heldA = cache->residentSequence(specs[0]).frames;
    auto heldF2 = cache->residentSequence(specs[5]).frames;
    check(heldA && heldF2, "a と f を使用中として持つ");
    cache->retainOnly({cache->sequenceKeyFor(specs[0]), cache->sequenceKeyFor(specs[1])});
    check(cache->residentBytes() == 8192 && cache->residentSequenceCount() == 1,
          "要求されなくなった f は cache から外れても、使用中なので memory に残る");
    check(cache->residentSequence(specs[1]).state == MathRasterCache::Residency::OverBudget,
          "使用中の a と f で上限が埋まり、b は収まらない");
    QString retried;
    QObject::connect(cache.get(), &MathRasterCache::entryChanged,
                     [&](const QString& key) { retried = key; });
    const auto keyB = cache->sequenceKeyFor(specs[1]);
    heldF2.reset();
    check(waitUntil([&] { return retried == keyB; }),
          "使用中の mask が手放されると、収まらなかった連番をもう一度試させる");
    check(cache->residentBytes() == 4096 &&
              use(specs[1]).state == MathRasterCache::Residency::Resident &&
              cache->residentBytes() == 8192,
          "手放された分で b を memory に置ける");
    heldA.reset();

    // 1 本で上限を超える連番は preview に置かず、書き出しには使える。
    cache->setResidentMemoryBudget(4095);
    check(cache->residentSequence(specs[1]).state == MathRasterCache::Residency::OverBudget &&
              cache->readySequence(specs[1]).has_value() && cache->residentBytes() == 0,
          "上限より大きい連番は memory に読まず、disk の連番は使える");
    cache->setResidentMemoryBudget(4096);
    check(use(specs[1]).state == MathRasterCache::Residency::Resident,
          "対照: 上限ちょうどの連番は memory に置ける");

    // retainOnly は memory の mask も手放す。
    cache->retainOnly({});
    check(cache->residentSequenceCount() == 0 && cache->residentBytes() == 0,
          "要求されなくなった連番の mask を手放す");
}

// disk の大きさは合うが中身が壊れた frame: preview が読むときに見つけ、artifact を消して
// 連番を Failed にする。forgetFailures の後は描き直す。
void testSequenceCorruptResident(const std::filesystem::path& root) {
    const auto directory = root / L"sequence corrupt";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    const auto s = writeSpec("WIDE corrupt", 8);
    check(waitForSequence(*cache, s).state == MathRasterCache::State::Ready, "連番が揃う");
    const auto frame =
        directory / L"write" / cache->sequenceKeyFor(s).toStdWString() / L"00003.png";
    const auto size = std::filesystem::file_size(frame);
    std::ofstream(frame, std::ios::binary) << std::string(static_cast<std::size_t>(size), 'x');
    check(std::filesystem::file_size(frame) == size, "負例の準備: 同じ大きさで中身を壊す");
    const auto broken = waitForResident(*cache, s);
    check(broken.state != MathRasterCache::Residency::Resident && cache->residentBytes() == 0,
          "壊れた frame の連番は memory に置かない (予約も返す)");
    check(cache->requestSequence(s).state == MathRasterCache::State::Failed &&
              !cache->readySequence(s) &&
              !std::filesystem::exists(directory / L"write" /
                                       (cache->sequenceKeyFor(s).toStdWString() + L".txt")),
          "連番を Failed にして artifact を消す (書き出しにも渡さない)");
    cache->forgetFailures();
    check(waitForSequence(*cache, s).state == MathRasterCache::State::Ready &&
              *backend.sequenceRenders == 2 &&
              waitForResident(*cache, s).state == MathRasterCache::Residency::Resident,
          "forgetFailures の後は描き直して読める");
}

// backend が描けない長さは、描かずに未対応として失敗する (Project の値は正しいまま)。
void testSequenceBackendCapability(const std::filesystem::path& root) {
    FakeBackend backend;
    backend.maximumSequenceFrames = 4;
    auto cache = readyCache(root / L"sequence capability", backend);
    const auto entry = waitForSequence(*cache, writeSpec("WIDE long", 8));
    check(entry.state == MathRasterCache::State::Failed &&
              entry.message.contains(QStringLiteral("4 frame")) &&
              entry.message.contains(QStringLiteral("8 frame")) && *backend.sequenceRenders == 0,
          "backend の上限 (4) を超える 8 frame の Write は描かずに理由付きで失敗する");
    check(waitForSequence(*cache, writeSpec("WIDE long", 4)).state == MathRasterCache::State::Ready,
          "対照: 上限ちょうどの Write は描ける");
}

void testSequenceWithoutBackendSupport(const std::filesystem::path& root) {
    FakeBackend backend;
    backend.withSequence = false;
    auto cache = readyCache(root / L"sequence unsupported", backend);
    const auto entry = waitForSequence(*cache, writeSpec("x", 4));
    check(entry.state == MathRasterCache::State::Failed && !cache->readySequence(writeSpec("x", 4)),
          "連番を描けない backend では Failed (静止で代用しない)");
}

// ---- 式から式への変形 (P2-4) ----
//
// 期待値は偽の backend の定義から手で数えた値 (実装の関数を呼ばない)。
//   source "x" は 3x2 (alpha 255 128 0 / 64 255 32)、target "WIDE ..." は 64x8 (全画素 255)。
//   canvas は 64+12 x 8+12 = 76x20。source の左上 ((76-3)/2, (20-2)/2) = (36, 9)、
//   target の左上 ((76-64)/2, (20-8)/2) = (6, 6)。frame 1 以降に canvas の (2, 3) の 1 画素。
//   artifact = x 2..70、y 3..14 → (2, 3, 68, 11)。
//   切り出した座標の端点: source (36-2, 9-3) = (34, 6)、target (6-2, 6-3) = (4, 3)。
constexpr int kCropWidth = 68;
constexpr int kCropHeight = 11;
constexpr int kSourceX = 34;
constexpr int kSourceY = 6;
constexpr int kTargetX = 4;
constexpr int kTargetY = 3;
constexpr std::size_t kCropBytes = static_cast<std::size_t>(kCropWidth) * kCropHeight; // 748

math::MathTransformSpec transformSpec(const std::string& source, const std::string& target,
                                      std::int64_t frames = 4) {
    return {spec(source), spec(target), frames};
}

MathRasterCache::TransformEntry waitForTransform(MathRasterCache& cache,
                                                 const math::MathTransformSpec& s,
                                                 int milliseconds = 10000) {
    auto entry = cache.requestTransform(s);
    waitUntil(
        [&] {
            entry = cache.requestTransform(s);
            return entry.state != MathRasterCache::State::Pending;
        },
        milliseconds);
    return entry;
}

MathRasterCache::ResidentSequence waitForResidentTransform(MathRasterCache& cache,
                                                           const math::MathTransformSpec& s) {
    auto result = cache.residentTransform(s);
    waitUntil([&] {
        result = cache.residentTransform(s);
        return result.state != MathRasterCache::Residency::Loading;
    });
    return result;
}

std::filesystem::path transformProvenanceOf(const std::filesystem::path& directory,
                                            const QString& key) {
    return directory / L"transform" / (key.toStdWString() + L".txt");
}

std::filesystem::path transformFrameOf(const std::filesystem::path& directory, const QString& key,
                                       int index) {
    wchar_t name[32] = {};
    std::swprintf(name, std::size(name), L"%05d.a8", index);
    return directory / L"transform" / key.toStdWString() / name;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    const std::string text = readText(path);
    return {text.begin(), text.end()};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// 切り出した frame 0 の期待値: source "x" の 3x2 を (34, 6) に置き、他は 0。
std::vector<std::uint8_t> expectedFrame0() {
    std::vector<std::uint8_t> frame(kCropBytes, 0);
    const std::uint8_t glyph[2][3] = {{255, 128, 0}, {64, 255, 32}};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            frame[mvm::test::coverageIndex(kCropWidth, kSourceX + x, kSourceY + y)] = glyph[y][x];
    return frame;
}

// 切り出した frame 1 以降の期待値: target の 64x8 (255) を (4, 3) に置き、(0, 0) に 200。
std::vector<std::uint8_t> expectedLaterFrame() {
    std::vector<std::uint8_t> frame(kCropBytes, 0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 64; ++x)
            frame[mvm::test::coverageIndex(kCropWidth, kTargetX + x, kTargetY + y)] = 255;
    frame[0] = 200;
    return frame;
}

std::vector<std::uint8_t> alphaOf(const mvm::media::StillImage& image) {
    std::vector<std::uint8_t> alpha;
    for (std::size_t at = 3; at < image.rgba.size(); at += 4)
        alpha.push_back(image.rgba[at]);
    return alpha;
}

// 両端の今の静止が揃うまで変形を描かない。揃ったら、その静止の mask をそのまま渡す。
void testTransformWaitsForEndpoints(const std::filesystem::path& root) {
    FakeBackend backend;
    backend.staticGate->store(true);
    auto cache = readyCache(root / L"transform wait", backend);
    const auto s = transformSpec("x", "WIDE GATE t");
    check(cache->requestTransform(s).state == MathRasterCache::State::Pending,
          "変形は Pending から始まる");
    check(waitForResult(*cache, spec("x")).state == MathRasterCache::State::Ready,
          "変形前の静止は描ける");
    waitUntil([] { return false; }, 200);
    check(cache->requestTransform(s).state == MathRasterCache::State::Pending &&
              *backend.transformRenders == 0,
          "変形後の静止が描けるまで変形を描かない (片方だけでは始めない)");
    backend.staticGate->store(false);
    const auto entry = waitForTransform(*cache, s);
    check(entry.state == MathRasterCache::State::Ready && entry.width == kCropWidth &&
              entry.height == kCropHeight && *backend.transformRenders == 1,
          "両端が揃うと 1 回だけ描き、切り出した大きさ (68x11) で Ready: " +
              entry.message.toStdString());
    const auto source = cache->request(spec("x"));
    const auto target = cache->request(spec("WIDE GATE t"));
    std::lock_guard lock(backend.transformLog->mutex);
    const auto& received = backend.transformLog->received;
    check(received.size() == 1 && source.mask && target.mask && received[0].first.width == 3 &&
              received[0].first.height == 2 && received[0].first.alpha == alphaOf(*source.mask) &&
              received[0].second.width == 64 && received[0].second.height == 8 &&
              received[0].second.alpha == alphaOf(*target.mask),
          "backend へ渡す mask は両端の今の静止の artifact そのもの");
}

// 今の端点の静止が描けなければ、前に描けた別の式の静止で代用しない。
void testTransformNoLastGoodEndpoint(const std::filesystem::path& root) {
    FakeBackend backend;
    auto cache = readyCache(root / L"transform no last good", backend);
    const auto good = transformSpec("x", "WIDE t");
    check(waitForTransform(*cache, good).state == MathRasterCache::State::Ready,
          "準備: 元の変形が Ready");
    const int before = *backend.transformRenders;
    const auto edited = transformSpec("x", "BAD");
    check(cache->transformKeyFor(edited) != cache->transformKeyFor(good),
          "端点の式を変えると変形の key が変わる");
    const auto failed = waitForTransform(*cache, edited);
    check(failed.state == MathRasterCache::State::Failed &&
              failed.status == math::MathRenderStatus::InvalidSource &&
              failed.message.contains(QStringLiteral("変形後")) &&
              failed.message.contains(QStringLiteral("Undefined control sequence.")),
          "変形後の静止の誤りは、変形を理由付きの Failed にする: " + failed.message.toStdString());
    check(*backend.transformRenders == before && !cache->readyTransform(edited),
          "描けた前の式 (WIDE t) の静止で代用して描かない");
    check(cache->readyTransform(good).has_value() &&
              cache->transformKeyFor(good) != cache->transformKeyFor(edited),
          "古い key の artifact は残るが、新しい key の要求を満たさない");
    const auto gone = waitForTransform(*cache, transformSpec("GONE", "WIDE t"));
    check(gone.state == MathRasterCache::State::Unavailable && *backend.transformRenders == before,
          "変形前の静止が Unavailable なら変形も Unavailable (描かない)");

    // 片方が描画中の間は、同じ大きさの静止が既にあっても始めない。
    backend.staticGate->store(true);
    const auto waiting = transformSpec("x", "WIDE GATE u");
    cache->requestTransform(waiting);
    waitUntil([] { return false; }, 200);
    check(cache->requestTransform(waiting).state == MathRasterCache::State::Pending &&
              *backend.transformRenders == before,
          "変形後の静止が Pending の間は、古い静止があっても変形を描かない");
    backend.staticGate->store(false);
    check(waitForTransform(*cache, waiting).state == MathRasterCache::State::Ready &&
              *backend.transformRenders == before + 1,
          "対照: 今の静止が描けると変形を描く");
}

// 描画中に端点を書き換える: 新しい key は古い描画の結果を受け取らない。
void testTransformEndpointEditInFlight(const std::filesystem::path& root) {
    const auto directory = root / L"transform edit";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    const auto oldSpec = transformSpec("x", "WIDE t");
    const auto newSpec = transformSpec("x", "WIDE t2");
    const auto oldKey = cache->transformKeyFor(oldSpec);
    const auto newKey = cache->transformKeyFor(newSpec);
    check(oldKey != newKey, "端点を変えると key が変わる");

    // (a) 古い要求を残したまま: 古い結果は古い key にだけ入る。
    backend.transformGate->store(true);
    cache->requestTransform(oldSpec);
    check(waitUntil([&] { return backend.transformHeld->load(); }), "古い変形の描画が始まる");
    cache->requestTransform(newSpec);
    backend.transformGate->store(false);
    check(waitForTransform(*cache, newSpec).state == MathRasterCache::State::Ready &&
              waitForTransform(*cache, oldSpec).state == MathRasterCache::State::Ready &&
              *backend.transformRenders == 2,
          "古い変形と新しい変形はそれぞれ 1 回ずつ描く");
    const auto oldText = readText(transformProvenanceOf(directory, oldKey));
    const auto newText = readText(transformProvenanceOf(directory, newKey));
    const auto targetKeyLine = [&](const std::string& source) {
        return "\ntarget_static_key=" + cache->keyFor(spec(source)).toStdString() + "\n";
    };
    check(oldText.find("\nkey=" + oldKey.toStdString() + "\n") != std::string::npos &&
              oldText.find(targetKeyLine("WIDE t")) != std::string::npos &&
              newText.find("\nkey=" + newKey.toStdString() + "\n") != std::string::npos &&
              newText.find(targetKeyLine("WIDE t2")) != std::string::npos &&
              newText.find(targetKeyLine("WIDE t")) == std::string::npos,
          "各 key の provenance はその key の端点の静止だけを記録する (古い A と新しい B "
          "を混ぜない)");

    // (b) 古い要求を取り下げる (controller の retainOnly): 古い描画の結果は確定しない。
    backend.transformHeld->store(false);
    backend.transformGate->store(true);
    const auto staleSpec = transformSpec("x", "WIDE t3");
    const auto staleKey = cache->transformKeyFor(staleSpec);
    cache->requestTransform(staleSpec);
    check(waitUntil([&] { return backend.transformHeld->load(); }), "取り下げる変形の描画が始まる");
    cache->retainOnly({newKey, cache->keyFor(spec("x")), cache->keyFor(spec("WIDE t2"))});
    backend.transformGate->store(false);
    waitUntil([] { return false; }, 300);
    check(cache->transformRecordCount() == 1 && !cache->readyTransform(staleSpec) &&
              cache->readyTransform(newSpec).has_value(),
          "取り下げた変形の結果を record に残さない (残すのは要求中の新しい変形だけ)");
    check(!std::filesystem::exists(transformProvenanceOf(directory, staleKey)),
          "取り下げた (取り消した) 変形の provenance を書かない");
}

// 入力中の式の静止を優先するための取消 (cancelPendingAnimations、P2-5.1): 描き終えていない変形
// だけを止めて忘れる。Ready の変形の disk の artifact と memory の mask は消さない。取り消した
// 変形は要求し直すと描ける。
void testCancelPendingAnimationsKeepsReady(const std::filesystem::path& root) {
    const auto directory = root / L"transform cancel pending";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    const auto ready = transformSpec("x", "WIDE t");
    check(waitForTransform(*cache, ready).state == MathRasterCache::State::Ready &&
              waitForResidentTransform(*cache, ready).state == MathRasterCache::Residency::Resident,
          "取消: Ready の変形を disk と memory に置く");
    const auto readyKey = cache->transformKeyFor(ready);

    backend.transformCancellableGate->store(true);
    const auto pending = transformSpec("x", "WIDE held");
    const auto pendingKey = cache->transformKeyFor(pending);
    cache->requestTransform(pending);
    check(waitUntil([&] { return backend.transformHeld->load(); }), "取消: 変形の描画が始まる");
    const int rendersBefore = *backend.transformRenders;
    cache->cancelPendingAnimations();
    check(waitUntil([&] { return backend.transformSawCancel->load(); }),
          "取消: 描画中の変形は取消を受け取って止まる");
    waitUntil([] { return false; }, 200);
    check(cache->transformRecordCount() == 1 && !cache->readyTransform(pending) &&
              !std::filesystem::exists(transformProvenanceOf(directory, pendingKey)),
          "取消: 描き終えていない変形を忘れ、結果を確定しない");
    check(cache->readyTransform(ready).has_value() &&
              std::filesystem::exists(transformProvenanceOf(directory, readyKey)) &&
              std::filesystem::exists(transformFrameOf(directory, readyKey, 0)) &&
              cache->transformResidencyOf(ready).state == MathRasterCache::Residency::Resident,
          "取消: Ready の変形の disk の artifact と memory の mask は残す");

    backend.transformCancellableGate->store(false);
    check(waitForTransform(*cache, pending).state == MathRasterCache::State::Ready &&
              *backend.transformRenders == rendersBefore + 1,
          "取消: 取り消した変形は要求し直すと描き直して Ready になる");
}

// 権限・世代が変わった後に、古い描画を確定させない。権限が無ければ何もしない。
void testTransformAuthority(const std::filesystem::path& root) {
    const auto directory = root / L"transform authority";
    {
        // (a) 描画中に権限を失う。
        FakeBackend backend;
        auto cache = readyCache(directory, backend);
        const auto s = transformSpec("x", "WIDE t");
        const auto key = cache->transformKeyFor(s);
        backend.transformGate->store(true);
        cache->requestTransform(s);
        check(waitUntil([&] { return backend.transformHeld->load(); }), "描画が始まる");
        cache->setAuthority(directory, false, QStringLiteral("他のプロセスが編集中です"));
        backend.transformGate->store(false);
        waitUntil([] { return false; }, 300);
        check(!std::filesystem::exists(transformProvenanceOf(directory, key)) &&
                  !std::filesystem::exists(directory / L"transform" / key.toStdWString()),
              "権限を失った後は、描き終えた変形を書かない (provenance も frame も)");
        check(cache->requestTransform(s).state == MathRasterCache::State::Unavailable &&
                  cache->transformRecordCount() == 0,
              "権限が無ければ Unavailable で、古い世代の record を残さない");

        // (b) 権限はあるまま世代だけが変わる (setAuthority のやり直し)。
        cache->setAuthority(directory, true);
        waitUntil(
            [&] { return cache->backendState() == MathRasterCache::BackendState::Available; });
        backend.transformHeld->store(false);
        backend.transformGate->store(true);
        const int before = *backend.transformRenders;
        cache->requestTransform(s);
        check(waitUntil([&] { return backend.transformHeld->load(); }), "次の世代で描画が始まる");
        cache->setAuthority(directory, true);
        backend.transformGate->store(false);
        waitUntil([] { return false; }, 300);
        check(!std::filesystem::exists(transformProvenanceOf(directory, key)),
              "世代が変わる前の描画は確定しない");
        waitUntil(
            [&] { return cache->backendState() == MathRasterCache::BackendState::Available; });
        check(waitForTransform(*cache, s).state == MathRasterCache::State::Ready &&
                  *backend.transformRenders == before + 2 &&
                  std::filesystem::exists(transformProvenanceOf(directory, key)),
              "対照: 今の世代の要求は描いて確定する");
    }
    {
        // (c) provenance を書く直前に権限を失う: frame は揃っていても確定しない。
        //     このとき provenance はまだ無く (provenance が最後)、frame は全部書けている。
        const auto raced = root / L"transform publish race";
        FakeBackend backend;
        auto cache = readyCache(raced, backend);
        const auto s = transformSpec("x", "WIDE t");
        const auto key = cache->transformKeyFor(s);
        auto atPublish = std::make_shared<std::atomic<bool>>(false);
        auto proceed = std::make_shared<std::atomic<bool>>(false);
        auto provenanceSeen = std::make_shared<std::atomic<bool>>(true);
        auto framesSeen = std::make_shared<std::atomic<int>>(0);
        cache->setBeforeTransformPublishForTest([=](const std::filesystem::path& provenance) {
            provenanceSeen->store(std::filesystem::exists(provenance));
            int frames = 0;
            for (int index = 0; index < 4; ++index)
                frames += std::filesystem::is_regular_file(transformFrameOf(raced, key, index));
            framesSeen->store(frames);
            atPublish->store(true);
            for (int i = 0; i < 3000 && !proceed->load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
        cache->requestTransform(s);
        check(waitUntil([&] { return atPublish->load(); }), "provenance を書く直前に着く");
        check(!provenanceSeen->load() && *framesSeen == 4,
              "provenance を書く前に 4 枚の frame がすべて揃っている (provenance が最後)");
        cache->setAuthority(raced, false, QStringLiteral("他のプロセスが編集中です"));
        proceed->store(true);
        waitUntil([] { return false; }, 300);
        check(!std::filesystem::exists(transformProvenanceOf(raced, key)),
              "provenance を書く直前に権限を失えば確定しない");
        check(std::filesystem::is_regular_file(transformFrameOf(raced, key, 0)),
              "権限を失った後は書きかけの frame も消さない (cache directory を変えない)");

        // provenance の無い frame は未完了。次の instance は使わずに描き直す。
        FakeBackend next;
        next.transformRenders = backend.transformRenders;
        auto reopened = readyCache(raced, next);
        check(waitForTransform(*reopened, s).state == MathRasterCache::State::Ready &&
                  *backend.transformRenders == 2,
              "provenance の無い (未完了の) 変形は使わずに描き直す");
    }
    {
        // (d) 最初から権限が無い: 描かず、確定せず、他の instance の作業も掃除しない。
        const auto foreign = root / L"transform foreign";
        const auto foreignFrames = foreign / L"transform" / L"other-key";
        std::filesystem::create_directories(foreignFrames);
        writeText(foreignFrames / L"00000.a8", "partial");
        writeText(foreign / L"transform" / L"other-key.txt", "partial");
        FakeBackend backend;
        MathRasterCache cache("session-z", backend.preflight());
        cache.setAuthority(foreign, false, QStringLiteral("他のプロセスが編集中です"));
        const auto entry = cache.requestTransform(transformSpec("x", "WIDE t"));
        waitUntil([] { return false; }, 200);
        check(entry.state == MathRasterCache::State::Unavailable &&
                  *backend.transformRenders == 0 && *backend.renders == 0,
              "権限が無ければ変形も端点の静止も描かない");
        check(std::filesystem::is_regular_file(foreignFrames / L"00000.a8") &&
                  std::filesystem::is_regular_file(foreign / L"transform" / L"other-key.txt") &&
                  cache.transformRecordCount() == 0,
              "権限が無ければ他の instance の変形の file を消さない");
    }
}

// disk の形・provenance・切り出し、開き直し、壊れた artifact の拒否。
void testTransformDiskAndProvenance(const std::filesystem::path& root) {
    const auto directory = root / L"transform disk 日本語";
    FakeBackend backend;
    const auto s = transformSpec("x", "WIDE t");
    QString key;
    {
        auto cache = readyCache(directory, backend);
        key = cache->transformKeyFor(s);
        check(key.size() == 64 && key != cache->keyFor(s.source) &&
                  key != cache->sequenceKeyFor(writeSpec("x", 4)),
              "変形の key は静止・連番と別の 64 桁");
        const auto entry = waitForTransform(*cache, s);
        check(entry.state == MathRasterCache::State::Ready, "変形が Ready");
        check(cache->residentBytes() == 0 && cache->residentLoadCount() == 0,
              "disk の Ready は memory に読むことを意味しない");
        const auto artifact = cache->readyTransform(s);
        check(artifact && artifact->frames.size() == 4 && artifact->width == kCropWidth &&
                  artifact->height == kCropHeight && artifact->sourceX == kSourceX &&
                  artifact->sourceY == kSourceY && artifact->targetX == kTargetX &&
                  artifact->targetY == kTargetY &&
                  artifact->frames.front() == transformFrameOf(directory, key, 0),
              "artifact は 4 枚、68x11、端点は切り出した座標 (34,6)・(4,3)");
        bool sized = true;
        for (int index = 0; index < 4; ++index)
            sized = sized && std::filesystem::file_size(transformFrameOf(directory, key, index)) ==
                                 kCropBytes;
        check(sized && !std::filesystem::exists(transformFrameOf(directory, key, 4)),
              "各 frame は 748 byte (切り出した大きさ) で、終状態の照合の 1 枚は保存しない");
        check(readBytes(transformFrameOf(directory, key, 0)) == expectedFrame0(),
              "切り出した frame 0 は変形前の静止 (3x2) を (34, 6) に置いたものと全画素一致");
        check(readBytes(transformFrameOf(directory, key, 1)) == expectedLaterFrame() &&
                  readBytes(transformFrameOf(directory, key, 3)) == expectedLaterFrame(),
              "frame 1 以降は変形後の静止を (4, 3) に置き、(0, 0) に 1 画素 (端点の位置の換算)");
        const auto text = readText(transformProvenanceOf(directory, key));
        const auto has = [&](const std::string& line) {
            return text.find("\n" + line + "\n") != std::string::npos;
        };
        check(text.rfind("mvm-math-transform-artifact/1\n", 0) == 0 &&
                  has("key=" + key.toStdString()) && has("frames=4") && has("width=68") &&
                  has("height=11") && has("canvas=76x20") && has("artifact_rect=2,3,68,11") &&
                  has("source_offset=34,6") && has("target_offset=4,3") &&
                  has("source_static=3x2") && has("target_static=64x8") &&
                  has("source_static_key=" + cache->keyFor(spec("x")).toStdString()) &&
                  has("target_static_key=" + cache->keyFor(spec("WIDE t")).toStdString()) &&
                  has("transform_template=fake-transform/1") &&
                  has("segmenter=mvm-tex-segments/1") && has("matching=mvm-tex-match/1") &&
                  text.find("\ntoolchain:\nbackend=fake\nversion=1\n") != std::string::npos,
              "provenance に版・key・大きさ・canvas・矩形・端点・静止の key・script・規則の版・"
              "toolchain を書く:\n" +
                  text);
        check(text.find("\nframe=00000.a8 748 ") != std::string::npos &&
                  text.find("\nframe=00003.a8 748 ") != std::string::npos,
              "provenance に各 frame の名前・byte 数・SHA-256 を順に書く");
        std::vector<std::uint8_t> loaded;
        std::string error;
        check(artifact && mvm::app::loadMathTransformFrame(*artifact, 0, loaded, error) &&
                  loaded == expectedFrame0(),
              "書き出し用の読み込み (loadMathTransformFrame) で frame 0 を読める");
    }
    const auto reopenReady = [&](const std::string& what, int expectedRenders) {
        auto cache = readyCache(directory, backend);
        const auto entry = waitForTransform(*cache, s);
        check(entry.state == MathRasterCache::State::Ready &&
                  *backend.transformRenders == expectedRenders,
              what + " (変形の描画 " + std::to_string(backend.transformRenders->load()) + " 回)");
        return entry;
    };
    const int staticRenders = *backend.renders;
    reopenReady("開き直した instance は disk の変形を描かずに読む", 1);
    check(*backend.renders == staticRenders, "開き直しでは端点の静止も disk から読む");

    const auto provenance = transformProvenanceOf(directory, key);
    std::filesystem::remove(provenance);
    int renders = 2;
    reopenReady("provenance の無い変形は未完了として描き直す", renders);

    std::string text = readText(provenance);
    std::string changed = text;
    changed.replace(changed.find("source_offset=34,6"), 18, "source_offset=34,7");
    writeText(provenance, changed);
    reopenReady("変形前の端点の位置を書き換えた provenance は使わない", ++renders);

    changed = text;
    // 切り出した矩形には収まる位置 (3,3) にずらす: 配置の契約だけが見つけられる。
    changed.replace(changed.find("target_offset=4,3"), 17, "target_offset=3,3");
    writeText(provenance, changed);
    reopenReady("変形後の端点の位置を書き換えた provenance は使わない", ++renders);

    writeText(provenance, text.substr(0, text.size() / 2));
    reopenReady("途中で切れた provenance は使わない", ++renders);

    writeText(provenance, text + "extra\n");
    reopenReady("末尾に余分な行がある provenance は使わない", ++renders);

    changed = text;
    changed.replace(changed.find("width=68"), 8, "width=068");
    writeText(provenance, changed);
    reopenReady("正準形でない数値の provenance は使わない", ++renders);

    // 同じ大きさで中身を壊した frame (SHA-256 で見つける)。
    writeText(transformFrameOf(directory, key, 2), std::string(kCropBytes, 'x'));
    reopenReady("中身の壊れた frame がある変形は Ready にせず描き直す", ++renders);

    writeText(transformFrameOf(directory, key, 1),
              readText(transformFrameOf(directory, key, 1)) + "x");
    reopenReady("大きさの違う frame がある変形は描き直す", ++renders);

    std::filesystem::remove(transformFrameOf(directory, key, 3));
    reopenReady("欠けた frame がある変形は描き直す", ++renders);

    // frame の順を入れ替える (中身はどれも正しい frame)。
    const auto frame0 = readText(transformFrameOf(directory, key, 0));
    const auto frame1 = readText(transformFrameOf(directory, key, 1));
    writeText(transformFrameOf(directory, key, 0), frame1);
    writeText(transformFrameOf(directory, key, 1), frame0);
    reopenReady("frame の順が入れ替わった変形は描き直す", ++renders);

    // 枚数を 3 に書き換え、4 枚目の行を消す (provenance 自体は正準形)。
    text = readText(provenance);
    changed = text;
    changed.replace(changed.find("\nframes=4\n"), 10, "\nframes=3\n");
    const auto last = changed.find("frame=00003.a8 ");
    changed.erase(last, changed.find('\n', last) + 1 - last);
    writeText(provenance, changed);
    reopenReady("枚数が要求と違う provenance は使わない", ++renders);

    // 対照: 何も壊していなければ描き直さない。
    reopenReady("対照: 壊していない変形は描き直さない", renders);
}

// cache の側の fail-closed: 切り出すと画素を失う矩形、frame 0 の不一致、backend の能力。
void testTransformValidation(const std::filesystem::path& root) {
    const auto directory = root / L"transform validation";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    const auto lie = transformSpec("LIE x", "WIDE t");
    const auto lied = waitForTransform(*cache, lie);
    check(
        lied.state == MathRasterCache::State::Failed &&
            lied.message.contains(QStringLiteral("artifact の矩形の外")) &&
            !std::filesystem::exists(transformProvenanceOf(directory, cache->transformKeyFor(lie))),
        "式の画素を含まない artifact の矩形は切り出さずに失敗 (provenance を書かない): " +
            lied.message.toStdString());
    const auto shifted = transformSpec("SHIFT x", "WIDE t");
    const auto shift = waitForTransform(*cache, shifted);
    check(shift.state == MathRasterCache::State::Failed &&
              shift.message.contains(QStringLiteral("一致しません")) &&
              !std::filesystem::exists(
                  transformProvenanceOf(directory, cache->transformKeyFor(shifted))),
          "切り出した frame 0 が変形前の静止と 1 画素でもずれれば失敗: " +
              shift.message.toStdString());
    const auto badt = waitForTransform(*cache, transformSpec("x", "WIDE BADT"));
    check(badt.state == MathRasterCache::State::Failed &&
              badt.status == math::MathRenderStatus::InvalidSource,
          "backend の変形の失敗は Failed");
    const int renders = *backend.transformRenders;
    cache->requestTransform(transformSpec("x", "WIDE BADT"));
    waitUntil([] { return false; }, 100);
    check(*backend.transformRenders == renders, "失敗した変形は自動では描き直さない");
    cache->forgetFailures();
    waitForTransform(*cache, transformSpec("x", "WIDE BADT"));
    check(*backend.transformRenders == renders + 1, "forgetFailures で変形の失敗も忘れる");

    FakeBackend limited;
    limited.maximumTransformFrames = 4;
    auto capped = readyCache(root / L"transform capability", limited);
    const auto tooLong = waitForTransform(*capped, transformSpec("x", "WIDE t", 8));
    check(tooLong.state == MathRasterCache::State::Failed &&
              tooLong.message.contains(QStringLiteral("4 frame")) &&
              tooLong.message.contains(QStringLiteral("8 frame")) &&
              *limited.transformRenders == 0 && *limited.renders == 0,
          "backend の上限 (4) を超える 8 frame の変形は描かずに理由付きで失敗 (Project の誤りに"
          "しない): " +
              tooLong.message.toStdString());
    check(waitForTransform(*capped, transformSpec("x", "WIDE t", 4)).state ==
              MathRasterCache::State::Ready,
          "対照: 上限ちょうどの変形は描ける");

    FakeBackend without;
    without.withTransform = false;
    auto unsupported = readyCache(root / L"transform unsupported", without);
    const auto none = waitForTransform(*unsupported, transformSpec("x", "WIDE t"));
    check(none.state == MathRasterCache::State::Failed &&
              none.message.contains(QStringLiteral("描けません")),
          "変形を描けない backend では Failed");
}

// 変形の preview 用の mask は Write と同じ全体の上限・予約・LRU を共有する。
// 変形 1 本 = 4 x 748 = 2992 byte、Write 1 本 = 64 x 8 x 8 = 4096 byte、上限 7100 byte。
void testTransformResidency(const std::filesystem::path& root) {
    const auto directory = root / L"transform residency";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    cache->setResidentMemoryBudget(7100);
    const auto t1 = transformSpec("x", "WIDE t1");
    const auto t2 = transformSpec("x", "WIDE t2");
    const auto w1 = writeSpec("WIDE w", 8);
    check(cache->residentTransform(t1).state == MathRasterCache::Residency::NotReady,
          "disk の変形が揃う前は memory に読まない");
    check(waitForTransform(*cache, t1).state == MathRasterCache::State::Ready &&
              waitForTransform(*cache, t2).state == MathRasterCache::State::Ready &&
              waitForSequence(*cache, w1).state == MathRasterCache::State::Ready,
          "変形 2 本と Write 1 本が disk に揃う");
    check(cache->residentBytes() == 0, "disk に揃っても memory には読まない");

    std::size_t peak = 0;
    const auto useT = [&](const math::MathTransformSpec& s) {
        const auto result = waitForResidentTransform(*cache, s);
        peak = std::max(peak, cache->residentBytes());
        return result;
    };
    const auto useW = [&](const math::MathSequenceSpec& s) {
        const auto result = waitForResident(*cache, s);
        peak = std::max(peak, cache->residentBytes());
        return result;
    };
    const int loads = cache->residentLoadCount();
    check(useW(w1).state == MathRasterCache::Residency::Resident &&
              useT(t1).state == MathRasterCache::Residency::Resident &&
              cache->residentBytes() == 4096 + 2992,
          "Write と変形の mask を同じ上限の中に置く (合計 7088 byte)");
    {
        // 検査の間だけ持つ (持ち続けると使用中になり、下の LRU の検査が変わる)。
        const auto resident = cache->residentTransform(t1);
        check(resident.frames && resident.frames->frames.size() == 4 &&
                  resident.frames->width == kCropWidth && resident.frames->height == kCropHeight &&
                  resident.frames->frames[0] == expectedFrame0() &&
                  resident.frames->frames[2] == expectedLaterFrame(),
              "memory の変形の mask は切り出した frame そのもの (frame 0 は静止と一致)");
    }
    check(useT(t2).state == MathRasterCache::Residency::Resident &&
              cache->residentBytes() == 2992 + 2992 &&
              cache->residencyOf(w1).state != MathRasterCache::Residency::Resident,
          "変形 t2 を置くために、最も長く使っていない Write を追い出す (同じ LRU)");
    check(useW(w1).state == MathRasterCache::Residency::Resident &&
              cache->transformResidencyOf(t1).state != MathRasterCache::Residency::Resident,
          "Write を読み直すときは最も古い変形 t1 を追い出す");
    const int renders = *backend.transformRenders;
    check(useT(t1).state == MathRasterCache::Residency::Resident &&
              *backend.transformRenders == renders && cache->residentLoadCount() == loads + 5,
          "追い出した変形は Manim を起動せず disk から読み直す");
    check(peak <= 7100, "Write と変形の合計は上限を超えない (最大 " + std::to_string(peak) + ")");

    // Write を使用中に持つと、変形は上限に収まらない。
    auto heldW = cache->residentSequence(w1).frames;
    auto heldT1 = cache->residentTransform(t1).frames;
    check(heldW && heldT1 && cache->residentBytes() == 7088, "Write と t1 を使用中として持つ");
    const auto refused = cache->residentTransform(t2);
    check(refused.state == MathRasterCache::Residency::OverBudget &&
              refused.message.contains(QStringLiteral("memory")) &&
              refused.message.contains(QStringLiteral("変形")) && cache->residentBytes() == 7088,
          "使用中の Write と変形で上限が埋まっていれば、変形は読まない: " +
              refused.message.toStdString());
    check(cache->transformResidencyOf(t2).state == MathRasterCache::Residency::OverBudget &&
              cache->requestTransform(t2).state == MathRasterCache::State::Ready &&
              cache->readyTransform(t2).has_value(),
          "上限に収まらなくても disk の変形は Ready のまま (書き出しに使える)");
    heldW.reset();
    heldT1.reset();

    // 逆: 変形 2 本を使用中に持つと、Write は上限に収まらない。
    check(useT(t2).state == MathRasterCache::Residency::Resident, "t2 を置く");
    auto heldT2 = cache->residentTransform(t2).frames;
    auto heldT1b = useT(t1).frames;
    check(heldT1b && heldT2 && cache->residentBytes() == 5984, "t1 と t2 を使用中として持つ");
    check(cache->residentSequence(w1).state == MathRasterCache::Residency::OverBudget &&
              cache->residentBytes() == 5984,
          "使用中の変形で上限が埋まっていれば、Write は読まない");
    // cache が手放しても使用中の mask は memory にあり、上限に数え続ける。
    cache->retainOnly({});
    check(cache->residentSequenceCount() == 0 && cache->residentBytes() == 5984,
          "cache が手放しても使用中の変形の mask は上限に数える");
    heldT1b.reset();
    heldT2.reset();
    check(cache->residentBytes() == 0, "mask が破棄されると予約が返る");

    // 1 本で上限を超える変形: disk は Ready、memory には置かない。
    check(waitForTransform(*cache, t1).state == MathRasterCache::State::Ready,
          "取り下げた変形を要求し直すと disk から Ready");
    cache->setResidentMemoryBudget(2991);
    const auto oversized = cache->residentTransform(t1);
    check(oversized.state == MathRasterCache::Residency::OverBudget &&
              oversized.message.contains(QStringLiteral("memory")) && cache->residentBytes() == 0 &&
              cache->requestTransform(t1).state == MathRasterCache::State::Ready &&
              cache->readyTransform(t1).has_value(),
          "上限 (2991) より大きい変形 (2992) は memory に置かず、disk の変形は Ready のまま");
    cache->setResidentMemoryBudget(2992);
    check(useT(t1).state == MathRasterCache::Residency::Resident,
          "対照: 上限ちょうどの変形は memory に置ける");
}

// disk の大きさは合うが中身の壊れた frame: memory に読むときに見つけ、artifact を消して Failed。
void testTransformCorruptResident(const std::filesystem::path& root) {
    const auto directory = root / L"transform corrupt";
    FakeBackend backend;
    auto cache = readyCache(directory, backend);
    const auto s = transformSpec("x", "WIDE t");
    check(waitForTransform(*cache, s).state == MathRasterCache::State::Ready, "変形が揃う");
    const auto key = cache->transformKeyFor(s);
    writeText(transformFrameOf(directory, key, 2), std::string(kCropBytes, 'x'));
    const auto broken = waitForResidentTransform(*cache, s);
    check(broken.state != MathRasterCache::Residency::Resident && cache->residentBytes() == 0,
          "中身の壊れた frame の変形は memory に置かない (予約も返す)");
    check(cache->requestTransform(s).state == MathRasterCache::State::Failed &&
              !cache->readyTransform(s) &&
              !std::filesystem::exists(transformProvenanceOf(directory, key)),
          "変形を Failed にして artifact を消す (書き出しにも渡さない)");
    cache->forgetFailures();
    check(waitForTransform(*cache, s).state == MathRasterCache::State::Ready &&
              *backend.transformRenders == 2 &&
              waitForResidentTransform(*cache, s).state == MathRasterCache::Residency::Resident,
          "forgetFailures の後は描き直して読める");
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
    testSequenceRenderAndDisk(root);
    testSequenceFailuresAndCancel(root);
    testSequenceResidency(root);
    testSequenceCorruptResident(root);
    testSequenceBackendCapability(root);
    testSequenceWithoutBackendSupport(root);
    testTransformWaitsForEndpoints(root);
    testTransformNoLastGoodEndpoint(root);
    testTransformEndpointEditInFlight(root);
    testCancelPendingAnimationsKeepsReady(root);
    testTransformAuthority(root);
    testTransformDiskAndProvenance(root);
    testTransformValidation(root);
    testTransformResidency(root);
    testTransformCorruptResident(root);
    testDestroyWhileRendering(root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
