# AGENTS.md

## 言語

**人間が読む想定の文章はすべて日本語で書く。**

- AI チャットの返答
- コードのコメント
- コミットメッセージ
- ドキュメント
- ログ・エラーメッセージ（利用者が読むもの）

識別子・型名・ファイル名・CLI のサブコマンド名は英語のままでよい。
混在させる基準は「読み手が人間か、機械か」である。

## 設計原則

### DRY

同じ判断ロジックを 2 箇所に書かない。特に検査ロジックは、
実装が食い違うと「片方だけ通る」という最悪の形で表面化する。

例: MLT の健全性検査は `src/media/mlt/mvm_mlt_runtime.c` に一本化し、
`mvm_mlt_hello` と `mvm_bench doctor` の両方がこれを呼ぶ。

ただし、テストの期待値は重複してよい。テストが実装と同じ式を共有すると、
実装のバグをテストが追認してしまう。

### 責務の分離

| 層                        | 責務                  | 依存してよいもの     |
| ------------------------- | --------------------- | -------------------- |
| `src/core`, `src/project` | 純粋なデータと計算    | 何にも依存しない     |
| `src/util`                | OS 依存の小さなヘルパ | Win32                |
| `src/media/mlt`           | MLT との唯一の接点    | MLT C API            |
| `src/app`                 | Qt シェル             | Qt                   |
| `tests/harness`           | 検証 CLI              | 上記の公開ヘッダのみ |

Phase 1 で追加した層:

| 層                      | 責務                                       | 依存してよいもの                    |
| ----------------------- | ------------------------------------------ | ----------------------------------- |
| `src/core`              | 純粋なデータと計算（marker reader を含む） | 何にも依存しない                    |
| `src/media/gpu_preview` | FFmpeg (D3D11VA) と D3D11                  | FFmpeg C API / D3D11。**Qt は不可** |
| `src/app/preview`       | QRhi / QQuickRhiItem との唯一の接点        | Qt（private API を含む）            |
| `apps/preview_spike`    | P1 の検証アプリ                            | 上記の公開ヘッダと Qt               |

**MLT のヘッダを include してよいのは `src/media/mlt/` だけ。**
同様に、**QRhi（Qt の private API）を include してよいのは `src/app/preview/` だけ。**
QRhi は patch release 間でも互換保証が無いため、壊れる範囲を限定する。
Mlt++（C++ ラッパ）は使わず C API のみを使う。
これにより Project Model・UI・公開 interface へ MLT の型が漏れず、
将来 backend を差し替える余地が残る。

`src/media/mlt/` の公開ヘッダに MLT の型を出してはいけない。
プレーンな構造体と関数だけを出す。

### フォールバックと互換性

**開発中なので最小でよい。**

- 依存が見つからなければ、代替を探さずに明確なメッセージで失敗する
- 環境差の吸収を先回りして書かない
- 後方互換のための分岐を残さない

暗黙のフォールバックは、失敗を静かに成功へ変えてしまう。
Phase 0 で最も避けたい事故がこれである（後述）。

### 失敗は必ず失敗として扱う (fail-closed)

MLT は失敗しても失敗と分からない形で縮退する。
実測した例は `docs/phase0-findings.md` にある。

- モジュールが 0 件でも `mlt_factory_init` は成功を返す
- profile が解決できなくても既定値を返す
- 静止画の尺は INT_MAX

したがって:

- 戻り値が非 NULL であることを成功と見なさない。**値を検証する**
- 「比較対象が無いので比較しない」を成功にしない。
  検証を飛ばしたなら、飛ばしたことを出力に残す
- テストが通ったら、**空振りで通っていないか**を確認する

過去に、duration の計算式が原因で WAV のテストが空振りで通過していた。
「一致した」ではなく「本当に比較したか」まで確認すること。

### テストしにくい巨大クラスを作らない

Phase 0 では検証 CLI が中心だが、それでも
「引数解析」「MLT 操作」「判定」「出力」は分けて書く。

---

## ツールチェーン

**MSYS2 UCRT64 に統一する。** Qt / MLT / FFmpeg / アプリ本体で
CRT および C++ ABI を混在させない。

この開発機には他プロジェクト用の Qt 6.8.3 (MSVC) が
`C:\Users\lambe\sdk\Qt\6.8.3` にある。**これを参照・変更・削除しない。**
誤って拾うとリンクは通るのに実行時に不可解な形で壊れるため、
`cmake/mvm_toolchain_guard.cmake` が configure 時に弾く。

FFmpeg / ffprobe は `C:\msys64\ucrt64\bin` のものだけを使う。
ホストの `C:\tools` 版や winget 版へフォールバックしない。

```powershell
pwsh scripts/bootstrap-msys2.ps1     # 依存導入
pwsh scripts/build.ps1               # ビルド (PATH を整えて cmake を呼ぶ)
pwsh scripts/test.ps1                # ビルド + CTest (release/debug)
pwsh scripts/format.ps1              # clang-format 適用
pwsh scripts/lint.ps1                # 整形差分と静的検査
pwsh scripts/coverage.ps1            # カバレッジ
```

計測用の matrix スクリプト（いずれも生 JSON から集計まで行う）:

```powershell
pwsh scripts/preview-matrix.ps1        # M7 preview (real_time 構成別)
pwsh scripts/proxy-matrix.ps1          # proxy 生成 (M8 の生成速度)
pwsh scripts/make-proxy-scenarios.ps1  # original / proxy の scenario 生成
pwsh scripts/scrub-matrix.ps1          # M6 scrub (正式 8 条件 x 3 回)
pwsh scripts/seek-matrix.ps1           # M5 seek (経路別)
pwsh scripts/memory-matrix.ps1         # メモリ切り分け (ケース A-G)
pwsh scripts/memory-unique-frames.ps1  # unique frame メモリ診断 (ケース U)
pwsh scripts/audio-graph-matrix.ps1    # 音声グラフの最小構成切り分け
```

Phase 1 / P1:

```powershell
pwsh scripts/p1-matrix.ps1             # **P1 の正式な計測。合否はこれだけで決める**
pwsh scripts/p1-matrix.ps1 -Quick      # 短縮版 (経路確認のみ。判定に使えない)
pwsh scripts/check-p1-contract.ps1 -Json <path>   # 生 JSON の契約検査
pwsh scripts/make-color-fixtures.ps1   # color correctness 用 fixture の生成
```

color fixture は `tests/assets/color/` に置く。
期待 RGB は生成スクリプトが **標準式から独立に**計算しており、
実装の関数 (`coefficientsFor`) は呼んでいない。
実装を呼んで期待値を作ると、実装のバグをテストが追認する。

契約検査は `check-p1-contract.ps1` に一本化している。
CTest と `p1-matrix.ps1` の両方がこれを呼ぶ。2 箇所に書かない。

`scripts/expect-exit.ps1` は「意図した終了コードで失敗したこと」を検査する。
CTest の `WILL_FAIL` は 0 以外なら合格なので、
使い方エラーを期待している negative test がクラッシュでも通ってしまう。

**計測値を文書へ手で転記しない。** スクリプトが生 JSON から再計算し、
集計の自己整合（例: `updates/sec == displayed/elapsed`）を機械で検査する。

`scripts/build.ps1` を使うこと。`C:\msys64\ucrt64\bin` が PATH に無いと
gcc は**エラー出力なしに**失敗し、CMake からは「compiler is broken」としか見えない。

## テスト

```powershell
cd build\ucrt64-release
ctest --output-on-failure
```

- 通常の CTest は Smoke 素材（5 秒）を使い短時間で終わること
- 長時間の性能計測は `-L performance` で分離する（通常実行に含めない）
- 合否を決められない長時間の診断は `-L stability` で分離する
  （メモリ測定など）。通常実行は `-LE 'performance|stability'` で
  **両方を除外する**。除外しないと「通常テストが何件通ったか」が分からなくなる
- テスト件数は種別ごとに分けて報告する（`scripts/test.ps1` が出力する）
- **性能値を exit criteria に使うときは release / RelWithDebInfo で測る。**
  debug ビルドの数値を判定に使わない

素材が未生成のテストは、原因不明の失敗にせず、
実行すべきコマンドを案内すること。

```powershell
pwsh scripts/make-testmedia.ps1 -Mode Smoke       # 自動検査用
pwsh scripts/make-testmedia.ps1 -Mode Benchmark   # 性能計測用
```

### negative test を必ず添える

新しい検査を足したら、**その検査が無ければ落ちるテスト**を同時に足す。
検査を書いただけでは、それが効いている証明にならない。

`tests/fixtures/` に、意図的に壊した manifest を置いてある。
対照群 `good-minimal.json` も置き、
「negative test が構造ではなく壊した箇所で落ちている」ことを示している。

## コミット

- 日本語で書く
- 何を変えたかではなく、**なぜ変えたか**を書く
- 実測値がある場合は含める（「24/24 通過」「seek p95 = 12ms」など）
- 依頼されない限りコミットしない

## ドキュメント

`docs/phase0-findings.md` は Phase 0 の中心的な記録である。
記述は必ず次のいずれかに分類する。混ぜない。

| 印         | 意味                                               |
| ---------- | -------------------------------------------------- |
| `[事実]`   | 実際に実行して観測した。再現手順を併記する         |
| `[推測]`   | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない       |
| `[回避策]` | 現在の対処。恒久策とは限らない                     |
| `[exit]`   | exit criteria への影響                             |

**「動くはず」を「動く」と書かない。** 過去に
「MLT は開けなかった素材でも producer を返す」と書いたが、
これは測って確かめた事実ではなく仮定であり、実測すると誤りだった。

## Phase 0 で作らないもの

タイムライン UI、undo/redo、本番 Project Model、自動字幕、数式アニメーション、
Python worker、エフェクト UI、キーフレーム編集、レンダーキャッシュ、
インストーラ、コード署名、`IMediaEngine` の全体像。

詳細は `docs/phase0-plan.md` の「Phase 0 では実装しない項目」を参照。

## 計測で踏んだ罠（同じ失敗を繰り返さない）

いずれも**「緑に見える」方向**の誤りだった。計測を書くときは必ず確認する。

| 罠                           | 症状                                               | 対処                                                                      |
| ---------------------------- | -------------------------------------------------- | ------------------------------------------------------------------------- |
| 配信数を fps と呼ぶ          | 60fps 出ているように見えて実際は 13.6fps           | `rendered=1` の frame だけ数える。`delivered` と `effective` を分けて出す |
| `real_time=0` で計測         | 何も描画せずに高い fps が出る                      | 拒否する（MLT は `mlt_frame_get_image` を呼ばない）                       |
| `Sleep` 回数で経過時間       | 「6 秒計測」が 12.3 秒になる                       | `QueryPerformanceCounter` の実測値を使う                                  |
| マーカーのセル幅を決め打ち   | 4K→1080p 合成で 314/314 不一致                     | `readMarkerAuto`。同期は「19 セル全部が振り切れている」まで要求する       |
| proxy の音声を再エンコード   | 尺が 1 frame 伸びて MLT からは 3601 frame に見える | `-c:a copy`。duration 差 1 frame **以上**で破棄                           |
| 中央値の max で判定          | 外れ値が隠れる                                     | 全 run を通した**観測 max** も出して、そちらで判定する                    |
| `WILL_FAIL` で negative test | クラッシュでも合格になる                           | `scripts/expect-exit.ps1` で終了コードを厳密に照合                        |
| 対象 0 件のテスト群          | 「全部通った」と報告される                         | `-N` で件数を数え、0 件なら失敗にする                                     |

## Interactive measurement protocol

Performance / ETW / DWM / GPU presentation / window-state acquisition を
ユーザーに実行してもらう場合、実行コマンドを提示する前に必ず
ユーザー操作の可否を明示する。

表記は次のいずれかとする。

- **操作可**: measurement 中も通常の PC 操作をしてよい。
- **操作制限あり**: 禁止する操作、理由、必要時間を具体的に示す。
- **操作停止必須**: measurement 中は対象 desktop へ入力しない。想定所要時間を必ず示す。

操作が測定結果へ影響し得るにもかかわらず、無案内で取得を依頼してはならない。
コマンドを渡すときは冒頭に必ず次の形式の banner を置く。

```text
【操作停止必須：約12分】
この測定は desktop damage が DWM wake に影響するため、
実行中は Alt+Tab、window 移動、他アプリ操作、動画再生をしないでください。
```

特に DWM / presentation / occlusion / dirty / VBlank / ETW を扱う取得では、
window overlap だけでなく Alt+Tab、window move/resize、他アプリの描画、
notification、動画再生等の desktop damage が結果を変え得るものとして扱う。
F3-C3-A3-T1 で `EXTERNAL_DIRTY` だけが DWM wake regime を激変させることが
3/3 で causal に出ている。mvm と無関係な別 window の damage で足りる。

ユーザー操作または desktop 状態が protocol 条件を破った run は
性能 FAIL として扱わず `PROTOCOL_INVALID` とする。
historical artifact は削除・上書きしない。

可能な限り runner 自身でも次を記録し fail-close する。

- target geometry / visibility / iconic / cloaked
- foreground change
- user input occurrence（`GetLastInputInfo`）
- unexpected window overlap
- acquisition/session collision

ただし**入力が無くても別アプリが勝手に描画すれば external dirty は起きる**。
別モニタへ逃がすだけでも不十分（T1 が示したのは DWM-wide effect のため）。
この種の run は同じ PC で 10〜15 分操作を止めるか、別の物理マシンで作業する。

retry-until-success が scheduler timing や測定対象の状態で cohort を
条件付ける可能性がある場合、自動 retry で有効 run を選別してはならない。
起動失敗が特定条件にのみ存在する場合は選別ではなく原因を先に直す。

## 起動時 configuration の確定順序

`QQmlApplicationEngine::load()` は QML の `visible: true` を通じて
render thread を起動しうる。したがって `load()` の後に config を書き込むと、
render thread の `initialize()` が GUI thread の設定を追い越した run だけ
別の構成で走る。

- render 側から読む設定は `load()` より前に確定させるか、
  window を `visible: false` で生成し設定確定後に明示的に可視化する。
- render 側の capability 取得を、後から変更される diagnostic flag に
  依存させない。使用可否は使用箇所で fail-close する。

この race は F3-C3-A3-T2-B で `TARGET_RHIITEM_PIXEL_TOGGLE` 条件だけを
確率的に 0xC0000005 で落とし、causal matrix の cohort を条件付けた。
regression は `tests/gpu_preview/test-p2-c3-a3-t2-startup-order-contract.ps1`、
起動反復の gate は `scripts/p2-c3-a3-t2-startup-smoke.ps1` で固定している。

## HWND damage probe の段階

`InvalidateRect` は update region を設定するだけで、`WM_PAINT` や DWM の
damage processing が起きるとは限らない。したがって「InvalidateRect が効かない」
だけで redirection path を疑ってはならない。必ず次の3段を経由する。

```text
1. TARGET_HWND_INVALIDATE   InvalidateRect(hwnd, &rect, FALSE)
2. TARGET_HWND_REDRAW_NOW   RedrawWindow(..., RDW_INVALIDATE|RDW_UPDATENOW|
                                              RDW_NOERASE|RDW_NOCHILDREN)
3. TARGET_REDIRECTION_PATH_SUSPECT
```

1 が suppression でも 2 へ進む。2 が REGULAR なら Win32/QPA → DWM の
damage-processing boundary への attribution であり、2 でも suppression かつ
`EXTERNAL_DIRTY` のみ REGULAR のときに初めて 3 へ進む。

両者が実際に別物であることは controller 側で確認できる。`InvalidateRect` の
直後は update region が残り、`RedrawWindow(UPDATENOW)` の直後は消費されている。
`target_update_region_observed_count` がこの区別を記録する。

`RDW_UPDATENOW` は Qt 側の event processing を刺激しうる。HWND damage だけを
注入した比較であり続けることを確かめるため、T2-C でも T2-A の update-chain
closure を全条件で再検査し、`native_present_count` の条件間 spread が 2% を
超えた run は `UPDATE_CHAIN_VOLUME_DIVERGENT` として解釈を限定する。

## DWM PresentStart を display authority にしない

**DWM Present cadence は app display cadence ではない。**
DWM PresentStart が 0 件でも、app は 900/900 displayed でありうる。

F3-C3-A3-T2-D0 で、T2-B / T2-C1 / T2-C2 の 27 run すべて
(`EXTERNAL_DIRTY` 9 run を含む) が次だった。

```text
present_mode   Hardware_Composed_Independent_Flip  900/900
DWM parent     0
FinalState     Presented 900
DisplayedQPC   900   provenance: InFrame+Win32k+DxgkPresent
                     (DwmParentDisplayed なし)
DWM-wide PresentStart  0 〜 885 (条件により変動するが display path は不変)
```

independent flip 中は DWM がスリープでき、app のフレームは直接 display へ出る。
外部 window の damage でだけ DWM が起きるのは期待挙動であって欠陥ではない。
逆に、大量 Discard を示した historical run では DWM parent が
3598/3598 付きながら displayed は 104 件しかなかった。
**DWM が最も活発な run が最も壊滅的に discard している。**

したがって次を守る。

- `target_attached_parent_count` や DWM PresentStart 数を
  physical-display authority にしない。
- display の最終 authority は次の順で閉じる。

```text
composition token
  -> native Present identity
  -> PresentMon app PresentEvent
  -> FinalState / DisplayedQPC
  -> physical display identity
```

- 「DWM wake が少ない = 表示 drop」と読まない。presentation path
  (independent / composed) を先に排除してから display を論じる。
- raw の field 欠損を 0 とみなさない。旧 acquisition schema は
  `attached_dwm_parent_present_start_qpc` を持たない。StrictMode で読み、
  欠損時は `DISPLAYED_PATH_UNRESOLVED` として fail-close する。

## capability と actual presentation path を混ぜない

`presentation_eligibility_preflight` は diagnostic-only の説明変数であり、
presentation path の authority ではない。

```text
capability (eligibility 説明変数)
    DXGI_SWAP_CHAIN_DESC1 実値
    hardware composition support flags
    tearing support
    adapter / output identity
    window style / cloaked / geometry

observed runtime (actual presentation path の authority)
    PresentMode
    FinalState
    DisplayedQPC とその provenance
    DWM parent identity
```

hardware composition capable であることは、その Present が independent flip /
MPO されたことを意味しない。schema 上も別 object に分け、
`is_presentation_path_authority=false` を固定する。

swapchain の値は Qt の設定や環境変数から再構成せず、実際に作成済みの
`IDXGISwapChain` から `GetDesc1()` で取る。patched Qt が記録する
`swapchainIdentity` は実ポインタであり、swapchain を所有する render thread の
`frameSwapped` 契機で一度だけ読む。

## PowerShell の自動変数を変数名に使わない

`$input` に加えて `$profile` も実際に踏んだ（IDE の PSScriptAnalyzer が検出）。
`$profile` は PowerShell profile path の自動変数である。


`$input` は PowerShell の**自動変数**で、pipeline / stdin の enumerator に
束縛される。これを自前の変数名として使うと、stdin が開いたままの pipe である
環境（ctest の test process など）で enumerator の materialize が EOF 待ちに
なり、**プロセスが無限にハングする**。

実際に `p2_c3_a3_t2_d1b1_probe_*` が ctest のバッチ実行で毎回 timeout した。
単体実行や小さいバッチでは再現せず、当初「一過性」と誤診した。
`$input` を `$userInput` へ改名して解消（106/106 PASS、timeout 0）。

- `$input` `$args` `$_` `$PSItem` `$matches` `$error` `$host` `$this` 等を
  変数名にしない。
- ctest 実行時は `--timeout` を付け、hang を無限待ちにしない。
- timeout が出たら「一過性」と断定せず、残留 process を PID 指定で止めて
  再現性を確認する。machine-wide な `Stop-Process -Name pwsh` は禁止。

## ctest の test 環境は PATH が細い

ctest が起動する test process の PATH には `git` や UCRT64 の tool が
入っていないことがある。開発シェルでは通るのに ctest でだけ落ちる。

- test / script から外部 tool を呼ぶなら `Get-Command` で解決し、
  見つからなければ既知の install 先へ fallback してから明示 path で呼ぶ。
- 解決できないときは fail-closed にする（暗黙に PATH 依存で通さない）。
- ctest 実行時は `--timeout` を付ける。

## historical archaeology の停止条件

過去 run の root cause 追跡は、次のいずれかに達したら止めて
current runtime の formal authority wiring へ戻る。

```text
EXACT_HISTORICAL_RUNTIME_UNAVAILABLE
    かつ
REBUILD_PROBE_NOT_EVALUABLE
```

`REBUILD_PROBE_NOT_EVALUABLE` は、observer を両 arm へ同等に適用できない場合に
宣言する。F3-C3-A3-T2-D1-B2 では native present hook の ABI が v2/v3 で
非互換であり、historical Qt6Gui も残っていなかった。

このとき historical 観測は不変保存したうえで次を確定させる。

```text
historical BAD         preserved
current reproducibility NOT ESTABLISHED
historical cause        NOT ESTABLISHED
```

**historical root cause の完全解明を closure の前提にしない。**
再現しない過去不具合のために production change を発明しない。

## Windows / CMake / Ninja ビルド運用

このリポジトリでは MSYS2 UCRT64 の CMake/Ninja ビルドが長時間かかることがある。
tool の command timeout と実際のビルドハングを混同しないこと。

### ビルドの原則

- CMake/Ninja のビルドには短い command timeout を設定しない。
- 変更確認では、まず変更対象に対応する最小 target をビルドする。
- full build / full CTest は必要なゲートでのみ実行する。
- command invocation が timeout しただけでは、Ninja がハングしたと判断しない。
- timeout 後に同じ build directory へ即座に別の build を重ねて起動しない。

### timeout / 長時間ビルド時の確認

ビルドが長時間返らない場合、ソースや Ninja metadata を変更する前に以下を確認する。

1. 現在の repo/build directory を使用している cmake/ninja/compiler process が残っているか。
2. CPU time が増えているか。
3. child compiler (`g++`, `cc1plus` 等) が動作中か。
4. process command line がこのリポジトリの build directory を指しているか。
5. `.ninja_log` の更新時刻が進んでいるか。

CPU / child process / log のいずれかが進行している場合は、原則として待つ。
「tool timeout」だけを理由に process を kill しない。

### process cleanup

`Stop-Process -Name ninja,cmake` のような machine-wide kill を禁止する。

cleanup が必要な場合は、このリポジトリの build directory を command line に含む
process、または今回起動した build process tree の PID に限定する。

別リポジトリやユーザーが起動した cmake/ninja/compiler processを終了しない。

### Ninja metadata

`.ninja_deps` / `.ninja_log` の削除・退避は通常のビルド手順として行わない。

以下を確認した場合のみ recovery として行う。

- この repo に属する残留 build process がすべて終了済み
- source/build.ninja/CMake configure の状態に問題がない
- Ninja が依存DB読み込み・依存走査付近で再現性をもって停止する
- 通常の再実行でも再現する

metadata recoveryを行った場合は、
何を退避/削除したか、なぜ必要だったかを報告する。

ソース変更によってビルド問題を回避しない。

### recovery escalation

順序は以下とする。

1. active buildなら待つ
2. repo-scoped residual processのみ終了
3. 同じ build dir で再実行
4. 必要なら CMake reconfigure
5. Ninja metadata corruptionの根拠がある場合だけ `.ninja_deps` 等を退避して再生成
6. 最後の手段として新しい clean build directory で再現確認

既存 build directory を無条件で削除しない。

「build directoryを保持している」「Ninja dependency DBが壊れている」等の原因を、
process/logによる証拠なしに推定して修復操作へ進まない。

agent の反復確認では `pwsh scripts/build.ps1 -Target <target>` を使う。
timeout 後の読み取り専用診断は
`pwsh scripts/build-diagnostics.ps1 -Preset ucrt64-release` を使い、表示された
repo-scoped PID、CPU time、child compiler、`.ninja_log` 更新時刻を根拠に次の操作を決める。

### Codex sandbox の既知制約

この開発環境では、sandbox 内から CMake/Ninja を実行した場合に、
Ninja 自体は起動するが child compiler (`g++` / `cc1plus`) が起動せず停止する事例を確認済み。

確認済みの特徴:

- CMake/Ninja process は存在する
- CMake/Ninja の CPU time がほぼ増加しない
- `g++` / `cc1plus` が存在しない
- `.ninja_log` が進行しない
- `ninja -n <target>` は正常かつ短時間で完了する
- 新しい clean build directory でも同じ症状になる
- 同じ公式 build command を sandbox 外で実行すると正常完了する

この既知パターンに一致する場合、
Ninja metadata corruption や source/CMake の問題を毎回再調査しない。

1. `scripts/build-diagnostics.ps1` で上記signatureを確認する
2. repo/build-dirへ重複buildを起動しない
3. `.ninja_deps` / `.ninja_log` を変更しない
4. clean build directoryを毎回作り直さない
5. 公式 `scripts/build.ps1 -Target <target>` を sandbox 外で1回実行する

sandbox 内での失敗だけを product/source の失敗として扱わない。
sandbox 外でも再現した場合にのみ通常の recovery escalation へ戻る。

### プロセス終了安全対策

process command line を権限制約で取得できず repo ownership を確認できない場合、
process名だけを根拠に終了してはいけない。
終了可能なのは、このagent自身が起動してPID/process treeを追跡できているprocessだけとする。
