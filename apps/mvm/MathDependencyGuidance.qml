import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 数式の描画 backend (Manim・MiKTeX) が使えないときの導入の案内。数式 clip と数式の変形の
// inspector が同じ文を出す (文を 2 箇所に書かない)。表示するかは使う側が unavailableReason で決める。
Label {
    Layout.fillWidth: true
    text: "Manim と MiKTeX を導入し、latex / dvisvgm が利用できる状態にして再試行してください。MiKTeX の不足パッケージは自動導入を有効にしてください。"
    color: "#aeb4bf"
    wrapMode: Text.Wrap
}
