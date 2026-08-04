# メモリ増加の切り分け (S6 correctness closure)

ownership soak で Working Set の増加が観測されたため、
**WorkingSetSize だけに依存しない測定**へ直して切り分けた。

計測: `mvm_bench memory-probe`、実行: `scripts/memory-matrix.ps1`
各ケースは**必ず別プロセス**で実行する（MLT の global cache を持ち越さないため）。
ビルドは RelWithDebInfo。

取得する値（`PROCESS_MEMORY_COUNTERS_EX`）:
`WorkingSetSize` / `PeakWorkingSetSize` / `PrivateUsage` / `PagefileUsage` / handle 数。

phase: `before_runtime_init` / `after_runtime_init` / `after_open` /
`after_frame` / `after_audio` / `after_close` / `after_runtime_shutdown` /
`after_runtime_shutdown_idle`（500ms 待機後）。

CSV には**全反復・全 phase** を出す（10 回ごとの省略はしない）。

---

## 結論

**リークではなく、MLT runtime の生存期間に紐づく global cache が最も整合する。**

根拠は 2 つある。

### 1. runtime shutdown でほぼ全量が返る

| ケース | 反復終了時の PrivateUsage | shutdown 後 | 返った量 |
| --- | --- | --- | --- |
| B（open/close x200） | 79.4 MB | **63.5 MB** | 15.9 MB |
| C（frame x200） | 280.2 MB | **72.6 MB** | **207.6 MB** |
| F（frame x1000, graph 再利用） | 470.5 MB | **61.1 MB** | **409.4 MB** |

`after_runtime_shutdown_idle`（500ms 後）は `after_runtime_shutdown` と
**完全に同値**だった。allocator が遅れて返しているのではなく、
`mlt_factory_close()` の時点で解放されている。

`before_runtime_init` は全ケースで PrivateUsage 1.9 MB。
shutdown 後に残る 61〜73 MB は反復回数に比例せず、
MLT モジュール 23 個と FFmpeg のロード分と考えられる（**[推測]**）。

### 2. 反復を増やすと plateau する

**ケース F が決定的。** 同一グラフを 1 回だけ open して 1000 フレーム取得した。

| 指標 | 値 |
| --- | --- |
| PrivateUsage first → last | 355.0 → 470.5 MB |
| 第 1 四分位平均 → 第 4 四分位平均 | 463.8 → 470.1 MB |
| 全体の傾き | +121,268 B/反復 |
| **後半 1/4 の傾き** | **−2,171 B/反復（減少）** |
| 最大の単発増加 | 反復 1 で +27.0 MB（355.0 → 382.0） |

**増加は最初の 250 反復でほぼ終わり、残り 750 反復では横ばい**である。
線形増加ではない。cache が容量まで埋まって止まった形。

---

## ケース別の結果

| ケース | 内容 | 反復 | 所要 | Priv first→last | Priv/反復 | 後半1/4/反復 | 形 | 最大段差 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | runtime のみ init/shutdown | 50 | — | **クラッシュ** | — | — | — | — |
| B | open/close（frame なし） | 200 | 16.0s | 59.6 → 79.4 MB | 104 KB | 58 KB | 逓減 | 反復 17 で +3.5 MB |
| C | open/frame/close | 200 | 81.4s | 129.9 → 280.2 MB | 792 KB | 69 KB | 段差後 plateau | **反復 173 で +132.4 MB**（147.8 → 280.2） |
| D | open/audio/close | 30 | 未実施 | — | — | — | — | — |
| E | open/frame/audio/close | 100 | 未実施 | — | — | — | — | — |
| F | graph 再利用 + frame x1000 | 1000 | 74.2s | 355.0 → 470.5 MB | 121 KB | **−2 KB** | 逓減 → plateau | 反復 1 で +27.0 MB |
| G | 反復ごとに runtime init/shutdown | 50 | 未完了 | — | — | — | — | — |

Working Set は全ケースで PrivateUsage とほぼ同じ形を示した
（B: 48.4→76.9、C: 123.0→279.1、F: 324.8→447.0 MB）。
**「Working Set だけ増えて PrivateUsage は横ばい」というパターンではない。**
確保した仮想メモリが実際に増えており、それが shutdown で返っている。

handle 数は全ケース 122 → 1762〜1865。これは MLT 初期化時の一度きりの増加であり、
反復には比例しない。ownership soak（単一 runtime 内）では 1877 → 1874 で横ばい。

### ケース C の +132 MB 段差について

**[未解決]** 反復 173 で PrivateUsage が 147.8 → 280.2 MB へ一気に増える。
それ以外の反復は数十 KB 単位の変化しかない。

cache の拡張、allocator の arena 追加、あるいは特定の反復でだけ
大きな確保が起きたなど複数の説明がありうるが、切り分けていない。
CSV（`build/ucrt64-release/memory/C.csv`）に全反復・全 phase を残してある。

### ケース A のクラッシュ

**[事実]** `mlt_factory_init` / `mlt_factory_close` を繰り返すと
`0xC0000409`（STATUS_STACK_BUFFER_OVERRUN 相当の fail-fast）で落ちる。

ケース G（反復ごとに runtime を作り直す）も完了しなかった。

**[推測]** MLT の runtime は 1 プロセス 1 回の初期化を前提としており、
繰り返しの init/shutdown は想定されていない。

**製品への影響は小さい。** mvm は runtime を起動時に 1 回だけ初期化し、
プロセス終了まで維持する設計である。ただし
**「runtime を長時間維持する前提が正しい」ことの裏返しでもある**ので、
テストコードで runtime を作り直さないよう注意する。

---

## 分類の当てはめ

| パターン | 該当 |
| --- | --- |
| Working Set 増加 / PrivateUsage 横ばい | **該当しない**（両方増える） |
| PrivateUsage が段差増加後 plateau | **該当**（C の反復 173、F 全体） |
| PrivateUsage が反復に比例して増え続ける | **該当しない**（F の後半 1/4 は減少） |
| shutdown で PrivateUsage が大きく戻る | **該当**（C で 208 MB、F で 409 MB） |
| shutdown 後も線形増加分を保持 | **該当しない**（残るのは反復数に依存しない 61〜73 MB） |

**最も整合するのは「MLT runtime lifetime の global cache」。**
製品は runtime を長時間維持するため、
**plateau する上限があること**が重要であり、ケース F がそれを示している
（約 470 MB で頭打ち）。

**[未検証]** 上限が素材の解像度・トラック数・尺にどう依存するか。
4K 素材や長尺で上限が実用外まで上がる可能性は排除できていない。
S7 で proxy を入れる前に、この上限の依存性を測るべきである。

**[方針]** `mlt_service_cache_purge` などの private API を
恒久的な対策として導入しない。使う場合は診断専用と明記する。
`EmptyWorkingSet` による強制 trim も、合否判定前には使わない。

---

## テストの分離

| テスト | 目的 | 判定 |
| --- | --- | --- |
| `ownership_soak_100` | **正しさ**。完走 / crash なし / marker 一致 / audio RMS 一致 / handle 増加なし | 通常 CTest |
| `memory_soak_diagnostic` | メモリの**診断**。PrivateUsage を phase 別に記録 | `stability` ラベル（通常 CTest から除外） |

**Working Set の増減だけを理由に ownership を失敗としない。**
ただし閾値を緩めて緑にしたのではなく、判定対象から外して
別テストへ切り出した。メモリについては
**PrivateUsage の上限依存性が分かるまで「合格」とは記録しない。**
