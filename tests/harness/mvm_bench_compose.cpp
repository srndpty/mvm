// mvm Phase 0 / S5 + S6 - 合成と seek / scrub の検証
//
//   compose <scenario.json> --frame <n> --output <png>
//   verify-compose <scenario.json> [--output-dir <dir>]
//   seek-bench <scenario.json> [--random <n>] [--seed <s>] [--csv <path>]
//   scrub-bench <scenario.json> [--requests <n>] [--pattern <name>] [--csv <path>]
//
// 判定の方針:
//   目視で合格にしない。ピクセルは一点ではなく小領域の統計で見る。
//   golden PNG のバイト一致は要求しない (圧縮とアンチエイリアスに弱いため)。

#include "bench_common.h"
#include "media/mlt/mvm_mlt_audiograph.h"
#include "media/mlt/mvm_mlt_compose.h"
#include "scrub_coalescer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <psapi.h>
#include <random>
#include <set>
#include <thread>
#include <tlhelp32.h>

namespace {

using namespace bench;

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// --------------------------------------------------------------------------
// シナリオ読み込み
// --------------------------------------------------------------------------

struct Region {
    int x = 0, y = 0, w = 0, h = 0;

    bool valid() const { return w > 0 && h > 0; }
};

struct AudioExpect {
    std::vector<long long> frames;
    int sampleRate = 48000;
    int channels = 2;
    std::vector<double> goertzelHz;
    double minRms = 0.0;
    double maxPeak = 1.0;
};

struct Scenario {
    std::string name;
    std::string path;
    MvmBenchTimeline timeline{};
    std::vector<long long> verifyFrames;
    std::map<std::string, Region> regions;
    AudioExpect audio;
};

std::string dirOf(const std::string& p) {
    fs::path fp = utf8Path(p);
    char* u = mvm_wide_to_utf8(fp.parent_path().wstring().c_str());
    std::string out = u ? u : "";
    mvm_str_free(u);
    return out;
}

std::string joinPath(const std::string& base, const std::string& rel) {
    fs::path p = utf8Path(base) / utf8Path(rel);
    p = p.lexically_normal();
    char* u = mvm_wide_to_utf8(p.wstring().c_str());
    std::string out = u ? u : "";
    mvm_str_free(u);
    return out;
}

void copyStr(char* dst, size_t n, const std::string& s) {
    std::snprintf(dst, n, "%s", s.c_str());
}

bool loadScenario(const std::string& path, Scenario& out, std::string& err) {
    auto text = readFileUtf8(path);
    if (!text) {
        err = "シナリオを読めません: " + path;
        return false;
    }
    JsonParser parser(*text);
    auto root = parser.parse();
    if (!root || root->type != JsonValue::Type::Object) {
        err = "シナリオを解析できません: " + path;
        return false;
    }

    auto* schema = root->find("schema_version");
    if (!schema || schema->asInt() != 1) {
        err = "schema_version が無いか対応外です";
        return false;
    }

    out.path = path;
    out.name = root->find("name") ? root->find("name")->asString() : "(no name)";

    // media_root はシナリオファイルからの相対。絶対パスをコミットしないため。
    std::string base = dirOf(path);
    std::string mediaRoot =
        root->find("media_root") ? joinPath(base, root->find("media_root")->asString()) : base;

    auto* prof = root->find("profile");
    if (!prof) {
        err = "profile がありません";
        return false;
    }
    MvmBenchTimeline& tl = out.timeline;
    copyStr(tl.profile_name, sizeof(tl.profile_name), prof->find("name")->asString());
    tl.width = (int)prof->find("width")->asInt();
    tl.height = (int)prof->find("height")->asInt();
    tl.fps_num = (int)prof->find("fps_num")->asInt();
    tl.fps_den = (int)prof->find("fps_den")->asInt();
    tl.sar_num = prof->find("sar_num") ? (int)prof->find("sar_num")->asInt() : 0;
    tl.sar_den = prof->find("sar_den") ? (int)prof->find("sar_den")->asInt() : 0;
    tl.progressive = prof->find("progressive") ? (int)prof->find("progressive")->asInt() : 0;

    if (auto* ts = root->find("text_service"))
        copyStr(tl.text_service, sizeof(tl.text_service), ts->asString());
    copyStr(tl.video_transition, sizeof(tl.video_transition),
            root->find("video_transition") ? root->find("video_transition")->asString() : "affine");
    copyStr(tl.audio_mix_mode, sizeof(tl.audio_mix_mode),
            root->find("audio_mix_mode") ? root->find("audio_mix_mode")->asString() : "sum");
    if (auto* ff = root->find("font_file"))
        copyStr(tl.font_file, sizeof(tl.font_file), ff->asString());

    auto* tracks = root->find("tracks");
    if (!tracks || tracks->type != JsonValue::Type::Array || tracks->arr.empty()) {
        err = "tracks がありません";
        return false;
    }
    if ((int)tracks->arr.size() > MVM_BENCH_MAX_TRACKS) {
        err = "track が多すぎます";
        return false;
    }

    tl.track_count = 0;
    for (const auto& tj : tracks->arr) {
        MvmBenchTrack& tr = tl.tracks[tl.track_count];
        tr = MvmBenchTrack{};

        std::string kind = tj.find("kind") ? tj.find("kind")->asString() : "";
        if (kind == "video")
            tr.kind = MVM_BENCH_TRACK_VIDEO;
        else if (kind == "audio")
            tr.kind = MVM_BENCH_TRACK_AUDIO;
        else if (kind == "text")
            tr.kind = MVM_BENCH_TRACK_TEXT;
        else {
            err = "未知の track kind: '" + kind + "'";
            return false;
        }

        copyStr(tr.name, sizeof(tr.name), tj.find("name") ? tj.find("name")->asString() : "");
        tr.disabled = tj.find("disabled") ? (tj.find("disabled")->asBool() ? 1 : 0) : 0;
        tr.z_order = tj.find("z_order") ? (int)tj.find("z_order")->asInt() : tl.track_count;
        tr.video_enabled = tj.find("video_enabled") ? tj.find("video_enabled")->asBool() : 1;
        tr.audio_enabled = tj.find("audio_enabled") ? tj.find("audio_enabled")->asBool() : 1;

        auto* clips = tj.find("clips");
        if (!clips || clips->type != JsonValue::Type::Array || clips->arr.empty()) {
            err = "track に clips がありません";
            return false;
        }
        if ((int)clips->arr.size() > MVM_BENCH_MAX_CLIPS) {
            err = "clip が多すぎます";
            return false;
        }

        tr.clip_count = 0;
        for (const auto& cj : clips->arr) {
            MvmBenchClip& c = tr.clips[tr.clip_count];
            c = MvmBenchClip{};

            if (auto* s = cj.find("source"))
                copyStr(c.source, sizeof(c.source), joinPath(mediaRoot, s->asString()));

            c.timeline_in = cj.find("timeline_in") ? cj.find("timeline_in")->asInt() : 0;
            c.timeline_out = cj.find("timeline_out") ? cj.find("timeline_out")->asInt() : 0;
            c.source_in = cj.find("source_in") ? cj.find("source_in")->asInt() : 0;
            c.source_out = cj.find("source_out") ? cj.find("source_out")->asInt() : 0;

            c.rect_x = cj.find("rect_x") ? cj.find("rect_x")->asDouble() : 0;
            c.rect_y = cj.find("rect_y") ? cj.find("rect_y")->asDouble() : 0;
            c.rect_w = cj.find("rect_w") ? cj.find("rect_w")->asDouble() : 0;
            c.rect_h = cj.find("rect_h") ? cj.find("rect_h")->asDouble() : 0;
            c.opacity = cj.find("opacity") ? cj.find("opacity")->asDouble() : 1.0;
            c.gain_db = cj.find("gain_db") ? cj.find("gain_db")->asDouble() : 0.0;

            if (auto* t = cj.find("text"))
                copyStr(c.text, sizeof(c.text), t->asString());
            copyStr(c.font_family, sizeof(c.font_family),
                    cj.find("font_family") ? cj.find("font_family")->asString() : "Sans");
            c.font_size = cj.find("font_size") ? (int)cj.find("font_size")->asInt() : 48;
            copyStr(c.fg_colour, sizeof(c.fg_colour),
                    cj.find("fg_colour") ? cj.find("fg_colour")->asString() : "0xffffffff");
            copyStr(c.bg_colour, sizeof(c.bg_colour),
                    cj.find("bg_colour") ? cj.find("bg_colour")->asString() : "0x00000000");
            copyStr(c.halign, sizeof(c.halign),
                    cj.find("halign") ? cj.find("halign")->asString() : "left");
            copyStr(c.valign, sizeof(c.valign),
                    cj.find("valign") ? cj.find("valign")->asString() : "top");
            c.text_x = cj.find("text_x") ? (int)cj.find("text_x")->asInt() : 0;
            c.text_y = cj.find("text_y") ? (int)cj.find("text_y")->asInt() : 0;
            c.text_w = cj.find("text_w") ? (int)cj.find("text_w")->asInt() : tl.width;
            c.text_h = cj.find("text_h") ? (int)cj.find("text_h")->asInt() : tl.height;

            tr.clip_count++;
        }
        tl.track_count++;
    }

    // 検証設定
    if (auto* v = root->find("verify")) {
        if (auto* fr = v->find("frames")) {
            for (const auto& f : fr->arr)
                out.verifyFrames.push_back(f.asInt());
        }
        if (auto* rg = v->find("regions")) {
            if (rg->type == JsonValue::Type::Object && rg->obj) {
                for (const auto& kv : *rg->obj) {
                    if (kv.second.type != JsonValue::Type::Object)
                        continue; // "//" コメントを飛ばす
                    Region r;
                    if (auto* x = kv.second.find("x"))
                        r.x = (int)x->asInt();
                    if (auto* y = kv.second.find("y"))
                        r.y = (int)y->asInt();
                    if (auto* w = kv.second.find("w"))
                        r.w = (int)w->asInt();
                    if (auto* h = kv.second.find("h"))
                        r.h = (int)h->asInt();
                    if (r.valid())
                        out.regions[kv.first] = r;
                }
            }
        }
        if (auto* au = v->find("audio")) {
            if (auto* fr = au->find("frames"))
                for (const auto& f : fr->arr)
                    out.audio.frames.push_back(f.asInt());
            if (auto* sr = au->find("expect_sample_rate"))
                out.audio.sampleRate = (int)sr->asInt();
            if (auto* ch = au->find("expect_channels"))
                out.audio.channels = (int)ch->asInt();
            if (auto* g = au->find("goertzel_hz"))
                for (const auto& f : g->arr)
                    out.audio.goertzelHz.push_back(f.asDouble());
            if (auto* m = au->find("min_rms"))
                out.audio.minRms = m->asDouble();
            if (auto* m = au->find("max_peak"))
                out.audio.maxPeak = m->asDouble();
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// 領域統計
// --------------------------------------------------------------------------
// 一点のピクセル値で判定すると、圧縮ノイズとアンチエイリアスで簡単に揺れる。
// 小領域の平均・分散・差分率で見る。

struct RegionStats {
    double meanR = 0, meanG = 0, meanB = 0, meanA = 0;
    double variance = 0; // 輝度の分散
    int pixels = 0;
};

RegionStats regionStats(const unsigned char* rgba, int w, int h, const Region& r) {
    RegionStats s;
    if (!rgba)
        return s;
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return s;

    double sr = 0, sg = 0, sb = 0, sa = 0;
    std::vector<double> luma;
    luma.reserve((size_t)((x1 - x0) * (y1 - y0)));
    for (int y = y0; y < y1; y++) {
        const unsigned char* row = rgba + (size_t)y * (size_t)w * 4u;
        for (int x = x0; x < x1; x++) {
            const unsigned char* px = row + (size_t)x * 4u;
            sr += px[0];
            sg += px[1];
            sb += px[2];
            sa += px[3];
            luma.push_back(0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]);
        }
    }
    s.pixels = (int)luma.size();
    if (s.pixels == 0)
        return s;
    s.meanR = sr / s.pixels;
    s.meanG = sg / s.pixels;
    s.meanB = sb / s.pixels;
    s.meanA = sa / s.pixels;

    double mean = std::accumulate(luma.begin(), luma.end(), 0.0) / s.pixels;
    double acc = 0;
    for (double v : luma)
        acc += (v - mean) * (v - mean);
    s.variance = acc / s.pixels;
    return s;
}

// 2 つの領域が「同じ絵か」を平均色の距離で見る
double meanColourDistance(const RegionStats& a, const RegionStats& b) {
    double dr = a.meanR - b.meanR, dg = a.meanG - b.meanG, db = a.meanB - b.meanB;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// --------------------------------------------------------------------------
// Goertzel 法による単一周波数の強度
// --------------------------------------------------------------------------
// 既知の周波数だけを見たいので FFT は不要。

double goertzel(const float* samples, int count, int stride, double sampleRate, double freq) {
    if (count <= 0 || sampleRate <= 0)
        return 0.0;
    double k = std::round((count * freq) / sampleRate);
    double omega = (2.0 * 3.14159265358979323846 * k) / count;
    double coeff = 2.0 * std::cos(omega);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < count; i++) {
        s0 = samples[(size_t)i * (size_t)stride] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return std::sqrt(std::max(0.0, power)) / (count / 2.0);
}

double rms(const float* samples, int count, int stride) {
    if (count <= 0)
        return 0.0;
    double acc = 0;
    for (int i = 0; i < count; i++) {
        double v = samples[(size_t)i * (size_t)stride];
        acc += v * v;
    }
    return std::sqrt(acc / count);
}

double peak(const float* samples, int count, int stride) {
    double p = 0;
    for (int i = 0; i < count; i++)
        p = std::max(p, (double)std::fabs(samples[(size_t)i * (size_t)stride]));
    return p;
}

// --------------------------------------------------------------------------
// 共通: シナリオを開く
// --------------------------------------------------------------------------

struct OpenedScenario {
    Scenario scenario;
    MvmComposeInfo info{};
    MvmComposeHandle* handle = nullptr;
};

// --------------------------------------------------------------------------
// A/B 差分
// --------------------------------------------------------------------------
// 領域の分散や別領域との色距離だけでは偽陽性になることが実測で分かっている。
// 「そのトラックが無いときの絵」との差分を見るのが唯一確実な判定である。

// 画素が「変化した」と見なす閾値。圧縮ノイズより十分大きく取る。
constexpr int kPixelDiffThreshold = 12;

bool pixelDiffers(const unsigned char* a, const unsigned char* b) {
    return std::abs((int)a[0] - (int)b[0]) > kPixelDiffThreshold ||
           std::abs((int)a[1] - (int)b[1]) > kPixelDiffThreshold ||
           std::abs((int)a[2] - (int)b[2]) > kPixelDiffThreshold;
}

// 矩形の内側で変化した画素の割合
double diffRatioInside(const MvmMltImage& a, const MvmMltImage& b, const Region& r) {
    if (!a.rgba || !b.rgba || a.width != b.width || a.height != b.height)
        return -1;
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(a.width, r.x + r.w), y1 = std::min(a.height, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return -1;
    long long total = 0, diff = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            size_t o = ((size_t)y * (size_t)a.width + (size_t)x) * 4u;
            total++;
            if (pixelDiffers(a.rgba + o, b.rgba + o))
                diff++;
        }
    }
    return total ? (double)diff / (double)total : -1;
}

// 矩形の外側で変化した画素の割合
double diffRatioOutside(const MvmMltImage& a, const MvmMltImage& b, const Region& r) {
    if (!a.rgba || !b.rgba || a.width != b.width || a.height != b.height)
        return -1;
    long long total = 0, diff = 0;
    for (int y = 0; y < a.height; y++) {
        for (int x = 0; x < a.width; x++) {
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                continue;
            size_t o = ((size_t)y * (size_t)a.width + (size_t)x) * 4u;
            total++;
            if (pixelDiffers(a.rgba + o, b.rgba + o))
                diff++;
        }
    }
    return total ? (double)diff / (double)total : -1;
}

// 差分が存在する範囲の外接矩形。PiP の実際の位置とサイズを測るのに使う。
Region diffBoundingBox(const MvmMltImage& a, const MvmMltImage& b) {
    Region out{};
    if (!a.rgba || !b.rgba || a.width != b.width || a.height != b.height)
        return out;
    int minX = a.width, minY = a.height, maxX = -1, maxY = -1;
    for (int y = 0; y < a.height; y++) {
        for (int x = 0; x < a.width; x++) {
            size_t o = ((size_t)y * (size_t)a.width + (size_t)x) * 4u;
            if (pixelDiffers(a.rgba + o, b.rgba + o)) {
                if (x < minX)
                    minX = x;
                if (y < minY)
                    minY = y;
                if (x > maxX)
                    maxX = x;
                if (y > maxY)
                    maxY = y;
            }
        }
    }
    if (maxX < 0)
        return out;
    out.x = minX;
    out.y = minY;
    out.w = maxX - minX + 1;
    out.h = maxY - minY + 1;
    return out;
}

bool disableTrackByName(MvmBenchTimeline& tl, const char* name) {
    for (int i = 0; i < tl.track_count; i++) {
        if (std::strcmp(tl.tracks[i].name, name) == 0) {
            tl.tracks[i].disabled = 1;
            return true;
        }
    }
    return false;
}

// タイムラインの 1 バリアント。full / no_v2 / no_text を同時に持つために使う。
struct Variant {
    MvmBenchTimeline timeline;
    MvmComposeInfo info{};
    MvmComposeHandle* handle = nullptr;

    explicit Variant(const MvmBenchTimeline& t) : timeline(t) {}

    ~Variant() {
        if (handle)
            mvm_mlt_compose_close(handle);
    }

    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;

    bool open(char* err, size_t n) {
        handle = mvm_mlt_compose_open(&timeline, &info, err, n);
        return handle != nullptr;
    }
};

bool openScenario(const Args& a, OpenedScenario& out, int& rc) {
    if (a.positional.empty()) {
        logMsg("シナリオファイルを指定してください");
        rc = kExitUsage;
        return false;
    }
    std::string err;
    if (!loadScenario(a.positional[0], out.scenario, err)) {
        logMsg(err);
        logMsg("素材が未生成の場合は:");
        logMsg("    pwsh scripts/make-testmedia.ps1 -Mode Smoke");
        rc = kExitError;
        return false;
    }
    if (!initMlt()) {
        rc = kExitError;
        return false;
    }

    // シナリオ側から text_service を上書きできる (qtext と dynamictext の比較用)
    if (a.has("text-service"))
        copyStr(out.scenario.timeline.text_service, sizeof(out.scenario.timeline.text_service),
                a.get("text-service"));
    if (a.has("font-file"))
        copyStr(out.scenario.timeline.font_file, sizeof(out.scenario.timeline.font_file),
                a.get("font-file"));
    if (a.has("video-transition"))
        copyStr(out.scenario.timeline.video_transition,
                sizeof(out.scenario.timeline.video_transition), a.get("video-transition"));
    if (a.has("mix-mode"))
        copyStr(out.scenario.timeline.audio_mix_mode, sizeof(out.scenario.timeline.audio_mix_mode),
                a.get("mix-mode"));

    char cerr[1024] = {0};
    out.handle = mvm_mlt_compose_open(&out.scenario.timeline, &out.info, cerr, sizeof(cerr));
    if (!out.handle) {
        logMsg(std::string("タイムラインを構築できません: ") + cerr);
        mvm_mlt_runtime_shutdown();
        rc = kExitMismatch;
        return false;
    }
    return true;
}

void closeScenario(OpenedScenario& s) {
    if (s.handle)
        mvm_mlt_compose_close(s.handle);
    s.handle = nullptr;
    mvm_mlt_runtime_shutdown();
}

void printComposeInfo(const MvmComposeInfo& info, std::ostream& os) {
    os << "  \"profile\": { \"width\": " << info.profile_width
       << ", \"height\": " << info.profile_height << ", \"fps_num\": " << info.profile_fps_num
       << ", \"fps_den\": " << info.profile_fps_den << ", \"sar_num\": " << info.profile_sar_num
       << ", \"sar_den\": " << info.profile_sar_den
       << ", \"progressive\": " << info.profile_progressive << " },\n";
    os << "  \"length\": " << info.length << ",\n";
    os << "  \"track_count\": " << info.track_count << ",\n";
    os << "  \"services\": [";
    for (int i = 0; i < info.note_count; i++) {
        os << (i ? ",\n    " : "\n    ") << "{ \"subject\": \"" << jsonEscape(info.notes[i].subject)
           << "\", \"detail\": \"" << jsonEscape(info.notes[i].detail) << "\" }";
    }
    os << (info.note_count ? "\n  ],\n" : "],\n");
}

} // namespace

// --------------------------------------------------------------------------
// compose
// --------------------------------------------------------------------------

int cmdCompose(const bench::Args& a) {
    using namespace bench;

    OpenedScenario s;
    int rc = kExitOk;
    if (!openScenario(a, s, rc))
        return rc;

    long long frame = std::strtoll(a.get("frame", "0").c_str(), nullptr, 10);

    MvmMltImage img{};
    char err[512] = {0};
    if (mvm_mlt_compose_frame(s.handle, frame, &img, err, sizeof(err)) != 0) {
        logMsg(std::string("合成フレームを取得できません: ") + err);
        closeScenario(s);
        return kExitMismatch;
    }

    MarkerRead marker = readMarkerAuto(img.rgba, img.width, img.height);

    bool wrote = false;
    std::string pngErr;
    std::string outPng = a.get("output");
    if (!outPng.empty() && outPng != "1")
        wrote = writePng(outPng, img.rgba, img.width, img.height, pngErr);

    std::ostringstream js;
    js << "{\n";
    js << "  \"scenario\": \"" << jsonEscape(s.scenario.name) << "\",\n";
    js << "  \"frame\": " << frame << ",\n";
    js << "  \"width\": " << img.width << ",\n  \"height\": " << img.height << ",\n";
    js << "  \"text_service\": \"" << jsonEscape(s.scenario.timeline.text_service) << "\",\n";
    printComposeInfo(s.info, js);
    js << "  \"marker_sync_ok\": " << (marker.syncOk ? "true" : "false") << ",\n";
    js << "  \"marker_value\": " << marker.value << ",\n";

    js << "  \"regions\": {";
    bool first = true;
    for (const auto& kv : s.scenario.regions) {
        RegionStats st = regionStats(img.rgba, img.width, img.height, kv.second);
        js << (first ? "\n    " : ",\n    ");
        first = false;
        js << "\"" << kv.first << "\": { \"mean_r\": " << st.meanR << ", \"mean_g\": " << st.meanG
           << ", \"mean_b\": " << st.meanB << ", \"mean_a\": " << st.meanA
           << ", \"variance\": " << st.variance << ", \"pixels\": " << st.pixels << " }";
    }
    js << (first ? "},\n" : "\n  },\n");
    js << "  \"png_written\": " << (wrote ? "true" : "false");
    if (!pngErr.empty())
        js << ",\n  \"png_error\": \"" << jsonEscape(pngErr) << "\"";
    js << "\n}\n";

    std::fputs(js.str().c_str(), stdout);

    mvm_mlt_image_free(&img);
    closeScenario(s);

    if (!outPng.empty() && outPng != "1" && !wrote) {
        logMsg("PNG を書き出せません: " + pngErr);
        return kExitError;
    }
    return kExitOk;
}

// --------------------------------------------------------------------------
// verify-compose
// --------------------------------------------------------------------------

int cmdVerifyCompose(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("シナリオファイルを指定してください");
        return kExitUsage;
    }

    Scenario base;
    std::string lerr;
    if (!loadScenario(a.positional[0], base, lerr)) {
        logMsg(lerr);
        logMsg("素材が未生成の場合は:  pwsh scripts/make-testmedia.ps1 -Mode Smoke");
        return kExitError;
    }
    if (a.has("text-service"))
        copyStr(base.timeline.text_service, sizeof(base.timeline.text_service),
                a.get("text-service"));
    if (a.has("font-file"))
        copyStr(base.timeline.font_file, sizeof(base.timeline.font_file), a.get("font-file"));
    if (a.has("video-transition"))
        copyStr(base.timeline.video_transition, sizeof(base.timeline.video_transition),
                a.get("video-transition"));

    if (!initMlt())
        return kExitError;

    std::vector<std::string> problems;
    std::string outDir = a.get("output-dir");

    // full / no_v2 / no_text の 3 系統を作る。
    Variant full(base.timeline), noV2(base.timeline), noText(base.timeline);
    if (!disableTrackByName(noV2.timeline, "V2"))
        problems.push_back("シナリオに name=V2 のトラックがありません");
    if (!disableTrackByName(noText.timeline, "T1"))
        problems.push_back("シナリオに name=T1 のトラックがありません");

    char cerr[1024] = {0};
    if (!full.open(cerr, sizeof(cerr))) {
        logMsg(std::string("full の構築に失敗: ") + cerr);
        mvm_mlt_runtime_shutdown();
        return kExitMismatch;
    }
    if (!noV2.open(cerr, sizeof(cerr)))
        problems.push_back(std::string("no_v2 の構築に失敗: ") + cerr);
    if (!noText.open(cerr, sizeof(cerr)))
        problems.push_back(std::string("no_text の構築に失敗: ") + cerr);

    Region pipRect{}, textRect{};
    if (base.regions.count("pip_rect"))
        pipRect = base.regions.at("pip_rect");
    if (base.regions.count("text"))
        textRect = base.regions.at("text");
    if (!pipRect.valid())
        problems.push_back("verify.regions に pip_rect が必要です (PiP の期待矩形)");
    if (!textRect.valid())
        problems.push_back("verify.regions に text が必要です");

    const double insideMin =
        a.has("inside-min-diff") ? std::strtod(a.get("inside-min-diff").c_str(), nullptr) : 0.20;
    const double outsideMax =
        a.has("outside-max-diff") ? std::strtod(a.get("outside-max-diff").c_str(), nullptr) : 0.02;
    const int geomTol = a.has("geom-tolerance")
                            ? (int)std::strtol(a.get("geom-tolerance").c_str(), nullptr, 10)
                            : 8;

    std::ostringstream js;
    js << "{\n  \"scenario\": \"" << jsonEscape(base.name) << "\",\n";
    js << "  \"text_service\": \"" << jsonEscape(base.timeline.text_service) << "\",\n";
    js << "  \"video_transition\": \"" << jsonEscape(base.timeline.video_transition) << "\",\n";
    js << "  \"font_file\": \"" << jsonEscape(base.timeline.font_file) << "\",\n";
    js << "  \"inside_min_diff\": " << insideMin << ",\n";
    js << "  \"outside_max_diff\": " << outsideMax << ",\n";
    printComposeInfo(full.info, js);
    js << "  \"frames\": [";

    int videoChecks = 0;
    bool firstFrame = true;

    for (long long frame : base.verifyFrames) {
        MvmMltImage imgFull{}, imgNoV2{}, imgNoText{};
        char e1[512] = {0}, e2[512] = {0}, e3[512] = {0};

        if (mvm_mlt_compose_frame(full.handle, frame, &imgFull, e1, sizeof(e1)) != 0) {
            problems.push_back("frame " + std::to_string(frame) + " (full): " + e1);
            continue;
        }
        bool haveNoV2 =
            noV2.handle && mvm_mlt_compose_frame(noV2.handle, frame, &imgNoV2, e2, sizeof(e2)) == 0;
        bool haveNoText = noText.handle && mvm_mlt_compose_frame(noText.handle, frame, &imgNoText,
                                                                 e3, sizeof(e3)) == 0;
        if (noV2.handle && !haveNoV2)
            problems.push_back("frame " + std::to_string(frame) + " (no_v2): " + e2);
        if (noText.handle && !haveNoText)
            problems.push_back("frame " + std::to_string(frame) + " (no_text): " + e3);

        videoChecks++;
        MarkerRead marker = readMarkerAuto(imgFull.rgba, imgFull.width, imgFull.height);
        if (!marker.syncOk)
            problems.push_back("frame " + std::to_string(frame) + ": マーカー同期が取れません");
        else if (marker.value != frame)
            problems.push_back("frame " + std::to_string(frame) +
                               ": マーカー不一致 marker=" + std::to_string(marker.value));

        double pipIn = -1, pipOut = -1, txIn = -1, txOut = -1;
        Region measured{};

        if (haveNoV2 && pipRect.valid()) {
            pipIn = diffRatioInside(imgFull, imgNoV2, pipRect);
            pipOut = diffRatioOutside(imgFull, imgNoV2, pipRect);
            measured = diffBoundingBox(imgFull, imgNoV2);

            if (pipIn < insideMin)
                problems.push_back("frame " + std::to_string(frame) +
                                   ": PiP 矩形内の差分率が小さすぎます " + std::to_string(pipIn) +
                                   " < " + std::to_string(insideMin) + " (V2 が合成されていない)");
            if (pipOut > outsideMax)
                problems.push_back("frame " + std::to_string(frame) +
                                   ": PiP 矩形外へ V2 の画素が漏れています 差分率 " +
                                   std::to_string(pipOut) + " > " + std::to_string(outsideMax));
            if (std::abs(measured.x - pipRect.x) > geomTol ||
                std::abs(measured.y - pipRect.y) > geomTol ||
                std::abs(measured.w - pipRect.w) > geomTol ||
                std::abs(measured.h - pipRect.h) > geomTol) {
                problems.push_back(
                    "frame " + std::to_string(frame) + ": PiP の位置/サイズが期待と違います 実測 " +
                    std::to_string(measured.x) + "," + std::to_string(measured.y) + " " +
                    std::to_string(measured.w) + "x" + std::to_string(measured.h) + " 期待 " +
                    std::to_string(pipRect.x) + "," + std::to_string(pipRect.y) + " " +
                    std::to_string(pipRect.w) + "x" + std::to_string(pipRect.h));
            }
        }

        if (haveNoText && textRect.valid()) {
            txIn = diffRatioInside(imgFull, imgNoText, textRect);
            txOut = diffRatioOutside(imgFull, imgNoText, textRect);
            if (txIn < insideMin)
                problems.push_back("frame " + std::to_string(frame) +
                                   ": text 矩形内の差分率が小さすぎます " + std::to_string(txIn) +
                                   " < " + std::to_string(insideMin) + " (文字が描かれていない)");
            if (txOut > outsideMax)
                problems.push_back("frame " + std::to_string(frame) +
                                   ": text 矩形外へ描画が漏れています 差分率 " +
                                   std::to_string(txOut) + " > " + std::to_string(outsideMax));
        }

        if (!outDir.empty() && outDir != "1") {
            std::string pe;
            writePng(outDir + "/full_" + std::to_string(frame) + ".png", imgFull.rgba,
                     imgFull.width, imgFull.height, pe);
        }

        js << (firstFrame ? "\n    " : ",\n    ");
        firstFrame = false;
        js << "{ \"frame\": " << frame << ", \"marker_value\": " << marker.value
           << ", \"pip_inside_diff\": " << pipIn << ", \"pip_outside_diff\": " << pipOut
           << ", \"pip_bbox\": { \"x\": " << measured.x << ", \"y\": " << measured.y
           << ", \"w\": " << measured.w << ", \"h\": " << measured.h << " }"
           << ", \"text_inside_diff\": " << txIn << ", \"text_outside_diff\": " << txOut << " }";

        mvm_mlt_image_free(&imgFull);
        if (haveNoV2)
            mvm_mlt_image_free(&imgNoV2);
        if (haveNoText)
            mvm_mlt_image_free(&imgNoText);
    }
    js << (firstFrame ? "],\n" : "\n  ],\n");

    int audioChecks = 0;
    js << "  \"audio\": [";
    bool firstAudio = true;
    for (long long frame : base.audio.frames) {
        MvmComposeAudio au{};
        char err[512] = {0};
        if (mvm_mlt_compose_audio(full.handle, frame, &au, err, sizeof(err)) != 0) {
            problems.push_back("audio frame " + std::to_string(frame) + ": " + err);
            continue;
        }
        audioChecks++;

        if (au.sample_rate != base.audio.sampleRate)
            problems.push_back("audio frame " + std::to_string(frame) + ": sample_rate " +
                               std::to_string(au.sample_rate));
        if (au.channels != base.audio.channels)
            problems.push_back("audio frame " + std::to_string(frame) + ": channels " +
                               std::to_string(au.channels));

        double rmsL = rms(au.data, au.samples, au.channels);
        double rmsR = rms(au.data + 1, au.samples, au.channels);
        double peakL = peak(au.data, au.samples, au.channels);
        double peakR = peak(au.data + 1, au.samples, au.channels);

        if (rmsL < base.audio.minRms || rmsR < base.audio.minRms)
            problems.push_back("audio frame " + std::to_string(frame) + ": 無音に近い rmsL " +
                               std::to_string(rmsL) + " rmsR " + std::to_string(rmsR));
        if (peakL > base.audio.maxPeak || peakR > base.audio.maxPeak)
            problems.push_back("audio frame " + std::to_string(frame) + ": clipping peak " +
                               std::to_string(std::max(peakL, peakR)));

        js << (firstAudio ? "\n    " : ",\n    ");
        firstAudio = false;
        js << "{ \"frame\": " << frame << ", \"sample_rate\": " << au.sample_rate
           << ", \"channels\": " << au.channels << ", \"samples\": " << au.samples
           << ", \"rms_l\": " << rmsL << ", \"rms_r\": " << rmsR << ", \"peak_l\": " << peakL
           << ", \"peak_r\": " << peakR;
        for (double hz : base.audio.goertzelHz) {
            double gl = goertzel(au.data, au.samples, au.channels, au.sample_rate, hz);
            double gr = goertzel(au.data + 1, au.samples, au.channels, au.sample_rate, hz);
            js << ", \"g" << (long long)hz << "_l\": " << gl << ", \"g" << (long long)hz
               << "_r\": " << gr;
        }
        js << " }";
        mvm_mlt_audio_free(&au);
    }
    js << (firstAudio ? "],\n" : "\n  ],\n");

    if (videoChecks == 0)
        problems.push_back("映像の検査が 1 件も実行されていません");
    if (audioChecks == 0)
        problems.push_back("音声の検査が 1 件も実行されていません");

    js << "  \"video_checks\": " << videoChecks << ",\n";
    js << "  \"audio_checks\": " << audioChecks << ",\n";
    js << "  \"problems\": [";
    for (size_t i = 0; i < problems.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(problems[i]) << "\"";
    js << (problems.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::fputs(js.str().c_str(), stdout);
    mvm_mlt_runtime_shutdown();

    if (!problems.empty()) {
        logMsg("合成検証に失敗しました:");
        for (const auto& p : problems)
            logMsg("  " + p);
        return kExitMismatch;
    }
    return kExitOk;
}

// --------------------------------------------------------------------------
// seek-bench
// --------------------------------------------------------------------------

namespace {

struct SeekSample {
    long long requested = 0;
    long long markerValue = -1;
    bool markerSync = false;
    bool match = false;
    double latencyMs = 0;
    bool cold = false;
    long long distance = 0;
    const char* category = "";
};

struct Percentiles {
    double p50 = 0, p95 = 0, max = 0, mean = 0, stddev = 0;
};

Percentiles computeStats(std::vector<double> v) {
    Percentiles p;
    if (v.empty())
        return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        size_t idx = (size_t)std::llround(q * (double)(v.size() - 1));
        return v[std::min(idx, v.size() - 1)];
    };
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.max = v.back();
    p.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double acc = 0;
    for (double x : v)
        acc += (x - p.mean) * (x - p.mean);
    p.stddev = std::sqrt(acc / v.size());
    return p;
}

void emitStats(std::ostream& os, const char* name, const Percentiles& p, size_t count) {
    os << "\"" << name << "\": { \"count\": " << count << ", \"p50\": " << p.p50
       << ", \"p95\": " << p.p95 << ", \"max\": " << p.max << ", \"mean\": " << p.mean
       << ", \"stddev\": " << p.stddev << " }";
}

} // namespace

int cmdSeekBench(const bench::Args& a) {
    using namespace bench;

    OpenedScenario s;
    int rc = kExitOk;
    if (!openScenario(a, s, rc))
        return rc;

    long long length = mvm_mlt_compose_length(s.handle);
    int randomCount = (int)std::strtol(a.get("random", "200").c_str(), nullptr, 10);
    unsigned seed = (unsigned)std::strtoul(a.get("seed", "20260804").c_str(), nullptr, 10);

    // 要求フレーム集合。固定点を先に、続いて固定 seed のランダム点。
    std::vector<long long> requests;
    auto push = [&](long long f) {
        if (f >= 0 && f < length)
            requests.push_back(f);
    };
    push(0);
    push(1);
    push(length - 1);
    push(length - 2);
    // GOP 境界付近 (素材は -g 60 で生成している)
    for (long long g : {59LL, 60LL, 61LL, 119LL, 120LL, 121LL, 179LL, 180LL, 181LL})
        push(g);
    // クリップ境界付近 (今回のシナリオは単一クリップなので端が境界)
    push(length / 2);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<long long> dist(0, length - 1);
    for (int i = 0; i < randomCount; i++)
        requests.push_back(dist(rng));

    std::vector<SeekSample> samples;
    samples.reserve(requests.size());

    long long prev = -1;
    for (size_t i = 0; i < requests.size(); i++) {
        long long f = requests[i];
        SeekSample smp;
        smp.requested = f;
        smp.cold = (i == 0);
        smp.distance = (prev < 0) ? 0 : (f - prev);
        smp.category = (prev < 0)   ? "first"
                       : (f > prev) ? "forward"
                       : (f < prev) ? "backward"
                                    : "same";

        MvmMltImage img{};
        char err[512] = {0};

        // 計測は steady_clock。PNG 出力や JSON 整形は含めない。
        auto t0 = Clock::now();
        int r = mvm_mlt_compose_frame(s.handle, f, &img, err, sizeof(err));
        auto t1 = Clock::now();
        smp.latencyMs = msSince(t0, t1);

        if (r != 0) {
            logMsg("seek 失敗 frame=" + std::to_string(f) + ": " + err);
            samples.push_back(smp);
            prev = f;
            continue;
        }

        MarkerRead m = readMarkerAuto(img.rgba, img.width, img.height);
        smp.markerSync = m.syncOk;
        smp.markerValue = m.value;
        // 再試行はしない。retry で隠すと M4 の判定が意味を失う。
        smp.match = m.syncOk && (m.value == f);

        mvm_mlt_image_free(&img);
        samples.push_back(smp);
        prev = f;
    }

    // 集計
    std::vector<double> all, forward, backward, cold, warm;
    int mismatches = 0;
    for (const auto& smp : samples) {
        all.push_back(smp.latencyMs);
        if (!smp.match)
            mismatches++;
        if (smp.cold)
            cold.push_back(smp.latencyMs);
        else
            warm.push_back(smp.latencyMs);
        if (std::string(smp.category) == "forward")
            forward.push_back(smp.latencyMs);
        else if (std::string(smp.category) == "backward")
            backward.push_back(smp.latencyMs);
    }

    // CSV
    std::string csv = a.get("csv");
    if (!csv.empty() && csv != "1") {
        FILE* f = _wfopen(utf8Path(csv).c_str(), L"wb");
        if (f) {
            std::fprintf(f, "index,requested,marker_value,marker_sync,match,latency_ms,cold,"
                            "distance,category\n");
            for (size_t i = 0; i < samples.size(); i++) {
                const auto& smp = samples[i];
                std::fprintf(f, "%zu,%lld,%lld,%d,%d,%.4f,%d,%lld,%s\n", i, smp.requested,
                             smp.markerValue, smp.markerSync ? 1 : 0, smp.match ? 1 : 0,
                             smp.latencyMs, smp.cold ? 1 : 0, smp.distance, smp.category);
            }
            std::fclose(f);
        }
    }

    Percentiles pAll = computeStats(all);

    std::ostringstream js;
    js << "{\n  \"scenario\": \"" << jsonEscape(s.scenario.name) << "\",\n";
    js << "  \"length\": " << length << ",\n";
    js << "  \"count\": " << samples.size() << ",\n";
    js << "  \"success\": " << (samples.size() - (size_t)mismatches) << ",\n";
    js << "  \"mismatch\": " << mismatches << ",\n";
    js << "  \"latency_ms\": {\n    ";
    emitStats(js, "all", pAll, all.size());
    js << ",\n    ";
    emitStats(js, "cold", computeStats(cold), cold.size());
    js << ",\n    ";
    emitStats(js, "warm", computeStats(warm), warm.size());
    js << ",\n    ";
    emitStats(js, "forward", computeStats(forward), forward.size());
    js << ",\n    ";
    emitStats(js, "backward", computeStats(backward), backward.size());
    js << "\n  }\n}\n";

    std::fputs(js.str().c_str(), stdout);
    closeScenario(s);

    // 閾値。--check を付けたときだけ判定する (計測と判定を分ける)。
    if (a.has("check")) {
        double p95Limit = std::strtod(a.get("max-p95-ms", "150").c_str(), nullptr);
        double maxLimit = std::strtod(a.get("max-max-ms", "400").c_str(), nullptr);
        bool ok = true;
        if (mismatches != 0) {
            logMsg("M4 不合格: フレーム不一致 " + std::to_string(mismatches) + " 件");
            ok = false;
        }
        if (pAll.p95 > p95Limit) {
            logMsg("M5 不合格: p95 " + std::to_string(pAll.p95) + "ms > " +
                   std::to_string(p95Limit) + "ms");
            ok = false;
        }
        if (pAll.max > maxLimit) {
            logMsg("M5 不合格: max " + std::to_string(pAll.max) + "ms > " +
                   std::to_string(maxLimit) + "ms");
            ok = false;
        }
        if (!ok)
            return kExitMismatch;
    }
    return kExitOk;
}

// --------------------------------------------------------------------------
// scrub-bench
// --------------------------------------------------------------------------
//
// scrub では要求を逐次すべて処理しない。
// 未処理の要求は最新 1 件だけ保持し、古いものは supersede する。
// decode 中の処理は中断しない (途中で壊すと状態が読めなくなる)。

namespace {

// ScrubRequest / ScrubResult は scrub_coalescer.h で定義している。

std::vector<long long> makePattern(const std::string& name, long long length, int count,
                                   unsigned seed) {
    std::vector<long long> out;
    out.reserve((size_t)count);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<long long> uni(0, length - 1);

    if (name == "linear") {
        // 0 -> 最終 -> 0 を線形に往復
        long long span = length - 1;
        for (int i = 0; i < count; i++) {
            long long phase = (long long)i % (2 * span);
            out.push_back(phase < span ? phase : (2 * span - phase));
        }
    } else if (name == "random") {
        for (int i = 0; i < count; i++)
            out.push_back(uni(rng));
    } else if (name == "jump") {
        // 大きく前後へ跳ぶ
        for (int i = 0; i < count; i++)
            out.push_back((i % 2 == 0) ? (long long)(i % 4) : (length - 1 - (long long)(i % 4)));
    } else { // "fine"
        // 同一位置付近で細かく動く
        long long center = length / 2;
        std::uniform_int_distribution<long long> fine(-5, 5);
        for (int i = 0; i < count; i++) {
            long long f = center + fine(rng);
            out.push_back(std::max(0LL, std::min(length - 1, f)));
        }
    }
    return out;
}

} // namespace

int cmdScrubBench(const bench::Args& a) {
    using namespace bench;

    OpenedScenario s;
    int rc = kExitOk;
    if (!openScenario(a, s, rc))
        return rc;

    long long length = mvm_mlt_compose_length(s.handle);
    int count = (int)std::strtol(a.get("requests", "1000").c_str(), nullptr, 10);
    std::string pattern = a.get("pattern", "linear");
    unsigned seed = (unsigned)std::strtoul(a.get("seed", "20260804").c_str(), nullptr, 10);
    int submitIntervalUs = (int)std::strtol(a.get("submit-interval-us", "0").c_str(), nullptr, 10);

    std::vector<long long> frames = makePattern(pattern, length, count, seed);

    // coalescing と表示契約は scrub_coalescer.h の ScrubCoalescer にある。
    // ここはスレッドと MLT の接続だけを担当する。
    ScrubCoalescer coal;

    std::mutex mu;
    std::condition_variable cv;

    // 表示された結果 (DisplayLatest + DisplayLagging) だけを集計する。
    std::vector<double> displayedLatencies;
    std::vector<double> generationLag;
    std::vector<double> frameDistance;
    std::vector<double> submitAgeMs;
    long long markerMismatch = 0;
    long long displayNonMonotonic = 0;
    long long prevDisplayed = -1;
    long long lastDisplayedFrame = -1;
    Clock::time_point lastSubmitTime{};
    Clock::time_point finalDisplayTime{};
    bool haveFinalDisplay = false;
    std::vector<std::string> contractErrors;

    std::vector<Clock::time_point> submitTime((size_t)frames.size());

    auto tStart = Clock::now();

    std::thread consumer([&] {
        for (;;) {
            ScrubRequest req;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return coal.hasPending() || coal.isDone(); });
                auto taken = coal.takePending();
                if (!taken) {
                    if (coal.isDone())
                        return;
                    continue;
                }
                req = *taken;
            }

            // decode はロックの外。in-flight は中断しない。
            MvmMltImage img{};
            char err[512] = {0};
            int r = mvm_mlt_compose_frame(s.handle, req.frame, &img, err, sizeof(err));
            auto tDone = Clock::now();

            ScrubResult res;
            res.generation = req.generation;
            res.frame = req.frame;
            res.decodeOk = (r == 0);

            long long markerValue = -1;
            bool markerSync = false;
            if (r == 0) {
                MarkerRead m = readMarkerAuto(img.rgba, img.width, img.height);
                markerSync = m.syncOk;
                markerValue = m.value;
                mvm_mlt_image_free(&img);
            }

            {
                std::lock_guard<std::mutex> lk(mu);
                long long latestAtCompletion = coal.latestSubmittedGeneration();
                long long latestFrameAtCompletion =
                    (latestAtCompletion >= 0 && latestAtCompletion < (long long)frames.size())
                        ? frames[(size_t)latestAtCompletion]
                        : req.frame;
                ScrubResultDecision d = coal.complete(res);
                switch (d) {
                case ScrubResultDecision::DisplayLatest:
                case ScrubResultDecision::DisplayLagging: {
                    displayedLatencies.push_back(
                        msSince(submitTime[(size_t)req.generation], tDone));
                    generationLag.push_back((double)(latestAtCompletion - req.generation));
                    frameDistance.push_back(
                        (double)std::llabs(latestFrameAtCompletion - req.frame));
                    if (latestAtCompletion >= 0 &&
                        latestAtCompletion < (long long)submitTime.size())
                        submitAgeMs.push_back(msSince(submitTime[(size_t)req.generation],
                                                      submitTime[(size_t)latestAtCompletion]));
                    // marker は表示された結果だけを対象に集計する
                    if (!markerSync || markerValue != req.frame)
                        markerMismatch++;
                    if (req.generation <= prevDisplayed)
                        displayNonMonotonic++;
                    prevDisplayed = req.generation;
                    lastDisplayedFrame = req.frame;
                    finalDisplayTime = tDone;
                    haveFinalDisplay = true;
                    break;
                }
                case ScrubResultDecision::RejectRegression:
                    break; // 巻き戻るので表示しない
                case ScrubResultDecision::DecodeFailed:
                    contractErrors.push_back("decode 失敗 gen=" + std::to_string(req.generation) +
                                             ": " + err);
                    break;
                case ScrubResultDecision::InvalidFutureGeneration:
                    contractErrors.push_back("未投入 generation の complete gen=" +
                                             std::to_string(req.generation));
                    break;
                case ScrubResultDecision::NotInFlight:
                    contractErrors.push_back("in-flight でない complete gen=" +
                                             std::to_string(req.generation));
                    break;
                }
            }
            cv.notify_all();
        }
    });

    for (long long i = 0; i < (long long)frames.size(); i++) {
        {
            std::lock_guard<std::mutex> lk(mu);
            auto now = Clock::now();
            auto sr = coal.submit(frames[(size_t)i]);
            if (sr.accepted && sr.generation >= 0 && sr.generation < (long long)submitTime.size())
                submitTime[(size_t)sr.generation] = now;
            lastSubmitTime = now;
        }
        cv.notify_all();
        if (submitIntervalUs > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(submitIntervalUs));
    }

    // 入力停止後、最終要求が必ず DisplayLatest されるまで drain する。
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return !coal.hasPending() && !coal.hasInFlight(); });
        coal.markDone();
    }
    cv.notify_all();
    consumer.join();

    auto tEnd = Clock::now();
    double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();
    long long displayedTotal = coal.displayedTotal();
    double displayUps = elapsedSec > 0 ? (double)displayedTotal / elapsedSec : 0.0;

    // 入力停止から最終表示までの追いつき時間
    double finalCatchupMs = haveFinalDisplay ? msSince(lastSubmitTime, finalDisplayTime) : -1.0;

    long long finalRequestedFrame = frames.empty() ? -1 : frames.back();
    bool finalFrameMatches = (lastDisplayedFrame == finalRequestedFrame);
    bool finalGenMatches = (coal.lastDisplayedGeneration() == coal.latestSubmittedGeneration());

    Percentiles p = computeStats(displayedLatencies);
    Percentiles lag = computeStats(generationLag);
    Percentiles fdist = computeStats(frameDistance);
    Percentiles sage = computeStats(submitAgeMs);

    // --- counter invariant -------------------------------------------------
    std::vector<std::string> invariantErrors;
    if (!coal.countersBalanced())
        invariantErrors.push_back("submitted(" + std::to_string(coal.submitted()) +
                                  ") != superseded_pending(" +
                                  std::to_string(coal.supersededPending()) + ") + decoded(" +
                                  std::to_string(coal.decoded()) + ")");
    if (!coal.decodeCountersBalanced())
        invariantErrors.push_back("decoded(" + std::to_string(coal.decoded()) +
                                  ") != display_latest(" + std::to_string(coal.displayLatest()) +
                                  ") + display_lagging(" + std::to_string(coal.displayLagging()) +
                                  ") + reject_regression(" +
                                  std::to_string(coal.rejectRegression()) + ") + decode_failed(" +
                                  std::to_string(coal.decodeFailed()) + ")");
    if (displayNonMonotonic != 0)
        invariantErrors.push_back("表示 generation が巻き戻りました (" +
                                  std::to_string(displayNonMonotonic) + " 件)");
    if (!finalGenMatches)
        invariantErrors.push_back(
            "final displayed generation(" + std::to_string(coal.lastDisplayedGeneration()) +
            ") != latest submitted(" + std::to_string(coal.latestSubmittedGeneration()) + ")");
    if (displayedTotal == 0)
        invariantErrors.push_back("displayed_total が 0 件です (成功扱いにしない)");
    if (coal.contractViolations() != 0)
        invariantErrors.push_back("契約違反 " + std::to_string(coal.contractViolations()) + " 件");
    // 集計の自己整合。手で転記した値と JSON がずれないことを機械で担保する。
    if (elapsedSec > 0) {
        double recomputed = (double)displayedTotal / elapsedSec;
        if (std::fabs(displayUps - recomputed) > 1e-6)
            invariantErrors.push_back("display_updates_per_sec が displayed_total/elapsed と "
                                      "一致しません");
    }
    for (const auto& e : contractErrors)
        invariantErrors.push_back(e);

    std::ostringstream js;
    js << "{\n  \"scenario\": \"" << jsonEscape(s.scenario.name) << "\",\n";
    js << "  \"pattern\": \"" << pattern << "\",\n";
    js << "  \"submit_interval_us\": " << submitIntervalUs << ",\n";
    js << "  \"seed\": " << seed << ",\n";
    js << "  \"length\": " << length << ",\n";
    js << "  \"elapsed_sec\": " << elapsedSec << ",\n";
    js << "  \"submitted\": " << coal.submitted() << ",\n";
    js << "  \"superseded_pending\": " << coal.supersededPending() << ",\n";
    js << "  \"decoded\": " << coal.decoded() << ",\n";
    js << "  \"display_latest\": " << coal.displayLatest() << ",\n";
    js << "  \"display_lagging\": " << coal.displayLagging() << ",\n";
    js << "  \"displayed_total\": " << displayedTotal << ",\n";
    js << "  \"reject_regression\": " << coal.rejectRegression() << ",\n";
    js << "  \"decode_failed\": " << coal.decodeFailed() << ",\n";
    js << "  \"contract_violations\": " << coal.contractViolations() << ",\n";
    js << "  \"display_updates_per_sec\": " << displayUps << ",\n";
    js << "  \"displayed_latency_ms\": { \"p50\": " << p.p50 << ", \"p95\": " << p.p95
       << ", \"max\": " << p.max << ", \"mean\": " << p.mean << " },\n";
    js << "  \"generation_lag\": { \"p50\": " << lag.p50 << ", \"p95\": " << lag.p95
       << ", \"max\": " << lag.max << " },\n";
    // 表示フレームと最新要求フレームの絶対距離。generation の差だけでは
    // 「どれだけ絵がずれて見えるか」が分からない。
    js << "  \"frame_distance\": { \"p50\": " << fdist.p50 << ", \"p95\": " << fdist.p95
       << ", \"max\": " << fdist.max << " },\n";
    // 表示した要求の submit 時刻と、その時点の最新要求の submit 時刻の差。
    // 「いま見ている絵は何ミリ秒前の操作か」に相当する。
    js << "  \"submit_age_ms\": { \"p50\": " << sage.p50 << ", \"p95\": " << sage.p95
       << ", \"max\": " << sage.max << " },\n";
    js << "  \"final_catchup_ms\": " << finalCatchupMs << ",\n";
    js << "  \"marker_mismatch_displayed_only\": " << markerMismatch << ",\n";
    js << "  \"display_non_monotonic\": " << displayNonMonotonic << ",\n";
    js << "  \"final_requested_frame\": " << finalRequestedFrame << ",\n";
    js << "  \"final_displayed_frame\": " << lastDisplayedFrame << ",\n";
    js << "  \"final_frame_matches\": " << (finalFrameMatches ? "true" : "false") << ",\n";
    js << "  \"latest_submitted_generation\": " << coal.latestSubmittedGeneration() << ",\n";
    js << "  \"final_displayed_generation\": " << coal.lastDisplayedGeneration() << ",\n";
    js << "  \"final_generation_matches\": " << (finalGenMatches ? "true" : "false") << ",\n";
    js << "  \"invariant_errors\": [";
    for (size_t i = 0; i < invariantErrors.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(invariantErrors[i]) << "\"";
    js << (invariantErrors.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::string jsonOut = a.get("json");
    if (!jsonOut.empty() && jsonOut != "1") {
        FILE* f = _wfopen(utf8Path(jsonOut).c_str(), L"wb");
        if (f) {
            std::string t = js.str();
            std::fwrite(t.data(), 1, t.size(), f);
            std::fclose(f);
        }
    }
    std::fputs(js.str().c_str(), stdout);
    closeScenario(s);

    if (!invariantErrors.empty()) {
        logMsg("counter invariant / 契約に違反しました:");
        for (const auto& e : invariantErrors)
            logMsg("  " + e);
        return kExitMismatch;
    }

    if (a.has("check")) {
        double minUps = std::strtod(a.get("min-updates-per-sec", "15").c_str(), nullptr);
        double p95Limit = std::strtod(a.get("max-p95-ms", "200").c_str(), nullptr);
        bool ok = true;
        if (!finalFrameMatches) {
            logMsg("M6 不合格: 最終要求 " + std::to_string(finalRequestedFrame) +
                   " が表示されていません (表示 " + std::to_string(lastDisplayedFrame) + ")");
            ok = false;
        }
        if (markerMismatch != 0) {
            logMsg("M6 不合格: marker 不一致 " + std::to_string(markerMismatch) + " 件");
            ok = false;
        }
        if (displayUps < minUps) {
            logMsg("M6 不合格: display updates/sec " + std::to_string(displayUps) + " < " +
                   std::to_string(minUps));
            ok = false;
        }
        if (p.p95 > p95Limit) {
            logMsg("M6 不合格: displayed latency p95 " + std::to_string(p.p95) + "ms > " +
                   std::to_string(p95Limit) + "ms");
            ok = false;
        }
        if (a.has("require-coalescing") && coal.supersededPending() == 0) {
            logMsg("coalescing が機能していません: superseded_pending が 0 です");
            ok = false;
        }
        if (!ok)
            return kExitMismatch;
    }
    return kExitOk;
}

// ==========================================================================
// WAV 読み取りと音声検証 (M3)
// ==========================================================================
//
// mlt_frame_get_audio の生バッファ解釈は未解決なので、M3 の判定には
// MLT の avformat consumer が書き出した実ファイルを使う。
// 読むのは自分たちが構造を完全に把握している pcm_s16le の WAV だけである。

namespace {

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    long long frames = 0;    // チャンネルあたりのサンプル数
    std::vector<float> data; // interleaved, -1.0..1.0
    bool ok = false;
    std::string error;
};

uint32_t rdU32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t rdU16(const unsigned char* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

// RIFF/WAVE の pcm_s16le だけを読む。想定外の形式は失敗させる。
WavData readWavS16(const std::string& path) {
    WavData w;
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) {
        w.error = "WAV を開けません: " + path;
        return w;
    }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    if (buf.size() < 44) {
        w.error = "WAV が小さすぎます (" + std::to_string(buf.size()) + " bytes)";
        return w;
    }
    if (std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        w.error = "RIFF/WAVE ヘッダがありません";
        return w;
    }

    size_t pos = 12;
    bool haveFmt = false;
    int bits = 0;
    while (pos + 8 <= buf.size()) {
        char id[5] = {0};
        std::memcpy(id, buf.data() + pos, 4);
        uint32_t sz = rdU32(buf.data() + pos + 4);
        size_t body = pos + 8;
        if (body + sz > buf.size())
            sz = (uint32_t)(buf.size() - body);

        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            uint16_t fmt = rdU16(buf.data() + body);
            w.channels = rdU16(buf.data() + body + 2);
            w.sampleRate = (int)rdU32(buf.data() + body + 4);
            bits = rdU16(buf.data() + body + 14);
            if (fmt != 1) {
                w.error = "PCM ではありません (format tag " + std::to_string(fmt) + ")";
                return w;
            }
            if (bits != 16) {
                w.error = "16bit ではありません (" + std::to_string(bits) + "bit)";
                return w;
            }
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!haveFmt) {
                w.error = "fmt チャンクより前に data チャンクがあります";
                return w;
            }
            if (w.channels <= 0) {
                w.error = "channels が 0 です";
                return w;
            }
            size_t count = sz / 2;
            w.data.resize(count);
            for (size_t i = 0; i < count; i++) {
                int16_t v = (int16_t)rdU16(buf.data() + body + i * 2);
                w.data[i] = (float)v / 32768.0f;
            }
            w.frames = (long long)(count / (size_t)w.channels);
            w.ok = true;
        }
        pos = body + sz + (sz & 1);
    }
    if (!w.ok && w.error.empty())
        w.error = "data チャンクがありません";
    return w;
}

double dcOffset(const std::vector<float>& d, int ch, int channels) {
    if (d.empty())
        return 0;
    double acc = 0;
    long long n = 0;
    for (size_t i = (size_t)ch; i < d.size(); i += (size_t)channels) {
        acc += d[i];
        n++;
    }
    return n ? acc / (double)n : 0;
}

// PCM 最大値付近に張り付いた割合
double clipRatio(const std::vector<float>& d, int ch, int channels) {
    long long n = 0, clipped = 0;
    for (size_t i = (size_t)ch; i < d.size(); i += (size_t)channels) {
        n++;
        if (std::fabs(d[i]) >= 0.999f)
            clipped++;
    }
    return n ? (double)clipped / (double)n : 0;
}

bool hasNonFinite(const std::vector<float>& d) {
    for (float v : d)
        if (!std::isfinite(v))
            return true;
    return false;
}

struct ChannelAnalysis {
    double rms = 0;
    double peak = 0;
    double dc = 0;
    double clip = 0;
    std::map<int, double> tone; // Hz -> 振幅
};

ChannelAnalysis analyseChannel(const WavData& w, int ch, const std::vector<int>& freqs) {
    ChannelAnalysis a;
    const float* base = w.data.data() + ch;
    int stride = w.channels;
    int count = (int)std::min<long long>(w.frames, 48000); // 最大 1 秒分で十分
    a.rms = rms(base, count, stride);
    a.peak = peak(base, count, stride);
    a.dc = dcOffset(w.data, ch, w.channels);
    a.clip = clipRatio(w.data, ch, w.channels);
    for (int hz : freqs)
        a.tone[hz] = goertzel(base, count, stride, w.sampleRate, (double)hz);
    return a;
}

// target 周波数 / 最大の非 target 周波数。存在の有無ではなく比で判定する。
double toneSnr(const ChannelAnalysis& a, const std::vector<int>& targets) {
    double tmin = 1e30, omax = 0;
    for (const auto& kv : a.tone) {
        bool isTarget = std::find(targets.begin(), targets.end(), kv.first) != targets.end();
        if (isTarget)
            tmin = std::min(tmin, kv.second);
        else
            omax = std::max(omax, kv.second);
    }
    if (tmin > 1e29)
        return 0;
    if (omax <= 1e-12)
        return 1e6;
    return tmin / omax;
}

void emitChannel(std::ostream& os, const char* name, const ChannelAnalysis& a) {
    os << "\"" << name << "\": { \"rms\": " << a.rms << ", \"peak\": " << a.peak
       << ", \"dc\": " << a.dc << ", \"clip_ratio\": " << a.clip << ", \"tones\": {";
    bool first = true;
    for (const auto& kv : a.tone) {
        os << (first ? " " : ", ") << "\"" << kv.first << "\": " << kv.second;
        first = false;
    }
    os << " } }";
}

} // namespace

int cmdRenderAudio(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench render-audio <scenario.json> --output <out.wav> "
               "[--disable-track <name>]");
        return kExitUsage;
    }
    std::string outPath = a.get("output");
    if (outPath.empty() || outPath == "1") {
        logMsg("--output を指定してください");
        return kExitUsage;
    }

    Scenario sc;
    std::string lerr;
    if (!loadScenario(a.positional[0], sc, lerr)) {
        logMsg(lerr);
        logMsg("素材が未生成の場合は:  pwsh scripts/make-testmedia.ps1 -Mode Smoke");
        return kExitError;
    }
    if (a.has("mix-mode"))
        copyStr(sc.timeline.audio_mix_mode, sizeof(sc.timeline.audio_mix_mode), a.get("mix-mode"));

    // トラックの無効化は disabled 機構を使う。volume=0 では
    // 「mix されていない」ことの証明にならない。
    // 複数指定はカンマ区切りで受ける (--disable-track A1,A2)
    std::vector<std::string> disabled;
    {
        std::string spec = a.get("disable-track");
        std::string cur;
        for (char c : spec) {
            if (c == ',') {
                if (!cur.empty())
                    disabled.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty() && cur != "1")
            disabled.push_back(cur);
    }

    for (const auto& name : disabled) {
        if (!disableTrackByName(sc.timeline, name.c_str())) {
            logMsg("シナリオに name=" + name + " のトラックがありません");
            return kExitUsage;
        }
    }
    // A2 の gain を上書きできるようにする (0dB と -6dB の比較用)
    if (a.has("gain-db")) {
        double g = std::strtod(a.get("gain-db").c_str(), nullptr);
        for (int i = 0; i < sc.timeline.track_count; i++) {
            if (std::strcmp(sc.timeline.tracks[i].name, "A2") == 0) {
                for (int c = 0; c < sc.timeline.tracks[i].clip_count; c++)
                    sc.timeline.tracks[i].clips[c].gain_db = g;
            }
        }
    }

    if (!initMlt())
        return kExitError;

    MvmComposeInfo info{};
    char cerr[1024] = {0};
    MvmComposeHandle* h = mvm_mlt_compose_open(&sc.timeline, &info, cerr, sizeof(cerr));
    if (!h) {
        logMsg(std::string("タイムラインを構築できません: ") + cerr);
        mvm_mlt_runtime_shutdown();
        return kExitMismatch;
    }

    // 一時ファイルへ出力し、検証が通ってから正規名へ rename する。
    std::string tmpPath = outPath + ".mvmtmp.wav";
    {
        std::error_code ec;
        fs::remove(utf8Path(tmpPath), ec);
    }

    int timeoutMs = (int)std::strtol(a.get("timeout-ms", "60000").c_str(), nullptr, 10);
    char rerr[1024] = {0};
    int rc = mvm_mlt_compose_render_audio(h, tmpPath.c_str(), timeoutMs, rerr, sizeof(rerr));
    mvm_mlt_compose_close(h);
    mvm_mlt_runtime_shutdown();

    if (rc != 0) {
        logMsg(std::string("音声を書き出せません: ") + rerr);
        std::error_code ec;
        fs::remove(utf8Path(tmpPath), ec);
        return kExitMismatch;
    }

    // --- ffprobe による構造検証 -------------------------------------------
    auto ffJson = runFfprobe(tmpPath);
    if (!ffJson) {
        logMsg("ffprobe が出力を読めません");
        std::error_code ec;
        fs::remove(utf8Path(tmpPath), ec);
        return kExitMismatch;
    }
    FfprobeInfo ff = parseFfprobe(*ffJson);

    std::vector<std::string> problems;
    if (!ff.ok)
        problems.push_back("ffprobe の解析に失敗");
    if (ff.container.find("wav") == std::string::npos)
        problems.push_back("container が wav ではありません: " + ff.container);
    if (!ff.hasAudio)
        problems.push_back("音声ストリームがありません");
    if (ff.hasVideo)
        problems.push_back("映像ストリームが含まれています (vn=1 が効いていない)");
    if (ff.audioCodec != "pcm_s16le")
        problems.push_back("codec が pcm_s16le ではありません: " + ff.audioCodec);
    if (ff.sampleRate != 48000)
        problems.push_back("sample_rate が 48000 ではありません: " + std::to_string(ff.sampleRate));
    if (ff.channels != 2)
        problems.push_back("channels が 2 ではありません: " + std::to_string(ff.channels));

    const double wantDuration = 5.0;
    const double durTol = 0.05; // 許容差 ±50ms (3 フレーム分)
    if (std::fabs(ff.duration - wantDuration) > durTol)
        problems.push_back("duration が " + std::to_string(wantDuration) + "s ±" +
                           std::to_string(durTol) +
                           " から外れています: " + std::to_string(ff.duration));

    // 妥当なサイズ: 48000 * 2ch * 2byte * 5s = 960000 バイト前後
    std::error_code ec;
    auto sz = fs::file_size(utf8Path(tmpPath), ec);
    long long expectBytes = 48000LL * 2 * 2 * 5;
    if (ec || (long long)sz < expectBytes * 9 / 10 || (long long)sz > expectBytes * 11 / 10 + 4096)
        problems.push_back("ファイルサイズが妥当ではありません: " + std::to_string((long long)sz) +
                           " (期待 " + std::to_string(expectBytes) + " 前後)");

    std::printf("{\n");
    std::printf("  \"output\": \"%s\",\n", jsonEscape(outPath).c_str());
    std::printf("  \"mix_mode\": \"%s\",\n", jsonEscape(sc.timeline.audio_mix_mode).c_str());
    std::printf("  \"disabled_tracks\": [");
    for (size_t i = 0; i < disabled.size(); i++)
        std::printf("%s\"%s\"", i ? ", " : "", jsonEscape(disabled[i]).c_str());
    std::printf("],\n");
    std::printf("  \"ffprobe\": { \"container\": \"%s\", \"codec\": \"%s\", \"sample_rate\": %lld,"
                " \"channels\": %lld, \"duration\": %g, \"size_bytes\": %lld,"
                " \"has_video\": %s },\n",
                jsonEscape(ff.container).c_str(), jsonEscape(ff.audioCodec).c_str(), ff.sampleRate,
                ff.channels, ff.duration, (long long)sz, ff.hasVideo ? "true" : "false");
    std::printf("  \"problems\": [");
    for (size_t i = 0; i < problems.size(); i++)
        std::printf("%s\n    \"%s\"", i ? "," : "", jsonEscape(problems[i]).c_str());
    std::printf("%s\n}\n", problems.empty() ? "]" : "\n  ]");

    if (!problems.empty()) {
        logMsg("出力 WAV の構造検証に失敗しました:");
        for (const auto& p : problems)
            logMsg("  " + p);
        fs::remove(utf8Path(tmpPath), ec);
        return kExitMismatch;
    }

    // 検証が通ってから正規名へ。途中出力を完成ファイルとして残さない。
    fs::remove(utf8Path(outPath), ec);
    fs::rename(utf8Path(tmpPath), utf8Path(outPath), ec);
    if (ec) {
        logMsg("出力を rename できません: " + ec.message());
        return kExitError;
    }
    return kExitOk;
}

int cmdVerifyAudio(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench verify-audio <dir> [--check-mix] [--check-gain] "
               "[--check-clipping]");
        return kExitUsage;
    }
    std::string dir = a.positional[0];

    const std::vector<int> freqs = {1000, 500, 1500, 750};
    const double minSnr = a.has("min-snr") ? std::strtod(a.get("min-snr").c_str(), nullptr) : 8.0;

    auto load = [&](const std::string& name, WavData& w) -> bool {
        w = readWavS16(dir + "/" + name);
        return w.ok;
    };

    WavData a1, a2, mixed;
    std::vector<std::string> problems;
    if (!load("A1-only.wav", a1))
        problems.push_back("A1-only.wav: " + a1.error);
    if (!load("A2-only.wav", a2))
        problems.push_back("A2-only.wav: " + a2.error);
    if (!load("mixed.wav", mixed))
        problems.push_back("mixed.wav: " + mixed.error);

    if (!problems.empty()) {
        for (const auto& p : problems)
            logMsg(p);
        logMsg("先に render-audio で 3 種類を出力してください");
        return kExitError;
    }

    ChannelAnalysis a1L = analyseChannel(a1, 0, freqs), a1R = analyseChannel(a1, 1, freqs);
    ChannelAnalysis a2L = analyseChannel(a2, 0, freqs), a2R = analyseChannel(a2, 1, freqs);
    ChannelAnalysis mxL = analyseChannel(mixed, 0, freqs), mxR = analyseChannel(mixed, 1, freqs);

    // --- A1-only: L=1000Hz, R=500Hz が支配的 ---
    double snrA1L = toneSnr(a1L, {1000});
    double snrA1R = toneSnr(a1R, {500});
    if (snrA1L < minSnr)
        problems.push_back("A1-only L: 1000Hz の SNR が低い " + std::to_string(snrA1L));
    if (snrA1R < minSnr)
        problems.push_back("A1-only R: 500Hz の SNR が低い " + std::to_string(snrA1R));

    // --- A2-only: L=1500Hz, R=750Hz が支配的 ---
    double snrA2L = toneSnr(a2L, {1500});
    double snrA2R = toneSnr(a2R, {750});
    if (snrA2L < minSnr)
        problems.push_back("A2-only L: 1500Hz の SNR が低い " + std::to_string(snrA2L));
    if (snrA2R < minSnr)
        problems.push_back("A2-only R: 750Hz の SNR が低い " + std::to_string(snrA2R));

    // --- mixed: 4 周波数すべてが存在する ---
    // 片方のトラックしか含まれていなければ、必ずどちらかが欠ける。
    if (a.has("check-mix")) {
        double snrMxL = toneSnr(mxL, {1000, 1500});
        double snrMxR = toneSnr(mxR, {500, 750});
        if (snrMxL < minSnr)
            problems.push_back("mixed L: 1000Hz と 1500Hz の両方が必要 SNR " +
                               std::to_string(snrMxL));
        if (snrMxR < minSnr)
            problems.push_back("mixed R: 500Hz と 750Hz の両方が必要 SNR " +
                               std::to_string(snrMxR));
    }

    // --- clipping / NaN / 無音 ---
    double gainDb = 0;
    if (a.has("check-clipping")) {
        if (hasNonFinite(mixed.data))
            problems.push_back("mixed: NaN または Inf が含まれています");
        if (mxL.rms < 1e-4 || mxR.rms < 1e-4)
            problems.push_back("mixed: 無音です");
        const double maxClip = 0.001;
        if (mxL.clip > maxClip || mxR.clip > maxClip)
            problems.push_back("mixed: clipping 率が高い L " + std::to_string(mxL.clip) + " R " +
                               std::to_string(mxR.clip));
    }

    // --- gain: A2-only の 0dB と -6dB を比較 ---
    if (a.has("check-gain")) {
        WavData a2ref;
        if (!load("A2-only-0db.wav", a2ref)) {
            problems.push_back("A2-only-0db.wav: " + a2ref.error);
        } else {
            ChannelAnalysis refL = analyseChannel(a2ref, 0, freqs);
            if (refL.rms <= 0 || a2L.rms <= 0) {
                problems.push_back("gain 比較: RMS が 0 です");
            } else {
                gainDb = 20.0 * std::log10(a2L.rms / refL.rms);
                const double want = -6.0, tol = 1.0;
                if (std::fabs(gainDb - want) > tol)
                    problems.push_back("gain が期待と違います 実測 " + std::to_string(gainDb) +
                                       "dB 期待 " + std::to_string(want) + "dB ±" +
                                       std::to_string(tol));
            }
        }
    }

    std::ostringstream js;
    js << "{\n  \"dir\": \"" << jsonEscape(dir) << "\",\n";
    js << "  \"min_snr\": " << minSnr << ",\n";
    js << "  \"a1_only\": { ";
    emitChannel(js, "L", a1L);
    js << ", ";
    emitChannel(js, "R", a1R);
    js << " },\n  \"a2_only\": { ";
    emitChannel(js, "L", a2L);
    js << ", ";
    emitChannel(js, "R", a2R);
    js << " },\n  \"mixed\": { ";
    emitChannel(js, "L", mxL);
    js << ", ";
    emitChannel(js, "R", mxR);
    js << " },\n";
    js << "  \"snr\": { \"a1_L\": " << snrA1L << ", \"a1_R\": " << snrA1R
       << ", \"a2_L\": " << snrA2L << ", \"a2_R\": " << snrA2R
       << ", \"mixed_L\": " << toneSnr(mxL, {1000, 1500})
       << ", \"mixed_R\": " << toneSnr(mxR, {500, 750}) << " },\n";
    if (a.has("check-gain"))
        js << "  \"gain_db_measured\": " << gainDb << ",\n";
    js << "  \"problems\": [";
    for (size_t i = 0; i < problems.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(problems[i]) << "\"";
    js << (problems.empty() ? "]\n}\n" : "\n  ]\n}\n");
    std::fputs(js.str().c_str(), stdout);

    if (!problems.empty()) {
        logMsg("音声検証に失敗しました:");
        for (const auto& p : problems)
            logMsg("  " + p);
        return kExitMismatch;
    }
    return kExitOk;
}

// ==========================================================================
// 音声グラフの最小切り分け (S5)
// ==========================================================================

int cmdAudioGraphProbe(const bench::Args& a) {
    using namespace bench;

    std::string caseName = a.get("case");
    std::string out = a.get("output");
    std::string diag = a.get("diag-dir");
    std::string smoke = a.get("smoke-dir");
    if (caseName.empty() || caseName == "1" || out.empty() || out == "1" || diag.empty() ||
        diag == "1") {
        logMsg("使い方: mvm_bench audio-graph-probe --case A --output x.wav "
               "--diag-dir <dir> [--smoke-dir <dir>]");
        return kExitUsage;
    }
    if (smoke.empty() || smoke == "1")
        smoke = diag + "/..";

    MvmAudioGraphPaths paths{};
    auto set = [](char* dst, size_t n, const std::string& v) {
        std::snprintf(dst, n, "%s", v.c_str());
    };
    set(paths.a1_av, sizeof(paths.a1_av), diag + "/a1_av.mp4");
    set(paths.a1_audio_only, sizeof(paths.a1_audio_only), diag + "/a1_audio_only.m4a");
    set(paths.v1_video_only, sizeof(paths.v1_video_only), diag + "/v1_video_only.mp4");
    set(paths.a2_wav, sizeof(paths.a2_wav), diag + "/a2.wav");
    set(paths.v1_h264, sizeof(paths.v1_h264), smoke + "/v1080p60_h264.mp4");
    set(paths.v2_hevc, sizeof(paths.v2_hevc), smoke + "/v1080p60_hevc.mp4");
    set(paths.wav_48k, sizeof(paths.wav_48k), smoke + "/wav_48k.wav");

    if (!initMlt())
        return kExitError;

    int timeoutMs = (int)std::strtol(a.get("timeout-ms", "60000").c_str(), nullptr, 10);
    char err[1024] = {0};
    int rc = mvm_mlt_audiograph_run(caseName.c_str(), &paths, out.c_str(), timeoutMs, stderr, err,
                                    sizeof(err));
    mvm_mlt_runtime_shutdown();

    if (rc == 2) {
        logMsg(std::string("timeout: ") + err);
        return 4; // timeout 専用
    }
    if (rc != 0) {
        logMsg(std::string("失敗: ") + err);
        return kExitMismatch;
    }
    return kExitOk;
}

int cmdAnalyzeWav(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench analyze-wav <file.wav> [--targets 997,613,1429,823]");
        return kExitUsage;
    }
    std::string path = a.positional[0];

    // 既定は切り分け用素材の 4 周波数。整数倍関係が無いものを選んである。
    std::vector<int> targets = {997, 613, 1429, 823};
    if (a.has("targets")) {
        targets.clear();
        std::string spec = a.get("targets"), cur;
        for (char c : spec) {
            if (c == ',') {
                if (!cur.empty())
                    targets.push_back(std::atoi(cur.c_str()));
                cur.clear();
            } else
                cur += c;
        }
        if (!cur.empty())
            targets.push_back(std::atoi(cur.c_str()));
    }

    // 高調波も測る。別トラックからの漏洩と高調波を混同しないため。
    std::vector<int> all = targets;
    for (int t : targets) {
        all.push_back(t * 2);
        all.push_back(t * 3);
    }

    WavData w = readWavS16(path);
    if (!w.ok) {
        logMsg("WAV を読めません: " + w.error);
        return kExitError;
    }

    // 先頭 1 秒ではなく 1.5 秒以降の中央区間を使う。
    // 立ち上がりやフィルタの過渡応答を避けるため。
    long long startFrame = (long long)(1.5 * w.sampleRate);
    long long useFrames = std::min<long long>(w.frames - startFrame, w.sampleRate);
    if (useFrames <= 0) {
        logMsg("解析区間が取れません (frames=" + std::to_string(w.frames) + ")");
        return kExitError;
    }

    std::printf("{\n  \"path\": \"%s\",\n", jsonEscape(path).c_str());
    std::printf("  \"sample_rate\": %d,\n  \"channels\": %d,\n  \"frames\": %lld,\n", w.sampleRate,
                w.channels, w.frames);
    std::printf("  \"analysis_start_sec\": 1.5,\n  \"analysis_frames\": %lld,\n", useFrames);

    const char* chName[2] = {"L", "R"};
    for (int ch = 0; ch < std::min(2, w.channels); ch++) {
        const float* base = w.data.data() + (size_t)startFrame * (size_t)w.channels + (size_t)ch;
        int n = (int)useFrames;
        int stride = w.channels;

        double r = rms(base, n, stride);
        double pk = peak(base, n, stride);
        double dc = 0;
        long long clipped = 0;
        for (int i = 0; i < n; i++) {
            double v = base[(size_t)i * (size_t)stride];
            dc += v;
            if (std::fabs(v) >= 0.999)
                clipped++;
        }
        dc /= n;

        std::printf("  \"%s\": { \"rms\": %g, \"peak\": %g, \"dc\": %g, \"clip_ratio\": %g,\n",
                    chName[ch], r, pk, dc, (double)clipped / n);
        std::printf("    \"tones\": {");
        std::map<int, double> tv;
        bool first = true;
        for (int hz : all) {
            double g = goertzel(base, n, stride, w.sampleRate, (double)hz);
            tv[hz] = g;
            std::printf("%s \"%d\": %g", first ? "" : ",", hz, g);
            first = false;
        }
        std::printf(" },\n");

        // SNR: このチャンネルの target / それ以外の最大 (高調波を除く)
        double best = 0;
        int bestHz = 0;
        for (int t : targets)
            if (tv[t] > best) {
                best = tv[t];
                bestHz = t;
            }
        double other = 0;
        for (int t : targets) {
            if (t == bestHz)
                continue;
            other = std::max(other, tv[t]);
        }
        std::printf("    \"dominant_hz\": %d, \"dominant\": %g, \"max_other_target\": %g,\n",
                    bestHz, best, other);
        std::printf("    \"snr\": %g }%s\n", other > 1e-12 ? best / other : 1e6,
                    ch + 1 < std::min(2, w.channels) ? "," : "");
    }
    std::printf("}\n");
    return kExitOk;
}

// ==========================================================================
// preview-bench (S7 / M7)
// ==========================================================================
//
// compose_frame を連続で呼ぶ経路は preview ではない。あれは毎回 seek して
// 1 枚取る経路であり、consumer の read-ahead も worker thread も通らない。
// ここでは mlt_consumer に tractor を接続し、consumer 側が連続して
// frame を要求する経路を測る。
//
// MLT 7.36.1 のソースで確認した事実は mvm_mlt_compose.h の
// preview セクションに記録してある。要点:
//   render thread 数 = abs(real_time)、正値はドロップあり、負値はドロップなし、
//   real_time=0 は mlt_frame_get_image を呼ばないので計測に使えない。

namespace {

// プロセス単位のスレッド数。CreateToolhelp32Snapshot で自プロセスを数える。
long long currentThreadCount() {
    DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    long long n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                                 sizeof(te.th32OwnerProcessID) &&
                te.th32OwnerProcessID == me)
                n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

double processCpuSeconds() {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return -1.0;
    auto toSec = [](const FILETIME& f) {
        ULARGE_INTEGER x;
        x.LowPart = f.dwLowDateTime;
        x.HighPart = f.dwHighDateTime;
        return (double)x.QuadPart / 1e7; // 100ns 単位
    };
    return toSec(k) + toSec(u);
}

struct MemSnap {
    double wsMb = 0, privMb = 0;
    unsigned long handles = 0;
    bool valid = false;
};

MemSnap takeMemSnap() {
    MemSnap s;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return s;
    DWORD h = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &h))
        return s;
    s.wsMb = (double)pmc.WorkingSetSize / 1048576.0;
    s.privMb = (double)pmc.PrivateUsage / 1048576.0;
    s.handles = h;
    s.valid = true;
    return s;
}

} // namespace

int cmdPreviewBench(const bench::Args& a) {
    using namespace bench;

    OpenedScenario s;
    int rc = kExitOk;
    if (!openScenario(a, s, rc))
        return rc;

    long long length = mvm_mlt_compose_length(s.handle);

    MvmPreviewRequest req{};
    std::string service = a.get("consumer", "null");
    req.consumer_service = service.c_str();
    req.real_time = (int)std::strtol(a.get("real-time", "-4").c_str(), nullptr, 10);
    // real_time=0 は同期経路で、mlt_frame_get_image を一度も呼ばない。
    // 走らせれば「フレームを作らずに速い」測定になる。これは測定の使い方の
    // 誤りなので、走らせる前に使い方エラー (2) で止める。
    // 走らせてから「rendered が 0 件」で落とすと、
    // 「使い方が誤っている」のか「実装が壊れている」のか区別できない。
    if (req.real_time == 0) {
        logMsg("--real-time 0 は mlt_frame_get_image を呼ばないため計測に使えません。"
               "非 0 を指定してください (負値=ドロップなし、正値=ドロップあり)。");
        return kExitUsage;
    }
    req.measure_ms = (int)std::strtol(a.get("measure-ms", "60000").c_str(), nullptr, 10);
    req.warmup_ms = (int)std::strtol(a.get("warmup-ms", "5000").c_str(), nullptr, 10);
    req.timeout_ms = (int)std::strtol(a.get("timeout-ms", "60000").c_str(), nullptr, 10);
    req.max_samples = std::strtoll(a.get("max-samples", "2000000").c_str(), nullptr, 10);

    MemSnap m0 = takeMemSnap();
    long long threadsFirst = currentThreadCount();
    double cpu0 = processCpuSeconds();

    // 計測中のピークを別スレッドで拾う。5ms ごとでは重すぎるので 100ms。
    std::atomic<bool> sampling{true};
    std::atomic<double> peakWs{m0.wsMb}, peakPriv{m0.privMb};
    std::atomic<long long> peakThreads{threadsFirst};
    std::thread watcher([&] {
        while (sampling.load()) {
            MemSnap m = takeMemSnap();
            if (m.valid) {
                double w = peakWs.load();
                if (m.wsMb > w)
                    peakWs.store(m.wsMb);
                double p = peakPriv.load();
                if (m.privMb > p)
                    peakPriv.store(m.privMb);
            }
            long long t = currentThreadCount();
            if (t > peakThreads.load())
                peakThreads.store(t);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    MvmPreviewResult res{};
    char err[1024] = {0};
    int prc = mvm_mlt_compose_preview(s.handle, &req, &res, err, sizeof(err));

    sampling.store(false);
    watcher.join();

    double cpu1 = processCpuSeconds();
    MemSnap m1 = takeMemSnap();
    long long threadsLast = currentThreadCount();

    // --- サンプル解析 ---------------------------------------------------
    std::vector<double> intervals;
    std::set<long long> unique;
    long long duplicates = 0, backwards = 0, outOfRange = 0, notRendered = 0;
    long long skipped = 0;
    long long prevPos = -1;
    double prevMs = -1;
    long long minPos = -1, maxPos = -1;

    for (long long i = 0; i < res.sample_count; i++) {
        const MvmPreviewSample& sm = res.samples[i];
        if (sm.position < 0 || sm.position >= length)
            outOfRange++;
        if (!sm.rendered)
            notRendered++;
        if (!unique.insert(sm.position).second)
            duplicates++;
        if (prevPos >= 0) {
            if (sm.position < prevPos)
                backwards++;
            else if (sm.position > prevPos + 1)
                skipped += sm.position - prevPos - 1;
        }
        if (minPos < 0 || sm.position < minPos)
            minPos = sm.position;
        if (sm.position > maxPos)
            maxPos = sm.position;
        if (prevMs >= 0)
            intervals.push_back(sm.t_ms - prevMs);
        prevPos = sm.position;
        prevMs = sm.t_ms;
    }

    Percentiles iv = computeStats(intervals);

    // 配信された frame のうち、実際に描画されたものだけを「更新」と数える。
    //
    // real_time > 0 では、MLT はドロップした frame も consumer-frame-show で
    // 配信する。その frame は "rendered" が 0 である。これを配信数に混ぜると
    // 「絵が更新された回数」を水増しすることになる。
    // (mlt_consumer.c: rendered が立っていない frame を drop_count に数える)
    long long renderedFrames = res.sample_count - notRendered;
    double deliveredFps = res.wall_sec > 0 ? (double)res.sample_count / res.wall_sec : 0.0;
    double fps = res.wall_sec > 0 ? (double)renderedFrames / res.wall_sec : 0.0;
    // 起動待ちを含まない定常 fps。最初の frame から最後の frame までで割る。
    // effective_fps は起動待ちを含む値。判定には effective_fps を使い、
    // steady_state_fps は「起動待ちがどれだけ効いているか」を見るために出す。
    double steadyFps = 0.0;
    if (res.sample_count >= 2) {
        double span = res.samples[res.sample_count - 1].t_ms - res.samples[0].t_ms;
        if (span > 0)
            steadyFps = (double)(res.sample_count - 1) / (span / 1000.0);
    }
    // 期待フレーム数は「wall 時間 x profile fps」。素材の尺ではない。
    double profileFps = (double)s.scenario.timeline.fps_num / (double)s.scenario.timeline.fps_den;
    double expectedFrames = profileFps * res.wall_sec;
    // drop 率は consumer の drop_count を基準にする。
    // delivered だけから推測しない。
    double dropRate = (renderedFrames + res.drop_count) > 0
                          ? (double)res.drop_count / (double)(renderedFrames + res.drop_count)
                          : 0.0;
    double cpuSec = (cpu1 >= 0 && cpu0 >= 0) ? (cpu1 - cpu0) : -1.0;
    double cpuUtil = (cpuSec >= 0 && res.wall_sec > 0) ? cpuSec / res.wall_sec : -1.0;

    std::vector<std::string> problems;
    if (prc != 0)
        problems.push_back(err);
    if (res.sample_overflow > 0)
        problems.push_back("サンプル配列が溢れました (" + std::to_string(res.sample_overflow) +
                           " 件)。--max-samples を増やしてください");
    if (outOfRange > 0)
        problems.push_back("範囲外の frame position が " + std::to_string(outOfRange) + " 件");
    // real_time < 0 はドロップしない契約なので、rendered=0 が出たら異常。
    // real_time > 0 では rendered=0 がドロップそのものなので異常ではない。
    if (notRendered > 0 && res.effective_real_time < 0)
        problems.push_back("real_time<0 (ドロップなし) なのに rendered=0 の frame が " +
                           std::to_string(notRendered) + " 件あります");
    if (notRendered > 0 && res.effective_real_time > 0 && notRendered != res.drop_count)
        problems.push_back("rendered=0 の frame 数 (" + std::to_string(notRendered) +
                           ") と consumer の drop_count (" + std::to_string(res.drop_count) +
                           ") が一致しません");
    if (renderedFrames <= 0)
        problems.push_back("描画された frame が 1 枚もありません");
    if (!m0.valid || !m1.valid)
        problems.push_back("メモリ採取に失敗しました。この測定は根拠に使えません");
    if (res.stopped_by_timeout)
        problems.push_back("consumer が timeout で停止しました");

    std::ostringstream js;
    js << "{\n";
    js << "  \"scenario\": \"" << jsonEscape(s.scenario.name) << "\",\n";
    js << "  \"configured_profile\": { \"width\": " << s.scenario.timeline.width
       << ", \"height\": " << s.scenario.timeline.height
       << ", \"fps_num\": " << s.scenario.timeline.fps_num
       << ", \"fps_den\": " << s.scenario.timeline.fps_den << " },\n";
    js << "  \"consumer_service\": \"" << jsonEscape(service) << "\",\n";
    js << "  \"requested_real_time\": " << req.real_time << ",\n";
    js << "  \"effective_real_time\": " << res.effective_real_time << ",\n";
    js << "  \"render_threads_implied\": " << std::abs(res.effective_real_time) << ",\n";
    js << "  \"drops_enabled\": " << (res.effective_real_time > 0 ? "true" : "false") << ",\n";
    js << "  \"timeline_length_frames\": " << length << ",\n";
    js << "  \"media_duration_sec\": " << (profileFps > 0 ? (double)length / profileFps : 0.0)
       << ",\n";
    js << "  \"warmup_ms\": " << req.warmup_ms << ",\n";
    js << "  \"measure_ms\": " << req.measure_ms << ",\n";
    js << "  \"measured_wall_sec\": " << res.wall_sec << ",\n";
    js << "  \"producer_ended\": " << (res.producer_ended ? "true" : "false") << ",\n";
    js << "  \"expected_frames\": " << expectedFrames << ",\n";
    js << "  \"delivered_frames\": " << res.sample_count << ",\n";
    js << "  \"rendered_frames\": " << renderedFrames << ",\n";
    js << "  \"delivered_fps\": " << deliveredFps << ",\n";
    js << "  \"unique_positions\": " << (long long)unique.size() << ",\n";
    js << "  \"duplicate_positions\": " << duplicates << ",\n";
    js << "  \"skipped_positions\": " << skipped << ",\n";
    js << "  \"backward_positions\": " << backwards << ",\n";
    js << "  \"out_of_range_positions\": " << outOfRange << ",\n";
    js << "  \"not_rendered_frames\": " << notRendered << ",\n";
    js << "  \"position_min\": " << minPos << ",\n";
    js << "  \"position_max\": " << maxPos << ",\n";
    js << "  \"dropped_frames\": " << res.drop_count << ",\n";
    js << "  \"effective_fps\": " << fps << ",\n";
    js << "  \"steady_state_fps\": " << steadyFps << ",\n";
    js << "  \"drop_rate\": " << dropRate << ",\n";
    js << "  \"startup_latency_ms\": " << res.start_latency_ms << ",\n";
    js << "  \"inter_frame_interval_ms\": { \"p50\": " << iv.p50 << ", \"p95\": " << iv.p95
       << ", \"max\": " << iv.max << ", \"mean\": " << iv.mean << " },\n";
    js << "  \"cpu_process_sec\": " << cpuSec << ",\n";
    js << "  \"cpu_utilization\": " << cpuUtil << ",\n";
    js << "  \"threads\": { \"first\": " << threadsFirst << ", \"peak\": " << peakThreads.load()
       << ", \"last\": " << threadsLast << " },\n";
    js << "  \"working_set_mb\": { \"first\": " << m0.wsMb << ", \"peak\": " << peakWs.load()
       << ", \"last\": " << m1.wsMb << " },\n";
    js << "  \"private_usage_mb\": { \"first\": " << m0.privMb << ", \"peak\": " << peakPriv.load()
       << ", \"last\": " << m1.privMb << " },\n";
    js << "  \"handles\": { \"first\": " << m0.handles << ", \"last\": " << m1.handles << " },\n";
    js << "  \"actual_properties\": \"" << jsonEscape(res.actual_properties) << "\",\n";
    js << "  \"problems\": [";
    for (size_t i = 0; i < problems.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(problems[i]) << "\"";
    js << (problems.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::string jsonOut = a.get("json");
    if (!jsonOut.empty() && jsonOut != "1") {
        FILE* f = _wfopen(utf8Path(jsonOut).c_str(), L"wb");
        if (f) {
            std::string t = js.str();
            std::fwrite(t.data(), 1, t.size(), f);
            std::fclose(f);
        }
    }
    std::fputs(js.str().c_str(), stdout);

    mvm_mlt_preview_free(&res);
    closeScenario(s);

    if (!problems.empty()) {
        for (const auto& p : problems)
            logMsg("  " + p);
        return kExitMismatch;
    }

    if (a.has("check")) {
        double minFps = std::strtod(a.get("min-fps", "50").c_str(), nullptr);
        double maxDrop = std::strtod(a.get("max-drop-rate", "0.05").c_str(), nullptr);
        bool ok = true;
        if (fps < minFps) {
            logMsg("M7 不合格: effective_fps " + std::to_string(fps) + " < " +
                   std::to_string(minFps));
            ok = false;
        }
        if (dropRate > maxDrop) {
            logMsg("M7 不合格: drop_rate " + std::to_string(dropRate) + " > " +
                   std::to_string(maxDrop));
            ok = false;
        }
        if (!ok)
            return kExitMismatch;
    }
    return kExitOk;
}

// ==========================================================================
// 反復テスト (参照リーク / handle リークの検出)
// ==========================================================================
//
// 同一プロセス内で open -> frame -> audio render -> close を繰り返す。
// MLT の参照所有権を間違えると、1 回では露見せず回数を重ねて初めて
// handle 数や RSS の線形増加として現れる。
//
// RSS には allocator のキャッシュが乗るため、小さな増減は
// 即リークと断定しない。明確な線形増加のみ不合格にする。

// ---------------------------------------------------------------------------
// S2-g4: kernel handle の種別内訳を数える診断専用ユーティリティ。
//
// GDI/USER は S2-g3 で owner 候補から除外できた (gdi=3 固定 / user は系統的増加なし)。
// 残るのは file / event / thread / section 等の kernel handle であり、
// GetGuiResources では分解できない。
//
// **この計装は診断専用である。**
//   verdict influence            = 0
//   threshold influence          = 0
//   ownership contract influence = 0
// artifact には kernel_handle_diagnostics_authoritative=false を明示する。
//
// ObjectTypeIndex から型名への対応は文書化されていない。NtQueryObject を任意の
// handle へ呼ぶと named pipe 等で blocking しうるため使わない。代わりに
// 自分で作った既知の object の index を引いて index->name 表を作る。
namespace mvm_g4 {

typedef struct {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} G4HandleEntry;

typedef struct {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    G4HandleEntry Handles[1];
} G4HandleInfo;

typedef LONG(NTAPI* PfnNtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);

inline PfnNtQuerySystemInformation ntQuery() {
    static PfnNtQuerySystemInformation fn = []() -> PfnNtQuerySystemInformation {
        HMODULE m = GetModuleHandleW(L"ntdll.dll");
        return m ? (PfnNtQuerySystemInformation)(void*)GetProcAddress(m, "NtQuerySystemInformation")
                 : nullptr;
    }();
    return fn;
}

// SystemExtendedHandleInformation
static const ULONG kSystemExtendedHandleInformation = 64;

inline std::vector<G4HandleEntry> snapshotOwnHandles() {
    std::vector<G4HandleEntry> out;
    auto fn = ntQuery();
    if (!fn)
        return out;
    const ULONG_PTR self = (ULONG_PTR)GetCurrentProcessId();
    ULONG size = 1u << 20;
    std::vector<unsigned char> buf;
    for (int attempt = 0; attempt < 8; ++attempt) {
        buf.assign(size, 0);
        ULONG need = 0;
        LONG st = fn(kSystemExtendedHandleInformation, buf.data(), size, &need);
        if (st == 0) {
            auto* info = reinterpret_cast<G4HandleInfo*>(buf.data());
            out.reserve(512);
            for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i)
                if (info->Handles[i].UniqueProcessId == self)
                    out.push_back(info->Handles[i]);
            return out;
        }
        // STATUS_INFO_LENGTH_MISMATCH 以外は諦める (診断なので失敗を許容する)。
        if (st != (LONG)0xC0000004L)
            return out;
        size = (need > size) ? (need + (1u << 16)) : (size * 2);
    }
    return out;
}

// 既知 object を作って ObjectTypeIndex を引き当て、index -> name 表を作る。
inline const std::map<USHORT, std::string>& typeNames() {
    static std::map<USHORT, std::string> table = []() {
        std::map<USHORT, std::string> t;
        auto learn = [&t](HANDLE h, const char* name) {
            if (!h || h == INVALID_HANDLE_VALUE)
                return;
            for (const auto& e : snapshotOwnHandles())
                if ((HANDLE)e.HandleValue == h) {
                    t[e.ObjectTypeIndex] = name;
                    break;
                }
        };
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        learn(ev, "Event");
        HANDLE mu = CreateMutexW(nullptr, FALSE, nullptr);
        learn(mu, "Mutant");
        HANDLE se = CreateSemaphoreW(nullptr, 0, 1, nullptr);
        learn(se, "Semaphore");
        HANDLE wt = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        learn(wt, "Timer");
        HANDLE io = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        learn(io, "IoCompletion");
        HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096,
                                        nullptr);
        learn(sec, "Section");
        wchar_t tmpDir[MAX_PATH] = {0};
        wchar_t tmpFile[MAX_PATH] = {0};
        HANDLE fh = INVALID_HANDLE_VALUE;
        if (GetTempPathW(MAX_PATH, tmpDir) && GetTempFileNameW(tmpDir, L"g4", 0, tmpFile)) {
            fh = CreateFileW(tmpFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                        FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            learn(fh, "File");
        }
        HANDLE th = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &th, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        learn(th, "Thread");
        HANDLE pr = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &pr, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        learn(pr, "Process");
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
            learn(tok, "Token");
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_READ, &hk) == ERROR_SUCCESS)
            learn((HANDLE)hk, "Key");

        if (ev) CloseHandle(ev);
        if (mu) CloseHandle(mu);
        if (se) CloseHandle(se);
        if (wt) CloseHandle(wt);
        if (io) CloseHandle(io);
        if (sec) CloseHandle(sec);
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        if (tmpFile[0]) DeleteFileW(tmpFile);
        if (th) CloseHandle(th);
        if (pr) CloseHandle(pr);
        if (tok) CloseHandle(tok);
        if (hk) RegCloseKey(hk);
        return t;
    }();
    return table;
}

// 型名 -> 個数。未知 index は "Type<N>" にまとめる。
inline std::map<std::string, size_t> countByType() {
    std::map<std::string, size_t> out;
    const auto& names = typeNames();
    for (const auto& e : snapshotOwnHandles()) {
        auto it = names.find(e.ObjectTypeIndex);
        if (it != names.end())
            out[it->second] += 1;
        else
            out["Type" + std::to_string((int)e.ObjectTypeIndex)] += 1;
    }
    return out;
}

inline std::string toJson(const std::map<std::string, size_t>& m) {
    std::string s = "{";
    bool first = true;
    for (const auto& kv : m) {
        if (!first)
            s += ", ";
        first = false;
        s += "\"" + kv.first + "\": " + std::to_string(kv.second);
    }
    s += "}";
    return s;
}

// S2-g5: teardown phase ごとの Event 数を記録する。
// logical stopped と physical teardown 完了を区別し、close 後も残るのか
// 遅れて解放されるのか (= race) を直接見る。
inline size_t eventCount() {
    const auto& names = typeNames();
    size_t n = 0;
    for (const auto& e : snapshotOwnHandles()) {
        auto it = names.find(e.ObjectTypeIndex);
        if (it != names.end() && it->second == "Event")
            n += 1;
    }
    return n;
}

inline std::vector<std::pair<std::string, size_t>>& phaseLog() {
    static std::vector<std::pair<std::string, size_t>> v;
    return v;
}

inline void tracePhase(const char* phase) {
    phaseLog().emplace_back(phase, eventCount());
}

} // namespace mvm_g4

int cmdSoak(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench soak <scenario.json> [--iterations 100] [--wav-dir <dir>]");
        return kExitUsage;
    }
    int iterations = (int)std::strtol(a.get("iterations", "100").c_str(), nullptr, 10);
    // S2-g1b: warmup は measurement-domain boundary であって retention の許容量ではない。
    // handle は audio iteration でのみ階段状に変化するため、25 iteration の quartile 窓に
    // 含まれる独立値は 2-3 個しかなく、先頭 block (startup 側) に支配される。
    // suite 条件では先頭 block が低く出て片側 delta が正へ偏る (観測 shift +4.8)。
    // warmup を measurement domain から外すと observed effective interval 20-40 で
    // combined max delta が +5 -> +1 になる。30 はその区間の中央として凍結した値であり、
    // 最適であることが証明された値ではない。
    const int warmupIterations =
        (int)std::strtol(a.get("warmup-iterations", "0").c_str(), nullptr, 10);
    // measured workload は減らさない。warmup は総回数に上乗せする。
    const int totalIterations = warmupIterations + iterations;
    std::string wavDir = a.get("wav-dir");
    bool doAudio = !wavDir.empty() && wavDir != "1";

    Scenario sc;
    std::string lerr;
    if (!loadScenario(a.positional[0], sc, lerr)) {
        logMsg(lerr);
        return kExitError;
    }
    if (!initMlt())
        return kExitError;

    struct Sample {
        int iteration;
        size_t handles;
        unsigned long long rssBytes;
        long long markerValue;
        double audioRmsL;
        // S2-g3: sustained growth の owner 特定用。診断専用で判定には使わない。
        size_t gdiObjects;
        size_t userObjects;
        bool audioThisIteration;
        size_t handlesBeforeAudio;
        size_t handlesAfterAudio;
        std::string typesBeforeAudio;
        std::string typesAfterAudio;
        std::string typesAfterClose;
        std::string phaseTrace;
    };

    std::vector<Sample> samples;
    std::vector<std::string> problems;

    auto handleCount = []() -> size_t {
        DWORD n = 0;
        GetProcessHandleCount(GetCurrentProcess(), &n);
        return (size_t)n;
    };
    // S2-g3: GetGuiResources は GDI / USER object のみを返す。kernel handle の
    // 種別内訳ではない。process handle が増えて GDI/USER が横ばいなら、
    // file/event/thread/section 等の kernel handle を疑う根拠になる。
    auto gdiObjects = []() -> size_t {
        return (size_t)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    };
    auto userObjects = []() -> size_t {
        return (size_t)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    };
    auto rssBytes = []() -> unsigned long long {
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return (unsigned long long)pmc.WorkingSetSize;
        return 0;
    };

    const long long probeFrame = 137;

    for (int it = 0; it < totalIterations; it++) {
        MvmComposeInfo info{};
        char cerr[1024] = {0};
        MvmComposeHandle* h = mvm_mlt_compose_open(&sc.timeline, &info, cerr, sizeof(cerr));
        if (!h) {
            problems.push_back("iteration " + std::to_string(it) + ": open 失敗: " + cerr);
            break;
        }

        MvmMltImage img{};
        char ferr[512] = {0};
        long long markerValue = -1;
        if (mvm_mlt_compose_frame(h, probeFrame, &img, ferr, sizeof(ferr)) != 0) {
            problems.push_back("iteration " + std::to_string(it) + ": frame 失敗: " + ferr);
            mvm_mlt_compose_close(h);
            break;
        }
        MarkerRead m = readMarkerAuto(img.rgba, img.width, img.height);
        markerValue = m.value;
        mvm_mlt_image_free(&img);

        // 音声レンダリングは 5 秒分の通しレンダリングなので重い。
        // 毎回行うと 100 回で現実的な時間に収まらないため、10 回に 1 回にする。
        // 音声経路の参照リークはこれで十分検出できる。
        double rmsL = 0;
        const bool audioThisIter = doAudio && (it % 10 == 0);
        // S2-g3: audio 前後で handle を挟み、retention が audio iteration 内で
        // 起きているか、それとも非同期な別 owner かを切り分ける。
        size_t handlesBeforeAudio = 0;
        size_t handlesAfterAudio = 0;
        // S2-g4: audio iteration でだけ kernel handle の種別内訳を 3 点で採る。
        // 全 iteration で handle table を列挙すると soak 自体が遅くなるため。
        std::string typesBeforeAudio, typesAfterAudio, typesAfterClose;
        std::string phaseTrace;
        if (audioThisIter) {
            handlesBeforeAudio = handleCount();
            typesBeforeAudio = mvm_g4::toJson(mvm_g4::countByType());
            mvm_g4::phaseLog().clear();
            mvm_g4::phaseLog().emplace_back("before_render", mvm_g4::eventCount());
            mvm_mlt_compose_set_trace_hook(&mvm_g4::tracePhase);
        }
        if (audioThisIter) {
            std::string tmp = wavDir + "/soak_tmp.wav";
            char rerr[512] = {0};
            if (mvm_mlt_compose_render_audio(h, tmp.c_str(), 60000, rerr, sizeof(rerr)) != 0) {
                problems.push_back("iteration " + std::to_string(it) + ": audio 失敗: " + rerr);
                mvm_mlt_compose_close(h);
                break;
            }
            WavData w = readWavS16(tmp);
            if (!w.ok) {
                problems.push_back("iteration " + std::to_string(it) + ": WAV 読めず: " + w.error);
                mvm_mlt_compose_close(h);
                break;
            }
            rmsL = rms(w.data.data(), (int)std::min<long long>(w.frames, 48000), w.channels);
            std::error_code ec;
            fs::remove(utf8Path(tmp), ec);
        }

        if (audioThisIter) {
            mvm_mlt_compose_set_trace_hook(nullptr);
            // close 後に遅れて解放されるかを見る。解放されるなら race、
            // 残り続けるなら cleanup omission である。
            // S2-g6b: settle sample は削除した。delayed release は S2-g5 で
            // 0/13 と確定しており (1000ms 待っても解放されない)、この待ちは
            // render timing を歪めるだけである。poll_count と retention の
            // 相関を見る以上、余計な待ちを入れない。
            mvm_g4::phaseLog().emplace_back("after_render_returned", mvm_g4::eventCount());
            std::string t = "[";
            bool first = true;
            for (const auto& kv : mvm_g4::phaseLog()) {
                if (!first)
                    t += ", ";
                first = false;
                t += "{\"phase\": \"" + kv.first + "\", \"event\": " + std::to_string(kv.second) + "}";
            }
            t += "]";
            phaseTrace = t;
            handlesAfterAudio = handleCount();
            typesAfterAudio = mvm_g4::toJson(mvm_g4::countByType());
        }

        mvm_mlt_compose_close(h);
        if (audioThisIter)
            typesAfterClose = mvm_g4::toJson(mvm_g4::countByType());

        samples.push_back({it, handleCount(), rssBytes(), markerValue, rmsL, gdiObjects(),
                           userObjects(), audioThisIter, handlesBeforeAudio, handlesAfterAudio,
                           typesBeforeAudio, typesAfterAudio, typesAfterClose, phaseTrace});

        // 進捗を stderr へ。長時間走るので無反応に見えないようにする。
        if ((it + 1) % 10 == 0 || it == 0) {
            std::fprintf(stderr, "  soak %d/%d handles=%zu rss=%.1fMB\n", it + 1, iterations,
                         samples.back().handles, samples.back().rssBytes / 1048576.0);
            std::fflush(stderr);
        }
    }

    mvm_mlt_runtime_shutdown();

    // warmup 中も functional correctness authority は有効である。完走要件も総回数で見る。
    if (samples.size() < (size_t)totalIterations)
        problems.push_back("完走しませんでした (" + std::to_string(samples.size()) + "/" +
                           std::to_string(totalIterations) + ")");

    bool valuesStable = true;
    if (samples.size() >= 2) {
        const Sample& f = samples.front();
        const Sample& l = samples.back();
        if (f.markerValue != l.markerValue) {
            problems.push_back("marker が初回と最終回で違います " + std::to_string(f.markerValue) +
                               " -> " + std::to_string(l.markerValue));
            valuesStable = false;
        }
        if (f.markerValue != probeFrame)
            problems.push_back("marker が要求フレームと一致しません " +
                               std::to_string(f.markerValue));
        // 音声は 10 回に 1 回なので、最初と最後に測った回どうしを比べる
        double firstAudio = 0, lastAudio = 0;
        for (const auto& sm : samples) {
            if (sm.audioRmsL > 0) {
                if (firstAudio == 0)
                    firstAudio = sm.audioRmsL;
                lastAudio = sm.audioRmsL;
            }
        }
        if (doAudio && firstAudio > 0 && std::fabs(firstAudio - lastAudio) > 1e-6) {
            problems.push_back("音声 RMS が初回と最終回で違います " + std::to_string(firstAudio) +
                               " -> " + std::to_string(lastAudio));
            valuesStable = false;
        }
    }

    // --no-memory-check: メモリ判定は memory-probe の担当。
    // ownership テストは正しさ (完走 / crash なし / 値の一致 / handle) だけを見る。
    const bool checkMemory = !a.has("no-memory-check");

    // handle は単調増加してはいけない。最後の 1/4 が最初の 1/4 より
    // 明確に多ければリークとみなす。
    // S2-g1b: threshold は凍結値である。warmup 導入によって緩めない。
    const size_t kHandleGrowthThreshold = 8;
    size_t handleFirst = 0, handleLast = 0;
    unsigned long long rssFirst = 0, rssLast = 0;
    // handle retention の sampling authority は measured domain だけに適用する。
    const size_t measuredBegin =
        (samples.size() > (size_t)warmupIterations) ? (size_t)warmupIterations : samples.size();
    const size_t measuredCount = samples.size() - measuredBegin;
    if (measuredCount >= 8) {
        size_t q = measuredCount / 4;
        for (size_t i = 0; i < q; i++) {
            handleFirst += samples[measuredBegin + i].handles;
            rssFirst += samples[measuredBegin + i].rssBytes;
        }
        for (size_t i = samples.size() - q; i < samples.size(); i++) {
            handleLast += samples[i].handles;
            rssLast += samples[i].rssBytes;
        }
        handleFirst /= q;
        handleLast /= q;
        rssFirst /= q;
        rssLast /= q;

        if (handleLast > handleFirst + kHandleGrowthThreshold)
            problems.push_back("handle 数が増加しています " + std::to_string(handleFirst) + " -> " +
                               std::to_string(handleLast));

        // WorkingSetSize だけを「リーク量」とは呼ばない。常駐した物理ページと
        // 確保し続けている仮想メモリを区別できないため、既定では合否に使わない。
        double perIter =
            (double)((long long)rssLast - (long long)rssFirst) / (double)(measuredCount * 3 / 4);
        if (checkMemory && perIter > 256.0 * 1024.0)
            problems.push_back("RSS が反復あたり " + std::to_string((long long)perIter) +
                               " バイト増加しています (診断は mvm_bench memory-probe を使うこと)");
    }

    std::printf("{\n  \"scenario\": \"%s\",\n", jsonEscape(sc.name).c_str());
    std::printf("  \"iterations_requested\": %d,\n  \"iterations_completed\": %zu,\n", iterations,
                samples.size());
    std::printf("  \"audio_enabled\": %s,\n", doAudio ? "true" : "false");
    std::printf("  \"values_stable\": %s,\n", valuesStable ? "true" : "false");
    if (!samples.empty()) {
        std::printf("  \"first\": { \"handles\": %zu, \"rss_mb\": %.2f, \"marker\": %lld,"
                    " \"audio_rms_l\": %g },\n",
                    samples.front().handles, samples.front().rssBytes / 1048576.0,
                    samples.front().markerValue, samples.front().audioRmsL);
        std::printf("  \"last\": { \"handles\": %zu, \"rss_mb\": %.2f, \"marker\": %lld,"
                    " \"audio_rms_l\": %g },\n",
                    samples.back().handles, samples.back().rssBytes / 1048576.0,
                    samples.back().markerValue, samples.back().audioRmsL);
    }
    std::printf("  \"quartile_avg\": { \"handles_first\": %zu, \"handles_last\": %zu,"
                " \"rss_mb_first\": %.2f, \"rss_mb_last\": %.2f },\n",
                handleFirst, handleLast, rssFirst / 1048576.0, rssLast / 1048576.0);
    // S2-g1a: --dump-all-samples は診断専用。判定条件には一切影響しない。
    // samples 自体は元から毎 iteration 記録しているが、既定では 10 回ごとにしか
    // 出力しないため warmup boundary を実測できない。
    const bool dumpAllSamples = a.has("dump-all-samples");
    std::printf("  \"sample_emission\": \"%s\",\n", dumpAllSamples ? "ALL" : "DECIMATED_10");
    std::printf("  \"kernel_handle_diagnostics_authoritative\": false,\n");
    std::printf("  \"handle_growth_threshold\": %zu,\n", kHandleGrowthThreshold);
    std::printf("  \"functional_authority_domain\": \"ALL_ITERATIONS\",\n");
    std::printf("  \"handle_retention_authority_domain\": \"MEASURED_ONLY\",\n");
    std::printf("  \"warmup_iteration_count\": %d,\n", warmupIterations);
    std::printf("  \"measured_iteration_count\": %zu,\n", measuredCount);
    std::printf("  \"handle_measurement_start_iteration\": %zu,\n", measuredBegin);
    std::printf("  \"quartile_size\": %zu,\n", measuredCount / 4);
    std::printf("  \"samples\": [");
    for (size_t i = 0; i < samples.size(); i++) {
        if (!dumpAllSamples && i % 10 != 0 && i + 1 != samples.size())
            continue; // 10 回ごとと最終回だけ出す
        std::printf("%s\n    { \"i\": %d, \"handles\": %zu, \"rss_mb\": %.2f,"
                    " \"gdi\": %zu, \"user\": %zu, \"audio\": %s,"
                    " \"h_before_audio\": %zu, \"h_after_audio\": %zu }",
                    i ? "," : "", samples[i].iteration, samples[i].handles,
                    samples[i].rssBytes / 1048576.0, samples[i].gdiObjects,
                    samples[i].userObjects, samples[i].audioThisIteration ? "true" : "false",
                    samples[i].handlesBeforeAudio, samples[i].handlesAfterAudio);
        if (samples[i].audioThisIteration && !samples[i].typesBeforeAudio.empty()) {
            std::printf(",\n    { \"i\": %d, \"kernel_handle_types\": "
                        "{ \"before_audio\": %s, \"after_audio\": %s, "
                        "\"after_close\": %s } }",
                        samples[i].iteration, samples[i].typesBeforeAudio.c_str(),
                        samples[i].typesAfterAudio.c_str(),
                        samples[i].typesAfterClose.c_str());
        }
        if (samples[i].audioThisIteration && !samples[i].phaseTrace.empty()) {
            std::printf(",\n    { \"i\": %d, \"teardown_phases\": %s }",
                        samples[i].iteration, samples[i].phaseTrace.c_str());
        }
    }
    std::printf("\n  ],\n  \"problems\": [");
    for (size_t i = 0; i < problems.size(); i++)
        std::printf("%s\n    \"%s\"", i ? "," : "", jsonEscape(problems[i]).c_str());
    std::printf("%s\n}\n", problems.empty() ? "]" : "\n  ]");

    if (!problems.empty()) {
        logMsg("反復テストに失敗しました:");
        for (const auto& p : problems)
            logMsg("  " + p);
        return kExitMismatch;
    }
    return kExitOk;
}

// ==========================================================================
// メモリ切り分け (S6 correctness closure)
// ==========================================================================
//
// WorkingSetSize だけでは「常駐した物理ページ」と「確保し続けている仮想メモリ」
// を区別できない。PROCESS_MEMORY_COUNTERS_EX の PrivateUsage を併記し、
// phase 単位で記録する。
//
// 各ケースは必ず別プロセスで実行する (scripts/memory-matrix.ps1)。
// MLT の global cache が前のケースから持ち越されるのを避けるため。

namespace {

struct MemSample {
    int iteration = -1;
    std::string phase;
    unsigned long long workingSet = 0;
    unsigned long long peakWorkingSet = 0;
    unsigned long long privateUsage = 0;
    unsigned long long pagefileUsage = 0;
    unsigned long handles = 0;
    // 採取に失敗したサンプルは 0 のまま。0 を実測値として扱うと
    // 「メモリが減った」「handle が 0 になった」という誤った結論になる。
    bool valid = false;
    unsigned long lastError = 0;
};

MemSample takeMemSample(int iteration, const std::string& phase) {
    MemSample s;
    s.iteration = iteration;
    s.phase = phase;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        s.lastError = GetLastError();
        return s; // valid = false
    }
    s.workingSet = pmc.WorkingSetSize;
    s.peakWorkingSet = pmc.PeakWorkingSetSize;
    s.privateUsage = pmc.PrivateUsage;
    s.pagefileUsage = pmc.PagefileUsage;

    DWORD h = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &h)) {
        s.lastError = GetLastError();
        return s; // valid = false
    }
    s.handles = h;
    s.valid = true;
    return s;
}

double mb(unsigned long long v) {
    return (double)v / 1048576.0;
}

// 増加の形を区別する。単純な first-last の傾きだけでは
// 「線形増加」「段差後 plateau」「周期的変動」を取り違える。
struct GrowthShape {
    double perIterBytes = 0;       // 全体の平均傾き
    double lastQuarterPerIter = 0; // 後半 1/4 の傾き
    long long biggestJumpIteration = -1;
    double biggestJumpBytes = 0;
    double beforeJump = 0;
    double afterJump = 0;
    std::string classification;
};

GrowthShape analyseGrowth(const std::vector<double>& v) {
    GrowthShape g;
    if (v.size() < 4)
        return g;
    g.perIterBytes = (v.back() - v.front()) / (double)(v.size() - 1);

    size_t q = v.size() / 4;
    if (q >= 2) {
        double a = v[v.size() - q];
        double b = v.back();
        g.lastQuarterPerIter = (b - a) / (double)(q - 1);
    }

    for (size_t i = 1; i < v.size(); i++) {
        double d = v[i] - v[i - 1];
        if (d > g.biggestJumpBytes) {
            g.biggestJumpBytes = d;
            g.biggestJumpIteration = (long long)i;
            g.beforeJump = v[i - 1];
            g.afterJump = v[i];
        }
    }

    // 分類。閾値は診断の目安であり合否ではない。
    double total = v.back() - v.front();
    if (total <= 0) {
        g.classification = "増加なし";
    } else if (g.biggestJumpBytes > total * 0.6) {
        g.classification = (g.lastQuarterPerIter < g.perIterBytes * 0.3) ? "段差後 plateau"
                                                                         : "段差あり + 継続増加";
    } else if (g.lastQuarterPerIter > g.perIterBytes * 0.6) {
        g.classification = "線形増加";
    } else {
        g.classification = "逓減 (cache 充填の可能性)";
    }
    return g;
}

void emitGrowth(std::ostream& os, const char* name, const GrowthShape& g) {
    os << "\"" << name << "\": { \"per_iter_bytes\": " << (long long)g.perIterBytes
       << ", \"last_quarter_per_iter_bytes\": " << (long long)g.lastQuarterPerIter
       << ", \"biggest_jump_iteration\": " << g.biggestJumpIteration
       << ", \"biggest_jump_mb\": " << g.biggestJumpBytes / 1048576.0
       << ", \"before_jump_mb\": " << g.beforeJump / 1048576.0
       << ", \"after_jump_mb\": " << g.afterJump / 1048576.0 << ", \"classification\": \""
       << g.classification << "\" }";
}

} // namespace

int cmdMemoryProbe(const bench::Args& a) {
    using namespace bench;

    std::string caseName = a.get("case", "E");
    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench memory-probe <scenario.json> --case A|B|C|D|E|F|G "
               "[--csv <path>] [--wav-dir <dir>]");
        return kExitUsage;
    }

    // ケースごとの既定反復回数
    int iterations = 0;
    if (caseName == "A")
        iterations = 50;
    else if (caseName == "B" || caseName == "C")
        iterations = 200;
    else if (caseName == "D")
        iterations = 30;
    else if (caseName == "E")
        iterations = 100;
    else if (caseName == "F")
        iterations = 1000;
    else if (caseName == "G")
        iterations = 50;
    else if (caseName == "U")
        // S7 診断: unique frame を順に取る。ケース F の frame = iteration % 300 とは違い、
        // 同じフレームを取り直さない。「上限で止まった」のか
        // 「新しいフレームが来なくなったから止まった」のかを区別するため。
        iterations = 3600;
    else {
        logMsg("未知のケース: " + caseName);
        return kExitUsage;
    }
    if (a.has("iterations"))
        iterations = (int)std::strtol(a.get("iterations").c_str(), nullptr, 10);

    Scenario sc;
    std::string lerr;
    if (!loadScenario(a.positional[0], sc, lerr)) {
        logMsg(lerr);
        return kExitError;
    }
    std::string wavDir = a.get("wav-dir");
    const bool haveWavDir = !wavDir.empty() && wavDir != "1";

    // D と E は音声レンダリングを含むケースである。--wav-dir が無いと
    // 音声を黙って飛ばし、「音声ありのケースを測った」という誤った記録が残る。
    // 測れないなら測れないと言う。
    if ((caseName == "D" || caseName == "E") && !haveWavDir) {
        logMsg("ケース " + caseName +
               " は音声レンダリングを含むため --wav-dir が必要です。"
               "指定が無いと音声を含まない測定になり、ケースの意味が変わります。");
        return kExitUsage;
    }

    std::vector<MemSample> samples;
    std::vector<std::string> problems;
    auto tStart = Clock::now();

    samples.push_back(takeMemSample(-1, "before_runtime_init"));

    // G は反復ごとに runtime を作り直す。それ以外は 1 回だけ。
    const bool runtimePerIteration = (caseName == "G");

    if (!runtimePerIteration) {
        if (!initMlt())
            return kExitError;
        samples.push_back(takeMemSample(-1, "after_runtime_init"));
    }

    MvmComposeHandle* sharedHandle = nullptr;
    if (caseName == "F" || caseName == "U") {
        MvmComposeInfo info{};
        char cerr[1024] = {0};
        sharedHandle = mvm_mlt_compose_open(&sc.timeline, &info, cerr, sizeof(cerr));
        if (!sharedHandle) {
            logMsg(std::string("open 失敗: ") + cerr);
            mvm_mlt_runtime_shutdown();
            return kExitError;
        }
        samples.push_back(takeMemSample(-1, "after_open"));
    }

    long long firstMarker = -1, lastMarker = -1;
    // ケース U 用。実際に何種類のフレームへアクセスしたかを数える。
    // 「unique frame を N 件触った」を推測ではなく実測で言うため。
    std::set<long long> uniqueFrames;
    long long markerBad = 0;
    const long long timelineLength = std::max(1LL, mvm_mlt_compose_length(sharedHandle));

    for (int it = 0; it < iterations; it++) {
        if (runtimePerIteration) {
            if (!initMlt()) {
                problems.push_back("iteration " + std::to_string(it) + ": runtime init 失敗");
                break;
            }
        }

        if (caseName == "F" || caseName == "U") {
            // グラフは使い回し、フレームだけ変える。
            //
            // F: frame = iteration % 300。**同じ 300 フレームを取り直す。**
            //    「増加が止まった」ことは「固有フレーム 300 件分を保持しきった」
            //    までしか意味しない。
            // U: frame = iteration。固有フレームを順に取る (S7 診断)。
            //    こちらは固有フレーム数に比例して増え続けるかどうかを見られる。
            long long frame =
                (caseName == "U") ? (long long)it % timelineLength : (long long)(it % 300);
            MvmMltImage img{};
            char err[512] = {0};
            if (mvm_mlt_compose_frame(sharedHandle, frame, &img, err, sizeof(err)) != 0) {
                problems.push_back("iteration " + std::to_string(it) + ": frame 失敗");
                break;
            }
            if (caseName == "U") {
                // 取ったフレームが要求どおりか確認する。
                // 中身が違うのに「unique frame を取った」と記録しない。
                MarkerRead m = readMarkerAuto(img.rgba, img.width, img.height);
                if (firstMarker < 0)
                    firstMarker = m.value;
                lastMarker = m.value;
                if (!m.syncOk || m.value != frame)
                    markerBad++;
                uniqueFrames.insert(frame);
            }
            mvm_mlt_image_free(&img);
            samples.push_back(takeMemSample(it, "after_frame"));
        } else if (caseName == "A") {
            // runtime だけ。open もしない。
            samples.push_back(takeMemSample(it, "after_runtime_init"));
            mvm_mlt_runtime_shutdown();
            samples.push_back(takeMemSample(it, "after_runtime_shutdown"));
            if (it + 1 < iterations && !initMlt()) {
                problems.push_back("re-init 失敗");
                break;
            }
        } else {
            MvmComposeInfo info{};
            char cerr[1024] = {0};
            MvmComposeHandle* h = mvm_mlt_compose_open(&sc.timeline, &info, cerr, sizeof(cerr));
            if (!h) {
                problems.push_back("iteration " + std::to_string(it) + ": open 失敗: " + cerr);
                break;
            }
            samples.push_back(takeMemSample(it, "after_open"));

            if (caseName == "C" || caseName == "E" || caseName == "G") {
                MvmMltImage img{};
                char err[512] = {0};
                if (mvm_mlt_compose_frame(h, 137, &img, err, sizeof(err)) != 0) {
                    problems.push_back("iteration " + std::to_string(it) + ": frame 失敗");
                    mvm_mlt_compose_close(h);
                    break;
                }
                MarkerRead m = readMarkerAuto(img.rgba, img.width, img.height);
                if (firstMarker < 0)
                    firstMarker = m.value;
                lastMarker = m.value;
                mvm_mlt_image_free(&img);
                samples.push_back(takeMemSample(it, "after_frame"));
            }

            bool wantAudio = (caseName == "D") || (caseName == "E" && it % 10 == 0);
            if (wantAudio) {
                std::string tmp = wavDir + "/mem_tmp.wav";
                char rerr[512] = {0};
                if (mvm_mlt_compose_render_audio(h, tmp.c_str(), 120000, rerr, sizeof(rerr)) != 0) {
                    problems.push_back("iteration " + std::to_string(it) + ": audio 失敗: " + rerr);
                    mvm_mlt_compose_close(h);
                    break;
                }
                std::error_code ec;
                fs::remove(utf8Path(tmp), ec);
                samples.push_back(takeMemSample(it, "after_audio"));
            }

            mvm_mlt_compose_close(h);
            samples.push_back(takeMemSample(it, "after_close"));
        }

        if (runtimePerIteration) {
            mvm_mlt_runtime_shutdown();
            samples.push_back(takeMemSample(it, "after_runtime_shutdown"));
        }

        if ((it + 1) % 25 == 0) {
            std::fprintf(stderr, "  mem[%s] %d/%d ws=%.1fMB priv=%.1fMB\n", caseName.c_str(),
                         it + 1, iterations, mb(samples.back().workingSet),
                         mb(samples.back().privateUsage));
            std::fflush(stderr);
        }
    }

    if ((caseName == "F" || caseName == "U") && sharedHandle) {
        mvm_mlt_compose_close(sharedHandle);
        samples.push_back(takeMemSample(-1, "after_close"));
    }

    if (!runtimePerIteration) {
        mvm_mlt_runtime_shutdown();
        samples.push_back(takeMemSample(-1, "after_runtime_shutdown"));
        // allocator が返すのを待つ。強制 trim はしない。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        samples.push_back(takeMemSample(-1, "after_runtime_shutdown_idle"));
    }

    auto tEnd = Clock::now();
    double elapsed = std::chrono::duration<double>(tEnd - tStart).count();

    // --- CSV には全サンプルを出す (省略しない) ---
    std::string csv = a.get("csv");
    if (!csv.empty() && csv != "1") {
        FILE* f = _wfopen(utf8Path(csv).c_str(), L"wb");
        if (f) {
            std::fprintf(f, "case,iteration,phase,valid,working_set,peak_working_set,private_usage,"
                            "pagefile_usage,handles,last_error\n");
            for (const auto& m : samples)
                std::fprintf(f, "%s,%d,%s,%d,%llu,%llu,%llu,%llu,%lu,%lu\n", caseName.c_str(),
                             m.iteration, m.phase.c_str(), m.valid ? 1 : 0, m.workingSet,
                             m.peakWorkingSet, m.privateUsage, m.pagefileUsage, m.handles,
                             m.lastError);
            std::fclose(f);
        }
    }

    // 反復ごとの代表 phase (after_close / after_frame) だけで形を見る
    const char* repPhase = (caseName == "F" || caseName == "U") ? "after_frame"
                           : (caseName == "A")                  ? "after_runtime_shutdown"
                           : (caseName == "G")                  ? "after_runtime_shutdown"
                                                                : "after_close";
    std::vector<double> ws, priv;
    for (const auto& m : samples) {
        // 無効サンプルは集計から外す。0 を実測値として混ぜない。
        if (m.valid && m.iteration >= 0 && m.phase == repPhase) {
            ws.push_back((double)m.workingSet);
            priv.push_back((double)m.privateUsage);
        }
    }

    GrowthShape gWs = analyseGrowth(ws);
    GrowthShape gPriv = analyseGrowth(priv);

    auto findPhase = [&](const std::string& ph) -> const MemSample* {
        for (const auto& m : samples)
            if (m.valid && m.phase == ph && m.iteration < 0)
                return &m;
        return nullptr;
    };
    const MemSample* beforeInit = findPhase("before_runtime_init");
    const MemSample* shutdown = findPhase("after_runtime_shutdown");
    const MemSample* idle = findPhase("after_runtime_shutdown_idle");

    std::ostringstream js;
    js << "{\n  \"case\": \"" << caseName << "\",\n";
    js << "  \"iterations\": " << iterations << ",\n";
    js << "  \"representative_phase\": \"" << repPhase << "\",\n";
    js << "  \"elapsed_sec\": " << elapsed << ",\n";
    js << "  \"samples_total\": " << samples.size() << ",\n";
    if (!ws.empty()) {
        size_t q = ws.size() / 4;
        double wsFirstQ = 0, wsLastQ = 0, pFirstQ = 0, pLastQ = 0;
        if (q >= 1) {
            for (size_t i = 0; i < q; i++) {
                wsFirstQ += ws[i];
                pFirstQ += priv[i];
            }
            for (size_t i = ws.size() - q; i < ws.size(); i++) {
                wsLastQ += ws[i];
                pLastQ += priv[i];
            }
            wsFirstQ /= q;
            wsLastQ /= q;
            pFirstQ /= q;
            pLastQ /= q;
        }
        js << "  \"working_set_mb\": { \"first\": " << mb((unsigned long long)ws.front())
           << ", \"last\": " << mb((unsigned long long)ws.back())
           << ", \"first_quartile\": " << wsFirstQ / 1048576.0
           << ", \"last_quartile\": " << wsLastQ / 1048576.0 << " },\n";
        js << "  \"private_usage_mb\": { \"first\": " << mb((unsigned long long)priv.front())
           << ", \"last\": " << mb((unsigned long long)priv.back())
           << ", \"first_quartile\": " << pFirstQ / 1048576.0
           << ", \"last_quartile\": " << pLastQ / 1048576.0 << " },\n";
    }
    js << "  \"growth\": { ";
    emitGrowth(js, "working_set", gWs);
    js << ",\n    ";
    emitGrowth(js, "private_usage", gPriv);
    js << " },\n";
    // handle 数は有効サンプルからのみ取る。採取に失敗した 0 を
    // 「handle が減った」と読み違えないため。
    const MemSample* firstValid = nullptr;
    const MemSample* lastValid = nullptr;
    long long invalidSamples = 0;
    for (const auto& m : samples) {
        if (!m.valid) {
            invalidSamples++;
            continue;
        }
        if (!firstValid)
            firstValid = &m;
        lastValid = &m;
    }
    js << "  \"handles\": { \"first\": " << (firstValid ? firstValid->handles : 0)
       << ", \"last\": " << (lastValid ? lastValid->handles : 0) << " },\n";
    js << "  \"invalid_samples\": " << invalidSamples << ",\n";
    js << "  \"samples_valid\": " << (long long)samples.size() - invalidSamples << ",\n";
    if (beforeInit)
        js << "  \"before_runtime_init_mb\": { \"ws\": " << mb(beforeInit->workingSet)
           << ", \"priv\": " << mb(beforeInit->privateUsage) << " },\n";
    if (shutdown)
        js << "  \"after_runtime_shutdown_mb\": { \"ws\": " << mb(shutdown->workingSet)
           << ", \"priv\": " << mb(shutdown->privateUsage) << " },\n";
    if (idle)
        js << "  \"after_runtime_shutdown_idle_mb\": { \"ws\": " << mb(idle->workingSet)
           << ", \"priv\": " << mb(idle->privateUsage) << " },\n";
    js << "  \"marker_first\": " << firstMarker << ",\n  \"marker_last\": " << lastMarker << ",\n";
    // 実際にアクセスした固有フレーム数。ケース F と U を取り違えないため、
    // どのケースでも出す。
    js << "  \"unique_frames_accessed\": " << (long long)uniqueFrames.size() << ",\n";
    js << "  \"timeline_length\": " << timelineLength << ",\n";
    js << "  \"marker_mismatch\": " << markerBad << ",\n";

    // 採取に失敗したサンプルが 1 件でもあれば、この測定を根拠に使ってはいけない。
    // 欠測を黙って無視した平均値は、無いより悪い。
    if (invalidSamples > 0)
        problems.push_back("メモリ採取に失敗したサンプルが " + std::to_string(invalidSamples) +
                           " 件あります。この測定は判断根拠に使えません。");
    if (ws.empty())
        problems.push_back("代表 phase (" + std::string(repPhase) +
                           ") の有効サンプルが 0 件です。");
    if (markerBad > 0)
        problems.push_back("要求と違うフレームが " + std::to_string(markerBad) +
                           " 件返りました。unique frame を触ったと記録できません。");

    js << "  \"problems\": [";
    for (size_t i = 0; i < problems.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(problems[i]) << "\"";
    js << (problems.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::fputs(js.str().c_str(), stdout);

    if (!problems.empty()) {
        for (const auto& p : problems)
            logMsg("  " + p);
        return kExitMismatch;
    }
    return kExitOk;
}
