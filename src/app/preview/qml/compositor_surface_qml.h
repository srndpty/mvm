#ifndef MVM_APP_PREVIEW_QML_COMPOSITOR_SURFACE_QML_H
#define MVM_APP_PREVIEW_QML_COMPOSITOR_SURFACE_QML_H

#include "app/preview/compositor_rhi_item.h"

#include <QtQml/qqmlregistration.h>

namespace mvm::app {

// 検証アプリの QML module へ CompositorRhiItem を宣言的に登録するための型。
// 実行時の qmlRegisterType では qmllint / qmlls から型が見えないため、
// 各アプリが自分の qt_add_qml_module の SOURCES へこのヘッダを加える。
class CompositorSurfaceQml : public CompositorRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(CompositorSurface)
public:
    using CompositorRhiItem::CompositorRhiItem;
};

} // namespace mvm::app

#endif // MVM_APP_PREVIEW_QML_COMPOSITOR_SURFACE_QML_H
