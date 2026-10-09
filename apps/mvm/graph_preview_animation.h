#ifndef MVM_APPS_GRAPH_PREVIEW_ANIMATION_H
#define MVM_APPS_GRAPH_PREVIEW_ANIMATION_H

#include "preview_engine/preview_types.h"
#include "project/graph_edit.h"

#include <algorithm>
#include <atomic>
#include <map>

namespace mvm::app {
// GUI thread は新しい不変の一覧を公開する。render thread は cache に触れない。
// weak owner は退避を妨げず、画素をコピーする間は lock した shared owner が保護する。
struct GraphPresentation {
    using Frames = std::map<std::int64_t, std::weak_ptr<const preview::PreviewStillImage>>;
    std::atomic<std::shared_ptr<const Frames>> frames{std::make_shared<const Frames>()};
};

class GraphPreviewAnimation final : public preview::PreviewStillAnimation {
public:
    enum class Diagnostic { Exact, OutsideClip, InvalidMapping, FrameMissing };

    struct Evaluation {
        Diagnostic diagnostic = Diagnostic::FrameMissing;
        std::int64_t index = -1;
        std::shared_ptr<const preview::PreviewStillImage> image;
    };

    GraphPreviewAnimation(project::TimelineClip clip, core::FrameRate fps,
                          std::shared_ptr<const GraphPresentation> presentation,
                          std::int64_t visibleFrames, int width, int height)
        : clip_(std::move(clip)), fps_(fps), presentation_(std::move(presentation)),
          visibleFrames_(visibleFrames), rect_{0, 0, width, height} {}

    preview::PreviewPixelRect patchRect() const override { return rect_; }

    Evaluation evaluate(std::int64_t output) const {
        if (output < clip_.timelineStartFrame ||
            output - clip_.timelineStartFrame >= visibleFrames_)
            return {Diagnostic::OutsideClip, -1, {}};
        std::string error;
        const auto frame =
            project::evaluateGraphClip(clip_, fps_, output - clip_.timelineStartFrame, error);
        if (!frame)
            return {Diagnostic::InvalidMapping, -1, {}};
        const auto index = frame->sourceFrame < clip_.graph.intro.frames ? frame->sourceFrame : -1;
        auto image = exactImage(index);
        return {image ? Diagnostic::Exact : Diagnostic::FrameMissing, index, std::move(image)};
    }

    std::int64_t stateAt(std::int64_t output) const override {
        const auto value = evaluate(output);
        // 0 は透明、1 は端点、2 以降は Draw の exact source index。
        return value.image ? value.index + 2 : 0;
    }

    void fillPatch(std::int64_t state, std::uint8_t* out) const override {
        const auto image = state > 0 ? exactImage(state - 2) : nullptr;
        if (image)
            std::copy(image->rgba.begin(), image->rgba.end(), out);
        else
            std::fill_n(out,
                        static_cast<std::size_t>(rect_.width) *
                            static_cast<std::size_t>(rect_.height) * 4,
                        std::uint8_t{0});
    }

private:
    std::shared_ptr<const preview::PreviewStillImage> exactImage(std::int64_t index) const {
        const auto frames = presentation_->frames.load();
        const auto found = frames->find(index);
        return found == frames->end() ? nullptr : found->second.lock();
    }

    const project::TimelineClip clip_;
    const core::FrameRate fps_;
    const std::shared_ptr<const GraphPresentation> presentation_;
    const std::int64_t visibleFrames_;
    const preview::PreviewPixelRect rect_;
};
} // namespace mvm::app
#endif
