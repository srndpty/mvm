# メモリ増加の切り分け (S6 correctness closure)

ownership soak で Working Set の増加が観測されたため、
**WorkingSetSize だけに依存しない測定**へ直して切り分けた。

計測: `mvm_bench memory-probe`、実行: `scripts/memory-matrix.ps1`
各ケースは**必ず別プロセス**で実行する（前のケースの状態を持ち越さないため）。
ビルドは RelWithDebInfo。

取得する値（`PROCESS_MEMORY_COUNTERS_EX`）:
`WorkingSetSize` / `PeakWorkingSetSize` / `PrivateUsage` / `PagefileUsage` / handle 数。

phase: `before_runtime_init` / `after_runtime_init` / `after_open` /
`after_frame` / `after_audio` / `after_close` / `after_runtime_shutdown` /
`after_runtime_shutdown_idle`（500ms 待機後）。

CSV には**全反復・全 phase** を出す（10 回ごとの省略はしない）。

---

## 結論

**[推測] 観測されたのは、少なくともこの構成では、反復回数に比例して
増え続ける種類の増加ではない。** MLT runtime の生存期間に紐づく
何らかの保持（cache 的なもの）が最も整合するが、
**MLT の内部実装は確認しておらず、「global cache である」とは断定しない。**

以下は「観測された形」であって、機構の同定ではない。

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

ケース F は同一グラフを 1 回だけ open して 1000 フレーム取得した。

**この条件の限界を先に書く。** ケース F は `frame = iteration % 300` で
フレームを選んでいる。つまり 1000 反復のうち**実際にアクセスした固有フレームは
300 件だけ**であり、301 反復目以降は既出フレームの再取得である。
したがって「増加が止まった」ことは
**「固有フレーム 300 件分を保持しきったら止まった」までしか意味しない。**
固有フレーム数を増やしたときに同じ上限で止まるかは**測っていない**。

| 指標 | 値 |
| --- | --- |
| PrivateUsage first → last | 355.0 → 470.5 MB |
| 第 1 四分位平均 → 第 4 四分位平均 | 463.8 → 470.1 MB |
| 全体の傾き | +121,268 B/反復 |
| **後半 1/4 の傾き** | **−2,171 B/反復（減少）** |
| 最大の単発増加 | 反復 1 で +27.0 MB（355.0 → 382.0） |

**増加は最初の 250 反復でほぼ終わり、残り 750 反復では横ばい**である。
この条件では線形増加ではない。ただし上記のとおり、横ばいに入った時点は
固有フレームを一巡し終えた時点とほぼ一致しており、
**「容量上限で止まった」のか「新しいフレームが来なくなったから止まった」のかを
この測定は区別できない。**

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

**[事実]** この構成で `mlt_factory_init` / `mlt_factory_close` を繰り返すと
`0xC0000409`（STATUS_STACK_BUFFER_OVERRUN 相当の fail-fast）で落ちた。
ケース G（反復ごとに runtime を作り直す）も完了しなかった。

言えるのは**「現在の構成では runtime の再初期化が安全に動かなかった」**までである。
MLT 側の制約なのか、mvm 側の初期化・終了手順の問題なのか、
モジュール読み込み順の問題なのかは**切り分けていない**。
`mlt_factory_close` の実装も確認していない。

**[推測]** MLT の runtime は 1 プロセス 1 回の初期化を前提としている可能性がある。

**製品への影響は小さい。** mvm は runtime を起動時に 1 回だけ初期化し、
プロセス終了まで維持する設計である。ただし
**「runtime を長時間維持する前提が正しい」ことの裏返しでもある**ので、
テストコードで runtime を作り直さないよう注意する。

---

## 分類の当てはめ

以下は**この測定条件（1080p60・5 トラック・固有フレーム 300 件）における**
当てはめである。条件を変えたときも同じとは限らない。

| パターン | 該当 |
| --- | --- |
| Working Set 増加 / PrivateUsage 横ばい | **該当しない**（両方増える） |
| PrivateUsage が段差増加後 plateau | **該当**（C の反復 173、F 全体） |
| PrivateUsage が反復に比例して増え続ける | **該当しない**（F の後半 1/4 は減少） |
| shutdown で PrivateUsage が大きく戻る | **該当**（C で 208 MB、F で 409 MB） |
| shutdown 後も線形増加分を保持 | **該当しない**（残るのは反復数に依存しない 61〜73 MB） |

**[推測]** 最も整合するのは「runtime lifetime に紐づく保持」という説明である。
ただし上で述べたとおり、**「上限がある」と言い切れる測定にはなっていない。**
ケース F の約 470 MB は「固有フレーム 300 件を保持した状態の値」であって、
確認された上限ではない。

**[未検証]** 以下はいずれも測っていない。S7 の proxy を入れる前に必要である。

- 固有フレーム数を増やしたとき（例: 1000 件、3000 件）に頭打ちになるか
- 素材の解像度（4K）・トラック数・尺への依存
- 上限が実用外の値まで上がらないこと

**現時点でメモリについて「合格」と記録できる根拠は無い。**

**[方針]** `mlt_service_cache_purge` などの private API を
恒久的な対策として導入しない。使う場合は診断専用と明記する。
`EmptyWorkingSet` による強制 trim も、合否判定前には使わない。

## unique frame に対するメモリ (S7 診断)

**S6 の宿題への回答である。**

S6 のケース F は `frame = iteration % 300` で、1000 反復のうち
実際にアクセスした固有フレームは **300 件だけ**だった。
そのため「増加が止まった」ことは
「固有フレーム 300 件分を保持しきったら止まった」までしか意味しなかった。

S7 ではケース U（`frame = iteration`、**同じフレームを取り直さない**）で
**3600 unique frame** を順に取った。S6 の 12 倍である。

計測: `mvm_bench memory-probe --case U`、実行: `scripts/memory-unique-frames.ps1`
経路ごとに別プロセス。全フレームで marker 一致を確認済み（不一致 0）。
メモリ採取に失敗したサンプルは 0 件。

| 経路 | unique frame | 所要 | Priv first→last | WS first→last | 全体の傾き | **後半 1/4 の傾き** | 最大段差 | shutdown 後 Priv | handle first→last |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1080p-native | **3600** | 216.1s | 358.4 → 472.3 MB | 327.6 → 450.6 MB | 33176 B/frame | **0 B/frame** | 反復 1 で 26.4 MB | 62.2 MB | 125 → 1860 |
| 4k-original | **3600** | 435.6s | 703.5 → 958.1 MB | 660.4 → 924.8 MB | 74188 B/frame | **-50 B/frame** | 反復 1 で 64.0 MB | 61.2 MB | 125 → 1860 |
| 540p-proxy | **3600** | 233.0s | 277.4 → 360.0 MB | 249.9 → 341.6 MB | 24078 B/frame | **0 B/frame** | 反復 1 で 17.7 MB | 61.7 MB | 125 → 1863 |

### 何が言えるか

**[事実] PrivateUsage は unique frame 数に比例して増え続けない。**
3600 unique frame でも**後半 1/4 の傾きは 0 / −50 / 0 B/frame** である。
S6 で観測した plateau は「300 件しか触っていなかったから」ではない。

**[事実] 最大の段差は 3 経路とも反復 1 で起きる**（17.7〜64.0 MB）。
以降は 1 frame あたり 0 に近い。初期化時の確保である。

**[事実] shutdown 後は 3 経路とも 61.2〜62.2 MB に戻る。**
経路によらずほぼ同じ値であり、
反復回数にも unique frame 数にも依存しない。

**[事実] 解像度に比例して水準は上がる。**
Priv last は 4K 958.1MB / 1080p 472.3MB / 540p proxy 360.0MB。
**proxy は 4K の 38%** である。

**[推測]** 反復 1 の段差はデコーダとフレームバッファの確保、
その後の平坦部分は解放と再利用が釣り合っている状態と考えられる。
ただし **MLT 内部の実装は確認していない。**
「cache」「leak」と機構を断定しない。

**[未検証]** これは M16 の正式な 30 分試験ではない。
以下は測っていない。

- 30 分連続再生（M16 の正式条件）
- 4K で 3600 を超える unique frame（素材が 3600 frame しかない）
- 複数の異なる素材を跨いだ場合
- proxy と original を切り替えながらの長時間動作

**重大所見に該当するものは無い。**
「PrivateUsage が unique frame 数に比例して増え続ける」という
S7 の中止条件には当たらなかった。

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
