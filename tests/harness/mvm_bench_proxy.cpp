// mvm Phase 0 / S7 - proxy の解決と frame mapping 検証
//
// 分けている理由:
//   - resolve-proxy   : MLT graph を作る前に resource path を決める (H)
//   - proxy-mapping   : original と proxy の frame 対応を検証する (I)
//
// proxy 情報は MLT XML にも MLT property にも持たせない。
// どのファイルを開くかは scenario を書き換える時点で確定させる。

#include "../../src/media/mlt/mvm_mlt_probe.h"
#include "../../src/media/mlt/mvm_mlt_runtime.h"
#include "../../src/util/mvm_win_utf8.h"
#include "bench_common.h"
#include "proxy_resolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace bench;

// --------------------------------------------------------------------------
// ごく小さな JSON 読み取り。scenario の書き換えは文字列置換ではなく
// 「source フィールドの値だけ」を対象にする。
// --------------------------------------------------------------------------

// "source": "xxx" の xxx を列挙する。位置も返す。
struct SourceRef {
    size_t valueStart = 0;
    size_t valueLen = 0;
    std::string value;
};

std::vector<SourceRef> findSourceRefs(const std::string& json) {
    std::vector<SourceRef> out;
    const std::string key = "\"source\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        size_t colon = json.find(':', pos + key.size());
        if (colon == std::string::npos)
            break;
        size_t q1 = json.find('"', colon);
        if (q1 == std::string::npos)
            break;
        size_t q2 = q1 + 1;
        std::string v;
        while (q2 < json.size() && json[q2] != '"') {
            if (json[q2] == '\\' && q2 + 1 < json.size()) {
                v += json[q2];
                v += json[q2 + 1];
                q2 += 2;
                continue;
            }
            v += json[q2];
            q2++;
        }
        if (q2 >= json.size())
            break;
        SourceRef r;
        r.valueStart = q1 + 1;
        r.valueLen = q2 - (q1 + 1);
        r.value = v;
        out.push_back(r);
        pos = q2;
    }
    return out;
}

std::string readFileUtf8(const std::string& path, bool& ok) {
    ok = false;
    FILE* f = _wfopen(utf8Path(path).c_str(), L"rb");
    if (!f)
        return {};
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    std::fclose(f);
    ok = true;
    return s;
}

bool writeFileUtf8(const std::string& path, const std::string& data) {
    FILE* f = _wfopen(utf8Path(path).c_str(), L"wb");
    if (!f)
        return false;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

// ==========================================================================
// resolve-proxy
// ==========================================================================
//
// scenario の source を、preview なら proxy へ、final なら original へ
// 解決した新しい scenario を書き出す。
//
// --map は「素材 id = proxy ファイル名」の行を並べたテキスト。
// 未登録の素材は解決しない。黙って元のパスを使う fallback は入れない。

int cmdResolveProxy(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench resolve-proxy <scenario.json> --target preview|final "
               "--map <id=proxyPath;...> [--out <path>] [--media-root-out <rel>]");
        return kExitUsage;
    }

    std::string target = a.get("target", "preview");
    if (target != "preview" && target != "final") {
        logMsg("--target は preview か final です: " + target);
        return kExitUsage;
    }

    bool ok = false;
    std::string json = readFileUtf8(a.positional[0], ok);
    if (!ok) {
        logMsg("scenario を読めません: " + a.positional[0]);
        return kExitError;
    }

    // --map の解析。"id=path" を ';' 区切りで並べる。
    ProxyResolver resolver;
    std::string mapStr = a.get("map");
    size_t start = 0;
    while (start < mapStr.size()) {
        size_t sep = mapStr.find(';', start);
        std::string item =
            mapStr.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!item.empty()) {
            size_t eq = item.find('=');
            if (eq == std::string::npos) {
                logMsg("--map の書式が不正です (id=path): " + item);
                return kExitUsage;
            }
            ProxyEntry e;
            e.sourceId = item.substr(0, eq);
            e.originalPath = e.sourceId; // scenario 上は media_root からの相対
            e.proxyPath = item.substr(eq + 1);
            e.proxyEnabled = true;
            if (!resolver.add(e)) {
                logMsg("--map に同じ id が二度あります: " + e.sourceId);
                return kExitUsage;
            }
        }
        if (sep == std::string::npos)
            break;
        start = sep + 1;
    }

    ResolveTarget rt = (target == "preview") ? ResolveTarget::Preview : ResolveTarget::Final;

    std::vector<SourceRef> refs = findSourceRefs(json);
    if (refs.empty()) {
        logMsg("scenario に source フィールドがありません");
        return kExitError;
    }

    // 後ろから置換する。前から置換すると以降の位置がずれる。
    std::string out = json;
    long long replaced = 0, keptOriginal = 0, unknown = 0;
    std::vector<std::string> lines;
    for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
        ResolveResult r = resolver.resolve(it->value, rt);
        if (!r.ok()) {
            // 未登録は「そのまま」でよい。proxy 対象でない素材があるのは正常。
            // ただし件数を必ず報告する。黙って通さない。
            unknown++;
            lines.push_back(it->value + " -> (未登録のためそのまま)");
            continue;
        }
        if (r.usedProxy()) {
            out.replace(it->valueStart, it->valueLen, r.path);
            replaced++;
            lines.push_back(it->value + " -> " + r.path + " [proxy]");
        } else {
            keptOriginal++;
            lines.push_back(it->value + " -> " + r.path + " [original: " + r.reason + "]");
        }
    }
    std::reverse(lines.begin(), lines.end());

    // final で proxy が 1 件でも混ざったら、それは resolver の設計が壊れている。
    if (rt == ResolveTarget::Final && replaced != 0) {
        logMsg("final なのに proxy へ解決した source が " + std::to_string(replaced) +
               " 件あります");
        return kExitMismatch;
    }

    std::string outPath = a.get("out");
    if (!outPath.empty() && outPath != "1") {
        if (!writeFileUtf8(outPath, out)) {
            logMsg("書き出せません: " + outPath);
            return kExitError;
        }
    }

    std::ostringstream js;
    js << "{\n  \"target\": \"" << target << "\",\n";
    js << "  \"source_refs\": " << refs.size() << ",\n";
    js << "  \"resolved_to_proxy\": " << replaced << ",\n";
    js << "  \"kept_original\": " << keptOriginal << ",\n";
    js << "  \"unregistered\": " << unknown << ",\n";
    js << "  \"out\": \"" << jsonEscape(outPath) << "\",\n";
    js << "  \"mapping\": [";
    for (size_t i = 0; i < lines.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(lines[i]) << "\"";
    js << (lines.empty() ? "]\n}\n" : "\n  ]\n}\n");
    std::fputs(js.str().c_str(), stdout);

    if (a.has("require-proxy") && replaced == 0) {
        logMsg("proxy へ解決された source が 1 件もありません");
        return kExitMismatch;
    }
    return kExitOk;
}

// ==========================================================================
// proxy-mapping
// ==========================================================================
//
// original と proxy で「同じフレーム番号が同じ内容を指す」ことを検証する。
//
// 画素の完全一致は要求しない。解像度もコーデックも違うので当然ずれる。
// **マーカー値と位置の一致だけを正しさの gate にする。**
// 画質は SSIM などの診断値として別に測る (ここでは扱わない)。

int cmdProxyMapping(const bench::Args& a) {
    using namespace bench;

    std::string origPath = a.get("original");
    std::string proxyPath = a.get("proxy");
    if (origPath.empty() || proxyPath.empty()) {
        logMsg("使い方: mvm_bench proxy-mapping --original <path> --proxy <path> "
               "[--random 200] [--seed 20260804]");
        return kExitUsage;
    }

    if (!initMlt())
        return kExitError;

    MvmMltProbeResult po{}, pp{};
    if (mvm_mlt_probe_file(origPath.c_str(), &po) != 0) {
        logMsg("original を probe できません: " + origPath);
        mvm_mlt_runtime_shutdown();
        return kExitError;
    }
    if (mvm_mlt_probe_file(proxyPath.c_str(), &pp) != 0) {
        logMsg("proxy を probe できません: " + proxyPath);
        mvm_mlt_runtime_shutdown();
        return kExitError;
    }

    std::vector<std::string> problems;

    // fps は rational で完全一致を要求する。
    // 60000/1001 と 60/1 を「だいたい同じ」で通すと尺がずれる。
    if (po.fps_num != pp.fps_num || po.fps_den != pp.fps_den)
        problems.push_back("fps rational が違います: original " + std::to_string(po.fps_num) + "/" +
                           std::to_string(po.fps_den) + " vs proxy " + std::to_string(pp.fps_num) +
                           "/" + std::to_string(pp.fps_den));

    if (po.frame_count != pp.frame_count)
        problems.push_back("frame count が違います: original " + std::to_string(po.frame_count) +
                           " vs proxy " + std::to_string(pp.frame_count));

    // duration の許容差は 1 frame 未満。
    double fps = (po.fps_den != 0) ? (double)po.fps_num / (double)po.fps_den : 0.0;
    double frameSec = (fps > 0) ? 1.0 / fps : 0.0;
    double durDiff = std::fabs(po.duration_sec - pp.duration_sec);
    if (frameSec > 0 && durDiff >= frameSec)
        problems.push_back("duration の差が 1 frame 以上あります: " + std::to_string(durDiff) +
                           "s (1 frame = " + std::to_string(frameSec) + "s)");

    long long length = po.frame_count;

    // 検証するフレーム。先頭・末尾・keyframe 境界を重点的に含める。
    std::set<long long> frameSet;
    for (long long f : {0LL, 1LL, 2LL, 137LL, 299LL, 600LL, 1799LL})
        if (f < length)
            frameSet.insert(f);
    if (length >= 1) {
        frameSet.insert(length - 1);
        if (length >= 2)
            frameSet.insert(length - 2);
    }
    // GOP 境界付近。proxy は GOP 12 / 1 なので 12 の倍数の前後を含める。
    for (long long g = 12; g < length && g <= 120; g += 12) {
        frameSet.insert(g - 1);
        frameSet.insert(g);
        frameSet.insert(g + 1);
    }

    int randomCount = (int)std::strtol(a.get("random", "200").c_str(), nullptr, 10);
    unsigned seed = (unsigned)std::strtoul(a.get("seed", "20260804").c_str(), nullptr, 10);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<long long> uni(0, length > 0 ? length - 1 : 0);
    for (int i = 0; i < randomCount; i++)
        frameSet.insert(uni(rng));

    std::vector<long long> frames(frameSet.begin(), frameSet.end());

    long long origMismatch = 0, proxyMismatch = 0, crossMismatch = 0, decodeFail = 0;
    std::vector<std::string> examples;

    for (long long f : frames) {
        MvmMltImage io{}, ip{};
        char e1[512] = {0}, e2[512] = {0};
        bool ok1 = mvm_mlt_decode_frame(origPath.c_str(), f, &io, e1, sizeof(e1)) == 0;
        bool ok2 = mvm_mlt_decode_frame(proxyPath.c_str(), f, &ip, e2, sizeof(e2)) == 0;
        if (!ok1 || !ok2) {
            decodeFail++;
            if (examples.size() < 10)
                examples.push_back("frame " + std::to_string(f) + ": decode 失敗 " +
                                   (ok1 ? "" : std::string("orig=") + e1) +
                                   (ok2 ? "" : std::string(" proxy=") + e2));
            if (ok1)
                mvm_mlt_image_free(&io);
            if (ok2)
                mvm_mlt_image_free(&ip);
            continue;
        }
        // proxy は縮小されているのでセル幅も縮む。
        // 64px 決め打ちで読むと必ず読めず、「proxy が壊れている」と
        // 誤って結論することになる。
        MarkerRead mo = readMarker(io.rgba, io.width, io.height);
        MarkerRead mp = readMarkerScaled(ip.rgba, ip.width, ip.height, po.width);

        if (!mo.syncOk || mo.value != f) {
            origMismatch++;
            if (examples.size() < 10)
                examples.push_back("frame " + std::to_string(f) + ": original marker=" +
                                   std::to_string(mo.value) + " sync=" + (mo.syncOk ? "1" : "0"));
        }
        if (!mp.syncOk || mp.value != f) {
            proxyMismatch++;
            if (examples.size() < 10)
                examples.push_back("frame " + std::to_string(f) + ": proxy marker=" +
                                   std::to_string(mp.value) + " sync=" + (mp.syncOk ? "1" : "0"));
        }
        if (mo.syncOk && mp.syncOk && mo.value != mp.value) {
            crossMismatch++;
            if (examples.size() < 10)
                examples.push_back("frame " + std::to_string(f) + ": original " +
                                   std::to_string(mo.value) + " vs proxy " +
                                   std::to_string(mp.value));
        }
        mvm_mlt_image_free(&io);
        mvm_mlt_image_free(&ip);
    }

    mvm_mlt_runtime_shutdown();

    if (decodeFail > 0)
        problems.push_back("decode 失敗が " + std::to_string(decodeFail) + " 件");
    if (origMismatch > 0)
        problems.push_back("original の marker 不一致が " + std::to_string(origMismatch) + " 件");
    if (proxyMismatch > 0)
        problems.push_back("proxy の marker 不一致が " + std::to_string(proxyMismatch) + " 件");
    if (crossMismatch > 0)
        problems.push_back("original と proxy の marker 不一致が " + std::to_string(crossMismatch) +
                           " 件");

    std::ostringstream js;
    js << "{\n  \"original\": \"" << jsonEscape(origPath) << "\",\n";
    js << "  \"proxy\": \"" << jsonEscape(proxyPath) << "\",\n";
    js << "  \"original_size\": { \"width\": " << po.width << ", \"height\": " << po.height
       << " },\n";
    js << "  \"proxy_size\": { \"width\": " << pp.width << ", \"height\": " << pp.height << " },\n";
    js << "  \"original_fps\": \"" << po.fps_num << "/" << po.fps_den << "\",\n";
    js << "  \"proxy_fps\": \"" << pp.fps_num << "/" << pp.fps_den << "\",\n";
    js << "  \"fps_exact_match\": "
       << ((po.fps_num == pp.fps_num && po.fps_den == pp.fps_den) ? "true" : "false") << ",\n";
    js << "  \"original_frames\": " << po.frame_count << ",\n";
    js << "  \"proxy_frames\": " << pp.frame_count << ",\n";
    js << "  \"frame_count_match\": " << (po.frame_count == pp.frame_count ? "true" : "false")
       << ",\n";
    js << "  \"original_duration_sec\": " << po.duration_sec << ",\n";
    js << "  \"proxy_duration_sec\": " << pp.duration_sec << ",\n";
    js << "  \"duration_diff_sec\": " << durDiff << ",\n";
    js << "  \"one_frame_sec\": " << frameSec << ",\n";
    js << "  \"checked_frames\": " << frames.size() << ",\n";
    js << "  \"random_count\": " << randomCount << ",\n";
    js << "  \"seed\": " << seed << ",\n";
    js << "  \"decode_failures\": " << decodeFail << ",\n";
    js << "  \"original_marker_mismatch\": " << origMismatch << ",\n";
    js << "  \"proxy_marker_mismatch\": " << proxyMismatch << ",\n";
    js << "  \"cross_marker_mismatch\": " << crossMismatch << ",\n";
    js << "  \"examples\": [";
    for (size_t i = 0; i < examples.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(examples[i]) << "\"";
    js << (examples.empty() ? "],\n" : "\n  ],\n");
    js << "  \"problems\": [";
    for (size_t i = 0; i < problems.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(problems[i]) << "\"";
    js << (problems.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::string jsonOut = a.get("json");
    if (!jsonOut.empty() && jsonOut != "1")
        writeFileUtf8(jsonOut, js.str());
    std::fputs(js.str().c_str(), stdout);

    if (!problems.empty()) {
        for (const auto& p : problems)
            logMsg("  " + p);
        return kExitMismatch;
    }
    return kExitOk;
}
