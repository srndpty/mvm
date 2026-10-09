# P4-5 の検証結果

状態: **HOLD**。encoder 直前の独立 RGBA oracle が未通過。P4 と P4-5 を PASS/CLOSED にしていない。

以下は新規の生ログから再計算した件数。通常 release、BuildIndependent、実 Manim と製品 UI、変異の閉鎖 gate は未実施。過去の P3/P4 の証拠は変更していない。

|取得|処理|終了コード|生ログの検査件数|証拠|
|---|---|---:|---|---|
|math-p45-20261009-192204-Focused|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-192204-Focused/)|
|math-p45-20261009-192204-Focused|focused|0|57 件、失敗 0 件|[生証拠](../build/math-p45-20261009-192204-Focused/)|
|math-p45-20261009-192314-Focused|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-192314-Focused/)|
|math-p45-20261009-192314-Focused|focused|0|57 件、失敗 0 件|[生証拠](../build/math-p45-20261009-192314-Focused/)|
|math-p45-20261009-192314-Focused|encode|1|5 件、失敗 1 件|[生証拠](../build/math-p45-20261009-192314-Focused/)|
|math-p45-20261009-192832-Focused|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-192832-Focused/)|
|math-p45-20261009-192832-Focused|focused|0|58 件、失敗 0 件|[生証拠](../build/math-p45-20261009-192832-Focused/)|
|math-p45-20261009-192832-Focused|encode|1|7 件、失敗 2 件|[生証拠](../build/math-p45-20261009-192832-Focused/)|
|math-p45-20261009-193053-Focused|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-193053-Focused/)|
|math-p45-20261009-193053-Focused|focused|0|58 件、失敗 0 件|[生証拠](../build/math-p45-20261009-193053-Focused/)|
|math-p45-20261009-193053-Focused|encode|1|7 件、失敗 1 件|[生証拠](../build/math-p45-20261009-193053-Focused/)|
|math-p45-20261009-193454-Lint|lint|0|静的検査通過|[生証拠](../build/math-p45-20261009-193454-Lint/)|
|math-p45-20261009-193457-Focused|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-193457-Focused/)|
|math-p45-20261009-193457-Focused|focused|0|58 件、失敗 0 件|[生証拠](../build/math-p45-20261009-193457-Focused/)|
|math-p45-20261009-193457-Focused|encode|1|8 件、失敗 1 件|[生証拠](../build/math-p45-20261009-193457-Focused/)|
|math-p45-20261009-193712-Dependencies|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-193712-Dependencies/)|
|math-p45-20261009-193712-Dependencies|dependencies|0|59 件、失敗 0 件|[生証拠](../build/math-p45-20261009-193712-Dependencies/)|
|math-p45-20261009-193823-Lint|lint|0|静的検査通過|[生証拠](../build/math-p45-20261009-193823-Lint/)|
|math-p45-20261009-193929-Controller|build|0|該当する集計なし|[生証拠](../build/math-p45-20261009-193929-Controller/)|
|math-p45-20261009-193929-Controller|inventory|0|該当する集計なし|[生証拠](../build/math-p45-20261009-193929-Controller/)|
|math-p45-20261009-193929-Controller|controller|0|1 件、失敗 0 件|[生証拠](../build/math-p45-20261009-193929-Controller/)|

## 画素の失敗と独立対照

Graph の静止 endpoint の straight RGBA は `[200,99,31,128]`。黒い不透明背景との最近接整数 source-over の期待値と、実際の MLT 合成後／YUV 変換前の byte を比較する。

|取得|期待 RGBA|実測 RGBA|
|---|---|---|
|math-p45-20261009-192832-Focused|`[100,50,16,255]`|`[100,49,15,254]`|
|math-p45-20261009-193053-Focused|`[100,50,16,255]`|`[100,49,15,254]`|
|math-p45-20261009-193457-Focused|`[100,50,16,255]`|`[100,49,15,254]`|

通常 Image の対照は同じ straight PNG を使い、Graph の compile／成果物 loader／source timing を通さない。対照の全 frame が Graph endpoint と全画素一致した assertion は encode.log に残す。Graph の画素問題を「無関係」として除外する根拠には使わない。

既存 MLT 7.36.1 の [interp.h](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/plus/interp.h) は float の source-over を uint8_t へ切り捨てる。Graph artifact 内の凍結した最近接整数丸めと、外側の既存 MLT 合成は異なる。Graph だけの別合成や許容差を追加せず、既存の丸めを保存したため、この oracle は未通過である。

## ソースの来歴

- math-p45-20261009-192204-Focused: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `683D0A2BD1D84AEEED25FF32695042CB26A774BCE761618F42257285CCC3C571`。各ファイルは [source-state.json](../build/math-p45-20261009-192204-Focused/source-state.json) と `sources/` に固定。
- math-p45-20261009-192314-Focused: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `509C932900C3BEED7E5A20519F87EBE30E098AD5C929A28723DFB55E49EAE5D5`。各ファイルは [source-state.json](../build/math-p45-20261009-192314-Focused/source-state.json) と `sources/` に固定。
- math-p45-20261009-192832-Focused: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `BDA9F15C0F2FBFB22DEEE17DD1FC1EBA5A4423958BB1C986450F88484592BC59`。各ファイルは [source-state.json](../build/math-p45-20261009-192832-Focused/source-state.json) と `sources/` に固定。
- math-p45-20261009-193053-Focused: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `463CEDFDE9A9003AB9DFC44EBC6759411C1D3E7488514123BE8CE282E344E6C4`。各ファイルは [source-state.json](../build/math-p45-20261009-193053-Focused/source-state.json) と `sources/` に固定。
- math-p45-20261009-193454-Lint: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `E85DAEB6C984959B81E4241EFED84B25393C9893E79D89B8009D0D30D84CCF22`。各ファイルは [source-state.json](../build/math-p45-20261009-193454-Lint/source-state.json) と `sources/` に固定。
- math-p45-20261009-193457-Focused: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `E85DAEB6C984959B81E4241EFED84B25393C9893E79D89B8009D0D30D84CCF22`。各ファイルは [source-state.json](../build/math-p45-20261009-193457-Focused/source-state.json) と `sources/` に固定。
- math-p45-20261009-193712-Dependencies: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `B8C2687F2B4C4E377D13206EBE85A88D8932CD4ADD1413FA0576A0225F9A8C52`。各ファイルは [source-state.json](../build/math-p45-20261009-193712-Dependencies/source-state.json) と `sources/` に固定。
- math-p45-20261009-193823-Lint: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `B8C2687F2B4C4E377D13206EBE85A88D8932CD4ADD1413FA0576A0225F9A8C52`。各ファイルは [source-state.json](../build/math-p45-20261009-193823-Lint/source-state.json) と `sources/` に固定。
- math-p45-20261009-193929-Controller: HEAD `3014f87421f860b2a94a1b5b3799e29cacae3c2f`、source snapshot SHA256 `69DCA04EFB6EC6B40D124F84A71757D878CF22937E16796242BFD08154B914C3`。各ファイルは [source-state.json](../build/math-p45-20261009-193929-Controller/source-state.json) と `sources/` に固定。

## 初期失敗と未達条件

- 最初のビルドは試験の Track.visible が存在しないため失敗。製品の既存 Track.muted に試験を修正した。sandbox 内ビルドの停止は公式 build の sandbox 外実行で回避し、source／Ninja metadata の修復はしていない。
- 通常 Image の最初の対照は、Image に Graph data を残した試験 fixture の構造不正で失敗。Graph data を空にして新規取得し、過去の失敗 directory は保存した。
- 既存 controller 試験の sandbox 内実行は QTemporaryDir の初期保存で失敗。build/math-p45-editor-sandbox-failure に LastTest と失敗一覧を退避した。同じ source の公式 build に変更対象の再コンパイルは無く、sandbox 外では一件通過した。制限外でも同じ失敗が出ることは確認されていない。
- 共通 key の静止所有者と Draw 所有者は先に集約して、一つの Draw package だけを準備する。台帳と一枚の const RGBA owner は preview residency に依存しない。
- 最終 oracle が通過する合成 authority は未確定。実 Manim → 製品 QML → H.264、全取消境界、普通の音声／字幕との組合せ、全変異、BuildIndependent、通常 release は閉鎖していない。要求 output 範囲を持つ planner は実装したが、製品 exporter は従来どおり timeline 全体を出力する。
- コミット・push は行っていない。P4-5 の実装・閉鎖は未完了。
