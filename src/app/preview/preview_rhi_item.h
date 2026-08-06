/*
 * mvm Phase 1 / P1 - Qt Quick への表示 (Qt 依存はここに閉じる)
 *
 * **QRhi は Qt の private API である。**
 * QQuickRhiItem 自体は public だが、patch release 間でも
 * ソース互換・バイナリ互換が保証されていない (docs/phase1-plan.md §7)。
 *
 * したがって Qt / QRhi に触れるコードは
 * このファイルと preview_rhi_item.cpp だけに閉じる。
 * src/media/gpu_preview/ は Qt を一切 include しない。
 * Phase 0 で「MLT のヘッダを include してよいのは src/media/mlt/ だけ」と
 * 決めたのと同じ隔離である。
 *
 * スレッド:
 *   - GUI thread   : property の読み書き
 *   - render thread: PreviewRhiRenderer (D3D11 device の取得元・描画)
 *   受け渡しは synchronize() でのみ行う (両スレッドが停止している時点)。
 */

#ifndef MVM_APP_PREVIEW_PREVIEW_RHI_ITEM_H
#define MVM_APP_PREVIEW_PREVIEW_RHI_ITEM_H

#include "media/gpu_preview/preview_state.h"

#include <memory>

#include <QColor>
#include <QQuickRhiItem>

namespace mvm::app {

class PreviewRhiItem : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(bool deviceReady READ deviceReady NOTIFY statusChanged)
    Q_PROPERTY(QString initError READ initError NOTIFY statusChanged)
    Q_PROPERTY(qlonglong displayedFrame READ displayedFrame NOTIFY statusChanged)
    Q_PROPERTY(bool linearFilter READ linearFilter WRITE setLinearFilter NOTIFY linearFilterChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY
                   backgroundColorChanged)

public:
    explicit PreviewRhiItem(QQuickItem* parent = nullptr);
    ~PreviewRhiItem() override;

    // 3 スレッドが共有する状態。GUI thread から decode 側へ渡す。
    std::shared_ptr<gpu::PreviewState> state() const { return state_; }

    bool deviceReady() const;
    QString initError() const;
    qlonglong displayedFrame() const;

    bool linearFilter() const { return linearFilter_; }

    void setLinearFilter(bool on);

    QColor backgroundColor() const { return background_; }

    void setBackgroundColor(const QColor& c);

    // GPU 完了追跡の backend を強制する (テスト用)。**attach より前に呼ぶ。**
    void setPreferredCompletionBackend(gpu::GpuCompletionBackend b) { preferredCompletion_ = b; }

    gpu::GpuCompletionBackend preferredCompletionBackend() const { return preferredCompletion_; }

    // GUI thread から呼ぶ。表示を捨てて背景だけにする。
    Q_INVOKABLE void clearSurface();

    // 表示状態を QML へ通知する (GUI thread のタイマから呼ぶ)。
    Q_INVOKABLE void refreshStatus();

Q_SIGNALS:
    void statusChanged();
    void linearFilterChanged();
    void backgroundColorChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    std::shared_ptr<gpu::PreviewState> state_;
    bool linearFilter_ = true;
    QColor background_{0, 0, 0};
    gpu::GpuCompletionBackend preferredCompletion_ = gpu::GpuCompletionBackend::Fence;
};

} // namespace mvm::app

#endif // MVM_APP_PREVIEW_PREVIEW_RHI_ITEM_H
