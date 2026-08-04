# MLT の参照所有権

対象: MLT 7.36.1（MSYS2 UCRT64）
根拠: 上流ソースを実際に読んで確認した。推測で close を足していない。

参照したファイル（`v7.36.1` タグ）:
`src/framework/mlt_service.c` / `mlt_field.c` / `mlt_transition.c`

---

## 結論

**`mlt_factory_*` で得た参照は、attach / plant が成功した直後に呼び出し側が閉じる。**

attach / plant 先は自前で参照を増やすため、factory 参照を持ち続けると
参照数が 1 多いまま残る。

---

## 根拠となるソース

### filter: `mlt_service_attach` は inc_ref する

```c
int mlt_service_attach(mlt_service self, mlt_filter filter)
{
    ...
    mlt_properties_inc_ref(MLT_FILTER_PROPERTIES(filter));   // ← +1
    base->filters[base->filter_count++] = filter;
    ...
}
```

解放側:

```c
void mlt_service_close(mlt_service self)
{
    ...
    while (count--)
        mlt_service_detach(self, base->filters[0]);   // detach が mlt_filter_close する
    ...
}
```

```c
int mlt_service_detach(mlt_service self, mlt_filter filter)
{
    ...
    mlt_filter_close(filter);   // ← -1
    ...
}
```

**factory で 1、attach で 2、service close で 1 に戻る。**
呼び出し側が閉じないと 1 残る。

### transition: `mlt_field_plant_transition` は自身では inc_ref しない

```c
int mlt_field_plant_transition(mlt_field self, mlt_transition that, int a_track, int b_track)
{
    int result = mlt_transition_connect(that, self->producer, a_track, b_track);
    if (result == 0) {
        self->producer = MLT_TRANSITION_SERVICE(that);
        mlt_tractor_connect(self->tractor, self->producer);   // ← ここで増える
        ...
    }
    return result;
}
```

`mlt_tractor_connect` → `mlt_service_connect_producer`:

```c
int mlt_service_connect_producer(mlt_service self, mlt_service producer, int index)
{
    ...
    mlt_properties_inc_ref(MLT_SERVICE_PROPERTIES(producer));   // ← +1 (producer = transition)
    ...
    mlt_service_close(current);   // 直前に繋がっていたものを閉じる
    ...
}
```

解放側は `mlt_service_close(tractor)` の

```c
for (i = 0; i < base->count; i++)
    if (base->in[i] != NULL)
        mlt_service_close(base->in[i]);   // ← -1
```

**transition も factory 参照を閉じないと 1 残る。**

なお `mlt_field_close` は tractor も multitrack も閉じない
（該当行がコメントアウトされている）。field は transition を所有しない。

複数の transition を plant した場合は、次の transition が
`mlt_transition_connect(that, self->producer, ...)` で前の transition を
inc_ref するため、鎖状に参照が保持される。tractor を閉じると
先頭から連鎖して解放される。

---

## 所有権表

| 対象 | 生成 | 誰が参照を増やすか | 誰が解放するか | 呼び出し側の責務 |
| --- | --- | --- | --- | --- |
| `mlt_profile` | `mlt_profile_init` | 誰も増やさない | 誰も解放しない | **`mlt_profile_close` する** |
| `mlt_producer`（素材） | `mlt_factory_producer` | `mlt_playlist_append` が cut 経由で保持 | playlist close 時 | **`mlt_producer_close` する** |
| `mlt_playlist` | `mlt_playlist_new` | `mlt_tractor_set_track` → `connect_producer` が +1 | tractor close 時 −1 | **`mlt_playlist_close` する** |
| `mlt_tractor` | `mlt_tractor_new` | 誰も増やさない | — | **`mlt_tractor_close` する** |
| `mlt_filter` | `mlt_factory_filter` | `mlt_service_attach` が +1 | attach 先の close → detach が −1 | **attach 成功直後に `mlt_filter_close` する** |
| `mlt_transition` | `mlt_factory_transition` | `plant` → `tractor_connect` が +1 | tractor close 時 −1 | **plant 成功直後に `mlt_transition_close` する** |
| `mlt_consumer` | `mlt_factory_consumer` | 誰も増やさない | — | **`mlt_consumer_close` する** |
| `mlt_frame` | `mlt_service_get_frame` | — | — | **`mlt_frame_close` する** |

解放順序は **tractor → playlist → producer → profile**。
tractor が playlist の参照を持っているため、先に tractor を閉じる。

---

## 修正した箇所

**[修正前]** 次のコメントを前提に、filter と transition を配列で保持したまま
close していなかった。

> attach / plant した時点で所有権が移っているので close しない

これは**誤り**だった。上表のとおり attach / plant は参照を「増やす」のであって
「移す」のではない。結果として `mvm_mlt_compose_open` を呼ぶたびに
filter と transition の参照が 1 ずつ残っていた。

**[修正後]**

- `src/media/mlt/mvm_mlt_compose.c`
  - qtext filter: `mlt_producer_attach` 成功直後に `mlt_filter_close`
  - volume filter: 同上
  - mix transition: `mlt_field_plant_transition` 成功直後に `mlt_transition_close`
  - 映像 transition（affine）: 同上
  - `filters[]` / `transitions[]` の所有配列を削除
- `src/media/mlt/mvm_mlt_audiograph.c`
  - volume filter / mix transition について同じ修正
  - 所有配列を削除

attach / plant が**失敗した場合**は、まだ誰も参照を増やしていないので
その場で `close` してから抜ける。

保持しなくなったため、構築後に filter / transition の property を
変更することはできない。必要になった場合は
「owned 参照」か「borrowed ポインタ」かをコメントで明示すること。

---

## 検証方法

`mvm_bench soak <scenario> --iterations 100 --wav-dir <dir>` が
同一プロセス内で open → frame → audio render → close を 100 回繰り返し、
以下を検査する（CTest の `ownership_soak_100`）。

- 100 回すべて成功する
- marker と音声 RMS が初回と最終回で一致する
- Windows handle 数が単調増加しない（前 1/4 と後 1/4 の平均で比較、+8 まで許容）
- RSS が反復あたり 256KB を超えて線形増加しない

RSS には allocator のキャッシュが乗るため、小さな増減はリークと断定しない。

---

## 100 回反復の実測結果

実行: RelWithDebInfo、5 トラックシナリオ、100 回、所要 **2 分 55 秒**。
音声レンダリングは 10 回に 1 回（5 秒分の通しレンダリングは重いため）。

| 項目 | 結果 |
| --- | --- |
| 完走 | **100/100** |
| crash / heap 破壊 | **なし** |
| marker（初回 / 最終回） | **137 / 137**（一致） |
| 音声 RMS（初回 / 最終回） | 0.394706 / 0.394706（一致） |
| **handle 数** | 1868 → 1873（前 1/4 平均 1869、後 1/4 平均 1874） |
| RSS | 225.3MB → 374.8MB |

**[事実] handle 数は増加していない。** 100 回で +5 は測定ノイズの範囲であり、
filter / transition の参照を閉じるようにした修正が効いていることを示す。

**[未解決] RSS は増加している。** サンプル（10 回ごと）:

| 反復 | RSS |
| --- | --- |
| 0 | 225.3MB |
| 30 | 233.4MB |
| 60 | 238.1MB |
| 90 | 245.1MB |
| 99 | **374.8MB** |

0→90 回は約 0.22MB/回のゆるやかな増加で、これは MLT の内部キャッシュ
（producer ごとの image cache 等）で説明できる範囲かもしれない。
一方、**90→99 回で 245MB → 375MB という不連続な跳ね上がり**があり、
これは説明できていない。

**CTest の `ownership_soak_100` は現在この RSS 判定で失敗する。**
閾値（反復あたり 256KB）を緩めて緑にすることはしない。
検査は実際に何かを検出しており、原因が分かるまで失敗のままにする。

**[未検証]** 跳ね上がりの原因。切り分けていない候補:
- MLT の image / frame キャッシュの解放タイミング
- allocator（mingw の msvcrt heap）がまとめて確保した分
- 音声レンダリング経路（10 回に 1 回）の蓄積
- 反復ごとの `mlt_profile_init` / `mlt_factory_*` の内部テーブル

次バッチで、音声なし 100 回・音声のみ 10 回など条件を分けて測るべきである。

## ASan

**実行できなかった。**

| 試行 | 結果 |
| --- | --- |
| `g++ -fsanitize=address` | `cannot find -lasan`（mingw の gcc は libasan を同梱しない） |
| `clang++ -fsanitize=address` | `libclang_rt.asan_dynamic.dll.a` が無い |
| `mingw-w64-ucrt-x86_64-compiler-rt` を導入して再試行 | 同じエラー。mingw ターゲット向けの ASan ランタイムが含まれていない |

MSYS2 の mingw-w64 ターゲットでは ASan が提供されていない。
使うなら MSVC ビルド（`/fsanitize=address`）か、
Linux 上でのビルドが必要になる。いずれも Phase 0 の範囲外。
