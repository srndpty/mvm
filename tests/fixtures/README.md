# tests/fixtures

`mvm_bench verify-media` の negative test 用に、意図的に壊した manifest を置く。

これらが**失敗すること**を CTest が確認する。通ってしまうと、
`verify-media` が「検証したつもり」で成功を返していることになり、
V2 の判定根拠そのものが無意味になる。

`relative_path` は `../assets/smoke/...` を指す。
manifest からの相対で解決されるため、実素材が必要（未生成ならテストは実行されない）。

| ファイル | 壊し方 | 期待 |
| --- | --- | --- |
| `bad-missing-schema.json` | `schema_version` が無い | 失敗 |
| `bad-schema-version.json` | `schema_version` が対応外 (999) | 失敗 |
| `bad-empty-assets.json` | `assets` が空配列 | 失敗 |
| `bad-unknown-kind.json` | `kind` が `"movie"` | 失敗 |
| `bad-duplicate-id.json` | 同じ `id` が 2 つ | 失敗 |
| `bad-missing-expected.json` | `expected` が無い | 失敗 |
| `bad-missing-field.json` | `expected` に `pix_fmt` が無い | 失敗 |
| `bad-wrong-pix-fmt.json` | `pix_fmt` を `yuv422p` に改変 | 失敗 |
| `bad-wrong-sar.json` | `sar_num`/`sar_den` を `4/3` に改変 | 失敗 |
| `bad-wrong-duration.json` | `duration_sec` を `9.0` に改変 | 失敗 |
| `bad-wrong-alpha.json` | PNG の `alpha_min_le` を `-1` に改変（透明画素が存在しえない） | 失敗 |
| `good-minimal.json` | 正しい最小 manifest | **成功** |

`good-minimal.json` は「negative test が構造ではなく壊した箇所で落ちている」ことを
示すための対照群である。これが失敗するなら、negative test の失敗理由は
意図した箇所ではない。
