#include "media/math/math_tex_segments.h"

#include <array>
#include <map>
#include <string_view>

namespace mvm::math {
namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 字句の終わり (begin の次の字句の先頭)。
std::size_t tokenEnd(const std::string& source, std::size_t begin) {
    const std::size_t size = source.size();
    if (source[begin] == '%') {
        std::size_t end = begin + 1;
        while (end < size && source[end] != '\n')
            ++end;
        return end < size ? end + 1 : end;
    }
    if (source[begin] != '\\')
        return begin + 1;
    std::size_t end = begin + 1;
    if (end >= size)
        return end; // 末尾の `\` だけ
    if (!isLetter(source[end]))
        return end + 1;
    while (end < size && isLetter(source[end]))
        ++end;
    return end;
}

template<std::size_t N>
bool isOneOf(std::string_view token, const std::array<std::string_view, N>& set) {
    for (const auto item : set)
        if (token == item)
            return true;
    return false;
}

constexpr std::array<std::string_view, 11> kRelations = {
    "=", "<", ">", "\\le", "\\ge", "\\leq", "\\geq", "\\ne", "\\neq", "\\approx", "\\equiv"};
constexpr std::array<std::string_view, 6> kBinaries = {"+",    "-",      "\\pm",
                                                       "\\mp", "\\cdot", "\\times"};
constexpr std::array<std::string_view, 3> kOpeners = {"{", "\\left", "\\begin"};
constexpr std::array<std::string_view, 3> kClosers = {"}", "\\right", "\\end"};
constexpr std::array<std::string_view, 3> kTakesArgument = {"^", "_", "\\not"};

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin]))
        ++begin;
    while (end > begin && isSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

// 直前の字句の種類 (空白と comment は数えない)。
enum class Previous { Start, Operator, Open, Term };

struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
};

} // namespace

std::string mathTexSegmentKey(const std::string& text) {
    return trimmed(text);
}

std::vector<MathTexSegment> segmentMathTex(const std::string& source) {
    const std::size_t size = source.size();
    std::vector<Range> cuts; // 演算子の字句
    int depth = 0;
    Previous previous = Previous::Start;
    bool argumentPending = false;
    for (std::size_t begin = 0; begin < size;) {
        const std::size_t end = tokenEnd(source, begin);
        const std::string_view token(source.data() + begin, end - begin);
        const std::size_t tokenBegin = begin;
        begin = end;
        if (token[0] == '%' || isSpace(token[0]))
            continue;
        if (isOneOf(token, kOpeners)) {
            ++depth;
            argumentPending = false;
            continue;
        }
        if (isOneOf(token, kClosers)) {
            --depth;
            previous = Previous::Term;
            continue;
        }
        if (depth != 0)
            continue;
        if (argumentPending) {
            argumentPending = false;
            previous = Previous::Term;
            continue;
        }
        if (isOneOf(token, kTakesArgument)) {
            argumentPending = true;
            previous = Previous::Term;
            continue;
        }
        if (isOneOf(token, kRelations) ||
            (isOneOf(token, kBinaries) && previous == Previous::Term)) {
            cuts.push_back({tokenBegin, end});
            previous = Previous::Operator;
        } else if (token == "(" || token == "[") {
            previous = Previous::Open;
        } else {
            previous = Previous::Term;
        }
    }

    std::vector<MathTexSegment> segments;
    auto push = [&](std::size_t from, std::size_t to) {
        if (from < to) {
            auto text = source.substr(from, to - from);
            auto key = mathTexSegmentKey(text);
            segments.push_back({std::move(text), std::move(key)});
        }
    };
    std::size_t position = 0;
    for (const auto& cut : cuts) {
        // 演算子の前後の空白を演算子の部分へ寄せる (前の演算子が取った分は越えない)。
        std::size_t from = cut.begin;
        while (from > position && isSpace(source[from - 1]))
            --from;
        std::size_t to = cut.end;
        while (to < size && isSpace(source[to]))
            ++to;
        push(position, from);
        push(from, to);
        position = to;
    }
    push(position, size);
    return segments;
}

MathSegmentMatching matchMathTexSegments(const std::vector<MathTexSegment>& source,
                                         const std::vector<MathTexSegment>& target) {
    // key ごとの target の出現 (昇順)。
    std::map<std::string, std::vector<std::size_t>> targetOccurrences;
    for (std::size_t j = 0; j < target.size(); ++j)
        targetOccurrences[target[j].key].push_back(j);

    MathSegmentMatching matching;
    std::map<std::string, std::size_t> seen; // key ごとの source の出現数
    std::vector<bool> targetUsed(target.size(), false);
    for (std::size_t i = 0; i < source.size(); ++i) {
        const std::size_t nth = seen[source[i].key]++;
        const auto found = targetOccurrences.find(source[i].key);
        if (found != targetOccurrences.end() && nth < found->second.size()) {
            const std::size_t j = found->second[nth];
            matching.pairs.push_back({i, j});
            targetUsed[j] = true;
        } else {
            matching.unmatchedSource.push_back(i);
        }
    }
    for (std::size_t j = 0; j < target.size(); ++j)
        if (!targetUsed[j])
            matching.unmatchedTarget.push_back(j);
    return matching;
}

} // namespace mvm::math
