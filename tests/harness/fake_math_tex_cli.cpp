// 数式 backend "manim-mathtex" の試験用の偽 Manim / latex / dvisvgm。
//
// 自分の file 名 (拡張子を除く) で振る舞いを変える。
//   latex / dvisvgm      --version に版を 1 行出す (標準エラーには MiKTeX の催促に似た雑音)
//   それ以外 (Manim 役)  --version に版を出す。render では mvm の scene template の契約
//                        (request.json を読み、info.txt と PNG、失敗時は error-kind.txt /
//                        error.txt と TeX の log) を真似る。振る舞いは request.json に含まれる
//                        印で選ぶ:
//     FAKE_LATEX_ERROR   TeX の error (exit 3)
//     FAKE_OTHER_ERROR   TeX 以外の error (exit 3)
//     FAKE_HANG          終わらない
//     FAKE_NO_OUTPUT     PNG を書かずに 0 で終わる
//     FAKE_TWO_OUTPUTS   PNG を 2 つ書く
//     (印なし)           3x2 の白い glyph の PNG を書く
//   scene が MvmMathWrite (Write の連番) なら、request.json の intro_frames 枚の PNG を
//   <Scene>0000.png から書く (書く順は逆にして、名前の順で並べることを確かめる)。
//     FAKE_WRITE_SHORT   1 枚少なく書く
//   scene が MvmMathTransform (式から式への変形) なら、request.json の canvas に frames + 1 枚を
//   偽の被覆率の画像 (math_test_transform.h) で書く。frame 0 は source の静止の模様、
//   最後は target の静止の模様を各 placement_px に置く。structure.txt は segments をそのまま
//   部分として報告する。印 (式に含める):
//     FAKE_FALLBACK        代用の log を報告し、部分を式全体の group にして exit 1
//     FAKE_PART_COUNT      target の最後の部分を報告しない
//     FAKE_PART_TEXT       source の最初の部分の文字列を変えて報告する
//     FAKE_NO_STRUCTURE    structure.txt を書かない
//     FAKE_INFO_SIZE       info.txt の canvas の幅を 1 増やす
//     FAKE_MISSING_FRAME   1 枚少なく書く      FAKE_EXTRA_FRAME   1 枚多く書く
//     FAKE_MALFORMED_FRAME frame 1 を読めない内容にする
//     FAKE_FRAME_SIZE      frame 1 の幅を 1 減らす
//     FAKE_EDGE            frame 1 の左端 (0, 高さ/2) に alpha を置く
//     FAKE_SPREAD          frame 1 の (10, 12) に alpha を置く (端点の外)
//     FAKE_FIRST_MISMATCH  frame 0 の 1 画素を変える
//     FAKE_LAST_MISMATCH   終状態の模様を 1 画素右へずらす
//   scene が MvmEquationSequence (P3-3) なら、phase ごとに structure.txt と区間の PNG を書く
//   (renderEquationSequence の注釈)。印 (式に含める):
//     FAKE_EQ_NO_REPORT / FAKE_EQ_MALFORMED / FAKE_EQ_FALLBACK  報告なし・壊れた行・代用の log
//     FAKE_EQ_COUNT      状態 0 の部分を 1 個少なく報告する
//     FAKE_EQ_MISSING    状態 0 の部分 0 の行を書かない
//     FAKE_EQ_TYPE / FAKE_EQ_TEXT  状態 0 の部分 0 の種類・文字列を変える
//     FAKE_EQ_SHARED     状態 0 の部分 1 が部分 0 の子孫 object を報告する
//     FAKE_EQ_ALIAS      状態 0 の 2 個の子孫が同じ点列を共有する
//     FAKE_EQ_UNCLAIMED  状態 0 の式にどの部分にも属さない子孫を足す
//     FAKE_EQ_EMPTY (と \,)  その segment は点を持つ子孫が 0 個
//     FAKE_EQ_PHASE_DIFF 描画の段階だけ状態 0 の部分 0 に子孫を足す
//     FAKE_EQ_SHORT / FAKE_EQ_CORRUPT / FAKE_EQ_SIZE  変形 0 の枚数・frame 1 の中身・幅
//     FAKE_EQ_STATIC / FAKE_EQ_ENDPOINT / FAKE_EQ_AFTER / FAKE_EQ_EDGE  静止・終状態・
//                        action の後・action frame 0 の縁を壊す
//     FAKE_EQ_RENDER_HANG / FAKE_EQ_RENDER_EXIT  描画の段階だけ終わらない・exit 3

#include "math_test_png.h"
#include "math_test_transform.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
}

void writePng(const std::filesystem::path& path) {
    writeFile(path, mvm::test::mathTestPngBytes());
}

bool has(const std::string& request, const char* marker) {
    return request.find(marker) != std::string::npos;
}

// from 以降で最初の "key": の後の整数を順に読む ("key": 3 と "key": [3, 4] の両方)。
std::vector<long long> numbersAfter(const std::string& text, std::size_t from, const char* key,
                                    std::size_t count) {
    std::vector<long long> numbers;
    const std::string pattern = std::string("\"") + key + "\": ";
    auto at = text.find(pattern, from);
    if (at == std::string::npos)
        return numbers;
    at += pattern.size();
    while (numbers.size() < count && at < text.size()) {
        while (at < text.size() && (text[at] == '[' || text[at] == ' ' || text[at] == ','))
            ++at;
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str() + at, &end, 10);
        if (end == text.c_str() + at)
            break;
        numbers.push_back(value);
        at = static_cast<std::size_t>(end - text.c_str());
    }
    return numbers;
}

// from 以降の "segments": [...] の JSON 文字列を戻す (mvm が書く escape だけを扱う)。
std::vector<std::string> segmentsAfter(const std::string& text, std::size_t from) {
    std::vector<std::string> segments;
    const std::string pattern = "\"segments\": [";
    auto at = text.find(pattern, from);
    if (at == std::string::npos)
        return segments;
    at += pattern.size();
    while (at < text.size() && text[at] != ']') {
        if (text[at] != '"') {
            ++at;
            continue;
        }
        std::string value;
        for (++at; at < text.size() && text[at] != '"'; ++at) {
            if (text[at] != '\\') {
                value += text[at];
                continue;
            }
            ++at;
            if (text[at] == 'u') {
                value +=
                    static_cast<char>(std::strtol(text.substr(at + 1, 4).c_str(), nullptr, 16));
                at += 4;
            } else {
                value += text[at];
            }
        }
        ++at;
        segments.push_back(value);
    }
    return segments;
}

std::string hexText(const std::string& text) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex = "x";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        hex += kHex[c >> 4];
        hex += kHex[c & 0x0F];
    }
    return hex;
}

// 被覆率の canvas に端点の静止の模様を置く。
void drawEndpoint(std::vector<std::uint8_t>& alpha, int canvasWidth, bool target, int left, int top,
                  int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            alpha[static_cast<std::size_t>(top + y) * static_cast<std::size_t>(canvasWidth) +
                  static_cast<std::size_t>(left + x)] =
                mvm::test::mathTestEndpointAlpha(target, x, y);
}

int renderTransform(const std::filesystem::path& job, const std::filesystem::path& media,
                    const std::string& request) {
    const auto frames = numbersAfter(request, 0, "frames", 1);
    const auto canvas = numbersAfter(request, 0, "canvas_px", 2);
    const auto sourceAt = request.find("\"source\": {");
    const auto targetAt = request.find("\"target\": {");
    if (frames.size() != 1 || canvas.size() != 2 || sourceAt == std::string::npos ||
        targetAt == std::string::npos)
        return 23;
    const auto sourceSize = numbersAfter(request, sourceAt, "static_px", 2);
    const auto sourcePlace = numbersAfter(request, sourceAt, "placement_px", 2);
    const auto targetSize = numbersAfter(request, targetAt, "static_px", 2);
    const auto targetPlace = numbersAfter(request, targetAt, "placement_px", 2);
    if (sourceSize.size() != 2 || sourcePlace.size() != 2 || targetSize.size() != 2 ||
        targetPlace.size() != 2)
        return 24;
    const int width = static_cast<int>(canvas[0]);
    const int height = static_cast<int>(canvas[1]);

    if (!has(request, "FAKE_NO_STRUCTURE")) {
        if (has(request, "FAKE_FALLBACK")) {
            // 実 Manim は代用した group では部分の番号で組む変形が IndexError になる。
            writeFile(job / L"structure.txt",
                      "fallback " +
                          hexText("MathTex: Could not find SVG group for tex part (id: "
                                  "unique000). Using fallback to root group.") +
                          "\npart source VGroup none 9\n");
            std::fputs("IndexError: list index out of range\n", stderr);
            return 1;
        }
        auto sourceSegments = segmentsAfter(request, sourceAt);
        auto targetSegments = segmentsAfter(request, targetAt);
        if (has(request, "FAKE_PART_TEXT") && !sourceSegments.empty())
            sourceSegments.front() += "?";
        if (has(request, "FAKE_PART_COUNT") && !targetSegments.empty())
            targetSegments.pop_back();
        std::string report;
        for (const auto& segment : sourceSegments)
            report += "part source MathTexPart " + hexText(segment) + " 1\n";
        for (const auto& segment : targetSegments)
            report += "part target MathTexPart " + hexText(segment) + " 1\n";
        writeFile(job / L"structure.txt", report);
    }
    writeFile(job / L"info.txt", std::to_string(width + (has(request, "FAKE_INFO_SIZE") ? 1 : 0)) +
                                     " " + std::to_string(height));

    long long count = frames[0] + 1;
    if (has(request, "FAKE_MISSING_FRAME"))
        --count;
    if (has(request, "FAKE_EXTRA_FRAME"))
        ++count;
    const auto images = media / L"images" / L"mvm_math_tex";
    for (long long index = count - 1; index >= 0; --index) {
        wchar_t name[64] = {};
        std::swprintf(name, std::size(name), L"MvmMathTransform%04lld.png", index);
        const auto path = images / name;
        if (index == 1 && has(request, "FAKE_MALFORMED_FRAME")) {
            writeFile(path, "not a coverage image");
            continue;
        }
        const int frameWidth = width - (index == 1 && has(request, "FAKE_FRAME_SIZE") ? 1 : 0);
        std::vector<std::uint8_t> alpha(
            static_cast<std::size_t>(frameWidth) * static_cast<std::size_t>(height), 0);
        if (index >= frames[0])
            drawEndpoint(alpha, frameWidth, true,
                         static_cast<int>(targetPlace[0]) +
                             (has(request, "FAKE_LAST_MISMATCH") ? 1 : 0),
                         static_cast<int>(targetPlace[1]), static_cast<int>(targetSize[0]),
                         static_cast<int>(targetSize[1]));
        else
            drawEndpoint(alpha, frameWidth, false, static_cast<int>(sourcePlace[0]),
                         static_cast<int>(sourcePlace[1]), static_cast<int>(sourceSize[0]),
                         static_cast<int>(sourceSize[1]));
        const auto at = [&](int x, int y) -> std::uint8_t& {
            return alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(frameWidth) +
                         static_cast<std::size_t>(x)];
        };
        if (index == 0 && has(request, "FAKE_FIRST_MISMATCH"))
            at(static_cast<int>(sourcePlace[0]) + 1, static_cast<int>(sourcePlace[1]) + 1) ^= 0x40;
        if (index == 1 && has(request, "FAKE_EDGE"))
            at(0, height / 2) = 1;
        if (index == 1 && has(request, "FAKE_SPREAD"))
            at(10, 12) = 200;
        writeFile(path, mvm::test::mathTestCoverageBytes(frameWidth, height, alpha));
    }
    std::puts("fake Manim: wrote transform");
    return 0;
}

// ---- Equation Sequence (P3-3) ----
//
// request.json の phase ("structure" / "render") を job の phases.txt へ 1 行ずつ追記する
// (試験が「構造の失敗では描画の段階を起動しない」ことを確かめる)。
// structure.txt: 各 segment は点を持つ子孫 1 個 (id は状態と番号から決める 16 進)。
// 文字列が \, か FAKE_EQ_EMPTY を含む segment は 0 個 (実 Manim の空の部分を真似る)。
// render: s<i>.png は状態 i の静止の模様 (mathTestEquationAlpha) を placement_px に置く。
// t<k>/ は N+1 枚 (frame 0 は前の状態、1 以降は後の状態の模様)。a<k>/ は base (状態の模様)・
// N 枚 (frame i は (2 + i % 3, 3) の 1 画素)・after (状態の模様)。

struct FakeEqItem {
    std::size_t at = 0; // request の中の位置
    std::vector<long long> numbers;
};

// region [from, to) の中の pattern の出現位置。
std::vector<std::size_t> occurrences(const std::string& text, const std::string& pattern,
                                     std::size_t from, std::size_t to) {
    std::vector<std::size_t> found;
    for (auto at = text.find(pattern, from); at != std::string::npos && at < to;
         at = text.find(pattern, at + 1))
        found.push_back(at);
    return found;
}

std::string fakeEqId(std::size_t state, std::size_t index) {
    char text[32] = {};
    std::snprintf(text, sizeof text, "%zx", (state + 1) * 0x1000 + index + 1);
    return text;
}

std::string fakeEqBuffer(std::size_t state, std::size_t index) {
    char text[32] = {};
    std::snprintf(text, sizeof text, "%zx", (state + 1) * 0x100000 + index + 1);
    return text;
}

std::string joinIds(const std::vector<std::string>& ids) {
    if (ids.empty())
        return "-";
    std::string text;
    for (std::size_t i = 0; i < ids.size(); ++i)
        text += (i > 0 ? "," : "") + ids[i];
    return text;
}

void drawEquationState(std::vector<std::uint8_t>& alpha, int canvasWidth, std::size_t state,
                       int left, int top, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            alpha[static_cast<std::size_t>(top + y) * static_cast<std::size_t>(canvasWidth) +
                  static_cast<std::size_t>(left + x)] =
                mvm::test::mathTestEquationAlpha(state, x, y);
}

void writeCoverage(const std::filesystem::path& path, int width, int height,
                   const std::vector<std::uint8_t>& alpha) {
    writeFile(path, mvm::test::mathTestCoverageBytes(width, height, alpha));
}

int renderEquationSequence(const std::filesystem::path& job, const std::string& request) {
    const bool render = has(request, "\"phase\": \"render\"");
    {
        std::ofstream phases(job / L"phases.txt", std::ios::binary | std::ios::app);
        phases << (render ? "render" : "structure") << "\n";
    }
    if (render && has(request, "FAKE_EQ_RENDER_HANG")) {
        Sleep(INFINITE);
        return 21;
    }
    if (render && has(request, "FAKE_EQ_RENDER_EXIT")) {
        writeFile(job / L"error-kind.txt", "other");
        writeFile(job / L"error.txt", "fake の描画の段階の失敗");
        return 3;
    }
    const auto statesAt = request.find("\"states\": [");
    const auto transitionsAt = request.find("\"transitions\": [");
    const auto actionsAt = request.find("\"actions\": [");
    if (statesAt == std::string::npos || transitionsAt == std::string::npos ||
        actionsAt == std::string::npos)
        return 30;
    const auto stateStarts = occurrences(request, "{\"segments\": [", statesAt, transitionsAt);

    // 構造の報告。
    if (!has(request, "FAKE_EQ_NO_REPORT")) {
        std::string report;
        if (has(request, "FAKE_EQ_FALLBACK"))
            report += "fallback " +
                      hexText("MathTex: Could not find SVG group for tex part (id: unique000). "
                              "Using fallback to root group.") +
                      "\n";
        for (std::size_t s = 0; s < stateStarts.size(); ++s) {
            const auto segments = segmentsAfter(request, stateStarts[s]);
            std::vector<std::vector<std::string>> owned(segments.size());
            std::vector<std::vector<std::string>> buffers(segments.size());
            std::vector<std::string> leaves;
            std::vector<std::string> leafBuffers;
            for (std::size_t k = 0; k < segments.size(); ++k) {
                const bool empty =
                    segments[k] == "\\," || segments[k].find("FAKE_EQ_EMPTY") != std::string::npos;
                if (empty)
                    continue;
                owned[k].push_back(fakeEqId(s, k));
                buffers[k].push_back(fakeEqBuffer(s, k));
                leaves.push_back(fakeEqId(s, k));
                leafBuffers.push_back(fakeEqBuffer(s, k));
            }
            if (s == 0 && render && has(request, "FAKE_EQ_PHASE_DIFF") && !segments.empty()) {
                owned[0].push_back("eeee");
                buffers[0].push_back("eeeeee");
                leaves.push_back("eeee");
                leafBuffers.push_back("eeeeee");
            }
            if (s == 0 && has(request, "FAKE_EQ_SHARED") && owned.size() > 1 && !owned[0].empty() &&
                !owned[1].empty()) {
                owned[1][0] = owned[0][0];
                buffers[1][0] = buffers[0][0];
            }
            if (s == 0 && has(request, "FAKE_EQ_ALIAS") && leafBuffers.size() > 1) {
                leafBuffers[1] = leafBuffers[0];
                if (owned.size() > 1 && !buffers[1].empty())
                    buffers[1][0] = leafBuffers[0];
            }
            if (s == 0 && has(request, "FAKE_EQ_UNCLAIMED")) {
                leaves.push_back("fff");
                leafBuffers.push_back("ffffff");
            }
            std::size_t parts = segments.size();
            if (s == 0 && has(request, "FAKE_EQ_COUNT"))
                --parts;
            report += "state " + std::to_string(s) + " " + std::to_string(parts) + " " +
                      std::to_string(leaves.size()) + " " + joinIds(leaves) + " " +
                      joinIds(leafBuffers) + "\n";
            if (s == 0 && has(request, "FAKE_EQ_MALFORMED"))
                report += "part 0 0 MathTexPart\n";
            for (std::size_t k = 0; k < parts; ++k) {
                if (s == 0 && k == 0 && has(request, "FAKE_EQ_MISSING"))
                    continue;
                std::string type = "MathTexPart";
                std::string text = segments[k];
                if (s == 0 && k == 0 && has(request, "FAKE_EQ_TYPE"))
                    type = "VGroup";
                if (s == 0 && k == 0 && has(request, "FAKE_EQ_TEXT"))
                    text += "?";
                report += "part " + std::to_string(s) + " " + std::to_string(k) + " " + type + " " +
                          hexText(text) + " " + std::to_string(owned[k].size()) + " " +
                          std::to_string(owned[k].size()) + " " + std::to_string(owned[k].size()) +
                          " " + joinIds(owned[k]) + " " + joinIds(buffers[k]) + "\n";
            }
        }
        writeFile(job / L"structure.txt", report);
    }
    writeFile(job / L"info.txt", "16 16");
    if (!render) {
        std::puts("fake Manim: wrote equation structure");
        return 0;
    }

    const auto output = job / L"sequence";

    struct StateBox {
        int width = 0;
        int height = 0;
    };

    std::vector<StateBox> boxes;
    for (std::size_t s = 0; s < stateStarts.size(); ++s) {
        const auto canvas = numbersAfter(request, stateStarts[s], "canvas_px", 2);
        const auto still = numbersAfter(request, stateStarts[s], "static_px", 2);
        const auto place = numbersAfter(request, stateStarts[s], "placement_px", 2);
        if (canvas.size() != 2 || still.size() != 2 || place.size() != 2)
            return 31;
        boxes.push_back({static_cast<int>(still[0]), static_cast<int>(still[1])});
        const int w = static_cast<int>(canvas[0]);
        const int h = static_cast<int>(canvas[1]);
        std::vector<std::uint8_t> alpha(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                                        0);
        drawEquationState(alpha, w, s, static_cast<int>(place[0]), static_cast<int>(place[1]),
                          boxes[s].width, boxes[s].height);
        if (s == 0 && has(request, "FAKE_EQ_STATIC"))
            alpha[static_cast<std::size_t>(place[1] + 1) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(place[0] + 1)] ^= 0x40;
        writeCoverage(output / (L"s" + std::to_wstring(s) + L".png"), w, h, alpha);
    }
    const auto transitionStarts = occurrences(request, "{\"from\": ", transitionsAt, actionsAt);
    for (std::size_t t = 0; t < transitionStarts.size(); ++t) {
        const auto at = transitionStarts[t];
        const auto from = numbersAfter(request, at, "from", 1);
        const auto to = numbersAfter(request, at, "to", 1);
        const auto canvas = numbersAfter(request, at, "canvas_px", 2);
        const auto sourcePlace = numbersAfter(request, at, "source_placement_px", 2);
        const auto targetPlace = numbersAfter(request, at, "target_placement_px", 2);
        const auto end = t + 1 < transitionStarts.size() ? transitionStarts[t + 1] : actionsAt;
        const auto alphaAt = request.find("\"alpha\": [", at);
        if (from.size() != 1 || to.size() != 1 || canvas.size() != 2 || sourcePlace.size() != 2 ||
            targetPlace.size() != 2 || alphaAt == std::string::npos || alphaAt > end)
            return 32;
        // alpha の [分子, 分母] の数 = N + 1。
        const auto count = static_cast<long long>(
            occurrences(request, "[", alphaAt + 10, request.find("]]", alphaAt) + 1).size());
        const int w = static_cast<int>(canvas[0]);
        const int h = static_cast<int>(canvas[1]);
        long long frames = count;
        if (t == 0 && has(request, "FAKE_EQ_SHORT"))
            --frames;
        for (long long i = 0; i < frames; ++i) {
            wchar_t name[32] = {};
            std::swprintf(name, std::size(name), L"%05lld.png", i);
            const auto path = output / (L"t" + std::to_wstring(t)) / name;
            if (t == 0 && i == 1 && has(request, "FAKE_EQ_CORRUPT")) {
                writeFile(path, "not a coverage image");
                continue;
            }
            const int frameWidth = w - (t == 0 && i == 1 && has(request, "FAKE_EQ_SIZE") ? 1 : 0);
            std::vector<std::uint8_t> alpha(
                static_cast<std::size_t>(frameWidth) * static_cast<std::size_t>(h), 0);
            const bool last = i == count - 1;
            if (i == 0)
                drawEquationState(alpha, frameWidth, static_cast<std::size_t>(from[0]),
                                  static_cast<int>(sourcePlace[0]),
                                  static_cast<int>(sourcePlace[1]),
                                  boxes[static_cast<std::size_t>(from[0])].width,
                                  boxes[static_cast<std::size_t>(from[0])].height);
            else
                drawEquationState(alpha, frameWidth, static_cast<std::size_t>(to[0]),
                                  static_cast<int>(targetPlace[0]) +
                                      (last && has(request, "FAKE_EQ_ENDPOINT") ? 1 : 0),
                                  static_cast<int>(targetPlace[1]),
                                  boxes[static_cast<std::size_t>(to[0])].width,
                                  boxes[static_cast<std::size_t>(to[0])].height);
            writeCoverage(path, frameWidth, h, alpha);
        }
    }
    const auto actionStarts = occurrences(request, "{\"state\": ", actionsAt, request.size());
    for (std::size_t a = 0; a < actionStarts.size(); ++a) {
        const auto at = actionStarts[a];
        const auto state = numbersAfter(request, at, "state", 1);
        const auto canvas = numbersAfter(request, at, "canvas_px", 2);
        const auto place = numbersAfter(request, at, "placement_px", 2);
        const auto alphaAt = request.find("\"alpha\": [", at);
        if (state.size() != 1 || canvas.size() != 2 || place.size() != 2 ||
            alphaAt == std::string::npos)
            return 33;
        const auto count = static_cast<long long>(
            occurrences(request, "[", alphaAt + 10, request.find("]]", alphaAt) + 1).size());
        const int w = static_cast<int>(canvas[0]);
        const int h = static_cast<int>(canvas[1]);
        const auto folder = output / (L"a" + std::to_wstring(a));
        const auto& box = boxes[static_cast<std::size_t>(state[0])];
        std::vector<std::uint8_t> still(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                                        0);
        drawEquationState(still, w, static_cast<std::size_t>(state[0]), static_cast<int>(place[0]),
                          static_cast<int>(place[1]), box.width, box.height);
        writeCoverage(folder / L"base.png", w, h, still);
        for (long long i = 0; i < count; ++i) {
            std::vector<std::uint8_t> alpha(
                static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            alpha[static_cast<std::size_t>(3) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(2 + i % 3)] = 255;
            if (a == 0 && i == 0 && has(request, "FAKE_EQ_EDGE"))
                alpha[static_cast<std::size_t>(h / 2) * static_cast<std::size_t>(w)] = 1;
            wchar_t name[32] = {};
            std::swprintf(name, std::size(name), L"%05lld.png", i);
            writeCoverage(folder / name, w, h, alpha);
        }
        auto after = still;
        if (a == 0 && has(request, "FAKE_EQ_AFTER"))
            after[static_cast<std::size_t>(place[1] + 1) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(place[0] + 1)] ^= 0x40;
        writeCoverage(folder / L"after.png", w, h, after);
    }
    std::puts("fake Manim: wrote equation sequence");
    return 0;
}

int render(const std::filesystem::path& script, const std::filesystem::path& media,
           const std::string& scene, bool lastFrameOnly) {
    const auto job = script.parent_path();
    const std::string request = readFile(job / L"request.json");
    if (request.empty())
        return 20;
    if (request.find("FAKE_HANG") != std::string::npos) {
        Sleep(INFINITE);
        return 21;
    }
    if (request.find("FAKE_LATEX_ERROR") != std::string::npos) {
        writeFile(media / L"Tex" / L"0123abcd.log", "This is pdfTeX (fake)\n"
                                                    "! Undefined control sequence.\n"
                                                    "<argument> ...\\fracc \n"
                                                    "l.9 ...\n");
        writeFile(job / L"error-kind.txt", "latex");
        writeFile(job / L"error.txt", "latex error converting to dvi. See log output above");
        std::fputs("ValueError: latex error converting to dvi\n", stderr);
        return 3;
    }
    if (request.find("FAKE_OTHER_ERROR") != std::string::npos) {
        writeFile(job / L"error-kind.txt", "other");
        writeFile(job / L"error.txt", "fake の別の失敗\n2 行目は使わない");
        return 3;
    }
    if (scene == "MvmMathTransform")
        return renderTransform(job, media, request);
    if (scene == "MvmEquationSequence")
        return renderEquationSequence(job, request);
    writeFile(job / L"info.txt", "3 2");
    if (request.find("FAKE_NO_OUTPUT") != std::string::npos)
        return 0;
    const auto images = media / L"images" / L"mvm_math_tex";
    if (scene == "MvmMathWrite") {
        const auto at = request.find("\"intro_frames\": ");
        if (at == std::string::npos)
            return 22;
        int frames = std::atoi(request.c_str() + at + std::string("\"intro_frames\": ").size());
        if (request.find("FAKE_WRITE_SHORT") != std::string::npos)
            --frames;
        // 実 Manim は -s では最後の 1 枚だけを書く。
        if (lastFrameOnly)
            frames = 1;
        for (int index = frames - 1; index >= 0; --index) {
            wchar_t name[64] = {};
            std::swprintf(name, std::size(name), L"MvmMathWrite%04d.png", index);
            writePng(images / name);
        }
        std::puts("fake Manim: wrote sequence");
        return 0;
    }
    writePng(images / L"MvmMathTex_ManimCE_fake.png");
    if (request.find("FAKE_TWO_OUTPUTS") != std::string::npos)
        writePng(images / L"MvmMathTex_second.png");
    std::puts("fake Manim: rendered");
    return 0;
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv)
        return 2;

    wchar_t self[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    const std::wstring role = std::filesystem::path(self).stem().wstring();

    int code = 2;
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::fputs("催促: MiKTeX の更新を確認していません\n", stderr);
        if (role == L"latex")
            std::puts("latex fake 1.2.3");
        else if (role == L"dvisvgm")
            std::puts("dvisvgm fake 3.4.5");
        else
            std::puts("Manim Community v0.0-fake");
        code = 0;
    } else if (argc >= 4 && std::string(argv[1]) == "render") {
        std::filesystem::path media;
        bool lastFrameOnly = false;
        for (int index = 2; index + 1 < argc; ++index) {
            if (std::string(argv[index]) == "--media_dir")
                media = fromUtf8(argv[index + 1]);
            if (std::string(argv[index]) == "-s")
                lastFrameOnly = true;
        }
        const auto script = fromUtf8(argv[argc - 2]);
        code = media.empty() ? 2 : render(script, media, argv[argc - 1], lastFrameOnly);
    }
    mvm_win_free_utf8_args(argv, argc);
    return code;
}
