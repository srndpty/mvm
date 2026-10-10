# P4-5 frame 順序調査の最終検証結果

この表は `build/p45-frame-order-investigation/final-audit.json` の値から生成した。
判断と制限は [調査記録](math-graph-p45-frame-order.md) に記載する。

## 最終 source と実行環境

基準 revision: `bfa991c4307c4289d4aec583aa17538245d8e087`。
Real / Focused の source 集合 SHA-256: `B2D2B06EDB2C7E0F9706ABABA97791C293C4F3CAF20579A357C5BCCCDFCD123E`。
対象は 1023 ファイル。変異終了後の全 hash を照合した。
元の FAIL/PASS とレビューの 2349 ファイルも不変である。

最終 Real: `build/math-p45-20261011-055819-Real`。
最終 Focused: `build/math-p45-20261011-055937-Focused`。
DLL・exe・公式 MLT source の SHA とパッケージ版は `runtime.json` に保存した。
これは元の初回 FAIL 取得時の exe hash を後付けで証明するものではない。

一時 MLT 観測の復元 SHA-256: `A02EDC908F5D51AEF202C822362A9445630CACE65C61361D1DA24260AAC98136`。
元 byte 列と一致し、製品 source・公開 API・CMake に恒久的な変更はない。
全 release/debug gate は繰り返さず、変更した試験に関連する gate を実行した。

## 通常の検証

| 種別 | 検査／試験件数 | 失敗 |
| --- | ---: | ---: |
| 最終 Real | 216 | 0 |
| FFV1 分離・frame 1 保持対照 | 217 | 0 |
| Focused | 72 | 0 |
| Focused encoder oracle | 16 | 0 |
| 通常 CTest 回帰 | 17 | 0 |

composition の diagnostic / oracle / differential も終了コード 0。
lint は通過。performance / stability の試験は今回実行していない。
回帰は `build/math-p45-20261011-060905-Regressions` に保存した。

## 動的な負例

| 変異 | ビルド終了コード | 変異試験終了コード | 復元後試験終了コード | SHA・byte 列 | FAIL 行数 |
| --- | ---: | ---: | ---: | --- | ---: |
| source mapping の誤り | 0 | 8 | 0 | 一致 | 14 |
| 要求出力 frame の欠落 | 0 | 8 | 0 | 一致 | 4 |
| RGBA validator 不在 | 0 | 8 | 0 | 一致 | 6 |
| RGBA 検査への frame ID 誤伝達 | 0 | 8 | 0 | 一致 | 5 |
| active worker への shutdown 取消不達 | 0 | 1 | 0 | 一致 | 3 |
| shutdown 後の stale 完了通知 | 0 | 1 | 0 | 一致 | 2 |
| RGBA validator の拒否を無視 | 0 | 8 | 0 | 一致 | 2 |

終了コード 8 は CTest の assertion 失敗、1 は Real の assertion 失敗。
全変異で意図した assertion の失敗を確認し、timeout・crash を検出実績に数えていない。
後続の同じ原因による FAIL 行も削除せず、全行を最終照合 JSON に保持する。

さらに最終 source の encoder 未開始負例は 216 検査中 5 assertion が失敗した。
実 frame の保持 assertion で検出し、validator event はゼロ、barrier 未到達だった。
runner の終了を待機解除条件に使うので、無関係な timeout ではない。
通常の Real、各変異の復元後 Real、復元後 focused は通過した。

## 保持する FAIL と制限

調査は CLOSED。frame 0 を最初の callback／保持対象とする前提と barrier の選出・公開の
競合は FIXED。正確な全 domain coverage と独立 RGBA oracle の検査は維持する。
元の初回 Real FAIL は HISTORICAL UNKNOWN として保持し、後続 PASS で解決済みにしない。
通知で検査順を固定した旧 assertion の対照も、前提と停止判定の二つの FAIL を保持した。
最初の静止背景対照は素材台帳と clip の不整合で保存 assertion が失敗した。
原因を修正した fixture の検証は上表のとおり通過したが、旧 FAIL は残す。
新規 frame 欠落変異の最初の span 検査も対象ゼロで失敗し、cast を含む実 source へ修正した。

元の run における frame 0 の要求・検査・barrier 選出の時系列がないため、
自然な MLT 順序変動と旧 harness の公開前競合のどちらだったかは確定できない。
試験の authority と同期を修正したことと、元 run の直接原因を特定したことを区別する。
コミット・push は行っていない。

