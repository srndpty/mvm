# tests/lint-negative

`scripts/lint.ps1` の producer service 検査そのものの negative test。

検査を書いただけでは、それが効いている証明にならない。
「違反を入れたら本当に落ちるか」と「正しいコードを誤検出しないか」を
両方確かめる。

| ディレクトリ | 内容 | 期待 |
| --- | --- | --- |
| `bad/` | `mlt_factory_producer(..., "avformat", ...)` を単一行と複数行で含む | lint が **失敗** |
| `good/` | producer は `NULL` (loader)、consumer は `"avformat"` | lint が **成功** |

層の隔離検査 (`-AsLayer` で層を与える):

| ディレクトリ | 内容 | 期待 |
| --- | --- | --- |
| `layer-bad/` | Qt / QRhi の include と CPU readback | `gpu_preview` として **失敗** |
| `layer-good/` | Qt も QRhi も使わない | `gpu_preview` として **成功** |
| `still-image-bad/` | Qt の include だけ (QRhi は含めない) | `still_image` として **失敗**、`preview_qt` として **成功** |

`still-image-bad/` に QRhi を含めないのは、含めると QRhi の規則で落ちてしまい、
still_image の規則が効いているかを判別できなくなるため。
`-AsLayer` に ValidateSet に無い値を渡すと、引数検証の失敗でも exit 1 になる。
negative test はそれでも緑になるので、層を足したら ValidateSet も更新し、
落ちた理由が検査のメッセージであることを確かめること。

`bad/` にはコメント中にも `avformat` と `mlt_factory_producer` を書いてある。
検査が文字列出現ではなく呼び出しの形に反応することを確かめるため。

これらはビルド対象ではない。通常の lint 走査からも除外され、
`-Path` で明示したときだけ検査される。
