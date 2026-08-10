#ifndef MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_INTERNAL_H
#define MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_INTERNAL_H

#include "preview_engine/preview_engine.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>

namespace mvm::preview::internal {

struct StateChangedEvent {
    PreviewEngineState state;
};

struct PositionChangedEvent {
    PreviewPosition position;
};

struct FramePresentedEvent {
    PresentedFrameInfo frame;
};

struct ErrorOccurredEvent {
    PreviewError error;
};

struct DeviceChangedEvent {
    PreviewDeviceInfo device;
};

using PreviewEvent = std::variant<StateChangedEvent, PositionChangedEvent, FramePresentedEvent,
                                  ErrorOccurredEvent, DeviceChangedEvent>;

class EventMailbox {
public:
    explicit EventMailbox(std::size_t capacity);

    Result<void> push(PreviewEvent event);
    std::optional<PreviewEvent> pop();
    std::size_t size() const;
    std::size_t capacity() const;
    bool empty() const;

private:
    bool isTerminal(const PreviewEvent& event) const;
    bool tryCoalesce(const PreviewEvent& event);

    std::size_t capacity_;
    std::deque<PreviewEvent> events_;
};

class PreviewStateMachine {
public:
    PreviewEngineState state() const;
    std::optional<PreviewError> lastError() const;
    bool destructionSafe() const;

    Result<void> initialize();
    Result<void> attachRenderDevice();
    Result<void> play();
    Result<void> pause();
    Result<void> seek();
    Result<void> completeSeek();
    Result<void> requestShutdown();
    Result<void> recordFatal(PreviewError error);
    Result<void> completeTeardown();

private:
    PreviewEngineState state_ = PreviewEngineState::Uninitialized;
    std::optional<PreviewError> lastError_;
    bool fatalPending_ = false;
    PreviewEngineState stateBeforeSeek_ = PreviewEngineState::ReadyPaused;
};

struct EligibleSource {
    bool videoEnabled = false;
};

class CompositionAcceptanceState {
public:
    Result<AcceptedComposition>
    submit(const std::shared_ptr<const CompositionSnapshot>& snapshot,
           const std::unordered_map<std::uint64_t, EligibleSource>& sources,
           const PreviewCapabilities& capabilities);

    void markPresented(AcceptedComposition composition);
    std::optional<AcceptedComposition> latestAcceptedToken() const;
    std::optional<AcceptedComposition> lastPresentedToken() const;
    const std::shared_ptr<const CompositionSnapshot>& latestAcceptedSnapshot() const;

private:
    std::shared_ptr<const CompositionSnapshot> latestAcceptedSnapshot_;
    std::optional<AcceptedComposition> latestAcceptedToken_;
    std::optional<AcceptedComposition> lastPresentedToken_;
    std::uint64_t nextId_ = 1;
    std::uint64_t nextRevision_ = 1;
};

class PreviewRenderPort {
public:
    static Result<void> attachLogicalDevice(PreviewEngine& engine);
    static Result<void> completeTeardown(PreviewEngine& engine);
    static Result<void> injectFatal(PreviewEngine& engine, PreviewError error);

    // bounded mailboxのfailure semanticsをbackend接続前に検査するinternal test seam。
    static void enqueueEventForTest(PreviewEngine& engine, PreviewEvent event);
    static std::size_t mailboxSizeForTest(const PreviewEngine& engine);
};

} // namespace mvm::preview::internal

#endif // MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_INTERNAL_H
