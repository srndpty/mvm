# tests/lint-negative

`scripts/lint.ps1` の producer service 検査そのものの negative test。

検査を書いただけでは、それが効いている証明にならない。
「違反を入れたら本当に落ちるか」と「正しいコードを誤検出しないか」を
両方確かめる。

| ディレクトリ | 内容 | 期待 |
| --- | --- | --- |
| `bad/` | `mlt_factory_producer(..., "avformat", ...)` を単一行と複数行で含む | lint が **失敗** |
| `good/` | producer は `NULL` (loader)、consumer は `"avformat"` | lint が **成功** |

`bad/` にはコメント中にも `avformat` と `mlt_factory_producer` を書いてある。
検査が文字列出現ではなく呼び出しの形に反応することを確かめるため。

これらはビルド対象ではない。通常の lint 走査からも除外され、
`-Path` で明示したときだけ検査される。
