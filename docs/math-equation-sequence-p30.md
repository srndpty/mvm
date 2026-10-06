# Equation Sequence と部分式の強調: P3-0

本書の P3-0 / P3-0.1 は当時の設計と証拠として閉じている。P3-1 の実装と現在の schema は
末尾の「P3-1 実装」で別に記録する。以下の schema 20 の記述は過去の段階を指す。

2026-10-06。P2 MathTransform は PASS/CLOSED のまま。本書は設計提案と製品外の renderer 検証であり、
Project schema 20、製品 UI、preview、export、cache publication は変更しない。
P0/P1/P2 の実装・証拠を維持する。実装の段階と未解決事項の管理先は [roadmap.md](roadmap.md#数式-clip)。

P3-0.1 は文書だけの明確化。状態削除時の action の原子的削除、sequence 内の ID の範囲、
透明背景、初期 operation を outline/pulse に限定する規則を以下へ反映した。
既存の renderer 証拠との矛盾はなく、spike は再実行しない。

## 推奨する所有境界

**新しい `TimelineClipKind::EquationSequence` を推奨する。** 一つの native clip が順序付きの状態、
内部の変形、部分式の ID、強調操作を所有する。Manim は renderer であり保存形式の正ではない。
式の一状態には既存 `MathClipData` の値の契約を使い、位置・拡大・回転・clip 全体の不透明度は外側の
`ClipEffects` が所有する。内部には track、隣接 clip ID、cut 前後の handle を持ち込まない。

|候補|正・編集・時間|再利用と将来性|判断|
|---|---|---|---|
|A: 新しい EquationSequence kind|一つの clip に状態と参照を閉じ込め、一編集を Undo 一回にできる。内部の時間域を明示できる|状態の静止描画と変形処理を再利用。部分式への action を独立に保存可能|採用提案。kind を増やす責任と新しい時間域の検証が必要|
|B: MathClipData に sequence を追加|現行の単一 `source` と sequence のどちらが正か、二重管理になる。variant 化しても既存 Math の静止・Write の意味が変わる|値のコードを共有できるが、旧 Math に trim・split・action の条件分岐が広がる|不採用。単式 Math の契約を保ち、値の小さな部品だけ共有|
|C: 複数 Math clip と TimelineTransition を高位 editor で束ねる|track 上の隣接関係と cut が正になる。移動・削除・コピーのたびに束の整合が必要で「一つの clip」の時間にならない|P2 の UI を使えるが、強調参照と保存時の束の同一性に別の永続概念が必要|不採用。既存 P2 は独立した編集方式として維持|

[事実] `src/project/project.h` の Math は素材を持たない静止 kind で、`sourceInFrame=0` の合成時間域を使う。
`timeline_edit.cpp` の左 trim は Write を新しい先頭から開始し、split も静止 clip の規則を使う。
Sequence にそのまま適用すると導出を最初から再開してしまうため、`isStillClipKind` に追加するだけでは足りない。
同ファイルの Project FPS 変更は clip の開始位置を換算するが、素材 FPS と素材範囲は維持する。
これらを P3 実装時の明示的な分岐点にする。

## 保存候補と時間の正

以下は説明用の形であり、公開ヘッダや JSON の変更ではない。

```cpp
EquationSequenceClipData {
    vector<EquationState> states;
    vector<EquationStepTransition> transitions;
    vector<EquationAction> actions;
}
EquationState { StateId id; MathClipData equation; FrameCount holdFrames; vector<SemanticPart> parts; }
EquationStepTransition { TransitionId id; StateId from; StateId to; FrameCount frames; vector<PartPair> correspondence; }
SemanticPart { PartId id; string label; SourceBinding binding; }
SourceBinding { SourceDigest revision; ByteOffset begin; ByteOffset end; string expectedText; BindingStatus status; }
EquationAction { ActionId id; StateId state; PartId target; FrameOffset start; FrameCount duration; NeutralOperation operation; }
```

`states` の vector が順序の正。transition は連続する二状態だけを ID で参照し、状態を複製しない。
状態数 n に対して n−1 個を持つ。hold は一 frame 以上、変形は一 frame 以上。
初期段階は内部の cut を種類として足さない。先頭の Write は別の intro 区間として後段で検討し、
現行 `MathClipAnimation` の「見えている先頭から始まる」意味を流用しない。

### 内部 ID の範囲と独立所有

StateId、TransitionId、ActionId、PartId はすべて **一つの EquationSequenceClipData 内で、
それぞれの ID 種別ごとに一意** とする。PartId は state 内だけでなく sequence の全 state を通じて
一意とする。種別間では同じ文字列を許すが、型を区別して解決する。Project 全体での一意性は要求しない。
既存の TimelineClip の ID は Project 全体で一意だが、内部参照は sequence の外へ出さないため、
その規則を内部 ID へ広げる具体的な理由はない。
action の対象 `(StateId,PartId)` は、その sequence の既存 state とその state が所有する part を解決する。
別 state に同じ PartId を探して補うことはしない。

コピー・paste・複製は新しい外側 clip ID と全内部 ID を発行し、transition の両端、correspondence の
part 参照、action の state/target を一括 remap する。missing PartId の修復可能な参照も新 ID へ
一貫して remap し、参照が欠けている状態を保つ。同じ missing ID を参照する action は同じ新 ID を共有する。
再発行時は実在 ID と保持中の missing 参照の ID に衝突させない。

split は左片に元の外側 clip ID と全内部 ID を残し、独立所有する右片に新しい外側 clip ID と
全内部 ID を発行して同じ規則で remap する。両片は完全な sequence を持ち、可視 source 範囲だけを
境界で分ける。state 単体の複製でも、その state と part と所有 action に新 ID を発行する。
保存・再読込と Undo/Redo は確定した ID と remap 結果を復元し、再発行しない。
これらの所有 ID は cache key に含めず、描画の意味と正準化した参照関係だけを材料とする。

### 初期の背景・style の不変条件

P3 初期では **すべての EquationState の Math 背景を透明 `#00000000` に限定** する。
既存 MathClipData の値の契約に加え、この sequence 固有の制約を検証する。背景補間は定義しない。
不透明・半透明の背景を状態に持たせることや、その補間を暗黙に renderer へ任せることは拒否する。
状態ごとの font size と foreground color の違いは、静止・変形端点・補間を renderer 契約が
保証する範囲で許す。非対応の組合せは明示的に失敗とし、同じ style へ黙って揃えない。
外側の配置・scale・rotation・opacity は Sequence clip の ClipEffects だけが所有する。
今回の spike は font size の変化を扱うが、P3 の foreground color 契約を新たに検証した証拠ではない。

**hold の尺を変形が消費しない。** `H0, T0, H1, T1, ... Hn` を隙間なく順に並べる。
`h[i]` と `d[i]` は整数 frame。`stateStart[i] = Σ(j<i)(h[j]+d[j])`、
全長 `L = Σh + Σd`。蓄積は overflow を検査する。絶対開始位置は派生値で保存しない。
action は state の hold 先頭からの相対 frame とし、`0 <= start`、`duration >= 1`、
`start+duration <= holdFrames` を要求する。P3 の初期契約では変形中・状態をまたぐ action を拒否する。

全区間は `[begin,end)`。変形が frame t に始まり N frame 続くなら、t+i (0≤i<N) は進み具合 i/N、
その次の t+N は target の hold の最初の frame。P2 と同じく進み具合 1 は連番に含めず、
静止端点の別 artifact と照合する。N=1 でも source と次の target の二つの標本になる。
easing は中立な列挙値と版で決め、backend の既定値を正にしない。
spike の 0..12 は端点比較を含む直接標本であり、製品の 12 frame 連番の受け入れではない。

時間は clip の `sourceFpsNum/Den` と整数 frame を使う native な内部時間域。
これを作成時の Project FPS に初期化し、`sourceFrameCount=L`、可視範囲を
`[sourceInFrame,sourceOutFrame)` とする。mvm の `clipTimebase` が Project frame との対応を決め、
Manim の run_time 秒は backend 内の正規化だけに使う。JSON に Manim 秒を保存しない。

|外側の操作|提案する意味|
|---|---|
|移動|timeline 開始だけを変更。内部の時刻・ID・描画 key は不変|
|左右 trim|可視範囲だけを変える。左 trim で導出や action を再開しない。L の外へ延長は拒否。状態の hold を伸ばすのは内部編集|
|split|完全な sequence を両片へ独立コピーし、可視範囲を境界で分ける。左片は元の ID、右片は新 ID と参照の remap。途中の変形・action も同じ内部時刻から続き、右片の seek に履歴実行は不要|
|コピー・複製・paste|新 clip ID と全内部 ID を生成し参照を一括 remap。同じ式でも別所有者。描画に同じ入力なら cache は共有可能|
|速度変更|P3 初期は拒否。将来追加時は正の有理数による `clipTimebase` の mapping のみを変え、内部 frame を書き直さない|
|Project FPS 変更|既存の素材時間域と同様、内部 FPS と frame は維持し、timeline 開始を既存の境界換算で変更。同じ秒数を新 Project FPS で標本化。action が一 output frame にも現れないなら明示的な診断を出す|
|内部の尺変更|hold/transition/action を明示編集。全体末尾は新 L へ追従し、右 trim は保持して範囲外なら拒否（P3-1.1）。action を暗黙に移動しない|

P3-1 は以下の **純粋な評価契約の確定と検証** を担当する。

```text
出力 timeline frame
  → sequence の source / 内部時刻
  → hold / transition / action の区間
  → 区間内の local frame と進み具合
```

有理数 FPS の換算、frame 境界の丸めと標本位相、trim/split の境界、整数演算の overflow を
一つの契約へ固定する。区間探索前に内部時刻を丸めるか、有理数のまま比較するかも明示し、
境界の片側だけを別の規則で判定しない。初期は retime を拒否するが、FPS の異なる出力の
renderer sampling はこの契約に従う。P2 の枚数だけを変える方法を
流用する場合も、出力 frame の格子に対する位相を key に含めなければならない。
この対応の実装検証は P3-0 の対象外で、P3-1 の gate とする。

## semantic part の最小の同一性

**source と別の named annotation を保存し、永続 PartId と revision 付き範囲を持つ方式を推奨。**
範囲は UTF-8 byte `[begin,end)` とし、codepoint 境界を検査する。UI の UTF-16 offset は確定時に変換する。
名前は表示用であり一意性の正ではない。action の対象は `(StateId,PartId)`。式の文字列検索ではない。
`expectedText` と source digest は範囲が別 revision に誤適用されないための binding の証人。
full TeX AST、代数的同値判定、glyph の番号は保存しない。

|方式|評価|
|---|---|
|裸の byte/codepoint range|最小だが先頭挿入でずれる。重複や編集履歴を表せない。ID と revision 検査なしでは採用しない|
|named annotation と stable ID|source をそのまま保持できる。編集 delta と検証で範囲を更新でき、保存・再読込で同じ対象を維持する。推奨|
|軽量 token/part node|自由 TeX を断片へ分けた node を正にすると構文をまたぐ編集が難しい。将来の構造 editor の候補。現段階では renderer 入力へ派生する平坦な分割だけ|
|`{{...}}` / `\mvmpart`|backend の group を生成する手段。Project の意味にすると renderer 固有の空白・構文規則へ依存する。保存形式にはしない|

P3 初期は連続・非空・非重複の annotation を扱う。ネスト、離れた複数範囲の集合、TeX 命令途中の境界は拒否。
完全な parser なしで「TeX が描けた」は選択の正しさを保証しない。backend は group 構造、
source の対応、glyph の存在、静止との画素配置を検査し、描けても同一性を保証できない範囲は拒否する。
分数の分子内部の `b^2-4ac` は今回の実測で支持されるが、任意の TeX macro まで保証しない。

|編集ケース|保証と失敗の規則|
|---|---|
|part より前へ挿入・削除|信頼できる editor delta で offset を移し同じ ID を維持。境界ちょうどの挿入は選択の外側とする|
|part の内部を編集・境界を横切る削除|ID を残し binding を `invalid` にする。明示 rebind で revision/text/range を更新するまで action は無効|
|同一部分式の重複|別 ID と別範囲。検索で別の出現へ移さない。明示 correspondence も一対一の ID 参照|
|source 全体の置換・履歴のない外部編集|自動追跡しない。source の変更は保存できるが既存 binding は invalid。文字列が偶然一致しても自動復活しない|
|テキストだけ copy/paste|新 source の文字として扱い ID を運ばない。part は明示的に新規作成|
|構造ごとの copy/paste・state duplication|state/part/action の ID を新規発行し内部参照を remap。外部の state への correspondence は自動生成しない|
|保存・再読込|ID・範囲・revision・明示 invalid 状態をそのまま復元。backend の glyph 対応は再生成|

既存 state 内の part を消したときは action の target の PartId を残し、`missing` と診断する。
この PartId の欠落と invalid binding は明示的に修復可能な対象として保存できる。
一方、**有効な EquationAction record の StateId は必ず既存 state を参照する**。
state の削除は、その state を所有者とするすべての action を同じ Project 編集・Undo transaction で
原子的に削除する。missing StateId は修復可能な参照として保存せず、読み込み・確定時に拒否する。
初期 P3 に orphan-action store は導入しない。
構造上の危険なデータ (各 ID の範囲内での重複、範囲外の bound range、未知 operation、
欠落 StateId、別 state の PartId を参照する action) は JSON の読み込みで拒否する。
Project の構造検証と「出力可能」の検証を分ける。参照の無効な sequence は export を拒否する。

P2 の n 番目の出現の照合は依然として派生値。P3 の明示 correspondence は利用者の意味の指定なので
保存候補とし、一対一・隣接 state・有効 part を検査する。指定しない領域には P2 の照合を使えるが、
それによって action の永続対象を作らない。選択領域と自動 segment の交差から描画 partition を作り、
同じ glyph を二重に Transform しないことを新しい compiler contract に要求する。

## 初期 action は一時的な強調区間

瞬間の imperative event を再生履歴の正にしない。すべてに start と duration を持たせ、任意 frame の
評価が seek 順序に依存しないようにする。表示名と保存用 operation を Manim の class 名から分ける。
**初期 P3 の保存対象 operation は `outline` と `pulse` だけ** とする。

|中立な operation|見た目の意味|区間の後|
|---|---|---|
|`outline`|選んだ部分の周囲に線を描いて消す。Circumscribe に対応可能|元の状態|
|`pulse`|選んだ部分の拡大と強調色を一時的に変える。Indicate に対応可能|元の状態|

action が存在しない対象は state の基本色・opacity 1。
P3 初期は同じ対象への区間重複を拒否する。
異なる対象の同時 action は将来の合成 gate を通すまで拒否する。保存の順番で勝者を決めない。
一時 outline/pulse は区間の直後に基本 state に戻る。境界の中途半端な効果を次の式へ暗黙に運ばない。

`set_color`、`reveal`、`conceal` は将来の設計候補だけとする。部分色や可視性を持続させる場合の
styled endpoint の保存・時間評価・次の変形への受け渡しを仕様化し、renderer で検証するまで導入しない。
初期 schema ではこれらの operation 値・field・既定動作を予約せず、読み込み時も未知 operation として拒否する。
本書の概念説明だけでは、その振る舞いを保存契約として承認したことにならない。

線幅・余白は clip の local px、色は既存 `#AARRGGBB`、easing は中立な有限列挙。
将来 geometry/graph は `ObjectId` の対象を同じ区間評価へ接続できるが、P3 で汎用 scene graph は作らない。
outline/pulse の operation だけを共有し、数式の source binding を他領域へ押し付けない。

## renderer 検証と証拠

実行 script は [scripts/spikes/math-p30.py](../scripts/spikes/math-p30.py)。製品の Project、cache、Qt、MLT を
import せず、実 Manim 0.21 の MathTex、Cairo camera と animation interpolation を使う。
latex/dvisvgm の実行で現在の MiKTeX を使う。PNG は試験の直接標本であり cache artifact として公開しない。
二次方程式 S0..S5、重複項、分数、left/right、glyph を作らない断片を二つの font size で調べる。
変形の明示対応は mvm 側に固定した番号表を使い、Manim の TransformMatchingTex に照合を任せない。

再現は通常操作可。以下の Python は既に導入済みの Manim tool の interpreter で、依存を追加導入しない。
出力先が存在すれば起動を拒否する。新しい名前で実行する。

```powershell
# 【操作可】画面の表示・音声・性能計測は行わない。
& "$env:APPDATA\uv\tools\manim\Scripts\python.exe" scripts/spikes/math-p30.py build/math-p30-<新しい名前>
```

[事実] 初回 `build/math-p30-20261006-a` は Circumscribe の `_setup_scene` を呼ばず
`AttributeError` で停止した。これは spike の scene 初期化不足で、製品の失敗ではない。
出力は維持し、修正後は別の directory と log に保存した。
数値の正は修正後の `results.json` とそこから script が生成する `summary.md`。
集計値を本書へ手で転記しない。

[事実] 最終の実行は `build/math-p30-20261006-d` (終了コード 0)、log は
`build/math-p30-20261006-d.log`。生データは [results.json](../build/math-p30-20261006-d/results.json)、
自動集計は [summary.md](../build/math-p30-20261006-d/summary.md)。
Manim 0.21.0、MiKTeX-pdfTeX 4.27 (MiKTeX 26.5)、dvisvgm 3.6 を実際に使用した。
args は全ケースで構造と静止 alpha が一致し、明示対応の変形も source/target の両端点が一致した。
最終式の判別式は複数 glyph の単独 part に対応し、outline/pulse は対象外の点を変えずに画素が変化した。
[outline の中間標本](../build/math-p30-20261006-d/outline-96-06.png) も目視確認した。
braces は一部で描画例外になり、描けても分数内部の part が単独 group にならない場合があった。
isolate は基本画素が一致するが top-level の parts は式全体であり、番号を semantic target として使えない。
SVG fallback の実発生は観測しておらず、拒否は人工的な報告を使う負の対照で確認した。

[事実] `...-b` は outline の表示対象を誤って camera に渡し、「画素が変化しない」を検出して
終了コード 1。`...-c` は Succession の終端で active animation が None になる処理不足で停止した。
両方の log と出力を保持した。d はこれらの spike の修正後の実行であり、無変更の retry による選別ではない。
MiKTeX は sandbox でユーザー領域の log 書き込み拒否と更新確認の案内を出したが、TeX/SVG/PNG の
生成は完了した。外部 log の書き込み成功を確認したとは扱わない。

公式 API の比較資料: [Manim 0.21 MathTex](https://docs.manim.community/en/stable/reference/manim.mobject.text.tex_mobject.MathTex.html)。
以下の結論は API の存在だけでなく、この spike とローカル 0.21 の `tex_mobject.py` の実装確認を根拠にする。

|backend の手段|P3 での使い方と制限|
|---|---|
|`MathTex(*segments)`|source と annotation から作る partition を一回の TeX 処理へ渡す。型・文字列・数・fallback を検査した後、part 番号を一時的な renderer handle として使う。推奨|
|backend 挿入 `{{...}}`|空白だけの group と text の空白を作りうる。source の文脈で開始条件が変わるので ID と番号の直結は不可。積極的な利点がなく初期契約で採用しない|
|explicit mobject selection|検証した part の番号から複数 glyph の対象を取る。文字列検索や最初の一致を使わない。複数同一式のうち一つだけを扱える|
|P2 segmenter/matcher|top-level の自動分割と n 番目対応を未注釈範囲で再利用する。分数内部を semantic part として自動発見しない|
|`substrings_to_isolate`|文字列への一致であり occurrence ID の正ではない。命令の内部へ一致すると式を壊しうるため、Project の target 解決に使わない|

検査は count/text/fallback を壊した負の対照と、glyph 0 の強調対象の拒否を含む。
構造の存在だけで成功にせず、args の全 alpha 画素を単一文字列の静止参照と比較する。
変形では両端点と静止の比較、強調では対象外の点の不変と PNG の時間変化を確認する。
ただし raw SVG から glyph 全体の排他的所有を一般に証明する契約、任意 macro、rendered CLI の
連番枚数、製品 decoder、D3D11、export はこの spike の検査範囲ではない。

## 再利用と新しい renderer 契約

|既存要素|再利用の範囲|新しく必要なもの|
|---|---|---|
|MathRenderSpec / 静止 endpoint|source・syntax・font の基本静止。注釈が基本画素を変えなければ同じ静止 key|annotation→glyph の map は別 artifact。静止 key に永続 ID を混ぜない|
|MathTransformSpec|注釈・明示対応・action がない二状態の変形はそのまま利用可能|明示 partition、対応表、annotation の作用する endpoint は別の `EquationTransitionRenderSpec` と key namespace|
|segmenter / matcher|既存の版を固定して未注釈領域の候補を作る|注釈境界との partition 合成、排他的な glyph 所有、明示対応の優先順位と検証|
|transform renderer|プロセス制御、TeX setup、ReplacementTransform/fade、端点位相補正、bbox 検査|spec を中立 plan へ compile し Manim 内部番号へ変換する adapter。現行関数の暗黙の自動分割は変えない|
|A8 artifact|基本の白 mask、サイズ・枚数・hash・provenance の検査、配置・decode|強調色や部分色は単一 A8 で表せない。基底/部分/装飾の複数 A8 layer と数値色/opacity を持つ新 artifact 契約|
|shared residency budget|Write/transform の実際の生存予約方式|全 layer の byte と同時 frame を同じ予算で予約し、途中の layer だけ ready にしない|

`EquationEvaluationPlan` (名前は候補) は Project から app 層が抽出する renderer 中立な要求。
`src/media/math` は Project/Manim/Qt に依存しない。Manim adapter は `src/media/manim` に置く。
全 sequence を一つの動画に焼かず、状態静止・変形・action の小さな依存 DAG を派生する。
同時に、新しい layer 合成と任意 seek の正しさを検証するまでは単独 action artifact の再利用を保証しない。

cache key は renderer に効く正準入力 (source/font、partition、対応、operation/param、標本数/位相、
compiler/renderer の版、toolchain) を含める。clip/StateId/TransitionId/ActionId/PartId 自体ではなく正準順序で参照を
解決し、意味が同じ複製を共有できるようにする。色は合成-only ならその layer の mask key には不要だが、
operation が renderer の形状を変えるなら入力へ含める。
ID→派生 handle の map と cache key を Project に保存しない。旧 A8 format の意味は変更せず、
新 artifact は別 namespace/version。権限・世代・取消・atomic publication の既存原則を維持する。

## 編集の不変条件

|編集・失敗|期待する処理|
|---|---|
|state の挿入|新 ID、新 hold を作る。旧 edge を取り除き新隣接 edge を明示生成。一回の Undo。対応/action は自動流用しない|
|state の削除|state、両側 edge、その state が所有する全 action を同じ編集・Undo transaction で原子的に削除。残る隣接に新 edge を明示作成し、旧対応を移さない。orphan action は残さない|
|一状態の source 変更|その静止、前後 edge、その状態の glyph map/action を無効化。範囲を delta で安全に移せない part は invalid。Project の確定を描画失敗で巻き戻さない|
|font/color の変更|font は mask/map/前後 edge/action を再描画。色は合成層で反映できる範囲のみ再利用。pulse の開始色など評価依存も更新|
|既存 state 内の part の削除・target の無効化|action を別の同一文字列へ付け替えない。invalid/missing PartId と理由を表示し export を拒否。StateId の欠落は保存・確定を拒否|
|transition/action render の失敗|clip 全体の readiness を失敗にする。明示 stale preview のみ許可し、cut や action 無しを完成扱いしない|
|backend 不在|unavailable。Project は保存可能、renderer の代用品を探さず出力を拒否|
|編集前の cache|historical file を消さず、新しい依存 key 以外を現在の出力に使わない。世代の違う worker 完了通知を捨てる|

操作は candidate を作って一括検証し確定する、現在の Project 編集規則を引き継ぐ。
内部編集で L が変わるときは外側の clip 長・可視範囲との衝突を検査し、他 clip を黙って ripple しない。
無効な binding は Undo で正確に元へ戻せる。一つの state の編集を全 sequence の再作成にしない。

## migration と後続の gate

schema 更新は P3-1 以降の独立作業。新 kind/object は次の schema を割り当ててから導入する。
schema 20 の Math と MathTransform は読み込んでも Sequence へ自動変換しない。
旧式を Sequence へ変える場合は利用者の明示操作と Undo を用意し、新 ID を発行する。
隣接 Math の取り込みは式・hold・transition の消費時間の違いを提示し、同じ見た目の timing を検証してから確定する。
旧 schema で新 kind を出現可能にしない。未知 field の厳格検査と旧版の受け入れ方針は現行規約を維持する。
P3-0 では migration code や新 schema fixture を追加しない。

最終受け入れは「一つの native Sequence clip」で、a で割る、c を移項する、平方完成項を加える、
平方完成、平方根、x を分離する、二次方程式の解、判別式の強調を UI から author し、保存・再読込、
実 D3D11 preview、映像のみ export で再現すること。今回の S0..S5 には割る/移項などを一段にまとめた
箇所があり、最終 UI 受け入れの全中間状態を網羅したとは扱わない。段階計画と未解決の判断は roadmap に置く。

## P3-1 実装

2026-10-07。対象は Project/domain、純粋な評価、構造編集だけ。
Manim、glyph の対応、partition compiler、cache/artifact、描画、書き出し、製品 UI は追加しない。
既存の preview/export に新 kind が渡された場合は未実装と明示して拒否する。

### 保存と構造

schema **21** に `kind: "equation_sequence"` と `equation_sequence` object を追加した。
20 の Math/Write/MathTransform はそのまま読み、Project の schema だけを 21 へ上げる。
旧 Math の意味を Sequence へ変換しない。schema 20 以下に新 kind を入れた file は拒否する。
過去の renderer 証拠や schema 20 の artifact は更新・削除しない。

`src/project/equation_sequence.h/.cpp` は Qt・Manim・MLT に依存しない。
StateId / TransitionId / ActionId / PartId は異なる C++ 型であり、各 sequence 内で種別ごとに
一意。PartId は全 state を通じて一意で、ラベルと式の同じ文字は ID の解決に使わない。
states の vector が唯一の順序で、辺はちょうど n−1 本、同じ順序の隣接状態だけを結ぶ。
保存する値は状態・状態の式と revision・hold・部分式の binding・辺の尺と明示対応・action の
所有状態と対象・開始 offset と尺・operation。絶対 state 開始 frame や renderer の派生値は持たない。
新 object の全 field は必須で、未知・重複 field を拒否する。式の値は既存 Math パーサーを共有する。

背景は既存 `parseArgbColor` の **parsed alpha == 0** が正。既存 grammar は大文字・小文字を許し、
透明 RGB を黒へ正規化しないので `#00aBcDef` も有効。`#00000000` の文字列一致に限定しない。
不透明・半透明は拒否する。foreground/font は既存 `validateMathClipData` を共有する。

action は既存 state の hold 相対区間だけを持ち、変形中の配置は表現できない。
尺は 1 以上、区間は hold 内。初期 operation は outline/pulse のみで、対象の同異によらず
同時 action を拒否する。辺の correspondence は両隣接 state の bound part の一対一に限定する。
整数和・区間の終端・FPS の積と変換結果は overflow を検査する。

### 修復可能な参照

`target_status: "present"` は所有 state の実在 part を要求する。
part を削除すると元の ID を残して `target_status: "missing"` にする。
その ID はどの state にも実在してはならず、同じ欠落 ID の参照は同じ所有 state に閉じる。
missing StateId、別 state の part、明示 missing と実在 part の矛盾は拒否する。

binding は `bound` / `invalid`。bound は revision、UTF-8 byte 境界、非空・非重複範囲、
expectedText を検証する。revision は domain 呼び出し元が渡す不透明な識別値で、P3-1 は
digest の生成や editor delta を実装しない。invalid は旧 revision の範囲と expectedText を保持し、
置換後の source へ範囲を移さない。短い source への全置換でも旧範囲を消さず保存できる。
全体の source 置換では文字が同じでも全 binding を invalid にし、関連 correspondence を消す。
構造が有効でも invalid/missing や標本ゼロの action は `equationRenderability` が拒否する。
これはデータと出力時間軸の検査であり、renderer の実装済みを意味しない。

### 時間と FPS の確定契約

`H0,T0,H1,...,Hn` の整数 `[begin,end)` を派生し、全長は hold と transition の和 L。
`evaluateEquationSequence` は hold の状態・localFrame、transition の ID・両端状態・localFrame・尺、
activeAction を返す。progress は **整数分子 i / 整数分母 N** であり浮動小数は authority にしない。
N=1 は i=0 の一標本、その次は target hold 0。progress 1 の transition 標本は出さない。

Sequence の source FPS は作成時の Project FPS。外側 clip の source FPS/frame count/in/out を正とし、
sequence 内で同じ FPS を重複保存しない。R = output FPS / source FPS とすると:

```text
可視 source 範囲 [in,out) の output 位置: [ceil(in R), ceil(out R))
clip local output frame k の素材原点からの位置: p = ceil(in R) + k
標本位相: output frame 始点
内部 sample s = floor(p / R)
評価: s が属する [begin,end) を探索 → localFrame = s - begin
timeline 配置: timelineStartFrame + k
```

丸める前の積、切り上げ、切り捨ては `core::convertFrameBoundary` の checked rational 演算を共有する。
既存メディアの `floor(p/R + 1/2)` は変えない。Sequence では四捨五入すると exclusive out を
左片が表示しうるため使わない。上の始点標本化なら全 sample が可視 source 範囲内に収まる。
内部 sample を選んでから区間と action を同じ半開区間規則で判定し、境界だけ別の丸めを使わない。

|source → output|先頭 output の内部 sample (手計算)|振る舞い|
|---|---|---|
|60 → 60|0,1,2,3,4,5|同一標本|
|24 → 60|0,0,0,1,1,2,2,2|同じ内部標本を複数回表示|
|60 → 24|0,2,5,7,10,12|内部標本の一部を飛ばす|
|30000/1001 → 60|0,0,0,1,1,2,2,3|非整数比も整数式で確定|

内部 action の絶対区間 [a,b) の output 区間は `[ceil(a R),ceil(b R))` と可視 output 範囲の交差。
交差が空なら unsampledActions に ID を返し、尺を伸ばさない。可視 source 範囲から全体が除かれた
action は出力対象外。例: 60→24 の source [1,2) は [1,1) となり標本がない。
Project の構造検証とは独立に、この output FPS での renderability を検査する。

### 編集と所有

trim は可視範囲だけを変える。素材原点の p を維持するので hold/transition/action を再開しない。
source [0,L) 外へ延長は拒否し、Sequence の端をメディア用 clamp で黙って止めない。
split は完全データを左右へ保持し、可視範囲だけを割る。
timeline split が整数 source 境界へ正確に戻らない場合は原子的に拒否する。
例: 24→60 の output 5 は source 2 で分割でき、output 1 は整数 source 境界で表せない。
左右とも任意の初回 seek が元の同じ output 位置を評価する。

左片は全 ID を維持し、右片・copy/paste/duplicate は全内部 ID と参照を一括 remap する。
欠落参照は新しい欠落 ID へ同じ対応で移す。発行時は実在・欠落 ID の両方を予約する。
発行器が衝突し続ける場合は有限回で失敗し、候補を確定しない。
所有 ID 以外の source/style/尺/operation は変えず、cache key の実装は追加しない。

state の挿入・削除・hold/transition の尺変更・source 置換・part 削除は候補を作って全検証後に確定する。
中間 state の削除は両辺と所有 action を同時に消し、呼び出し元の明示的な新辺を追加する。
新辺へ旧 correspondence を継承しない。最後の state の削除は拒否する。
外側 source 範囲は下記 P3-1.1 の契約で更新する。`editEquationSequenceData` は通常の Project transaction と Undo 履歴へ
一操作を一回で確定する C++ 入口であり、製品 UI ではない。
Undo/Redo は既存の Project snapshot を復元し、ID 発行器を再実行しない。
Project FPS 変更は timeline 位置だけを既存換算で移し、内部 FPS・整数尺・offset・ID を保持する。

### 検証証拠

[事実] UCRT64 release の全体ビルドが完了。集中 CTest は **8/8 通過**。
新規 domain は **236 検査 / 0 失敗**、実 controller の保存・copy/paste/duplicate・split・
状態削除・source 置換・Undo/Redo は **25 検査 / 0 失敗**。
既存 Math JSON は 114 検査、MathTransform は 131 検査で失敗 0。
他の集中対象は timeline edit、two-track Project、ClipEffects、字幕。
証拠: `build/math-p31-focused-final-20261007.log` / `.xml` と
`build/math-p31-focused-final-details-20261007.log`。

```powershell
pwsh scripts/build.ps1
# 集中対象 8 件を -N で数えてから実行。CTest の環境を使い、実保存は sandbox 外で確認した。
C:\msys64\ucrt64\bin\ctest.exe --test-dir build/ucrt64-release `
  -R 'math_equation_sequence|math_project_json|math_transform_timeline|timeline_edit|clip_effects|two_track_project|subtitles' `
  --output-on-failure --timeout 60
pwsh scripts/test-equation-sequence-mutations.ps1
pwsh scripts/test.ps1 -Preset ucrt64-release -Group BuildIndependent
pwsh scripts/lint.ps1
```

[事実] **5/5 変異を検出**。inclusive end、`(i+1)/N`、状態削除後の action 残留、
double による境界換算、同じ文字の別 PartId への自動付け替えを、実装の複製へ一つずつ入れた。
各変異の実行は検査の終了コード 1。クラッシュ・timeout・compile error は検出として数えない。
製品 source を書き換えず、変異 source と実行ログを
`build/ucrt64-release/equation-mutations-20261007-003319/` に保存した。
Undo の ID 再発行は controller の exact snapshot 比較で検査し、controller の変異コンパイルは行っていない。

[事実] BuildIndependent は **1078/1078 通過** (`build/math-p31-independent-20261007.log`、
詳細は `build/math-p31-independent-details-20261007.log`)。
lint は format、層の隔離、producer service、PSScriptAnalyzer を含めて通過
(`build/math-p31-lint-verified-20261007.log`)。

[事実] 途中の失敗を保持した。初回 domain は JSON 変異の検索対象の空白が異なり 3 検査が失敗
(`build/math-p31-domain-first-failed-20261007.log`)。検索箇所の存在を検査したため空振りの成功にはならなかった。
全体ビルドの初回は既存 timeline 試験の schema 定数の名前空間漏れで失敗
(`build/math-p31-build-20261007.log`)。集中 8 件の初回は旧 Math の「未来版 21 を拒否する」負例が失敗
(`build/math-p31-focused-20261007.log` / `.xml`、`build/math-p31-focused-first-details-20261007.log`)。
未来版を現行版+1 とし、schema 20 の MathTransform の対照も加えた後に集中 8/8 が通過した。

[事実] sandbox 内の controller 保存・lock 試験は失敗し、同じ実保存試験は sandbox 外で通過した。
失敗は `build/math-p31-history-first-20261007.log` と `build/math-p31-history-second-20261007.log`、
対照は `build/math-p31-history-unsandboxed-20261007.log`。その後、状態削除ケースを追加した最終試験も
sandbox 外の集中 CTest で通過。sandbox 内の最初のビルドは compiler 終了後に Ninja の CPU と
`.ninja_log` が進まなくなり、診断後に当該実行を止めて公式入口を sandbox 外で実行した。
build directory と Ninja metadata は削除しなかった。

[事実] 通常 release gate は **一回で 1460/1460 通過**。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。
performance / stability を除外し、extended / workstation は短縮・除外しなかった。
成功するまでの再試行や有効 run の選別はしていない。
証拠: `build/math-p31-release-20261007.log` と `build/math-p31-release-details-20261007.log`。
P3-1 の実装・検証は完了。P3-2 以降の実装、commit、push は行っていない。

## P3-1.1 外側 source 範囲の修正

内部編集前の `sourceOutFrame == sourceFrameCount` を記録し、全体末尾まで表示していた
clip だけ編集後の新 L へ末尾を追従させる。`sourceInFrame` は保持する。
右 trim 済みの clip は `sourceOutFrame` を保持し、新 L がその末尾を下回れば
Project と Undo 履歴を変更せず拒否する。内部編集によって右 trim を解除しない。

domain と controller で hold・transition の延長/短縮、state 挿入/削除、右 trim の
延長/範囲内短縮/範囲外短縮を検査する。controller の Undo/Redo は Project 全体の
比較で frameCount/in/out・全 ID・sequence データの厳密な復元を確認する。

[事実] `scripts/test-equation-sequence-mutations.ps1` の `source_count_only` は
新しい末尾更新を無効にして旧動作へ戻す。domain の 263 検査中 9 件が失敗し、
終了コード 1 で検出した。製品 source は変更せず build 配下の複製を使った。
既存分を含め 6/6 変異を検出。
証拠: `build/ucrt64-release/equation-mutations-20261007-011430/`。

[事実] 最終 release ビルド、focused 3/3（domain・controller・履歴）、lint は通過。
focused 証拠: `build/math-p311-focused-final.log`。

[事実] BuildIndependent は 1078/1078 通過。
通常 release gate 一回は 1459/1460 通過で、`m4_timeline_export_focused_tractor` の
「crop + 回転の clip を書き出せません」が失敗した。通常 gate は未通過。
この対照は通常動画で `editEquationSequence` を呼ばない。失敗原因は未特定で、
再試行していない。一般の書き出しの未解決事項として roadmap に記録。
証拠: `build/math-p311-independent.log`、`build/math-p311-release.log`、
`build/math-p311-release-lasttest.log`。performance/stability は除外した。

## P3-2 semantic binding と renderer 中立 plan

### 保存の正と編集境界

schema 21 と P3-1 の `PartId + UTF-8 byte [begin,end) + revision + expectedText + Bound/Invalid`
を維持する。partition と plan は派生値であり保存しない。glyph/submobject index は持たない。
UTF-8/UTF-16 変換は `src/core/text_offsets.*` に置き、文字列全体の不正 UTF-8、継続 byte、
surrogate pair の内部、終端外を拒否する。結合文字の前後は個別の codepoint 境界として有効。
書記素 cluster の意味を推測せず、binding の正は byte / code unit の境界である。
Bound の証人検査は `equationBindingMatchesSource` に一本化し、Project 検証・明示 rebind・compiler が共有する。

`editEquationSourceTrusted` は旧 source、UTF-16 編集区間、UTF-8 replacement、新 source、
新 revision を要求する。実際の置換結果が新 source と一致しない場合は候補を確定しない。
編集区間を UTF-8 の `[u,v)` へ変換し、part の `[b,e)` に対して次を適用する。

| 条件 | 結果 |
| --- | --- |
| `v <= b`（begin に一致する挿入を含む） | replacement byte 長との差で begin/end を移動 |
| `u >= e`（end に一致する挿入を含む） | 範囲は維持 |
| 上記以外 | PartId と旧証人を維持して Invalid |

Bound を維持した part だけ新 revision に更新する。既存 Invalid は移動も復旧もしない。
無効になった part の correspondence を除き、無傷の対応は保持する。
未信頼の source 置換は P3-1 の一括無効化と対応除去を使い、同じ文字列でも再 binding しない。
`rebindEquationPart` は既存 state / PartId、現在 revision、非空かつ正しい UTF-8 境界、
source slice と完全一致する expectedText、他の Bound part と非重複を要求する。
同じ ID に対する action は再解決可能になるが、除去済み correspondence は復旧しない。

### partition と TeX の支持範囲

`src/app/equation_sequence_compile.*` は Project と既存 P2 計算の橋渡しであり、Qt / Manim に依存しない。
`src/media/math` へ Project 型を持ち込まない。semantic を source 順で配置し、各未被覆区間だけを
既存の `segmentMathTex` に渡す。自動 segment は semantic 境界を越えず、全 byte を一回ずつ覆う。
等しい文字の part は別の PartId と別の handle を持つ。P2 の契約に独立の literal 種別はないため、
未被覆領域は Auto とし、照合できない handle を fade に回す。

初期の字句検査は UTF-8 境界、制御命令の名前、escape、comment、group 深さと内部の最低深さ、
引数の欠けた既知命令を検査する。`frac` / `sqrt` は中括弧の引数を要求する。
一般 macro 展開、optional argument、environment、`left/right` は支持しない。
未知の制御命令が source 内にある場合も保守的に `UnsupportedTexBoundary` にする。
comment を含む semantic 範囲、comment 内の境界、命令名の途中、裸の escape、
`begin/end` token の途中、group の片側だけを含む範囲を拒否する。
分数内部の `b^2-4ac` は支持範囲だが、任意の TeX byte 範囲を renderer が分離できるとは主張しない。

Project として構造が有効でも compiler は失敗しうる。失敗理由は機械可読の enum で
`InvalidBinding`、`MissingPart`、`UnsupportedTexBoundary`、`PartitionConflict`、
`InvalidCorrespondencePlan`、`UnsupportedEmptyTarget`、`InvalidSequence` を区別する。

### transition・action・正準入力

explicit correspondence を最初に handle 対へ解決し、一対一の所有を確保する。
残った semantic / auto handle だけを pinned P2 matcher に渡す。自動照合 pool の同じ key は
nth-occurrence 順で対応し、explicit に確保した handle は自動照合へ再投入しない。
semantic / auto の key は共通の `mathTexSegmentKey` で先頭末尾の ASCII 空白だけを除く。
既存 P2 の segmenter / matcher の規則と版は変更していない。
全 source/target handle は transform または fade-out/fade-in の一つだけへ所属する。
pair は source handle 順へ正規化する。実 backend の object index は出力しない。

action の解決用 plan は StateId / PartId、派生 semantic handle、局所 start/duration、operation を持つ。
Missing、Invalid、危険な境界、明らかに空の空白/group target は失敗する。
非空の文字列でも glyph の存在は証明できないため、全支持 target の proof は
`BackendValidationRequired` のままとする。`equationTargetReadiness` はこの状態を実行可能と判定しない。
P3-3 は実 glyph/submobject が非空であり、所有が排他的であることを検証して初めて描画へ進める。
P3-2 の plan 導出成功を renderer 成功とみなさない。

`EquationPartitionSpec` / `EquationTransitionSpec` / `EquationActionSpec` が正準の値型。
source/style、範囲、segment kind、handle 対、整数尺、operation、規則の版を含む。
StateId / PartId / ActionId / TransitionId、revision、label は含めず、所有参照は source 順の index へ解決する。
正準 action 順は state index / start 順。等値比較で copy/remap の同値性を検証する。
cache key の hash、cache publication、Manim rendering、preview/export、製品 UI は追加していない。

### 検証証拠

[事実] 初回集中試験は 175 検査中 1 失敗。分数の命令だけを切り出す拒否条件が、
命令を含まない引数内部にも適用されていた。begin が命令を含む場合だけに限定して修正した。
失敗を `build/math-p32-compile-first.log` に保持した。

[事実] 変異 runner の初回は、括弧なしの for body を二つの文へ置換したため compile error で停止。
検出成功とは数えず、`build/math-p32-mutations.log` と
`build/ucrt64-release/equation-compile-mutations-20261007-014626-516/` を保持した。
変異文を block に修正後、等しい文字による自動再 binding、begin 挿入の内部扱い、
自動 segment の semantic 境界越え、explicit の二重照合、raw PartId に依存する正準入力の
5/5 を、それぞれ対応する検査の終了コード 1 で検出した。
証拠: `build/math-p32-mutations-fixed.log` と
`build/ucrt64-release/equation-compile-mutations-20261007-014702-933/`。
製品 source は変異させていない。

[事実] sandbox 内の最小ビルドは compiler 不在、Ninja の CPU と `.ninja_log` の進行停止を確認した。
当該実行を終了し、公式 build 入口の sandbox 外実行で成功した。Ninja metadata は変更していない。
証拠: `build/math-p32-build-first.log`、`build/math-p32-build-unsandboxed.log`。

[事実] 最終 source の集中 CTest は 5/5 通過。新規 compiler は 191 検査 / 0 失敗、
既存 P3-1 domain は 263 検査、履歴は 54 検査、Math JSON は 114 検査、
P2 timeline は 131 検査で失敗 0。実ファイル保存・再読込も新規試験に含む。
証拠: `build/math-p32-focused-gates.log`。
集中対象を `-N` で 5 件と確認し、`--timeout 60` 付きで実行した。

[事実] 単独 BuildIndependent は 1078/1078 通過。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release -Group BuildIndependent`。
証拠: `build/math-p32-independent.log`。

[事実] 最終 source の変異試験も 5/5 検出した。各変異は対応する負例のメッセージと
終了コード 1 を照合しており、単なる保存失敗などを検出証拠に数えない。
証拠: `build/math-p32-mutations-final.log` と
`build/ucrt64-release/equation-compile-mutations-20261007-015412-157/`。
lint は format、層の隔離、producer service、PSScriptAnalyzer を含めて通過した。
証拠: `build/math-p32-lint-final.log`。最終全 target のビルドは
`build/math-p32-build-gates.log` に保存した。

[事実] 通常 release gate は一回で 1461/1461 通過。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。
performance / stability を除外し、extended / workstation は短縮・除外していない。
最終 source に対する非依存試験もこの gate に含む。成功するまでの再試行や有効 run の選別はしていない。
証拠: `build/math-p32-release.log` と `build/math-p32-release-lasttest.log`。
過去の P3-1.1 の tractor 失敗記録と未解決事項は保持し、今回の通過だけで原因解消とはしない。
P3-2 の実装・検証を完了した。schema 21 を維持し、P3-3 以降の rendering/cache/preview/export/製品 UI、
commit、push は行っていない。

再現入口:

```powershell
pwsh scripts/build.ps1 -Target mvm_test_equation_compile
C:\msys64\ucrt64\bin\ctest.exe --test-dir build/ucrt64-release `
  -R 'math_equation_sequence|math_project_json|math_transform_timeline' `
  --output-on-failure --timeout 60
pwsh scripts/test-equation-compile-mutations.ps1
pwsh scripts/lint.ps1
pwsh scripts/test.ps1 -Preset ucrt64-release
```

## P3-2.1 action の失敗分類

`compileEquationSequence` の早期 action 分類を除き、既存の構造検証を通過した
action だけを解決する。partition / correspondence の固有の失敗分類は維持する。
存在しない StateId、および `present` の存在しない PartId は `InvalidSequence`。
合法な `missing` の存在しない PartId は `MissingPart`、実在する Invalid binding は
`InvalidBinding` とする。実在する PartId に `missing` を付けた状態も構造不正として拒否する。
schema 21、partition、照合、描画・cache・preview/export・UI は変更しない。

[事実] 集中 compiler/domain CTest は 2/2 通過（200 / 263 検査、失敗 0）。
4 ケースの分類と合法な Missing-Part の JSON 保存形式・再読込を検証した。
証拠: `build/math-p321-focused.log`。

[事実] `action_failure_conflation` は build 配下の複製に旧 preflight を戻す変異。
存在しない StateId と present の存在しない PartId の両方の回帰試験が失敗し、
終了コード 1 と両メッセージを確認した。既存分を含め 6/6 検出、製品 source は変異させていない。
証拠: `build/math-p321-mutations.log` と
`build/ucrt64-release/equation-compile-mutations-20261007-021739-526/`。

[事実] BuildIndependent は 1078/1078 通過、lint は通過。
証拠: `build/math-p321-independent.log`、`build/math-p321-lint.log`。
最小 target のビルドは `build/math-p321-build.log` に保持した。

[事実] 通常 release gate の `transition_preview` が 23.976fps の preview 準備と
区間始点の frame mapping 検査で失敗した。原因と分類変更との因果関係は未特定。
再試行せず失敗を保持し、未解決事項を roadmap に記録した。
最終結果は 1460/1461 通過、失敗はこの 1 件であり通常 gate は未通過。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。performance / stability を除外し、
extended / workstation は短縮・除外していない。
証拠: `build/math-p321-release.log`、`build/math-p321-release-lasttest.log`。
P3-2.1 の変更と集中検証を完了し、P3-3 の実装、commit、push は行っていない。

## P3-3 sequence/action renderer

2026-10-07。対象は実 Manim の EquationSequence renderer、backend の構造検証、disk の artifact の契約だけ。
製品 UI、timeline preview、RAM residency/prefetch、export、inspector、authoring UI は追加していない。
既存の EquationSequence の preview/export の fail-closed な拒否はそのまま。schema 21、P3-1 の時間の正、
P3-2 の partition と照合の意味は変更していない (backend との矛盾は見つからなかった)。commit / push はしていない。

### 境界と入力の正

既存の数式 backend の束 (`math::MathRenderBackend`) に `renderEquationSequence` /
`equationSequenceTemplate` / `maximumEquationSequenceFrames` を足し、`preflightManimMathTex` が
静止・Write・P2 変形と同じ Manim executable・toolchain fingerprint で束ねる。別の Manim の正は作らない。
process の起動・失敗の分類・取消 (job object で外部 process の木ごと停止) は P2 と同じ
`runScene` を `src/media/manim/manim_scene.h` 経由で共有し、P0-P2 の script の byte 列は変えていない。

入力の正は P3-2 の `EquationSequenceSpec` だけ。app 層の `equationSequenceRenderSpecFor`
(`src/app/equation_sequence_render.*`) がそれを Project の型を含まない
`math::EquationSequenceRenderSpec` (`src/media/math/equation_sequence_render.*`) へ一対一に写す
(色は parse 済みの数値)。StateId / PartId / ActionId / TransitionId、revision、label、Project JSON の構造は
backend に届かない。backend は状態・segment・handle の番号と、整数の枚数・進み具合だけを受け取る。

### 実 target の検証 (backend の構造検証)

各状態は P3-2 の segment の順のまま `MathTex(*segments)` で作る。Manim は 2 段階の別 process で起動する。

1. structure: 何も描かずに、各状態の top-level の部分と点を持つ子孫を `structure.txt` に事実として書く。
2. render: 1 を mvm が検証して通ったときだけ起動する。同じ報告を再び書き、1 と同じ所有でなければ
   `StructureChangedBetweenPhases` で失敗する。

「描画される非空の target」は Manim 0.21 では **点を持つ子孫 (`len(points) > 0` の family の member)
が 1 個以上ある top-level の `MathTexPart`** と定義した。画素の推測ではなく object の木の構造で決める。
segment ごとに次を記録する (backend の診断・provenance であり Project には保存しない)。

```text
neutral segment index (状態, segment)
top-level の MathTex の子の種類 (MathTexPart)
直接の子の数・子孫の数・点を持つ子孫の数
非空か (点を持つ子孫 > 0)
所有の集合 = 式全体の点を持つ子孫の並び (Manim の木の順) での番号
```

Python の `id()` と点列の buffer の address は同じ process の中の照合にだけ使い、所有の集合は
式全体の並びの番号へ写して正準化する (process をまたいで決定的)。検査は fail-closed で、
型付きの理由 `math::EquationBackendFailure` を返す。

|検査|失敗理由|
|---|---|
|報告が無い・読めない・未知の行|`StructureReportMissing` / `StructureReportMalformed`|
|Manim の SVG group の代用 log|`GroupingFallback`|
|top-level の部分の数 ≠ segment の数|`SegmentCountMismatch`|
|segment の番号の部分が無い|`MissingSegmentObject`|
|`MathTexPart` でない・文字列が無い|`SegmentTypeMismatch`|
|文字列が segment と違う|`SegmentTextMismatch`|
|同じ子孫を 2 つの handle が所有、式の木に同じ object が 2 回|`SharedDescendant`|
|別の子孫が同じ点列を共有、部分と式で点列が違う|`AliasedPointData`|
|どの handle にも属さない glyph、式の木に無い object を部分が所有|`UnclaimedDescendant`|
|action の対象の点を持つ子孫が 0|`EmptyActionTarget`|
|変形の対の片側だけが空|`EmptyTransitionHandle`|

空と空の対 (空白だけの auto segment どうし) と空の fade は許す。成功の判断に、Manim の終了コード 0・
入力の文字列の数・object の同一性だけ・file の存在だけを使わない。P3-2 の `BackendValidationRequired` は、
`equationTargetReadiness(proof, validation, state, segment)` (app 層の追加の overload) が検証済みかつ
非空のときだけ `None` (実行可能) にする。P3-2 の既存の関数は変えていない。

### 状態の raster と静止の同値

hold (action の無い区間) は **通常の静止 Math の artifact (`mvm-math-static/1`) をそのまま使う**。
別の配置の定義は作らない。代わりに render の段階で各状態を静止の大きさ + 各辺 200 px の canvas に
P2 の配置規則 (`mathEndpointPlacement`、半画素の位相補正を含む) で描き、通常の静止の描画と全画素で
照合する (`StaticMismatch`)。cache は全状態の今の静止 (同じ key) が Ready になるまで待ち、その mask を
backend に渡す。状態ごとの font size と foreground color は異なってよい。背景は parse 後の alpha 0 だけ。

### frame の意味

時間の正は P3-1 の整数区間。進み具合は整数の分子 / 分母で mvm が決め、frame ごとの値を request.json で
渡す (Python は時間を計算しない。Manim の秒は使わない)。各 frame は `Animation.interpolate(alpha)` の
直接標本化で、自前の Camera で 1 枚ずつ描く。再生の履歴に依存しない。

|区間|N 枚の frame i (0 <= i < N)|frame 0|最後に表示する frame|区間の直後|
|---|---|---|---|---|
|変形|進み具合 i/N (P2 と同じ)|前の状態の静止と全画素一致|(N−1)/N|後の状態の hold の frame 0 (静止)|
|outline|進み具合 (2i+1)/(2N)|1/(2N)|(2N−1)/(2N)|通常の静止|
|pulse|重み (N−\|2i+1−N\|)/N (進み具合 (2i+1)/(2N) の三角波)|重み 1/N|重み 1/N|通常の静止|

変形の照合用の終状態 (N/N) は描いて後の状態の静止と全画素で照合するが、timeline の frame にも artifact
にもしない。action は区間の全 frame が (0, 1) の内側にあり、N=1 でも中央 (outline 1/2、pulse 重み 1)
を見せる。区間の前と後の frame は action の無い通常の静止。outline は `ShowPassingFlash` +
`SurroundingRectangle` (Circumscribe の既定の分岐と同じ)、pulse は `Indicate` (拡大 1.2、rate は線形で
重みをそのまま alpha に使う) を backend の内部でだけ使う。

変形は P3-2 の handle の対だけで組む: 対は `ReplacementTransform`、余りは `FadeOut` / `FadeIn`
(rate は P2 と同じ smooth)。`TransformMatchingTex` は使わず、backend は独自の照合をしない。

### hold と action

action は所有する状態の上の一時的な層で、状態の object を変えない。action の描画は状態の copy に対して
行い、区間の後の式を描き直して静止と全画素で照合する (`ActionMutatedState`)。さらに action を変形より
先に描くので、action が状態の object を変えれば、続く変形の frame 0 と静止の照合でも見つかる。
P3 初期は同時 action を Project が拒否するので、汎用の多重 action 合成は作っていない。

任意の source frame f の見え方は `equationSequenceFrameAt(data, spec, f)` が P3-1 の
`evaluateEquationSequence` だけで決める (`Hold` / `HoldAction` (action の番号と区間内の frame) /
`Transition` (変形の番号と frame))。描画側に別の時間の実装を持たない。

### artifact と publication

key の名前空間は **`mvm-equation-sequence/1`** (静止 `mvm-math-static/1`、Write `mvm-math-sequence/1`、
P2 `mvm-math-transform/1` と別)。key の材料は compiler の版、全状態の partition (syntax/source/font/色/
背景/hold/segmenter の版/segment の種類と文字列)、変形 (両端/尺/対と余り/matcher の版)、action
(状態/segment/start/尺/operation)、raster の版 `a8-crop/1`、進み具合の規則の版、強調色、backend の id、
toolchain fingerprint、描画 template `manim-equation-sequence/1`。所有 ID・revision・label は含まない
ので、copy/remap した同じ意味の sequence は同じ key になる。色・hold・start を含めるため、それらの
変更も sequence の再描画になる (未解決事項)。

disk (`MathRasterCache` の cache directory の下):

```text
equation-sequence/<key>.txt              provenance (mvm-equation-sequence-artifact/1、最後に atomic に書く)
equation-sequence/<key>/t<k>/00000.a8    変形 k の frame (artifact の矩形で切り出した A8)
equation-sequence/<key>/a<k>/base.a8     action k の base 層 (区間中に変わらない)
equation-sequence/<key>/a<k>/00000.a8    action k の accent 層の frame
```

層と合成の順: outline は base = 状態全体 (状態の色)、accent = 線 (強調色 `#FFFFFF00`)。pulse は
base = 対象以外 (状態の色)、accent = 拡大する対象 (状態の色と強調色の重みの補間)。どちらも base の上に
accent。変形の frame i の色は P2 の `mathTransformColorAt(前の色, 後の色, i, N)`。各 frame の色は
provenance に記録する (P3-4/P3-5 が計算し直さない)。provenance は状態の静止の key と大きさ、segment の
所有、区間ごとの canvas・artifact の矩形・端点 (静止) の切り出し座標、frame の byte 数と SHA-256、
toolchain を持つ。

publication は P2 の変形と同じ規則で、`MathRasterCache` の同じ worker・Project lock の権限・世代・
publish の gate (`publishGate_`) の下で行う (`apps/mvm/math_equation_sequence_artifact.*`)。
backend の一時 directory で描き、backend が全区間を検証した後、cache が再び切り出しと端点を検査する。
古い provenance を消し、frame を書き、provenance を最後に書く。失敗・取消は Ready にならず provenance を
書かない。取消は外部 process の木を止める。取り下げ・世代・権限の変更は gate と排他なので、古い世代が
後から終わっても確定しない。読むたびに provenance を job から組み直した正準形と byte 単位で比べ、
全 frame の SHA-256・変形の frame 0 と outline の base の静止との一致を確かめる。
過去の artifact は消さず再解釈しない (toolchain が変われば別 key)。P2 の検証は弱めていない。

raster の検査 (区間ごと): 枚数ちょうど、canvas の大きさ、byte 数 (幅 x 高さ、行間の余白なし)、
SHA-256、一時 canvas の縁に触れない (= 外周の alpha が 0 で背景が透明)、全 frame の外接矩形と静止の
矩形の和の内側だけを切り出す、変形の両端・状態・action の後の静止との全画素一致。

### 実 Manim の受け入れ (実 toolchain の証拠)

[事実] 実 toolchain は Manim Community v0.21.0、MiKTeX-pdfTeX 4.27 (MiKTeX 26.5)、dvisvgm 3.6
(preflight の fingerprint、`results.json` の `toolchain`)。描画 template は `manim-equation-sequence/1`、
key の名前空間は `mvm-equation-sequence/1`、artifact の形式は `mvm-equation-sequence-artifact/1`。

受け入れは `tests/harness/mvm_equation_sequence_smoke.cpp` (CTest に登録しない手動の executable。
P2 の `mvm_math_transform_smoke` と同じ扱い)。各ケースは Project の EquationSequenceClipData を作り、
P3-2 の compile → `equationSequenceRenderSpecFor` → 実 backend を通す。静止は製品の静止の描画で描く。
backend の合否に加え、変形の frame 0 と照合用の終状態を読み直して両端の静止と全画素一致・1 画素
ずらすと不一致、action の frame の時間変化、P3-1 の評価による source frame の引き当て、実
`MathRasterCache` での公開と provenance・全 frame の SHA-256・合成の色を backend とは別に確かめる。

```powershell
# 【操作可】画面の表示・音声・性能計測は行わない。証拠の directory が存在すれば起動を拒否する。
pwsh scripts/build.ps1 -Target mvm_equation_sequence_smoke
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\build\ucrt64-release\bin\mvm_equation_sequence_smoke.exe "$env:USERPROFILE\.local\bin\manim.exe" build\math-p33-acceptance-<新しい名前>
```

[事実] 証拠の実行 `build/math-p33-acceptance-20261007-b` は **249 検査 / 0 失敗、終了コード 0**。
log は `build/math-p33-acceptance-20261007-b.log`、生データは
[results.json](../build/math-p33-acceptance-20261007-b/results.json)、公開した artifact は
`build/math-p33-acceptance-20261007-b/cache/equation-sequence/` (provenance は `<key>.txt`)。
集計値は results.json が正で、以下は観測の要約。

|群|ケース|結果|
|---|---|---|
|partition / 構造検証|`x+x`、同じ文字の semantic 2 個、分数内部の `b^2-4ac`、Greek (`\beta`)、上付き/下付き (`b_{1}`, `c_n^3`)、空白だけの auto segment (`x x`)|全て構造検証を通る。空白の auto segment は点を持つ子孫 0 の空の handle で、空どうしの対として変形できた|
|変形|P3-0 の S4→S5 (判別式の明示対応)、明示対応の入れ替え、重複する自動の項、fade-in/out、N=1、font 96→64、色 白→`#FFFF8040`、`\left`/`\right` を含む auto だけの段階|全て frame 0 = 前の静止 (違う画素 0)、1 画素ずらすと不一致、照合用の終状態 = 後の静止 (違う画素 0)|
|action|判別式の outline、pulse、複数 glyph の対象 (`2ab`) に outline と pulse、action の後に変形|pulse の全 frame で対象が描かれ、frame が時間で変わる。action の後の式は静止と一致|
|任意 seek|outline / pulse の N=3 の中央の frame と N=1 の唯一の frame|byte 単位で一致 (同じ進み具合 1/2・重み 1。先行 frame の無い描画と同じ)|
|frame の引き当て|action の直前・frame 0・中央・最後・直後、変形の frame 0・最後・直後|P3-1 の評価で期待どおり。後ろから引いても同じ|
|実 backend の負例|`a\,b` の `\,` を outline の対象|`empty_action_target`。`\,` の部分は点を持つ子孫 0 (results.json の ownership)。描画の段階は起動していない|
||`\,` と `+` の明示対応|`empty_transition_handle`。描画の段階は起動していない|
||`{{a}} + b` (Manim が `{{ }}` で分け直す)|`segment_text_mismatch` (Manim "a"、mvm "{{a}}")。描画の段階は起動していない|
|artifact|S4→S5 (色 白→`#FF40C0FF`、font 72→64) に outline と pulse を公開|Ready、15 frame を SHA-256 付きで読み直し、合成の色が規則どおり、remap した複製は同じ key|

[事実] 実 Manim で構造上空でない source 範囲から glyph が出ない例として、P3-2 の字句検査を通る
`\,` (thin space) が確実に再現した。P3-0 の spike で調べた `\frac{` の片側は P3-2 が先に拒否するので
P3-3 の経路には入らない。

[事実] 開発中の実行 `build/math-p33-dev-1` (同じ harness、Python template の action と変形の順を
入れ替える前) も 249 / 249。証拠ではなく開発の記録として保持した。

[事実] 最初の証拠の実行 `build/math-p33-acceptance-20261007-a` と P2 smoke
`build/math-p33-p2-transform-smoke-20261007-a` は、PowerShell の PATH に UCRT64 の bin が無く
実行時 DLL を読めずに終了コード 0xC0000135 で起動しなかった (directory は作られていない)。
log は `build/math-p33-acceptance-20261007-a.log` / `build/math-p33-p2-transform-smoke-20261007-a.log`。
PATH を正して `-b` で一回ずつ実行した。

### 偽の backend の試験 (実 Manim の証拠ではない)

以下は偽の Manim (`tests/harness/fake_math_tex_cli.cpp` の `MvmEquationSequence`) と偽の描画関数
(`tests/harness/math_fake_backend.h`) による検査で、実 toolchain の事実ではない。実では起こしにくい
構造の失敗を、検査が本当に効くかの証明に使う。

|試験 (CTest)|内容|検査数|
|---|---|---|
|`math_equation_sequence_render_contract`|進み具合の手計算値、値の検査の各違反、key の名前空間と各 field の効き|125|
|`math_equation_sequence_render_bridge`|正準入力の写像、remap/label/revision で同じ描画要求と key、P3-1 の評価による引き当て (3 通りの順)、実行可能性|72|
|`manim_equation_sequence_focused`|手で書いた構造の報告の全失敗理由と対照、偽の Manim での 2 段階起動 (構造の失敗で描画を起動しない)、枚数・壊れた frame・大きさ・縁・静止/端点/action の後の不一致、描画の段階の失敗、TeX の誤り、取消 (外部 process を停止)、timeout、preflight の束|114|
|`math_raster_cache_focused`|既存 P0-P2 に加え、公開・provenance の内容と色の手計算値・開き直し・provenance の各書き換え (色/状態色/toolchain/template/operation/静止の大きさ/所有)・壊れた frame・欠けた frame・toolchain の変更・構造検証の失敗・枚数の不一致・読めない frame・静止の失敗・backend の上限・不正な要求・取消・古い spec の後からの完了・権限の喪失・provenance 直前の権限の喪失|335|

実 Manim で起こしにくい負例 (部分の数の不一致、部分の欠落、共有・alias された子孫、どの handle にも
属さない glyph、段階間の構造の変化、枚数の不一致、壊れた frame、古い toolchain/provenance、取消、
spec の変更後の古い描画の完了) はこの偽の試験だけで確かめた。

[事実] `scripts/test-equation-renderer-mutations.ps1` は build 配下の複製に変異を入れ、対象の試験を
作り直して終了コード 1 と対象の検査のメッセージを照合する。共有子孫の見逃し、どの handle にも属さない
glyph の見逃し、空の対象の受け入れ、検証前の描画、段階間の比較の削除、枚数の未検査、静止の同値の
未検査、変形の端点の未検査、action の後の未検査、provenance の identity の無視、frame の SHA-256 の
未検査、幾何の検査なしの公開、公開時の取消の無視の **13 / 13 を検出**。製品 source は変えていない。
証拠: `build/math-p33-mutations-fixed.log` と
`build/ucrt64-release/equation-renderer-mutations-20261007-032441-172/`。
最初の実行は clang-format が折り返した変異箇所を見つけられず、変異の前に停止した (検出には数えない)。
証拠: `build/math-p33-mutations.log`。開発中の 13/13 は
`build/ucrt64-release/equation-renderer-mutations-20261007-031518-019/` (format 前の source)。

### P0/P1/P2 の同値

静止 (`mvm-math-static/1`)・Write (`mvm-math-sequence/1`)・P2 変形 (`mvm-math-transform/1`) の key、
provenance の形式、script の byte 列、検証は変えていない。共有した変更は backend の束への field の追加、
`manim_math_tex.cpp` の内部 helper の公開 (wrapper) と preflight での束ね、`MathRasterCache` の取消・
世代・`retainOnly`・`forgetFailures`・`cancelPendingAnimations` への新しい record の追加。

[事実] P2 の実 Manim の smoke `mvm_math_transform_smoke` は `build/math-p33-p2-transform-smoke-20261007-b`
で **71 / 71、終了コード 0** (log は同名の `.log`)。P2-3 の記録と同じ検査数。

### 検証の実行と結果

```powershell
pwsh scripts/build.ps1
# 集中対象 20 件を -N で数えてから実行
C:\msys64\ucrt64\bin\ctest.exe --test-dir build/ucrt64-release -R '<下の 20 件>' --output-on-failure --timeout 300 -V
pwsh scripts/test-equation-renderer-mutations.ps1
pwsh scripts/test.ps1 -Preset ucrt64-release -Group BuildIndependent
pwsh scripts/lint.ps1
pwsh scripts/test.ps1 -Preset ucrt64-release
```

[事実] 最終 source の全体ビルドは成功 (`build/math-p33-build-final.log`)。その前の全体ビルド
(`build/math-p33-build.log`) も成功したが、その後に試験の型変換の警告を直したので最終としない。

[事実] 集中 CTest は 20 件 (`-N` で確認、`build/math-p33-focused-count.log`) を一回実行し **19 / 20 通過**。
P3-3 の新規 4 件、P3-1/P3-2 の domain・compiler・履歴、Math JSON、P0-P2 の key・変形の契約・timeline・
backend・cache・controller・書き出し・変形の native 再生は通過した。
`math_write_native_playback` (P1 Write の実 D3D11 再生) が 22 検査中 3 件失敗した
(「一時停止中に mask を読み、合成に Write を付ける」「先頭から再生する」「再生が 60 frame まで進む」。
同じ実行の後半の再生は通過)。実行時の利用者の無操作は約 1 分で、画面の消灯ではない。
この試験は変更した `MathRasterCache` を link する。原因と今回の変更との因果関係は未特定。
証拠: `build/math-p33-focused.log`。
診断として同じ試験を単独で 3 回実行し 3 / 3 通過した (`build/math-p33-diag-native-write-1..3.log`)。
これは間欠的であることの診断であり、失敗の記録を置き換えない。

[事実] BuildIndependent は **1077 / 1078 通過**。`audio_mixer_controls_qml` が QML ScrollBar の
binding loop の警告 (同じ log に `OpenThemeData() failed ... ハンドルが無効` が多数) で失敗した。
今回は QML と、それが読む file を変更していない。原因は未特定で、再試行していない。
証拠: `build/math-p33-independent.log`。

[事実] lint は format、層の隔離、producer service、PSScriptAnalyzer を含めて通過
(`build/math-p33-lint.log`)。開発中の lint は新規 file の未整形で失敗した (`build/math-p33-lint-dev.log`)。
`scripts/format.ps1` で整形した。

[事実] 通常 release gate は一回で **1463 / 1464 通過**、通常 gate は未通過。
失敗は BuildIndependent と同じ `audio_mixer_controls_qml` (QML ScrollBar の binding loop の警告)。
この試験は P3-2 の gate では通過していた。今回は QML と、それが読む file を変更していない。
原因は未特定で、再試行による選別はしていない。集中試験で失敗した `math_write_native_playback` は、
この gate では通過した。performance / stability を除外し、extended / workstation は短縮・除外していない。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。証拠: `build/math-p33-release.log`。

[事実] gate の後に受け入れ harness の先頭の注釈だけを直した (`summary.md` を書くという誤記。
結果は results.json だけ)。当該 target の再ビルドと lint は通過
(`build/math-p33-build-comment.log`、`build/math-p33-lint-final.log`)。挙動は変えていない。

P3-3 の実装・検証を完了した。schema 21、P3-1 の時間の正、P3-2 の partition と照合の意味は維持した。
P3-4 以降の residency・preview・export・製品 UI、commit、push は行っていない。

## P3-3.1 pulse の base の静止の分解

2026-10-07。P3-3 の artifact の正しさの穴を一つ塞ぐ。preview/residency/export/UI、schema 21、
P3-1 / P3-2 の意味は変更していない。commit / push はしていない。

### 問題

pulse の artifact の契約は「base = 対象の segment を除いた通常の状態、accent = 動く対象」だが、
P3-3 の公開と読み込みは pulse の base の大きさと SHA-256 しか検査していなかった。outline と違い、
base が対象の正しい補集合であることを証明していない。backend が誤って base に式全体を返しても、
無関係な segment を欠いても Ready になりえた。実際に P3-3 の偽の描画関数 (cache の試験) は
pulse の base に式全体を返していた。

### 合成規則と証明

層の被覆 (A8) の合成規則を中立な契約に定義した (`equationCoverageOver` /
`composeEquationCoverage`、`src/media/math/equation_sequence_render.*`)。下の層 under の上に over を重ねる
alpha だけの source-over で、1 画素ごとに整数で

```text
out = over + round(under * (255 - over) / 255)      (x / 255 はちょうど半分にならない)
```

P3-4 の preview / export の合成もこの規則を使う (順序は base の上に accent)。

Manim の job は pulse ごとに、拡大していない通常の大きさ・位置・色の対象だけの層 `target.png` を
一時的に描く (artifact には保存しない)。次の等式を厳密に要求する。

```text
composeEquationCoverage(pulse の base, 通常の対象) == 状態の通常の静止 (全画素、同じ配置)
```

検査は 2 か所で行い、違えば型付きの失敗 `PulseBaseMismatch` で provenance を書かずに失敗する。

- backend (`renderManimEquationSequence`): 一時 canvas の base と対象を合成し、静止を P2 の配置規則で
  置いたものと比べる。対象の層が無ければ枚数の不一致 (`FrameCountMismatch`)。
- 公開 (`renderEquationSequenceJob`): backend の報告を信じず、base と対象を同じ artifact の矩形で
  切り出し直して合成し、静止と比べる。対象の層が無ければ `PulseBaseMismatch`。

読み込みでは対象の層が無いので再合成しない。SHA-256 で公開時に証明した base と同じことを確かめる。

この変更が確立するのは **静止の分解** だけで、動く 2 層の mvm 合成が Manim の 1 枚の scene で描いた
pulse と等しいことは主張しない (P3-4 の別の問い。roadmap に残す)。

### 証拠

[事実] 開発中の確認として、実 Manim の scratch script (製品外) で 3 つの文字サイズ x 6 個の対象
(`b^2-4ac`、`2ab`、`\beta`、`b_{1}`、`x`、`2x`) を同じ camera で描き、合成は全て静止と違う画素 0、
base を式全体にした誤りは 259〜1690 画素の不一致だった。証拠ではなく設計の確認として扱う。

[事実] 実 Manim の受け入れ `build/math-p331-acceptance-20261007-a` は **270 検査 / 0 失敗、終了コード 0**
(log は同名の `.log`、生データは [results.json](../build/math-p331-acceptance-20261007-a/results.json))。
toolchain は P3-3 と同じ (Manim 0.21.0、MiKTeX-pdfTeX 4.27 (MiKTeX 26.5)、dvisvgm 3.6)。
harness は backend とは別に base と通常の対象を読み直して合成し、静止と照合する。pulse の 7 件
(Greek、下付き、判別式、複数 glyph、action の後の変形、seek の N=3 と N=1) は全て違う画素 0。
対照として base を式全体にした合成は 401〜1216 画素で静止と一致しない (空振りでない)。
実 cache の公開 (公開前の照合を含む) も Ready。

[事実] これらの実例では base と対象の被覆が重なる画素が 0 だった (results.json の
`base_target_overlap_pixels`)。重なった画素での丸めは実例では通っておらず、規則の手計算値と全 65536 組の
順序非依存性の単体試験だけで確かめた。重なる対象で厳密一致しない実例が出たら fail-closed で失敗する。

偽の backend の負例 (実 Manim の証拠ではない):

|試験|負例|結果|
|---|---|---|
|`manim_equation_sequence_focused`|base が対象を含んだまま (`FAKE_EQ_PULSE_FULL`)、別の部分を欠く (`FAKE_EQ_PULSE_OMIT`)、通常の対象を 1 画素ずらす (`FAKE_EQ_PULSE_SHIFT`)|`PulseBaseMismatch`|
||通常の対象の層が無い|`FrameCountMismatch`|
|`math_raster_cache_focused`|同じ 3 種と層の欠落を backend が Ok で返す (`EQPULSEFULL` / `EQPULSEOMIT` / `EQPULSESHIFT` / `EQPULSENOTARGET`)|公開前に `PulseBaseMismatch`、provenance も frame も残さない。対照の正しい分解は Ready|
|`math_equation_sequence_render_contract`|合成の手計算値、順序非依存、被覆を失わない、大きさの不一致|通過|

偽の描画関数の pulse は、静止の左半分の列を通常の対象、残りを base に分けるよう直した
(P3-3 の式全体の base は今回の検査で拒否される)。

[事実] 変異 runner に、backend と公開の両方で合成の照合を外す 2 件を足し **15 / 15 を検出**
(`build/math-p331-mutations-final.log`、`build/ucrt64-release/equation-renderer-mutations-20261007-041351-940/`)。

[事実] 途中の失敗を保持した。最初の変異の実行 (`build/math-p331-mutations.log`) は公開側の変異が
検出ではなく 0xC0000409 で終わった。変異名が長くなり、試験の作業 directory の下の cache の作業 path が
260 文字を超えて job directory を作れず、続く試験が書き換え箇所の無い文字列に `replace` を呼んで例外で
止まった。runner の作業 directory を短い名前にし、試験は書き換え箇所が無ければ検査の失敗だけを記録して
続けるよう直した。2 回目 (`build/math-p331-mutations-fixed.log`) は 15 / 15 だったが、直した runner に
PSScriptAnalyzer の未使用 parameter が残り lint が失敗した (`build/math-p331-lint.log`)。
直した後の変異 (上記 final) と lint (`build/math-p331-lint-final.log`) は通過。
最初の全体ビルドと集中試験 (`build/math-p331-build.log`、`build/math-p331-focused.log`、12 / 12) は
試験の修正の前の source で、最終は `build/math-p331-build-final.log`、`build/math-p331-focused-final.log`
(12 / 12、`-N` で 12 件)。

[事実] 集中 CTest 12 件は EquationSequence の domain・compiler・契約・橋渡し・backend・cache と、
共有する P0-P2 の key・変形の契約・timeline・backend・書き出し。P2 の合成の code は変更していない
(新しい合成規則は Equation Sequence だけが使う) ので、P2 の実 Manim の smoke は回していない。

[事実] BuildIndependent は 1077 / 1078 通過。P3-3 と同じ `audio_mixer_controls_qml` (QML ScrollBar の
binding loop の警告) が失敗した。原因は未特定で再試行していない。証拠: `build/math-p331-independent.log`。

[事実] lint は format、層の隔離、producer service、PSScriptAnalyzer を含めて通過
(`build/math-p331-lint-final.log`)。

[事実] 通常 release gate は一回で **1463 / 1464 通過**、通常 gate は未通過。失敗は
`audio_mixer_controls_qml` (QML ScrollBar の binding loop の警告) だけ。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。証拠: `build/math-p331-release.log`。
performance / stability を除外し、extended / workstation は短縮・除外していない。再試行はしていない。

P3-3.1 の実装・検証を完了した。schema 21、P3-1 / P3-2 の意味を維持し、preview/residency/export/UI、
commit、push は行っていない。

## P3-4 residency と product preview

2026-10-07。既存の schema 21 の EquationSequence clip を製品の timeline preview (D3D11/QRhi) で
任意 seek・再生できるようにした。書き出しと authoring/editor UI は追加していない (書き出しの
fail-closed な拒否はそのまま)。schema 21、P3-1 の時間の正、P3-2 の compile・照合、P3-3 の
artifact・provenance の意味は変更していない。以前の契約との矛盾は見つからなかった。commit / push はしていない。

### 正の分担と経路

```text
Project (schema 21)        意味の正
P3-2 compileEquationSequence   partition・照合・action の plan (今の data の compile だけ)
P3-1 clipSourceFrameAt + equationSequenceFrameAt   output frame → source frame → 区間と frame
P3-3 provenance / artifact     大きさ・配置・各層の色 (preview で計算し直さない)
MathRasterCache residency      cache (Write・変形と同じ上限)
preview (EquationPreviewModel) 提示だけ。再生の履歴を持たない
```

純粋な部分は `src/app/equation_sequence_preview.*` (Qt・cache に依存しない)。製品の経路は
`timeline_preview_mapping` が sequence clip を数式と同じ静止画 layer として写し、
`MvmController::equationSequencePreviewAnimation` が 1 本の clip の全区間を 1 つの
`PreviewStillAnimation` にまとめる。render thread の `stateAt(output frame)` が model に問い合わせ、
P3-1 の写像で区間と frame を決める。再生中に合成の差し替えが提示より遅れて届いても、
同じ output frame は同じ画素になる。QML は sequence の内部を見ない (提示は engine の layer だけ)。
timeline のトランジションで素材範囲を延ばした sequence の区間は P3-1 の時間の正の外なので、
preview でも未対応として拒否する (書き出しは従来どおり拒否)。

### 提示の代用の契約

| 区間 | 揃っているとき | 揃わないとき (読み込み中・OverBudget・無効・artifact 無し) |
| --- | --- | --- |
| `Hold(s)` | 状態 s の通常の静止 (静止の artifact、通常の Math と同じ key・配置・色) | 何も見せない (別の式・前に描けた静止で代用しない) |
| `HoldAction(s, a, i)` | action a の frame i の base と accent | 状態 s の通常の静止 |
| `Transition(t, i)` | 変形 t の frame i | 区間の全 frame で前の状態の静止。後の状態へは target hold の frame 0 で切り替わる |

```text
変形を省いた代用: source 静止 ... source 静止 | target 静止   (区間の後、P3-1 の終端)
```

Blend の合成や区間の中央での cut は作らない。compile に失敗した sequence は P3-1 の評価で状態だけを
決めて静止を見せ、前に成功した spec の action・変形を出さない (失敗の理由は保持する)。Project は
直さない。この代用は preview だけのもので、書き出し (P3-5 以降) は fail-closed のまま引き継がない。

### 層の合成の契約

provenance の A8 と色が正。変形の frame は provenance の frame の色で着色し、位置は P2 と同じ
`mathTransformRasterPlacement` / `mathTransformArtifactOriginAt` (artifact の source/target の位置から)。
action は base と accent を同じ原点 (状態の静止の配置 − artifact の中の静止の位置) に置き、別々に
中央へ寄せない。外側の ClipEffects は数式と同じく layer に 1 回だけ掛ける。

2 層の色付きの合成 (`composeEquationLayerPixel`、`src/media/math/equation_sequence_render.*`):

```text
a = base の被覆 * base の色の alpha、b = accent の被覆 * accent の色の alpha  (0..255*255)
alpha = equationCoverageOver(round(a / 255), round(b / 255))        (P3-3.1 の規則)
色    = round((C_accent * b * 65025 + C_base * a * (65025 - b)) / (b * 65025 + a * (65025 - b)))
```

両方の色が不透明なら alpha は `composeEquationCoverage` と同じ値 (全 65536 組で試験)。accent の無い
画素は単層の `composeMathPatch` と byte 単位で同じ。合成は CPU で行い、GPU には結果の RGBA
(straight alpha) を 1 枚の静止画 layer として渡す (shader・QML に別の丸めを持たない)。

### residency

Write・変形と同じ `MathResidencyBudget` (別の上限・別の cache を持たない)。層は 1 枚ごとの key
(`<sequence key>/t<変形>/<frame>`、`/a<action>/base`、`/a<action>/<frame>`、派生の cache の識別) で、
予約は実 byte (幅 × 高さ)。disk の Ready と memory の Resident は別の状態。

- 今の frame の束: 変形は 1 枚、action は base と今の accent の 2 枚。束の足りない層の byte を
  まとめて予約し、収まらなければ束全体を OverBudget (片方だけ読まない)。読み込みは優先度付き
- 先読み: 今の束が揃ったときだけ、今の区間の残りと次の区間 (上限 240 束)。何も追い出さず、
  OverBudget を記録しない。広い先読みはしない (roadmap)
- 追い出しは cache だけが持つ層 (preview が使っていない) を LRU で。束の中の層は外さない
- 読むたびに disk の provenance が検証した時と byte 単位で同じか、frame の大きさと SHA-256 を
  確かめる。合わなければ artifact を消し、sequence を Failed (CorruptFrame) にして静止で見せる
- 要求するのは再生位置に掛かる (preview に見える) sequence だけ。残すのは今の sequence の key と
  全状態の今の静止。層の key は owner の sequence の key が残る間だけ残る

### 世代と古い結果

編集で sequence の key が変わると `retainOnly` が前の key の record・層・読み込みを捨てる。
前の key の読み込みが後から終わっても、ticket の照合で捨てる。新しい key の artifact が揃うまでは
代用 (静止) で見せ、前の key の変形・action を使わない (last-good を正にしない)。Ready の artifact の
状態の静止の key が今の静止の key と違えば使わない。

### 状態の区別

`MvmController::equationSequencePreviewStatus(clipId, frame)` が、見た目が同じ代用でも内部の状態を
区別して返す: compile の失敗 (InvalidBinding など)、backend (Checking / Available / Unavailable)、
disk (Pending / Ready / Failed / Unavailable と backend の理由、壊れ・古い artifact は CorruptFrame)、
今の frame の residency (NotReady / Loading / Resident / OverBudget)、見せたもの。UI への表示は P3-5。

### 試験

|試験 (CTest)|内容|検査数|
|---|---|---|
|`math_equation_sequence_render_contract`|P3-3 の契約に加え、2 層の色付き合成の手計算値、不透明な 2 色の alpha = 被覆の合成規則 (全 65536 組)、accent の無い画素 = 単層の静止の合成、矩形への書き込み|149|
|`math_equation_sequence_preview_model`|P3-1 の写像の手の表 (clip の先頭・hold の最後・action の先頭/中央/最後/直後・変形の frame 0/N−1・target hold 0・N=1 の action と変形・左 trim が hold/action/変形の途中・24→60・逆向きの seek)、代用の規則、配置、provenance の色 (Project の色と別の値) での合成、2 層の原子性、古い・数の合わない artifact、compile の失敗、ClipEffects を含めない、今の束と先読みの層|82|
|`math_equation_sequence_preview_controller`|偽の backend で製品の controller を通す。全 91 frame の提示、受け入れの frame の画素と独立の参照の全画素一致 (対照: 1 画素ずらす・色の取り違え・accent を省く)、遅延した直接 seek (pulse の中央・変形の中央)、上限 (2 層の束・変形の 1 枚・戻すと同じ frame へ)、Write と同じ上限、編集後の古い key・遅れて届く古い層、壊れた変形の frame / base / accent と provenance の削除、compile の失敗、backend 不在、ClipEffects を 1 回、見えない sequence を描かない|166|
|`math_equation_sequence_native_preview` (workstation)|schema 21 の fixture (`tests/fixtures/equation-sequence/p34-preview.mvm`) を実 D3D11 の preview で。一時停止中の直接 seek で層が遅れて届く (pulse・変形)、再生中に届く、受け入れの 16 frame を engine と同じ手順 (更新可能な静止画の texture へ patch) で product compositor に通して全画素を読む|68|

参照は試験の側で独立に組んだ合成 (artifact の provenance の大きさ・位置・色と disk の `.a8`、手の配置の式、
規則を書き直した 2 層の式) で、製品の合成関数を呼ばない。

### 変異試験

`scripts/test-equation-preview-mutations.ps1` は変異した file を `compile_commands.json` の同じ命令で
compile し、ninja の同じ link 命令で対象の試験を作り直す (製品 source は変えない)。
変形に (i+1)/N の frame、片方の層だけで action、届いた action の先頭からのやり直し、新しい key が
揃うまで前の key の animation、変形の色を Project から作り直す、ClipEffects の不透明度を 2 回、
編集後も前の key の層を memory に残す、層を読むときに provenance を照合しない、action の 2 層を
1 層ずつ予約する、の **9 / 9 を検出** (終了コード 1 と対象の検査のメッセージを照合)。

[事実] 最終 source の証拠: `build/math-p34-mutations-final.log` と
`build/ucrt64-release/equation-preview-mutations-20261007-054825-441/`。format 前の source でも 9 / 9
(`build/math-p34-mutations.log`、`.../equation-preview-mutations-20261007-053744-642/`)。
P3-3 の artifact の file を変えたので P3-3 の変異も再実行し 15 / 15 (`build/math-p34-p33-renderer-mutations.log`)。

### animated pulse の同値 (実 Manim)

`tests/harness/mvm_equation_pulse_equivalence.cpp` (手動、CTest に入れない) が製品の `MathRasterCache` と
実 backend で通常の P3-3 の artifact を公開し、公開の直前に写した backend の `request.json` を
`scripts/spikes/math-p34-pulse-reference.py` (製品外) に渡す。参照は同じ canvas・配置・font・segment・
重みで、状態の色の式の対象に Indicate (強調色、線形) を掛けて 1 回で描いた Cairo の premultiplied RGBA。
`scene` は式全体を 1 回で描き、`ordered` は対象を最後に描く。製品の側は artifact の層を
`composeEquationLayersAt` で置いて premultiplied にする。

```powershell
# 【操作可】画面の表示・音声・性能計測は行わない。証拠の directory が存在すれば起動を拒否する。
pwsh scripts/build.ps1 -Target mvm_equation_pulse_equivalence
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\build\ucrt64-release\bin\mvm_equation_pulse_equivalence.exe "$env:USERPROFILE\.local\bin\manim.exe" `
  "$env:APPDATA\uv\tools\manim\Scripts\python.exe" scripts/spikes/math-p34-pulse-reference.py build\math-p34-pulse-<新しい名前>
```

参照の script は Manim の TeX cache を実行時の作業 directory の `media/Tex` に作る。repository の直下で
実行した今回は、計測の後にその生成物だけを消した (証拠には含まれない)。

[事実] 証拠の実行は `build/math-p34-pulse-20261007-g` (**139 検査 / 0 失敗**、終了コード 0)。生データは
[results.json](../build/math-p34-pulse-20261007-g/results.json)、log は同名の `.log`、参照と製品の PNG は
`reference/<ケース>/`。toolchain は Manim 0.21.0、MiKTeX-pdfTeX 4.27 (MiKTeX 26.5)、dvisvgm 3.6。
状態の色 `#FF40C0FF`、強調色 `#FFFFFF00`、文字 72。ケースは判別式 `b^2-4ac` (N=5 の frame 0・中央 2・
最後 4 と N=1)、複数 glyph `2ab`、Greek `\beta`、下付き `b_{1}`。以下は results.json の要約。

- **厳密には一致しない。** alpha は全ケース・全 frame で違う画素 0 (base と拡大した対象の重なりを含む)
- premultiplied の色 channel の最大差は 1。違う画素は判別式で 17〜36、`2ab` 19、`\beta` 7、`b_{1}` 17
- 違う画素はすべて部分被覆の画素: antialias の縁 (7〜19) と、重みが最大 (中央の frame、N=1) の
  判別式で base と拡大した対象が重なる画素 (20)。内部の画素 (被覆 0 か 255 で重ならない) は 0
- `scene` と `ordered` は全て同じ値 (描く順は効いていない)
- 違う channel の参照の値は全て、同じ実数の合成値 (2 層の premultiplied を丸める前) の floor か ceil
  (`differing_channels_outside_rounding` = 0)。製品の値もその隣に入らないのは重なる画素の 1 channel
  だけ (straight の 8 bit を経てから premultiplied にする二重の丸め)
- 外接矩形は対象の付近 (results.json の `bbox`)。対照: accent を省いた合成は 638〜3302 画素違い、
  別の進み具合の参照とも自分の参照より多く違う (比較は空振りではない)

[推測] Cairo は path ごとに 8 bit の premultiplied で source-over し、mvm は白で描いた最終の被覆に色を
1 回だけ掛けるので、同じ画素に複数の path が掛かると丸めの順が違う。alpha が一致し色が丸めの隣だけで
違うことは、この説明と矛盾しない。Cairo の内部は確かめていない。
この差は正の検査の許容差にしていない (preview の正は P3-3 の A8 と provenance の色と上の規則)。
1/255 の違いを製品の品質として受け入れるかは未判断として roadmap に残した。

[事実] 途中の失敗を保持した。`-a` / `-b` は harness が cache directory を相対 path で渡し、Manim が
script の path を二重に解決して静止の描画が終了コード 1 (`-b` は失敗の log を残すよう直した後)。
`-c` は作業 directory の写しの親を作らず (260 文字を超えうる path も含む) request.json を見つけられなかった。
`-d` は最初に通った実行で、`-e` で丸めの分類を足した。`-e` / `-f` は results.json の toolchain の改行を
escape せず JSON として不正 (計測値は `-g` と同一。log を比べて確かめた)。いずれも harness の誤りで、
製品の変更はしていない。`-g` の後に clang-format で harness と製品の整形だけを直した。

### 実 D3D11 の product preview の受け入れ

`math_equation_sequence_native_preview` (偽の backend、CTest) に加え、同じ区間の表で式を実の TeX にした
実 Manim の実行 (`mvm_test_equation_preview_controller --real-manim <manim.exe>`、CTest に入れない、
画面を表示するが入力は送らない、操作可):

[事実] `build/math-p34-real-preview-20261007-a.log` は **67 検査 / 0 失敗**。実 Manim の sequence の描画
約 10 秒。受け入れの 16 frame (H0、outline の先頭/中央/最後、outline の後、pulse の先頭/中央/最後、
pulse の後、T0 の frame 0/中央/最後に見せる frame、H1 frame 0、N=1 の pulse、N=1 の変形、H2 frame 0)
で、product compositor の readback は CPU の参照を黒へ source-over した値と、alpha 0/255 の画素も
半透明の画素 (frame ごとに 850〜7483 画素) も全て一致 (最大差 0、許容差なし)。対照の 1 画素ずらし・
色の取り違え・accent の省略は全て検出した。再生中の engine の評価は全て区間の frame かその代用で、
届いた pulse は区間の途中 (frame 20) から始まり frame 0 からやり直さない。
偽の backend の CTest (68 検査) も同じく最大差 0。`-a` の後に整形だけを直した。

### gate

```powershell
pwsh scripts/build.ps1
# 集中対象 28 件を -N で数えてから実行
C:\msys64\ucrt64\bin\ctest.exe --test-dir build/ucrt64-release `
  -R '^(transition_preview|math_.*|manim_.*|m7b_2_timeline_preview_mapping_focused|still_layer_compositor)$' `
  --output-on-failure --timeout 300
pwsh scripts/test-equation-preview-mutations.ps1
pwsh scripts/test.ps1 -Preset ucrt64-release -Group BuildIndependent
pwsh scripts/lint.ps1
pwsh scripts/test.ps1 -Preset ucrt64-release
```

[事実] 最終 source の全体ビルドは成功 (`build/math-p34-build-final.log`)。

[事実] 集中 CTest は 28 件 (`-N`、`build/math-p34-focused-count.log`) を一回実行し **27 / 28 通過**。
EquationSequence の新規 3 件 (model・controller・実 D3D11)、P3-1〜P3-3 の domain・compile・契約・橋渡し・
backend・cache・履歴、静止 (`math_controller_focused`)、P2 の変形 (`math_transform_native_playback` を含む)、
書き出し、inspector の product UI、`transition_preview`、preview の写像、静止画 layer の compositor は通過。
`math_write_native_playback` が 22 検査中 3 件失敗した (「一時停止中に mask を読み、合成に Write を付ける」
「先頭から再生する」「再生が 60 frame まで進む」)。P3-3 の集中試験の失敗と同じ 3 件。
証拠: `build/math-p34-focused.log`。P3-4 は Write の経路とこの試験を変更していないが、共有の
`MathRasterCache` と controller を link する。原因と今回の変更との因果関係は未特定。
診断として同じ試験を単独で 3 回実行し、通過・失敗 (同じ 3 件)・通過
(`build/math-p34-diag-native-write-1..3.log`)。間欠的であることの診断であり、失敗の記録を置き換えない。

[事実] BuildIndependent は **1077 / 1078 通過**。`audio_mixer_controls_qml` が P3-3 以来と同じ
QML ScrollBar の binding loop の警告 (`OpenThemeData() failed` が 193 行) で失敗した。今回は QML と、
それが読む file を変更していない。原因は未特定で、再試行していない。証拠: `build/math-p34-independent.log`。

[事実] lint は format、層の隔離、producer service、PSScriptAnalyzer を含めて通過 (`build/math-p34-lint.log`)。
開発中の lint は新規 file の未整形で失敗し (log は保存していない)、`scripts/format.ps1` で整形した。

[事実] 通常 release gate は一回で **1466 / 1467 通過**、通常 gate は未通過。失敗は BuildIndependent と同じ
`audio_mixer_controls_qml` (QML ScrollBar の binding loop、`OpenThemeData() failed` が 193 行) だけ。
新規の `math_equation_sequence_preview_controller` / `math_equation_sequence_native_preview`、
`transition_preview`、集中試験で失敗した `math_write_native_playback` はこの gate では通過した
(通過は失敗の記録を置き換えない)。performance / stability を除外し、extended / workstation は
短縮・除外していない。再試行はしていない。
再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。証拠: `build/math-p34-release.log`。

[事実] 開発中の記録: controller の集中試験の初回は 166 検査中 4 件が試験の側の誤りで失敗した
(変形の frame 0 は provenance の色が前の状態の色そのものなので「色の取り違え」の対照にならない、
frame 19 で組んだ animation を持たない区間の frame で評価した、OverBudget の印を前の上限の予約の返却が
消す競合)。試験を直して 166 / 166。log は保存していない。実 D3D11 の開発中の実行は
`build/math-p34-native-dev-1.log` / `-dev-2.log`。

P3-4 の実装・検証を完了した。schema 21、P3-1〜P3-3 の意味を維持し、書き出し・authoring UI、
commit、push は行っていない。

