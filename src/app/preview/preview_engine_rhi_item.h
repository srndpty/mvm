#ifndef MVM_APP_PREVIEW_PREVIEW_ENGINE_RHI_ITEM_H
#define MVM_APP_PREVIEW_PREVIEW_ENGINE_RHI_ITEM_H

#include "preview_engine/preview_engine.h"

#include <QQuickRhiItem>

namespace mvm::app {

// P5-C product用の薄いQt隔離層。engineとnative targetの所有権は持たない。
class PreviewEngineRhiItem final : public QQuickRhiItem {
    Q_OBJECT
public:
    explicit PreviewEngineRhiItem(QQuickItem* parent = nullptr);

    void setEngine(preview::PreviewEngine* engine);

    preview::PreviewEngine* engine() const { return engine_; }

    void requestRenderUpdate();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    preview::PreviewEngine* engine_ = nullptr;
};

} // namespace mvm::app

#endif // MVM_APP_PREVIEW_PREVIEW_ENGINE_RHI_ITEM_H
