#ifndef MVM_APPS_MVM_PREVIEW_SURFACE_QML_H
#define MVM_APPS_MVM_PREVIEW_SURFACE_QML_H

#include "app/preview/preview_engine_rhi_item.h"

#include <QtQml/qqmlregistration.h>

namespace mvm::app {

class PreviewSurfaceQml : public PreviewEngineRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(PreviewSurface)
public:
    using PreviewEngineRhiItem::PreviewEngineRhiItem;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_PREVIEW_SURFACE_QML_H
