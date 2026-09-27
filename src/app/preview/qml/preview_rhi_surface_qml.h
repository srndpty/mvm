#ifndef MVM_APP_PREVIEW_QML_PREVIEW_RHI_SURFACE_QML_H
#define MVM_APP_PREVIEW_QML_PREVIEW_RHI_SURFACE_QML_H

#include "app/preview/preview_rhi_item.h"

#include <QtQml/qqmlregistration.h>

namespace mvm::app {

// preview_spike の QML module へ PreviewRhiItem を宣言的に登録するための型。
// 実行時の qmlRegisterType では qmllint / qmlls から型が見えない。
class PreviewRhiSurfaceQml : public PreviewRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(PreviewSurface)
public:
    using PreviewRhiItem::PreviewRhiItem;
};

} // namespace mvm::app

#endif // MVM_APP_PREVIEW_QML_PREVIEW_RHI_SURFACE_QML_H
