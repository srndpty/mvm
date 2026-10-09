# Native Graph Clip: P4-0

2026-10-08。P4-0 は製品外の設計・C++ 式試作・実 Manim 検証に限定する。
Project は schema 21 のまま。P4-1 の実装、製品 UI、preview/export 統合、production cache は追加しない。
コミット・push は行わない。P3 EquationSequence の PASS/CLOSED と過去の失敗・INVALID は維持する。

## P4-2 renderer, RGBA artifact and provenance

2026-10-09。以下は P4-2 の実装契約であり、上の P4-0 の履歴を変更しない。
schema22、graph-expression/1、graph-sampling/1、graph-draw/1 と timeline semantics は維持する。
P4-3 の residency/D3D11、P4-4 の製品 UI、P4-5 の export は追加しない。
gate と失敗を含む証拠の集計は [math-graph-p42-results.md](math-graph-p42-results.md) に置く。

### 中立 spec と座標

`app::compileGraphRender` が Project の構造検証と P4-1 の compile/sample を呼び、
`graph::GraphRenderSpec` または typed な `graph::Error` を返す。Project と Manim の接続は app に置く。
中立層は Qt・MLT・Project・Manim を参照しない。
spec は canvas、viewport、plot、順序付き background paths、曲線別 AST identity/effective domain/
切断済み geometry/ARGB/基準線幅/解決済み pixel 線幅、ラベル帯/内側矩形/回転、Draw N を持つ。
所有 ID は診断に残すが、正準 spec、JSON protocol、artifact key へ入れない。
Project JSON に spec・backend 名・artifact path を保存しない。

graph-layout/1 は canvas の各辺の1/8を余白、中央3/4を plot とする。
1920×1080 では左上(240,135)、plot1440×810。座標は比を先に求める affine map で
`left+(x-xMin)/(xMax-xMin)*plotWidth`、`top+(yMax-y)/(yMax-yMin)*plotHeight`。
軸は zero が viewport 内のときだけ描く。grid は独立で各方向11本、色#FF333A44。
軸は白、基準線幅2 pixel、grid の基準線幅は1 pixel。
曲線は関数順・segment順に、切断済み端点を丸め直さず直線として描く。
backend で式の評価・再標本化・smoothing・segment の接続をしない。

ラベルは下帯のx、左帯で90度回転するy、上帯を関数数で等分した関数ラベルの順。
空文字でも帯は保持する。中立層が帯の左右5%・上下10%を除いた content 矩形を確定する。
MathTex の実際の glyph 矩形を、その content 矩形へ等比で最大 fit し中央配置する。
内容・順序・実描画 bounds を C++ で照合し、非空入力の空 glyph は ArtifactCorrupt、
TeX の失敗は LabelFailure とする。数値式を TeX として解釈しない。

### 線幅と RGBA

保存線幅は1920×1080の pixel 単位。解決済み幅は
`referenceWidth*min(canvasWidth/1920,canvasHeight/1080)`。
縦横倍率が違っても線幅は等方的に保つ。Manim Camera は1 unitを1 pixelに固定し、
`backendStrokeWidth=resolvedPixelWidth/camera.cairo_line_width_multiple` で換算する。
この係数と Camera/VMobject の実ソースを toolchain fingerprint に含める。
stroke は pixel center が plot 内にある画素だけへ切り出す。
これは border の stroke footprint も切る明示的な raster 規則である。

RGBA は `rgba8-straight-source-over/1`。Cairo の premultiplied RGB を straight と誤認しない。
オブジェクトを白・不透明で個別に raster 化し、alpha 被覆だけを取得する。
各 path を background→関数→segment の順で、最後に白いラベルを source-over する。
同じ曲線の複数 segment もこの明示順で合成し、backend 独自の union は行わない。
被覆c・色alphaAから `a=round(c*A/255)`、既存straight画素のalphaをbとして
`w=a*255+b*(255-a)`、出力alphaは `round(w/255)`、各RGBは
`round((sourceRGB*a*255+underRGB*b*(255-a))/w)`。w=0なら全byteを0にする。
丸めは非負の最近接、ちょうど半分は上側。各段階でRGBA8へ量子化する。
将来のpreview/exportも、この完成済みstraight RGBAを同じ意味で読み、別の丸めを導入しない。

独立 oracle は long double の source-over と明示した重なりの期待byteを使う。
axis/grid の AA 被覆は診断用白RGBA PNGへ保存し、被覆という共通のcanonical段階から
高精度 oracle を適用して、backend の完成RGBAと照合する。合成実装のhelperは呼ばない。
AA 境界でも単色のstraight RGBが変わらず、alpha0のRGBは0であることを検査する。
線幅の被覆積分は固定した水平fixture専用であり、量子化差をpreview/exportの許容差にしない。
細い正の線幅がCairoで消える場合を、全透明PNGの一律拒否で隠さない。
一方、十分な幅・alpha・長さがある内側の線を全透明へ縮退させた出力は拒否する。

### Draw、key、toolchain

Draw frame i の点列は P4-1 `reveal` の i/N だけで確定する。
0..N-1と別のstatic endpointを保存する。frame N以上の中立 geometry はstaticそのもの。
ManimのCreate・秒・履歴を使わない。Drawのendpointは照合専用に再描画し、decoded RGBA全byteを
staticと照合する。endpointの照合PNGはjobの証拠だけに残し、追加のtimeline状態にしない。
axes/grid/labelsは静止。alpha0曲線・正当な空曲線・曲線のないframe0は成功できる。

静止 namespace はmvm-graph-static/1、Drawはmvm-graph-draw/1。
静止はcanonical AST、viewport/effective domains、曲線順、geometry、全style、labels/layout、
expression/sampling/layout/RGBA版とtoolchainをbyte長前置でserializeしてSHA-256。
Drawはstatic key、N、graph-draw/1を同じ方式でhashする。
UUID、FunctionId、Project filename、start、trim/split、effects、L、FPSは入れない。
Nだけの変更はstatic keyを変えない。artifactの物理locationはkeyと別である。
Drawの総量budgetを超えるNでも静止identityは計算できる。総量は生成要求で拒否し、
静止だけを要求する場合は中立specのDraw枚数を0にして同じstatic keyを使う。

preflightは明示したPythonと固定backend scriptを使う。Manim0.21.0を要求し、実際の
Python/NumPy/Pillow/pycairo/Cairo、Camera/VMobjectソース、TeX template、latex/dvisvgm/kpsewhichの
版とexe SHA、Computer Modernのtfm SHAを取得する。C++ compiler版/build種別/flagsと
numeric/presentation/adapterのsource/header SHA、およびbackend script SHAを加える。
renderの直前にも環境とscript authorityを照合する。identity不明の新規生成は失敗する。
将来のexportは、信頼できる期待specと記録済みtoolchain identityを保持している場合に限り、
Manimを起動せず`validateArtifact`で既存artifactを検証できる。P4-5はここでは実装しない。

### 成果物と公開

static.pngとDrawのframe-0.png..frame-(N-1).pngは透明RGBA8 PNG。
manifest.txtはUTF-8のbyte長前置field列であり、deterministicな完全一致で検証する。
順序はstatic/Draw namespace、static/Draw key、toolchain、compiler/sampling/layout/Draw/RGBA版、
width/height/N、geometryを含むcanonical specのSHAと本文、その後に各frameの
index/name/encoded SHA/decoded RGBA SHA、最後にstatic endpointの関係と画素SHA。
NoneではDraw keyを空にし、static一枚だけを要求する。
読み取り検証はManim・preview residencyを使わず、期待spec/provenance、PNGのRGBA8・寸法・
decode・透明RGB・可視性・全hash・必須frameを独立に照合する。存在するだけではReadyにしない。

新規job→backend構造検査→raster検査→hash→cache内の新規pending directoryへPNGをコピー→
コピー後の再検証→flushを伴うatomic manifest write→既存を置換しないdirectory rename→Ready。
manifestは最後に書く。jobのTeX・request・構造報告・白い被覆・process出力を消さず、
cacheにはPNGとmanifestだけを置く。壊れた同keyを黙って旧keyへ置き換えない。
同keyの重複要求は独立jobで生成し、公開を直列化する。後着は先着artifactを再検証する。
新しいkeyの失敗で以前の正常artifactを削除しない。

`PublicationAuthority`の世代変更と公開は同じmutexで直列化し、非置換renameを公開のlinearization pointとする。
P4-2.1では重い staging I/O と全画素検証をmutex外へ移した。応答性と静止cacheの契約は
[math-graph-p421.md](math-graph-p421.md)を参照する。static identityの安定性は異なるDraw N間の物理共有を意味しない。
supersede/shutdownは実行中のbackendへ取消を伝える。取消・旧世代をrenderer失敗と区別し、
検証・コピー・最終公開でも再検査する。既存Win32 Job Objectのprocess-tree取消を再利用する。
同期generateを使う呼び出し側はshutdown後に実行中呼び出しをjoinしてからauthorityを破棄する。
自動retryはしない。publicationのwrite/rename失敗はPublicationFailureとして返す。

### 上限と gate

canvas各辺1..8192、総pixel16,777,216、点数196,608、JSON32MiB、RGBA一枚最大64MiB、
Draw1..10,000（Noneは0）、decoded全frame合計とencoded全PNG合計それぞれ1GiB、
processのstdout/stderr各1MiB、backend timeout最大300秒。
実行前の計算と逐次検査でResourceLimitを返し、密度を落としたり部分出力で成功にしない。
NumPyの合成には複数のuint64作業面が必要であり、一枚のRGBA byte数をpeak RAMと呼ばない。

【操作可】以下はoffline描画と通常検証であり、performance/ETW/DWMの測定ではない。
通常releaseでは既存display-power leaseを取得し、背面・入力透過のGUI試験を使う。

```powershell
pwsh scripts/math-p42-gate.ps1 -Stage Focused
pwsh scripts/math-p42-gate.ps1 -Stage Real
pwsh scripts/math-p42-gate.ps1 -Stage Mutations
pwsh scripts/math-p42-gate.ps1 -Stage Regressions
pwsh scripts/math-p42-gate.ps1 -Stage BuildIndependent
pwsh scripts/math-p42-gate.ps1 -Stage Lint
pwsh scripts/math-p42-gate.ps1 -Stage Release
pwsh scripts/math-p42-report.ps1
```

[事実] 2026-10-09、必要な全 gate が通過したため P4-2 は PASS/CLOSED。
証拠は以下の新規取得に固定する。件数・線幅測定・初期失敗を含む結果は
[機械集計](math-graph-p42-results.md) を参照する。

|gate|最終証拠|
|---|---|
|Focused|`build/math-p42-20261009-011019-Focused`|
|Real・独立 alpha oracle|`build/math-p42-20261009-011100-Real`|
|17変異・復元後ビルド|`build/math-p42-20261009-011338-Mutations`|
|P4-1/P3回帰|`build/math-p42-20261009-010134-Regressions`|
|BuildIndependent|`build/math-p42-20261009-010327-BuildIndependent`|
|Lint|`build/math-p42-20261009-012027-Lint`|
|通常release・display-power lease|`build/math-p42-20261009-011640-Release`|

P3/P4の過去の失敗・INVALIDは変更しない。P4-3 は GO・未着手であり、
この閉鎖は preview/residency・製品UI・export の完了を意味しない。

## 回収した P3 の制約

[事実] [P3 の記録](math-equation-sequence-p30.md) と現在の `src/project/project.h`、
`project_json.cpp`、`src/media/math/equation_sequence_render.*`、`src/media/manim/manim_scene.*` を確認した。
内部時間は整数 source frame と有理数 FPS、区間は半開区間、animation は i/N で静止端点を別に扱う。
コピーの所有 ID と artifact key は別である。無効な editable source と実行時の失敗を区別する。
renderer の非 NULL や存在するファイルだけを成功にせず、構造・画素・provenance を検査して公開する。
P3 の A8 二層は単色 glyph と accent の合成用であり、多色 Graph の artifact 型として流用しない。
P3-6.1 の可視 source 範囲に必要な artifact だけを要求する規則を、後続 Graph export にも適用する。

## 対象と所有境界

2D Cartesian、y=f(x)、1〜3 関数、固定した有限 x/y 範囲、線形軸、任意の grid、
x/y ラベル、任意の関数ラベル、曲線別色・線幅、静止、任意の Draw intro に限定する。
parametric/polar/3D/implicit、領域塗り、vector field、点や接線、morph、camera animation、
任意 Python/Manim、幾何構成、データ chart は含めない。任意 Manim は既存 Script Clip が担当する。

```text
Project の GraphClipData
  → mvm parser → 中立 AST → C++ evaluator / 中立な切断済み点列
  → GraphRenderSpec → GraphRenderer
  → 初期 Manim + MiKTeX → immutable RGBA artifact
  → 後続の native preview / export
```

Project は backend の名前、Python、SVG path、raster filename、object identity を持たない。
配置・scale・rotation・全体 opacity は `ClipEffects`。Graph 内へ複製しない。

## P4-1 に渡す domain 契約

以下は提案であり、公開ヘッダの追加ではない。

```cpp
GraphClipData { GraphViewport viewport; GraphAxes axes;
                vector<GraphFunction> functions; GraphIntro intro; }
GraphViewport { double xMin, xMax, yMin, yMax; }
GraphAxes { bool showAxes, showGrid; string xLabel, yLabel; }
GraphFunction { GraphFunctionId id; string expression;
                optional<double> domainMin, domainMax;
                string label; uint32_t colorArgb; double strokeWidth; }
GraphIntro { GraphIntroKind kind; int64_t frames; } // None / Draw
```

構造は常に functions 1〜3 個。ID は非空の ASCII 英数字・`-`・`_`、最大64 byte、clip 内で一意。
functions の順序は重なりの描画順であり identity ではない。viewport は各値・差が有限、
xMin<xMax、yMin<yMax。domain の両端は省略時 viewport の端、指定時有限で min<max、
viewport と正の長さで交差する。空交差は InvalidGraph。線幅は基準 1920x1080 raster の
pixel 単位で 0<width<=64、有限。出力解像度に比例して拡大する。試作の線幅3はこの実装ではなく
Manim の backend 単位による視覚試験であり、pixel 換算の実装は P4-2 が担当する。
背景は透明固定。色は既存 `parseArgbColor` の `#AARRGGBB` を使い、保存は大文字正準表現。
alpha 0 の曲線もデータとして許す。全透明を破損とする試作の検査は可視色の fixture 専用であり、
製品では期待可視性を spec から求める。

source は UTF-8 の原文を保存する。上限4096 byte、labels はそれぞれ4096 byte。
空・構文不正・未知識別子の source も保存・再編集可能。原文の空白や括弧を勝手に変更しない。
「persisted canonical source」は意味の書き換えを伴うため採らず、key のために AST を正準化する。
不正 source は原文の診断用 identity を持つが drawable artifact は作らない。
ラベルは数値の正ではなく、明示的な数学表示用文字列。空は表示なし。自動 TeX 変換はしない。
domain/viewport/style/ID の構造不正は loader が拒否し、数式の editable-invalid と混同しない。

## 式の言語と数値 authority

```text
sum     := product (('+' | '-') product)*
product := unary (('*' | '/') unary)*
unary   := ('+' | '-') unary | power
power   := atom ('^' unary)?
atom    := number | 'x' | 'pi' | 'e' | '(' sum ')' | function '(' sum ')'
function:= 'sin' | 'cos' | 'tan' | 'exp' | 'log' | 'sqrt' | 'abs'
number  := (digits ('.' digits?)? | '.' digits) ([eE] [+-]? digits)?
```

関数はすべて一引数。暗黙の積は非対応（製品受け入れの 2x+1 は `2*x+1` を入力する）。
優先順位は sum < product < unary < power < atom、累乗は右結合。
`-x^2=-(x^2)`、`2^3^2=2^(3^2)`、`2^-2=0.25`。
空白は ASCII space/tab/CR/LF のみ。decimal は `.`、locale 非依存の `from_chars`。
識別子は小文字の列、未知識別子は UnsupportedExpression、token/arity/括弧違反は InvalidExpression。
属性・index・underscore・文字列・lambda・comprehension・任意 identifier は実行言語へ渡さない。
TeX を numeric language にしない。中立 AST は Literal/Variable/Unary/Binary/Call の許可 enum のみ。
source 最大4096 byte、AST 深さ64（括弧・unary・power の parser recursion も64）、nodes 最大4096。

数値は UCRT64 C++20 の binary64、fast-math 無効、各演算後に有限性を確認する。
pi/e は `std::numbers`、三角関数は radian、log は自然対数。
division の ±0 は UndefinedDivision、sqrt 負/log 非正は UndefinedDomain、
負底の非整数累乗等の NaN は UndefinedDomain、無限大は UndefinedOverflow。
tan は `abs(cos(arg))<1e-12` を UndefinedDomain とする（versioned な明示規則）。
0^0 は C++ pow の1を採用する。x 自体も有限を要求する。
同じ toolchain/build では再現可能な順序で評価し、異なる libm/CPU での bit 完全一致は保証しない。
numeric/compiler version と toolchain fingerprint を artifact identity に含める。

syntax invalid / semantic unsupported / sampled x で undefined / finite を別状態にする。
undefined は曲線の辺を切る理由で、clip 全体の syntax failure ではない。
NoFiniteSamples は有効 domain に drawable な有限の二点以上の segment がない場合。
全 finite 点が viewport 外なら、正常な空曲線 artifact と警告にする。axes/labels の可視性とは別に判定する。

## sampling と不連続の契約

**B: mvm が点列を所有し、backend は raster 化だけを担当する。**
A（AST から作った callback を backend plot に渡す）は任意コード注入を防げても、
sampling、非有限点の扱い、smoothing、切断規則が backend に依存する。初期実装では使わない。
Manim に user source や numeric callback は一切渡さず、finite な点列だけを JSON で渡す。

version `graph-sampling/1` は固定密度と midpoint 検査を採用する。適応的再試行は行わない。
plot 内幅 Wpx・高さ Hpx を render spec の layout で決め、M=ceil(4*Wpx)、
domain と viewport の交差の x を等間隔 M 分割する。domain の狭さにかかわらず同じ上限密度で標本化する。
各隣接 finite 点の中央 x も評価する。未定義の点・中央があれば辺を捨て segment を切る。
`abs(y1-y0)*Hpx/ySpan > Hpx/8` または
`abs(ymid-(y0+y1)/2)*Hpx/ySpan > 0.5px` なら辺を捨てる。
閾値ぴったりは接続する。孤立一点は線にしない。backend の smoothing は禁止し直線だけを使う。
最終曲線は viewport rectangle に辺単位で clip し、範囲外を結んで戻ることはしない。
点列を丸め直さず17桁の round-trip decimal で渡す。
試作は W=640、H=360、M=2560、jump=45px。production は M<=32768、最大評価65537/関数、
超過は EvaluationFailure（SamplingBudgetExceeded）とし黙って密度を落とさない。

式評価・切断・budget は中立 numeric compiler の責務。raster ごとの W/H・余白・stroke AA は
versioned な中立 presentation policy。閾値を Manim に隠さない。解像度変更は点列と artifact を無効化する。
点列 B は preview/export に同じ artifact を渡せ、将来 backend へ移植できる。
高密度直線と Cairo AA を使い、smooth curve 補間による overshoot を避ける。点数・JSON 転送量の代価がある。

これは記号的な連続性証明ではない。極端な高周波や二つの標本間だけの特異点をすべて検出できるとは主張しない。
そのような式を別の意味に rewrite しない。精度限界を診断表示し、sampling version を更新する場合は
新しい独立 fixture と key version を使う。普通の 1/x、tan、sqrt、log の規定例について実測で検査する。

## 座標・軸・ラベル

グラフ座標は raster と独立、plot rectangle への affine mapping だけを使う。
showAxes=true で y=0 が viewport 内なら横軸、x=0 が内なら縦軸を出す。
zero が範囲外なら該当軸は描かず、端に偽の zero 軸を作らない。grid は axes と独立。
tick/grid の手動値は保存しない。初期 `graph-layout/1` は各範囲の10等分（横11本、縦11本）、
tick はその grid と同じ位置、数値 tick label は初期範囲外。
試作の縦 grid は8等分であり、等分方針の feasibility を調べたもの。製品規則の golden ではない。
軸は白、grid は `#FF333A44`、これらも render spec の resolved color に残す。

x label は下の帯、y label は左の帯で90度回転、function label は上の帯を描画順に割り当てる。
長いラベルは帯へ等比縮小し plot を侵食しない。空欄の帯は保持して layout を変えない。
初期の基準 layout は plot 1440x810、canvas1920x1080、上下各135、左右各240。
function label は上帯を関数数で等分し、x/y は各一帯。TeX の支持範囲は既存 Math と同じ backend 制約、
TeX error は RendererFailure(LabelFailure)、空 glyph の非空ラベルは ArtifactCorrupt。
式とは別の表示入力であることを editor で示す。

## Draw と timeline

None は frames=0、Draw は1<=N<=10000。clip の普通の duration と intro 長は別。
source 時間は作成時の Project FPS の有理数、`sourceFrameCount=L`、L>=1、0<=in<out<=L。
N<=L を要求する。N=L なら endpoint は artifact として存在するが、その clip の可視範囲には含まれない。
source i<N は progress=i/N、i>=N は静止端点。N=1 は曲線なしの frame0 の次から完成形。
axes/grid/labels は最初から静止。関数ごとに有効 domain の min+progress*(max-min) を境に、
x の増加方向へ reveal、全関数同時、線分の境界点は直線内挿。長さ比例の Create は採用しない。
history、Manim 秒、scene playback は authority にしない。

左右 trim は source 範囲だけを変更、split は完全な関数集合を両片へコピーし、可視 source 範囲を分ける。
Draw 途中の右片は同じ source progress から続く。duration 延長は L を伸ばし static endpoint を保持、
intro frame の暗黙変更なし。短縮は N>L なら拒否して先に intro を明示編集させる。
速度変更は初期では拒否。Project FPS 変更で source FPS と整数 frame を換算し直さない。
`clipTimebase` の frame 始点標本化を使い、seek はその内部 frame だけから解決する。

add は新 FunctionId、delete は対象だけ削除（最後の一つは拒否）、reorder は ID を保持する。
expression/domain/style/labels の編集は ID 保持。copy/paste/duplicate は外側 ID と全 FunctionId を新規発行。
split は左の ID を保持、右の外側 ID と全 FunctionId を新規発行する。Undo/Redo は確定した ID を復元し再発行しない。
clip 間の function 参照は初期では存在しない。ID を artifact key に含めない。

## renderer と artifact の提案

`GraphRenderSpec` は compiler/layout/sampling version、resolved colors/widths、canvas、
plot rectangle、ラベル、有限点列と segment topology、N と phase version を持つ。
`GraphRenderer` は request/cancel→typed result、`GraphRenderArtifact` は immutable dimensions、
frame index、RGBA encoding、hashes と provenance。domain/public interface に Manim の型や API 名を出さない。
core/project は純粋、numeric compiler は core、render 契約は独立 `src/media/graph`、
Manim との唯一の接点は `src/media/manim` 内に置く。Qt・MLT を持ち込まない。

初期 artifact は透明 straight RGBA PNG の静止一枚＋Draw 0..N-1 の連番を推奨する。
endpoint=N は static と byte/pixel 同値を検査する。PNG alpha は真の多色 raster を保持するため
P3 の白 glyph A8 へ押し込まない。transparent pixel RGB は0へ正準化し、premultiply は合成境界だけ。
普通の MP4 は alpha authority にしない。Manim の movie writer を通さず Camera から直接保存する。

静止 key namespace `mvm-graph-static/1` は正準 AST（binary64 bit pattern）、domain、viewport、
描画順、labels、ARGB、width、axes/grid、raster/layout/sampling/compiler versions、backend template と
toolchain fingerprint（Manim/MiKTeX/dvisvgm/font・numeric build）を length-prefix で serialize して hash。
同じ key に点列 digest と検査結果を provenance で束ねる。Draw key `mvm-graph-draw/1` は static key、
N、phase/reveal version を含む。None は Draw artifact 不要。intro N の変更は静止を無効化しない。
式・viewport・domain・labels・色・width・grid/axes・描画順は静止とDrawを無効化する。
clip UUID、function ID、timeline start、trim/split、outer effects、L、FPS は renderer key に含めない。
FPS は i/N の画素を変えず、duration の mapping だけに使う。toolchain が解決できない新 session は
検証済み identity のない artifact を成功として使用しない。

publication は一時 job → 構造検査 → raster 寸法/channel/期待可視性検査 → hash →
provenance-last atomic publication。欠損0扱い・stderr分類・半端な連番の代用はしない。
任意 seek は immutable manifest の frame index から一枚を読み、frame0 の replay は不要。
将来の residency は今の一枚と有限な先読み、共有 budget と世代/ticket による stale result 排除を使う。
P4-0 は cache/publication/residency を実装していない。

共有候補は process/cancel、toolchain detection/fingerprint、job/canonical serialization、
atomic publication、cache directory、budget/ticket。共有は型の意味が一致する小さな層に限る。
MathClipData や EquationSequenceArtifact への type-punning はしない。

## typed failure

|型|正と扱い|
|---|---|
|InvalidGraph|関数数/ID/domain/style/timing の構造不正、loader/edit commit 拒否|
|InvalidViewport|有限性/範囲の不正、loader/edit commit 拒否|
|InvalidExpression|構文・arity・literal/制限違反、原文は保存・編集可能、render不可|
|UnsupportedExpression|未知関数/識別子、原文は保存・編集可能、render不可|
|EvaluationFailure|numeric compiler/budgetの失敗、sourceは保持、artifact公開不可|
|NoFiniteSamples|有効segmentなし、sourceは保持して理由を表示|
|BackendUnavailable|toolchain未導入・identity未取得、環境状態|
|RendererFailure|起動/TeX/描画失敗、環境またはlabel内容の診断|
|ArtifactCorrupt|構造・寸法・channel・hash・連番/provenance不一致、公開/使用拒否|
|Cancelled|requestの取消、公開せず失敗の再試行として数えない|

sample-local UndefinedDivision/Domain/Overflow は segment diagnostics に集計し、
typed enum＋location を正とする。stderr は補助証拠のみ。部分未定義でも有限segmentがあれば成功可能。

## schema 22 の提案（P4-1 の変更）

`TimelineClipKind::Graph`、JSON kind=`graph`。media path は空、video track のみ。
source FPS/count/in/out と普通の effects を使い、Graph を単純に `isStillClipKind` に足さない。
新 kind は `graph` object 必須、他 kind に graph payload は拒否する。

```json
{
  "graph": {
    "viewport": {"x_min": -5, "x_max": 5, "y_min": -5, "y_max": 25},
    "axes": {"show_axes": true, "show_grid": true, "x_label": "x", "y_label": "y"},
    "functions": [
      {"id": "f1", "expression": "x^2", "label": "y=x^2", "color": "#FFFF6655", "stroke_width": 3},
      {"id": "f2", "expression": "2*x+1", "label": "y=2x+1", "color": "#FF55CCFF", "stroke_width": 3}
    ],
    "intro": {"kind": "draw", "frames": 3}
  }
}
```

viewport/axes/functions/intro と例示内の各 field はすべて必須。
domain_min/domain_max だけ省略可能（viewport端）、null は拒否。creator の既定は viewport[-5,5]²、
axes/grid true、axis labels x/y、一関数x^2、function label空、色#FFFF6655、width3、None/0。
既定値は新規作成用であり、不完全JSONを補って通す規則ではない。
重複fieldと graph subtree の未知field は拒否。現在 EquationSequence の payload と専用 clip が
未知fieldを拒否する方針に合わせ、Graph clip の未知fieldも拒否する。
他の既存kind/Projectの未知field方針は変更しない。
schema21→22 は既存データを変更せず version を更新、新kindへ変換しない。
schema21 以下で graph は拒否、schema22 を旧loaderへ読ませる互換分岐は足さない。
不正式を含むGraphも構造が正しければ round-trip、診断/AST/raster path は保存しない。

## 実験と証拠

実行: [C++ 試作](../scripts/spikes/math-p40-expression.cpp)、
[Manim matrix](../scripts/spikes/math-p40-graph.py)。製品コードは変更しない。
公式 toolchain の UCRT64 g++ C++20 -O2、Manim0.21.0、MiKTeX26.5、dvisvgm3.6 を使用する。
【操作可】オフライン raster 描画であり desktop/presentation の測定ではない。

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
& C:\msys64\ucrt64\bin\g++.exe -std=c++20 -O2 -Wall -Wextra -Werror -pedantic scripts/spikes/math-p40-expression.cpp -o build/math-p40-expression.exe
& .\build\math-p40-expression.exe --test
& "$env:APPDATA\uv\tools\manim\Scripts\python.exe" scripts/spikes/math-p40-graph.py build/math-p40-expression.exe build/math-p40-<新しい名前>
```

最初の `build/math-p40-20261008-a/report.json` は子process DLL path不足で0xC0000135、
MiKTeXのsandboxログ書き込み拒否もstderrに出た。描画件数0で成功扱いしていない。
PATHを明示し実MiKTeXの書き込みが許される環境で実行したbのreportは11条件、負例12件、
Draw N=1/3任意順が成功。bの長いlabelの図を目視し重なりを発見したので、
配置を上下左の別帯へ修正しcで検証する。a/bを上書き・削除せず保存する。
最終結果と機械集計は後述する。文書へ計測値を手転記しない。

[事実] P4-0 は PASS/CLOSED。最終証拠は `build/math-p40-20261008-f/report.json`、
画素は同じ directory の PNG、点列は `*-geometry.json`。
[機械生成の集計](math-graph-p40-results.md) に起動時間、静止/Draw 時間、artifact byte、
一枚の RGBA buffer の RAM、関数数・label/grid・不連続の差を記録した。
式試作35件、静止11条件、負例15件、Draw N=1/3 の直接任意順・静止端点照合が通過した。
label mismatch と raster 寸法/空画素の負例は正常系と同じ検査関数で拒否した。
各曲線の resolved color の不透明画素も検出した。全11図で transparent pixel の RGB は0、
中間 alpha が存在した。label図・1/x図を実際に表示し、別帯の配置と極の非接続を確認した。
c/d は段階的に負例や端点検査を追加した証拠として保存、e は起動・N枚生成の独立計測と色検査を加えた。
f は深さ64の左結合ASTを受け入れ、65を拒否する独立境界テストを追加した最終run。
stdout/stderr は `build/math-p40-20261008-f.log`、試作sourceとexeの保存は
`build/math-p40-20261008-f-sources/`。各変更を理由とする別runであり、
同一条件を成功まで反復して選別していない。b のlabel重なりも過去証拠のまま保持する。

lint は `pwsh scripts/lint.ps1` が成功、証拠 `build/math-p40-lint-20261008.log`。
公式format/lintの対象外にある試作C++も、clang-formatのdry-runとg++の警告エラー化で検査した。
Python二ファイルの構文検査も実行した。production/shared renderer は触らないため、
通常full release gate と既存P3 regressionは今回の必須gateではなく実行しない。
P4-1 を開始せず、上記のdomain・言語・sampling・timing・schema提案をその実装契約にする。

集計の再生成:

```powershell
& "$env:APPDATA\uv\tools\manim\Scripts\python.exe" scripts/spikes/math-p40-report.py build/math-p40-20261008-f/report.json docs/math-graph-p40-results.md
```

Manimの正式APIは [Camera.capture_mobjects](https://docs.manim.community/en/stable/reference/manim.camera.camera.Camera.html)
による直接raster化と [VMobject.set_points_as_corners](https://docs.manim.community/en/stable/reference/manim.mobject.types.vectorized_mobject.VMobject.html)
による直線点列を使用した。callback候補は固定した安全な1/x演算と明示 discontinuities を使い比較する。

## 後続の閉鎖条件

|段階|担当する契約|
|---|---|
|P4-1|純粋domain/parser/AST/evaluator/sampling/timing、schema22、編集ID・Undo、独立negative fixtures|
|P4-2|中立render spec・Manim点列・label構造・pixel換算・RGBA連番・provenance/publication・cache key|
|P4-3|共有residency budget、stale防止、seekable native preview、透明動画上の合成|
|P4-4|既存暗色パネルのGraph authoring、無効原文の修復、狭い/低いpanelで全操作到達|
|P4-5|現在入力と可視範囲が正のvideo export、UIからの保存再読込・E2E閉鎖|

製品UIで x^2 と 2*x+1、x[-5,5]/y[-5,25]、grid/labels、Drawを作成し、
別clipで1/x、範囲[-5,5]²を作る。保存→close/reopen→N-1,0,middleのseek→
Draw途中trim/split→video上preview→映像exportを実行する。
pre-encoder frameのsource identity、phase、曲線分割、alpha合成を独立C++期待値と
検証済みartifactで比較する。export成功だけでは閉鎖しない。N=1/3、複製のartifact共有、
無効式保持、未知識別子、backend不在、破損manifest/raster、Draw途中右片の継続も必須。
P4-0の性能値にPASS閾値は設けない。量の最適化は後続の正しさのgateとは別に判断する。
未解決の改善・検証課題は [roadmap.md](roadmap.md) だけで管理する。

## P4-1 domain, numeric compiler, sampling and schema 22

P4-0 の上記記録は当時の設計・実測として保持する。以下は P4-1 の製品実装である。
閉鎖判定は focused、変異、既存回帰、BuildIndependent、lint、通常 release の証拠が揃ってから行う。

### API と構造の正

`src/project/graph_clip.*` は GraphViewport/GraphAxes/GraphFunction/GraphIntro/GraphClipData と
`validateGraph` を持つ。GraphValidationStatus は Valid/InvalidGraph/InvalidViewport。
関数数・ASCII ID・finite viewport/span・有効 domain の正長交差・線幅・byte 長・intro と L を検査する。
背景 field は持たず透明固定。色の検査は既存 `parseArgbColor`、保存は同じ parser を使う
`canonicalArgbColor` の大文字 ARGB。空・不正・未知の式は構造として有効で、原文を保持する。
ClipEffects は TimelineClip の既存 field のまま。

`src/project/graph_edit.*` は候補 Project の全体検証後に一度だけ置き換える。
新規は既存の 5 秒の既定尺を整数 source frame と作成時の有理数 FPS にする。
関数の追加・削除・並べ替え・置換は候補を検査し、最後の関数の削除を拒否する。
置換は ID で対象を特定し、式/domain/label/color/stroke の変更は所有 ID を保持する。
viewport/axes/intro の変更は `editGraph` の同じ全体検証を通す。
controller の createGraphClip/editGraphData/setGraphDuration は既存 commitProjectEdit に一回だけ確定する。
失敗時に履歴を追加しない。Undo/Redo は Project snapshot に保存された生成済み ID を復元する。

copy/paste/duplicate は外側 ID と関数 ID を発行し直し、trim は Graph データを保つ。
split/上書きの分割/時間編集の右片も関数 ID を発行し直す。左片は元の所有 ID を保つ。
`remapGraphIds` は旧 ID と今回の新 ID を予約し、発行試行は関数あたり最大128回。
Graph は video track のみで media path と media item を持たず、isStillClipKind には加えない。
速度変更・frame hold・リンクは構造として拒否する。

### 式・AST と資源上限

`src/media/graph/graph_numeric.*` は Qt/Project/Manim/Python/FFmpeg/MLT/D3D11 に依存しない。
graph-expression/1 の文法は上記 P4-0 と同じ。Operation enum と後置順の有限 vector を使い、
文字列による演算 dispatch はしない。number は `from_chars`、小数点と空白は ASCII のみ。
`-x^2` は `-(x^2)`、累乗は右結合、関数は一引数、暗黙の積を拒否する。
compile status と sample-local EvaluationStatus は別の型である。

source <=4096 byte、AST depth <=64、node <=4096、入力が増やす parser recursion <=64。
固定した sum/product/unary/atom 間の呼び出しは入力再帰 budget と区別する。
括弧・関数引数・単項・累乗の再帰は降下前に検査する。括弧64重は有効、65重は Recursion。
関数・単項・累乗64層は literal を含む AST depth65 が先に拒否する。65層は降下前の Recursion。
左結合の深い AST も node の追加時に拒否し、評価と正準化は反復で行う。
4096/4097 node の独立した大きい平衡木入力は、より小さい source byte 上限が先に拒否する。
node guard 自体は呼び出し側が上限を縮める CompileBudget で独立検査する。
4096 を超える指定で最大値を引き上げることはできない。既定の production 上限は変更しない。

canonical は `graph-expression/1:` に prefix 順の固定2桁 Operation code と、literal の
binary64 bit pattern の16桁 hex を連結する。子の順序と unary node を保ち、代数的簡約しない。
例: `1` は `graph-expression/1:003ff0000000000000`、
`x+1` は `graph-expression/1:0601003ff0000000000000`。
所有 ID と source の空白は含まない。将来の key はこの version と numeric/toolchain identity を別々に持つ。
cross-libm の bitwise determinism は主張しない。

### 数値・sampling・Draw

e/pi は std::numbers、log は自然対数。division/domain/overflow は enum で報告する。
入力 x と各演算結果の finite 性を検査し、未定義の value は成功として渡さない。
fast-math を target で明示的に無効化する。中間の未定義も後段へ伝播する。

graph-sampling/1 は M=ceil(4W)、M+1 の端点と finite な隣接端点の midpoint を評価する。
M<=32768、最大65537評価。密度の引き下げや適応的再試行はない。
中央未定義・H/8 超の縦変位・0.5px 超の中央偏差で辺を切る。閾値の等号は接続する。
幾何の差と平均は拡張精度で求め、極端な有限 binary64 値の差で overflow しないようにする。
関数値自体の authority は binary64 evaluator のまま。
finiteSamples は端点数、finiteEvaluations と未定義の分類別数は中央を含む全評価数。
discardedEdges/midpointFailures/jumpDiscardedEdges/deviationDiscardedEdges と segment vector を返す。

Liang–Barsky で各辺を viewport に clip する。切断後や可視範囲外を経由して再接続しない。
同一点の線・孤立一点・境界への一点接触は描画可能な線ではない。接触だけの可視結果は
ValidEmptyCurve、有効 domain 内で接続可能な二点の辺が無ければ NoFiniteSamples。
正常な空曲線を式の構文不正と混同しない。関数ごとに Geometry が所有 ID と有効 domain を持つ。

graph-draw/1 の reveal は source i/N と各関数の有効 domain から x 境界を求め、
交差辺を直線補間する。i>=N は static Geometry をそのまま返す。履歴・arc length は使わない。
evaluateGraphClip は既存 clipSourceFrameAt/clipTimebase の frame 始点標本化を使う。
trim/split の sourceInFrame を進捗から引かず、右片は元の位相を継続する。
L の延長で intro は変えず、N>L になる短縮は候補検証で拒否する。

### schema と検証の証拠

schema22 は graph payload を Graph kind にだけ要求する。Graph subtree の必須・未知・重複 field を
既存の strict object reader で検査し、domain の省略だけを許可する。明示 null は拒否する。
schema21 は既存内容を変えず22へ上げ、21以下の Graph と未来版は拒否する。
AST/diagnostics/segments/raster/backend を保存しない。loader は式の描画可能性を要求しない。
preview/export は Graph を明示的に未対応として拒否し、別の clip kind へ変換しない。

実行入口は `pwsh scripts/math-p41-gate.ps1 -Stage <Focused|Mutations|Regressions|BuildIndependent|Lint|Release>`。
各実行は新規 `build/math-p41-<timestamp>-<stage>/` に source snapshot/hash、revision、正確なコマンド、
選択件数、終了コード、stdout/stderr、CTest raw log を保存する。0件の focused test を拒否する。
変異は source の byte 列を退避し、finally で復元して SHA256 を照合する。
compile 失敗・crash・timeout は assertion による変異検出成功に数えない。
初期の失敗と環境切り分けは [初期記録](math-graph-p41-early-runs.md) を参照する。
最終 gate の [機械集計](math-graph-p41-results.md) は `scripts/math-p41-report.ps1` が生成する。
P4-0 の実測・閾値・過去分類は変更しない。

[事実] 2026-10-08、P4-1 は PASS/CLOSED。構造 domain、bounded parser/AST、数値評価、
切断・clipping、pure Draw、trim/split の source 継続、所有 ID、Undo/Redo、schema22 の
移行・round-trip、編集可能な無効式、security/resource 負例を集中試験で確認した。
最終 focused、15 個の変異検出、関連回帰、BuildIndependent、lint、通常 release 一回は
すべて通過した。件数と終了コードは上記の機械集計を正とする。
最終通常 gate は `build/math-p41-20261008-191124-Release/` に保存した。
初期の失敗証拠も保持している。P4 全体の閉鎖を意味せず、P4-2 は未着手である。

renderer/artifact/cache/UI/preview 実装/export 実装は追加しない。固定密度 sampling の限界と
後続の未解決事項は [roadmap.md](roadmap.md) の P4-2 以降で管理する。

## P4-1.1 式の空白境界

graph-expression/1 の凍結された空白は ASCII space (0x20)、tab (0x09)、
CR (0x0D)、LF (0x0A) のみである。P4-1 の parser が form feed (0x0C) と
vertical tab (0x0B) も読み飛ばしていた箇所を、この四種類へ限定する。
それぞれの正例と、先頭・末尾・token 間・空白だけの FF/VT の負例を追加した。
六種類を読み飛ばす旧実装へ戻す変異を `six-whitespace` として検査する。
他の P4-1 契約、schema22、過去の証拠は保持する。P4-2 は未着手である。

検証入口は `scripts/math-p41-gate.ps1 -EvidencePrefix math-p411 -Stage <stage>`、
変異は `-Stage Mutations -MutationCase six-whitespace`。新しい directory に保存し、
`scripts/math-p41-report.ps1 -EvidencePrefix math-p411 -Phase P4-1.1
-OutputPath docs/math-graph-p411-results.md` で機械集計する。

[事実] 2026-10-08、P4-1.1 は PASS/CLOSED。集中 parser/domain/history、旧六種類へ戻す
変異、関連回帰、BuildIndependent、lint、通常 ucrt64-release 一回が通過した。
件数・終了コード・source/revision は [機械集計](math-graph-p411-results.md) とリンク先の
新規証拠を正とする。通常 gate は `build/math-p411-20261008-234140-Release/` に保存した。
P4-1 の過去の gate と結果文書は上書きしていない。コミット・push は行っていない。
