# MLT 実装メモ

S3 の本格的な資料調査はまだ行っていない。
本書は S1 / S4 で**実際に動かして確かめた**範囲だけを記録する。
上流ドキュメントの記述ではなく、この環境（MLT 7.36.1 / MSYS2 UCRT64）での観測である。

---

## 初期化

```c
_putenv_s("MLT_DATA", data_dir);        /* factory 初期化より前に必要 */
mlt_repository repo = mlt_factory_init(module_dir);
```

- `mlt_factory_init(NULL)` は場所を実行ファイル相対で推測する。開発ビルドでは必ず外れる
- **外れても失敗しない。** service 0 件のまま成功を返す
- `MLT_DATA` は `mlt_factory_init` の前に設定する。`profiles/` の解決に使われる
- モジュール/データ ディレクトリは **ASCII のみ**。非 ASCII を渡すと全 service が消える

MSYS2 のレイアウト（上流ドキュメントの `lib/mlt-7` ではない）:

| | |
| --- | --- |
| モジュール | `<prefix>/lib/mlt` |
| データ | `<prefix>/share/mlt` |
| profile | `<prefix>/share/mlt/profiles` |

pkg-config から取得できる（推測に頼らない）:

```
pkgconf --variable=moduledir   mlt-framework-7   # <prefix>/lib/mlt
pkgconf --variable=mltdatadir  mlt-framework-7   # <prefix>/share/mlt
```

## profile

```c
mlt_profile p = mlt_profile_init("atsc_1080p_60");
```

- **解決に失敗しても NULL を返さない。** 既定値 `720x576 @ 25/1 (SAR 16/15)` を返す
- したがって返り値の `width` / `height` / `frame_rate_num` / `frame_rate_den` を必ず照合する
- `mlt_profile_init(NULL)` は既定 profile を返す

素材から導出する:

```c
mlt_profile_from_producer(profile, producer);
```

- 素材の解像度・fps・SAR を profile に取り込む
- **映像が無い素材（WAV など）では fps が変わらない。** 既定の 25 のままになる
- 取り込んだ後は producer を開き直す必要がある

## producer（avformat）

```c
mlt_producer p = mlt_factory_producer(profile, "avformat", path_utf8);
```

- **素材パスは UTF-8 で渡せる。** CP932 外の文字（絵文字・ハングル・簡体字）でも動作する
  （モジュールディレクトリとは対照的。所見 8 参照）

確認済みのプロパティ:

| プロパティ | 内容 |
| --- | --- |
| `meta.media.nb_streams` | ストリーム数 |
| `meta.media.<n>.stream.type` | `"video"` / `"audio"` |
| `meta.media.<n>.codec.name` | codec 名（`h264` / `hevc` / `aac` / `pcm_s16le` / `png`） |
| `meta.media.<n>.codec.pix_fmt` | `yuv420p` / `yuv420p10le` / `rgba` |
| `meta.media.<n>.codec.sample_rate` | 音声サンプルレート |
| `meta.media.<n>.codec.channels` | 音声チャンネル数 |
| `meta.media.width` / `.height` | 映像の解像度 |
| `meta.media.frame_rate_num` / `_den` | 映像の fps（有理数） |
| `meta.media.sample_aspect_num` / `_den` | SAR |
| `resource` | 開いたパス |

`mvm_bench probe --dump-props` で全プロパティを実素材に対して出力できる。
プロパティ名は version やモジュールで変わりうるので、推測せずこれで確認する。

### 尺（length）の扱い — 要注意

`mlt_producer_get_length()` は **profile の fps でのフレーム数**を返す。

| 素材 | length | 意味 |
| --- | --- | --- |
| 1080p60 / 5 秒 | 300 | profile を素材から導出済みなので素材のフレーム数と一致 |
| WAV / 5 秒 | 125 | profile が 25fps のため。素材の性質ではない |
| PNG（静止画） | 2147483647 | INT_MAX。「尺が無限」の意味。duration ではない |

duration を出すには **profile の fps** で割る。素材の fps ではない。

```c
duration = length * profile->frame_rate_den / profile->frame_rate_num;
```

INT_MAX の場合は duration を計算してはいけない（8.59e7 秒になる）。

## フレームの取り出し

```c
mlt_producer_seek(p, frame);
mlt_frame f = NULL;
mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &f, 0);

mlt_image_format fmt = mlt_image_rgba;
int w = 0, h = 0;
uint8_t* image = NULL;
mlt_frame_get_image(f, &image, &fmt, &w, &h, 0);
/* image は frame が所有する。frame を close する前にコピーする */
mlt_frame_close(f);
```

- `mlt_image_rgba` で取り出すとアルファが保持される
  （PNG のアルファグラデーションが `alpha_min=0 / alpha_max=253` で残ることを確認済み）
- 単発 seek + 取得は要求フレームに正確に着地する（マーカーで確認、frame 0/1/137/299）
- 連続 seek / scrub は S6 で実測した（[seek-scrub-notes.md](seek-scrub-notes.md)）。
  seek 精度は 214/214 一致。速度（M5）と scrub の random パターン（M6）は
  基準未達で、**proxy 導入後（S7）の再測定が必要**

## service の登録名

- `avfilter` は `avfilter.<name>` として個別に登録される。
  `avfilter` という名前の filter は存在しない
- フィルタ総数が 474 件と多いのはこのため

確認済みの必須 service（MLT 7.36.1 / MSYS2）:

| 種別 | 名前 | モジュール |
| --- | --- | --- |
| producer | `avformat` `qimage` `xml` `color` | avformat / qt / xml / core |
| filter | `avfilter.scale` `crop` `brightness` `volume` | avfilter / core |
| filter | `affine` `dynamictext` | **plus**（optional 依存が必要） |
| filter | `qtext` | qt6 |
| transition | `qtblend` `mix` `composite` | qt6 / core |
| consumer | `avformat` `sdl2` `null` | avformat / sdl2 / core |

`plus` モジュールは `libebur128` と `libfftw3` を必要とする。
MSYS2 ではこれらが optional 扱いのため、既定では `affine` と `dynamictext` が消える。

## エラー報告の性質

MLT は失敗を戻り値ではなく stderr の 1 行で伝え、処理を継続する傾向がある。
API から「失敗したモジュール一覧」を取得する手段は無い。

そのため mvm 側で以下を自前で検査している（`mvm_mlt_runtime.c`）:

- モジュールディレクトリを走査して `LoadLibraryW` を試す
- ディレクトリが無い / DLL が 0 件の場合も失敗として数える
- 必須 service の登録を照合する
- profile の実値を期待値と照合する

## まだ調べていないこと（S3 / S5 以降）

- `mlt_tractor` / `mlt_multitrack` / `mlt_playlist` の構造と mvm の Project Model への写像
- `real_time` プロパティとフレームドロップの挙動
- consumer の `rescale` と preview / final の一致（V12）
- `mlt_animation` によるキーフレーム表現
- `affine` の座標系（原点・値域・SAR の扱い）
- Shotcut / Kdenlive の実装（`mltcontroller.cpp` / `glwidget.cpp`）
