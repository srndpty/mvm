# P4-5 製品受入の再開記録

状態: **P4-5 PASS/CLOSED**、**P4 PASS/CLOSED**。
以前の [P4-5 結果](math-graph-p45-results.md) と [P4-5.1 結果](math-graph-p451-results.md) は履歴として保存する。

## 実製品の受入経路

既存の実 `Main.qml`、`MvmController`、native preview、書き出しダイアログ、export worker を使う。
作成メニューと inspector の入力から schema22 を保存し、終了・再起動して実 Manim の
検証済み artifact を H.264 に書き出す。不正式も保存・再読込して出力拒否を確認し、
修復・Undo/Redo・保存・再起動して正常出力へ戻す。

Draw 途中の実 split、source frame 4 からの trim、24000/1001 fps を検査する。
source origin は P4-1 の半開 frame 境界であり、独立期待値は
`ceil(sourceIn * outputFps / sourceFps)` から整数で求める。local frame 0 へ戻さない。
MP4 の全 video frame の timestamp と frame count は UCRT64 ffprobe で検査し、
UCRT64 ffmpeg で stream 全体を復号する。encoder callback の回数を frame count にしない。
全 callback の全 RGBA channel を比較し、さらに一意な output frame の全域を検査する。

既知 RGB の実動画、PCM 音声、字幕、Graph opacity を同じ Project に配置する。
Graph の有無で AAC を復号した PCM の byte 完全一致を確認する。無効 Graph に不正式を残しても
書き出せることを確認する。preview の resident 状態が残っていても、disk の画素変更・Draw 欠損を
拒否する。取消と shutdown では最終出力と一時出力を残さない。

## MLT oracle の追加検証と適用範囲

実 Manim の非一様 PNG は、以前の一様画素 fixture にない alpha 112 を含んでいた。
UCRT64 MLT 7.36.1-1 の Release build は `-ffast-math` を指定しており、実 DLL の nearest 命令列は
定数除算を binary32 の逆数 `0x3b808081` の乗算へ変換する。
`as = F(F(F(opacity) * F(1/255)) * Sa)`、`ad = F(Da * F(1/255))` の順に評価し、
P4-5.1 の合成 alpha・weight・RGB の演算ごとの binary32 と byte 切り捨てを続ける。
白・alpha 112 を不透明黒へ合成すると `[112,112,112,254]` となる。直接除算では最後の alpha が違う。
この発見はテストの独立 oracle に反映した。renderer、artifact、MLT の製品実装は変更していない。

AlphaDomain は全 byte alpha と黒／有色の不透明背景を同じ DLL の各境界で比較する。
RealPng と製品受入では実 Manim の全画素を比較する。数式 oracle は記録した版・実 binary・
identity の整数画素配置と opacity 1/0.5 で実証した経路だけに適用する。
実 consumer の既定 rescale は bilinear だが、非整数の空間 filtering へこの数式を一般化しない。
位置・scale・rotation・crop・空間 keyframe は、Graph が stage した同じ PNG bytes を
通常 Image へ渡す differential で全画素を比較し、独立数式 oracle と区別する。
両入力 alpha 0 の除算は未定義域として拒否する。

診断専用 C source は現在の静的 library に維持する。`nm` で製品 exe に診断 symbol がなく、
検証 exe だけに存在することを Review gate に残す。未参照 object を製品へリンクしないため、
移設だけを目的にした refactor は行わない。

## 負例と変異の対応

| 元の負例 | 検査／controlled mutation |
| --- | --- |
| local frame と source frame の混同 | trim・mixed FPS の独立 literal、source-frame 変異 |
| split 後の Draw 再開 | 実 UI split・Project split 後の encoder 全 frame 比較 |
| Draw を static へ置換 | Draw frame 0／途中／endpoint の異なる画素比較 |
| 画面外の不正式が全体を阻害 | visible-intersection 変異、offscreen 対照 |
| 可視の不正式を無視 | focused typed failure と実 UI 保存再読込の export 拒否 |
| preview の透明 fallback を使用 | package 欠損は ArtifactMissing、raster は空 |
| 編集後に旧 key を使用 | 原文変更後の backend 不在拒否、provenance 不一致 |
| decoded SHA を省略 | decoded-integrity 変異、実 UI の valid PNG 画素変更 |
| Draw frame 欠損を見逃す | focused と実 UI の frame 欠損拒否 |
| artifact の順序を交換 | source-frame 変異と全 source ledger 比較 |
| alpha を 255 へ置換 | opaque-alpha 変異 |
| ClipEffects を二回掛ける | double-effects 変異、通常 Image の同一入力対照 |
| track の順序を逆転 | multilayer／reordered exact 比較、P4-5.1 の layer 順変異履歴 |
| 未対応 transition を hard cut にする | source handle を持つ Graph transition の typed 拒否 |
| encoder callback から backend を呼ぶ | backend-from-encoder 変異、準備呼出数の検査 |
| 取消後に公開 | final-cancellation 変異、実 UI 取消と shutdown |
| 失敗で既存 MP4 を壊す | 検証済み MP4 の byte 保存、publication-error 変異、実 rename 失敗 |
| 非可視の独立 static/Draw を要求 | Draw package endpoint の再利用と backend 呼出数 |
| key 共有で所有者の時間を共有 | 二所有者の独立 source mapping |
| oracle 失敗を無視 | encoder validator の意図的 false で非公開・既存出力保存 |
| shutdown 後の stale 完了通知 | stale-completion 変異と controller lifecycle 回帰 |
| 逆数乗算を直接除算へ変更 | reciprocal-normalization 変異、alpha 112 の固定対照 |

変異は一箇所だけ変更し、build 成功と狙った assertion の失敗を要求する。
クラッシュ・timeout・build failure は検出成功に数えない。finally で元の byte 列と SHA を復元し、
再ビルド後に同じ試験が通ることを要求する。

## 保存した失敗の原因

再開中の全 FAIL/INVALID directory は削除・上書きしない。threads=1 を試した診断は
同じ alpha 差分を残したため、production への追加を撤回した。旧 P4-5.1 証拠は再解釈しない。
全画素不一致の原因は上記の実 DLL の逆数乗算であり、許容差や合成経路変更で隠していない。
初期の callback 回数の検査は同一 frame の複数取得を考慮していなかったため、
全 callback の exact 比較・一意 frame の coverage・MP4 の実 frame count に分けた。
fractional FPS の初期期待式は sourceIn へ local の floor を足しており、P4-1 の ceil origin を
使っていなかった。実装を変更せず、独立期待式を半開境界へ直した。
失敗ダイアログを閉じず修復操作を送っていた試験は実 OK ボタンを押してから続行するよう修正した。
追加 transition 負例は source handle がなく一般範囲検査で先に落ちていたため、正しい余白を付けた。
非出力 Graph の選択は clip index 固定と preview 更新の戻り値に依存していたため、永続 ID と
実 inspector の選択・原文入力を検査するよう修正した。

## Gate と provenance

保存した再開後の FAIL は次の新規 directory に残す。終了コードと生ログは各 `commands.json` から取得できる。

| 証拠 directory | 因果と修正 |
| --- | --- |
| `math-p45-20261010-010734-Real` | 初期診断。実 PNG の alpha 差と待機失敗。途中の source 更新があるため最終 source の受入証拠にしない |
| `math-p45-20261010-010909-Focused` | 追加の原子的公開境界の取消判定。通知回数の仮定を除去した。後述の変異で rename 対照も境界を区別しないと判明し、さらに修正 |
| `math-p45-20261010-011210-Real` | Ready 前の duration 編集と alpha 差。artifact Ready 後の正式 tail trim に変更。debugger 観測は診断のみ |
| `math-p45-20261010-011908-Real` | threads=1 でも alpha 差が残った。仮説を否定し production の追加を撤回 |
| `math-p45-20261010-012420-RealPng` | 実 Manim の alpha 112 で旧 oracle と実 DLL が相違。逆数乗算の命令順を独立期待値へ反映 |
| `math-p45-20261010-012814-AlphaDomain` | alpha 0 に非ゼロ hidden RGB を持つ不正 fixture。既存 canonical PNG 契約に合わせて全 channel 0 に修正 |
| `math-p45-20261010-013140-Real` | 画素一致後、複数 callback を frame 数として数えたため失敗。一意 frame と MP4 実数を別途検査 |
| `math-p45-20261010-013951-Real` | 失敗ダイアログが修復操作を阻害。実 OK を押して閉じる |
| `math-p45-20261010-014312-Real` | fractional FPS の独立期待式の origin が誤っていた。P4-1 の ceil 境界を使用 |
| `math-p45-20261010-015519-Real`、`math-p45-20261010-020438-Real` | 無効 Graph の選択戻り値で入力前に短絡。永続 ID と実選択内容で検査 |
| `math-p45-20261010-020244-Focused` | transition 負例の source handle 不足。一般範囲を満たす対照へ修正 |

これらは `build/` の下に保持する。元の FAIL を PASS に書き換えず、修正後の新規取得だけを閉鎖判定に使う。

`math-p45-20261010-021918-Mutations` では final-cancellation が未検出だった。
avformat は C backend の完了通知前にファイルを閉じうるため、rename の可否だけでは
アプリの公開直前と区別できなかった。試験は Windows unwind identity と実 stack で C backend
内の通知を区別し、その間は取り消さない。backend が返った後のアプリ側通知だけで取り消し、
取消理由も「最終公開前」であることを要求する。通知回数や待機時間で判定しない。

閉鎖集計自身の負例は `math-p45-20261010-ClosureNegative` と
`build/math-p45-closure-negative.log` に保存した。controlled mutation 中の
`src/app/timeline_export.cpp` の SHA 差を検出し、終了コード 1 で拒否した。
最終 source の変異は `math-p45-20261010-023058-Mutations` に保存した。
検出・byte 復元・復元後の同一試験の正常終了は `mutations.json` と `result.json` を参照する。

`math-p45-gate.ps1` は毎回新規 directory に source 全 byte と各 SHA、集合 SHA、HEAD、
実コマンドと終了コードを保存する。`math-p45-mutations.ps1` は同一 source の Focused を要求する。
`math-p45-closure-report.ps1` は必要な全 stage、実製品の非空の検査、変異の検出・復元、
現在 source と全 gate の同一 SHA を検査し、生 JSON／ログから件数を再集計する。

## 取消の偽陽性と閉鎖

`math-p45-20261010-022437-Real` は 202 件失敗 0 と記録されたが、取消の最終 status は
文字 Preview の FFV1 失敗のままだった。保存済み Project の先頭式は `sin(` であり、
出力ファイルが無いことだけを取消成功にしていた。この directory は閉鎖判定に使わない。

その後の失敗も残す。

| 証拠 directory | 因果 |
| --- | --- |
| `math-p45-20261010-031601-Real` | `unique_ptr` を `QObject::connect` に渡し、compile 失敗 |
| `math-p45-20261010-031703-Real` | 修復が Project に残らず、取消完了の前に式の compile 失敗 |
| `math-p45-20261010-032158-Real` | FFV1 の preview 更新失敗で `selectClip` が false を返した。選択そのものは残る |
| `math-p45-20261010-032741-Focused`、`math-p45-20261010-032752-Mutations` | console の CP932 が UTF-8 の試験出力を壊し、変異文言の照合が空振りした。byte 列は復元済み |

[事実] 2026-10-10、`math-p45-20261010-032445-Real` は 205 件、失敗 0。
取消は status 列 `書き出しています…`、`キャンセルしています…`、`書き出しをキャンセルしました` を同期的に記録し、最終出力は無い。
shutdown は `書き出しています…` のあと最終出力が無い。
修復後の Project から `sin(` は消えている。

同一 source `0791B37C7E976A2A0C089E733F787868C863315AF3AB57A8C705C0AE8EECFAC8` の機械集計は
`build/math-p45-20261010-040724-Closure`。Focused は diagnostic 54、oracle 216、differential 1068、
focused 72、encode 16。AlphaDomain は 4608。RealPng は 7。回帰は 17。
変異は `math-p45-20261010-033029-Mutations` で検出 10/10、byte 復元 10/10。
BuildIndependent は 1084。lint は通過。通常 release は 1494。
Review は製品 exe に診断 symbol が無く、検証 exe にだけあることを確認した。
診断専用 source は静的 library に残す。製品の範囲指定 export は追加していない。
