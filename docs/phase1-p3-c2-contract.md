# P3-C-2 display-target formal contract

本書はP3-C-1の性能・correctness条件を維持したまま、display targetの妥当性と
provenanceを追加する新しいformal contractを定義する。P3-C-1のraw、checker、summary、
判定はhistorical resultとして変更しない。

- contract version: `P3-C-2`
- raw schema: `mvm-p3-formal-2`
- matrix summary schema: `mvm-p3-matrix-summary-2`
- output directory: `build/ucrt64-release/p3-matrix-c2/`

## 性能・correctness

Playback、Seek、PauseResumeのthreshold、seed、fixture、pre-roll、測定区間、終了条件、
correctness counter、teardown条件はP3-C-1から変更しない。P3-C-2 checkerは
P3-C-1 checkerを呼び出して全条件を再検査する。

## Display-target preflight

Qt/D3D11 deviceとactual RHI targetがreadyになった後、decoder/audio pipelineをopenする前に
preflightを実行する。次をMUSTとする。

- requested output: 1920x1080
- QQuickWindow logical size: 1920x1080
- CompositorSurface logical size: 1920x1080
- actual RHI target pixel size: 1920x1080
- devicePixelRatio: 1.0（比較epsilonは1e-6）

RHI targetは`QQuickRhiItem`のactual color texture `pixelSize()`をsource of truthとする。
requested QML sizeだけではPASSにしない。orientationとavailableGeometryは診断・provenanceであり、
orientation名やavailableGeometryの大小を単独のMUSTにはしない。

targetが未初期化なら最大10秒readyを待つ。不一致ならwindowをresizeせずfail-closedし、
formal workloadへ遷移しない。可能な範囲のrawを保存してprocessをnonzeroで終了する。

## Per-process display provenance

各rawは`display_environment_start`と`display_environment_end`へ次を保存する。

- screen name / orientation
- screen geometry width / height
- available geometry width / height
- device pixel ratio
- QQuickWindow logical width / height
- CompositorSurface logical width / height
- actual RHI target pixel width / height
- native window outer/client width / height（診断値）

start/endではnative outer/clientを除く全fieldが不変でなければならない。start/endのlogical、
surface、RHI targetはそれぞれ1920x1080、DPRは1.0でなければならない。

## Matrix-level provenance

Playback、Seek、PauseResumeを各3 independent processで実行する。9 rawのdisplay signatureを比較し、
`display_environment_provenance_unchanged`へ保存する。P3-C-2 PASSは次の論理積とする。

`all_runs_pass && provenance_unchanged && hardware_provenance_unchanged &&
display_environment_provenance_unchanged`

raw producerの`formal_verdict`は従来どおり`NOT_RUN`とし、正式PASS/FAILはmatrix summaryだけが
所有する。DryRun summaryも`formal_verdict = NOT_RUN`、`p3_c_pass = false`とする。

## 実行コマンド

短縮検証:

```powershell
pwsh -NoProfile -File scripts/p3-c2-matrix.ps1 -DryRun
```

正式9 run（本ラリーでは実行しない）:

```powershell
pwsh -NoProfile -File scripts/p3-c2-matrix.ps1
```
