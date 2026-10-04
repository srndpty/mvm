# BGM の自動音量調整

メニューバーの「編集」→「自動音量調整…」から声トラックを複数、BGM トラックを 1 本指定する。
同じトラックを両方に指定することはできない。mute・solo は試聴にのみ反映し、
解析対象は指定トラックの有効な音声クリップで決める。

正規化とダッキングは独立して有効にできる。既定値は声 −16 LUFS、BGM −24 LUFS、
減衰 12 dB、検出 −35 dBFS、下降 150 ms、保持 300 ms、復帰 600 ms。
解析結果には測定値、補正量、ピーク制限、検出区間、BGM のカーブを表示する。
測定不能な無音や短い音声は正規化を適用せず、以前の補正を保持する。

「音声を試聴」は現在の再生位置から候補の音声だけを再生する。
解析・試聴では Project を変更しない。「適用」で全対象を Undo 1 回分として確定する。
キャンセルや解析失敗では一部だけを適用しない。

## 音声処理と保存

- `src/media/audio_analysis` は Qt・MLT を使わず、FFmpeg の `libavfilter` と
  `ebur128` で積分ラウドネスと true peak を測定する。必須依存が無ければ失敗する。
  ピーク制限には、小数 3 桁に丸められる metadata ではなく、公開 AVOption の
  double 値を使う（[FFmpeg の実装](https://github.com/FFmpeg/FFmpeg/blob/n8.1/libavfilter/f_ebur128.c)）。
- 既存のプレビュー用 decoder と時間軸を使用し、48 kHz ステレオを 200 ms ずつ読む。
  素材全体の PCM は保持しない。トリム、素材の開始 PTS、速度、音程保持、手動音量、
  フェード、トランジション、トラックゲインとパンを反映する。
- ゲインは sample ごとではなく frame 境界で評価し、同じ frame の PCM にまとめて掛ける。
  整数・小数 frame rate の境界は共通の時間軸で求める。RMS の検出窓は 20 ms のまま。
  最大 4 クリップを並列処理し、各 worker が独立した decoder と測定器を保持する。
  結果は元のクリップ順で取りまとめ、失敗・キャンセル時には一括適用を拒否する。
  カーブの折点は昇順に評価し、終了した発話区間を繰り返し探索しない。
  1 時間に分散した 1200 発話の下降・保持・復帰位置も回帰試験で照合する。
- 測定から以前の正規化補正とダッキングを除き、再生成による累積を防ぐ。
  補正は目標との差と −1 dBTP までの余裕の小さい方とする。
  モデルの補正範囲は −96〜60 dB。ピークまたは補正範囲で目標に届かない場合は表示する。
- 声の検出は正規化後の左右 RMS の大きい方を使う。複数トラックの区間を統合し、
  近接発話のカーブをつなぐ。発話モデルではないため、効果音や雑音にも反応する。
- ゲインは `src/project/timeline_render.cpp` で評価する。通常プレビュー、スクラブ、
  シャトルと MLT 書き出しが同じ値を使う。ダッキングは dB で線形補間する。
- schema 17 に正規化補正・ダッキングの基準値とキー・生成設定・入力 fingerprint を保存する。
  schema 16 は自動補正なしで読み込む。既存のクリップ編集処理でキーの時間軸を扱う。
  カーブはエフェクトコントロールの「ダッキング (dB)」で手直しできる。
- 入力 fingerprint の authority は、音声の解析入力だけである。timeline fps、
  対象音声 clip の素材対応、速度、トリム、手動音量、フェード、音声トランジション、
  トラックのゲインとパン、素材の path・size・100ns 更新時刻・内容 SHA-256 を含む。
  mute・solo、字幕、映像の変形、自動補正の出力は含めない。内容 hash は解析の開始時と
  完了時に worker が計算し、候補の確認中は size と更新時刻をすぐ見て、内容は非同期に
  照合する。size と更新時刻を戻しても内容が違えば適用できない。適用後の声の編集では
  カーブを維持し、ミキサーに再生成が必要なことを表示する。ダイアログに出す設定は
  Project の最後に適用した 1 件で、最初の clip に残った過去の設定ではない。
  解析の中止は worker の終了を待たない。設定の検証に失敗したときは再生を止めない。
- 内容 hash と読み取り保護の単位は clip ではなく素材 (正規化した path) である。
  長尺素材を細かく切って多数の clip で参照しても、素材 1 つにつき開始時と完了時の 2 回だけ読む。
  保存済み設定の射影は対象 track の集合ごとに 1 回だけ計算し、目標値だけ違う過去の設定を
  clip 数に比例して計算し直さない。適用の確定は clip ID の索引で引き、clip 数の二乗にしない。
- 保存済み調整の待機中は 100 ms の poll を止め、`QFileSystemWatcher` の通知で再開する。
  削除・rename で watcher が外れた素材は 5 秒ごとの size・更新時刻の確認で拾う。

−1 dBTP は測定した各クリップに対する制限であり、複数トラックの加算やその後の
手動増幅による最終ミックスのピークは保証しない。ミックス全体のリミッターは含めない。

## 検証

`audio_adjustment_contract` は一時フォルダーに fixture を生成し、独立した FFmpeg の
再測定で目標 LUFS を照合する。ピーク制限、無音、素材の開始 PTS、速度・音程保持、
トリム・分割、クロスフェード、再生成、保存、Undo/Redo、キャンセル、編集・素材変更による
失効を検査する。不正な設定・数値・キー・新 schema のフィールド欠落も拒否する。

`[事実]` 2026-10-04、同じ声素材を 60 clip に切った Project で、保護 handle は素材数の 2、
内容 hash は 2 素材 × 開始・完了の 4 回だった。素材単位にまとめる処理を外すと 61・122 回になり
試験が落ちる。目標値だけ違う設定 61 種類が残る Project で編集 1 回の射影計算は 2 回以下を要求し、
raw の設定ごとに計算する実装に戻すと 62 回で落ちる。待機中に poll を止めることと、止めたままでも
素材の書き換えを検知して再生成を案内することも同じ試験で確かめる。

true peak の oversampling 遅延は専用の測定経路へ末尾の無音を補って処理する。
ラウドネスの測定経路と使用区間にはこの無音を含めない。使用区間の最後の sample
にだけ大きなピークを置いた回帰試験と、400 ms 未満の測定不能な音声の試験を設けている。

ゲイン値の比較に加えて、非圧縮 Matroska/PCM の実書き出しとプレビュー PCM の振幅を
下降前・減衰中・復帰中・復帰後で比較する。20 dB を超える補正を使い、MLT の
volume フィルターの既定上限による縮退が無いことも確認する。
非圧縮出力は公開書き出し API の検証用指定であり、通常の UI は MP4/AAC を使う。

QML は通常サイズと狭幅・低い高さで実描画し、警告・スクロール到達性・親領域の clip を検査する。
実音声 endpoint を使う `audio_adjustment_audition` は workstation ラベルに分け、
CTest の共通音量制限と排他制御を使用する。

```powershell
pwsh scripts/build.ps1 -Target mvm_test_audio_adjustment
C:\msys64\ucrt64\bin\ctest.exe --test-dir build/ucrt64-release -R '^audio_adjustment_(contract|audition)$' --output-on-failure --timeout 120
pwsh scripts/test.ps1
pwsh scripts/lint.ps1
```

### 2026-10-04 の導入時の検証結果

- 導入時の release / debug ビルドと lint は通過。
- 導入時の関連 CTest の追試は各構成 5/5 通過。内訳は通常の契約・編集・書き出し 4 件と、
  実音声 endpoint の workstation 試験 1 件。末尾ピークの回帰試験も含む。
- 正式な `scripts/test.ps1` の初回実行は、release の通常試験 1435/1437、debug の
  ビルド依存試験 358/361。ビルド非依存の 1076 件は release だけで実行した。
  自動音量調整の QML 試験と音声チャンネル分類の試験で見つかった失敗は修正し、
  上記の関連追試で通過した。
- extended の soak 試験は各構成 100/100。performance / stability は通常試験から除外した。
- release の `transition_preview` に断続的な失敗が残る。この時点では全体試験を修正版で全件再実行した
  結果ではないため、全件成功とは扱わない。比較条件・結果と未解決事項は
  [ロードマップ](roadmap.md) に記録している。

### 入口移動・最適化後の検証

release / debug のビルドと lint は通過。正式な `scripts/test.ps1 -Fast` は
release の通常短縮試験 1434/1434、debug のビルド依存試験 356/357。
ビルド非依存の 1077 件は release で検証し、debug では重複実行していない。
performance / stability / extended は除外した。
debug の 1 件は既知の `transition_preview` の incoming 不透明度の断続的な失敗で、
全体を合格とは扱わない。自動音量調整の解析・試聴・製品メニューの実操作は両構成で通過した。
[正式スクリプトのログ](../build/audio-adjustment-optimized-tests.log) を保存している。

最後のカーブ探索変更後の関連追試は release 8/8、debug の初回は 7/8。
debug のミキサー試験は、非同期 seek の最新提示を待たずにシャトルへ進む前提で失敗した。
試験を最新フレームの提示完了と再生開始まで待つように修正し、製品メニュー・ミキサーの
実操作を release 1/1、debug の固定 3 回で 3/3 確認した。
関連試験の内訳は通常 6 件と workstation 2 件。
最後の変更後に全体試験を再実行した結果ではなく、上記の全体結果と関連追試を分けて扱う。

### 長尺音声の診断計測

1 時間の正弦波素材を声と BGM の 2 クリップに使用し、release で変更前・最適化後を
固定 3 回ずつ測定した。処理時間と声の測定値・検出区間数を生 JSON に記録し、
集計スクリプトが件数・尺・結果の一致を検査して中央値・観測最大・速度比を計算する。
変更前の計測中にはビルドと短い試験も実行しているため、診断値として扱う。
最終ビルドの計測は静的検査とも一部重なっている。
実スピーチ・圧縮音声・速度変更時の所要時間を保証する計測ではない。
結果は [最終ビルドの集計](../build/audio-adjustment-hour-final-report.md) と
[最初の最適化計測の集計](../build/audio-adjustment-hour-report.md) を参照。
最適化後の 2 組の計測でも処理時間に差があるため、特定の所要時間を保証する値には使わない。

【操作可】
この診断は desktop の表示状態を取得しないため、通常の PC 操作を続けられる。
ただし同時に重い処理を実行すると処理時間の比較に影響する。

```powershell
C:\msys64\ucrt64\bin\ffmpeg.exe -hide_banner -loglevel error -y -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=3600 -c:a pcm_s16le build/audio-adjustment-benchmark-3600.wav
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$measurementBase = 'build/audio-adjustment-hour-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
build/ucrt64-release/bin/mvm_test_audio_adjustment.exe build/ucrt64-release --benchmark C:/dev/soft/mvm/build/audio-adjustment-benchmark-3600.wav 3600 1> "$measurementBase.jsonl" 2> "$measurementBase.log"
pwsh scripts/summarize-audio-adjustment-benchmark.ps1 -Before build/audio-adjustment-hour-before.jsonl -After "$measurementBase.jsonl" -Report "$measurementBase.md" -BeforeHadOtherWork
```
