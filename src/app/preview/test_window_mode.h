#ifndef MVM_APP_PREVIEW_TEST_WINDOW_MODE_H
#define MVM_APP_PREVIEW_TEST_WINDOW_MODE_H

#include <QVariantMap>
#include <Qt>

class QWindow;

namespace mvm::app {

// 表示前に設定する。非アクティブだけでは Z 順の上に表示されるため、背面も明示する。
inline Qt::WindowFlags testBackgroundWindowFlags() {
    return Qt::Tool | Qt::WindowStaysOnBottomHint | Qt::WindowDoesNotAcceptFocus |
           Qt::WindowTransparentForInput;
}

// 通常の試験 (CTest) の結果を、利用者の画面の向き・解像度・拡大率に依存させないための設定。
// 環境変数 MVM_TEST_FIXED_WINDOW=1 のときだけ有効にする (tests/CMakeLists.txt が全試験に付ける)。
//
// - 拡大率を 1.0 に固定する (QGuiApplication の生成前に prepare... を呼ぶ)。
// - window を枠なしにする。Windows は枠付きの window を画面より大きくさせないが、
//   枠なしの window は縮めないので、縦向きの画面でも 1920x1080 の描画先を作れる。
//   画面からはみ出した部分も描画は止まらない (検査は描画先から読み戻す)。
// - 背面に保ち、focus を取らず、入力を透過する。タスクバーにも表示しない。
//
// 物理画面の状態そのものが測定対象の正式計測では使わない。この設定で正式判定を通さないこと。
bool testFixedWindowRequested();
void prepareTestFixedWindowEnvironment();
Qt::WindowFlags testFixedWindowFlags();
// QML の root Window へ渡す初期 property (flags と位置)。無効なら空。
QVariantMap testFixedWindowInitialProperties();
// C++ で作る window へ、show() の前に適用する。無効なら何もしない。
void applyTestFixedWindow(QWindow& window);

} // namespace mvm::app

#endif
