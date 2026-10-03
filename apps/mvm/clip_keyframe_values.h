#ifndef MVM_CLIP_KEYFRAME_VALUES_H
#define MVM_CLIP_KEYFRAME_VALUES_H
#include "project/clip_effects.h"

#include <algorithm>

#include <QVariantList>
#include <QVariantMap>

namespace mvm::app {
inline QVariantList clipKeyframeValues(const std::vector<project::ClipKeyframe>& source) {
    QVariantList keys;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto& key = source[index];
        const auto controls = project::clipKeySplineControls(key);
        QVariantList samples;
        if (index + 1 < source.size() && key.interpolation != project::KeyInterpolation::Linear) {
            const auto end = source[index + 1].frame;
            const auto count = std::min<std::int64_t>(32, end - key.frame);
            for (std::int64_t step = 1; step < count; ++step) {
                const auto frame = key.frame + (end - key.frame) * step / count;
                samples.append(QVariantMap{
                    {QStringLiteral("frame"), frame},
                    {QStringLiteral("value"), project::evaluateClipKeys(source, 0, frame)}});
            }
        }
        keys.append(
            QVariantMap{{QStringLiteral("frame"), key.frame},
                        {QStringLiteral("value"), key.value},
                        {QStringLiteral("interpolation"), static_cast<int>(key.interpolation)},
                        {QStringLiteral("control1"), controls.first},
                        {QStringLiteral("control2"), controls.second},
                        {QStringLiteral("samples"), samples}});
    }
    return keys;
}
} // namespace mvm::app
#endif
