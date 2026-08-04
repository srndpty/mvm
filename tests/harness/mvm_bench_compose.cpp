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
#include "media/mlt/mvm_mlt_compose.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>

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

    MarkerRead marker = readMarker(img.rgba, img.width, img.height);

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

    OpenedScenario s;
    int rc = kExitOk;
    if (!openScenario(a, s, rc))
        return rc;

    std::string outDir = a.get("output-dir");
    std::vector<std::string> problems;

    // 未実行の検査があれば成功にしない。
    int videoChecks = 0, audioChecks = 0;

    const Scenario& sc = s.scenario;
    if (sc.verifyFrames.empty())
        problems.push_back("verify.frames が空です");
    if (sc.regions.find("v2_inside") == sc.regions.end() ||
        sc.regions.find("v1_only") == sc.regions.end() ||
        sc.regions.find("text") == sc.regions.end())
        problems.push_back("verify.regions に v2_inside / v1_only / text が必要です");

    std::ostringstream js;
    js << "{\n";
    js << "  \"scenario\": \"" << jsonEscape(sc.name) << "\",\n";
    js << "  \"text_service\": \"" << jsonEscape(sc.timeline.text_service) << "\",\n";
    js << "  \"font_file\": \"" << jsonEscape(sc.timeline.font_file) << "\",\n";
    printComposeInfo(s.info, js);
    js << "  \"frames\": [";

    bool firstFrame = true;
    for (long long frame : sc.verifyFrames) {
        MvmMltImage img{};
        char err[512] = {0};
        if (mvm_mlt_compose_frame(s.handle, frame, &img, err, sizeof(err)) != 0) {
            problems.push_back("frame " + std::to_string(frame) + ": 取得できません: " + err);
            continue;
        }
        videoChecks++;

        RegionStats v2 = regionStats(img.rgba, img.width, img.height, sc.regions.at("v2_inside"));
        RegionStats v1 = regionStats(img.rgba, img.width, img.height, sc.regions.at("v1_only"));
        RegionStats tx = regionStats(img.rgba, img.width, img.height, sc.regions.at("text"));
        MarkerRead marker = readMarker(img.rgba, img.width, img.height);

        // 1. マーカー: 合成後も V1 のマーカーが正しいフレームを示すこと
        if (!marker.syncOk)
            problems.push_back("frame " + std::to_string(frame) + ": マーカーの同期が取れません");
        else if (marker.value != frame)
            problems.push_back("frame " + std::to_string(frame) +
                               ": マーカー不一致 marker=" + std::to_string(marker.value));

        // 2. V2 領域と V1 のみ領域が別の絵であること。
        //    同じなら V2 が合成されていない。
        double dist = meanColourDistance(v2, v1);
        if (dist < 8.0)
            problems.push_back("frame " + std::to_string(frame) +
                               ": V2 領域と V1 領域の平均色が近すぎます (距離 " +
                               std::to_string(dist) + ")。V2 が合成されていない可能性があります");

        // 3. V2 領域に絵があること (分散が 0 なら単色 = 合成失敗)
        if (v2.variance < 1.0)
            problems.push_back("frame " + std::to_string(frame) + ": V2 領域が単色です (variance " +
                               std::to_string(v2.variance) + ")");

        // 4. text 領域に有効画素があること。
        //    背景 (bg_colour) と文字色の両方が含まれるため分散が立つ。
        if (tx.variance < 5.0)
            problems.push_back("frame " + std::to_string(frame) +
                               ": text 領域に描画がありません (variance " +
                               std::to_string(tx.variance) + ")");

        // 5. alpha が全体に効いていないこと。
        //    最終合成結果は不透明であるべき。半透明のまま出てくるのは
        //    合成が途中で終わっている兆候。
        if (v1.meanA < 250.0)
            problems.push_back("frame " + std::to_string(frame) +
                               ": 背景領域の alpha が不透明ではありません (mean_a " +
                               std::to_string(v1.meanA) + ")");

        if (!outDir.empty() && outDir != "1") {
            std::string pngPath = outDir + "/compose_" + std::to_string(frame) + ".png";
            std::string pngErr;
            writePng(pngPath, img.rgba, img.width, img.height, pngErr);
        }

        js << (firstFrame ? "\n    " : ",\n    ");
        firstFrame = false;
        js << "{ \"frame\": " << frame
           << ", \"marker_sync_ok\": " << (marker.syncOk ? "true" : "false")
           << ", \"marker_value\": " << marker.value << ", \"v2_v1_distance\": " << dist
           << ", \"v2_variance\": " << v2.variance << ", \"text_variance\": " << tx.variance
           << ", \"v1_mean_alpha\": " << v1.meanA << " }";

        mvm_mlt_image_free(&img);
    }
    js << (firstFrame ? "],\n" : "\n  ],\n");

    // --- 音声 ---
    js << "  \"audio\": [";
    bool firstAudio = true;
    for (long long frame : sc.audio.frames) {
        MvmComposeAudio au{};
        char err[512] = {0};
        if (mvm_mlt_compose_audio(s.handle, frame, &au, err, sizeof(err)) != 0) {
            problems.push_back("audio frame " + std::to_string(frame) + ": 取得できません: " + err);
            continue;
        }
        audioChecks++;

        if (au.sample_rate != sc.audio.sampleRate)
            problems.push_back("audio frame " + std::to_string(frame) + ": sample_rate " +
                               std::to_string(au.sample_rate) +
                               " != " + std::to_string(sc.audio.sampleRate));
        if (au.channels != sc.audio.channels)
            problems.push_back("audio frame " + std::to_string(frame) + ": channels " +
                               std::to_string(au.channels) +
                               " != " + std::to_string(sc.audio.channels));

        double rmsL = rms(au.data, au.samples, au.channels);
        double rmsR = rms(au.data + 1, au.samples, au.channels);
        double peakL = peak(au.data, au.samples, au.channels);
        double peakR = peak(au.data + 1, au.samples, au.channels);

        if (rmsL < sc.audio.minRms || rmsR < sc.audio.minRms)
            problems.push_back("audio frame " + std::to_string(frame) + ": 無音に近いです (rmsL " +
                               std::to_string(rmsL) + " rmsR " + std::to_string(rmsR) + ")");
        if (peakL > sc.audio.maxPeak || peakR > sc.audio.maxPeak)
            problems.push_back("audio frame " + std::to_string(frame) +
                               ": clipping しています (peak " +
                               std::to_string(std::max(peakL, peakR)) + ")");

        js << (firstAudio ? "\n    " : ",\n    ");
        firstAudio = false;
        js << "{ \"frame\": " << frame << ", \"sample_rate\": " << au.sample_rate
           << ", \"channels\": " << au.channels << ", \"samples\": " << au.samples
           << ", \"rms_l\": " << rmsL << ", \"rms_r\": " << rmsR << ", \"peak_l\": " << peakL
           << ", \"peak_r\": " << peakR;

        for (double hz : sc.audio.goertzelHz) {
            double gl = goertzel(au.data, au.samples, au.channels, au.sample_rate, hz);
            double gr = goertzel(au.data + 1, au.samples, au.channels, au.sample_rate, hz);
            js << ", \"g" << (long long)hz << "_l\": " << gl << ", \"g" << (long long)hz
               << "_r\": " << gr;

            // L は 1000Hz、R は 500Hz が主成分になるよう素材を作っている。
            // A1 と A2 の両方が同じ構成なので、mix されていれば
            // どちらのチャンネルにも該当成分が残る。
            if (hz == 1000.0 && gl < 0.01)
                problems.push_back("audio frame " + std::to_string(frame) +
                                   ": L に 1000Hz 成分がありません (" + std::to_string(gl) + ")");
            if (hz == 500.0 && gr < 0.01)
                problems.push_back("audio frame " + std::to_string(frame) +
                                   ": R に 500Hz 成分がありません (" + std::to_string(gr) + ")");
        }
        js << " }";
        mvm_mlt_audio_free(&au);
    }
    js << (firstAudio ? "],\n" : "\n  ],\n");

    // 検査が 1 件も走っていないなら成功にしない
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
    closeScenario(s);

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

        MarkerRead m = readMarker(img.rgba, img.width, img.height);
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

struct ScrubRequest {
    long long generation = 0;
    long long frame = 0;
    Clock::time_point submitted;
};

struct ScrubResult {
    long long generation = 0;
    long long frame = 0;
    long long markerValue = -1;
    bool markerSync = false;
    double latencyMs = 0;
};

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
    // 人間のスクラブ操作を模して要求を発行する間隔
    int submitIntervalUs = (int)std::strtol(a.get("submit-interval-us", "0").c_str(), nullptr, 10);

    std::vector<long long> frames = makePattern(pattern, length, count, seed);

    // --- coalescing モデル ---
    // producer 側: 要求を投入する
    // consumer 側: 処理中でない未処理要求は最新 1 件だけ保持する
    std::mutex mu;
    ScrubRequest pending;
    bool hasPending = false;
    bool done = false;

    long long submitted = 0, superseded = 0, decoded = 0, accepted = 0, staleRejected = 0;
    long long lastAcceptedGeneration = -1;
    long long lastAcceptedFrame = -1;
    long long markerMismatch = 0;
    std::vector<double> latencies;

    auto tStart = Clock::now();

    std::thread consumer([&] {
        for (;;) {
            ScrubRequest req;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (!hasPending) {
                    if (done)
                        return;
                    req.generation = -1;
                } else {
                    req = pending;
                    hasPending = false;
                }
            }
            if (req.generation < 0) {
                std::this_thread::yield();
                continue;
            }

            MvmMltImage img{};
            char err[512] = {0};
            auto t0 = Clock::now();
            int r = mvm_mlt_compose_frame(s.handle, req.frame, &img, err, sizeof(err));
            auto t1 = Clock::now();
            (void)t0;

            ScrubResult res;
            res.generation = req.generation;
            res.frame = req.frame;
            res.latencyMs = msSince(req.submitted, t1);

            if (r == 0) {
                MarkerRead m = readMarker(img.rgba, img.width, img.height);
                res.markerSync = m.syncOk;
                res.markerValue = m.value;
                mvm_mlt_image_free(&img);
            }

            {
                std::lock_guard<std::mutex> lk(mu);
                decoded++;
                // 古い結果は表示対象にしない。
                // generation が現在の最新受理より古ければ stale。
                if (res.generation < lastAcceptedGeneration) {
                    staleRejected++;
                } else {
                    accepted++;
                    lastAcceptedGeneration = res.generation;
                    lastAcceptedFrame = res.frame;
                    latencies.push_back(res.latencyMs);
                    if (!res.markerSync || res.markerValue != res.frame)
                        markerMismatch++;
                }
            }
        }
    });

    for (long long i = 0; i < (long long)frames.size(); i++) {
        ScrubRequest req;
        req.generation = i;
        req.frame = frames[(size_t)i];
        req.submitted = Clock::now();
        {
            std::lock_guard<std::mutex> lk(mu);
            if (hasPending)
                superseded++; // 未処理のまま置き換えられた
            pending = req;
            hasPending = true;
            submitted++;
        }
        if (submitIntervalUs > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(submitIntervalUs));
    }

    // 最終要求は必ず処理されなければならない。
    // 投入を止めたうえで、pending が捌けるまで待つ。
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!hasPending) {
                done = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    consumer.join();

    auto tEnd = Clock::now();
    double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();
    double updatesPerSec = elapsedSec > 0 ? (double)accepted / elapsedSec : 0.0;

    long long finalRequestedFrame = frames.empty() ? -1 : frames.back();
    bool finalMatches = (lastAcceptedFrame == finalRequestedFrame);

    Percentiles p = computeStats(latencies);

    std::string csv = a.get("csv");
    if (!csv.empty() && csv != "1") {
        FILE* f = _wfopen(utf8Path(csv).c_str(), L"wb");
        if (f) {
            std::fprintf(f, "metric,value\n");
            std::fprintf(f, "pattern,%s\n", pattern.c_str());
            std::fprintf(f, "submitted,%lld\n", submitted);
            std::fprintf(f, "decoded,%lld\n", decoded);
            std::fprintf(f, "superseded,%lld\n", superseded);
            std::fprintf(f, "accepted,%lld\n", accepted);
            std::fprintf(f, "stale_rejected,%lld\n", staleRejected);
            std::fprintf(f, "updates_per_sec,%.4f\n", updatesPerSec);
            std::fprintf(f, "latency_p50_ms,%.4f\n", p.p50);
            std::fprintf(f, "latency_p95_ms,%.4f\n", p.p95);
            std::fprintf(f, "latency_max_ms,%.4f\n", p.max);
            std::fclose(f);
        }
    }

    std::ostringstream js;
    js << "{\n  \"scenario\": \"" << jsonEscape(s.scenario.name) << "\",\n";
    js << "  \"pattern\": \"" << pattern << "\",\n";
    js << "  \"length\": " << length << ",\n";
    js << "  \"elapsed_sec\": " << elapsedSec << ",\n";
    js << "  \"submitted\": " << submitted << ",\n";
    js << "  \"decoded\": " << decoded << ",\n";
    js << "  \"superseded\": " << superseded << ",\n";
    js << "  \"accepted\": " << accepted << ",\n";
    js << "  \"stale_rejected\": " << staleRejected << ",\n";
    js << "  \"marker_mismatch\": " << markerMismatch << ",\n";
    js << "  \"updates_per_sec\": " << updatesPerSec << ",\n";
    js << "  \"final_requested_frame\": " << finalRequestedFrame << ",\n";
    js << "  \"final_displayed_frame\": " << lastAcceptedFrame << ",\n";
    js << "  \"final_matches\": " << (finalMatches ? "true" : "false") << ",\n";
    js << "  \"latency_ms\": { \"p50\": " << p.p50 << ", \"p95\": " << p.p95
       << ", \"max\": " << p.max << ", \"mean\": " << p.mean << " }\n}\n";

    std::fputs(js.str().c_str(), stdout);
    closeScenario(s);

    if (a.has("check")) {
        double minUps = std::strtod(a.get("min-updates-per-sec", "15").c_str(), nullptr);
        double p95Limit = std::strtod(a.get("max-p95-ms", "200").c_str(), nullptr);
        bool ok = true;
        if (!finalMatches) {
            logMsg("M6 不合格: 最終要求 " + std::to_string(finalRequestedFrame) +
                   " が表示されていません (表示 " + std::to_string(lastAcceptedFrame) + ")");
            ok = false;
        }
        if (markerMismatch != 0) {
            logMsg("M6 不合格: marker 不一致 " + std::to_string(markerMismatch) + " 件");
            ok = false;
        }
        if (updatesPerSec < minUps) {
            logMsg("M6 不合格: updates/sec " + std::to_string(updatesPerSec) + " < " +
                   std::to_string(minUps));
            ok = false;
        }
        if (p.p95 > p95Limit) {
            logMsg("M6 不合格: latency p95 " + std::to_string(p.p95) + "ms > " +
                   std::to_string(p95Limit) + "ms");
            ok = false;
        }
        // coalescing が機能していない実装を検出する。
        // 全要求を逐次処理していれば decoded == submitted になる。
        if (a.has("require-coalescing") && superseded == 0) {
            logMsg("coalescing が機能していません: superseded が 0 です");
            ok = false;
        }
        if (!ok)
            return kExitMismatch;
    }
    return kExitOk;
}
