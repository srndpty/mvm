// P3-4: 実 Manim の animated pulse について、mvm の 2 層の合成と、Manim の 1 つの scene で描いた
// frame を比べる手動の計測 (CTest に登録しない。実 Manim と MiKTeX を build・試験の条件にしない)。
//
//   mvm_equation_pulse_equivalence <manim.exe> <python.exe> <reference script> <証拠
//   (存在しないこと)>
//
// 1. 製品の MathRasterCache と実 backend で、通常の P3-3 の artifact (base の A8 + 動く accent の
// A8 +
//    provenance の色) を公開する。公開の直前に backend の作業 directory を写し、request.json
//    (canvas・配置・font・segment・重み) を参照の描画に渡す
// 2. scripts/spikes/math-p34-pulse-reference.py が、同じ request.json で状態の色の式に Indicate
//    (強調色、線形、同じ重み) を掛けた 1 枚を描く (Cairo の premultiplied RGBA)
// 3. artifact の層を製品の合成 (composeEquationLayersAt、P3-4 の 2 層の規則) で canvas に置き、
//    premultiplied へ変えて参照と比べる
// 違いの画素数・channel の最大差・外接矩形・違う画素の分類 (base と accent の重なり / 半透明の縁 /
// それ以外) を results.json に記録する。一致を前提にしない (合否の判定は記録から人が行う)。
// 合否に使う検査は、経路が本当に比べたか (artifact・参照の大きさ、premultiplied の前提、
// 参照が artifact の矩形の外に描かない、別の進み具合・accent を省いた対照で違いが出る) だけ。

#include "app/equation_sequence_compile.h"
#include "app/equation_sequence_render.h"
#include "app/math_clip_render.h"
#include "math_raster_cache.h"
#include "media/manim/manim_math_tex.h"
#include "media/math/equation_sequence_render.h"
#include "util/mvm_long_path.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>

namespace {

namespace math = mvm::math;
namespace manim = mvm::manim;
namespace project = mvm::project;
namespace app = mvm::app;

int checks = 0;
int failures = 0;

bool check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
    return condition;
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string jsonString(const std::string& text) {
    static const char kHex[] = "0123456789abcdef";
    std::string json = "\"";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            json += '\\';
            json += static_cast<char>(c);
        } else if (c < 0x20) {
            json += "\\u00";
            json += kHex[c >> 4];
            json += kHex[c & 0x0F];
        } else {
            json += static_cast<char>(c);
        }
    }
    return json + "\"";
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool wait(const std::function<bool()>& done, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done() && std::chrono::steady_clock::now() < until) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

struct CaseDef {
    std::string label;
    std::string source;
    std::string target;
    std::int64_t frames = 5;
};

constexpr char kStateColor[] = "#FF40C0FF";

project::EquationSequenceClipData sequenceFor(const CaseDef& c) {
    project::EquationState state;
    state.id = {"S0"};
    state.revision = "r0";
    state.equation.source = c.source;
    state.equation.fontSize = 72;
    state.equation.color = kStateColor;
    state.holdFrames = c.frames + 2;
    const auto at = c.source.find(c.target);
    state.parts.push_back(
        {{"P"},
         "対象",
         {"r0", static_cast<std::int64_t>(at), static_cast<std::int64_t>(at + c.target.size()),
          c.target, project::BindingStatus::Bound}});
    project::EquationSequenceClipData data;
    data.states = {state};
    data.actions = {{{"A"},
                     {"S0"},
                     {"P"},
                     project::EquationTargetStatus::Present,
                     1,
                     c.frames,
                     project::EquationOperation::Pulse}};
    return data;
}

std::string argbText(std::uint32_t argb) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "#%08X", argb);
    return text;
}

struct Metrics {
    std::size_t differing = 0;
    int maxDifference = 0;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = -1;
    int maxY = -1;
    std::size_t overlap = 0;  // base と accent の両方に被覆がある画素
    std::size_t edge = 0;     // 重ならないが、どちらかの層が半透明 (antialias の縁)
    std::size_t interior = 0; // どちらでもない (完全な被覆か空の画素)
    std::size_t alphaDiffering = 0;
    int alphaMaxDifference = 0;
    std::size_t covered = 0; // 参照か製品で alpha > 0
    // 違う画素の channel のうち、参照の値が実数の合成値 (2 層の premultiplied、丸める前) の
    // floor / ceil のどちらか (同じ実数の丸め方の違い) に入るもの・入らないもの。
    std::size_t roundingNeighbour = 0;
    std::size_t outsideRounding = 0;
    // 製品の channel (全画素) のうち、同じ実数の floor / ceil に入らないもの。
    std::size_t productOutsideRounding = 0;

    std::string json() const {
        std::string text =
            "{\"differing_pixels\": " + std::to_string(differing) +
            ", \"max_channel_difference\": " + std::to_string(maxDifference) +
            ", \"overlap_pixels\": " + std::to_string(overlap) +
            ", \"edge_pixels\": " + std::to_string(edge) +
            ", \"interior_pixels\": " + std::to_string(interior) +
            ", \"alpha_differing_pixels\": " + std::to_string(alphaDiffering) +
            ", \"alpha_max_difference\": " + std::to_string(alphaMaxDifference) +
            ", \"covered_pixels\": " + std::to_string(covered) +
            ", \"differing_channels_rounding_neighbour\": " + std::to_string(roundingNeighbour) +
            ", \"differing_channels_outside_rounding\": " + std::to_string(outsideRounding) +
            ", \"product_channels_outside_rounding\": " + std::to_string(productOutsideRounding);
        if (maxX >= 0)
            text += ", \"bbox\": [" + std::to_string(minX) + ", " + std::to_string(minY) + ", " +
                    std::to_string(maxX - minX + 1) + ", " + std::to_string(maxY - minY + 1) + "]";
        else
            text += ", \"bbox\": null";
        return text + "}";
    }
};

// product と reference は canvas の premultiplied RGBA。base / accent は canvas の座標の被覆。
// colors (base, accent) を渡すと、違う channel が実数の合成値の丸めの隣にあるかを数える。
Metrics compare(const std::vector<std::uint8_t>& product,
                const std::vector<std::uint8_t>& reference, const std::vector<std::uint8_t>& base,
                const std::vector<std::uint8_t>& accent, int width, std::uint32_t baseArgb = 0,
                std::uint32_t accentArgb = 0) {
    Metrics m;
    for (std::size_t p = 0; p * 4 + 3 < product.size() && p * 4 + 3 < reference.size(); ++p) {
        if (baseArgb != 0 || accentArgb != 0) {
            // 実数の premultiplied: C_accent * b + C_base * a * (1 - b) (a, b は 0..1 の被覆 * 色の
            // alpha)。
            const double a = base[p] / 255.0 * ((baseArgb >> 24) & 0xFF) / 255.0;
            const double b = accent[p] / 255.0 * ((accentArgb >> 24) & 0xFF) / 255.0;
            for (std::size_t ch = 0; ch < 3; ++ch) {
                const int shift = 16 - 8 * static_cast<int>(ch);
                const double exact = ((accentArgb >> shift) & 0xFF) * b +
                                     ((baseArgb >> shift) & 0xFF) * a * (1.0 - b);
                const auto isNeighbour = [&](int value) {
                    return value == static_cast<int>(std::floor(exact + 1e-9)) ||
                           value == static_cast<int>(std::ceil(exact - 1e-9));
                };
                if (!isNeighbour(product[p * 4 + ch]))
                    ++m.productOutsideRounding;
                if (product[p * 4 + ch] == reference[p * 4 + ch])
                    continue;
                (isNeighbour(reference[p * 4 + ch]) ? m.roundingNeighbour : m.outsideRounding) += 1;
            }
        }
        int maxDifference = 0;
        for (std::size_t ch = 0; ch < 4; ++ch)
            maxDifference = std::max(
                maxDifference, std::abs(int(product[p * 4 + ch]) - int(reference[p * 4 + ch])));
        const int alphaDifference = std::abs(int(product[p * 4 + 3]) - int(reference[p * 4 + 3]));
        m.covered += product[p * 4 + 3] > 0 || reference[p * 4 + 3] > 0;
        if (alphaDifference > 0) {
            ++m.alphaDiffering;
            m.alphaMaxDifference = std::max(m.alphaMaxDifference, alphaDifference);
        }
        if (maxDifference == 0)
            continue;
        ++m.differing;
        m.maxDifference = std::max(m.maxDifference, maxDifference);
        const int x = static_cast<int>(p % static_cast<std::size_t>(width));
        const int y = static_cast<int>(p / static_cast<std::size_t>(width));
        m.minX = std::min(m.minX, x);
        m.minY = std::min(m.minY, y);
        m.maxX = std::max(m.maxX, x);
        m.maxY = std::max(m.maxY, y);
        const int b = base[p];
        const int a = accent[p];
        if (b > 0 && a > 0)
            ++m.overlap;
        else if ((b > 0 && b < 255) || (a > 0 && a < 255))
            ++m.edge;
        else
            ++m.interior;
    }
    return m;
}

void savePng(const std::vector<std::uint8_t>& premultiplied, int width, int height,
             const std::filesystem::path& path) {
    QImage image(premultiplied.data(), width, height, width * 4,
                 QImage::Format_RGBA8888_Premultiplied);
    image.copy().save(QString::fromStdWString(path.wstring()), "PNG");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    mvm_enable_utf8_console();
    if (argc != 5) {
        std::fprintf(stderr, "使い方: mvm_equation_pulse_equivalence <manim.exe> <python.exe> "
                             "<reference script> <証拠の directory (存在しないこと)>\n");
        return 2;
    }
    const auto manimExe = fromUtf8(argv[1]);
    const auto python = fromUtf8(argv[2]);
    const auto script = fromUtf8(argv[3]);
    std::error_code error;
    // 製品の cache directory と同じく絶対 path にする (Manim は作業 directory で script を探す)。
    const auto evidence = std::filesystem::absolute(fromUtf8(argv[4]), error);
    if (std::filesystem::exists(evidence, error)) {
        std::fprintf(stderr, "証拠の directory が既にあります (上書きしません)\n");
        return 2;
    }
    std::filesystem::create_directories(evidence, error);

    app::MathRasterCache cache("p34-pulse", [manimExe](const std::filesystem::path& work,
                                                       const std::atomic<bool>* cancel) {
        return manim::preflightManimMathTex({manimExe, work, std::chrono::milliseconds(60000)},
                                            cancel);
    });
    cache.setRenderTimeout(std::chrono::milliseconds(120000));
    cache.setAuthority(evidence / L"cache", true);
    if (!check(wait(
                   [&] {
                       return cache.backendState() != app::MathRasterCache::BackendState::Checking;
                   },
                   180000) &&
                   cache.backendState() == app::MathRasterCache::BackendState::Available,
               "実 backend の preflight が Available: " + cache.backendMessage().toStdString()))
        return 1;
    std::printf("toolchain:\n%s\n", qUtf8Printable(cache.toolchainText()));

    const std::string quadratic = "x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}";
    const std::vector<CaseDef> cases = {
        {"disc", quadratic, "b^2-4ac", 5},
        {"disc-n1", quadratic, "b^2-4ac", 1},
        {"multi-glyph", "y=a^2+2ab+b^2", "2ab", 5},
        {"greek", "\\alpha+\\beta=\\gamma", "\\beta", 5},
        {"subscript", "a^{2}+b_{1}=c_n^3", "b_{1}", 5},
    };
    std::string results =
        "{\"toolchain\": " + jsonString(cache.toolchainText().toStdString()) +
        ", \"state_color\": " + jsonString(kStateColor) +
        ", \"accent_color\": " + jsonString(argbText(math::kEquationActionAccentArgb)) +
        ", \"comparison_space\": \"premultiplied RGBA (Cairo の pixel_array と同じ)\"" +
        ", \"cases\": [";
    for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
        const auto& c = cases[caseIndex];
        std::printf("[%s] %s / %s N=%lld\n", c.label.c_str(), c.source.c_str(), c.target.c_str(),
                    static_cast<long long>(c.frames));
        const auto data = sequenceFor(c);
        const auto compiled = app::compileEquationSequence(data);
        std::string specError;
        const auto render = compiled.value
                                ? app::equationSequenceRenderSpecFor(*compiled.value, specError)
                                : std::nullopt;
        if (!check(render.has_value(), c.label + ": compile と描画要求: " + specError))
            continue;
        // 公開の直前に backend の作業 directory (request.json と canvas の PNG) を写す。
        const auto snapshot = evidence / L"jobs" / c.label;
        // 作業 directory の path は 260 文字を超えうる (extended-length で写す)。
        cache.setBeforeEquationSequencePublishForTest(
            [&cache, snapshot](const std::filesystem::path&) {
                std::error_code copyError;
                std::filesystem::create_directories(
                    mvm::util::extendedLengthPath(snapshot.parent_path()), copyError);
                std::filesystem::copy(mvm::util::extendedLengthPath(cache.jobsDirectory()),
                                      mvm::util::extendedLengthPath(snapshot),
                                      std::filesystem::copy_options::recursive, copyError);
                if (copyError)
                    std::fprintf(stderr, "作業 directory を写せません: %s\n",
                                 copyError.message().c_str());
            });
        for (const auto& state : render->states)
            cache.request(state.still);
        app::MathRasterCache::EquationSequenceEntry entry;
        wait(
            [&] {
                entry = cache.requestEquationSequence(*render);
                return entry.state != app::MathRasterCache::State::Pending;
            },
            600000);
        cache.setBeforeEquationSequencePublishForTest({});
        if (!check(entry.state == app::MathRasterCache::State::Ready,
                   c.label + ": artifact を公開できる: " + entry.message.toStdString())) {
            // 失敗の手がかり (静止と sequence の log の末尾) を残す。
            std::string log = entry.log.toStdString();
            for (const auto& state : render->states)
                log += "\n--- static ---\n" + cache.request(state.still).log.toStdString();
            std::ofstream(evidence / (c.label + "-failure.log"), std::ios::binary) << log;
            std::fprintf(stderr, "%s\n",
                         log.substr(log.size() > 2000 ? log.size() - 2000 : 0).c_str());
            continue;
        }
        const auto artifact = cache.readyEquationSequence(*render);
        if (!check(artifact.has_value() && artifact->actions.size() == 1,
                   c.label + ": 公開した artifact を全 frame の SHA-256 で検査し直せる"))
            continue;
        const auto& action = artifact->actions[0];
        // 写した作業 directory の request.json。
        std::filesystem::path requestPath;
        for (std::filesystem::recursive_directory_iterator
                 it(mvm::util::extendedLengthPath(snapshot), error),
             end;
             !error && it != end; it.increment(error))
            if (it->path().filename() == L"request.json" &&
                it->path().parent_path().filename().wstring().find(L"-equation-sequence-") !=
                    std::wstring::npos)
                requestPath = it->path();
        if (!check(!requestPath.empty(), c.label + ": backend の request.json を写せた"))
            continue;
        const auto requestBytes = readBytes(requestPath);
        const auto requestDocument =
            QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(requestBytes.data()),
                                               static_cast<qsizetype>(requestBytes.size())));
        const auto actionJson = requestDocument.object()["actions"].toArray()[0].toObject();
        const int canvasWidth = actionJson["canvas_px"].toArray()[0].toInt();
        const int canvasHeight = actionJson["canvas_px"].toArray()[1].toInt();
        const int placementLeft = actionJson["placement_px"].toArray()[0].toInt();
        const int placementTop = actionJson["placement_px"].toArray()[1].toInt();
        const int left = placementLeft - action.staticX;
        const int top = placementTop - action.staticY;
        check(canvasWidth > 0 && left >= 0 && top >= 0 && left + action.width <= canvasWidth &&
                  top + action.height <= canvasHeight,
              c.label + ": artifact の矩形は canvas の中");

        // 参照の描画 (実 Manim の 1 つの scene)。
        const auto referenceDirectory = evidence / L"reference" / c.label;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QString::fromStdWString(python.wstring()),
                      {QString::fromStdWString(script.wstring()),
                       QString::fromStdWString(requestPath.wstring()),
                       QString::fromStdWString(referenceDirectory.wstring()),
                       QString::fromLatin1(kStateColor),
                       QString::fromStdString(argbText(math::kEquationActionAccentArgb))});
        const bool finished = process.waitForFinished(600000);
        const auto output = process.readAll().toStdString();
        {
            std::ofstream log(evidence / (c.label + "-reference.log"), std::ios::binary);
            log << output;
        }
        if (!check(finished && process.exitCode() == 0,
                   c.label + ": 参照を描ける (log: " + c.label + "-reference.log)"))
            continue;

        const auto base = readBytes(action.base.path);
        std::vector<std::int64_t> sampled = {0};
        if (c.frames > 1)
            sampled = {0, c.frames / 2, c.frames - 1};
        std::string frames = "[";
        for (std::size_t s = 0; s < sampled.size(); ++s) {
            const auto i = sampled[s];
            const auto& accentFrame = action.accent[static_cast<std::size_t>(i)];
            const auto accent = readBytes(accentFrame.path);
            // 製品の 2 層の合成 (provenance の色) を canvas に置き、premultiplied にする。
            std::vector<std::uint8_t> product(static_cast<std::size_t>(canvasWidth) *
                                                  static_cast<std::size_t>(canvasHeight) * 4U,
                                              0);
            math::composeEquationLayersAt(base.data(), action.base.colorArgb, accent.data(),
                                          accentFrame.colorArgb, action.width, action.height,
                                          product.data(), canvasWidth, left, top);
            std::vector<std::uint8_t> productAccentless(product.size(), 0);
            const std::vector<std::uint8_t> zero(accent.size(), 0);
            math::composeEquationLayersAt(base.data(), action.base.colorArgb, zero.data(),
                                          accentFrame.colorArgb, action.width, action.height,
                                          productAccentless.data(), canvasWidth, left, top);
            const auto premultiply = [](std::vector<std::uint8_t>& rgba) {
                for (std::size_t p = 0; p + 3 < rgba.size(); p += 4)
                    for (std::size_t ch = 0; ch < 3; ++ch)
                        rgba[p + ch] =
                            static_cast<std::uint8_t>((rgba[p + ch] * rgba[p + 3] + 127) / 255);
            };
            premultiply(product);
            premultiply(productAccentless);
            // 層の被覆を canvas の座標へ (違う画素の分類用)。
            std::vector<std::uint8_t> baseCanvas(product.size() / 4, 0);
            std::vector<std::uint8_t> accentCanvas(product.size() / 4, 0);
            for (int y = 0; y < action.height; ++y)
                for (int x = 0; x < action.width; ++x) {
                    const auto from = static_cast<std::size_t>(y * action.width + x);
                    const auto to = static_cast<std::size_t>((top + y) * canvasWidth + left + x);
                    baseCanvas[to] = base[from];
                    accentCanvas[to] = accent[from];
                }
            std::string variants;
            for (const char* variant : {"scene", "ordered"}) {
                char name[64] = {};
                std::snprintf(name, sizeof(name), "a0-%s-%05lld.rgba", variant,
                              static_cast<long long>(i));
                const auto reference = readBytes(referenceDirectory / name);
                if (!check(reference.size() == product.size(),
                           c.label + ": 参照の大きさが canvas と同じ: " + name))
                    continue;
                // premultiplied の前提 (各 channel <= alpha) と、artifact の矩形の外に描かない。
                std::size_t premultipliedViolation = 0;
                std::size_t outside = 0;
                for (std::size_t p = 0; p + 3 < reference.size(); p += 4) {
                    premultipliedViolation += reference[p] > reference[p + 3] ||
                                              reference[p + 1] > reference[p + 3] ||
                                              reference[p + 2] > reference[p + 3];
                    const int x = static_cast<int>((p / 4) % static_cast<std::size_t>(canvasWidth));
                    const int y = static_cast<int>((p / 4) / static_cast<std::size_t>(canvasWidth));
                    outside +=
                        reference[p + 3] > 0 && (x < left || y < top || x >= left + action.width ||
                                                 y >= top + action.height);
                }
                check(premultipliedViolation == 0,
                      c.label + ": 参照は premultiplied (channel <= alpha): " + name);
                check(outside == 0, c.label + ": 参照は artifact の矩形の外に描かない: " + name);
                const auto metrics =
                    compare(product, reference, baseCanvas, accentCanvas, canvasWidth,
                            action.base.colorArgb, accentFrame.colorArgb);
                const auto accentless =
                    compare(productAccentless, reference, baseCanvas, accentCanvas, canvasWidth);
                check(accentless.differing > metrics.differing,
                      c.label + ": 対照 (accent を省く) は参照からもっと離れる: " + name);
                savePng(reference, canvasWidth, canvasHeight,
                        evidence / L"reference" / c.label / (std::string(name) + ".png"));
                std::printf("  frame %lld %-8s 違う画素 %zu (重なり %zu / 縁 %zu / 内部 %zu)、"
                            "最大差 %d、alpha の違い %zu (最大 %d)、丸めの隣 %zu / 外 %zu、"
                            "製品の丸めの外 %zu、対照 %zu\n",
                            static_cast<long long>(i), variant, metrics.differing, metrics.overlap,
                            metrics.edge, metrics.interior, metrics.maxDifference,
                            metrics.alphaDiffering, metrics.alphaMaxDifference,
                            metrics.roundingNeighbour, metrics.outsideRounding,
                            metrics.productOutsideRounding, accentless.differing);
                variants += std::string(variants.empty() ? "" : ", ") + "\"" + variant +
                            "\": " + metrics.json() + ", \"" + variant +
                            "_accentless_control\": " + accentless.json();
            }
            savePng(product, canvasWidth, canvasHeight,
                    evidence / L"reference" / c.label / ("product-" + std::to_string(i) + ".png"));
            frames += std::string(s > 0 ? ", " : "") + "{\"frame\": " + std::to_string(i) +
                      ", \"accent_color\": " + jsonString(argbText(accentFrame.colorArgb)) + ", " +
                      variants + "}";
        }
        frames += "]";
        // 別の進み具合の参照との対照 (比較が進み具合を見分ける)。
        if (c.frames > 1) {
            const auto accent = readBytes(action.accent[0].path);
            std::vector<std::uint8_t> product(static_cast<std::size_t>(canvasWidth) *
                                                  static_cast<std::size_t>(canvasHeight) * 4U,
                                              0);
            math::composeEquationLayersAt(base.data(), action.base.colorArgb, accent.data(),
                                          action.accent[0].colorArgb, action.width, action.height,
                                          product.data(), canvasWidth, left, top);
            for (std::size_t p = 0; p + 3 < product.size(); p += 4)
                for (std::size_t ch = 0; ch < 3; ++ch)
                    product[p + ch] =
                        static_cast<std::uint8_t>((product[p + ch] * product[p + 3] + 127) / 255);
            char name[64] = {};
            std::snprintf(name, sizeof(name), "a0-scene-%05lld.rgba",
                          static_cast<long long>(c.frames / 2));
            const auto other = readBytes(referenceDirectory / name);
            const std::vector<std::uint8_t> none(product.size() / 4, 0);
            const auto own = readBytes(referenceDirectory / "a0-scene-00000.rgba");
            check(compare(product, other, none, none, canvasWidth).differing >
                      compare(product, own, none, none, canvasWidth).differing,
                  c.label + ": 対照 (別の進み具合の参照) は自分の参照より離れる");
        }
        results += std::string(caseIndex > 0 ? ", " : "") + "{\"label\": " + jsonString(c.label) +
                   ", \"source\": " + jsonString(c.source) +
                   ", \"target\": " + jsonString(c.target) +
                   ", \"frames\": " + std::to_string(c.frames) + ", \"canvas\": [" +
                   std::to_string(canvasWidth) + ", " + std::to_string(canvasHeight) +
                   "], \"artifact\": [" + std::to_string(left) + ", " + std::to_string(top) + ", " +
                   std::to_string(action.width) + ", " + std::to_string(action.height) +
                   "], \"base_color\": " + jsonString(argbText(action.base.colorArgb)) +
                   ", \"sampled\": " + frames + "}";
    }
    results += "], \"checks\": " + std::to_string(checks) +
               ", \"failures\": " + std::to_string(failures) + "}\n";
    {
        std::ofstream out(evidence / "results.json", std::ios::binary);
        out << results;
    }
    cache.shutdown();
    std::printf("%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
