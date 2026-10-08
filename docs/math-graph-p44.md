# P4-4: Graph clip の製品編集

状態: **P4-4 PASS/CLOSED**。P4-5 は未着手。schema23、renderer の変更は対象外。P3 と P4-0〜P4-3.2 の歴史的判定は維持する。
`stableGraph` / `graphMemo` の再利用条件を変更しない。コミット・push は行わない。

製品のファイルメニュー「グラフ clip を追加」から作成し、選択した clip の
エフェクトコントロール内で `GraphClipInspector` を開く。表示範囲、軸とグリッド、
TeX ラベル、1〜3 関数、ARGB 色と基準 pixel 線幅、任意の定義域、Draw の整数 frame 数を編集する。
パネル全体の既存 `BoundedFlickable` で縦にスクロールする。狭い幅では範囲を一列にし、
追加・削除・並べ替えのボタンを折り返す。破壊的操作を hover に依存させない。

## 編集の authority

`MvmController::selectedGraphClip` は表示用 snapshot。QML は Project や JSON を直接変更しない。
`editGraphFromUi(authority, functionId, operation, values)` は snapshot の clip ID、Project 世代、
revision と現在選択を照合する。関数は `GraphFunctionId` で指定し、行番号で編集しない。
構造検証は既存 `project::editGraph` と関数編集 API を通す。確定は既存 Project 履歴へ一回だけ積む。
同値の確定は既存の履歴契約により Undo を増やさない。拒否は Project と履歴を変えない。

入力欄はローカルの文字 draft を保持し、Enter または focus loss で確定する。
不完全な数値は確定を拒否して文字と診断を残す。Escape で確定値に戻す。
別の関数・clip に移った draft は世代を含む identity ごとに保持する。
古い revision からの確定は拒否し、別の対象に適用しない。
数式の空白・空文字・不正構文・未対応識別子は原文のまま Project に保存できる。
数値式の compile 成功を保存条件にしない。色は alpha を保って大文字の ARGB に確定する。
空の定義域欄は optional の未指定であり、ゼロと区別する。

## 非同期 preview

確定後は既存 `requestMathRenders` と `refreshTextPreview` へ進む。
GUI slot から同期 Manim 生成を呼ばない。描画状態・診断は Undo に入れない。
`graphStatusFromUi` は既存の読み取り専用 `graphPreviewStatus` を照会する。
typed reason と job、current key、source frame を返し、本文から状態を推定しない。
Ready は compile 有効・検証済み snapshot・exact frame resident・fallback なしをすべて要求する。
正当な alpha-zero の resident frame は Ready にできる。
表示中のパネルだけが 250ms 間隔で読む。状態の問い合わせから描画を要求しない。

## 検証入口

操作可。QML は offscreen、製品受け入れは背面・非フォーカス・入力透過の window と
既存 display-power lease を使う。

```powershell
pwsh scripts/math-p44-focused.ps1
pwsh scripts/math-p44-focused.ps1 -Stage Real
pwsh scripts/math-p44-mutations.ps1 -EvidenceDirectory build/<新規 directory>
```

集中試験は原文・構造拒否・履歴・保存再読込・ID・alpha・Draw・読むだけの状態を検査する。
Basic と Windows style の QML 試験は `failOnWarning` を維持し、通常／狭い／低い／両方の
各寸法で必要な操作と最下部へ到達できることを検査する。
共通のカスタム入力部品三種は native style の未対応カスタマイズを避けるため Basic を明示する。

製品受け入れは `Main.qml` の実メニューとキー・クリック入力から関数を作る。
保存した同じ Project を原寸 native surface で描き、検証済み RGBA PNG に独立の整数式で
黒背景を合成した期待値と全画素比較する。source の JSON を直接編集して authoring を代用しない。
結果・過去失敗・gate は [結果文書](math-graph-p44-results.md) に残す。

## 閉鎖

基準 HEAD は `39f8995371db625907f1ce9c13ed56bf248a668b`。
`stableGraph` / `graphMemo` は変更していない。コミット・push は行っていない。

製品の作成、関数の追加・削除・並べ替え、範囲・軸・色・ラベル、編集可能な不正原文、
Undo/Redo と安定した関数 ID、保存と再読込、Draw 途中の trim/split と同じ source frame、
現在 key の非同期 preview、実 Manim の native 全画素、低い/狭い panel、負例と変異は、
最終 source の次の取得で閉じる。

|gate|証拠|結果|
|---|---|---|
|集中 controller / QML|通常 release に含まれる四件。先行の Final-Focused は修正前 source の失敗として保存|release で通過|
|製品 UI|`build/math-p44-20261009-Final-Real`。UI 99 検査、原寸 5 frame|失敗 0|
|変異|`build/math-p44-20261009-Final-Mutations-02`。6/6 検出、hash 復元、復元後の試験通過|通過|
|BuildIndependent|`build/math-p44-20261009-Final-BuildIndependent-02`。1084 件|失敗 0|
|lint|`build/math-p44-20261009-Final-Lint-02`。変異ログの UTF-8 捕捉を含む|通過|
|通常 release|`build/math-p44-20261009-Final-Release`。1489 件|失敗 0|

最初の BuildIndependent は Escape 修正前の QML なので、最終 QML の独立 gate には数えない。
Final-Mutations は照合用ログの文字化けであり、製品の拒否が消えた証拠ではない。
製品の apps / src / tests は release、Final-Real、BuildIndependent-02 と SHA256 が一致する。
その後に変えたのは、変異ログを UTF-8 で残す script と結果を再計算する script だけで、Lint-02 が検査した。

P4-5 へ残すもの: H.264 / video export、schema 23、新しい式構文、adaptive sampling、
renderer の変更、静止 artifact の物理共有、Graph の新しい transition、任意の Python / Manim script、
parametric / polar / 3D。Graph clip はまだ export 対象ではない。
