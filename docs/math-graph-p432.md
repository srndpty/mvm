# P4-3.2: 時間依存 ClipEffects と composition 再利用

状態: **P4-3.2 PASS/CLOSED**、**P4-3 PASS/CLOSED**、**P4-4 GO（未着手）**。
commit・push は行っていない。

[事実] 既存の render-time motion により、懸念された評価済み効果の固定化は否定された。
製品の `stableGraph` / `graphMemo` は変更せず、試験入口・決定的な native 回帰・負例を追加した。
focused、real native、関連回帰、BuildIndependent、lint、通常 Release の全 gate が通過した。

## 再利用契約の調査

Project revision が同じでも、評価した提示値が同じとは限らない。今回の判断根拠は
revision 単独ではなく、以下の既存の render-time 契約である。

|状態|既存の authority と再利用時の扱い|
|---|---|
|Graph の Draw / 端点|`GraphPreviewAnimation::evaluate()` が出力 frame ごとに P4-1 の `evaluateGraphClip()` を使い、exact resident RGBA を選ぶ。memo に source frame を固定しない。|
|位置 X/Y、拡大 X/Y、回転、crop 四辺|`attachClipMotion()` が既存 `effectChannels()` の key 有無を調べ、不変 `ClipPreviewMotion` を付ける。`evaluate()` は当該出力 frame の local frame を求め、既存 `evaluateClipEffects()` と `mapClipEffects()` を呼ぶ。|
|不透明度と fade|同じ motion が既存 `clipFadeSourceFrameAt()` と `evaluateClipOpacity()` で評価する。Graph では既存 preview mapping の `still.opacity` も memo に含まれるため、値が変わる GUI 更新では新しい composition を作る。更新が遅れても render-time 評価は正しい。|
|効果の適用回数|engine の `addStillLayersLocked()` 内の motion 評価は、評価値を `rendered.destination`、`sourceUv`、`rotationDegrees`、`opacity` へ代入する。初回値へ加算・乗算して累積せず、外側 ClipEffects は一度だけ適用する。transition multiplier のみ明示的に掛ける。|
|音量、ducking、normalization|映像の提示座標・RGBA の効果ではない。audio の既存 authority を変更しない。`effectChannels()` の列挙には volume と ducking も含まれる。|
|動画|clip ID、slot、登録 source identity が memo に入る。request の source frame は再利用時も現在の mapping で更新する。動画の時間依存 ClipEffects も同じ motion を付ける。|
|track・timeline の出入りと順序|再利用判断は毎回、既存 `mapTimelinePreviewFrame()` の層集合を使う。clip ID と slot の列が変われば memo が変わる。Project 編集は revision、Graph clip 自体の変更は record の clip 比較でも拒否する。|
|timeline transition|動画の `transitionOpacity != 1` または `dissolveIncoming` は再利用対象外。Graph に関係する timeline transition は既存 Graph branch が提示対象から除外する未対応条件であり、再利用で対応済みに変えない。|
|字幕・他の still layer・編集中の override|現在の字幕がある、Graph 以外の still layer がある、ClipEffects override がある場合は `stableGraph` が成立しない。通常経路で構築する。|

ソースは [controller](../apps/mvm/mvm_controller.cpp) の `ClipPreviewMotion`、
`attachClipMotion()`、`previewCompositionFor()`、[channel 定義と評価](../src/project/clip_effects.cpp)、
[preview mapping](../src/app/timeline_preview_mapping.cpp)、
[render engine](../src/preview_engine/preview_engine.cpp) の motion 評価を照合した。
render engine は animation の画素を選んだ後、各 render frame で motion を評価してから compositor に渡す。
前の frame の値や GUI cache を参照する契約には変更しない。

## 変更前の決定的な比較

製品の `stableGraph` / `graphMemo` 判定を変更せずに検証した。
`previewCompositionForTest()` は既存の mapping と製品構築関数を呼ぶだけの試験入口で、
非再利用の比較呼び出しは memo を一時的に退避・復元する。ClipEffects の規則は追加しない。

native 試験は製品 controller が作る composition の motion を保持し、Graph の画素だけを
独立した識別色 RGBA fixture に置換する。連続側は case ごとに一度だけ composition を submit し、
render clock を 0 → 1 → 2 → 3 と進める。seek と frame ごとの再 submit は使わない。
参照側は同じ製品経路で各 frame の composition を新規構築し、GUI 側で評価済みの値を描画する。
参照側の motion を外すため、誤った render-time 評価を双方が追認することはない。

静的 Graph、animated opacity、位置 X/拡大 X、位置 Y/拡大 Y、Draw と opacity/変形、
Draw と変形だけ、回転、crop 左辺、crop 四辺、fade を比較する。
位置・拡大・crop と色・不透明度は実装の評価関数を呼ばない独立の全画素 oracle でも検査する。
回転・fade は frame ごとの非再利用経路との全画素比較を使う。
非静的 case では最初と最後の画素の変化を必須にし、比較対象が空振りになることを拒否する。

位置・拡大・回転・crop の evaluated value が変わっても、製品の memo と snapshot pointer が
同じであることを直接確認する。各 case の連続描画では composition revision と seek 件数も検査する。
既存の real Manim → artifact → native D3D11 の動画 alpha oracle と製品連続再生もそのまま実行する。
字幕・Graph-free 初期 seek・P3 native playback・timeline transition は既存の回帰を再実行する。

## 負例

今回の懸念は既存契約で否定されたため、「修正前の unsafe decision」を戻す対象は存在しない。
代わりに `attachClipMotion()` の Graph motion を外す変異を入れ、初回の評価済み効果を再利用する
不正な状態を作る。これは再利用の安全性に必要な render-time 契約の負例である。
位置・拡大の後続三 frame が、非再利用の基準と独立 oracle の両方から外れることを必須にする。
source を hash 完全一致で復元し、復元 build と同じ native 試験の成功まで確認する。
既存の製品再利用判定を不要に制限したり、memo へ output frame を無条件に追加したりしない。

## 検証入口と保存

`scripts/math-p43-gate.ps1` の Effects / Real / Regressions / BuildIndependent / Lint / Release を使い、
各 stage に新しい `build/math-p432-*` directory を指定する。
通常 Release は既存の display-power lease、背面・入力透過 window、workstation 排他で一回実行する。
新しい `graph_continuous_effects_native` は通常 CTest にも登録し、実行ごとに新しい証拠 directory を作る。
操作可。利用者の通常操作を止めない。

[生成した結果](math-graph-p432-results.md) は `scripts/math-p432-report.ps1` が生 JSON・TSV・CTest log から再計算する。
[P4-3.1 の PASS](math-graph-p431-results.md) と [P4-3 の歴史的 FAIL](math-graph-p43-results.md) は変更しない。

製品の再利用コードと歴史的文書の不変性は [source 照合](../build/math-p432-20261009-Release-01/reuse-source-proof.json)、
Release 開始時の検証対象との一致は [hash 照合](../build/math-p432-20261009-Release-01/source-match.json) に保存した。
