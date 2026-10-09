# P4-5.1: MLT 合成の authority と exact oracle

結論は **A: oracle の境界誤り**。Graph artifact と MLT の通常合成を別々に検査する。
今回の閉鎖対象は合成契約だけであり、P4-5 全体と P4 は HOLD を維持する。
実行結果は [結果文書](math-graph-p451-results.md) に機械集計する。

## 三つの境界

|境界|authority|画素の契約|
|---|---|---|
|A: Graph artifact|P4-2 renderer、decoded RGBA SHA、package provenance|straight RGBA8、`rgba8-straight-source-over/1` の非負最近接整数丸め|
|B: timeline layer|既存 qimage/color/affine、通常の track 順と ClipEffects|MLT 7.36.1 の RGBA8、binary32 演算後の byte 切り捨て|
|C: encoder 入力|最終 producer の RGBA audit|全 layer の合成後、YUV 変換前の `mlt_image_rgba`。B の byte を保持|

A の契約・renderer・keys・schema22 は変更しない。B の alpha を補正せず、Graph 専用の合成も作らない。
C は MP4 の復号 RGB や YUV を authority にせず、format・寸法・全 frame の照合を要求する。

## 実バイナリと対応するソース

インストール済みパッケージは `mingw-w64-ucrt-x86_64-mlt 7.36.1-1`。
`desc` と cache の `.BUILDINFO`、framework/core/plus/qt6/avformat の DLL と SHA を保存する。
`.BUILDINFO` の PKGBUILD SHA は
`9787dcdc4b65e4162f1e77311656bb363bc206eefca3e95ee7dd66f27e3f26df`。
これは [MSYS2 履歴版の PKGBUILD](https://github.com/msys2/MINGW-packages/blob/294df5bd8ade076c6580afe8546e6cc5979d0334/mingw-w64-mlt/PKGBUILD)
と一致した。source archive SHA は
`0d2b956864ba2ff58bb4e2b2779aa36870bd2a3a835e2dbfda33faa5fc6f4d3a` と一致する。
三つのパッチは install/manpage/x86 判定を変更し、画素演算を変更しない。
パッケージは Release、GCC 15.2.0-14、`!lto`/`!debug`。今回の mvm は公式 UCRT64 の
RelWithDebInfo（`-O2 -g -DNDEBUG`）、Qt 6.11.1 でビルドした。
ビルド時 Qt の記録は 6.11.0-3 であり、現在の実行時と同じだとは記述しない。

公式ソースの対応箇所は [interp.h](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/plus/interp.h)、
[transition_affine.c](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/plus/transition_affine.c)、
[qimage_wrapper.cpp](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/qt/qimage_wrapper.cpp)、
[filter_affine.c](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/plus/filter_affine.c)、
[consumer_avformat.c](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/avformat/consumer_avformat.c)。
取得ソース・archive・パッチの SHA とコピーは新規の gate 証拠に残す。
archive 内の調査対象七ファイルと取得した tag ソースを改行差だけ正規化して照合し、
一致結果を `build/math-p451-source-proof.json` に保存した。

## 独立した数学的参照

`F(x)` を IEEE 754 binary32 の最近接・偶数丸め、`T(x)` を非負値の整数切り捨てとする。
source byte を `S`、destination byte を `D`、ClipEffects の opacity を `o` とする。
普通の overlay は `b_alpha=0` であり、alpha を上書きする atop ではない。

```text
as = F(F(Sa / 255) * F(o))
ad = F(Da / 255)
a  = F(F(as + ad) - F(as * ad))
Aa = T(F(255 * a))
w  = F(as / a)
Cc = T(F(F(Dc * F(1 - w)) + F(Sc * w)))
```

各加算・減算・乗算・除算に F を置く。多層では一段ごとに得た RGBA8 を次段の D とする。
`tests/harness/mlt_rgba_oracle.h` は double 中間値を volatile float に丸める小さい CPU 参照であり、
MLT の関数・観測結果・graph::sourceOver を呼ばない。乗算加算の融合を避ける。
±1 の許容や alpha255 の固定は無い。

`Sa=128, Da=255, o=1` では as は `128/255` の binary32 近似で、
先に `as+1` を丸めてから as を引くため a は 1 より小さくなる。
`T(F(255*a))` は 254。normalized weight で色を計算して切り捨てると
`[100,49,15,254]` になる。255/255 自体は正確な 1 であり、背景 producer の alpha 欠損ではない。
旧 oracle の `[100,50,16,255]` は A の最近接整数契約を B に適用した値だった。

## format・affine・観測範囲

qimage は alpha を持つ画像を straight ARGB32 に戻し、RGBA8888 をコピーする。
resize が premultiply した場合にも straight へ戻すが、今回の exact fixture は
素材・profile とも 64×36 で resize の影響を切り分ける。
affine は RGBA8 を要求し、consumer.rescale が未指定の場合は nearest を選ぶ。
座標は逆 affine で計算し、範囲内だけを `rintf` でサンプリングする。
範囲外を新しい色に clamp せず、destination が残る。試験は整数座標の identity を使う。
ordinary ClipEffects の opacity は rect.o として as に掛かる。
spatial scale/crop/rotation の既存 Image/動画 export と EquationSequence の契約は関連回帰で確認する。
bilinear/bicubic や任意の rescale 構成へ nearest の式を流用する契約ではない。

MLT の両入力 alpha が 0 の nearest 合成は normalized weight の除算が定義域外になる。
製品の最下層は黒の不透明背景であり、透明背景は実 service の controlled 診断だけで調べる。
透明 destination に alpha1/128/254/255 を重ねるケースを検証する。
透明 source と透明 destination の組合せを成功扱いしない。
参照実装も定義域外・RGBA8 範囲外を拒否し、MLT に無い clamp を期待値へ追加しない。

製品の最終 producer の filter は、上流を `mlt_image_rgba` で取得した直後に callback を呼び、
consumer が要求した形式へその後で変換する。RGBA の alpha を変更しない。
callback が無ければこの filter は接続されない。
avformat の `encode_video` は get_image の完了後に format に合う plane を設定し、
`sws_scale` で encoder 用の色変換を行う。この順序が C を pre-YUV とするソース上の根拠である。

DLL 内部の interpolation 呼出しの前後を直接 hook する変更は行わない。
代わりに `src/media/mlt/mvm_mlt_rgba_diagnostic.c` が実 factory の qimage/color/affine を
一段ずつ実行し、producer、合成直前の両入力、直後を観測する。
これは早期に get_image を要求する controlled 診断であり、製品 tractor の全内部段階を
直接観測したものだとは扱わない。製品が実際に保存した各 staging PNG をコピーして
同じ診断に掛け、結果と製品 pre-YUV を全画素で照合する。
これにより元 PNG だけの対照や最終 Graph/Image 一致だけには依存しない。

## 行列と負例

alpha255/128/0/1/254、黒・色付き不透明背景、支持される透明背景、二層・三層・逆順、
static/Draw/endpoint、opacity50% を検証する。黒・色付き背景では通常 Image と Graph の
実 endpoint staging PNG を共有し、全 frame・全画素を照合する。
Draw は index ごとに独立した色を持ち、source frame の取り違えを見逃さない。
各 stage の raw RGBA・PNG SHA・RGBA SHA・oracle SHA・先頭差分座標/channel・差分数を保存する。
同じ frame が consumer から複数回取得されても、それぞれ照合し、distinct frame 数も要求する。

八つの source 変異は source frame、alpha 強制、Graph/Image 分岐、alpha 解釈、channel 丸め、
layer 順、audit の合成前への誤移動、背景 alpha を対象とする。
production の staging/loader/background/audit と controlled diagnostic、参照実装を個別に変更する。
DLL を編集せず、DLL に対する参照の解釈誤りも検出対象にする。
build 成功後の assertion 失敗だけを検出と数え、source は元 byte と SHA に戻す。
復元 build と同じ試験の PASS まで確認する。

## 変更範囲と再現

通常の MLT 合成処理・Graph の画素・音声・renderer・schema は変更していない。
製品コードへの追加は任意の staging 観測 callback、MLT adapter の追加は検証専用診断だけ。
主な修正は誤った post-MLT oracle の置換と検証・記録である。

【操作可】通常の PC 操作を続けられる集中検証。性能・desktop・実製品 UI の閉鎖は実行しない。

```powershell
pwsh scripts/math-p451-gate.ps1 -Stage Focused
pwsh scripts/math-p451-gate.ps1 -Stage Regressions
pwsh scripts/math-p451-mutations.ps1
pwsh scripts/math-p451-gate.ps1 -Stage Lint
pwsh scripts/math-p451-report.ps1
```

依存・frame loader は既存の 59 検査をそのまま実行する。
P4-2 artifact の長倍精度独立 oracle と期待値は変更していない。
過去 P4-5 の整数 oracle FAIL は履歴として保持する。
初回 P4-5.1 の色背景入力誤りと診断試験の compile error も閉鎖結果へ混ぜず記録する。
P4-5 の残りの実 UI→Manim→H.264、製品受け入れ、full release・BuildIndependent は別の閉鎖作業である。
