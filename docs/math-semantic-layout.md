# 部分式の配置と動きを分ける数式編集

2026-10-11、解の公式の教材で、変えていない項まで移動する問題を見直した。
式全体の外接矩形を毎回中央揃えすると、分母や約分線の追加で他の文字も動く。
全体のクロスフェードでは、計算で何が変わったかを追いにくい。

## 採用した責務

Manim は TeX の字形を生成する。教材側が、意味を持つ部品の識別子、座標、
出現・消失、動かしてよい部品を持つ。字形の大きさから式全体を自動で再配置しない。
今回の問題は配置の所有者を明確にすれば解決できるため、Manim の再実装は行っていない。

`scripts/math_authoring/math_score.py` は配置 JSON を既存の Math clip と
位置・不透明度のキーフレームへ変換する。Python は Manim に依存しない。
Project の検証は `mvm_project_validate` が製品の `loadProjectJson` と
`validateTimeline` を呼ぶ。検証を通った候補だけを出力先へ公開する。

- 部品の `source` は一定。分子、分母、分数線、約分線を別部品にできる。
- 全段階で同じ画面座標を使う。位置変更には `moves` への明示が必要。
- `fixed` の移動、未定義部品、非有限座標、静止時間を残さない遷移は失敗する。
- 出現・消失は同じ座標で不透明度だけ変える。一定の座標にはキーフレームを作らない。
- `enter_delay_frames` で、移動が進んでから分数線・分母を出せる。
- `enter_from: {"x": ..., "y": ...}` で、新しく出す部品を開始位置から滑り込ませる。
  表示中の部品への指定は拒否し、既存の位置を不意に変えない。
- `exit_frames` を遷移の尺以下に指定すると、旧式を先に消せる。
  `enter_delay_frames` と組み合わせて、旧式と新式の文字が重ならない順序を作る。
- `exit_frames: 0` は段階の境界で瞬時に消す。打ち消し線を移動へ持ち越さない用途に使う。
- 移動の既定補間は `ease_out`。初速を速く、終端をゆっくりにする。
  `motion_interpolation` に `linear`、`ease_in`、`ease_out`、`ease_in_out` を指定できる。
  不透明度の補間は `ease_in_out` のまま保つ。
- 各部品の `scale_x`・`scale_y` は百分率で、既定値は100。分数線だけを伸縮できる。
  拡大率の変更も `moves` を必要とし、`fixed` の変更と非有限・非正の値を拒否する。
- 各段階の `highlight` に部品IDを指定すると、同じ座標・位置曲線の赤い字形を重ねる。
  強調は一時表示して黒へ戻す。強調色は `highlight_color` で指定できる。
- 同じ部品が連続して存在する間は一つの clip を使う。尺の変更でも識別子は安定する。
- 各段階の開始点と停止点を marker にする。`subtitle_cue_id` を指定すると字幕の時刻も追従する。
  字幕本文と書式は引き継ぐ。

これは専用の数式配置 GUI を完成させたものではない。配置の再編集は JSON、
生成した位置・透明度・尺の編集は既存の clip 編集で行う。
Math clip のプレビュー上の選択・変形枠には、実際に表示中のラスタの範囲を使うよう修正した。
描画準備中は最後に表示できたラスタの範囲を使い、透明になった部品はクリックを遮らない。

## 配置 JSON の最小例

```json
{
  "format": "mvm-math-score/1",
  "id": "約分の説明",
  "canvas": [1920, 1080],
  "fps": [60, 1],
  "objects": {
    "numerator": {"source": "x^2", "font_size": 96},
    "denominator": {"source": "a", "font_size": 96}
  },
  "states": [
    {
      "id": "分数", "duration_frames": 180, "transition_frames": 0,
      "parts": {"numerator": {"x": 800, "y": 400}, "denominator": {"x": 800, "y": 520}}
    },
    {
      "id": "移動", "duration_frames": 240, "transition_frames": 60,
      "moves": ["numerator"],
      "parts": {"numerator": {"x": 800, "y": 460}}
    }
  ]
}
```

座標は `canvas` 内の中心位置、期間は frame、透明度は 0〜100。
`color` の既定値は黒。背景は入力 Project の画像 clip などを引き継ぐ。
変換先は入力と別ファイルにする。

生成した clip の ID は `mvm-math-score:{配置ID}:{部分式}:{状態}` で、
強調は末尾に `:highlight` を付ける。配置 ID・部分式 ID・状態 ID にコロンは使えない。
再生成は、この ID と Project 内の所有記録 `math_score_ownership` が指す clip、
マーカー、以前に足した track だけを置き換える。空になった元の track は消さない。

- 置き換える入力の数式は `replace_clip_ids` に明示する。書かれていない利用者の数式は失敗とし、消さない。
  この配置の clip が既にある再生成では、取り込み済みで存在しない ID を欠落として失敗にしない。
- 残す数式は `preserve_clip_ids` に明示する。他の配置が所有する数式は、指定しなくても残す。
- 元の映像 track は空でも、番号・名前・mute・solo・ミキサー設定を維持する。生成 track は末尾へ足す。
- 所有者の記録が無いマーカーは消さない。配置の停止点と同じ frame なら失敗する。
- 所有記録が無く、この配置の ID を持つ数式が残っている Project は再生成しない。
  マーカーが 0 件でも同じ。以前に足した track の記録も失われているので、clip ID だけでは復元できない。
- 削除する数式に結び付いた字幕は、状態の `subtitle_cue_id` で引き取るときだけ結び付きを外す。
  それ以外は失敗する。残る clip に結び付いた字幕の時刻は変えない。

製品が Project を保存すると、未知の所有記録は書き出されない。clip ID は残る。
その状態からの再生成は、マーカーの有無によらず上の規則で失敗する。推測では移行しない。

```powershell
pwsh scripts/build.ps1 -Target mvm_project_validate
python scripts/math_authoring/math_score.py --score build/quadratic-semantic-20261011/quadratic-score.json --project build/quadratic-lesson-20261011/quadratic-lesson-white.mvm --output build/quadratic-semantic-20261011/quadratic.mvm --report build/quadratic-semantic-20261011/score-final-report.json --validator build/ucrt64-release/bin/mvm_project_validate.exe
```

配置 JSON の段階の `duration_frames` を変えて再変換すると、停止点と対応字幕を再計算する。
入力 Project 側の字幕を編集してから再変換すれば、その本文・書式を保持できる。
生成先で手編集した位置を JSON に逆輸入する機能はない。

## 約分線

共通 TeX template に `\usepackage[thicklines]{cancel}` を追加した。
静止式・Write・Transform・EquationSequence が同じ template を使う。
template version を更新して古い描画キャッシュを無効にした。
部分式の範囲検査も `\cancel`、`\bcancel`、`\xcancel`、`\cancelto` の引数を扱う。
未知のコマンドや引数不足を許す変更ではない。

[正式の cancel package](https://ctan.org/pkg/cancel) を使い、独自の細い線のマクロは置かない。
今回の教材では `\cancel{\phantom{a}}` の線を独立した部品として重ねる。
線の外接矩形が大きくても、分子・分母や隣の項の位置は変わらない。
旧 Project に独自の `\newcommand{\cancel}` があれば削除して正式 package へ移行する必要がある。
MiKTeX では `miktex.exe packages install cancel` で導入した。

## 解の公式の教材での変更と検証

既存八状態の教材は `build/quadratic-review-20261011/quadratic.mvm`。
`scripts/math-equation-sequence-export-acceptance.ps1` の実行結果
`build/quadratic-review-20261011/results.json` では失敗ゼロ、protocol 有効を確認した。
保存・再読込後の動画の八状態と変形途中を目視し、分数・根号の欠けは見られなかったが、
変形中の文字の重なりが読み取りを妨げていた。
当時の画像は `build/quadratic-visual-review-20261011/current-states.png` と
`current-details.png` に残す。

この確認時点の Project は `build/quadratic-semantic-20261011/quadratic.mvm`。
白背景・黒数式、20 段階、対応する字幕、開始・停止の marker を持つ。
元の教材 Project は残した。

- 01→02：`ax^2+` の位置を保ち、透明度だけ 50% へ変える。
- 02→03：同じ分子の部品を上へ移動し、分数線と分母を遅れてフェードインする。
- 04→05：約分線を独立表示し、式の高さを変えない。
- 05→06：`x^2` だけを移動し、打ち消した `a`・分数線・約分線をフェードアウトする。
- 12→13：既存の式を固定し、右辺に掛ける因子だけを追加する。
- 絶対値を外す段階：分母の `2` と `a` は固定し、縦棒だけを消す。

製品 QML・controller・GPU 描画を使う、入力透過・非フォーカスの検証アプリで確認する。
検証コードと各 frame の画像、座標記録は同じ build ディレクトリに残す。
画素比較は補間の実装を共有せず、実際のプレビューの固定領域を比較する。

[事実] 最新版の静止 20 段階と変化途中を含む 48 frame を描画し、目視した。
固定領域の 17 比較はすべて差分 0 画素だった。
`build/quadratic-semantic-20261011/review-cells/pixel-comparison.json` と
`states-first.png`、`states-second.png`、`numerator-motion.png`、`cancellation-motion.png` に結果を残す。
同じ製品 QML 上で数式の直接クリックによる選択も通過した。
最新版は 45 clip、字幕 20 件、marker 40 件で、保存・再読込の内容が一致した。
最終 compiler で再生成した Project も最新版と一致した。

[事実] 最初の確認は静止とシークが中心で、連続再生の先読みを検証していなかった。
利用者の再生で16秒付近に合成16枚の上限超過が発生した。
上限はGPUの固定制約ではなく製品の受け入れ設定だったため、128枚へ更新した。
数式を準備してから、製品QMLとcontrollerで冒頭から114秒の終端までシークせず再生し、
途中停止なし・先読み失敗0を確認した。
ログと再現用の検証コードは `build/quadratic-semantic-20261011/playback-review/playback.log` と
`build_playback_review.py`。入力透過・非フォーカスの固定ウィンドウで実行した。
128枚の実GPU描画、129枚の拒否を含む関連CTest 4/4とlintも通過した。
性能の実測済み範囲は更新していない。

## 追加の演出修正（2026-10-11）

新しい編集版は `build/quadratic-semantic-20261011-v2/quadratic.mvm`。
旧版は保存した。字幕の本文・書式を引き継ぎ、最後のセンタリング用に同じ本文の字幕区間を追加する。

- 冒頭の `ax^2` は不透明度を下げず、独立した打ち消し線で示す。
- 1:12の通分結果は分子・分母・分数線を分け、両方の分数線を同じ高さへ揃える。
- 1:16→1:17の右辺の整理は分子を移動し、分母は片方を移動・片方を消す。
- 平方の中身を独立した同じ部品として持ち、1:21→1:22では左辺 `x+b/(2a)` を固定する。
  括弧と指数を消し、等号を左へ移動、`±` と根号の式を滑り込ませる。
- 変わった部品を一時的に赤で強調し、静止して読む区間では黒へ戻す。
  絶対値を外す段階は、位置を保った `±` と `2a` を強調する。
- 1:46に解の公式を完成させ、1:50から全体を同じ移動量で中央へ寄せる。
  全21段階、総尺114秒。最後までの字幕区間と停止点も生成する。

[事実] 21段階と変形途中の42 frameを実製品のGPUプレビューで目視した。
平方根への切り替えで文字が重なった初回結果は `review-v2` と `quadratic-first.mvm` に残し、
消去を24 frame、出現の開始を30 frameへ調整した最終結果は `review-final-v2` に保存した。
左辺の位置記録と固定領域の5比較はすべて一致し、画素差分は0だった。
分数線・分母の高さ、移動する共通分母、完成後の中央配置も検査した。
コードは同じbuildディレクトリの `check_visual.py`、結果は `review-final-v2/verification.json`。
絶対値を外す段階の強調は追加で5 frameを描画し、`review-abs-v2` に記録する。
専用の入力透過・非フォーカスの固定ウィンドウで確認し、製品QML上で数式の直接クリックも通過した。

[事実] 配置compilerの試験25/25、正式lintが通過した。
Projectは102 clip、字幕21件、marker42件で、保存・再読込の内容が一致した。
従来のnative Math clipと位置・透明度のkeyframeを使い、製品の描画・Project形式は変更していない。
最新版と同じ内容の検証用コピーを、数式準備後に冒頭から114秒の終端まで連続再生した。
途中停止なし・先読み失敗0だった。記録は `playback-review/playback.log` と
`runtime-latest.log`、再現用のコードは `build_playback_review.py`。
同じProjectの同時編集ロックを保つため、描画確認と連続再生には別ファイルの同一内容を使った。
最新の21段階の一覧画像は `build/quadratic-semantic-20261011-v2/states-latest.png`。

## 動きと間隔の追加調整（2026-10-11、v3）

最新の編集版は `build/quadratic-semantic-20261011-v3/quadratic.mvm`。
冒頭の打ち消し線は境界で消し、位置と分数線の伸縮には製品の `ease_out` を使う。
0:36の `−c/a` は直前から座標を保ち、強調対象から外した。
平方の指数を上へ移し、展開の矢印を拡大、1:12のマイナスと分数を離した。

平方根への変形は、既存の分子・分母・分数線を同じclipとして残す。
左辺を固定したまま等号を左へ、分数を右へ少し移動して、`±` と根号を同じ位置でフェード表示する。
次の段階でも `b²−4ac` を消さずに移動し、分数線を横へ伸ばす。
1:41では分母 `2a` の間隔を通常のTeX組版に揃える。

[事実] 製品GPUプレビューの50 frameを描画して目視した。
固定領域の16比較は画素差分0。移動の中間点では移動量の75%に到達し、
一定速度ではないことを公開座標でも確認した。
分子の継続表示、分数の間隔、指数、分数線の伸縮、最終式の中央配置も検査した。
記録は `review-v3/verification.json`、再現コードは `check_visual.py`。
矢印をさらに拡大した追加の描画確認は `review-arrow` に保存した。

[事実] 数式を準備後、114秒の終端まで連続再生し、途中停止なし・先読み失敗0だった。
記録は `playback-review/playback.log`。この連続再生の後に変更したのは矢印の文字サイズだけである。
配置compilerの単体試験32/32と正式lintが通過した。
最新版は100 clip、字幕21件、marker42件で、製品の保存・再読込検証も通過した。
今回の補間と伸縮は既存の製品機能で表現できたため、C++の追加変更はない。

### 矢印の大きさの修正（v4）

最新の編集版は `build/quadratic-semantic-20261011-v4/quadratic.mvm`。
矢印の文字サイズを160から64へ縮め、縦座標を324から313へ変更した。
上下の数式の描画矩形へ侵入しない範囲へ収めるためである。
[事実] 実製品のGPUプレビューで目視し、公開された描画矩形から上下の間隔も検査した。
記録は `review-arrow/verification.json`、再現コードは `check_arrow.py`。
矢印とその強調用clip以外の98clip、字幕、停止点はv3と一致し、
製品の保存・再読込検証も通過した。v3以前の成果物は残している。

今回の通常テストは、Manim template 2 件、数式 controller と範囲検査 2 件、
Project validator の正常・不正 fixture 2 件を対象とした。全 CTest を実行したという記録ではない。
配置 compiler の単体試験は `python -m unittest discover -s tests/math_authoring -v` で実行する。
今回の対象 CTest は 6/6、配置 compiler は 17/17、正式 lint は通過した。
専用 GUI、疎なラスタの合成、書き出しの追加受け入れ試験は今後の項目として roadmap に残す。

[事実] 2026-10-11、v4 の `quadratic.mvm` を製品の `MvmController` が組む要求のまま
`exportTimeline`（MLT avformat / H.264、CRF 23）へ渡した。
数式 cache の PNG を 121 枚 stage し、libx264 は consumer 開始時の警告まで出したが、
約 46 分・CPU 時間 約 3000 秒のあいだ出力は 48 byte のまま増えなかった。
作業メモリは約 1.7 GB で横ばいだった。frame は mux されていない。
この実行は途中で止め、未完成の `quadratic-export.mp4.mvmtmp` は残していない。
同じ Project の GPU preview 14 frame は `export-parity/preview-native-*.png` にある。
preview の成功は書き出しの受け入れではない。

## 書き出しが進まなかった原因の調査（2026-10-11）

上の 46 分の実行は記録として残す。原因は deadlock ではなく、1 frame の合成が極端に遅いことだった。
証拠は `build/quadratic-export-diagnosis-20261011/` にある（修正前 `20261011-191032-before-fix`、
修正後 `*-after-fix`。各 directory の `identity.json` に source と runtime の SHA-256）。
再現は `pwsh scripts/math-score-export-diagnose.ps1`。

### 観測の方法

出力 file の大きさは進行の指標にならない。x264 は先読みの分だけ packet を出さず、白地の静止画は
packet が小さいので、file は長いあいだ増えない。代わりに次の境界の通過回数を数えた
（`MvmExportObserver`、`src/media/mlt/mvm_mlt_export.h`）。frame ごとの I/O や lock は無く、
記録は counter と、各境界の最初の 4 回・2 の冪の回の抜き取りだけである。

1. 書き出し計画の構築、2. 素材の stage 完了（`TimelineExportRequest::stageObserver`）
3. 最終 producer への frame 要求と戻り、4. encoder 直前の RGBA 検査の入口と戻り
5. encoder への投入（MLT の `consumer-frame-show`）
6. x264 が 1 frame の符号化を終えた記録、7. muxer へ渡した packet
   （6・7 は FFmpeg の debug 記録を harness が数える。7 は FFmpeg 8.1 の `write_packet_common`）
8. 完了と最終 file の公開

harness は 1 秒ごとに counter と process の CPU 時間を記録し、どの counter も 45 秒進まなければ
「停止」、上限時間まで進み続ければ「上限到達（進行中）」と分けて報告する。どちらも取消の前に
gdb で全 thread の stack を採る。

### 修正前の実測

[事実] fixture は実行前に固定した（`tests/harness/math_score_export_parity.cpp` の `kFixtures`）。
数式は v4 の Project の clip と、その cache の成果物をそのまま使う。1920x1080・60 fps・CRF 23。

| fixture | 数式の層 | frame | 結果 | 1 frame の合成 | 実行中の CPU |
| --- | --- | --- | --- | --- | --- |
| 背景のみ | 0 | 120 | 完走 1.3 秒 | 0.006 秒 | — |
| 数式 1 層 | 1 | 120 | 完走 1.5 秒 | 0.007 秒 | — |
| 数式 16 層 | 16 | 120 | 完走 32.8 秒 | 0.22 秒 | 約 4.0 core |
| 数式 48 層 | 48 | 120 | 完走 97.6 秒 | 0.65 秒 | 約 3.9 core |
| 数式 16 層 | 16 | 480 | 上限 180 秒で 325 frame | 0.57 秒 | 約 2.3 core |
| 数式 16 層 | 16 | 1920 | 上限 180 秒で 80 frame | 2.39 秒 | 約 1.4 core |
| v4 の 88〜91 秒 | 36 + 字幕 | 180 | 完走 155.4 秒 | 0.73 秒 | 約 2.8 core |
| v4 全体 | 最大 40 + 字幕 | 6840 | 上限 180 秒で 25 frame | 8.67 秒 | 約 1.1 core |

- どの fixture も counter は進み続けた。進行が止まる境界は無い。「停止」と判定した run は 0 件。
- 層の数を変えると 1 frame の時間はほぼ比例する（16 層 0.22 秒、48 層 0.65 秒）。
- 層の数を固定して尺だけを伸ばすと、1 frame の時間が伸び、使える CPU が減る
  （120 → 480 → 1920 frame で 0.22 → 0.57 → 2.39 秒、4.0 → 2.3 → 1.4 core）。
- v4 全体は 8.67 秒/frame・約 1.1 core で、46 分の実行の CPU 時間（約 1.09 core）と合う。
  この速さでは 6840 frame に約 16 時間かかる。最初の packet が mux されたのは開始 234 秒後だった。
- 尺の長い 3 つの run で採った stack では、実行中の thread は 1〜2 本で、すべて
  `affine transition → mlt_property_get_string_tf → mlt_animation_serialize_tf →
  mlt_animation_serialize_cut_tf` にいた。他の render worker は affine の中の mutex、consumer は
  `mlt_service_get_frame` の lock を待っていた（`*-stacks-*.resolved.txt`）。
  MSYS2 の MLT は debug 情報を持たないので、関数名は DLL の export 表から解決した
  （`scripts/math-score-export-resolve-stacks.py`。offset は 0x139 以下）。

### 原因

書き出し計画は上位 track の clip へ、位置・不透明度を **全 frame 分** 渡す。MLT 層はそれを
そのまま全 frame の keyframe にしていた。MLT 7.36.1 の `transition_affine.c` は毎 frame
`mlt_properties_get(properties, "rect")` を呼び、これが animation 全体を文字列へ直す
（`mlt_animation_serialize_cut_tf`。keyframe ごとに先頭から探し、`strlen`・`strcat` を繰り返すので
keyframe 数の 2 乗の手間になる）。この処理は transition の lock の中なので、render worker を
4 本にしても直列になる。v4 は 6840 frame の clip を含む 99 clip で、合計 665171 個の値を
keyframe にしていた。

### 修正

`src/media/mlt/mvm_mlt_export.c` は、前後と同じ値の keyframe を置かない。MLT の線形補間は
`y1 + (y2 - y1) * t` で、両端が同じ値ならどの frame でも同じ値を返す。値が変わる区間は
全 frame を残すので、どの frame で評価しても結果は変わらない。v4 は 665171 個の値に対して
keyframe が 19632 個になった。製品の合成・描画・検査の経路は変えていない。

[事実] 修正の前後で、pre-YUV の RGBA は比較できた全 frame で一致した
（frame ごとの digest。完走した 6 fixture の全 frame と、未完走だった 3 fixture の共通 430 frame）。

| fixture | 修正前 | 修正後 |
| --- | --- | --- |
| 数式 16 層・120 frame | 32.8 秒 | 19.6 秒 |
| 数式 48 層・120 frame | 97.6 秒 | 81.8 秒 |
| 数式 16 層・480 frame | 上限で未完了（0.57 秒/frame） | 完走 104.7 秒（0.21 秒/frame） |
| 数式 16 層・1920 frame | 2.39 秒/frame | 0.22 秒/frame（上限 240 秒で 1106 frame） |
| v4 の 88〜91 秒 | 155.4 秒 | 65.2 秒 |
| v4 全体 | 8.67 秒/frame、MLT の構築 13.2 秒 | 0.26 秒/frame、構築 0.75 秒（上限 240 秒で 919 frame） |

回帰は `mlt_export_capacity_overflow` に足した（60 個の値が 24 個の keyframe になること）。
間引きを外すと 60 個になって落ちることを確かめた。

### 残っている制約

[事実] 尺への依存は消えたが、層の数に比例するコストは残る。数式 1 層は全画面（1920x1080）の
透過 PNG で、MLT の affine は字形が占める範囲によらず全画素を合成する。修正後も 48 層で
0.55 秒/frame、v4 の 36 層の区間で 0.30 秒/frame かかる。

[事実] 製品の書き出しは開始から 10 分（`TimelineExportRequest::timeoutMs` の既定値）で、進行中でも
失敗にする。v4 全体は修正後も 10 分では終わらない。

### 書き出しの照合（parity）

`mvm_math_score_export_parity` は fixture ごとに次を要求する。どれかが欠ければ不成立で終わる。

- 出力 frame の集合が事前に決めた尺と一致する（製品の検証・書き出し結果・pre-YUV・復号の 4 つ）。
- pre-YUV は全 frame を 1 回以上観測する。同じ frame の再要求や順不同の呼び出しは許すが、
  再要求で画素が変わったら不成立にする。
- 代表 frame を、MLT の合成結果を使わずに求めた期待値と比べる。期待値は 2 段ある。
  - 全 layer が等倍・整数位置なら、既存の MLT 7.36.1 oracle（`mlt_rgba_oracle.h`）で
    全画素・4 channel の完全一致を要求する。
  - 端数位置・拡縮を含むなら、MLT の双線形の式を倍精度で独立に計算した参照と比べ、差は
    1 階調まで許す（MLT は binary32 で計算して切り捨てる）。これは完全一致ではない。
  - 補間の途中・fade の途中・回転・crop を含む frame は期待値を独立に求めない。比較しなかった
    ことを記録し、一致として数えない。比較できた代表 frame が 0 件、または数式の画素を含む
    比較が 0 件なら不成立にする。
- 復号した H.264 は codec・frame 数・各 frame の寸法・時刻（0 始まりの等間隔）を見る。
  圧縮後の画素を pre-YUV と同じとは仮定しない。
- 失敗・取消では、最終 file も一時 file も残っていないことを見る。

[事実] v4 の数式は Project の座標が端数（例: 中心から -227.759 画素）なので、等倍・整数位置の
frame は実 Project には無い。既存の oracle がそのまま当てはまるのは、数式を画面中央へ置き直した
対照 fixture（`math-1-integer`）だけで、これは 3 frame とも全画素が完全一致した（数式の画素 1244）。
実 Project の配置はすべて 2 段目（参照との差 1 階調以内）で比べている。

[事実] pre-YUV の alpha は全画素 255 ではない。字形の縁の半透明の画素で 254 になる
（MLT の `255 * alpha` の切り捨て。oracle も同じ値を出す）。以前の harness の「代表 frame の alpha が
すべて 255」という条件は MLT の合成では成り立たないので、代表 frame は alpha を含む 4 channel を
期待値と比べ、全 frame では最小値が 254 以上であることを見る。観測した最小値は、数式を重ねた全 fixture で 254（背景のみは 255）。

[事実] 修正後、v4 全体（6840 frame）を最後まで書き出して照合した。ただし製品の timeout（10 分）
では終わらないので、この 1 回だけ timeout を 90 分へ上書きした（製品の条件ではない。report の
`request_timeout_is_product` が false）。所要 4157 秒（同じ PC で build・lint・CTest を並行して
いたので、速度の測定値としては使わない）。

- pre-YUV は 6840 frame すべてを観測。欠落 0、範囲外 0、再要求 1 frame（画素は同じ）、alpha の最小 254。
- 復号した H.264 は 6840 frame、1920x1080、時刻は 0 始まりの等間隔。
- 代表 3 frame のうち 600 と 6720 は参照との差 1 階調以内（27 層・25 層、数式と字幕の画素
  29160・49299、差のある画素 6180・10309、許容を超える画素 0）。5370 は補間の途中なので比較していない。

記録は `build/quadratic-export-diagnosis-20261011/20261011-195317-after-fix-v4-full-no-product-timeout`。

[事実] negative control は 4 種とも期待どおりに落ちた（`math-1`）: pre-YUV の観測を 1 frame 落とすと
被覆の不成立、期待値を 1 画素ずらすと代表 frame の不一致、検査が 1 frame を拒否すると書き出しの失敗、
途中の取消は取消。後の 2 つでは最終 file も一時 file も残らなかった。
