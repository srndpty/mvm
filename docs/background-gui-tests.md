# 通常 GUI テストの背面実行

【操作可】`pwsh scripts/test.ps1` または `./dev.ps1 test` の実行中も、通常の PC 操作を続けられる。

GUI テストは `src/app/preview/test_window_mode.h` の `testBackgroundWindowFlags()` を表示前に
設定する。背面への配置、フォーカスの拒否、マウス入力の透過、タスクバーからの除外を共通化している。
非アクティブなウィンドウでも Z 順の上に表示されるため、フォーカスの拒否だけでは不十分である。

固定サイズの検証アプリは、従来どおり `prepareTestFixedWindowEnvironment()` と
`applyTestFixedWindow()` を使う。QML の `visible: true` はロード中に表示されるので、
`testFixedWindowInitialProperties()` をロード前に渡す。`QQmlComponent` から直接生成する
場合も `createWithInitialProperties()` で flags を渡す。生成後に背面へ下げる方法では、
最初の表示が利用者の作業を遮る。

プレビューの engine と controller も `show()` より前に接続する。表示により render thread が
起動しうるため、表示後に設定する順序では初期化と設定が競合する。

キーボード入力の試験は `tests/harness/test_window_focus.h` の `focusWithoutForeground()` で
Qt 内部のフォーカスを設定し、QTest の合成イベントを直接送る。OS の前面は変更しない。
`isolatedFromUserInput()` は実 HWND の非アクティブ・透過・非最前面の設定と、作業中の
前面ウィンドウより背後にあることも検査する。

`fixed_test_window_independent_of_screen` は C++ と QML の両方で、背面配置を維持しながら
描画と画素の読み戻しができることを検査する。背面指定が欠落した対照を拒否することも確かめる。
画面外への移動・最小化・非表示で描画検査を省略しない。

トランジションと音声のみの再生の待ち処理は `tests/harness/test_event_wait.h` に共通化する。
条件成立後にさらにイベントを処理して再判定しない。初回 seek で ready が一時的に戻っても、
既に観測した成立を取り消さない。固定ウィンドウの回帰検査には、その状態変更を再現する対照も含む。
初回提示が必要な検査は、ready と最新の提示の両方を満たすまで待つ。cache 更新中の seek は、
既存の受理待ちヘルパーを使う。

物理画面・DWM・提示経路自体を観測する正式計測は、背面化すると測定条件が変わる。
通常テストからは `performance|stability` ラベルで分離し、それぞれの計測手順に従う。
