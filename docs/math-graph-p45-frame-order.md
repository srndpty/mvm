# P4-5 Real の frame 1 保持を調査する

## 契約と判断

成功した製品 export は、timeline の整数 frame domain 全体について source mapping、
encoder 色変換前の RGBA、実 MP4 の frame 数と有理 timestamp を満たす必要がある。
validator の最初の呼出が frame 0 であるという契約は設けていない。
[P4-5](math-graph-p45.md)、[RGBA audit と独立 oracle](math-graph-p451.md) は維持する。

MLT v7.36.1 の [mlt_consumer.c](https://github.com/mltframework/mlt/blob/v7.36.1/src/framework/mlt_consumer.c)
では、`consumer_worker_thread` が queue mutex 内で未処理 frame を選び、mutex を
解放した後に `mlt_frame_get_image` を呼ぶ。複数 worker の画像取得・検査の実行順は
queue の順序を保証しない。負の `real_time` の `worker_get_frame` は queue 先頭の
`rendered` を待ってから先頭を取り出す。
[consumer_avformat.c](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/avformat/consumer_avformat.c)
はその frame を encoder へ渡す。検査 callback の順序と出力順序は別の境界である。
取得した公式 source は調査 directory に保存し、runtime DLL の SHA も記録する。

[事実] 固定負荷の観測で、callback の並行実行、同じ frame の反復、frame 6→4→5 の
検査順を確認した。成功 export の異なる frame ID は要求 domain を満たし、実 MP4 の
frame 数・全 timestamp の検査も通過した。呼出回数だけでは coverage を証明できない。

[事実] frame 0 の検査を frame 1 の検査完了まで通知で待たせた対照では、旧試験が
元の FAIL と同じ前提・総合停止 assertion で失敗した。保持 frame 1 の全画素一致、
取消、join、stale 通知不在は成立した。成功 export 側の全 domain 検査も通過した。
この対照の frame 0 は producer から渡され、RGBA validator に到達している。
callback への進入は frame 0 が先で、画素検査と barrier の取得は frame 1 が先だった。
「最初に保持した frame」を「最初に要求／呼出された frame」の authority にしてはならない。

[事実] 旧 harness は保持対象を mutex 内で記録した後、mutex を解放してから
`barrierReached` を公開していた。その間に別 callback が対象を上書きできる。
修正では選出・記録・release 公開を同じ mutex 区間に置き、公開後に mutex を解放する。
保持条件は要求 domain 内の実 frame、独立 oracle の全画素一致、preflight、worker thread
である。shutdown の取消を完了待ち loop が観測すること、join、queued 完了の配送と
stale 通知不在は従来どおり要求する。runner が先に終了した場合は barrier 待機を終え、
encoder 未開始を前提 assertion で拒否する。

成功 export の検査は別に維持し、期待値の map と比較済み ID の双方が
`[0, timelineEndFrame)` の各 frame を含むことを明示的に要求する。
画素の許容差・順序の許容時間・任意の sleep は追加しない。

## 観測・再現条件

証拠の入口は `build/p45-frame-order-investigation/`。
`historical-provenance.json` は元の Real FAIL/PASS と既存レビュー証拠の全ファイルを
SHA-256 で記録する。過去の directory は変更しない。
元取得時の実行 exe は保存されていないため、現在の exe の hash を元取得時の hash と
同一だとは主張しない。元の source snapshot、コマンド、既存 runtime 記録は保持する。

観測 cohort は `observed/cohort.json` で事前に固定した CPU 負荷二本、MLT render worker
四本、Real 三回。結果を条件に回数を追加していない。`observed/runs.json` に全結果を残す。
MLT 観測は producer の filter に到達した frame、画像取得開始、validator の進入・戻りを
QPC・thread ID とともに記録した。これは全 seek API の記録ではなく、最終 producer が
consumer に渡す frame の観測である。試験側は validator と barrier の順序、要求 domain
を JSON に記録する。観測自体が scheduling に与える影響は排除していない。
domain は worker に渡された Project の `timelineEndFrame` から取得する。この入口と
製品 plan の `totalDurationFrames` は、ともに同じ `validateTimeline` の `totalFrames`
を使うことを source で確認した。検査用に別の尺の算式は作っていない。

`forced-old/` は通知による順序対照を旧 assertion で一回実行した FAIL。
製品の RGBA 観測用の一時変更は、その source snapshot と
`mvm_mlt_export.observed.c` に保存し、元の byte 列に復元した。
`observation-restored.json` に復元 SHA を残す。恒久的な製品 source・公開 API・CMake
の変更はない。

修正後の対照は `controls/cohort.json` で事前に列挙し、各一回だけ取得する。
`MVM_P45_FRAME1_FIRST` は条件変数で検査順を固定する試験専用入口、
`MVM_P45_ACTIVE_IMAGE` は最後の active shutdown の背景だけを元の既知 RGB PNG に
換える対照、`MVM_P45_NO_ENCODER` は最後の runner が encoder を開始せず戻る負例である。
表示電源 lease、固定 window、背面・入力透過、音量 scale を既存 Real gate から使う。
操作は可能。ビルドは公式 script を sandbox 外で直列実行する。

[事実] 最初の静止背景対照は、clip の種別とパスだけを換え、素材台帳に元の Video が
残っていたため、保存 assertion が失敗した。`validateMediaReferences` は素材と clip の
種別・パス一致を要求する。この取得は `controls/frame1-first-no-ffv1/` に FAIL として
保持し、有効な受入証拠に数えない。素材台帳の種別・パスと静止画の技術値を合わせ、
`amended-controls/cohort.json` で静止背景・通常 Real を各一回と宣言して検証する。
原因が判明した fixture の修正後検証であり、同じ source の成功までの再試行ではない。

## 原因について残る制限

元の初回 FAIL には MLT 要求・callback 進入・所有者選出の時系列がない。
その run の frame 0 がいつ要求／検査されたか、自然な worker 順序変動と harness の
公開前競合のどちらで frame 1 が保持されたかは、後続の PASS からは確定できない。
元の run の厳密な発生経路は E（未解決）として保持する。

一方、B の順序保証がないことと C の harness authority／公開区間の問題は、source と
対照に基づき修正する。観測した成功 export に A の欠落・mapping 不一致はない。
これは元の取消 run に production 欠落が絶対になかったという証明ではない。
FFV1 preview エラーと export 順序差の因果は別に扱い、再実行の成功で解決済みにしない。

[事実] 修正した静止背景 fixture の対照では、preview の FFV1 エラーがない状態で
frame 1 の保持・全画素一致・取消・join が成立した。FFV1 エラーは、制御した frame 1
保持の必要条件ではない。背景 producer も動画から画像に変わる対照なので、元の自然な
順序差への preview の寄与まで排除したものではない。

変異の事前 span 検査は、新規 `missing-output-frame` の文字列に `mlt_position` cast が
なく対象ゼロとして一度失敗した。`preflight-failed.json` に保持し、実 source に合わせた。
修正後の全定義は一箇所ずつに一致する。変異は compilation と対象 assertion の実行で
判定し、span 検査だけを動的な検出実績には数えない。

検証の最終結果と negative control の記録は、証拠 JSON から生成した
[最終検証結果](math-graph-p45-frame-order-results.md) を参照する。
コミット・push は行わない。
