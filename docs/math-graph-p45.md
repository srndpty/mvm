# P4-5: Graph の映像書き出し

状態: **実装中・HOLD**。schema22 を維持する。過去の P3/P4 の証拠は変更しない。
コミット・push は行わない。

## 実装前に確認した接続点と計画

- 製品の `MvmController::startTimelineExport` は Project を値で捕捉し、busy にして既存 worker へ渡す。この snapshot を唯一の編集 authority とする。
- `mapTimelineExportPlan` と `timelineRenderSegments` が出力 track・区間・効果の authority。Graph の未対応拒否を、実際に出力する区間の依存計画へ置き換える。
- P4-1 の `evaluateGraphClip` に output frame を渡し、source frame と Draw index を決める。trim/split の原点を作り直さない。
- 新しい `graph_export` は現在の clip から spec を再構築し、P4-2 の `validateArtifact` と `PublicationAuthority` だけを使う。preview presentation/residency は参照しない。
- 物理契約 B を維持する。Draw を要する範囲は一つの Draw package、静止だけの範囲は検証済み Draw endpoint または独立 static package 一つを使う。
- 必要な package を encoding 前に準備し、clip ごとの source mapping と key ごとの package を分離した ledger を固定する。PNG は一枚ずつ decode・hash 検査し、const shared owner で返す。
- EquationSequence の連番 staging と MLT の通常 overlay・ClipEffects・音声・encoder・最終公開を再利用する。Manim を encoder callback から呼ばない。
- 集中依存／mapping／破損／画素／取消試験から開始し、実 Manim・製品 UI・関連回帰・変異・BuildIndependent・lint・通常 release の順で検証する。未分類の集中失敗があれば full gate を開始しない。

## 検証記録

実行結果と未達の閉鎖条件は [結果文書](math-graph-p45-results.md) に記録する。

## 現在の実装と未達条件

`prepareGraphExport` は半開区間 `[outputBegin, outputEnd)` と既存の video render segments の
交差だけを計画する。clip/track の出力規則を通し、source は P4-1 の整数・有理数 authority
から求める。clip ごとの `GraphExportFrame` と key ごとの `GraphExportPackage` は分離する。
同じ key の Draw 需要を全所有者から先に集約するため、静止所有者の順序で余分な package を
生成しない。Draw package 内の static.png は package 全体の整合検査として必須であり、
独立 static package の描画依存と区別する。

確認済み toolchain を開始時点で値として捕捉できれば、backend を起動せず既存 package を
再検証する。identity が無ければ export worker 自身が preflight する。必要な package が
無い場合だけ同じ identity の backend と P4-2 PublicationAuthority で生成する。
既存の破損 package を別 package の生成で隠さない。読み取り専用 Project は生成を許可しない。
画素や weak presentation は preview cache から取得しない。

`loadGraphExportFrame` は ledger の exact output frame を検索し、検証済み package の
static.png または exact frame-s.png だけを decode する。RGBA SHA を毎回照合し、
`shared_ptr<const Raster>` で所有する。全 Draw frame を RAM に置かない。一枚は P4-2 の
最大 64MiB、台帳は一つの clip で最大 1,000,000 output frame とし、上限拒否の負例を添えた。
可視の不正式・欠損・破損は typed failure、透明な検証済み frame は成功として区別する。

製品は従来の値コピー Project、busy、exportCancelRequested_ と exportThread の寿命を使う。
Graph の連番 staging は encoder 開始前に終え、MLT の qimage と通常の affine overlay を使う。
最下層 Graph にも黒背景の一層を残し、ClipEffects を通常 mapping で一度だけ掛ける。
音声経路は変更していないが、Graph と音声の製品受け入れは未検証である。
最終公開前に取消を再確認し、失敗した一時出力を除去する。既存の rename の置換方針は変更しない。
通常 Image が実際に生成した検証済み MP4 を、置換失敗時の byte 完全保持の対照に使う。

追加した MLT の最終 producer filter は、上流の全 layer 合成を RGBA8 で取得してから、
encoder の YUV 変換前に callback を呼ぶ。検査が失敗／不足なら export は失敗し、
最終ファイルを公開しない。callback が無い通常出力にはこの filter を接続しない。
最終 stage の alpha を書き換えず、Graph の半透明を不透明にする代用は入れていない。

**P4-5.1 で解決した阻害条件:** 黒背景と `[200,99,31,128]` の Graph endpoint の旧整数 oracle は
`[100,50,16,255]` だが、既存 MLT 合成後は `[100,49,15,254]` だった。
Graph を通さない通常 Image の同じ RGBA でも全画素で一致する。MLT の float 切り捨てと
Graph の凍結した最近接整数丸めの相違を、許容差や Graph 限定の別合成で隠していない。
P4-5.1 は artifact の整数丸めと MLT の binary32・切り捨てを別々の authority として検証し、
production の合成を保持した。数式・stage 診断・exact oracle・変異・関連回帰は
[合成契約](math-graph-p451.md) と [結果](math-graph-p451-results.md) に記録する。
P4-5 の集中阻害条件は解決したが、実 Manim・製品 UI・BuildIndependent・通常 release
による P4-5 全体の閉鎖はまだ行っていない。製品の範囲指定 export は追加しておらず、
既存の全 timeline 出力だけを Graph 対応させた段階である。
