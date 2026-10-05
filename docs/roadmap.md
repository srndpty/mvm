# 改善のロードマップ

「このアプリの今後の改善策は何か」に答えるための一覧。着手したら該当の文書 (例: `docs/subtitles.md`) へ移し、ここからは消す。記述の印は `docs/phase0-findings.md` と同じ (`[事実]` `[推測]` `[未検証]`)。

## 一般の音声・書き出し

- [事実] P2-6 の通常 release gate で `ownership_soak_100` が音声 consumer の
  60000 ms timeout により 100/130 回で終了した。通常試験は 1456 件中 1455 件通過。
  再現コマンドは `pwsh scripts/test.ps1 -Preset ucrt64-release`、証拠は
  `build/math-p26-release-alpha-20261006.log`。原因と今回の変更との因果関係は未特定。
  音声 soak は別途調査する。成功するまでの再試行による選別はしていない。
- [事実] P0-6.1 の同条件対照で、Math を含まない通常 Text と通常画像の音声付き MP4 も
  AAC の `Input contains (near) NaN/+-Inf`、frame 2〜4 の encode error、
  `tractor出力を検証できません` を再現した。Math Clip を必要条件としない既存の不具合。
  同じ 48 kHz stereo WAV、3 秒 / 180 frame、60 fps、1920x1080、同じ export 設定と
  出力 path の規則を使っている。根本原因は未特定で、一般の export/audio として修正する。
  再現: 実 Manim smoke の対照。証拠は `build/math-p061-attribution-20261004-203505.log` と
  同名 directory の `audio-text.mvm` / `audio-image.mvm` / `acceptance.mvm`。
  Math Clip P0 の完了 gate には音声出力の成功を含めない。過去の P0-6 の失敗は保持する。

## 数式 clip

- 数式間のクロスディゾルブは P1+ へ延期する。静止数式 P0 の範囲は既存の fade と
  ClipEffects。既存の generic still-layer 契約も dissolve を許可しておらず、
  Video / Manim の全画面・不透明な layer に限定している。P0 の機能として追加しない。
- [事実] 文字サイズの変更中は、次の描画が済むまで last-good を前の大きさで表示する。
  計画の「前の mask を拡大縮小して即座に見せる」は未実装。必要性を P0.5 で判断する。
- [事実] 配置は画像・文字と同じ空き track の規則を使い、V1 が空なら数式も V1 に置く。
  計画の「常に V2 を既定にする」とは異なる。専用規則を足すかは別の改善として判断する。
- [事実] 再試行は成功した disk cache を使い続ける。fingerprint に含めない TeX package の
  更新で glyph が変わる場合に、artifact を明示的に無効化する操作が必要かを P0.5 で検討する。
- backend の無い機械での artifact 利用と、session をまたぐ last-good の対応付けは P0 の対象外。
  必要なら provenance と利用者の明示的な判断を使う方式を別途検討する。
- P2: 式から式への変形。設計の結論と P2-1 (Project / timeline、schema 20) は
  `docs/math-clips.md` の「式から式への変形 (P2)」にある。残りは次の段階で行う。
  - (済: P2-2) mvm の分け方と n 番目の出現の照合、変形の key・端点の配置・色の補間の中立な契約
  - (済: P2-4) 変形の artifact (`mvm-math-transform-artifact/1`、切り出した A8) と cache、
    Write と共有する memory の上限
  - P2-4 の変形の frame は圧縮しない A8 で disk に置く (1 frame = artifact の幅 x 高さ byte)。
    長い変形で disk が大きくなる場合は、可逆の圧縮 (PNG の encode など) を検討する
  - (済: P2-3) Manim の backend (`MathTex(*segments)` と明示の変形・fade、部分の構造の検査、
    実測の bbox による artifact の矩形、半画素の補正、端点の照合)
  - (済: P2-5) preview (A の layer と B の layer が同じ変形の frame を見せる、artifact の位置の規則) と
    エフェクトコントロールの状態 (disk の描画と preview の memory を分けて示す)
  - 変形の mask の先読み: 今は前・後ろの clip が見える frame の合成が要求してから memory に読むので、
    再生中に読み終えるまでは cut で見せる (読み終えた次の tick から、その時刻の変形の frame を見せる)
- P2 の後: 手動の照合・部分式の ID・強調、背景の矩形の補間、区間の中で ClipEffects が
  違う変形、変形と後ろの clip の Write の両立、トランジションごとのコピー / 貼り付け。
- P2 の最適化: Write の書き出しは Write の区間の timeline frame ごとに出力全面の PNG を
  合成・encode・stage する (尺と解像度に比例)。mask の矩形だけを stage し、配置を MLT の
  affine に任せれば減らせる。preview と同じ画素の契約 (`composeMathPatch`) を保つこと。
- P2: 再生中に次の Write の clip の mask を先に読む (prefetch)。今は合成が要求してから読むので、
  clip に入ってから読み終えるまでの数 frame は書き終えた式を見せる (読み終えた次の tick から、
  その時刻の Write の frame を見せる。`docs/math-clips.md` の P1.2)。
- [未検証] Write の preview の再生中の負荷。patch の着色と送信は render thread で行い、
  大きな式 (1080p 全面) では 1 frame あたり約 8 MB になる。重ければ patch を前もって作る、
  または GPU で着色する。Write の後の Unwrite・途中から書く・速さの曲線の指定は未対応。

## 自動字幕 (文字起こし) の精度

2026-10-03 時点の方針: 認識モデルは whisper.cpp + `ggml-large-v3.bin` (Vulkan、ビーム探索) のまま保留する。モデルの変更だけでは意味の誤り (「衆参」→「中3」) を解消しきれないため、下の順で進める。比較の実測値は `docs/subtitles.md` の「large-v3 以外のモデルの比較」にある。

### 1. 利用者の実素材で比較する (他の項目の前提)

[未検証] 公開データ (放送・読み上げ) の結果は、会話・固有名詞の多い長尺の実素材での順位を保証しない。利用者が修正済みの字幕 (SRT) と元の音声を用意し、モデルごとの文字誤り率と誤りの種類 (意味の取り違え／表記の揺れ／区切り) を比べる。評価環境は `C:\ai\asr-eval` (リポジトリ外、`evaluate.py`・`longform.py`)。採用を決めるなら、評価スクリプトを `scripts/` へ移して再現できるようにする。

### 2. 認識エンジンを Parakeet-ja へ切り替える

- 候補: NVIDIA `parakeet-tdt_ctc-0.6b-ja` (CC-BY-4.0、表示義務あり)。[事実] 読み上げ音声で Whisper large-v3 の 7.8% に対し 6.4%、Whisper の誤りの多くが意味の取り違えだったのに対し、差の多くは表記の揺れだった。
- 実行系: CrispASR (C++/ggml、C の ABI、Vulkan) を版を固定して UCRT64 で構築する。[事実] v0.8.41 の `libcrispasr.dll` と `ggml-vulkan.dll` は UCRT64 で構築できた (付属の試験の実行ファイル 1 つが miniaudio の未解決で link に失敗するので、examples と tests は構築しない)。
- 制約: CrispASR は whisper.cpp と別版の `ggml.dll` を同梱するため、1 つのプロセスに両方を読み込めない。whisper.cpp を外して CrispASR へ一本化する (CrispASR は Whisper のモデルも動かせるので、large-v3 を選択肢として残せる)。
- CrispASR は更新がほぼ毎日ある。版の固定と、`scripts/build-whisper.ps1` と同じ取得・検査の手順を用意する。

### 3. 単語の時刻から字幕を区切り直す

[事実] Parakeet-ja は短い間で句点を落とし、2 文を 1 区間にまとめる (長尺の試験で区間の文字数 中央値 36・最大 71)。認識結果の単語の時刻から、無音の長さ・句読点・最大文字数 (画面 2 行程度) で区切り直す処理をアプリ側に持つ。Whisper にも同じ規則を適用すれば、エンジンによらず字幕の区切りを揃えられる。

### 4. 文脈による校正 (ローカル LLM)

同音異義の取り違え (「衆参」→「中3」) は、音響モデルより前後の文脈で直せる誤りである。認識後に全文を RTX 4090 上のローカル LLM へ渡し、話題の文脈から誤りの候補を出させる。勝手に置き換えず、差分を候補として示し、利用者が字幕ごとに採否を選ぶ (LLM の創作で正しい字幕を壊さない)。[未検証] 日本語の校正に足りる規模 (8B〜14B) と所要時間。

### 5. 語彙ヒントをプロジェクトに保存する

現状の語彙ヒントは認識のたびに入力する。番組・プロジェクトごとの用語集 (人名・地名・用語) を Project に保存し、毎回の認識へ渡す。Parakeet にはプロンプトが無いので、4 の校正の入力にも使う。

### 6. 見直しの支援

- 単語ごとの確信度の低い箇所を字幕一覧と本文欄で強調し、確認すべき字幕へ移動できるようにする。
- 無音・音楽区間での作文 (幻覚) を減らすため、VAD で発話区間だけを認識する。

### 7. 未評価のモデル

- Cohere Transcribe 03-2026: 他社の比較で上位。Hugging Face で利用者が利用規約に同意すれば評価できる。時刻を出さないので、使うなら Qwen3-ForcedAligner (CrispASR の `-am`) で時刻を付ける。
- Qwen3-ASR-1.7B (日本語版の追加学習を含む) は [事実] Whisper large-v3 と同等で、切り替える利点が見つからなかった。

### 8. 長尺素材のメモリと処理の分割

素材全体を認識すると、16kHz モノラルの float を全部メモリへ持つ (1 時間で約 230MB、4 時間で約 920MB)。大きいモデルの使用メモリと重なるので、認識を区間ごとに分けて処理し、結果をつなぐ。区間の境目の文を壊さないよう、無音の位置で分ける。

### 9. 自動字幕の素材時刻の保持

clip の編集で字幕を置き直すとき、毎回 timeline の frame から素材の時刻を求め直す (frame へ丸めた値から)。速度変更を何度も繰り返すと丸め誤差が積もりうる。自動生成した字幕に認識元の素材の開始・終了時刻を内部に持たせ、そこから再投影すれば誤差が積もらない。上書きで他の clip の端が削られた場合の置き直しも、この情報があれば同じ規則で扱える。

### 10. 適用時の内容の照合

認識の前後は内容全体の hash で素材の差し替えを検出するが、候補を確認している間の差し替えは実体・size・更新時刻だけで検出している (適用時に GUI thread で数 GB を読まないため)。更新時刻まで偽装した差し替えも検出するなら、適用の操作を非同期にして worker で hash を取り直す。

## 既知の未解決の問題

- [事実] 2026-10-05、release の通常 CTest (1447 件) で `preview_spike_json_contract` が 1 回だけ落ちた。
  `preview_spike_device_sharing_contract` が書いた `p1/contract-h264.json` で、
  `displayed=181` が `submitted=180`・`decoded=180` を超えていた (契約違反 3 件 / 検査 80 件)。
  直後に producer と checker を組で 3 回回すと 3/3 通過した。
  `mvm_preview_spike` は `gpu_preview` と `preview_qt` だけに依存し、これらはこの時点で変更していない。
  [推測] 提示の数え方に、まれに 1 回多く数える競合がある。
  [未検証] 再現条件。
- [事実] 2026-10-05、release の通常 CTest で `preview_engine_p5c_product_smoke` が 1 回だけ
  `0xC0000409` で異常終了し、単独で 3 回回すと 3/3 通過した。`mvm_p5c_preview_smoke` は controller を含まず、
  preview engine はこの時点で変更していない。[未検証] 再現条件と原因。
- (解決済み) 同じ時期の `transition_preview` の失敗 (フレーム送りで mapping と違う frame を提示) は、
  数式 clip の cache が起動時の backend 確認の完了を通知した時刻に、数式 clip の無い Project でも
  preview を組み直していたためと推測した [推測]。数式 clip が無ければ組み直さないよう直した後、
  通常 CTest 1447/1447 で通過した。

- [未検証] 利用者から、長尺のスピーチと BGM を含む Project
  (`build/ucrt64-debug/m6a-gui/project.mvm`) で、縮小時のホイール応答が重いとの報告がある。
  画面外の目盛り・音量線・キーの生成を抑え、長尺波形を使う回帰試験で検査した
  (`docs/premiere-like-editing.md` §26)。初回の対策後も、30 分以降を中心にしたズームで重いとの報告があった。
  [事実] 再現試験では、倍率とスクロール補正の途中に広い表示範囲で字幕を大量生成していた。
  更新をまとめる修正と操作途中の件数の回帰試験を追加した (§26.1)。
  [未検証] この Project での修正後の体感改善と残る負荷。
  [推測] 全体表示で多数の字幕を一度に生成する負荷は残る。改善が不足する場合は、
  波形だけでなく字幕 delegate の生成と scene graph 更新を切り分け、画面の密度に応じた
  字幕表示を検討する。性能判定は release で行う。

- [事実] 2026-10-04、保存済みの自動音量調整の素材を監視中に、中央の 1 byte を書き換えて
  size と更新時刻を元へ戻すと、`QFileSystemWatcher` の `fileChanged` が 30 秒以内に来なかった
  (`audio_adjustment_contract` の作成中に観測)。[推測] Qt の Windows 実装が stat の変化で通知を
  判定している。待機中の再生成の案内はこの変更を見逃す。解析結果の適用は保護した素材の
  内容 hash で照合するので、この経路では古い解析を適用しない。待機中も検出が必要なら、
  低頻度の内容 hash か OS の変更 journal を検討する。

- [事実] 2026-10-04、自動音量調整の検証で release の `transition_preview` が、
  23.976 fps の初期提示の準備と `stale-engine-seek` の先読み要求の前提で失敗する回があった。
  debug の全体検査では通過した。変更前 HEAD (`91eda91`) と変更後の release を固定回数で
  比較すると、変更前は通過し、変更後にも通過する回があった。原因と新機能との因果関係は
  未特定であり、全体検査を合格とは扱わない。新機能の解析・試聴・編集・書き出しの関連追試は
  修正後に通過している。比較ログは `build/audio-adjustment-baseline/baseline-transition*.xml`、
  `build/ucrt64-release/audio-adjustment-final.xml` と `audio-adjustment-transition-*.xml` に保存した。
  入口移動・音声解析の最適化後の `scripts/test.ps1 -Fast` では release は 1434/1434 通過し、
  debug は `transition_preview` だけ失敗した (frame 110 の incoming 不透明度が −1、期待 0.025)。
  この形は下記の単独検証でも観測されている。新機能の解析・試聴・製品メニュー操作は両構成で通過。
  ログは `build/audio-adjustment-optimized-tests.log` に保存した。

- [事実] 2026-10-04、release の通常テスト一式 (並列 8) の 1 回で
  `preview_engine_p5e_remove_fatal_event_order` が SEGFAULT で落ちた。単独では 5/5 通過。
  preview engine は字幕の作業で変更していない。再現条件と原因は未確認。

- [事実] 利用者の環境で、debug build の clip の Alt+ドラッグ複製を離した直後に `std::vector<TimelineClip>::operator[]` の範囲検査で落ちた。mp3・リンクした字幕・実 preview・実 window の操作を揃えても再現しなかった。再発時は `C:\msys64\ucrt64\bin\gdb.exe -batch -ex run -ex bt --args build\ucrt64-debug\bin\mvm.exe` の出力から原因を特定する。推測による修正はしていない。
- [事実] 2026-10-04 の切り分け: `transition_preview` を単独で、負荷なしで 5 回 → 4 回通過
  (1 回は「再生中のトランジションで incoming の不透明度が進み具合で上がりません」)。全論理コアを
  回す負荷をかけて 5 回 → 0 回通過で、テスト一式で出ていた失敗 (「境界の source の準備を
  要求しません」が複数の場面で、「cut の前から再生を開始できません」) がそのまま再現した。
  テスト一式を直列 (`-Jobs 1`) で回すと通過した。CPU の競合が失敗の主因である。
  続けて試験に失敗時の状態の出力を足して全負荷で回すと、失敗した場面はすべて最初の段階
  (preview の準備が 30 秒以内に終わらない) で止まっていた (再生位置が先へ進んで前提を外れる
  という当初の推測は誤り)。負荷なし・半分の負荷 (各 3 回) はすべて通過し、製品の cut の
  最大の提示間隔は負荷の有無にかかわらず 1〜2 frame で、cut で止まることは無かった。
  対処として、この試験を CTest で他の試験と並べずに走らせる (`RUN_SERIAL`)。
  [未検証] 全論理コアを埋める負荷の下で preview の準備に 30 秒以上かかることが、実際の利用で
  問題になるか (半分の負荷では起きない)。
- [事実] `transition_preview` は字幕の作業より前の版 (`e92e3ea`) でも、release の通常テスト一式の中で失敗した (2 回のうち 1 回。「境界の source の準備を要求しません」という前提の失敗で、字幕の作業後と同じ形)。この版では他に 23〜25 件の失敗があり、比較の条件は揃っていない。直列 (`-Jobs 1`) での実行と、失敗時の準備要求の前後の記録による「並列の負荷による期限切れ」か「共有状態の競合」かの切り分けは未実施。以下はそれ以前の記録。
- [事実] `transition_preview` が release の通常テスト一式の中で 2 回とも失敗し (失敗する場面は毎回異なり、いずれも「境界の source の準備を要求しません」という前提の失敗)、単独では 2 回とも通過した。原因と、今回の変更より前の版での発生有無は未確認。
