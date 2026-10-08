#include "graph_preview_cache.h"

#include <algorithm>
#include <string_view>

#include <QMetaObject>
#include <QUuid>

namespace mvm::app {
GraphPreviewCache::GraphPreviewCache(Preflight preflight, QObject* parent)
    : QObject(parent), preflight_(std::move(preflight)),
      authority_(std::make_shared<graph::PublicationAuthority>()) {
    renderPool_.setMaxThreadCount(2);
    decodePool_.setMaxThreadCount(1);
}

GraphPreviewCache::~GraphPreviewCache() {
    shutdown();
}

GraphPreviewCache::Reason GraphPreviewCache::reasonFor(graph::Failure failure) {
    using F = graph::Failure;
    switch (failure) {
    case F::InvalidExpression:
        return Reason::InvalidExpression;
    case F::UnsupportedExpression:
        return Reason::UnsupportedExpression;
    case F::NoFiniteSamples:
        return Reason::NoFiniteSamples;
    case F::BackendUnavailable:
        return Reason::BackendUnavailable;
    case F::ArtifactCorrupt:
        return Reason::ArtifactCorrupt;
    case F::Cancelled:
        return Reason::Cancelled;
    case F::Superseded:
        return Reason::Superseded;
    case F::RendererFailure:
        return Reason::RendererFailure;
    case F::LabelFailure:
        return Reason::LabelFailure;
    case F::PublicationFailure:
        return Reason::PublicationFailure;
    case F::ResourceLimit:
        return Reason::ResourceLimit;
    default:
        return Reason::InvalidGraph;
    }
}

void GraphPreviewCache::invalidate() {
    ++generation_;
    publicationGeneration_ = authority_->supersede();
    if (preflightCancel_)
        preflightCancel_->store(true);
    checking_ = false;
    for (auto& record : records_)
        if (record.cancel)
            record.cancel->store(true);
    for (auto& frame : frames_)
        if (frame.cancel)
            frame.cancel->store(true);
    records_.clear();
    waitingIdentity_.clear();
    frames_.clear();
}

void GraphPreviewCache::setAuthority(std::filesystem::path directory, bool authorized) {
    directory = directory.lexically_normal();
    if (closed_ || (directory == directory_ && authorized == authorized_))
        return;
    invalidate();
    directory_ = std::move(directory);
    authorized_ = authorized;
    // 確認済み identity は session 内だけ保持する。確認前の read-only session は受理しない。
    preflightAttempted_ = false;
    Q_EMIT changed();
}

void GraphPreviewCache::startPreflight() {
    if (closed_ || checking_ || !authorized_)
        return;
    checking_ = true;
    preflightAttempted_ = true;
    preflightCancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = preflightCancel_;
    const auto generation = generation_;
    const auto work = directory_ / (".preflight-" + QUuid::createUuid().toString().toStdString());
    auto preflight = preflight_;
    renderPool_.start([this, preflight, work, cancel, generation] {
        auto result = preflight(work, cancel.get());
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result), generation]() mutable {
                if (closed_ || generation != generation_)
                    return;
                checking_ = false;
                available_ = std::holds_alternative<manim::GraphBackend>(result);
                if (available_)
                    backend_ = std::get<manim::GraphBackend>(std::move(result));
                if (available_) {
                    auto waiting = std::move(waitingIdentity_);
                    waitingIdentity_.clear();
                    for (const auto& spec : waiting)
                        request(spec);
                }
                pump();
                Q_EMIT changed();
            },
            Qt::QueuedConnection);
    });
}

QString GraphPreviewCache::keyFor(const graph::GraphRenderSpec& spec) const {
    if (!backend_ || backend_->toolchain.empty())
        return {};
    const auto key = graph::staticKey(spec, backend_->toolchain);
    return QString::fromStdString(spec.drawFrames ? graph::drawKey(key, spec.drawFrames) : key);
}

void GraphPreviewCache::retainOnly(const QSet<QString>& keys) {
    for (auto it = records_.begin(); it != records_.end();) {
        if (keys.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true);
        it = records_.erase(it);
    }
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (keys.contains(it->owner)) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true);
        it = frames_.erase(it);
    }
}

void GraphPreviewCache::request(const graph::GraphRenderSpec& spec) {
    const auto key = keyFor(spec);
    if (closed_)
        return;
    if (key.isEmpty()) {
        const auto input = QString::fromStdString(graph::digest(graph::canonicalSpec(spec))) +
                           QLatin1Char('/') + QString::number(spec.drawFrames);
        if (waitingIdentity_.size() < 32 || waitingIdentity_.contains(input))
            waitingIdentity_.insert(input, spec);
        if (!preflightAttempted_ && authorized_)
            startPreflight();
        return;
    }
    if (!records_.contains(key)) {
        Record record;
        record.spec = spec;
        record.status.key = key;
        record.status.generation = generation_;
        record.ticket = ++ticket_;
        record.requested = std::chrono::steady_clock::now();
        records_.insert(key, std::move(record));
    }
    records_[key].priority = ++tick_;
    pump();
}

void GraphPreviewCache::pump() {
    if (closed_ || checking_ || activeRenders_ >= 2 || !backend_)
        return;
    auto chosen = records_.end();
    for (auto it = records_.begin(); it != records_.end(); ++it)
        if (it->status.job == Job::Unrequested &&
            (chosen == records_.end() || it->priority > chosen->priority))
            chosen = it;
    if (chosen == records_.end())
        return;
    const auto key = chosen.key();
    auto& record = chosen.value();
    record.status.job = Job::Checking;
    record.status.reason = Reason::Rendering;
    record.cancel = std::make_shared<std::atomic<bool>>(false);
    ++activeRenders_;
    const auto cancel = record.cancel;
    const auto generation = generation_, ticket = record.ticket;
    const auto spec = record.spec;
    const auto directory = directory_;
    const auto backend = *backend_;
    const bool generate = authorized_ && available_;
    auto authority = authority_;
    // publication の世代は cache session と別。編集で同じ artifact の別 consumer を止めない。
    const auto publicationGeneration = publicationGeneration_;
    const auto job = directory / (".job-" + QUuid::createUuid().toString().toStdString());
    renderPool_.start([this, key, spec, directory, backend, generate, authority,
                       publicationGeneration, job, cancel, generation, ticket] {
        auto result = graph::validateArtifact(directory / key.toStdString(), spec,
                                              backend.toolchain, cancel.get());
        std::error_code ec;
        const bool exists = std::filesystem::exists(directory / key.toStdString(), ec);
        bool launched = false;
        if (!std::holds_alternative<graph::Artifact>(result) && generate && !cancel->load()) {
            launched = true;
            QMetaObject::invokeMethod(
                this,
                [this, key, generation, ticket] {
                    auto it = records_.find(key);
                    if (!closed_ && generation == generation_ && it != records_.end() &&
                        it->ticket == ticket)
                        it->status.job = Job::Rendering;
                },
                Qt::QueuedConnection);
            result = authority->generate({spec, backend.toolchain, job}, directory,
                                         publicationGeneration, backend.render, cancel.get());
        }
        QMetaObject::invokeMethod(
            this,
            [this, key, result = std::move(result), cancel, generation, ticket, launched, exists,
             generate]() mutable {
                if (launched)
                    ++renderCount_;
                --activeRenders_;
                auto it = records_.find(key);
                if (!closed_ && generation == generation_ && it != records_.end() &&
                    it->ticket == ticket && !cancel->load()) {
                    if (auto* artifact = std::get_if<graph::Artifact>(&result)) {
                        it->artifact = std::move(*artifact);
                        it->status.job = Job::Ready;
                        it->status.artifact = ArtifactState::Validated;
                        it->status.reason = Reason::None;
                        it->status.validatedSnapshot = true;
                        it->status.artifactReadyMs =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - it->requested)
                                .count();
                    } else {
                        const auto& error = std::get<graph::Error>(result);
                        it->status.job = Job::Failed;
                        it->status.reason = reasonFor(error.failure);
                        it->status.message = QString::fromStdString(error.message);
                        it->status.artifact =
                            exists ? ArtifactState::Corrupt : ArtifactState::Missing;
                        if (!generate && !exists)
                            it->status.reason = Reason::BackendUnavailable;
                    }
                }
                pump();
                if (!closed_)
                    Q_EMIT changed();
            },
            Qt::QueuedConnection);
    });
    pump();
}

std::int64_t GraphPreviewCache::frameIndex(const graph::GraphRenderSpec& spec,
                                           std::int64_t sourceFrame) {
    return sourceFrame >= 0 && sourceFrame < spec.drawFrames ? sourceFrame : -1;
}

QString GraphPreviewCache::frameKey(const QString& key, std::int64_t frame) {
    return key + QLatin1Char('/') + QString::number(frame);
}

void GraphPreviewCache::requestFrame(const graph::GraphRenderSpec& spec, std::int64_t sourceFrame) {
    request(spec);
    requestDecoded(spec, sourceFrame, false);
    // 現在の exact frame が使える場合だけ一枚先を要求する。先読みは退避を起こさない。
    if (status(spec, sourceFrame).frameAvailable && sourceFrame >= 0 &&
        sourceFrame < spec.drawFrames)
        requestDecoded(spec, sourceFrame + 1, true);
}

void GraphPreviewCache::requestDecoded(const graph::GraphRenderSpec& spec, std::int64_t sourceFrame,
                                       bool prefetch) {
    const auto key = keyFor(spec);
    const auto record = records_.constFind(key);
    if (record == records_.constEnd() || !record->artifact)
        return;
    const auto index = frameIndex(spec, sourceFrame);
    const auto id = frameKey(key, index);
    if (auto it = frames_.find(id); it != frames_.end()) {
        if (it->state != Residency::OverBudget) {
            it->tick = ++tick_;
            return;
        }
        frames_.erase(it);
    }
    // 待機中の decode も二件に制限し、seek による列の増大を防ぐ。
    if (activeDecodes_ >= 2)
        return;
    const std::size_t bytes =
        static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.height) * 4;
    if (prefetch && (bytes > budget_->limit || budget_->live.load() > budget_->limit - bytes))
        return;
    while (!prefetch && bytes <= budget_->limit && budget_->live.load() > budget_->limit - bytes) {
        auto oldest = frames_.end();
        for (auto it = frames_.begin(); it != frames_.end(); ++it)
            if (it->image && it->image.use_count() == 1 &&
                (oldest == frames_.end() || it->tick < oldest->tick))
                oldest = it;
        if (oldest == frames_.end())
            break;
        frames_.erase(oldest);
    }
    Frame frame;
    frame.reason = Reason::Loading;
    frame.owner = key;
    frame.index = index;
    frame.tick = ++tick_;
    if (bytes > budget_->limit || budget_->live.load() > budget_->limit - bytes) {
        frame.state = Residency::OverBudget;
        frame.reason = Reason::OverBudget;
        frames_.insert(id, std::move(frame));
        return;
    }
    frame.cancel = std::make_shared<std::atomic<bool>>(false);
    const auto cancel = frame.cancel;
    frames_.insert(id, std::move(frame));
    const auto budget = budget_;
    const auto live = budget->live.fetch_add(bytes) + bytes;
    budget->peak.store(std::max(budget->peak.load(), live));
    // 最後の snapshot/GPU 所有者が解放した時だけ byte を返す。decode 中も予約に含む。
    auto image = std::shared_ptr<preview::PreviewStillImage>(
        new preview::PreviewStillImage, [budget, bytes](preview::PreviewStillImage* value) {
            delete value;
            budget->live.fetch_sub(bytes);
        });
    const auto artifact = *record->artifact;
    const auto generation = generation_;
    ++activeDecodes_;
    decodePool_.start([this, spec, artifact, index, id, image, cancel, generation] {
        const auto started = std::chrono::steady_clock::now();
        const auto path = artifact.directory /
                          (index < 0 ? "static.png" : "frame-" + std::to_string(index) + ".png");
        auto raster = graph::readRgba(path, spec.width, spec.height);
        Reason reason = Reason::None;
        if (cancel->load())
            reason = Reason::Cancelled;
        else if (auto* rgba = std::get_if<graph::Raster>(&raster)) {
            const auto hashIndex = static_cast<std::size_t>(index + 1);
            const std::string_view pixels(reinterpret_cast<const char*>(rgba->rgba.data()),
                                          rgba->rgba.size());
            if (hashIndex >= artifact.pixelHashes.size() ||
                graph::digest(pixels) != artifact.pixelHashes[hashIndex])
                reason = Reason::ProvenanceMismatch;
            else {
                image->width = rgba->width;
                image->height = rgba->height;
                image->rgba = std::move(rgba->rgba);
            }
        } else
            reason = Reason::FrameMissing;
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        QMetaObject::invokeMethod(
            this,
            [this, id, image, cancel, reason, generation, elapsed] {
                --activeDecodes_;
                if (closed_ || generation != generation_ || cancel->load())
                    return;
                auto it = frames_.find(id);
                if (it == frames_.end() || it->cancel != cancel)
                    return;
                it->reason = reason;
                it->decodeMs = elapsed;
                it->state = reason == Reason::None ? Residency::Resident : Residency::Corrupt;
                if (reason == Reason::None)
                    it->image = image;
                else {
                    const auto owner = it->owner;
                    auto failedRecord = records_.find(owner);
                    if (failedRecord != records_.end()) {
                        failedRecord->artifact.reset();
                        failedRecord->status.artifact = ArtifactState::Corrupt;
                        failedRecord->status.validatedSnapshot = false;
                        failedRecord->status.job = Job::Failed;
                        failedRecord->status.reason = reason;
                    }
                    for (auto failedFrame = frames_.begin(); failedFrame != frames_.end();) {
                        if (failedFrame->owner != owner) {
                            ++failedFrame;
                            continue;
                        }
                        if (failedFrame->cancel)
                            failedFrame->cancel->store(true);
                        failedFrame = frames_.erase(failedFrame);
                    }
                }
                Q_EMIT changed();
            },
            Qt::QueuedConnection);
    });
}

GraphPreviewCache::Status GraphPreviewCache::status(const graph::GraphRenderSpec& spec,
                                                    std::int64_t sourceFrame) const {
    Status result;
    result.key = keyFor(spec);
    if (auto it = records_.constFind(result.key); it != records_.constEnd())
        result = it->status;
    result.sourceFrame = sourceFrame;
    result.generation = generation_;
    result.identityEstablished = backend_.has_value();
    result.backendAvailable = available_;
    result.backendCheckInProgress = checking_;
    if (checking_ && result.job == Job::Unrequested)
        result.job = Job::Checking;
    if (!backend_)
        result.reason = Reason::BackendUnavailable;
    if (result.artifact == ArtifactState::Validated)
        result.reason = Reason::FrameMissing;
    if (auto it = frames_.constFind(frameKey(result.key, frameIndex(spec, sourceFrame)));
        it != frames_.constEnd()) {
        result.residency = it->state;
        result.reason = it->reason;
        result.decodeMs = it->decodeMs;
        result.frameAvailable = bool(it->image);
        result.transparentFallback = !result.frameAvailable;
    }
    return result;
}

std::map<std::int64_t, std::shared_ptr<const preview::PreviewStillImage>>
GraphPreviewCache::frames(const graph::GraphRenderSpec& spec) const {
    std::map<std::int64_t, std::shared_ptr<const preview::PreviewStillImage>> result;
    const auto key = keyFor(spec);
    const auto record = records_.constFind(key);
    if (record == records_.constEnd() || !record->artifact)
        return result;
    for (const auto& frame : frames_)
        if (frame.owner == key && frame.image)
            result.emplace(frame.index, frame.image);
    return result;
}

void GraphPreviewCache::shutdown() {
    if (closed_)
        return;
    closed_ = true;
    authority_->shutdown();
    invalidate();
    renderPool_.waitForDone();
    decodePool_.waitForDone();
}

void GraphPreviewCache::setBudgetForTest(std::size_t bytes) {
    frames_.clear();
    budget_ = std::make_shared<Budget>();
    budget_->limit = bytes;
}

std::size_t GraphPreviewCache::residentBytes() const {
    return budget_->live.load();
}

std::size_t GraphPreviewCache::peakBytes() const {
    return budget_->peak.load();
}

std::size_t GraphPreviewCache::pendingDecodeCount() const {
    return static_cast<std::size_t>(
        std::count_if(frames_.begin(), frames_.end(),
                      [](const auto& frame) { return frame.state == Residency::Loading; }));
}

void GraphPreviewCache::resetSession() {
    if (closed_)
        return;
    invalidate();
    backend_.reset();
    available_ = false;
    preflightAttempted_ = false;
}

void GraphPreviewCache::refreshBackend() {
    startPreflight();
}
} // namespace mvm::app
