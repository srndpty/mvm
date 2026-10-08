# P4-3.1: Graph 連続再生と閉鎖 gate

2026-10-09。P4-3.1 と P4-3 は PASS/CLOSED。P4-4 は GO だが未着手。
P4-4 の実装、コミット、push は行っていない。

## frame authority と所有

Graph の animation は一枚の画像を capture せず、clip・FPS・可視尺と presentation slot を保持する。
GUI thread が resident の不変一覧を atomic に公開する。render thread は P4-1 の
`evaluateGraphClip` で各 output frame の source index を決め、その index の画像だけを選ぶ。
端点の index は -1。欠損は `FrameMissing`、区間外は `OutsideClip`、不正 mapping は
`InvalidMapping` と透明にする。以前の frame や静止端点を Draw の代わりにしない。

一覧は weak owner を持ち、RGBA budget の退避を妨げない。画素をコピーする間は shared owner が
保持し、GPU はコピー済みの texture を既存の retirement 契約で所有する。GUI cache の container、
renderer、PNG decoder を render thread から呼ばない。session/key の失効は旧 slot も空にする。
安定した Graph/video の composition は revision・mapping の層集合・source identity・opacity が
同じ間は再利用する。resident の変更だけでは animation instance を交換しない。

## ミキサーの因果比較

pre-P4-3 の `7220820` を `git archive` で独立 directory へ展開した。
`math-p431-mixer-ab.ps1` が両 arm の fixture、mixer、Qt DLL、QML plugin の hash、環境、
正確な引数、作業 directory と display-power lease を記録する。
QML fixture は製品 controller をロードしない。Windows style では両 arm が同じ
`ScrollView.qml` / `DefaultScrollBar.qml` の `visible` loop で失敗し、Basic style では両 arm が通る。
これは P4-3 より前の mixer でも存在する layout cycle の現在 runtime による独立比較である。

循環は `visible → effectiveScrollBarWidth/Height → padding → availableWidth/Height → size → visible`。
内容の高さも `availableHeight` を参照していた。mixer の right/bottom padding を固定して
viewport を棒の可視性から独立させ、棒は内容へ重ねる。Windows style の回帰も通常 CTest に追加した。
`failOnWarning` は維持し、警告の例外・抑制は追加しない。循環を戻す変異は同じ警告で失敗する。

元の CTest では PATH 末尾の backslash が list separator を escape し、Basic の設定が
独立した ENVIRONMENT 項目にならない構成が観測された。Basic の意図は専用の
`ENVIRONMENT_MODIFICATION` でも明示する。cycle 修正は Windows regression でも必要である。
この比較は過去の failed gate を PASS や環境干渉に読み替えるものではない。

## 字幕と公開

Graph cache 通知の対象は既存 preview mapping の現在 frame にある Graph と、実際の
submitted composition にある animation に限定する。disabled・非出力 track・区間外の Graph は
初期 seek を増やさない。mapping にあっても式が不正で composition に寄与しない Graph も検査する。
初期 seek request が厳密に一回で、後続の無関係な通知でも増えないことを確かめる。
Graph-free の元の native 試験と、通知を無条件に戻す negative control を保持する。

Windows の公開 rename の retry 条件と mutex 境界は変更しない。300ms の共有 lock を保持する
回帰に加え、公開済み package の繰り返し要求が manifest と pixel hash を変えず renderer も
呼ばないことを検査する。実際の共有 lock error の後、mutex の外にある retry 境界で同期し、
lock を解除する前に取消・supersede・shutdown が完了すること、競合する別の有効 package が
先に公開されても retry で置換しないことを確かめる。retry する error の集合は変更していない。

## 証拠と判定

【操作可】通常 gate は背面・非フォーカス・入力透過の window と既存 display-power lease を使う。
新規の `build/math-p431-*` に source snapshot、正確な command と結果を保存する。
固定 composition の native clock 試験は、独立した区別可能な画素で Draw N=3、非単調順、trim、split、
異なる FPS を全画素比較し、case 内の composition revision と seek 0 も記録する。
late residency の前は下地だけ、到着後はその output frame の exact 画素を比較する。
実 Manim の製品 controller 再生でも、Draw から端点まで composition の交換と再構築が無いことを検査する。

P4-3 の BuildIndependent 1079/1080 と Release 1477/1479 の directory・分類・結果文書は保持する。
過去文書の「mixer QML を変更していないので Graph と無関係」という説明だけでは因果証拠に
ならない。上の A/B と layout の変異を今回の説明の根拠とする。

## 閉鎖

集中 cache・連続 frame・公開の gate は `math-p431-20261009-Focused-04`、実 Manim と
native 連続受け入れは `math-p431-20261009-Real-02`、P3・字幕・汎用 preview・QML は
`math-p431-20261009-Regressions-01`、変異は `math-p431-20261009-Mutations-01` を正とする。
BuildIndependent は `math-p431-20261009-BuildIndependent-01`、最終 lint は
`math-p431-20261009-Lint-03`、通常 release 一回は `math-p431-20261009-Release-01`。
すべて終了コード 0、変異は検出後の復元と再検査も通過した。
件数・画素比較・seek 回数・初期失敗を含む集計は [機械集計](math-graph-p431-results.md) を正とする。

```text
P4-3.1 PASS/CLOSED
P4-3   PASS/CLOSED
P4-4   GO
```

P4-4 は未着手。historical FAIL は保持し、コミット・push は行っていない。
