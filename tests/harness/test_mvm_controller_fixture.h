#pragma once

#include "mvm_controller.h"
#include "test_media_fixture.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

#include <QCoreApplication>

namespace mvm::test::controller {
inline int failures = 0;

inline void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

inline bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents();
    return predicate();
}

// recovery は編集から debounce (2 秒) の後に worker が書く。待つ上限は debounce に
// 書き込み (serialize と fsync) の時間を足したものより十分長くする。CI の並列実行では
// 4 秒で足りずに失敗したことがある。条件を満たした時点で抜けるので、通常は待たない。
constexpr int kRecoveryWaitMs = 20000;

inline mvm::project::Project videoProject() {
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip video;
    video.id = "video";
    video.name = "video";
    video.mediaPath = L"C:/mvm-test-video.mp4";
    video.sourceFpsNum = 60;
    video.sourceFpsDen = 1;
    video.sourceFrameCount = 120;
    video.sourceOutFrame = 120;
    project.timelineClips.push_back(video);
    mvm::test::attachFixtureMedia(project);
    return project;
}

} // namespace mvm::test::controller
