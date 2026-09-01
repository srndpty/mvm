# M7b-P0 MLT overlay primitive 所見

## 再現

```powershell
pwsh scripts/build.ps1 -Target mvm_test_m7b_p0_mlt
ctest --test-dir build/ucrt64-release -R '^m7b_p0_' --output-on-failure --timeout 180
```

生の入力、出力、実画素計測値は
`build/ucrt64-release/tests/m7b-p0-raw.json`、その機械集計は
`build/ucrt64-release/tests/m7b-p0-summary.json` に生成する。
計測値はこの文書へ手転記しない。

## 確定した graph

- [事実] `mlt_tractor` の track 0 に V1 playlist、track 1 に V2 playlist を置き、
  V1/V2 間へ V2 clip ごとの `affine` transition を plant すると、V2 の透明領域、
  crop、opacity、fade は黒ではなく V1 を露出する。
- [事実] V2 は source producer の明示的な cut を playlist へ入れ、前後と clip 間は
  `mlt_playlist_blank()` で埋める。非ゼロ source-in、非ゼロ timeline start、2本の
  非重複 V2 clip で映像または effect の漏れは検出されなかった。
- [事実] V2 へ M7a の affine filterを付ける必要はない。P0 graph の
  opaque-black affine filter 数は常に 0 である。
- [事実] crop は V2 cut に `crop(active=0,use_profile=1,left,top,right,bottom)` と
  `crop(active=1)` をこの順で attach する。非ズーム crop は、残った source 寸法に
  対応する affine `rect` を明示することで成立した。
- [事実] affine transition は `fill=1`、`distort=1`、`b_alpha=0`、
  `repeat_off=1`、`mirror_off=1`、`keyed=0`、`halign=center`、
  `valign=middle` を使う。位置・scale は typed animation `rect`、opacity/fade は
  同じ `rect.o` へ線形 keyframe として渡す。
- [事実] MLT 7.36.1 の画面内 2D 回転は `fix_rotate_z` では実画素に現れず、
  `keyed=0` の `fix_rotate_x` で成立した。中心 marker は回転後も同じ中心に残った。

## 座標 domain

- [事実] `mlt_transition_set_in_and_out()` の in/out は timeline-absolute である。
  同じ値を transition-local として与えた対照 case は実画素期待値から分離した。
- [事実] `mlt_properties_anim_set_rect()` の keyframe position は
  transition-local である。timeline-absolute key を与えた対照 case は実画素期待値から
  分離した。
- [事実] fade key は `core::clipFadeFactor(localFrame, clipDuration, ...)` から作り、
  transition-local position に設定すると V1 を露出する。MLT 側へ別の fade 式は持たせない。

## Gate

- [事実] V1 baseline、centered/scale、opacity 50%、asymmetric crop、position/rotation、
  fade in/out、非ゼロ V2 source-in、非ゼロ timeline start、前後 blank、非重複 V2 2本を
  decoded MP4画素で判定する。
- [事実] 対象画素が 0 件の検査は専用 negative test で終了コード 7 を要求し、
  `WILL_FAIL` は使わない。
- [exit] 上記の固定 graph で要求された M7b export semantics を表現できる。
  generic graph、新しい export backend、`qtblend`、`composite` は不要である。

