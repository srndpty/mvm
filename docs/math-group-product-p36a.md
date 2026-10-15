# P3-6A: 数式 group の書き出しを、製品の入口から選べるようにする

2026-10-14。これまで診断用の入力でしか動かなかった数式 group の graph を、製品の書き出しの入口
（`MvmController` → `mvm::app::exportTimeline`）から、明示の選択で使えるようにした。**既定は off** で、
設定の UI は足していない。

数式 group の合成の式、下地の alpha の条件、MLT の affine、V1 の背景の保護、書き出しの timeout
（600000 ms、実時間）、画素の authority は変えていない。commit・push はしていない。これまでの証拠と判定は
変更していない（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま）。v4 全体（6840 frame）の
書き出しは、この phase では行っていない。

この phase で確かめたのは、選択と統合の正しさである。製品としての最終の検収ではない。

証拠は `build/math-group-product-p36a-20261014/`。再現は `pwsh scripts/math-group-product-p36a-verify.ps1`。

## 判定

| 項目 | 結果 |
| --- | --- |
| 製品の入口から数式 group の graph を選べる | 成立。`MvmController` の内部の選択 → 要求の `mathGroupOptimization = Auto` |
| 既定（off）の動きは変わらない | 成立。判定そのものをしない。従来の graph で、出力の MP4 は同じ |
| 判定が正しい graph を選ぶ | 成立。ELIGIBLE → 数式 group、INELIGIBLE・UNKNOWN → 書き出しの前に従来の graph（22/22 通りが期待どおり） |
| timeout・取消・公開の契約 | 成立。どちらの graph でも同じ道を通る（5/5 通りが期待どおり） |
| encoder 直前の RGBA | 成立。全 frame が従来の経路と一致（2,160 frame） |
| 対照 | 成立。実行時の拒否は書き出しの失敗、取り違えた画像は画素の不一致として検出 |
| 全 CTest・lint | **PASS: 1523/1523**（3 回目。実行の前後とも GUI の環境は VALID）。1・2 回目は 1 件ずつ落ちた（7 の経過）。lint 通過 |

**全体: PASS。** 既定は off のまま。次は P3-6B。

## 1. 製品の呼び出しの道（Task A）

| 段 | 場所 | すること |
| --- | --- | --- |
| 1 | `MvmController::exportTimeline(QUrl)`（`apps/mvm/mvm_controller_export.cpp`） | 再生を止め、`TimelineExportRequest` を作る（出力の大きさ・fps・CRF・字幕・worker 4 本・数式の成果物・進行と取消の callback）。書き出しの thread を起こす |
| 2 | `mvm::app::exportTimeline`（`src/app/timeline_export.cpp`） | 計画（clip の順、track、区間）、stage（数式・文字・画像を PNG にする）、数式 group を使うなら準備済みの画像を作る |
| 3 | 同 | `MvmExportClip` の配列と `MvmExportSpec`（timeout、worker、callback）を作る |
| 4 | `mvm_mlt_export_two_track`（`src/media/mlt/mvm_mlt_export.c`） | 数式 group の割り当てと **判定**、V1 の背景（保護つき）、graph の構築、consumer、待ち（取消・失敗・timeout）、close、出力の検証 |
| 5 | `mvm::app::exportTimeline` | 一時 file（`.mvmtmp`）へ書いたものを、成功したときだけ最終の名前へ rename。失敗・取消では一時 file を消す |
| 6 | `MvmController::finishTimelineExport` | 結果を UI の状態へ反映する |

検証 harness（`mvm_math_score_export_parity`）は、製品の `MvmController` に書き出しをさせ、5 の関数を呼ぶ所
（controller の `ExportRunner`）へ割り込んで、要求へ診断用のものを足していた。

| harness が足していたもの | 製品の要求に元からあるか | 今回 |
| --- | --- | --- |
| encoder 直前の RGBA の検査（`encoderFrameValidator`） | ある（要求の項目） | そのまま |
| worker の数・timeout の上書き | ある（要求の項目） | そのまま |
| 数式 group の mode（`observer->diagnostic_math_group`）・分け方・画像の上限 | **無かった**（診断用の observer の入力） | **要求の項目 `mathGroupOptimization` を足した**。書き出しの関数が、mode 3（判定つき）・分け方 1・範囲だけの画像・上限 64 MB に固定して observer へ入れる |
| 故障の注入、証拠の差し替え、下地の観測、以前の timeout の数え方 | 無い（診断用の observer の入力） | 足していない。observer を渡したときだけ使える |
| 数式 group の結果（判定・graph・数） | 無かった（observer の出力を harness が読んでいた） | **結果の項目 `mathGroup` を足した** |

stage・clip の順・成果物の出どころ・取消・timeout・公開は、どちらの graph でも同じ関数の同じ道を通る。
書き出しの実装は複製していない。

## 2. 選択（Task B）

- `TimelineExportRequest::mathGroupOptimization`（`Off` / `Auto`）。**既定は `Off`。**
- `MvmController::setMathGroupExportOptimization(bool)`。既定は false。C++ の関数で、QML からは呼べない
  （property でも invokable でもない）。設定の UI は無い。
- `Off`: 数式 group の入力を MLT の層へ渡さない。判定もしない。従来の graph で書き出す（これまでと同じ code の道）。
- `Auto`: 書き出しの関数が、判定つきの mode を指定する。MLT の層が、graph を作る前に判定する
  （P3-3D・P3-4 の判定をそのまま使う）。
  - ELIGIBLE → 数式 group の graph。
  - INELIGIBLE・UNKNOWN → 従来の graph（書き出しを始める前に決まる）。
- 判定の材料は、V1 の画素の alpha の証明（stage した静止画の全画素の走査）と、clip の配置だけである。
  Project の名前、clip の数、fixture かどうかは見ていない。
- 記録: 書き出しの結果（`TimelineExportResult::mathGroup`）に、要求された選択、判定と理由、選んだ graph、
  group の数、まとめた clip の数、従来の経路に残した clip の数、残った affine の数、実際に合成した回数、
  失敗の数、準備済みの画像の数と大きさ、graph を閉じた後に残っていた transition・callback の数が入る。
  成功・失敗・取消のどれでも入る。controller は、最後の書き出しの記録を持つ
  （`lastExportMathGroupReport()`）。
- 故障の注入と以前の timeout の数え方は、診断用の observer の入力である。製品の要求（controller が作る要求）は
  observer を渡さない。`Auto` のとき、書き出しの関数は observer を自分で作るが、全部 0 で始まり、上の 4 つ
  （mode・分け方・画像の持ち方・上限）しか書かない。[事実] 試験した全部の書き出しで、controller が作った
  要求に observer は付いていなかった。

## 3. 製品の契約（Task C）

どちらの graph でも同じである（code の道が同じ）。

| 契約 | 確かめ方 |
| --- | --- |
| timeout 600000 ms（consumer の待ち、実時間） | 値は controller の要求のまま。timeout を 300 ms に上書きした書き出し（選択 on）は timeout で失敗 |
| 取消と終わり方の分類 | 選択 on の途中の取消は、取消で終わる |
| 出力の path と一時 file | 失敗・取消・timeout で、最終 file も一時 file も残らない |
| 検証を通ってからの公開 | 成功した書き出しだけ、最終 file がある |
| V1 の背景の cache の保護 | 変えていない（P3-5A.2）。隙間のある fixture を選択 on で書き出し、全 frame 一致 |
| stage した成果物と、準備済みの画像の所有 | 画像は書き出しの関数の中の置き場が持ち、関数から戻るまで生きる |
| consumer・transition の後始末 | 書き出しの後、transition・callback・画像の残りは 0 |

## 4. 実行時の失敗（Task D）

- UNKNOWN・INELIGIBLE: 最初から従来の graph。数式 group は 1 つも使わない。
- ELIGIBLE で、数式 group の合成が実行時に失敗した場合（下地に alpha 0 があった、など）:
  **書き出しを失敗にする。** 一時 file を消し、error の文と記録（`mathGroup.errors`）を残す。
  **従来の graph でやり直さない**（この phase では入れない。P3-6C で決める）。

製品にとっての意味: **`Auto` にすると、従来の graph なら成功していた書き出しが失敗することがある。**
起きるのは、判定の前提が外れたときである。

- 判定は、V1 の stage した静止画の alpha を全画素見ているが、「MLT がその画素を同じ alpha で渡す」
  「下の layer を重ねても alpha は 0 にならない」は前提である（P3-3D）。
- 準備済みの画像が上限（64 MB）を超えた場合も、書き出しは失敗する（stage の段階で、MLT を始める前）。
  v4 では 2.81 MB である。
  - **追記（2026-10-14、P3-6A.1）:** この動きは、判定が UNKNOWN・INELIGIBLE になる Project まで
    失敗にする互換性の欠陥だった。P3-6B の監査で見つけ、`Auto` では準備を取りやめて従来の graph で
    書き出すように直した（[P3-6B の結果](math-group-product-p36b.md) の 1）。上限（64 MB）は変えていない。
- 既定（`Off`）では、どちらも起きない。

## 5. 製品の入口からの試験（Task E）

`scripts/math-group-product-p36a-verify.ps1`、`20261014-033518-final/`。期待は実行の前に script に書いてある。

どの場合も、製品の `MvmController` が書き出しの要求を作り、`mvm::app::exportTimeline` が書き出す。
candidate は controller の内部の選択を切り替えて書き出し、同じ process で先に書き出した従来の経路
（選択 off）と、encoder 直前の RGBA を全 frame 比べる。「observer あり」と書いていない場合は、observer を
渡していない（製品の要求と同じ）。

| 場合 | 判定（理由） | 選んだ graph | まとめた clip・残った affine | 合成した回数 | 結果 |
| --- | --- | --- | --- | --- | --- |
| 選択 off（16 層） | 判定しない | 従来 | 0・16 | 0 | 全 120 frame 一致 |
| 選択 off（v4 の全体 graph、区間 1200。observer あり） | 判定しない | 従来 | 0・120 | 0 | 全 60 frame 一致 |
| 16 層 | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | 全 120 frame 一致 |
| 48 層 | ELIGIBLE（`base-alpha-proven`） | 数式 group | 48・0 | 145 | 全 120 frame 一致 |
| v4 の 20〜22 秒（移動と不透明度の補間） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 28・2 | 145 | 全 120 frame 一致 |
| 数式 16 層のうち拡縮・回転・crop が 1 層ずつ（従来の affine と混ざる） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 13・3 | 580 | 全 120 frame 一致 |
| v4 の 88〜91 秒（拡縮する層が従来の affine に残る） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 34・4 | 410 | 全 180 frame 一致 |
| V1 が不透明な静止画 | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | 全 120 frame 一致 |
| V1 が alpha 1 の静止画 | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | 全 120 frame 一致 |
| V1 に clip が無い（隙間だけ） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | 全 120 frame 一致 |
| V1 の clip が前半だけ | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | 全 120 frame 一致 |
| V1 に alpha 0 がある | INELIGIBLE（`base-alpha-zero`） | 従来 | 0・16 | 0 | 全 120 frame 一致 |
| 数式の層が全部回転（対象の clip が無い） | INELIGIBLE（`no-eligible-math-clip`） | 従来 | 0・16 | 0 | 全 120 frame 一致 |
| V1 に effect（110% に拡大） | UNKNOWN（`base-clip-transformed`） | 従来 | 0・16 | 0 | 全 120 frame 一致 |
| alpha の証拠が無い（observer あり） | UNKNOWN（`base-alpha-unproven`） | 従来 | 0・16 | 0 | 全 120 frame 一致 |
| alpha の証拠が範囲外（observer あり） | UNKNOWN（`base-alpha-unproven`） | 従来 | 0・16 | 0 | 全 120 frame 一致 |
| alpha 0 の V1 に「不透明」という誤った証拠（observer あり） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 0 | **書き出しが失敗**（合成器が拒否。出力なし） |
| 実行時に下地を拒否（故障の注入。observer あり） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 0 | **書き出しが失敗**（合成器が拒否。出力なし） |
| 別の clip の画像を使う（故障の注入。observer あり） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | **画素の不一致を検出**（0/120 frame だけ一致） |
| 同じ process で 3 回続けて | ELIGIBLE（`base-alpha-proven`） | 数式 group | 16・0 | 145 | 全 120 frame 一致 |
| v4 の全体 graph、区間 1200（observer あり） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 97・23 | 170 | 全 60 frame 一致 |
| v4 の全体 graph、区間 5280（observer あり） | ELIGIBLE（`base-alpha-proven`） | 数式 group | 97・23 | 170 | 全 60 frame 一致 |

- 数式 group を選んだ場合は、「選んだ」という記録だけでなく、**実際に合成した回数が 0 でないこと**と、
  affine の数が従来より減っていることを確かめている。
- 従来の graph を選んだ場合は、group が 0、合成が 0 で、affine の数が従来と同じであることを確かめている。
- 「別の clip の画像を使う」は、準備済みの画像の取り違え（成果物の同一性の食い違い）を作る対照である。
  画素の比較が検出する。成果物の出どころと内容による共有の検査そのものは、単体試験（P3-3D）にある。
- V1 に動画を置いた書き出しは、試していない（v4 の Project に動画が無い）。V1 の動的な effect は試した
  （UNKNOWN → 従来）[V1 の動画は未検証]。

選択 off と on の出力（observer なしの製品の要求どうし）:

| 選択 off | 選択 on | MP4 の SHA-256 |
| --- | --- | --- |
| `off-layers-16` | `on-eligible-layers-16` | 同じ（`dd0aa3dc20a2d790…`） |
| `off-v4-window-1200` | `on-v4-window-1200` | 同じ（`dbdab1627abb2f2b…`） |

主の書き出し（選択 on。harness の観測用の observer が付く。16 層の fixture）:

| 場合 | 結果 | 最終 file | 一時 file | 判定 → graph | 残った transition・callback・画像 |
| --- | --- | --- | --- | --- | --- |
| 選択 on、最後まで | PASS | あり | 無い | ELIGIBLE → 数式 group | 0・0・0 |
| 選択 on、途中で取消 | CANCELLED（書き出しをキャンセルしました） | 無い | 無い | ELIGIBLE → 数式 group | 0・0・0 |
| 選択 on、timeout 300 ms | EXPORT_FAILED（tractor consumerがtimeoutしました） | 無い | 無い | ELIGIBLE → 数式 group | 0・0・0 |
| 選択 on、実行時に下地を拒否（故障の注入） | EXPORT_FAILED（数式 group: 合成器が拒否しました: 数式の合成先の alpha の最小値） | 無い | 無い | ELIGIBLE → 数式 group | 0・0・0 |
| 選択 off、最後まで | PASS | あり | 無い | 判定しない → 従来 | 0・0・0 |

## 6. 所有と観測（Task F）

| もの | 持ち主と寿命 |
| --- | --- |
| 準備済みの数式の画像 | `exportTimeline` の中の置き場（`PreparedMathRasterPool`）。MLT の関数から戻った後、`exportTimeline` から戻るときに解放する。MLT の関数は、戻る前に consumer と tractor を閉じる |
| 数式 group の transition | MLT の関数の中で作り、tractor と frame が参照を持つ。tractor を閉じ、frame が無くなると解放される。MLT の関数は、最後に「残りが 0」でなければ失敗を返す |
| MLT の frame と callback | consumer を閉じた後は callback が走らない（P3-5A の検出器つきの harness で確かめた範囲） |
| observer（`Auto` のときに書き出しの関数が作るもの） | `exportTimeline` の中の `unique_ptr`。MLT の関数より長く生きる。MLT の層は、呼び出しの間だけ pointer を持つ |
| 一時 file | `exportTimeline`。失敗・取消で消す。成功で rename する（変えていない） |
| 早い return（stage の失敗、画像の上限の超過など） | MLT の関数を呼ぶ前に戻る。置き場と observer は、関数から戻るときに解放される |

- `Auto` のとき、MLT の層は observer があるので、通過の回数の記録（frame の要求、consumer の event の
  listener）も行う。`Off` のときは行わない。画素と要求の形式には関わらない（RGBA の検査が無ければ、
  形式を変えない）。時間への影響は測っていない [未検証]。
- 書き出しの後に残りが 0 であることは、成功・失敗・取消・timeout の全部で確かめた。**これは解放漏れが
  無いことの確認で、race が無いことの証明ではない。** `mvm_mlt_export.c` そのものは、今も race の検出器の
  下で動かしていない（P3-5A）。今回、数式 group の transition と背景の module の source は変えていないので、
  検出器つきの harness は実行し直していない。

## 7. 回帰の gate と provenance（Task G）

| gate | 結果 |
| --- | --- |
| 製品の入口の試験（candidate 22 通り、MP4 の比較 2 組、主の書き出し 5 通り） | 22/22、2/2、5/5 が期待どおり |
| `m4_timeline_export_focused_tractor` に足した検査（既定は判定しない。数式の clip が無い Project の `Auto` は INELIGIBLE で、出力の file が既定と byte 単位で同じ） | 通過 |
| lint | 通過 |
| **release の全 CTest**（`pwsh scripts/test.ps1 -Preset ucrt64-release`） | **1523/1523 通過**（3 回目。1142.28 sec。`ctest-final/`）。GUI の環境は 前: VALID、後: VALID |
| debug の全 CTest | 実行していない |
| v4 全体（6840 frame）の書き出し | 行っていない（この phase の対象外） |

全 CTest と統合試験の経過（前の結果は上書きせず、全部残してある）:

| 回 | 全 CTest | 落ちた試験 | 調べたこと |
| --- | --- | --- | --- |
| 1（`ctest-run-1-one-failure/`） | 1522/1523。環境は前後とも VALID | `graph_render_artifact`（「共有 lock が実際の rename retry を起こす」、5.3 秒） | file の lock の競合を時間で作る試験。binary は 2026-10-10 のもので、今回の変更は入っていない。単独では 5 回中 5 回通る（約 1.5 秒） |
| 2（`ctest-run-2-one-failure/`） | 1522/1523。環境は前後とも VALID | `math_equation_sequence_product_ui`（「PROTOCOL_INVALID: 試験を操作から隔離できません: 試験の window が利用者の前面 window より手前にあります」、135 秒） | 検査は 453 件中 0 件失敗。利用者が PC を操作していた時間帯で、試験が自分で「操作と干渉した」と判定した。**P3-3A から UNKNOWN のままの SegFault とは別の形である**（crash ではない）。環境を確かめる script は、利用者の操作との干渉を見ていない |
| 3（`ctest-final/`） | **1523/1523**。環境は前後とも VALID | なし | — |

- 1・2 回目に落ちた試験は、どちらも 3 回目では通った。落ちた理由は今回の変更と結び付かないが、原因を
  切り分けたわけではない。**全 CTest は、負荷や利用者の操作によって 1 件落ちることがある**（P3-5A.2 の
  closure でも 1 回あった）。検査を弱めたり、sleep を伸ばしたりはしていない。
- 統合試験の 1 回目（`20261014-022305-verify-run-1-script-error/`、不成立 4）は、私の script の誤りである。
  v4 の全体 graph の区間を指定した 3 つを、observer なしで書き出した。区間の指定は observer の診断用の
  入力なので、**区間が無視され、Project 全体（6840 frame）を書き出した。** 比較は区間の 60 frame だけを
  対象にしていたので、余分な frame があるとして COMPARE_INVALID になった。
  - 意図した全長の書き出しではなく、検収でもない（全 frame の比較をしていない）。この phase では全長の
    検収を行わない、という指示に反して実行してしまったものである。
  - 起きたことの記録: 選択 off の全長は、611.7 秒で timeout した（600000 ms、実時間）。選択 on の全長は
    2 回とも完走した（305.4 秒・297.3 秒。区間の 60 frame は一致）。出力の MP4 の SHA-256 は
    `ff8eb4d5d80b3021…` で、P3-4・P3-5A の診断用の全長の出力と同じ値である。
  - script を直し（区間を指定する場合は observer を付ける）、やり直した。上の表は、やり直した run である。

最終の file（SHA-256 の先頭 16 桁。全体は `20261014-033518-final/identity.json` と `ctest-final/identity-*.json`）:

| file | SHA-256 |
| --- | --- |
| `src/app/timeline_export.cpp` | `DFAFF7F86E80F508…` |
| `apps/mvm/mvm_controller_export.cpp` | `5E58F1FFFA20BC3F…` |
| `mvm_math_score_export_parity.exe` | `68374FFFA98AC84B…` |
| `mvm.exe`（統合試験の時点） | `902B7592565C2E69…` |
| `libmlt-7.dll` | `13D5EFA4536D3225…` |

git の HEAD は `9793395` で、今回の変更は commit していない（差分は `ctest-final/git-diff.patch`）。
最終の build の後は、source を書き換えていない。

## 8. 残っていること

1. 製品としての検収（v4 全体を、製品の入口から選択 on で書き出す）。この phase では行っていない。
2. 既定は off のままである。on にする入口は、controller の C++ の関数だけで、UI も設定の保存も無い。
3. ELIGIBLE で実行時に失敗したとき、従来の graph でやり直すかどうか（P3-6C）。今は失敗になる。
4. V1 に動画がある Project は UNKNOWN になり、従来の経路のまま。V1 に動画を置いた書き出しは試していない。
5. `mvm_mlt_export.c` そのものを race の検出器の下で動かす手段が無い。
6. `Auto` のときの観測の負荷（observer）の時間への影響は測っていない。
7. 全 CTest の中で、負荷によって落ちることのある試験がある（7 の経過）。UI の SegFault は UNKNOWN。

次は P3-6B を勧める。
