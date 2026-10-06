#ifndef MVM_MEDIA_MATH_MATH_TEX_SEGMENTS_H
#define MVM_MEDIA_MATH_MATH_TEX_SEGMENTS_H

// 式から式への変形 (P2) の、TeX の式の自動の分け方と、部分どうしの照合。
//
// backend 中立な純粋な計算で、Manim の型・記法 (MathTex・{{ }} など) を含まない。
// 分け方と照合は派生の値で Project には保存しない。どちらかの規則を変えたら版を上げる
// (版は変形の cache key に入るので、古い artifact を引かない)。

#include <cstddef>
#include <string>
#include <vector>

namespace mvm::math {

// 分け方の規則の版。
//
// mvm-tex-segments/1:
// - 字句: `\` + 英字の並び、`\` + 1 文字 (`\{`・`\\`・`\%` など)、`%` から改行 (を含む) までの
//   comment、それ以外は 1 byte。
// - 深さ: `{`・`\left`・`\begin` で 1 増え、`}`・`\right`・`\end` で 1 減る。
// - 深さ 0 の次の字句の前後で分ける。
//   - 関係子 `=` `<` `>` `\le` `\ge` `\leq` `\geq` `\ne` `\neq` `\approx` `\equiv` は常に。
//   - 二項演算子 `+` `-` `\pm` `\mp` `\cdot` `\times` は、直前 (空白と comment を除く) が
//     項のときだけ (先頭・演算子・`(`・`[` の直後の単項の符号では分けない)。
// - `^`・`_`・`\not` の直後の字句 (空白と comment を除く) は引数とみなし、分けない。
// - 演算子の前後の空白は演算子の部分に含める。部分は空にならない。
// - 部分の文字列をすべて連結すると、入力と byte 単位で一致する (空白も comment も落とさない)。
inline constexpr const char* kMathTexSegmenterVersion = "mvm-tex-segments/1";

// 照合の規則の版。
//
// mvm-tex-match/1:
// - 部分の key (前後の空白を除いた文字列) が等しいものを対応させる。
// - 同じ key が重複するときは source の n 番目の出現 → target の n 番目の出現。
// - 対応しない source の部分は消え (fade-out)、対応しない target の部分は現れる (fade-in)。
inline constexpr const char* kMathTexMatchingVersion = "mvm-tex-match/1";

struct MathTexSegment {
    // 入力の連続した一部 (空白を含む)。連結すると入力に戻る。
    std::string text;
    // 照合に使う値。text の前後の ASCII 空白 (space・tab・CR・LF) を除いたもの。
    std::string key;
    bool operator==(const MathTexSegment&) const = default;
};

// 空の入力は 0 個の部分。空白だけの入力は key が空の 1 個の部分。
// 中括弧が釣り合わない式も分ける (深さ 0 に戻らない範囲では分けない)。描けるかどうかは見ない。
std::vector<MathTexSegment> segmentMathTex(const std::string& source);
// P2 の key 規則を semantic partition でも共有する。先頭末尾の ASCII 空白だけを除く。
std::string mathTexSegmentKey(const std::string& text);

struct MathSegmentPair {
    std::size_t source = 0; // source の部分の番号
    std::size_t target = 0; // target の部分の番号
    bool operator==(const MathSegmentPair&) const = default;
};

struct MathSegmentMatching {
    // source の番号の昇順。target の番号は昇順とは限らない (項の並べ替え)。
    std::vector<MathSegmentPair> pairs;
    std::vector<std::size_t> unmatchedSource; // 昇順。消える部分
    std::vector<std::size_t> unmatchedTarget; // 昇順。現れる部分
    bool operator==(const MathSegmentMatching&) const = default;
};

MathSegmentMatching matchMathTexSegments(const std::vector<MathTexSegment>& source,
                                         const std::vector<MathTexSegment>& target);

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_TEX_SEGMENTS_H
