# P3-Design: 数式 layer の合成を MLT の affine から外す設計

2026-10-12。[P1](math-sparse-raster-p1.md)・[P2-A](math-export-p2a.md)・[P2-B](math-export-p2b.md) で、
MLT 7.36.1 の書き出し経路に残る構造的なコストを測った。

- animation の付いた property を読むたびの文字列化（有効な transition の keyframe 数に依存）
- 植えた transition 1 つにつき毎 frame 0.29〜0.35 ms の固定費（graph の大きさに比例）
- transition の lock による直列化（render worker を増やしても速くならない）
- 数式 1 層ごとの全画面の合成

この文書は、数式だけを対象にした合成器の設計と実現性の検討である。**製品の経路・timeout・既存 oracle の
基準は変えていない。** 実装はしていない（下の小さな spike は検証 harness の中だけにある）。
commit・push はしていない。P1・P2-A・P2-B の成果物と判定は変更していない。

## 結論

| 案 | 判定 | 理由 |
| --- | --- | --- |
| A. 字形の範囲だけを合成する CPU 合成器（MLT の合成の位置に置く） | **PASS（次の段階へ進めてよい）** | track の順序を保てる。4 つのコストを同時に避けられる。MLT と byte 単位で一致させられる見込みがあるが、まだ 1 画素の差が残る |
| B. 既存の D3D11 / QRhi を使う GPU 合成器 | **REJECT（この目的では）** | 現在の pre-YUV と byte 単位で一致しない。readback と thread・層の制約が増える |
| C. 数式の group を RGBA の frame に前合成して MLT へ渡す | **HOLD** | 構造上、現在の pre-YUV と一致しない。採るには別の承認が要る |
| A + C（CPU 合成器で group を作り、MLT の affine で重ねる） | **HOLD** | C と同じ理由 |

推奨は A。ただし「byte 単位で一致」はまだ示せていない（spike で 7 frame 中 6 frame が全画素一致、
1 frame で 1 画素が 1 階調違う）。この差の原因を確かめるまでは、製品の挙動を変える判断をしない。

## 1. 現在の経路と、数式をまとめられる位置（Task A）

```text
MvmController::startTimelineExport
  └ TimelineExportRequest            数式 clip ID -> cache の PNG (provenance 検査済み)
exportTimeline (src/app)
  ├ mapTimelineExportPlan            clip ごとの layer 番号、区間、全 frame の位置・拡大・不透明度
  ├ staging                          数式: composeMathClipFromPng -> 全画面の透過 PNG
  └ mvm_mlt_export_two_track (src/media/mlt)
       ├ 映像 layer ごとに playlist (layer 0 の空きは黒)
       ├ 上位 layer の clip ごとに affine transition (layer 0 との間、植えた順 = 下から上)
       ├ 字幕は最上位の layer、音声は clip ごとの playlist を mix
       ├ tractor の出力に RGBA 検査 (pre-YUV。ここが画素の authority)
       └ avformat consumer             RGBA -> yuv420p -> libx264 -> MP4
```

合成は「layer 0 の画像へ、上の layer を下から順に 1 枚ずつ source-over して 8 bit へ切り捨てる」の
繰り返しである。したがって数式をまとめられるのは、**layer の順で連続している数式だけ**である。

- **数式の group**: layer の順で隣り合い、その frame で有効な clip がすべて「静止の数式」である範囲。
  間に数式以外の layer（映像・画像・文字・Graph・EquationSequence・Write・変形）が 1 枚でもあれば、
  そこで group を分ける。数式をすべて 1 枚の最上位 layer にまとめることはできない
  （間の layer との前後関係と、その layer の半透明の重なりが変わる）。
- group の合成は、その時点までの合成結果（下の layer すべて）を入力に取り、その上へ group の layer を
  下から順に重ねる。これは現在の順序と同じである。
- v4 では layer 0 が背景の画像、数式は layer 1〜41、字幕が最上位なので、group は 1 つになる。
  数式の間に別の種類の clip がある Project では group が複数になり、効果は group の数だけ小さくなる。

変えないもの:

| 項目 | 保ち方 |
| --- | --- |
| 素材 frame の対応 | 位置・拡大・不透明度は計画（`TimelineExportPlan`）の全 frame の値をそのまま使う。MLT の animation を通さない |
| track の順序と重なり | group は layer の順で連続する範囲だけ。group の中も下から順 |
| 数式の成果物と provenance | 入力は今と同じ `mathArtifacts`（cache が検査した PNG）と `composeMathClipFromPng` |
| 取消と終了 | 合成は MLT の render worker の中で同期に行う。自前の thread・queue を持たない。取消は今の進行 callback のまま |
| 音声・字幕の時刻 | 触らない。字幕は今の MLT の layer のまま |
| encoder の frame の被覆 | frame を作るのは今と同じ tractor。出力 frame の集合は変わらない |

## 2. 3 案の比較（Task B）

### A. CPU 合成器（字形の範囲だけ）を MLT の合成の位置に置く

MLT 層に自前の transition を 1 種類足す（`mlt_transition_new` で作る service。`src/media/mlt` の中だけ）。
group ごとに 1 つ植え、その `get_image` が下の合成結果（a frame の RGBA）を受け取り、group の layer を
字形の範囲だけ、下から順に直接書き込む。layer の画像と全 frame の値は、MLT の property ではなく
自前の構造体で持つ。

- 境界: 合成の計算は MLT にも Qt にも依存しない関数にする（`src/media/math`）。MLT との接点は
  `src/media/mlt` の transition だけ。group の決め方は `src/app/timeline_export.cpp` の計画。
- 既存 library の変更: MLT は変えない。`mvm_mlt_export` に「group の transition」を足す。
- メモリ: 成果物ごとに字形の範囲の RGBA を 1 枚（v4 は 39 種、平均で全画面の 0.38%）。
  出力 frame は MLT の frame をその場で書き換えるだけで、溜めない。
- CPU/GPU の転送: 無い。
- 直列になる所: group の transition は 1 つなので lock は 1 つ。ただし layer ごとの lock・文字列化・
  transition の固定費は無くなる。合成そのものは render worker の中で進む。
- thread・取消・backpressure: MLT の render worker と consumer の仕組みをそのまま使う。新しい待ちを作らない。
- 透明と重なり: 下から順に source-over。現在と同じ順序・同じ式にできる。
- 決定性: 整数と binary32 の演算だけ。thread に分ける場合も、行ごとに分ければ結果は順序に依存しない。
- 複雑さ: 中。式は小さい（下の spike）。難しいのは MLT との完全一致の証明と、拡縮する layer。

### B. GPU 合成器（D3D11 / QRhi）

- 境界: QRhi を使ってよいのは `src/app/preview` だけ、書き出しは GUI thread の外で動く。
  書き出しから使うには device の所有と thread の規則を新しく決める必要がある。
- 転送: 毎 frame、1920x1080 の RGBA（約 8 MB）を GPU から読み戻す。`gpu_preview` は CPU への readback を
  禁止事項にしている（lint の検査項目）。
- 決定性: GPU の補間と丸めは、MLT の binary32 の切り捨てと一致しない。driver と機種で変わりうる。
- 判定: 現在の pre-YUV との一致という条件と、層の規則の両方に合わない。この目的では採らない。

### C. group を RGBA の frame に前合成して MLT へ渡す

group を透明な下地の上で先に 1 枚に合成し、その frame 列を MLT の 1 つの layer として渡す。

- 境界: frame 列を MLT へ渡す手段が要る（連番の PNG を stage するか、自前の producer）。
  連番の PNG は 6840 枚の書き出しと読み込みになり、自前の producer なら A と同じく service を足す。
- 直列になる所: group ごとに全画面の affine が 1 回残る。
- 透明と重なり: **現在の結果と一致しない。** 現在は layer を 1 枚重ねるたびに 8 bit へ切り捨てる。
  先に group の中で重ねてから下地へ重ねると、切り捨ての回数と順序が変わる。半透明どうしが重なる画素で
  差が出る。P1 の「字形の範囲そのもの」と同じ種類の、小さいが 0 でない差である。
- 判定: 画素の基準を変える承認が無い限り採れない。A が MLT へ組み込めなかった場合の代案として残す。

### A + C

A の合成器で group の RGBA を作り（C）、MLT の affine で下地へ重ねる形。実装は A より MLT から離れるが、
画素は C と同じ理由で現在と一致しない。HOLD。

## 3. 画素の authority（Task C）

現在の契約（変更しない）:

- authority は tractor の出力の RGBA（encoder の YUV 変換の前）。復号した H.264 の画素は authority ではない。
- 上位 layer は、MLT の affine が下の画像へ 1 枚ずつ重ねる。素材座標は倍精度で計算して binary32 へ落とし、
  双線形補間（`interp.h` の `interpBL_b32`）と source-over を binary32 で行い、結果を 8 bit へ**切り捨てる**。
  alpha は `255 * alpha` の切り捨てで、不透明な下地でも字形の縁で 254 になる。
- 素材は straight alpha の RGBA。premultiply はしない。範囲の外の画素には触れない。
- 等倍なら素材は元の大きさのまま置く。拡縮があると、MLT は素材を矩形の大きさへ Qt（`QImage::scaled`）で
  縮拡してから置く。
- 既存の照合: 等倍・整数位置は既存の oracle（`mlt_rgba_oracle.h`）で全画素完全一致。端数位置・拡縮は
  倍精度の参照との差 1 階調以内。この基準と過去の結果は変えていない。

2 つの水準を分ける:

1. **現在の MLT の出力と byte 単位で一致する。** 製品の挙動を変えない。これを満たせば、画素についての
   新しい承認は要らない。
2. **新しく定義した独立の RGBA 合成の仕様に対して正しい。** 現在の出力とは一致しない（差は小さい）。
   これは製品の画素を変えることなので、別の承認が要る。

### spike: 独立した binary32 の合成で MLT と一致するか（Task D）

検証 harness の期待値の計算を、MLT の式を binary32 のままなぞる形に切り替えられるようにした
（`--reference-mode`。判定の基準は変えていない。差のある画素数を読むためだけに使う）。
baseline は製品の MLT の書き出し。代表 frame の全画素・4 channel を比べた。

| 計算 | 端数位置 1 層（2 frame） | 16 層（2 frame） | v4 の 20〜22 秒・25〜29 層 + 字幕（3 frame） |
| --- | --- | --- | --- |
| 倍精度の参照（既存） | 390 画素 | 2,464 | 5,133・4,434・4,528 |
| binary32、source の式の順 | 339 | 1,758 | 2,956・2,638・2,932 |
| binary32、不透明度に 1/255 を先に掛ける | **0** | **0** | **0・0・1** |

（差のある画素数。最大差はどれも 1 階調。証拠は `build/math-compositor-p3-20261012/spike/`。
`identity.txt` に harness の binary と source の SHA-256。）

[事実] 不透明度へ先に `1/255` を掛ける順（既存の oracle が整数位置について確かめた順と同じ）で計算すると、
端数位置・fade を含む 1 層、同じ位置に重なる layer を含む 16 層、v4 の実区間の 7 frame のうち 6 frame で
全画素が MLT と一致した。残る 1 frame で 1 画素だけ 1 階調違う。

- byte 単位の一致は**見込みがあるが、まだ示せていない**。1 画素の差の原因は調べていない
  [未検証]。素材座標を binary32 へ落とす所の丸めか、MLT 側の演算の順の違いが候補である。
- 比べたのは代表 frame だけである（補間の途中の frame は、値を独立に求めていないので比べていない）。
  全 frame の比較は次の段階で行う。
- 拡縮する layer は、Qt の縮拡を同じ呼び出しで再現する必要がある。この spike では試していない。
- この spike の計算は harness の検証用の実装で、速度は測っていない。

byte 単位の一致が最終的に成り立たない場合は、差の画素数・位置・最大差を記録したうえで、
「独立の仕様に対して正しい」ことを基準にするかどうかを、製品の挙動を変える前に別に承認を取る。

## 4. 性能の見通し（Task E）

実測した部品（v4、1920x1080）:

| 部品 | 値 | 出典 |
| --- | --- | --- |
| 数式なし（背景だけ）の書き出し | render 約 1.1 秒 / 合成 145 回（約 8 ms/回。encode を含む） | P1・P2-A の `background-only` |
| 現在の合成（v4 全体の graph） | 441〜849 ms/frame（前半・中盤・後半の中央値） | P2-B |
| 合成対象の画素（字形の範囲 + 余白） | 平均 128 万 / frame（現在の 2.2%）、最大 458 万 | P1 の `v4-cost-pixels.json` |
| 有効な数式 layer | 平均 27.5、最大 40 | 同上 |

[推測] A では、layer ごとの全画面の合成・文字列化・transition の固定費・layer ごとの lock が無くなり、
残るのは字形の範囲の合成と、数式なしの場合と同じ MLT の処理である。字形の範囲の合成を 1 画素あたり
20〜30 ns（補間と source-over、1 thread）と置くと、1 frame あたり 26〜38 ms、背景だけの処理 8 ms と合わせて
34〜46 ms/frame、6840 frame で約 230〜320 秒になる。行で分けて並列にすればさらに短い。

これは見積もりであり、書き出しの実測ではない。1 画素あたりの時間は測っていない。
この数字で 10 分の合否を言うことはしない。次の段階で、合成器の時間を MLT の受け渡し・encode と分けて測る。
全 frame を同時にメモリへ置く設計は含まない（1 frame ずつ、MLT の frame の上で合成する）。

## 5. 推奨する構成（Task F）

```text
src/app/timeline_export.cpp（計画と staging。所有: 書き出し 1 回分）
  ├ 数式の group を決める: layer の順で連続する静止の数式。条件に合わない clip は今の経路のまま
  ├ group の入力を作る: 成果物ごとの字形の範囲の RGBA、clip ごとの全 frame の位置・不透明度
  └ MLT 層へ渡す（POD の配列。MLT の型は出さない）
        │
src/media/math（新しい純粋な関数。MLT・Qt に依存しない）
  └ composeMathGroupFrame(下の RGBA, layer の配列, frame)   binary32 の仕様どおりに、字形の範囲だけ
        │ 呼ぶ
src/media/mlt/mvm_mlt_export.c（MLT との唯一の接点）
  ├ group ごとに自前の transition を 1 つ植える（layer の順は今のまま）
  │    get_image: 下の合成結果を RGBA で受け取り、上の関数でその場に書き込む
  ├ 数式以外の layer・字幕・音声・検査・consumer は今のまま
  └ 所有: group の入力は書き出しの関数が持ち、consumer を止めてから解放する
```

所有と寿命: group の入力（画像と値の配列）は `exportTimeline` が作り、MLT 層の関数が戻るまで生かす
（今の clip の配列と同じ規則）。transition は MLT が持ち、tractor と一緒に閉じる。合成器は状態を持たない。

### 画素と parity の契約

- 目標は水準 1（現在の MLT の出力と byte 単位で一致）。許容差は設けない。
- 比較の authority は、今の製品の全画面・分割なしの書き出しの pre-YUV。
- 比較は全 frame・全画素・4 channel。比べられなかった frame は一致に数えない。
- 対照: frame の対応の誤り、不透明度の誤り、layer の順序の誤り、frame の被覆、取消。
  どれも意図した検査で落ちることを確かめる。無関係な書き出しの失敗を検出に数えない。
- 水準 1 を満たせない場合は、そこで止めて報告する。水準 2 へ切り替えるのは別の承認の後。

### 段階（それぞれ単独で検証できる）

| 段階 | 内容 | 完了の条件 |
| --- | --- | --- |
| P3-1 | 合成の関数を `src/media/math` に作る（等倍・端数位置・不透明度）。MLT なしの単体試験 | 既存 oracle（整数位置）と全画素一致。spike の 1 画素の差の原因を特定 |
| P3-2 | harness で、全 frame について関数の出力を MLT の baseline と比べる（製品の経路は未変更） | 比較 fixture と v4 の区間の全 frame で完全一致。満たせなければ差を記録して停止 |
| P3-3 | MLT 層に group の transition を足す（既定は無効）。計画で group を決める | 既定が無効のとき既存の全試験が通る。有効にした書き出しが baseline と全 frame 完全一致。対照が検出 |
| P3-4 | 性能。合成器・MLT の受け渡し・encode を分けて測る。同じ区間で順番を回して比較 | 利益が区間によらず再現する。その後で v4 全体を製品の条件で実測 |
| P3-5 | 拡縮する layer、group が複数になる Project、取消・終了の試験 | 対象外の clip が今の経路へ正しく残る |
| P3-6 | 採用の判断（別の承認） | 上のすべての証拠 |

### リスク

| 分類 | リスク | 備え |
| --- | --- | --- |
| 寿命 | transition が group の入力を解放後に読む | 入力は書き出しの関数が持ち、consumer の stop・close の後に解放する。今の observer の復元と同じ順序 |
| deadlock | 合成の中で MLT の lock を取る・待つ | 合成器は MLT の API を呼ばない純粋な関数にする。自前の thread・queue・待ちを作らない |
| alpha | 切り捨てと alpha 254 の再現を誤る | alpha も含めた 4 channel を全画素で比べる。既存 oracle を単体試験に使う |
| 対応 | frame・layer の順・位置の取り違え | 計画の値をそのまま使う。対応を誤らせた対照が落ちることを確かめる |
| メモリ | 画像や frame を溜める | 成果物ごとに字形の範囲を 1 枚だけ。出力はその場で書く。上限を単体試験で確かめる |
| 書き出しの正しさ | 対象外の clip（拡縮・回転・Write・変形）を誤って group に入れる | 条件に合わない clip は MLT 層が拒否する（P1 の切り出しと同じ fail-closed）。group が 0 なら今の経路と同じ |
| 一致しない | byte 単位の一致が成り立たない | P3-2 で止める。水準 2 へは別の承認 |
| 決定性 | 並列化で結果が変わる | 最初は 1 thread。分けるなら行単位にし、結果が同じことを全画素で確かめる |
| 効果が小さい | 数式の間に別の layer がある Project では group が増える | group の数を計測に含める。v4 以外の fixture でも測る |
