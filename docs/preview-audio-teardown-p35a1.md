# P3-5A.1: preview の audio の後始末の解放後の参照の修正

2026-10-13。[P3-5A](math-export-p35a.md) の全 CTest で `transition_preview` が SegFault した原因
（`PreviewEngine::removeSource` の解放後の参照）を直し、退行を捕まえる試験を足した。

数式 group の合成・MLT の書き出し・書き出しの timeout・画素の authority は変えていない。MLT の color producer
にも触れていない。commit・push はしていない。P3-5A の全 CTest の結果（1513/1514）は、履歴としてそのまま
である。`math_equation_sequence_product_ui` の SegFault は UNKNOWN のままである。

証拠は `build/preview-audio-teardown-p35a1-20261013/`。

## 判定

| 項目 | 結果 |
| --- | --- |
| 解放後の参照の説明 | **できた。** 2 つの道があった（1.3）。どちらも debugger の下で毎回再現する |
| 修正 | sink が clock を共有して持つ（sink が生きている間は clock が解放されない）。`removeSource` は破棄の順も直した |
| 修正の前の失敗の保存 | 修正の前の binary で、同じ条件で記録した（2） |
| 回帰の試験 | debug heap の下で動かす試験を 6 つ足した。修正を外すと、意図した試験が同じ場所で落ちる（4.2） |
| 修正の後の cohort | **成立。** 9 つの試験を debug heap の下で 52 回、通常の起動で 27 回。signal による停止は 0 回 |
| 全 CTest・lint | **成立。** release の全 CTest 1521/1521 通過。lint 通過 |

## 1. 所有と後始末の順（Task A）

### 1.1 誰が何を持つか

| もの | 持ち主 | ほかを指す形 |
| --- | --- | --- |
| `AudioMasterClock` | `PreviewEngine::Impl::audioClock`（`shared_ptr`）。shutdown では `detachedWorkers.audioClock` へ移る | 何も指さない（mutex と数値だけ） |
| `AudioDecodeWorker` | `Impl::audioWorker`（`shared_ptr`）、mix 入力は `extraAudioSources[..].worker` | clock も sink も指さない。自分の queue を持つ |
| `WasapiAudioSink` | `Impl::audioSink`（`shared_ptr`） | worker の queue を生の pointer で指す。**clock を参照（`AudioMasterClock&`）で持っていた** |

- sink の destructor は `stop()` を呼ぶ。`stop()` は、endpoint を止め、**`clock_.stop()` を呼び**、render の
  thread を join し、device を手放す。queue には触れない。
- したがって、**sink は clock より先に壊れなければならない。** worker は sink より後でよい（sink の thread は
  `stop()` で join 済みで、destructor は queue を触らない）。契約の順は sink → worker → clock
  （`preview-engine-contract.md` §12）。

### 1.2 sink の参照を局所に写す場所

`Impl::audioSink` を局所の `shared_ptr` に写す場所は 5 つある。engine の lock を持ったまま audio を止めない
（`audioTransportMutex` との取得順を固定する）ために、lock の中で写し、lock の外で使う形になっている。

| 場所 | thread | clock を手放す処理との関係 |
| --- | --- | --- |
| `removeSource`（主の audio を外す） | control | **同じ関数の中で `impl_->audioClock.reset()` を行う。局所の sink が残る（道 1）** |
| `removeSource`（止められずに fatal） | control | 関数の中にいる間に、shutdown の thread と render の thread が後始末を最後まで進めうる（道 2） |
| `play`・`pause`・`seek` | control | 自分では clock を手放さない。関数の中にいる間に fatal の後始末が最後まで進めば、道 2 と同じ形になりうる [推測。再現はしていない] |
| shutdown の thread | shutdown | 後始末を render へ依頼する前に、局所の参照を手放している（以前から対処済み） |

### 1.3 解放後の参照になる 2 つの道

**道 1: 停止中に主の audio source を外す（毎回起きていた）**

1. phase 1（lock の中）: `audioSink = impl_->audioSink` と写す。
2. phase 2（lock の外）: `audioSink->pause()`・`audioSink->stop()`（thread を join）、`audioWorker->stop()`。
3. phase 3（lock の中）: `impl_->audioSink.reset()` — 局所の参照があるので、sink は壊れない。
   `impl_->audioWorker.reset()`、`impl_->audioClock.reset()` — **clock が解放される。**
4. 関数の終わり: 局所の `audioSink` が壊れる → `~WasapiAudioSink` → `stop()` → `clock_.stop()` →
   **解放済みの clock の mutex を lock する。**

controller は、seek のたびに audio source の集合を差し替える（`applyAudioSourceFor` → `removeSource`）。
`transition_preview` は最初の seek でこの道を通る。

**道 2: removal が fatal になり、関数の中にいる間に後始末が最後まで進む**

1. `removeSource` が局所に sink を写す。audio を止められず、fatal として shutdown を始める。
2. `removeSource` が戻る前に、shutdown の thread が worker を止め、render の thread が
   `detachedWorkers` を解放する（sink → worker → clock の順に `reset`。sink は局所の参照があるので壊れない。
   clock は解放される）。
3. `removeSource` が戻るときに局所の sink が壊れ、解放済みの clock を触る。

既存の試験 `preview_engine_p5e_remove_fatal_event_order` は、この順を barrier で決定的に作っている
（event の順を確かめるための試験）。通常の起動では通るが、debug heap の下では毎回落ちた。

どちらも、通常の起動では落ちにくい。解放した直後の memory はまだ読めて、mutex として使えてしまうことが多い。
`Impl` の comment にも「この UAF は crash しないため、test でも捕まえられない」とあった。

## 2. 修正の前の失敗（Task B）

`20261013-064906-before/`。修正の前の source（git の HEAD `0c2fd23`、作業 tree の変更なし）で作った binary を、
debugger の下（debug heap）と通常の起動で動かした。`identity.json` に source・binary・gdb の SHA-256 がある。

| 試験 | debug heap（3 回） | 通常（2 回） |
| --- | --- | --- |
| `p5e-remove-audio-source`（停止中に主の audio を外す） | **3 回とも SIGSEGV** | 2 回とも成功 |
| `transition-preview`（controller 経由） | **3 回とも SIGSEGV** | 2 回とも成功 |
| `p5e-hot-swap`・`p5e-remove-shutdown-race`・`p5e-remove-audio-stop`・`p5e-product`・`p5d-product` | 3 回とも成功 | 2 回とも成功 |

- 例外: access violation（gdb では SIGSEGV）。register に `0xFEEEFEEE…`（debug heap が解放した memory を
  埋める値）がある。
- 止まった場所: `AudioMasterClock::stop()`（`audio_clock.cpp:141`）← `WasapiAudioSink::stop`
  （`wasapi_audio_sink.cpp:654`）← `~WasapiAudioSink`（101 行）← `PreviewEngine::removeSource` の終わり。
- P3-5A の記録（procdump の下で 12 回中 12 回、dump 2 つ）は
  `build/math-export-p35a-20261013/transition-preview-cohort/` にそのまま残してある。
- `20261013-064211-before/` は、CTest が付ける環境変数（音量の倍率・固定 window）を付けずに実行した
  最初の run で、`transition-preview` の通常の起動が「試験の音量を下げていない」という前提の失敗になった
  （crash ではない）。`NOTE.txt` を付けて残し、同じ条件で取り直した上の run を基準にした。
  この run では、試験の音が倍率なしで鳴った。
- 道 2（`--remove-fatal-event-order`）は、この時点の cohort に入れていなかった。道 1 だけを直した binary で
  見つけた（`probe-fatal-paths/`、3 回とも SIGSEGV）。修正の前の binary では確かめていないが、
  道 1 の修正はこの道に関わらない [推測]。

## 3. 修正（Task C）

変えた製品の code は 3 か所である。

1. **`WasapiAudioSink` が clock を共有して持てるようにした**（`wasapi_audio_sink.h`・`.cpp`）。
   `shared_ptr<AudioMasterClock>` を受け取る constructor を足し、sink の member として持つ。
   sink が生きている間は clock が解放されないので、どの持ち主が最後に sink を手放しても、destructor が
   触る clock は生きている。参照を受け取る従来の constructor は残した（局所変数どうしで寿命が決まる
   使い方のため。検証用の app と単体試験が使っている）。
2. **`PreviewEngine` が、その constructor で sink を作る**（`preview_engine.cpp` の `addSource`、1 行）。
3. **`removeSource` で、clock を手放す前に局所の sink の参照を手放す**（`preview_engine.cpp`）。
   1・2 だけでも解放後の参照にはならないが、破棄の順（sink → worker → clock）を契約どおりに保つ。

確かめたこと:

- audio の worker と sink の thread は、依存する資源を壊す前に止まって join している（phase 2 と、
  shutdown の thread。変えていない）。
- sink は、自分が参照する clock より先に、または clock を保ったまま壊れる。
- 局所の持ち主が sink を clock の解放より後まで延ばしても、解放済みの clock は触らない（道 2 の試験）。
- 同じ source を 2 回外すことはできない（2 回目は `InvalidSource`）。足し直して外すことをくり返しても、
  shutdown まで正常に進む（新しい scenario）。
- sink の destructor が呼ぶのは clock だけで、engine や controller へ戻る callback は無い。

変えなかったこと: `play`・`pause`・`seek` の局所の参照、shutdown の thread、`DetachedWorkers` の破棄の順、
worker と queue の持ち方。preview のほかの部分は触っていない。

## 4. 回帰の試験（Task D）

### 4.1 足したもの

- **`scripts/run-under-debug-heap.ps1`**: 実行 file を gdb の下で起動し、signal で止まったら失敗にする。
  debugger の下で起動した process は debug heap を使うので、解放後に読んだ値を pointer として使う誤りが、
  決まった場所で落ちる。UCRT64 の toolchain に AddressSanitizer が無いための代わりである。
  **検出できるのはこの形だけで、解放後の memory を読むだけ・書くだけの誤りは見つからない。**
- **`mvm_p5e_preview_smoke --remove-audio-readd`**: 主の audio source を外す → もう一度外そうとして拒否される →
  足し直す → 外す、を 3 回くり返してから shutdown する。
- **CTest**（どれも `workstation`）:

| 試験 | 内容 |
| --- | --- |
| `preview_engine_p5e_remove_audio_readd` | 上の scenario（通常の起動） |
| `preview_audio_teardown_debug_heap_remove_paused` | 停止中に主の audio を外す（道 1） |
| `preview_audio_teardown_debug_heap_remove_readd` | 外す・足し直すのくり返しと shutdown |
| `preview_audio_teardown_debug_heap_hot_swap` | 再生中に video・audio を外し、差し替えた audio も外す |
| `preview_audio_teardown_debug_heap_shutdown_race` | audio を止めている間に shutdown が入る |
| `preview_audio_teardown_debug_heap_fatal_event_order` | removal が fatal になり、関数の中にいる間に後始末が最後まで進む（道 2） |
| `preview_audio_teardown_debug_heap_transition_preview` | controller 経由。seek のたびに audio source の集合を差し替える |

- `transition_preview` は実時間の再生を検査するので、debugger の下では遅くなって自分の検査に落ちることが
  ある（修正の後の最初の cohort で 12 回中 9 回。signal は 0 回。`20261013-070431-after/`）。
  その検査は通常の `transition_preview` が受け持つので、debug heap の試験では signal だけを見る
  （`-SignalOnly`）。ほかの 5 つは、終了コードも見る。
- sleep を伸ばしたり、試験の順を変えたりはしていない。

### 4.2 対照（修正を外すと落ちること）

| 変異 | debug heap の試験の結果 | 証拠 |
| --- | --- | --- |
| `removeSource` の順の修正を外す（sink は clock を参照で持つ。元の code と同じ動き） | `remove_paused`・`remove_readd`・`transition_preview` が SIGSEGV。`hot_swap`・`shutdown_race` は成功。**通常の起動の同じ試験は 3 つとも成功**（通常の起動では捕まえられないことの確認） | `mutant-without-fix/` |
| 順の修正は残し、clock の共有だけを外す | `fatal_event_order` だけが SIGSEGV。ほかの 5 つは成功 | `mutant-reference-clock-order-fix-only/` |

どちらも、止まった場所は `AudioMasterClock::stop()` ← `~WasapiAudioSink` ← `PreviewEngine::removeSource` である。
変異は最終の build の前に行い、source は戻してある。

### 4.3 修正の後の cohort

`20261013-080150-after/`（最終の binary。`identity.json` に SHA-256）。

| 試験 | debug heap | 通常 |
| --- | --- | --- |
| `p5e-remove-audio-source`（道 1） | 5 回とも成功 | 3 回とも成功 |
| `p5e-remove-audio-readd`（くり返し） | 5 回とも成功 | 3 回とも成功 |
| `p5e-remove-fatal-event-order`（道 2） | 5 回とも成功 | 3 回とも成功 |
| `p5e-hot-swap`（再生中の削除と差し替え） | 5 回とも成功 | 3 回とも成功 |
| `p5e-remove-shutdown-race` | 5 回とも成功 | 3 回とも成功 |
| `p5e-remove-audio-stop`（fatal） | 5 回とも成功 | 3 回とも成功 |
| `p5e-product`・`p5d-product` | 各 5 回とも成功 | 各 3 回とも成功 |
| `transition-preview` | **12 回とも signal なし**（うち 3 回は終了コード 0、9 回は実時間の検査で 0 以外） | 3 回とも成功 |

- debug heap の下の `transition-preview` は、修正の前は 3 回中 3 回（P3-5A では 12 回中 12 回）、最初の seek で
  落ちていた。修正の後は 12 回とも最後まで走る。0 以外の終了コードは、debugger の下で再生が遅くなることに
  よる実時間の検査の失敗で、memory の誤りではない（4.1）。通常の起動では 3 回とも全部の検査が通る。
- `20261013-070431-after/` は、道 1 だけを直した binary の cohort（`transition-preview` の signal は 0 回。
  終了コードも見る判定だったので「不成立 9」と記録されている）。`20261013-075552-after/` は、script の誤りで
  途中で止まった run（`NOTE.txt`）。どちらも残してある。
- dump を採る必要のある失敗は無かった。

## 5. 回帰の gate と provenance（Task E）

| gate | 結果 |
| --- | --- |
| lint | 通過 |
| release の全 CTest（`pwsh scripts/test.ps1 -Preset ucrt64-release`、並列 8） | **1521/1521 通過**（`ctest-full.log`、1,149 秒）。P3-5A の 1514 件に、今回の 7 件が加わった |
| preview・audio の試験 | 全 CTest の中で実行。`transition_preview`、`preview_engine_p5c`〜`p5e`、今回の 7 件を含めて通過 |
| debug の全 CTest | 実行していない |

- 全 CTest は最終の binary で実行した。その後は build していない（cohort の script の誤りを 1 行直しただけ）。
- **P3-5A の全 CTest の結果（1513/1514、`transition_preview` が SegFault）は、履歴としてそのままである。**
- 全 CTest は 1 回だけである。`transition_preview` の通常の起動は、cohort と合わせて修正の後に 4 回実行し、
  4 回とも通った。修正の前は、たまにしか落ちなかった（P3-5A: 全 CTest で 1 回、単独 5 回中は crash 0 回）ので、
  通常の起動の回数で「直った」ことは示せない。示しているのは、debug heap の下の結果と対照である。

source・binary（修正の前 → 後。SHA-256 の先頭 16 桁。全体は各 run の `identity.json`）:

| file | 修正の前 | 修正の後 |
| --- | --- | --- |
| `src/preview_engine/preview_engine.cpp` | `1DFEE4541AC5BC5D…` | `72586D214DAA37A5…` |
| `wasapi_audio_sink.cpp` | `69F8DC9E20C38C48…` | `B12D715EA2607592…` |
| `wasapi_audio_sink.h` | `A437A8899AA2010B…` | `3725837AE4335117…` |
| `apps/p5e_preview_smoke/main.cpp` | `AB73437C9599FFE0…` | `9D20335649221ECA…` |
| `mvm_p5e_preview_smoke.exe` | `2F30B98C637FC0D4…` | `9D84118537AE1DD1…` |
| `mvm_p5d_preview_smoke.exe` | `9452981C33F5B6CA…` | `CB7B7C6BC2B01A10…` |
| `mvm_test_transition_preview.exe` | `2537A0B530C69181…` | `5B4691B9A76F8961…` |
| `gdb.exe` | `E17F7D6E68F22996…` | `E17F7D6E68F22996…` |

git の HEAD は `0c2fd23` で、今回の変更は commit していない（差分は各 run の `git-diff.patch`）。
runtime（MSYS2 UCRT64 の Qt・FFmpeg・gdb）は、修正の前後で変えていない。

## 6. 残っていること

- debug heap の試験は、解放後に読んだ値を pointer として使う誤りしか検出しない。AddressSanitizer の代わりには
  ならない。
- `play`・`pause`・`seek` の局所の sink の参照は、そのままである。clock の共有によって解放後の参照には
  ならないが、fatal の後始末と重なったとき、sink の破棄が control の thread で関数の終わりに起きる。
  この順を作る試験は足していない [未検証]。
- sink は worker の queue を生の pointer で指している。sink の destructor は queue を触らないので今は
  問題にならないが、所有の形としては clock と同じ弱さがある（変えていない）。
- `math_equation_sequence_product_ui` の SegFault は UNKNOWN のままである。P3-5A で、同じ方法（gdb の下で
  起動）で 1 回動かしたが落ちなかった。今回の原因と同じだという証拠は無い。
- MLT の color producer の data race（P3-5A の 3.3）は未修正である。
