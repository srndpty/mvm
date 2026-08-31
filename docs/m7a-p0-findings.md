# M7a-P0 backend primitive 検証結果

## 再現手順とartifact

**[事実]** 次のfocused testは、MLTで生成した実MP4と既存D3D11
`GpuCompositor`のoffscreen出力を画素検査する。property読み戻しだけでは合格しない。

```powershell
pwsh scripts/build.ps1 -Target mvm_test_m7a_p0_mlt
pwsh scripts/build.ps1 -Target mvm_test_m7a_p0_gpu
ctest --test-dir build/ucrt64-release -L m7a-p0 --output-on-failure --timeout 600
```

生データと集計はbuild directoryへ生成される。

- `build/ucrt64-release/tests/m7a-p0-mlt.json`
- `build/ucrt64-release/tests/m7a-p0-gpu.json`
- `build/ucrt64-release/tests/m7a-p0-summary.json`

集計値は `scripts/summarize-m7a-p0.ps1` が生JSONから再計算する。この文書へ
計測値を手転記しない。

## Clip-local fade authority

**[事実]** Fade式は `core/clip_fade.*` の純粋helperに一本化した。入力は
`localFrame`、clip duration、fade-in/out frame数だけであり、timeline frame、
Preview output ordinal、presentation ordinalを受け取らない。

**[事実]** 非ゼロsource-inのMLT cutは、frameにparent producer positionを保持する。
`affine` filterのin/outを明示的なsource範囲へ設定すると、filter positionが
clip-localになり、helperが生成した0始まりkeyframeと実画素fadeが一致した。
source frameと無関係なoutput ordinalを異なる値にした対照testも追加した。

## MLT crop / affine

**[事実]** MLT `crop` filterは2段で動く。passive instanceがframeの
`crop.*`と`meta.media.*`を設定し、`active=1`のinstanceが実画素を切る。
active instanceへleft/top/right/bottomを直接設定するだけでは、値を読み戻せても
画素は変わらない。

**[事実]** `crop`後に`affine`を適用すると、cropped imageはtyped
`transition.rect`全体へfillされる。このままでは辺を透明化するmaskではなく、
cropした範囲の拡大表示になる。Premiere型crop maskには、crop率からvisible rectを
縮めて配置する決定論的な補正が必要である。

**[事実]** position、scale、`transition.fix_rotate_z`、opaque black背景上の
rect opacity、clip-local rect opacity animationは実MP4へ反映された。
rotationの中心markerは非回転時のrect中心と一致し、pivotはaffine rect中心だった。
今回の単一producer probeではcrop/affineのattach順を逆にしても観測結果は一致した。

**[回避策]** `affine` filterの背景はopaque blackを明示する。transparent背景では、
MP4 consumerがalphaを捨てた後に元のRGBが残り、opacityが画面上の暗化として現れない。

## GPU preview primitive

**[事実]** 現行`GpuCompositor`でasymmetric source UV、縮小destination、position、
opacityを同時に実画素確認できた。Fade opacityはdecoded source frameから
`localFrame = decodedSourceFrame - sourceInFrame`を明示計算している。

**[事実]** 現行GPU primitiveはaxis-aligned destination viewportだけを受け取り、
rotationを表現するfieldもvertex geometryも持たない。P0ではshader、PreviewEngine、
presentation accountingを変更していない。

**[推測]** 現行pixel shader、NV12/P010 sampling、blend、GPU lifetimeを維持したまま、
vertex側を中心回転するtextured quadへ局所拡張すればMLTと同じrect-center pivotを
表現できる。追加render pass、CPU readback、generic graphは不要と見込むが、これは
full M7a実装前の設計判断であり、P0の実画素rotation実績ではない。

## Gate

**[exit]** 既存serviceだけでMLTの全primitiveは成立し、GPUもrotation以外は成立した。
低コスト経路は存在する。ただしProjectのCrop/pivot semanticsはまだfreezeせず、
このP0結果のレビュー後に決定する。full M7a、M7b、multi-trackは開始していない。
