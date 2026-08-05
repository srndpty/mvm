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

// 一時ファイルへ書いて flush/close を確認してから正規名へ rename する。
//
// 検証前に正規の out を書いてしまうと、required の解決に失敗した実行でも
// 「それらしい scenario」が残る。次の実行がそれを拾えば、
// proxy が効いていない構成で測定してしまう。
// 書き込みは検証が全て通った後にしか行わない。
bool writeFileAtomic(const std::string& path, const std::string& data, std::string& err) {
    std::string tmp = path + ".mvmtmp";
    FILE* f = _wfopen(utf8Path(tmp).c_str(), L"wb");
    if (!f) {
        err = "一時ファイルを開けません: " + tmp;
        return false;
    }
    size_t wrote = std::fwrite(data.data(), 1, data.size(), f);
    bool ok = (wrote == data.size());
    if (ok && std::fflush(f) != 0)
        ok = false;
    if (std::fclose(f) != 0)
        ok = false;
    if (!ok) {
        err = "一時ファイルへ書き切れません: " + tmp;
        std::error_code ec;
        fs::remove(utf8Path(tmp), ec);
        return false;
    }
    std::error_code ec;
    // MoveFileEx 相当。既存の out を置き換える。
    fs::rename(utf8Path(tmp), utf8Path(path), ec);
    if (ec) {
        err = "rename できません: " + tmp + " -> " + path;
        std::error_code ec2;
        fs::remove(utf8Path(tmp), ec2);
        return false;
    }
    return true;
}

// scenario JSON から media_root を取り出す。
// proxy の実在確認をこの基準で行う。
std::string findMediaRoot(const std::string& json) {
    const std::string key = "\"media_root\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos)
        return {};
    size_t q1 = json.find('"', json.find(':', pos + key.size()));
    if (q1 == std::string::npos)
        return {};
    size_t q2 = q1 + 1;
    std::string v;
    while (q2 < json.size() && json[q2] != '"') {
        if (json[q2] == '\\' && q2 + 1 < json.size()) {
            v += json[q2 + 1];
            q2 += 2;
            continue;
        }
        v += json[q2];
        q2++;
    }
    return v;
}

std::string dirOf(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return (i == std::string::npos) ? std::string(".") : p.substr(0, i);
}

std::string joinPath(const std::string& base, const std::string& rel) {
    if (base.empty())
        return rel;
    if (rel.size() > 1 && (rel[1] == ':' || rel[0] == '/' || rel[0] == '\\'))
        return rel;
    return base + "/" + rel;
}

bool isRegularFileUtf8(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(utf8Path(path), ec);
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
//
// --- required / optional の契約 (S7.1 で明確化) ---------------------------
//
// 「全 source を proxy 必須」にはできない。WAV や音声専用 source まで
// proxy 必須になってしまうためである。そこで 2 種類に分ける。
//
//   required (--require-proxy-ids で明示)
//     - scenario にその id が実在すること
//     - --map に登録されていること
//     - preview では **全出現が** proxy へ置換されること
//     - final では **1 件も** proxy にならないこと
//     いずれか 1 つでも破れたら exit 4 で fail-closed。
//
//   optional (required に挙げていない source)
//     - 未登録なら original のまま。これは正常
//     - ただし id と件数を必ず報告する。黙って通さない
//
// 旧 --require-proxy (「1 件以上置換されれば成功」) は契約として弱すぎる。
// V1 だけ proxy 化されて V2 が original のままでも通ってしまい、
// 実際に S7 の M8 はその状態で「proxy 評価」として報告されていた。
// diagnostic 専用へ降格し、判定には使わない。

int cmdResolveProxy(const bench::Args& a) {
    using namespace bench;

    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench resolve-proxy <scenario.json> --target preview|final "
               "--map <id=proxyPath;...> [--require-proxy-ids <id;id>] [--out <path>]");
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

    // --require-proxy-ids の解析。';' 区切り。
    std::vector<std::string> requiredIds;
    {
        std::string s = a.get("require-proxy-ids");
        std::set<std::string> seen;
        size_t p = 0;
        while (p < s.size()) {
            size_t sep = s.find(';', p);
            std::string item = s.substr(p, sep == std::string::npos ? std::string::npos : sep - p);
            if (!item.empty()) {
                // 重複指定は使い方の誤り。黙って 1 件として扱うと
                // 「2 件必須のつもりが 1 件だった」に気づけない。
                if (!seen.insert(item).second) {
                    logMsg("--require-proxy-ids に同じ id が二度あります: " + item);
                    return kExitUsage;
                }
                requiredIds.push_back(item);
            }
            if (sep == std::string::npos)
                break;
            p = sep + 1;
        }
    }

    ResolveTarget rt = (target == "preview") ? ResolveTarget::Preview : ResolveTarget::Final;

    // proxy の実在確認は scenario の media_root を基準に行う。
    // media_root は scenario ファイルからの相対である。
    std::string mediaRootRel = findMediaRoot(json);
    std::string mediaRoot = mediaRootRel.empty() ? dirOf(a.positional[0])
                                                 : joinPath(dirOf(a.positional[0]), mediaRootRel);

    std::vector<SourceRef> refs = findSourceRefs(json);
    if (refs.empty()) {
        logMsg("scenario に source フィールドがありません");
        return kExitError;
    }

    // 後ろから置換する。前から置換すると以降の位置がずれる。
    std::string out = json;
    long long replaced = 0, keptOriginal = 0, unknown = 0;
    std::vector<std::string> lines;
    std::map<std::string, long long> unregisteredIds;
    // required id ごとに「scenario に何回現れたか」「何回 proxy へ置換したか」。
    // 同じ素材が複数 clip にある場合、全出現を置換できたかを見る。
    std::map<std::string, long long> occurrences, proxied;
    for (const auto& id : requiredIds) {
        occurrences[id] = 0;
        proxied[id] = 0;
    }

    for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
        auto occ = occurrences.find(it->value);
        if (occ != occurrences.end())
            occ->second++;

        ResolveResult r = resolver.resolve(it->value, rt);
        if (!r.ok()) {
            // 未登録は「そのまま」でよい。proxy 対象でない素材があるのは正常。
            // ただし id と件数を必ず報告する。黙って通さない。
            unknown++;
            unregisteredIds[it->value]++;
            lines.push_back(it->value + " -> (未登録のためそのまま)");
            continue;
        }
        if (r.usedProxy()) {
            out.replace(it->valueStart, it->valueLen, r.path);
            replaced++;
            auto pr = proxied.find(it->value);
            if (pr != proxied.end())
                pr->second++;
            lines.push_back(it->value + " -> " + r.path + " [proxy]");
        } else {
            keptOriginal++;
            lines.push_back(it->value + " -> " + r.path + " [original: " + r.reason + "]");
        }
    }
    std::reverse(lines.begin(), lines.end());

    // --- required proxy id の検査 (fail-closed) ---------------------------
    std::vector<std::string> resolvedRequired, missingRequired;
    long long resolvedOccurrences = 0;
    for (const auto& id : requiredIds) {
        long long occ = occurrences[id];
        long long px = proxied[id];
        resolvedOccurrences += px;

        if (occ == 0) {
            missingRequired.push_back(id + " (scenario に存在しない)");
            continue;
        }
        if (!resolver.has(id)) {
            missingRequired.push_back(id + " (--map に登録されていない)");
            continue;
        }
        // 文字列置換が成功しただけでは required 解決とみなさない。
        // proxy の実体が無ければ、その scenario で測っても
        // 「proxy を使った」ことにならない。
        {
            ResolveResult pv = resolver.resolve(id, ResolveTarget::Preview);
            std::string rel = pv.usedProxy() ? pv.path : std::string();
            std::string abs = joinPath(mediaRoot, rel);
            if (rel.empty() || !isRegularFileUtf8(abs)) {
                missingRequired.push_back(id + " (proxy の実体がありません: " + abs + ")");
                continue;
            }
        }
        if (rt == ResolveTarget::Preview) {
            if (px != occ) {
                missingRequired.push_back(id + " (出現 " + std::to_string(occ) + " 件中 " +
                                          std::to_string(px) + " 件しか proxy へ置換されていない)");
                continue;
            }
        } else {
            // final では proxy になってはいけない。
            if (px != 0) {
                missingRequired.push_back(id + " (final なのに proxy へ置換された)");
                continue;
            }
        }
        resolvedRequired.push_back(id);
    }

    // final で proxy が 1 件でも混ざったら、それは resolver の設計が壊れている。
    if (rt == ResolveTarget::Final && replaced != 0) {
        logMsg("final なのに proxy へ解決した source が " + std::to_string(replaced) +
               " 件あります");
        return kExitMismatch;
    }

    std::string outPath = a.get("out");

    std::ostringstream js;
    js << "{\n  \"target\": \"" << target << "\",\n";
    js << "  \"source_refs\": " << refs.size() << ",\n";
    js << "  \"resolved_to_proxy\": " << replaced << ",\n";
    js << "  \"kept_original\": " << keptOriginal << ",\n";
    js << "  \"unregistered\": " << unknown << ",\n";
    js << "  \"out\": \"" << jsonEscape(outPath) << "\",\n";

    auto emitList = [&](const char* key, const std::vector<std::string>& v) {
        js << "  \"" << key << "\": [";
        for (size_t i = 0; i < v.size(); i++)
            js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(v[i]) << "\"";
        js << (v.empty() ? "],\n" : "\n  ],\n");
    };
    emitList("required_proxy_ids", requiredIds);
    emitList("resolved_required_ids", resolvedRequired);
    emitList("missing_required_ids", missingRequired);
    js << "  \"resolved_occurrences\": " << resolvedOccurrences << ",\n";
    // 未登録 optional は id だけでなく出現件数も出す。
    // 「1 箇所だけ original が残っていた」を件数で追えるようにする。
    js << "  \"unregistered_optional_sources\": [";
    {
        bool first = true;
        for (const auto& kv : unregisteredIds) {
            js << (first ? "\n    " : ",\n    ") << "{ \"id\": \"" << jsonEscape(kv.first)
               << "\", \"occurrences\": " << kv.second << " }";
            first = false;
        }
        js << (unregisteredIds.empty() ? "],\n" : "\n  ],\n");
    }

    js << "  \"mapping\": [";
    for (size_t i = 0; i < lines.size(); i++)
        js << (i ? ",\n    " : "\n    ") << "\"" << jsonEscape(lines[i]) << "\"";
    js << (lines.empty() ? "]\n}\n" : "\n  ]\n}\n");
    std::fputs(js.str().c_str(), stdout);

    // required が 1 件でも解決できていなければ専用の exit code で落とす。
    // 「proxy が効いていない構成」を測定に使わせないための門である。
    //
    // **ここで返る場合、out は一切書いていない。**
    // 検証前に書くと、失敗した実行でも「それらしい scenario」が残り、
    // 次の実行がそれを拾って proxy 無しの構成で測ってしまう。
    if (!missingRequired.empty()) {
        logMsg("required proxy id が解決できませんでした:");
        for (const auto& m : missingRequired)
            logMsg("  " + m);
        logMsg("out は作成も変更もしていません: " +
               (outPath.empty() ? std::string("(なし)") : outPath));
        return kExitRequiredProxyUnresolved;
    }

    // 旧契約。diagnostic 専用であり、判定には使わない。
    if (a.has("require-proxy") && replaced == 0) {
        logMsg("[diagnostic] proxy へ解決された source が 1 件もありません "
               "(この検査は弱い。判定には --require-proxy-ids を使うこと)");
        return kExitMismatch;
    }

    // 全ての検証を通過した後にだけ書く。一時ファイル経由で atomic に置き換える。
    if (!outPath.empty() && outPath != "1") {
        std::string werr;
        if (!writeFileAtomic(outPath, out, werr)) {
            logMsg(werr);
            return kExitError;
        }
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
