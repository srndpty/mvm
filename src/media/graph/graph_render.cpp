#include "media/graph/graph_render.h"

#include "util/mvm_atomic_write.h"
#include "util/mvm_sha256.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <png.h>
#include <sstream>
#include <thread>

namespace mvm::graph {
namespace {
void field(std::string& out, std::string_view value) {
    out += std::to_string(value.size()) + ":";
    out.append(value);
}

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << (value == 0 ? 0 : value);
    return out.str();
}

void real(std::string& out, double value) {
    field(out, number(value));
}

void rectangle(std::string& out, const Rectangle& r) {
    for (double v : {r.left, r.top, r.width, r.height})
        real(out, v);
}

void pathMaterial(std::string& out, const Path& path) {
    field(out, std::to_string(path.argb));
    real(out, path.widthPixels);
    field(out, std::to_string(path.points.size()));
    for (auto p : path.points) {
        real(out, p.x);
        real(out, p.y);
    }
}

std::string quote(std::string_view value) {
    std::string out = "\"";
    for (char character : value) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}

std::string pathJson(const Path& path) {
    std::string out = "{\"function\":" + std::to_string(path.function) +
                      ",\"segment\":" + std::to_string(path.segment) +
                      ",\"argb\":" + std::to_string(path.argb) +
                      ",\"width\":" + number(path.widthPixels) + ",\"points\":[";
    for (std::size_t i = 0; i < path.points.size(); ++i) {
        if (i)
            out += ',';
        out += '[' + number(path.points[i].x) + ',' + number(path.points[i].y) + ']';
    }
    return out + "]}";
}

Error corrupt(std::string text) {
    return {Failure::ArtifactCorrupt, 0, std::move(text)};
}

bool cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load();
}

std::string readBounded(const std::filesystem::path& path, std::size_t limit) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit)
        return {};
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return {};
    return bytes;
}

std::string frameName(std::int64_t index) {
    return index < 0 ? "static.png" : "frame-" + std::to_string(index) + ".png";
}

// manifest は長さ前置の UTF-8 field。expected header と各 frame の二つの hash 以外を許さない。
std::string manifestHeader(const GraphRenderSpec& spec, const std::string& toolchain) {
    std::string out;
    for (const auto& s :
         {std::string(staticVersion), std::string(artifactDrawVersion), staticKey(spec, toolchain),
          drawKey(staticKey(spec, toolchain), spec.drawFrames), toolchain,
          std::string(expressionVersion), std::string(samplingVersion), std::string(layoutVersion),
          std::string(drawVersion), std::string(rgbaVersion), std::to_string(spec.width),
          std::to_string(spec.height), std::to_string(spec.drawFrames), digest(canonicalSpec(spec)),
          canonicalSpec(spec)})
        field(out, s);
    return out;
}

struct Validation {
    std::string manifest;
    std::vector<std::string> pixelHashes;
};

bool withinRenderBudget(const GraphRenderSpec& spec) {
    return spec.width > 0 && spec.height > 0 && spec.drawFrames >= 0 &&
           static_cast<std::uint64_t>(spec.width) * static_cast<std::uint64_t>(spec.height) * 4 *
                   (static_cast<std::uint64_t>(spec.drawFrames) + 1) <=
               Limits::artifactBytes;
}

bool expectsVisibleStroke(const GraphRenderSpec& spec, std::int64_t frame) {
    for (const auto& path : framePaths(spec, frame)) {
        if (path.widthPixels < 1 || (path.argb >> 24) < 16)
            continue;
        for (std::size_t i = 1; i < path.points.size(); ++i) {
            const auto a = path.points[i - 1], b = path.points[i];
            const double x = (a.x + b.x) / 2, y = (a.y + b.y) / 2;
            if (std::hypot(b.x - a.x, b.y - a.y) >= 4 && x >= spec.plot.left + 1 &&
                x <= spec.plot.left + spec.plot.width - 1 && y >= spec.plot.top + 1 &&
                y <= spec.plot.top + spec.plot.height - 1)
                return true;
        }
        // sampling が密な通常の一本線も、途中の点と全体の幅から可視性を確定する。
        if (path.points.size() >= 3) {
            const auto a = path.points.front(), b = path.points.back(),
                       p = path.points[path.points.size() / 2];
            if (std::hypot(b.x - a.x, b.y - a.y) >= 4 && p.x >= spec.plot.left + 1 &&
                p.x <= spec.plot.left + spec.plot.width - 1 && p.y >= spec.plot.top + 1 &&
                p.y <= spec.plot.top + spec.plot.height - 1)
                return true;
        }
    }
    return false;
}

std::variant<Validation, Error> inspectFrames(const std::filesystem::path& directory,
                                              const GraphRenderSpec& spec,
                                              const std::string& toolchain,
                                              const std::atomic<bool>* cancel) {
    Validation result{manifestHeader(spec, toolchain), {}};
    std::size_t bytes = 0;
    for (std::int64_t i = -1; i < spec.drawFrames; ++i) {
        if (cancelled(cancel))
            return Error{Failure::Cancelled, 0, "検証を取り消しました"};
        const auto name = frameName(i);
        const auto encoded = readBounded(directory / name, Limits::pixels * 5);
        bytes += encoded.size();
        if (encoded.empty() || bytes > Limits::artifactBytes)
            return corrupt("PNG が欠損または上限超過です");
        auto raster = readRgba(directory / name, spec.width, spec.height);
        if (auto* error = std::get_if<Error>(&raster))
            return *error;
        const auto& rgba = std::get<Raster>(raster).rgba;
        if (expectsVisibleStroke(spec, i)) {
            bool visible = false;
            for (std::size_t pixel = 3; pixel < rgba.size(); pixel += 4)
                visible |= rgba[pixel] != 0;
            if (!visible)
                return corrupt("可視の線が必要な frame が全透明です");
        }
        const auto hash = digest({reinterpret_cast<const char*>(rgba.data()), rgba.size()});
        const auto encodedHash = digest(encoded);
        if (hash.empty() || encodedHash.empty())
            return corrupt("画素 hash を計算できません");
        field(result.manifest, std::to_string(i));
        field(result.manifest, name);
        field(result.manifest, encodedHash);
        field(result.manifest, hash);
        result.pixelHashes.push_back(hash);
    }
    // 静止端点の関係は manifest に重複して固定し、読み出し時にも照合する。
    field(result.manifest, "endpoint=static.png");
    field(result.manifest, result.pixelHashes.front());
    return result;
}
} // namespace

double strokeScale(int width, int height) {
    return std::min(width / 1920.0, height / 1080.0);
}

Rectangle labelContentRectangle(const Rectangle& band) {
    return {band.left + band.width * .05, band.top + band.height * .1, band.width * .9,
            band.height * .8};
}

Point mapPoint(const GraphRenderSpec& spec, Point p) {
    // 先に無次元比へ変換し、極端な有限 span でも積を overflow させない。
    return {spec.plot.left + ((p.x - spec.xMin) / (spec.xMax - spec.xMin)) * spec.plot.width,
            spec.plot.top + ((spec.yMax - p.y) / (spec.yMax - spec.yMin)) * spec.plot.height};
}

std::vector<Path> framePaths(const GraphRenderSpec& spec, std::int64_t frame) {
    auto paths = spec.background;
    for (std::size_t index = 0; index < spec.curves.size(); ++index) {
        const auto& curve = spec.curves[index];
        const auto geometry =
            frame < 0 ? curve.geometry : reveal(curve.geometry, frame, spec.drawFrames);
        for (std::size_t segmentIndex = 0; segmentIndex < geometry.segments.size();
             ++segmentIndex) {
            const auto& segment = geometry.segments[segmentIndex];
            Path path{{},
                      curve.argb,
                      curve.widthPixels,
                      static_cast<std::int64_t>(index),
                      static_cast<std::int64_t>(segmentIndex)};
            for (auto p : segment)
                path.points.push_back(mapPoint(spec, p));
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::variant<std::monostate, Error> validateSpec(const GraphRenderSpec& spec) {
    if (spec.width < 1 || spec.height < 1 || spec.width > Limits::dimension ||
        spec.height > Limits::dimension ||
        static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.height) >
            Limits::pixels ||
        spec.drawFrames < 0 || spec.drawFrames > Limits::frames)
        return Error{Failure::ResourceLimit, 0, "Graph の raster budget を超えています"};
    if (!std::isfinite(spec.xMin) || !std::isfinite(spec.xMax) || !std::isfinite(spec.yMin) ||
        !std::isfinite(spec.yMax) || !(spec.xMin < spec.xMax) || !(spec.yMin < spec.yMax) ||
        !std::isfinite(spec.xMax - spec.xMin) || !std::isfinite(spec.yMax - spec.yMin) ||
        spec.plot !=
            Rectangle{spec.width / 8.0, spec.height / 8.0, spec.width * .75, spec.height * .75})
        return Error{Failure::InvalidViewport, 0, "Graph の座標配置が不正です"};
    if (spec.curves.empty() || spec.curves.size() > 3 ||
        spec.labels.size() != spec.curves.size() + 2 || spec.background.size() > 24)
        return Error{Failure::InvalidGraph, 0, "Graph の関数またはラベル数が不正です"};
    std::size_t points = 0;
    for (const auto& c : spec.curves) {
        if (c.ast.empty() || c.ast.size() > 131072 || !std::isfinite(c.referenceWidth) ||
            !(c.referenceWidth > 0 && c.referenceWidth <= 64) ||
            c.widthPixels != c.referenceWidth * strokeScale(spec.width, spec.height) ||
            !(c.geometry.domainMin < c.geometry.domainMax) || c.geometry.domainMin < spec.xMin ||
            c.geometry.domainMax > spec.xMax)
            return Error{Failure::InvalidGraph, 0, "Graph の曲線設定が不正です"};
        double previous = c.geometry.domainMin;
        for (const auto& segment : c.geometry.segments) {
            if (segment.size() < 2 || segment.front() == segment.back() ||
                segment.front().x < previous)
                return Error{Failure::InvalidGraph, 0, "Graph の segment が不正です"};
            for (auto p : segment) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < previous ||
                    p.x < c.geometry.domainMin || p.x > c.geometry.domainMax || p.y < spec.yMin ||
                    p.y > spec.yMax)
                    return Error{Failure::InvalidGraph, 0, "Graph の点列が不正です"};
                previous = p.x;
                if (++points > Limits::points)
                    return Error{Failure::ResourceLimit, 0, "点数の上限を超えています"};
            }
        }
    }
    for (std::size_t i = 0; i < spec.labels.size(); ++i) {
        const auto& label = spec.labels[i];
        const auto& b = label.band;
        const double bandWidth = spec.plot.width / static_cast<double>(spec.curves.size());
        const Rectangle expected =
            i == 0
                ? Rectangle{spec.plot.left, spec.height * .875, spec.plot.width, spec.height * .125}
            : i == 1 ? Rectangle{0, spec.plot.top, spec.width * .125, spec.plot.height}
                     : Rectangle{spec.plot.left + static_cast<double>(i - 2) * bandWidth, 0,
                                 bandWidth, spec.height * .125};
        if (b != expected || label.rotated != (i == 1) || label.content != labelContentRectangle(b))
            return Error{Failure::InvalidGraph, i, "凍結されたラベル配置と一致しません"};
        if (label.text.size() > 4096 || !std::isfinite(b.left) || !std::isfinite(b.top) ||
            !std::isfinite(b.width) || !std::isfinite(b.height) || b.left < 0 || b.top < 0 ||
            b.width <= 0 || b.height <= 0 || b.left + b.width > spec.width ||
            b.top + b.height > spec.height ||
            (b.left < spec.plot.left + spec.plot.width && b.left + b.width > spec.plot.left &&
             b.top < spec.plot.top + spec.plot.height && b.top + b.height > spec.plot.top))
            return Error{Failure::InvalidGraph, 0, "ラベルの帯が plot と重なっています"};
    }
    for (const auto& p : spec.background) {
        if (p.points.size() != 2 || !std::isfinite(p.widthPixels) || p.widthPixels <= 0)
            return Error{Failure::InvalidGraph, 0, "背景の線が不正です"};
        for (auto v : p.points)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || v.x < spec.plot.left ||
                v.x > spec.plot.left + spec.plot.width || v.y < spec.plot.top ||
                v.y > spec.plot.top + spec.plot.height)
                return Error{Failure::InvalidGraph, 0, "背景の座標が不正です"};
    }
    return std::monostate{};
}

std::string digest(std::string_view value) {
    char out[MVM_SHA256_HEX_SIZE];
    return mvm_sha256_hex(value.data(), value.size(), out) == 0 ? out : "";
}

std::string canonicalSpec(const GraphRenderSpec& spec) {
    std::string out;
    for (auto v : {expressionVersion, samplingVersion, std::string_view(layoutVersion),
                   std::string_view(rgbaVersion)})
        field(out, v);
    field(out, std::to_string(spec.width));
    field(out, std::to_string(spec.height));
    for (double v : {spec.xMin, spec.xMax, spec.yMin, spec.yMax})
        real(out, v);
    rectangle(out, spec.plot);
    field(out, std::to_string(spec.background.size()));
    for (const auto& path : spec.background)
        pathMaterial(out, path);
    field(out, std::to_string(spec.curves.size()));
    for (const auto& c : spec.curves) {
        field(out, c.ast);
        real(out, c.geometry.domainMin);
        real(out, c.geometry.domainMax);
        field(out, std::to_string(c.argb));
        real(out, c.referenceWidth);
        real(out, c.widthPixels);
        field(out, std::to_string(c.geometry.segments.size()));
        for (const auto& segment : c.geometry.segments)
            pathMaterial(out, {segment, c.argb, c.widthPixels});
    }
    for (const auto& label : spec.labels) {
        field(out, label.text);
        rectangle(out, label.band);
        rectangle(out, label.content);
        field(out, label.rotated ? "1" : "0");
    }
    return out;
}

std::string staticKey(const GraphRenderSpec& spec, const std::string& toolchain) {
    if (toolchain.empty() || std::holds_alternative<Error>(validateSpec(spec)))
        return {};
    std::string out;
    field(out, staticVersion);
    field(out, canonicalSpec(spec));
    field(out, toolchain);
    return digest(out);
}

std::string drawKey(const std::string& key, std::int64_t frames) {
    if (key.size() != 64 || frames < 1 || frames > Limits::frames)
        return {};
    std::string out;
    field(out, artifactDrawVersion);
    field(out, key);
    field(out, std::to_string(frames));
    field(out, drawVersion);
    return digest(out);
}

std::string frameProtocol(const GraphRenderSpec& spec, std::int64_t frame) {
    // 実際に作成した object の順序・点数・色・線幅を backend が報告する。
    std::string out = std::to_string(frame) + " " + std::to_string(spec.width) + " " +
                      std::to_string(spec.height) + " " + std::to_string(spec.curves.size()) + " " +
                      std::to_string(frame < 0 ? 1 : frame) + " " +
                      std::to_string(frame < 0 ? 1 : spec.drawFrames);
    for (const auto& path : framePaths(spec, frame))
        out += " " + std::to_string(path.function) + ":" + std::to_string(path.segment) + ":" +
               std::to_string(path.points.size()) + ":" + std::to_string(path.argb) + ":" +
               number(path.widthPixels) + ":" + pathGeometryDigest(spec, path);
    return out + "\n";
}

std::string pathGeometryDigest(const GraphRenderSpec& spec, const Path& path) {
    static_assert(std::endian::native == std::endian::little);
    std::string bytes;
    for (auto p : path.points) {
        const double mapped[] = {p.x - spec.width / 2.0, spec.height / 2.0 - p.y, 0};
        bytes.append(reinterpret_cast<const char*>(mapped), sizeof(mapped));
    }
    return digest(bytes);
}

std::string requestJson(const GraphRenderSpec& spec, const std::string& toolchain) {
    if (std::holds_alternative<Error>(validateSpec(spec)) || !withinRenderBudget(spec))
        return {};
    std::string out =
        "{\"schema\":\"mvm-graph-render/1\",\"function_count\":" +
        std::to_string(spec.curves.size()) + ",\"width\":" + std::to_string(spec.width) +
        ",\"height\":" + std::to_string(spec.height) + ",\"toolchain\":" + quote(toolchain) +
        ",\"plot\":[" + number(spec.plot.left) + ',' + number(spec.plot.top) + ',' +
        number(spec.plot.width) + ',' + number(spec.plot.height) + "],\"labels\":[";
    for (std::size_t i = 0; i < spec.labels.size(); ++i) {
        if (i)
            out += ',';
        const auto& l = spec.labels[i];
        out += "{\"text\":" + quote(l.text) + ",\"rotated\":" + (l.rotated ? "true" : "false") +
               ",\"band\":[" + number(l.band.left) + ',' + number(l.band.top) + ',' +
               number(l.band.width) + ',' + number(l.band.height) + "],\"content\":[" +
               number(l.content.left) + ',' + number(l.content.top) + ',' +
               number(l.content.width) + ',' + number(l.content.height) + "]}";
    }
    out += "],\"frames\":[";
    for (std::int64_t i = -1; i < spec.drawFrames; ++i) {
        if (i != -1)
            out += ',';
        out += "{\"index\":" + std::to_string(i) + ",\"phase\":[" + std::to_string(i < 0 ? 1 : i) +
               ',' + std::to_string(i < 0 ? 1 : spec.drawFrames) + "],\"paths\":[";
        const auto paths = framePaths(spec, i);
        for (std::size_t j = 0; j < paths.size(); ++j) {
            if (j)
                out += ',';
            out += pathJson(paths[j]);
        }
        out += "]}";
        if (out.size() > Limits::jsonBytes)
            return {};
    }
    return out + "]}";
}

void sourceOver(const std::uint8_t* under, std::uint8_t coverage, std::uint32_t argb,
                std::uint8_t* out) {
    const std::uint64_t a = (static_cast<std::uint64_t>(coverage) * (argb >> 24) + 127) / 255;
    const std::uint64_t b = under[3], weight = a * 255 + b * (255 - a);
    for (int i = 0; i < 3; ++i) {
        const auto c = (argb >> (16 - 8 * i)) & 255;
        out[i] = weight ? static_cast<std::uint8_t>(
                              (c * a * 255 + under[i] * b * (255 - a) + weight / 2) / weight)
                        : 0;
    }
    out[3] = static_cast<std::uint8_t>((weight + 127) / 255);
}

RasterResult readRgba(const std::filesystem::path& path, int width, int height) {
    const auto bytes = readBounded(path, Limits::pixels * 5);
    constexpr unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    constexpr unsigned char end[] = {0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    if (bytes.size() < 45 ||
        !std::equal(std::begin(signature), std::end(signature),
                    reinterpret_cast<const unsigned char*>(bytes.data())) ||
        !std::equal(std::begin(end), std::end(end),
                    reinterpret_cast<const unsigned char*>(bytes.data() + bytes.size() - 12)) ||
        static_cast<unsigned char>(bytes[24]) != 8 || static_cast<unsigned char>(bytes[25]) != 6)
        return corrupt("PNG は RGBA8 ではありません");
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()))
        return corrupt("PNG の構造が不正です");
    if (image.width != static_cast<unsigned>(width) ||
        image.height != static_cast<unsigned>(height) ||
        static_cast<std::size_t>(image.width) * image.height > Limits::pixels) {
        png_image_free(&image);
        return corrupt("PNG の寸法が不正です");
    }
    image.format = PNG_FORMAT_RGBA;
    Raster raster{width, height,
                  std::vector<std::uint8_t>(static_cast<std::size_t>(width) *
                                            static_cast<std::size_t>(height) * 4)};
    const bool valid = png_image_finish_read(&image, nullptr, raster.rgba.data(), 0, nullptr) != 0;
    png_image_free(&image);
    if (!valid)
        return corrupt("PNG を decode できません");
    for (std::size_t i = 0; i < raster.rgba.size(); i += 4)
        if (!raster.rgba[i + 3] && (raster.rgba[i] || raster.rgba[i + 1] || raster.rgba[i + 2]))
            return corrupt("透明画素の RGB が正準値ではありません");
    return raster;
}

bool writeRgba(const std::filesystem::path& path, const Raster& raster) {
    if (raster.width < 1 || raster.height < 1 || raster.width > Limits::dimension ||
        raster.height > Limits::dimension ||
        static_cast<std::size_t>(raster.width) * static_cast<std::size_t>(raster.height) >
            Limits::pixels ||
        raster.rgba.size() !=
            static_cast<std::size_t>(raster.width) * static_cast<std::size_t>(raster.height) * 4)
        return false;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(raster.width);
    image.height = static_cast<png_uint_32>(raster.height);
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, raster.rgba.data(), 0, nullptr))
        return false;
    std::vector<char> encoded(size);
    if (!png_image_write_to_memory(&image, encoded.data(), &size, 0, raster.rgba.data(), 0,
                                   nullptr))
        return false;
    std::ofstream out(path, std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

ArtifactResult validateArtifact(const std::filesystem::path& directory, const GraphRenderSpec& spec,
                                const std::string& toolchain, const std::atomic<bool>* cancel) {
    for (const auto& component : std::filesystem::absolute(directory).lexically_normal())
        if (component.string().starts_with(".pending-"))
            return corrupt("pending artifact は公開 authority ではありません");
    if (std::holds_alternative<Error>(validateSpec(spec)) || !withinRenderBudget(spec) ||
        toolchain.empty())
        return corrupt("検証 authority が不正です");
    auto checked = inspectFrames(directory, spec, toolchain, cancel);
    if (auto* error = std::get_if<Error>(&checked))
        return *error;
    const auto& result = std::get<Validation>(checked);
    if (readBounded(directory / "manifest.txt", Limits::jsonBytes) != result.manifest)
        return corrupt("manifest、provenance または hash が一致しません");
    return Artifact{directory, staticKey(spec, toolchain),
                    drawKey(staticKey(spec, toolchain), spec.drawFrames), result.pixelHashes};
}

std::uint64_t PublicationAuthority::supersede() {
    std::lock_guard lock(mutex_);
    generationCancel_->store(true);
    generationCancel_ = std::make_shared<std::atomic<bool>>(false);
    return ++generation_;
}

void PublicationAuthority::shutdown() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    generationCancel_->store(true);
}

ArtifactResult PublicationAuthority::generate(const RenderRequest& request,
                                              const std::filesystem::path& cache,
                                              std::uint64_t generation,
                                              const GraphRenderer& renderer,
                                              const std::atomic<bool>* cancel,
                                              const PublicationObserver& observer) {
    if (auto checked = validateSpec(request.spec); std::holds_alternative<Error>(checked))
        return std::get<Error>(checked);
    if (!withinRenderBudget(request.spec))
        return Error{Failure::ResourceLimit, 0, "Graph の全 frame budget を超えています"};
    const auto key = staticKey(request.spec, request.toolchain);
    if (key.empty())
        return Error{Failure::BackendUnavailable, 0, "toolchain identity がありません"};
    const auto target =
        cache / (request.spec.drawFrames ? drawKey(key, request.spec.drawFrames) : key);
    auto guard = [&]() -> std::optional<Error> {
        if (closed_)
            return Error{Failure::Cancelled, 0, "renderer は終了処理中です"};
        if (cancelled(cancel))
            return Error{Failure::Cancelled, 0, "描画を取り消しました"};
        if (generation != generation_)
            return Error{Failure::Superseded, 0, "描画 authority が更新されました"};
        return {};
    };
    std::shared_ptr<std::atomic<bool>> generationCancel;
    {
        std::lock_guard lock(mutex_);
        if (auto error = guard())
            return *error;
        generationCancel = generationCancel_;
    }
    auto checkAuthority = [&]() -> std::optional<Error> {
        std::lock_guard lock(mutex_);
        return guard();
    };
    auto readPublished = [&]() -> ArtifactResult {
        auto result = validateArtifact(target, request.spec, request.toolchain, cancel);
        if (auto error = checkAuthority())
            return *error;
        return result;
    };
    if (std::filesystem::exists(target))
        return readPublished();
    std::atomic<bool> stop{false};
    std::jthread watcher([&](std::stop_token token) {
        while (!token.stop_requested()) {
            if (cancelled(cancel) || generationCancel->load()) {
                stop.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::error_code ec;
    if (!std::filesystem::create_directories(request.job, ec) || ec)
        return Error{Failure::PublicationFailure, 0, "新しい job directory を作成できません"};
    auto rendered = renderer(request, &stop);
    {
        std::lock_guard lock(mutex_);
        if (auto error = guard())
            return *error;
    }
    if (auto* error = std::get_if<Error>(&rendered))
        return *error;
    auto checked = inspectFrames(request.job, request.spec, request.toolchain, &stop);
    if (auto* error = std::get_if<Error>(&checked))
        return *error;
    const auto& validated = std::get<Validation>(checked);
    if (auto error = checkAuthority())
        return *error;
    std::filesystem::create_directories(cache, ec);
    if (ec)
        return Error{Failure::PublicationFailure, 0, "cache を作成できません"};
    if (std::filesystem::exists(target))
        return readPublished();
    // 診断・TeX・request は job に保存する。再利用 cache は検証対象の PNG と manifest だけ。
    static std::atomic<std::uint64_t> sequence{0};
    const auto staging =
        cache / (".pending-" + key + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++sequence));
    if (!std::filesystem::create_directory(staging, ec) || ec)
        return Error{Failure::PublicationFailure, 0, "公開用 directory を作成できません"};
    for (std::int64_t i = -1; i < request.spec.drawFrames; ++i) {
        if (observer)
            observer(PublicationStage::Copy);
        if (auto error = checkAuthority())
            return *error;
        std::filesystem::copy_file(request.job / frameName(i), staging / frameName(i), ec);
        if (ec)
            return Error{Failure::PublicationFailure, 0, "公開用 PNG をコピーできません"};
    }
    if (observer)
        observer(PublicationStage::Validate);
    if (auto error = checkAuthority())
        return *error;
    auto copied = inspectFrames(staging, request.spec, request.toolchain, &stop);
    if (auto* error = std::get_if<Error>(&copied))
        return *error;
    if (std::get<Validation>(copied).manifest != validated.manifest)
        return corrupt("検証後に PNG が変わりました");
    // manifest は全 frame の検証後に書く。directory rename は既存 artifact を置換しない。
    char writeError[512]{};
    const auto manifestPath = std::filesystem::absolute(staging / "manifest.txt").wstring();
    if (mvm_atomic_write_file(manifestPath.c_str(), validated.manifest.data(),
                              validated.manifest.size(), writeError, sizeof(writeError)) != 0)
        return Error{Failure::PublicationFailure, 0, "manifest を確定できません"};
    {
        std::lock_guard lock(mutex_);
        if (auto error = guard())
            return *error;
        // この非置換 rename が公開の線形化点。重い検証は mutex の外で行う。
        std::filesystem::rename(staging, target, ec);
    }
    if (ec) {
        if (std::filesystem::exists(target))
            return readPublished();
        return Error{Failure::PublicationFailure, 0, "artifact を公開できません"};
    }
    return readPublished();
}
} // namespace mvm::graph
