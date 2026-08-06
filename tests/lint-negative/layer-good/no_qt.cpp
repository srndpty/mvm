// lint の negative test の対照群。**ビルド対象ではない。**
//
// 上の layer-bad/ が「違反すれば落ちる」ことを示すのに対し、
// こちらは「正しいコードでは落ちない」ことを示す。
// 対照群が無いと、negative test が構造そのもの (フィクスチャの置き場所など)
// で落ちているのか、意図した違反で落ちているのかが分からない。

#include <d3d11.h>
#include <string>
#include <vector>

// Qt も QRhi も include していない。src/media/gpu_preview と同じ立場。
void mvm_lint_fixture_no_qt(void);
