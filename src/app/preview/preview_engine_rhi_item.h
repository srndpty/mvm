#ifndef MVM_APP_PREVIEW_PREVIEW_ENGINE_RHI_ITEM_H
#define MVM_APP_PREVIEW_PREVIEW_ENGINE_RHI_ITEM_H

#include "preview_engine/preview_engine.h"

#include <memory>

#include <QQuickRhiItem>

namespace mvm::app {

// P5-C product用の薄いQt隔離層。native targetは所有しないが、engineはrendererの
// 切替完了まで共有所有し、GUI側の即時破棄によるuse-after-freeを防ぐ。
class PreviewEngineRhiItem : public QQuickRhiItem {
    Q_OBJECT
public:
    explicit PreviewEngineRhiItem(QQuickItem* parent = nullptr);

    void setEngine(std::shared_ptr<preview::PreviewEngine> engine);

    std::shared_ptr<preview::PreviewEngine> engine() const { return engine_; }

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    std::shared_ptr<preview::PreviewEngine> engine_;
};

} // namespace mvm::app

#endif // MVM_APP_PREVIEW_PREVIEW_ENGINE_RHI_ITEM_H
