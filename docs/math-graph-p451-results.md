# P4-5.1: 合成契約の検証結果

P4-5.1 composition authority: **PASS/CLOSED**。
P4-5 original focused blocker: **RESOLVED**。
P4-5 full product closure: **STILL OPEN**。P4/P4-5 は CLOSED にしない。

結論 A: artifact の最近接整数 oracle を post-MLT に適用した境界誤り。合成の production 処理は変更しない。
数式・source provenance・観測の限界は [契約文書](math-graph-p451.md) を参照する。

## alpha254 の独立演算 trace

|binary32 演算|独立計算値|
|---|---|
|as|0.50196081399917603|
|F(as+1)|1.5019607543945312|
|a|0.99999994039535522|
|F(255*a)|254.99998474121094|
|w|0.5019608736038208|
255*a の byte 切り捨て: 254。255/255 自体ではなく加算後の中間丸めが原因。

## 集中 gate

|段階|証拠 directory|生ログからの結果|
|---|---|---|
|diagnostic|`build/math-p451-20261010-004732-Focused`|検査 54、失敗 0|
|oracle|`build/math-p451-20261010-004732-Focused`|検査 215、失敗 0|
|differential|`build/math-p451-20261010-004732-Focused`|検査 1010、失敗 0|
|dependencies-loader|`build/math-p451-20261010-004732-Focused`|検査 59、失敗 0|
|original-encoder|`build/math-p451-20261010-004732-Focused`|検査 8、失敗 0|
|regressions|`build/math-p451-20261010-003557-Regressions`|9/9 PASS|
|lint|`build/math-p451-20261010-005158-Lint`|終了コード 0|

## 記録 fixture の各境界

|境界|RGBA|差分数|
|---|---|---|
|artifact-2|`[200,99,31,128]`|0|
|staging-2|`[200,99,31,128]`|0|
|producer|`[200,99,31,128]`|0|
|background|`[0,0,0,255]`|0|
|pre-source|`[200,99,31,128]`|0|
|pre-destination|`[0,0,0,255]`|0|
|post|`[100,49,15,254]`|0|
|pre-yuv-graph|`[100,49,15,254]`|0|
|pre-yuv-image|`[100,49,15,254]`|0|

最初の byte 差は affine 直後。実 staging を別途 qimage/affine に掛けた結果も製品 pre-YUV と一致する。
raw RGBA と TSV に全画素 SHA、oracle SHA、先頭差分 x/y/channel、差分数を保存した。
旧最近接整数 oracle `[100,50,16,255]` と post の比較: 先頭 byte=1（x=0/y=0/channel=1）、差分 byte 数=6912。

## 行列の endpoint

|条件|独立 oracle と一致した RGBA|Graph/Image 対照|
|---|---|---|
|black-0|`[0,0,0,255]`|全画素 exact|
|black-1|`[0,0,0,255]`|全画素 exact|
|black-128|`[100,49,15,254]`|全画素 exact|
|black-254|`[199,98,30,255]`|全画素 exact|
|black-255|`[200,99,31,255]`|全画素 exact|
|colored-0|`[23,71,143,255]`|全画素 exact|
|colored-1|`[23,71,142,255]`|全画素 exact|
|colored-128|`[111,85,86,254]`|全画素 exact|
|colored-254|`[199,98,31,255]`|全画素 exact|
|colored-255|`[200,99,31,255]`|全画素 exact|
|draw|`[100,49,15,254]`|全画素 exact|
|effects|`[50,24,7,255]`|全画素 exact|
|reordered|`[124,111,87,254]`|全画素 exact|
|three|`[80,41,202,254]`|全画素 exact|
|transparent-1|`[200,99,31,1]`|透明背景の controlled 診断のみ|
|transparent-128|`[200,99,31,128]`|透明背景の controlled 診断のみ|
|transparent-254|`[200,99,31,254]`|透明背景の controlled 診断のみ|
|transparent-255|`[200,99,31,255]`|透明背景の controlled 診断のみ|
|two|`[58,130,51,254]`|全画素 exact|

差分を持つ stage 行: 0。alpha0 同士の定義域外は支持ケースに含めない。

## MLT provenance

|ファイル|SHA256|
|---|---|
|`C:/msys64/ucrt64/bin/libmlt-7.dll`|`13D5EFA4536D322534112E51BD4AA45F585A3CC65A5E189B893CF9381A69D976`|
|`C:/msys64/ucrt64/lib/mlt/libmltplus.dll`|`1C6DA17405923A263F2427532F96AF4396C3BF58029FEC64146EDBDC7C956DF0`|
|`C:/msys64/ucrt64/lib/mlt/libmltqt6.dll`|`3476292ED034FF0BD776D014A1169B97BA818B00F73B4CDF52A4B967AD75095D`|
|`C:/msys64/ucrt64/lib/mlt/libmltcore.dll`|`0A4F93FA10E1DAF799356EB6507FF8DB15B05D3D27425E04E7F5E786CBC5A593`|
|`C:/msys64/ucrt64/lib/mlt/libmltavformat.dll`|`0FA0E99C0D06E2C32EDAFCB94AC1EA196B4153758E2F3E5C06AD9D64E5AD45D2`|
|`C:/msys64/var/lib/pacman/local/mingw-w64-ucrt-x86_64-mlt-7.36.1-1/desc`|`F6DFC0F833EA67EDF6F34C2AF1E7F42D41FA32ABC0AF9F27AE6304AFC12722B4`|
|`mlt-7.36.1-0001-cmake-fix-libs-install.patch`|`13E37F51782F25EBBD16DD2AB901E27C7DC594ACE4DBA3225D2304E22445B545`|
|`mlt-7.36.1-0002-cmake-install-manpage.patch`|`88DDBA2695D8C0CFB7E0B0BE01A3CAEB47C2BA2EA0677F489CC7DD2A7844885C`|
|`mlt-7.36.1-0003-cmake-fix-x86-detection.patch`|`500E518BDB3892DBDCEAB9E03E43A148B83C0000D3088F131D22B013B24FDA48`|
|`mlt-7.36.1-BUILDINFO`|`8D0A443994B74C308CA297B72388A85FF38CE7EF0EEF36FB6D89717052C7AD5C`|
|`mlt-7.36.1-consumer_avformat.c`|`22CB32424A18344B412CAFFB6C772EC41E5111109CC124A501C16D0320D56A3C`|
|`mlt-7.36.1-filter_affine.c`|`10FCB4C547D0B6DEDDE7EBC5BC96067EA8E3DCCF8E55199DE692B5C9A4D243C6`|
|`mlt-7.36.1-interp.h`|`D16BC2CE15334FDFFFA7C805DA4BF5C74C891316D6588E48926E13213DBC22AA`|
|`mlt-7.36.1-PKGBUILD`|`9787DCDC4B65E4162F1E77311656BB363BC206EEFCA3E95EE7DD66F27E3F26DF`|
|`mlt-7.36.1-producer_colour.c`|`3C8601EB91018173930935CE4CB929D0F48CE5841127E1B9A38D987DA0C53327`|
|`mlt-7.36.1-producer_qimage.c`|`7438ECA3B7850757B5D2764ADD587AB826BA53B730F968498C222808F26F1DEA`|
|`mlt-7.36.1-qimage_wrapper.cpp`|`01EB106BC0EB0104E37586D041B843752C1718FB1F34D74C1A4CDD81F267D58E`|
|`mlt-7.36.1-source.tar.gz`|`0D2B956864BA2FF58BB4E2B2779AA36870BD2A3A835E2DBFDA33FAA5FC6F4D3A`|
|`mlt-7.36.1-transition_affine.c`|`0D6810E61A488E65275F2A1DF0C0770B88D1ED109B0FD176805C2E0464D6D9BC`|
探索時の旧 recipe 履歴 JSON は取得履歴として保持し、正確な build authority は .BUILDINFO と SHA 一致する PKGBUILD/archive で固定する。

## Source 変異

|変異|build 終了|試験終了|検出|復元 SHA 一致|復元試験終了|
|---|---|---|---|---|---|
|source-frame|0|8|True|True|0|
|opaque-alpha|0|8|True|True|0|
|graph-image-divergence|0|8|True|True|0|
|alpha-interpretation|0|8|True|True|0|
|round-channel|0|8|True|True|0|
|layer-order|0|8|True|True|0|
|audit-format|0|8|True|True|0|
|background-alpha|0|8|True|True|0|

証拠: `build/math-p451-20261010-004405-Mutations`。元・変異・復元の SHA、変異 source、全 build/CTest ログを保持。

## 履歴と未実施範囲

P4-5 の旧整数 oracle FAIL と sandbox 起因の試験失敗は [旧結果](math-graph-p45-results.md) に保持する。
`build/math-p451-matrix-initial` は色背景を RGBA の順で MLT の ARGB resource へ渡し、540 検査中14失敗。元記録を残した。
初回診断の build は試験側の Track.id／GraphRenderSpec.functions の誤参照で失敗し、実際の型へ修正した。変異検出に数えない。
`build/math-p451-20261010-003741-Mutations` の初回 audit-format は上流 affine が RGBA に戻す等価変異で未検出だった。
`build/math-p451-20261010-004109-Mutations` の条件付き convert_image による YUV 往復変異も、今回の pipeline では byte を変えず未検出だった。
未検出を成功へ読み替えず、audit の attach 先を最終 producer から合成前の背景 producer へ移す実際の境界変異に変更した。
追加は staging 観測 callback と検証専用 MLT 診断、修正は独立 exact oracle。凍結 artifact oracle の期待値は変更していない。
実製品 UI→Manim→H.264、full release、BuildIndependent は未実施。P4-5 全体の閉鎖は別途必要。コミット・push は行っていない。
