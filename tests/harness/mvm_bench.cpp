// mvm Phase 0 / S4 - 検証用 CLI
//
// 位置づけ:
//   Qt に依存しない。MLT のヘッダも include しない (src/media/mlt/ の
//   C API 越しにしか触らない)。
//
// サブコマンド:
//   doctor                        MLT ランタイムの健全性検査
//   probe <path> [--json <out>]   MLT と ffprobe の解析結果を比較
//   decode <path> --frame <n> --output <png>
//                                 指定フレームを PNG 化し、マーカーを照合
//   verify-media <manifest>       manifest 記載の全素材を検証
//   explore <video> [audio]       MLT の API と property を実測する (S3 調査用)
//
//   compose / verify-compose / seek-bench / scrub-bench は
//   mvm_bench_compose.cpp にある。
//
// 出力方針:
//   機械可読 JSON は stdout または --json、診断ログは stderr。
//   成功 0 / 実行時エラー 1 / 使い方の誤り 2 / 検証不一致 3。

#include "bench_common.h"
#include "media/mlt/mvm_mlt_explore.h"

// compose 系サブコマンド (mvm_bench_compose.cpp)
int cmdCompose(const bench::Args& a);
int cmdVerifyCompose(const bench::Args& a);
int cmdSeekBench(const bench::Args& a);
int cmdScrubBench(const bench::Args& a);
int cmdRenderAudio(const bench::Args& a);
int cmdVerifyAudio(const bench::Args& a);
int cmdAudioGraphProbe(const bench::Args& a);
int cmdAnalyzeWav(const bench::Args& a);
int cmdSoak(const bench::Args& a);

namespace {

using namespace bench;

// --------------------------------------------------------------------------
// doctor
// --------------------------------------------------------------------------

int cmdDoctor(const Args& a) {
    // profile の期待値は上書き可能にする。negative test で
    // 「誤った profile を成功扱いしないこと」を確認するため。
    std::string profileName = a.get("profile", "atsc_1080p_60");
    int wantW = (int)std::strtol(a.get("expect-width", "1920").c_str(), nullptr, 10);
    int wantH = (int)std::strtol(a.get("expect-height", "1080").c_str(), nullptr, 10);
    int wantFpsNum = (int)std::strtol(a.get("expect-fps-num", "60").c_str(), nullptr, 10);
    int wantFpsDen = (int)std::strtol(a.get("expect-fps-den", "1").c_str(), nullptr, 10);
    // SAR も既定で照合する。取り違えると V12 で「なぜか横に伸びる」形で表面化する。
    int wantSarNum = (int)std::strtol(a.get("expect-sar-num", "1").c_str(), nullptr, 10);
    int wantSarDen = (int)std::strtol(a.get("expect-sar-den", "1").c_str(), nullptr, 10);

    if (!initMlt())
        return kExitError;

    MvmMltDoctorReport report{};
    report.module_dir = gModuleDir.c_str();
    report.data_dir = gDataDir.c_str();

    int issues = mvm_mlt_doctor_run(profileName.c_str(), wantW, wantH, wantFpsNum, wantFpsDen,
                                    wantSarNum, wantSarDen, &report);

    if (a.has("json")) {
        std::string out = a.get("json");
        if (out == "-" || out == "1") {
            mvm_mlt_doctor_print_json(&report, stdout);
        } else {
            FILE* f = _wfopen(utf8Path(out).c_str(), L"wb");
            if (!f) {
                logMsg("JSON を書き出せません: " + out);
                mvm_mlt_runtime_shutdown();
                return kExitError;
            }
            mvm_mlt_doctor_print_json(&report, f);
            std::fclose(f);
        }
    } else {
        mvm_mlt_doctor_print(&report, stdout);
    }
    mvm_mlt_doctor_print_summary_line(&report, stderr);

    mvm_mlt_runtime_shutdown();

    // 静かな縮退を成功扱いしない。
    return issues == 0 ? kExitOk : kExitMismatch;
}

// --------------------------------------------------------------------------
// probe
// --------------------------------------------------------------------------

struct Mismatch {
    std::string field, mlt, ffprobe;
};

int cmdProbe(const Args& a) {
    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench probe <path> [--json <out>]");
        return kExitUsage;
    }
    std::string path = a.positional[0];

    if (!fs::exists(utf8Path(path))) {
        logMsg("素材が見つかりません: " + path);
        logMsg("素材を生成するには:");
        logMsg("    pwsh scripts/make-testmedia.ps1 -Mode Smoke");
        return kExitError;
    }

    if (!initMlt())
        return kExitError;

    if (a.has("dump-props")) {
        mvm_mlt_dump_properties(path.c_str(), stderr);
    }

    MvmMltProbeResult mlt{};
    int rc = mvm_mlt_probe_file(path.c_str(), &mlt);

    auto ffJson = runFfprobe(path);
    FfprobeInfo ff;
    if (ffJson)
        ff = parseFfprobe(*ffJson);

    std::vector<Mismatch> mismatches;
    auto cmpStr = [&](const char* field, const std::string& m, const std::string& f) {
        if (m != f)
            mismatches.push_back({field, m, f});
    };
    auto cmpInt = [&](const char* field, long long m, long long f) {
        if (m != f)
            mismatches.push_back({field, std::to_string(m), std::to_string(f)});
    };

    // 静止画は MLT が length を INT_MAX で返す (尺が無限)。
    // ffprobe の 1 フレーム / 25fps 既定値と比較しても意味がないので除外する。
    // 黙って丸めるのではなく、除外したことを JSON に残す。
    bool isImage = mlt.is_unbounded_length ||
                   (ff.hasVideo && (ff.container.find("image2") != std::string::npos ||
                                    ff.container.find("png") != std::string::npos));

    // MLT SAR は profile 由来。ffprobe SAR は "1:1" 形式の文字列。
    // どちらも gcd で正規化してから比較する。
    Rational mltSar = makeRational(mlt.sar_num, mlt.sar_den);
    Rational ffSar = parseRational(ff.sar);

    if (rc == 0 && ff.ok) {
        // ストリームの有無そのものを比較する。
        // 「MLT は音声を見つけられなかったが ffprobe は見つけた」という
        // 食い違いは、これを比較しないと最後まで表面化しない。
        if (mlt.has_video != (ff.hasVideo ? 1 : 0))
            mismatches.push_back(
                {"has_video", mlt.has_video ? "true" : "false", ff.hasVideo ? "true" : "false"});
        if (mlt.has_audio != (ff.hasAudio ? 1 : 0))
            mismatches.push_back(
                {"has_audio", mlt.has_audio ? "true" : "false", ff.hasAudio ? "true" : "false"});

        if (ff.hasVideo && mlt.has_video) {
            cmpStr("video_codec", mlt.video_codec, ff.videoCodec);
            cmpStr("pix_fmt", mlt.pix_fmt, ff.pixFmt);
            cmpInt("width", mlt.width, ff.width);
            cmpInt("height", mlt.height, ff.height);

            // SAR は静止画でも意味を持つ (profile 由来でも 1/1 であるべき)
            if (!(mltSar == ffSar))
                mismatches.push_back({"sample_aspect_ratio", mltSar.str(), ffSar.str()});

            // 静止画は ffprobe が既定の 25/1 を返し、MLT は length を INT_MAX とする。
            // 比較しても意味が無いので明示的に除外する (黙って丸めるのではなく除外)。
            if (!isImage) {
                cmpInt("fps_num", mlt.fps_num, ff.fpsNum);
                cmpInt("fps_den", mlt.fps_den, ff.fpsDen);
                cmpInt("frame_count", mlt.frame_count, ff.frameCount);
            }
        }
        if (ff.hasAudio && mlt.has_audio) {
            cmpStr("audio_codec", mlt.audio_codec, ff.audioCodec);
            cmpInt("sample_rate", mlt.sample_rate, ff.sampleRate);
            cmpInt("channels", mlt.channels, ff.channels);
        }
        // duration は映像・音声いずれの素材でも比較する。
        // 音声のみの素材で比較を諦めると、WAV のテストが実質空振りになる。
        if (!isImage && ff.duration > 0) {
            if (mlt.duration_sec <= 0) {
                mismatches.push_back(
                    {"duration_sec", "0 (MLT が尺を出せていません)", std::to_string(ff.duration)});
            } else {
                double diff = std::fabs(mlt.duration_sec - ff.duration);
                if (diff > kDurationToleranceSec) {
                    mismatches.push_back({"duration_sec", std::to_string(mlt.duration_sec),
                                          std::to_string(ff.duration)});
                }
            }
        }
    }

    // --- JSON 出力 ---
    std::ostringstream js;
    js << "{\n";
    js << "  \"path\": \"" << jsonEscape(path) << "\",\n";
    js << "  \"mlt_ok\": " << (rc == 0 ? "true" : "false") << ",\n";
    js << "  \"mlt_error\": \"" << jsonEscape(mlt.error) << "\",\n";
    js << "  \"ffprobe_ok\": " << (ff.ok ? "true" : "false") << ",\n";
    js << "  \"is_image\": " << (isImage ? "true" : "false") << ",\n";
    js << "  \"duration_tolerance_sec\": " << kDurationToleranceSec << ",\n";
    js << "  \"sar_normalized\": { \"mlt\": \"" << mltSar.str() << "\", \"ffprobe\": \""
       << ffSar.str() << "\" },\n";
    js << "  \"mlt\": { \"has_video\": " << (mlt.has_video ? "true" : "false")
       << ", \"has_audio\": " << (mlt.has_audio ? "true" : "false") << ", \"video_codec\": \""
       << jsonEscape(mlt.video_codec) << "\", \"audio_codec\": \"" << jsonEscape(mlt.audio_codec)
       << "\", \"pix_fmt\": \"" << jsonEscape(mlt.pix_fmt) << "\", \"width\": " << mlt.width
       << ", \"height\": " << mlt.height << ", \"fps_num\": " << mlt.fps_num
       << ", \"fps_den\": " << mlt.fps_den << ", \"sar_num\": " << mlt.sar_num
       << ", \"sar_den\": " << mlt.sar_den << ", \"frame_count\": " << mlt.frame_count
       << ", \"duration_sec\": " << mlt.duration_sec
       << ", \"profile_fps_num\": " << mlt.profile_fps_num
       << ", \"profile_fps_den\": " << mlt.profile_fps_den
       << ", \"is_unbounded_length\": " << (mlt.is_unbounded_length ? "true" : "false")
       << ", \"sample_rate\": " << mlt.sample_rate << ", \"channels\": " << mlt.channels
       << ", \"has_alpha\": " << (mlt.has_alpha ? "true" : "false")
       << ", \"alpha_min\": " << mlt.alpha_min << ", \"alpha_max\": " << mlt.alpha_max << " },\n";
    js << "  \"ffprobe\": { \"has_video\": " << (ff.hasVideo ? "true" : "false")
       << ", \"has_audio\": " << (ff.hasAudio ? "true" : "false") << ", \"video_codec\": \""
       << jsonEscape(ff.videoCodec) << "\", \"audio_codec\": \"" << jsonEscape(ff.audioCodec)
       << "\", \"pix_fmt\": \"" << jsonEscape(ff.pixFmt) << "\", \"container\": \""
       << jsonEscape(ff.container) << "\", \"width\": " << ff.width << ", \"height\": " << ff.height
       << ", \"fps_num\": " << ff.fpsNum << ", \"fps_den\": " << ff.fpsDen << ", \"sar\": \""
       << jsonEscape(ff.sar) << "\", \"frame_count\": " << ff.frameCount
       << ", \"duration_sec\": " << ff.duration << ", \"sample_rate\": " << ff.sampleRate
       << ", \"channels\": " << ff.channels << " },\n";
    js << "  \"mismatches\": [";
    for (size_t i = 0; i < mismatches.size(); i++) {
        js << (i ? ",\n    " : "\n    ") << "{ \"field\": \"" << mismatches[i].field
           << "\", \"mlt\": \"" << jsonEscape(mismatches[i].mlt) << "\", \"ffprobe\": \""
           << jsonEscape(mismatches[i].ffprobe) << "\" }";
    }
    js << (mismatches.empty() ? "]\n}\n" : "\n  ]\n}\n");

    if (a.has("json") && a.get("json") != "-" && a.get("json") != "1") {
        std::string out = a.get("json");
        FILE* f = _wfopen(utf8Path(out).c_str(), L"wb");
        if (!f) {
            logMsg("JSON を書き出せません: " + out);
            mvm_mlt_runtime_shutdown();
            return kExitError;
        }
        std::string s = js.str();
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    } else {
        std::fputs(js.str().c_str(), stdout);
    }

    mvm_mlt_runtime_shutdown();

    if (rc != 0) {
        logMsg("MLT が素材を読めませんでした: " + std::string(mlt.error));
        return kExitMismatch;
    }
    if (!ff.ok) {
        logMsg("ffprobe が素材を読めませんでした");
        return kExitError;
    }
    if (!mismatches.empty()) {
        logMsg("MLT と ffprobe の解析結果が一致しません:");
        for (const auto& m : mismatches)
            logMsg("  " + m.field + " : MLT=" + m.mlt + " ffprobe=" + m.ffprobe);
        return kExitMismatch;
    }
    return kExitOk;
}

// --------------------------------------------------------------------------
// decode
// --------------------------------------------------------------------------

int cmdDecode(const Args& a) {
    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench decode <path> --frame <n> [--output <png>] [--expect-marker]");
        return kExitUsage;
    }
    std::string path = a.positional[0];
    long long frame = std::strtoll(a.get("frame", "0").c_str(), nullptr, 10);

    if (!fs::exists(utf8Path(path))) {
        logMsg("素材が見つかりません: " + path);
        logMsg("素材を生成するには:");
        logMsg("    pwsh scripts/make-testmedia.ps1 -Mode Smoke");
        return kExitError;
    }

    if (!initMlt())
        return kExitError;

    MvmMltImage img{};
    char err[512] = {0};
    if (mvm_mlt_decode_frame(path.c_str(), frame, &img, err, sizeof(err)) != 0) {
        logMsg("フレームを取り出せません: " + std::string(err));
        mvm_mlt_runtime_shutdown();
        return kExitMismatch;
    }

    MarkerRead marker = readMarker(img.rgba, img.width, img.height);

    std::string outPng = a.get("output");
    bool wrote = false;
    std::string pngErr;
    if (!outPng.empty() && outPng != "1") {
        wrote = writePng(outPng, img.rgba, img.width, img.height, pngErr);
    }

    std::printf("{\n");
    std::printf("  \"path\": \"%s\",\n", jsonEscape(path).c_str());
    std::printf("  \"requested_frame\": %lld,\n", frame);
    std::printf("  \"width\": %d,\n  \"height\": %d,\n", img.width, img.height);
    std::printf("  \"marker_sync_ok\": %s,\n", marker.syncOk ? "true" : "false");
    std::printf("  \"marker_value\": %lld,\n", marker.value);
    std::printf("  \"marker_luma_min\": %d,\n  \"marker_luma_max\": %d,\n", marker.lumaMin,
                marker.lumaMax);
    std::printf("  \"png_written\": %s", wrote ? "true" : "false");
    if (!pngErr.empty())
        std::printf(",\n  \"png_error\": \"%s\"", jsonEscape(pngErr).c_str());
    std::printf("\n}\n");

    mvm_mlt_image_free(&img);
    mvm_mlt_runtime_shutdown();

    if (!outPng.empty() && outPng != "1" && !wrote) {
        logMsg("PNG を書き出せません: " + pngErr);
        return kExitError;
    }

    // --expect-marker が指定された場合のみ、マーカーと要求フレームの一致を要求する。
    // マーカーを持たない素材 (PNG / WAV) では指定しない。
    if (a.has("expect-marker")) {
        if (!marker.syncOk) {
            logMsg("マーカーの同期セルを読めませんでした。"
                   "素材が想定と異なるか、フレームの取り出し位置がずれています。");
            return kExitMismatch;
        }
        if (marker.value != frame) {
            logMsg("マーカーが要求フレームと一致しません: 要求=" + std::to_string(frame) +
                   " マーカー=" + std::to_string(marker.value));
            return kExitMismatch;
        }
    }
    return kExitOk;
}

// --------------------------------------------------------------------------
// verify-media
// --------------------------------------------------------------------------

int cmdVerifyMedia(const Args& a) {
    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench verify-media <manifest.json>");
        return kExitUsage;
    }
    std::string manifestPath = a.positional[0];

    auto text = readFileUtf8(manifestPath);
    if (!text) {
        logMsg("manifest を読めません: " + manifestPath);
        logMsg("素材と manifest を生成するには:");
        logMsg("    pwsh scripts/make-testmedia.ps1 -Mode Smoke");
        return kExitError;
    }

    JsonParser parser(*text);
    auto root = parser.parse();
    if (!root || root->type != JsonValue::Type::Object) {
        logMsg("manifest を解析できません: " + manifestPath);
        return kExitError;
    }

    // --- manifest 自体の妥当性検査 ---
    // 壊れた manifest を「検証対象 0 件で成功」にしてしまうと、
    // このコマンド全体が無意味になる。構造から先に確かめる。
    constexpr long long kSupportedSchema = 1;

    auto* schema = root->find("schema_version");
    if (!schema || schema->type != JsonValue::Type::Number) {
        logMsg("manifest に schema_version がありません: " + manifestPath);
        return kExitError;
    }
    if (schema->asInt() != kSupportedSchema) {
        logMsg("manifest の schema_version が対応外です: " + std::to_string(schema->asInt()) +
               " (対応: " + std::to_string(kSupportedSchema) + ")");
        return kExitError;
    }

    auto* assets = root->find("assets");
    if (!assets || assets->type != JsonValue::Type::Array) {
        logMsg("manifest に assets がありません");
        return kExitError;
    }
    if (assets->arr.empty()) {
        logMsg("manifest の assets が空です");
        return kExitError;
    }

    fs::path base = utf8Path(manifestPath).parent_path();

    if (!initMlt())
        return kExitError;

    int checked = 0, failed = 0;
    std::map<std::string, int> seenIds;

    static const std::vector<std::string> kValidKinds = {"video", "image", "audio"};

    for (const auto& asset : assets->arr) {
        checked++;

        // 必須フィールドの欠落は、その asset を黙って飛ばすのではなく失敗にする。
        // 飛ばすと「検証したつもり」になる。
        auto* idv = asset.find("id");
        auto* relv = asset.find("relative_path");
        auto* kindv = asset.find("kind");
        auto* expected = asset.find("expected");

        std::vector<std::string> schemaProblems;
        if (!idv || idv->type != JsonValue::Type::String || idv->asString().empty())
            schemaProblems.push_back("id が無いか文字列でない");
        if (!relv || relv->type != JsonValue::Type::String || relv->asString().empty())
            schemaProblems.push_back("relative_path が無いか文字列でない");
        if (!kindv || kindv->type != JsonValue::Type::String)
            schemaProblems.push_back("kind が無いか文字列でない");
        if (!expected || expected->type != JsonValue::Type::Object)
            schemaProblems.push_back("expected が無いかオブジェクトでない");

        std::string id = idv ? idv->asString() : "(id 無し)";
        std::string kind = kindv ? kindv->asString() : "";

        if (kindv && std::find(kValidKinds.begin(), kValidKinds.end(), kind) == kValidKinds.end()) {
            schemaProblems.push_back("未知の kind: '" + kind + "'");
        }

        if (idv && ++seenIds[id] > 1)
            schemaProblems.push_back("id が重複しています");

        if (!schemaProblems.empty()) {
            std::printf("  FAIL %-22s (manifest の構造)\n", id.c_str());
            for (const auto& p : schemaProblems)
                std::printf("         %s\n", p.c_str());
            failed++;
            continue;
        }

        std::string rel = relv->asString();

        // relative_path は manifest 生成時の Windows 形式 (バックスラッシュ) を含む
        fs::path full = base / utf8Path(rel);

        char* fullUtf8 = mvm_wide_to_utf8(full.wstring().c_str());
        std::string fullStr = fullUtf8 ? fullUtf8 : "";
        mvm_str_free(fullUtf8);

        if (!fs::exists(full)) {
            std::printf("  FAIL %-22s ファイルがありません: %s\n", id.c_str(), fullStr.c_str());
            failed++;
            continue;
        }

        MvmMltProbeResult r{};
        if (mvm_mlt_probe_file(fullStr.c_str(), &r) != 0) {
            std::printf("  FAIL %-22s MLT が読めません: %s\n", id.c_str(), r.error);
            failed++;
            continue;
        }

        std::vector<std::string> problems;

        // expected のフィールドは「無ければ 0 とみなす」のではなく、
        // 「無ければ検証しない」と「必須」を区別する。
        auto expHas = [&](const char* key) { return expected->find(key) != nullptr; };
        auto expInt = [&](const char* key) -> long long {
            auto* v = expected->find(key);
            return v ? v->asInt() : 0;
        };
        auto expStr = [&](const char* key) -> std::string {
            auto* v = expected->find(key);
            return v ? v->asString() : "";
        };
        auto expDouble = [&](const char* key) -> double {
            auto* v = expected->find(key);
            return v ? v->asDouble() : 0.0;
        };
        auto require = [&](const char* key) {
            if (!expHas(key))
                problems.push_back(std::string("expected に必須フィールド '") + key +
                                   "' がありません");
        };

        // kind 別に必須フィールドを定める。欠けていれば失敗にする。
        if (kind == "video") {
            for (const char* k : {"width", "height", "video_codec", "pix_fmt", "fps_num", "fps_den",
                                  "frames", "sar_num", "sar_den", "duration_sec"})
                require(k);
        } else if (kind == "image") {
            for (const char* k :
                 {"width", "height", "video_codec", "pix_fmt", "sar_num", "sar_den"})
                require(k);
        } else if (kind == "audio") {
            for (const char* k : {"audio_codec", "sample_rate", "channels", "duration_sec"})
                require(k);
        }

        if (kind == "video" || kind == "image") {
            if (!r.has_video)
                problems.push_back("映像ストリームがありません");

            if (expHas("width") && r.width != expInt("width"))
                problems.push_back("width " + std::to_string(r.width) +
                                   " != " + std::to_string(expInt("width")));
            if (expHas("height") && r.height != expInt("height"))
                problems.push_back("height " + std::to_string(r.height) +
                                   " != " + std::to_string(expInt("height")));
            if (expHas("video_codec") && r.video_codec != expStr("video_codec"))
                problems.push_back("video_codec " + std::string(r.video_codec) +
                                   " != " + expStr("video_codec"));
            if (expHas("pix_fmt") && r.pix_fmt != expStr("pix_fmt"))
                problems.push_back("pix_fmt " + std::string(r.pix_fmt) +
                                   " != " + expStr("pix_fmt"));

            // SAR は gcd で正規化して比較する
            if (expHas("sar_num") && expHas("sar_den")) {
                Rational got = makeRational(r.sar_num, r.sar_den);
                Rational want = makeRational(expInt("sar_num"), expInt("sar_den"));
                if (!(got == want))
                    problems.push_back("sample_aspect_ratio " + got.str() + " != " + want.str());
            }
        }

        if (kind == "video") {
            // smoke 素材は CFR なので fps と frame count は完全一致を要求する
            if (expHas("fps_num") && expHas("fps_den") &&
                (r.fps_num != expInt("fps_num") || r.fps_den != expInt("fps_den")))
                problems.push_back("fps " + std::to_string(r.fps_num) + "/" +
                                   std::to_string(r.fps_den) +
                                   " != " + std::to_string(expInt("fps_num")) + "/" +
                                   std::to_string(expInt("fps_den")));
            if (expHas("frames") && r.frame_count != expInt("frames"))
                problems.push_back("frame_count " + std::to_string(r.frame_count) +
                                   " != " + std::to_string(expInt("frames")));
        }

        // duration。静止画は length が INT_MAX なので対象外。
        if (kind != "image" && expHas("duration_sec")) {
            double want = expDouble("duration_sec");
            if (r.is_unbounded_length) {
                problems.push_back("尺が無限 (INT_MAX) です。duration を検証できません");
            } else if (std::fabs(r.duration_sec - want) > kDurationToleranceSec) {
                problems.push_back("duration_sec " + std::to_string(r.duration_sec) +
                                   " != " + std::to_string(want) + " (許容差 " +
                                   std::to_string(kDurationToleranceSec) + ")");
            }
        }

        if (kind == "audio" || (expHas("audio_codec") && !expStr("audio_codec").empty())) {
            if (!r.has_audio)
                problems.push_back("音声ストリームがありません");
            if (expHas("audio_codec") && r.audio_codec != expStr("audio_codec"))
                problems.push_back("audio_codec " + std::string(r.audio_codec) +
                                   " != " + expStr("audio_codec"));
            if (expHas("sample_rate") && r.sample_rate != expInt("sample_rate"))
                problems.push_back("sample_rate " + std::to_string(r.sample_rate) +
                                   " != " + std::to_string(expInt("sample_rate")));
            if (expHas("channels") && r.channels != expInt("channels"))
                problems.push_back("channels " + std::to_string(r.channels) +
                                   " != " + std::to_string(expInt("channels")));
        }

        // アルファ。has_alpha の真偽だけでなく、実測した値域まで検証する。
        // pix_fmt が rgba でも中身が全て 255 ならアルファは死んでいる。
        auto* alphaV = expected->find("has_alpha");
        if (alphaV && alphaV->asBool()) {
            if (!r.has_alpha) {
                problems.push_back(
                    "alpha が失われています (alpha_min=" + std::to_string(r.alpha_min) +
                    " alpha_max=" + std::to_string(r.alpha_max) + ")");
            }
            if (expHas("alpha_min_le") && r.alpha_min > expInt("alpha_min_le"))
                problems.push_back("alpha_min " + std::to_string(r.alpha_min) + " > " +
                                   std::to_string(expInt("alpha_min_le")) +
                                   " (透明な画素が見つかりません)");
            if (expHas("alpha_max_ge") && r.alpha_max < expInt("alpha_max_ge"))
                problems.push_back("alpha_max " + std::to_string(r.alpha_max) + " < " +
                                   std::to_string(expInt("alpha_max_ge")) +
                                   " (不透明な画素が見つかりません)");
        }

        if (problems.empty()) {
            std::printf("  OK   %-22s %s\n", id.c_str(), rel.c_str());
        } else {
            std::printf("  FAIL %-22s %s\n", id.c_str(), rel.c_str());
            for (const auto& p : problems)
                std::printf("         %s\n", p.c_str());
            failed++;
        }
    }

    mvm_mlt_runtime_shutdown();

    std::printf("\n検証: %d 件中 %d 件が失敗\n", checked, failed);
    if (checked == 0) {
        logMsg("検証対象が 0 件でした。manifest が空か壊れています。");
        return kExitError;
    }
    return failed == 0 ? kExitOk : kExitMismatch;
}

// --------------------------------------------------------------------------
// explore (S3 調査用)
// --------------------------------------------------------------------------

int cmdExplore(const Args& a) {
    if (a.positional.empty()) {
        logMsg("使い方: mvm_bench explore <video> [audio]");
        return kExitUsage;
    }
    if (!initMlt())
        return kExitError;

    std::string video = a.positional[0];
    std::string audio = a.positional.size() > 1 ? a.positional[1] : "";

    int rc = mvm_mlt_explore(video.c_str(), audio.c_str(), stdout);
    mvm_mlt_runtime_shutdown();
    return rc == 0 ? kExitOk : kExitError;
}

void printUsage() {
    std::fprintf(stderr,
                 "mvm_bench - mvm Phase 0 検証 CLI\n"
                 "\n"
                 "  mvm_bench doctor [--profile <name>] [--expect-width N] [--expect-height N]\n"
                 "                   [--expect-fps-num N] [--expect-fps-den N] [--json <out>]\n"
                 "  mvm_bench probe <path> [--json <out>] [--dump-props]\n"
                 "  mvm_bench decode <path> --frame <n> [--output <png>] [--expect-marker]\n"
                 "  mvm_bench verify-media <manifest.json>\n"
                 "  mvm_bench explore <video> [audio]\n"
                 "\n"
                 "合成と seek / scrub (S5 / S6):\n"
                 "  mvm_bench compose <scenario.json> --frame <n> [--output <png>]\n"
                 "  mvm_bench verify-compose <scenario.json> [--output-dir <dir>]\n"
                 "  mvm_bench seek-bench <scenario.json> [--random N] [--seed S] [--csv <path>]\n"
                 "                       [--check] [--max-p95-ms N] [--max-max-ms N]\n"
                 "  mvm_bench scrub-bench <scenario.json> [--requests N] [--pattern NAME]\n"
                 "                        [--csv <path>] [--check] [--require-coalescing]\n"
                 "    pattern: linear / random / jump / fine\n"
                 "\n"
                 "  --text-service <qtext|dynamictext>  文字描画 service を上書き\n"
                 "  --font-file <path>                  フォントファイルを上書き\n"
                 "\n"
                 "共通オプション:\n"
                 "  --module-dir <dir>   MLT モジュールの場所を上書き\n"
                 "  --data-dir <dir>     MLT データの場所を上書き\n"
                 "\n"
                 "終了コード: 0=成功 1=実行時エラー 2=使い方の誤り 3=検証不一致\n");
}

} // namespace

int main() {
    mvm_enable_utf8_console();

    // main の argv は ANSI であり UTF-8 ではない。日本語パスを正しく受け取るため
    // GetCommandLineW から取り直す (V10 の実測所見)。
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv) {
        std::fprintf(stderr, "コマンドライン引数を取得できませんでした\n");
        return kExitError;
    }

    if (argc < 2) {
        printUsage();
        mvm_win_free_utf8_args(argv, argc);
        return kExitUsage;
    }

    std::string cmd = argv[1];
    Args a = parseArgs(argc, argv, 2);
    applyRuntimeOverrides(a);

    int rc;
    if (cmd == "doctor")
        rc = cmdDoctor(a);
    else if (cmd == "probe")
        rc = cmdProbe(a);
    else if (cmd == "decode")
        rc = cmdDecode(a);
    else if (cmd == "verify-media")
        rc = cmdVerifyMedia(a);
    else if (cmd == "compose")
        rc = cmdCompose(a);
    else if (cmd == "verify-compose")
        rc = cmdVerifyCompose(a);
    else if (cmd == "seek-bench")
        rc = cmdSeekBench(a);
    else if (cmd == "scrub-bench")
        rc = cmdScrubBench(a);
    else if (cmd == "render-audio")
        rc = cmdRenderAudio(a);
    else if (cmd == "verify-audio")
        rc = cmdVerifyAudio(a);
    else if (cmd == "audio-graph-probe")
        rc = cmdAudioGraphProbe(a);
    else if (cmd == "analyze-wav")
        rc = cmdAnalyzeWav(a);
    else if (cmd == "soak")
        rc = cmdSoak(a);
    else if (cmd == "explore")
        rc = cmdExplore(a);
    else {
        std::fprintf(stderr, "不明なサブコマンド: %s\n\n", cmd.c_str());
        printUsage();
        rc = kExitUsage;
    }

    mvm_win_free_utf8_args(argv, argc);
    return rc;
}
