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
