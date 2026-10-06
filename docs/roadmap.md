# 改善のロードマップ

「このアプリの今後の改善策は何か」に答えるための一覧。着手したら該当の文書 (例: `docs/subtitles.md`) へ移し、ここからは消す。記述の印は `docs/phase0-findings.md` と同じ (`[事実]` `[推測]` `[未検証]`)。

## 一般の preview

- [事実] 2026-10-07、P3-2.1 の通常 release gate 一回で `transition_preview` が
  「23.976fps の preview が準備できません」と「区間の始まりのフレーム送りで mapping と違う frame を提示しました」で失敗した。
  原因と今回の compiler 分類変更との因果関係は未特定。再試行はしていない。
  再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。
  通常試験は 1460/1461 通過、通常 gate は未通過。
  証拠: `build/math-p321-release.log`、`build/math-p321-release-lasttest.log`。

## 一般の音声・書き出し

- [事実] 2026-10-07、P3-1.1 の通常 release gate 一回は 1459/1460 通過。
  `m4_timeline_export_focused_tractor` が「crop + 回転の clip を書き出せません」で失敗した。
  通常動画の対照で `editEquationSequence` を呼ばない。原因は未特定。再試行はしていない。
  再現: `pwsh scripts/test.ps1 -Preset ucrt64-release`。
  証拠: `build/math-p311-release.log`、`build/math-p311-release-lasttest.log`。

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
- [事実] 2026-10-06、P2-7 の通常 release gate 一回で `m4_timeline_export_focused_tractor` が失敗した
  (「末尾補完が必要なclipをtractorで書き出せません」、`padding-v1-effects.mp4` の tractor 出力を検証できない)。
  1457 件中 1456 件通過。P2-7 は書き出しの経路を変えていない。再試行はしていない。
  再現コマンドは `pwsh scripts/test.ps1 -Preset ucrt64-release`、証拠は `build/math-p27-release-20261006.log`。
  原因と再現性は未調査。
- [事実] 2026-10-06 (P2-8)、書き出しの成功の後に状態へ「Explorerで表示できません (HRESULT=0x80070057)」が
  付いた (書き出しは成功)。`revealFileInExplorer` は `QUrl::toLocalFile` の "/" 区切りの path を
  `SHParseDisplayName` へ渡しており、小さな program で "/" 区切りは 0x80070057、"\" 区切りは成功を確かめた。
  数式とは無関係。[未検証] 実アプリの書き出しの dialog でも毎回起きるか。直すなら区切りを "\" にしてから渡す。

## 数式 clip

### P3: native Equation Sequence と部分式の強調

- P3-0 は設計と製品外 renderer spike のみ。判断と再現手順は
  [設計文書](math-equation-sequence-p30.md)。新しい `EquationSequence` kind を推奨し、
  clip 内の整数 frame の hold/transform、永続 part ID と revision 付き source annotation、
  中立な区間 action を Project の正とする。P2 PASS/CLOSED と schema 20 は維持する。
- P3-0.1 は文書のみの明確化を完了。内部 ID は種別ごとに sequence 内で一意とし、
  copy/paste/duplicate は全 ID を新規発行して remap、split は左片に元 ID を残し右片を新規発行する。
  両片は完全な sequence と異なる可視 source 範囲を持つ。cache key は所有 ID に依存しない。
  state 削除時は所有 action も原子的に削除し、missing StateId と orphan-action store は認めない。
  修復可能な missing PartId は既存 state 内だけ。全状態の Math 背景は透明、初期 operation は outline/pulse のみ。
- (実装・検証済み: P3-1) 記録は [設計文書の P3-1 実装](math-equation-sequence-p30.md#p3-1-実装) へ移した。
- P3-1.1 の外側 source 範囲修正は [設計文書](math-equation-sequence-p30.md#p3-11-外側-source-範囲の修正) に記録。
  schema 21 の domain、素材原点の frame 始点標本化、構造編集、参照 remap、通常 Undo を扱う。
  renderer・cache・製品 UI は後続段階のまま。
- P3-2 の binding と renderer 中立 plan の実装は
  [設計文書の P3-2](math-equation-sequence-p30.md#p3-2-semantic-binding-と-renderer-中立-plan) に移した。
  UTF-8/UTF-16 境界、信頼編集、明示 rebind、semantic partition、explicit 優先と所有の排他性、
  ID 非依存の正準入力を扱う。非空の実 glyph の証明は `BackendValidationRequired` として P3-3 に残す。
- P3-3 の実 Manim renderer・backend の構造検証・disk の artifact の契約は
  [設計文書の P3-3](math-equation-sequence-p30.md#p3-3-sequenceaction-renderer) に移した。
  key namespace `mvm-equation-sequence/1`、2 段階の Manim 起動 (構造検証の後だけ描く)、
  点を持つ子孫による非空・排他的所有、静止の artifact で hold を出す同値、outline/pulse の 2 層 A8、
  P2 と同じ publication の規則を扱う。set_color/reveal/conceal と styled endpoint、同時 action は含めない。
- P3-4 の residency と product preview は
  [設計文書の P3-4](math-equation-sequence-p30.md#p3-4-residency-と-product-preview) に移した。
  output frame だけから決まる提示、代用の契約 (変形は区間の全 frame で前の静止)、Write・変形と
  共有の上限での 1 層ごとの residency と action の 2 層の束、provenance の色での合成、前の key の
  結果を使わない世代の扱いを扱う。書き出し・authoring UI は含めない。
- P3-5: 製品 UI。既存の暗色 panel と共通部品で状態の追加/削除/順序/hold/変形、part の指定、
  既存 state 内の invalid/missing PartId の修正、outline/pulse の preview と一回の Undo を実装。
  狭幅・低い panel・長文・空一覧を実描画で検証。state 削除は所有 action の削除も同じ操作として示す。
- P3-6: 実 D3D11 preview と映像のみ export。全区間が現在の入力で ready であることを要求し、
  一つの clip の導出全段と判別式の強調を UI author→save/reopen→preview→export で検証する。
  任意 seek、action 中の split、編集・失敗・backend 不在、画素をずらす負例を含める。

未解決の判断・検証:

- 任意 TeX macro の支持は未検証。P3-3 は P3-2 の支持範囲の segment について、Manim 0.21 の
  点を持つ子孫の排他的所有を描画ごとに検査する (実装記録)。検査を通った範囲の成功を、
  任意範囲の renderer 分離保証へ一般化しない。
- 動く pulse の 2 層の合成と Manim の 1 つの scene の差は P3-4 で測った
  ([設計文書](math-equation-sequence-p30.md#p3-4-residency-と-product-preview))。alpha は全画素一致、
  premultiplied の色は部分被覆の画素だけで最大 1 違う (同じ実数の丸め方の違い)。この差を製品の
  品質として受け入れるか (書き出しの受け入れ基準に含めるか) は未判断。許容差を正の検査には入れていない。
- P3-4 の先読みは今の区間の残りと次の区間だけ。再生中に先読みが間に合わない frame は代用 (静止) で
  見せる。広い lookahead・区間の先頭の事前読み込みは性能の課題として残す。
- P3-4 の Equation Sequence の layer は出力の大きさの透明な静止画を下地にし、animation の instance
  ごとに GPU texture を作る (数式の Write・変形と同じ方式)。層が届くたびに instance を作り直すので、
  再生中の texture の作り直しの回数と時間は未測定。
- timeline のトランジション (Blend など) と EquationSequence clip の重なりは preview でも未対応として
  拒否する。sequence の可視範囲の外へ素材範囲を延ばす意味を決めてから扱う。
- [事実] P3-3.1 の変異試験で、長い名前の作業 directory の下で cache の作業 path
  (`jobs/<session>/<64 桁 key>-equation-sequence-<n>`) が 260 文字を超え、job directory を作れなかった。
  P2 の変形の作業 path も同じ形。cache directory が深い Project で同じことが起きうる [推測]。
  extended-length path で作るかは未判断。
- P3-3 の key は色・hold・action の start も含むので、それらだけの編集でも sequence 全体を描き直す。
  区間ごとの小さな artifact (P3-0 の依存 DAG) への分割と、合成だけの値を key から外す条件は未判断。
  P3-4 は正しさに必要ないので sequence 全体の key と artifact の構造をそのまま使った (性能の課題)。
- [未検証] Equation Sequence の描画時間と disk 量。1 件ごとに Manim を 2 回起動する
  (実測は受け入れの results.json の ms のみで、性能計測ではない)。
- [事実] P3-3 の集中 CTest で `math_write_native_playback` が 22 検査中 3 件失敗し、単独の診断 3 回は
  通過した。BuildIndependent で `audio_mixer_controls_qml` が ScrollBar の binding loop で失敗した。
  通常 release gate 一回は 1463/1464 で、同じ `audio_mixer_controls_qml` だけが失敗した
  (`build/math-p33-release.log`。`math_write_native_playback` は通過)。
  どちらも原因と P3-3 の変更との因果関係は未特定。再試行による選別はしていない。
  証拠: `build/math-p33-focused.log`、`build/math-p33-diag-native-write-1..3.log`、
  `build/math-p33-independent.log`。
- [事実] `audio_mixer_controls_qml` は P3-3 の BuildIndependent・gate、P3-3.1 の BuildIndependent・gate の
  4 回続けて同じ警告 (QML ScrollBar の binding loop) で失敗した。P3-2 の gate では通過していた。
  失敗する log には `OpenThemeData() failed ... ハンドルが無効` が多数出る。QML とそれが読む file は
  変更していない。原因は未特定 (環境の theme の状態か、別の変更か) で、再試行による選別はしていない。
  証拠: `build/math-p331-independent.log`、`build/math-p331-release.log`。
  P3-4 の BuildIndependent と通常 gate でも同じ警告で失敗した (6 回続けて。QML は変更していない)。
  証拠: `build/math-p34-independent.log`、`build/math-p34-release.log`。
- [事実] P3-4 の集中 CTest で `math_write_native_playback` が P3-3 と同じ 3 件で失敗した
  (`build/math-p34-focused.log`)。単独の診断 3 回は通過・失敗・通過 (`build/math-p34-diag-native-write-1..3.log`)、
  通常 gate では通過。Write の経路と試験は変更していない。原因と因果関係は未特定。
- retime を後段で許す条件は将来の判断とする。P3-1 の標本位相と短い action の消失の契約は
  [実装記録](math-equation-sequence-p30.md#時間と-fps-の確定契約) にある。
- 既存 state 内の invalid/missing PartId の診断・修復 UI と書き出し拒否の境界を検証する。
  state 削除時の action の原子的削除は確定事項であり、missing StateId を保持する選択肢は設けない。
- set_color/reveal/conceal は将来の設計候補。styled endpoint の保存と変形を仕様化し renderer で
  検証してから別段階として導入する。概念説明を理由に初期 schema に値や振る舞いを予約しない。
  同時 action と入れ子/非連続 part も初期に拒否し、必要なものだけ gate を増やす。
- outline/pulse の拡張 bbox と layer 数に対する予算、RGBA と複数 A8 の実測比較。
  renderer spike は性能・cache publication・製品 decoder の証明ではない。
- 既存の複数 Math clip を一 clip に明示変換する際の hold/transform の消費時間の対応。
  自動 migration は行わず、受け入れ対象に必要なら別操作として検証する。


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
  - (済: P2-7) 製品の UI からの作成・長さ・削除 (`docs/math-clips.md` の「MathTransform の編集 UI」)
  - (済: P2-8) 統合受け入れと手動の確認。MathTransform P2 は PASS/CLOSED
    (`docs/math-clips.md` の「P2-8 統合受け入れ」と「最終判定」)。
  - [事実] P2-8 で、数式の作業 directory の下の 260 文字を超える file を直した (extended-length の走査と
    Win32 の木の削除)。残る制限: 作業 directory の下の directory 自体が 260 文字を超える深さの Project
    (この開発機の作業 directory の作り方では、Project の directory と file 名の合計が約 100 文字を超える場合)
    では、libstdc++ の列挙がその中を返さず、描画は「PNG が N 枚ではありません (件数=0)」で失敗する
    (止まりはしない)。直すなら、作業 directory の名前から key (64 文字) を外して短くするか、走査も Win32 で行う。
  - `extendedLengthPath` は drive の絶対 path・UNC・普通の相対 path だけを対象にしている
    (`src/util/mvm_long_path.h`)。drive 相対 (`C:foo`)・root 相対 (`\foo`) が必要になったら、
    Windows の意味を実装して試験を足す。
  - [未検証] `mvm_remove_tree` は `FindFirstFileExW` に `FIND_FIRST_EX_LARGE_FETCH` を渡す。特殊な
    file system (NAS など) に cache を置く場合に `ERROR_INVALID_PARAMETER` 等で断られるなら、
    flag 無しで開き直す。今の local の cache では問題は観測していない。
  - [推測] P2-8 で、試験の待ちが約 2 ms ごとに engine の status を読むと、試験の window の描画の周期が
    止まり、paused の seek の提示が 30 秒待っても終わらなかった (20 ms ごとでは 5 / 5 通過)。
    `math_write_native_playback` などの同じ形の待ち (`pump` の 2 ms) の一時的な失敗が同じ原因かは未検証。
    仕組みも確かめていない。
  - [事実] 2026-10-06、P2-7 の集中試験の一括実行で `math_write_native_playback` が 1 回失敗した
    (一時停止中の mask の読み込みと再生の開始、22 検査中 3 件)。単独では 6 / 6 通過し、
    同じ日の通常 release gate でも通過した。原因は未特定 (`build/math-p27-focused-20261006.log`)。
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
  [事実] 2026-10-06、P2-6.1 の通常 release gate 一回でも、frame 110 の incoming 不透明度が
  −1 (期待 0.025) となり、`stale-engine-reset` の source 準備要求の前提も失敗した。
  証拠は `build/math-p261-release-20261006.log`。preview の処理は変更していないが、
  今回の発生原因は未特定。再試行による成功 run の選別はしていない。
  [事実] 2026-10-06、P2-8 の集中試験 (24 件) でも同じ「frame 110: incoming の不透明度 -1.000
  (期待 0.025)」で 1 回失敗した (`build/math-p28-focused-20261006.log`)。再試行はしていない。

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
