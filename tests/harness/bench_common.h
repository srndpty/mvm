/*
 * mvm Phase 0 - 検証ハーネスの共有部品
 *
 * mvm_bench.cpp (doctor / probe / decode / verify-media) と
 * mvm_bench_compose.cpp (compose / verify-compose / seek-bench / scrub-bench) の
 * 両方から使う。
 *
 * 同じ判断ロジックを 2 箇所に書くと、実装が食い違ったときに
 * 「片方だけ通る」という最悪の形で表面化する。特に JSON 解析と
 * マーカー読み取りは両者で完全に一致している必要がある。
 *
 * ヘッダのみで完結させているのは、これが製品コードではなく
 * 検証ハーネスであり、リンク構成を増やす価値が無いため。
 */

#ifndef MVM_BENCH_COMMON_H
#define MVM_BENCH_COMMON_H

#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <png.h>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace bench {

namespace fs = std::filesystem;

// 終了コード。CTest がこれで合否を判定する。
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitMismatch = 3;

// duration はコンテナのタイムベース丸めがあるため完全一致を要求できない。
// 1 フレーム分 (60fps で 16.7ms) より十分小さい値を許容差とする。
constexpr double kDurationToleranceSec = 0.005;

// MLT のモジュール/データの場所。--module-dir / --data-dir で上書きできる。
inline std::string gModuleDir = MVM_MLT_MODULE_DIR;
inline std::string gDataDir = MVM_MLT_DATA_DIR;

inline void logMsg(const std::string& s) {
    std::fprintf(stderr, "%s\n", s.c_str());
}

// --------------------------------------------------------------------------
// 最小限の JSON 読み取り
// --------------------------------------------------------------------------
// manifest を読むためだけのもの。外部依存を増やさないための割り切りであり、
// 汎用 JSON パーサではない。数値・文字列・真偽値・配列・オブジェクトのみ。

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    JsonArray arr;
    std::shared_ptr<JsonObject> obj;

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object || !obj)
            return nullptr;
        auto it = obj->find(key);
        return it == obj->end() ? nullptr : &it->second;
    }

    std::string asString(const std::string& def = "") const {
        return type == Type::String ? str : def;
    }

    long long asInt(long long def = 0) const {
        return type == Type::Number ? (long long)llround(num) : def;
    }

    double asDouble(double def = 0) const { return type == Type::Number ? num : def; }

    bool asBool(bool def = false) const { return type == Type::Bool ? b : def; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    std::optional<JsonValue> parse() {
        skipWs();
        auto v = parseValue();
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r' ||
                s_[i_] == '\xEF' || s_[i_] == '\xBB' || s_[i_] == '\xBF'))
            i_++;
    }

    std::optional<JsonValue> parseValue() {
        skipWs();
        if (i_ >= s_.size())
            return std::nullopt;

        switch (s_[i_]) {
        case '{':
            return parseObject();
        case '[':
            return parseArray();
        case '"': {
            JsonValue v;
            v.type = JsonValue::Type::String;
            auto str = parseString();
            if (!str)
                return std::nullopt;
            v.str = *str;
            return v;
        }
        case 't':
            if (s_.compare(i_, 4, "true") == 0) {
                i_ += 4;
                JsonValue v;
                v.type = JsonValue::Type::Bool;
                v.b = true;
                return v;
            }
            return std::nullopt;
        case 'f':
            if (s_.compare(i_, 5, "false") == 0) {
                i_ += 5;
                JsonValue v;
                v.type = JsonValue::Type::Bool;
                v.b = false;
                return v;
            }
            return std::nullopt;
        case 'n':
            if (s_.compare(i_, 4, "null") == 0) {
                i_ += 4;
                return JsonValue{};
            }
            return std::nullopt;
        default: {
            size_t start = i_;
            while (i_ < s_.size() &&
                   (std::isdigit((unsigned char)s_[i_]) || s_[i_] == '-' || s_[i_] == '+' ||
                    s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
                i_++;
            if (start == i_)
                return std::nullopt;
            JsonValue v;
            v.type = JsonValue::Type::Number;
            v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
            return v;
        }
        }
    }

    std::optional<std::string> parseString() {
        if (s_[i_] != '"')
            return std::nullopt;
        i_++;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                i_++;
                switch (s_[i_]) {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'u': {
                    // \uXXXX -> UTF-8。日本語パスが manifest に入るため必要。
                    if (i_ + 4 >= s_.size())
                        return std::nullopt;
                    unsigned cp = (unsigned)std::strtoul(s_.substr(i_ + 1, 4).c_str(), nullptr, 16);
                    i_ += 4;
                    // サロゲートペア
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 < s_.size() && s_[i_ + 1] == '\\' &&
                        s_[i_ + 2] == 'u') {
                        unsigned lo =
                            (unsigned)std::strtoul(s_.substr(i_ + 3, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i_ += 6;
                        }
                    }
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xF0 | (cp >> 18));
                        out += (char)(0x80 | ((cp >> 12) & 0x3F));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    out += s_[i_];
                }
                i_++;
            } else {
                out += s_[i_++];
            }
        }
        if (i_ >= s_.size())
            return std::nullopt;
        i_++; // closing quote
        return out;
    }

    std::optional<JsonValue> parseObject() {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.obj = std::make_shared<JsonObject>();
        i_++; // {
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') {
            i_++;
            return v;
        }
        while (i_ < s_.size()) {
            skipWs();
            auto key = parseString();
            if (!key)
                return std::nullopt;
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':')
                return std::nullopt;
            i_++;
            auto val = parseValue();
            if (!val)
                return std::nullopt;
            (*v.obj)[*key] = *val;
            skipWs();
            if (i_ < s_.size() && s_[i_] == ',') {
                i_++;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                i_++;
                return v;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseArray() {
        JsonValue v;
        v.type = JsonValue::Type::Array;
        i_++; // [
        skipWs();
        if (i_ < s_.size() && s_[i_] == ']') {
            i_++;
            return v;
        }
        while (i_ < s_.size()) {
            auto val = parseValue();
            if (!val)
                return std::nullopt;
            v.arr.push_back(*val);
            skipWs();
            if (i_ < s_.size() && s_[i_] == ',') {
                i_++;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                i_++;
                return v;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
};

// --------------------------------------------------------------------------
// JSON 出力
// --------------------------------------------------------------------------

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

// --------------------------------------------------------------------------
// ファイル入出力 (UTF-8 パス対応)
// --------------------------------------------------------------------------

inline fs::path utf8Path(const std::string& utf8) {
    // std::filesystem::path は Windows では wchar_t を使う。
    // UTF-8 -> wide を明示的に通す (char8_t 経由だと環境差が出るため)。
    wchar_t* w = mvm_utf8_to_wide(utf8.c_str());
    if (!w)
        return fs::path(utf8);
    fs::path p(w);
    mvm_str_free(w);
    return p;
}

inline std::optional<std::string> readFileUtf8(const std::string& path) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f)
        return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --------------------------------------------------------------------------
// 有理数 (SAR / fps)
// --------------------------------------------------------------------------
// SAR は "1:1" と "2:2" のように等価な表現があるため、文字列比較してはいけない。
// gcd で正規化してから比較する。

struct Rational {
    long long num = 0;
    long long den = 0;
    bool valid = false;

    std::string str() const {
        if (!valid)
            return "(未指定)";
        return std::to_string(num) + "/" + std::to_string(den);
    }

    bool operator==(const Rational& o) const {
        return valid == o.valid && (!valid || (num == o.num && den == o.den));
    }
};

inline long long gcdLL(long long a, long long b) {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

inline Rational makeRational(long long n, long long d) {
    Rational r;
    // 0/0 や N/0 は「未指定」として扱う。ffprobe は SAR 不明時に
    // 0:1 を返すことがあるので、それも未指定とする。
    if (d == 0 || n == 0)
        return r;
    long long g = gcdLL(n, d);
    if (g == 0)
        return r;
    r.num = n / g;
    r.den = d / g;
    if (r.den < 0) {
        r.num = -r.num;
        r.den = -r.den;
    }
    r.valid = true;
    return r;
}

// ffprobe の "N:M" (SAR) / "N/M" (frame rate) の両方を受ける
inline Rational parseRational(const std::string& s) {
    if (s.empty() || s == "N/A")
        return Rational{};
    size_t sep = s.find_first_of(":/");
    if (sep == std::string::npos)
        return Rational{};
    long long n = std::strtoll(s.substr(0, sep).c_str(), nullptr, 10);
    long long d = std::strtoll(s.substr(sep + 1).c_str(), nullptr, 10);
    return makeRational(n, d);
}

// --------------------------------------------------------------------------
// ffprobe 実行
// --------------------------------------------------------------------------
// ホストの C:\tools や winget 版ではなく、UCRT64 版を必ず使う。
// mvm がリンクする libav* と同じビルドでないと比較の意味がない。

inline std::string ffprobePath() {
    return std::string(MVM_FFPROBE_EXE);
}

// コマンドライン引数を CreateProcessW 用に quote する。
// 引数中の " と、その直前の連続する \ をエスケープする必要がある
// (Windows の標準的な引数解析規則)。
inline std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;

    std::wstring out = L"\"";
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out += arg[i];
    }
    out += L'"';
    return out;
}

inline std::wstring toWide(const std::string& utf8) {
    wchar_t* w = mvm_utf8_to_wide(utf8.c_str());
    std::wstring out = w ? w : L"";
    mvm_str_free(w);
    return out;
}

// ffprobe を直接起動する。
//
// cmd.exe は経由しない。cmd.exe を挟むと、リダイレクトの記法や
// ^ & | といった文字の解釈がもう一段入り、日本語や記号を含むパスで
// 壊れ方が環境依存になる。stdout/stderr は一時ファイルのハンドルを
// 直接渡して受け取る。
inline std::optional<std::string> runFfprobe(const std::string& mediaPath) {
    static std::atomic<unsigned> counter{0};
    fs::path tmp =
        fs::temp_directory_path() / ("mvm_ffprobe_" + std::to_string(GetCurrentProcessId()) + "_" +
                                     std::to_string(counter++) + ".json");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // 子プロセスへ継承させる

    HANDLE hOut = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
        return std::nullopt;

    // 引数は自前で quote する。ffprobe 実行ファイル名も含めて 1 本の
    // コマンドラインに組む (lpApplicationName は別途フルパスで渡す)。
    std::wstring exe = toWide(ffprobePath());
    std::wstring cmdline = quoteArg(exe);
    for (const wchar_t* opt : {L"-hide_banner", L"-loglevel", L"error", L"-print_format", L"json",
                               L"-show_format", L"-show_streams", L"--"}) {
        cmdline += L' ';
        cmdline += opt;
    }
    cmdline += L' ';
    cmdline += quoteArg(toWide(mediaPath));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOut;
    si.hStdError = hOut; // 診断も同じ先へ。ffprobe は error 以外を出さない設定
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    BOOL started =
        CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr,
                       /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);

    if (!started) {
        std::error_code ec;
        fs::remove(tmp, ec);
        logMsg("ffprobe を起動できません: " + ffprobePath());
        return std::nullopt;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::ifstream f(tmp, std::ios::binary);
    if (!f) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    f.close();
    std::error_code ec;
    fs::remove(tmp, ec);

    if (code != 0)
        return std::nullopt;
    return ss.str();
}

struct FfprobeInfo {
    bool ok = false;
    bool hasVideo = false;
    bool hasAudio = false;
    std::string videoCodec, audioCodec, pixFmt, container, sar;
    long long width = 0, height = 0;
    long long fpsNum = 0, fpsDen = 0;
    long long frameCount = 0;
    double duration = 0;
    long long sampleRate = 0, channels = 0;
};

inline FfprobeInfo parseFfprobe(const std::string& json) {
    FfprobeInfo info;
    JsonParser parser(json);
    auto root = parser.parse();
    if (!root)
        return info;

    if (auto* fmt = root->find("format")) {
        info.container = fmt->find("format_name") ? fmt->find("format_name")->asString() : "";
        if (auto* d = fmt->find("duration"))
            info.duration = std::strtod(d->asString("0").c_str(), nullptr);
    }

    auto* streams = root->find("streams");
    if (!streams || streams->type != JsonValue::Type::Array)
        return info;

    for (const auto& st : streams->arr) {
        auto* t = st.find("codec_type");
        if (!t)
            continue;
        std::string type = t->asString();
        if (type == "video" && !info.hasVideo) {
            info.hasVideo = true;
            if (auto* v = st.find("codec_name"))
                info.videoCodec = v->asString();
            if (auto* v = st.find("pix_fmt"))
                info.pixFmt = v->asString();
            if (auto* v = st.find("width"))
                info.width = v->asInt();
            if (auto* v = st.find("height"))
                info.height = v->asInt();
            if (auto* v = st.find("sample_aspect_ratio"))
                info.sar = v->asString();
            if (auto* v = st.find("r_frame_rate")) {
                std::string r = v->asString("0/0");
                size_t slash = r.find('/');
                if (slash != std::string::npos) {
                    info.fpsNum = std::strtoll(r.substr(0, slash).c_str(), nullptr, 10);
                    info.fpsDen = std::strtoll(r.substr(slash + 1).c_str(), nullptr, 10);
                }
            }
            if (auto* v = st.find("nb_frames"))
                info.frameCount = std::strtoll(v->asString("0").c_str(), nullptr, 10);
        } else if (type == "audio" && !info.hasAudio) {
            info.hasAudio = true;
            if (auto* v = st.find("codec_name"))
                info.audioCodec = v->asString();
            if (auto* v = st.find("sample_rate"))
                info.sampleRate = std::strtoll(v->asString("0").c_str(), nullptr, 10);
            if (auto* v = st.find("channels"))
                info.channels = v->asInt();
        }
    }
    info.ok = true;
    return info;
}

// --------------------------------------------------------------------------
// フレーム固有マーカーの読み取り
// --------------------------------------------------------------------------
// 仕様: docs/research/test-media-format.md
//   セル幅 64px、帯 (0,0)-(1216,64)。
//   cell0=白(同期) cell1=黒(同期) cell2..17=16bit LSB first cell18=白(同期)
// OCR は使わない。セル中心の輝度を閾値判定するだけ。

constexpr int kCellSize = 64;
constexpr int kCellCount = 19;
constexpr int kDataCells = 16;

struct MarkerRead {
    bool syncOk = false;
    long long value = -1;
    int lumaMin = 255, lumaMax = 0;
};

inline MarkerRead readMarker(const unsigned char* rgba, int w, int h) {
    MarkerRead r;
    if (!rgba || w < kCellSize * kCellCount || h < kCellSize)
        return r;

    auto cellLuma = [&](int cell) -> int {
        // セル中心の 8x8 を平均する。圧縮ノイズに対する余裕を持たせる。
        const int cx = cell * kCellSize + kCellSize / 2;
        const int cy = kCellSize / 2;
        long sum = 0;
        int n = 0;
        for (int y = cy - 4; y < cy + 4; y++) {
            for (int x = cx - 4; x < cx + 4; x++) {
                const size_t offset =
                    ((size_t)(unsigned)y * (size_t)(unsigned)w + (size_t)(unsigned)x) * 4u;
                const unsigned char* px = rgba + offset;
                // BT.601 luma
                sum += (long)(0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]);
                n++;
            }
        }
        return n ? (int)(sum / n) : 0;
    };

    std::vector<int> luma((size_t)kCellCount);
    for (int c = 0; c < kCellCount; c++) {
        luma[(size_t)c] = cellLuma(c);
        r.lumaMin = std::min(r.lumaMin, luma[(size_t)c]);
        r.lumaMax = std::max(r.lumaMax, luma[(size_t)c]);
    }

    // 同期セルで閾値の妥当性を確認する。
    // ここが崩れていれば読み取り位置がずれているか、素材が別物である。
    const int threshold = 128;
    bool sync = (luma[0] > threshold) && (luma[1] < threshold) &&
                (luma[(size_t)kCellCount - 1] > threshold);
    r.syncOk = sync;
    if (!sync)
        return r;

    long long value = 0;
    for (int b = 0; b < kDataCells; b++) {
        if (luma[(size_t)(2 + b)] > threshold)
            value |= (1LL << b);
    }
    r.value = value;
    return r;
}

// --------------------------------------------------------------------------
// PNG 出力
// --------------------------------------------------------------------------

inline bool writePng(const std::string& path, const unsigned char* rgba, int w, int h,
                     std::string& err) {
    fs::path p = utf8Path(path);
    FILE* fp = _wfopen(p.c_str(), L"wb");
    if (!fp) {
        err = "出力ファイルを開けません: " + path;
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        std::fclose(fp);
        err = "png_create_write_struct に失敗";
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(fp);
        err = "png_create_info_struct に失敗";
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        err = "PNG の書き込みに失敗";
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows((size_t)h);
    for (int y = 0; y < h; y++)
        rows[(size_t)y] = (png_bytep)(rgba + (size_t)y * (size_t)w * 4);

    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

// --------------------------------------------------------------------------
// 引数
// --------------------------------------------------------------------------

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    bool has(const std::string& k) const { return options.count(k) > 0; }

    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
};

inline Args parseArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; i++) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                a.options[key] = argv[++i];
            } else {
                a.options[key] = "1";
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

inline void applyRuntimeOverrides(const Args& a) {
    if (a.has("module-dir"))
        gModuleDir = a.get("module-dir");
    if (a.has("data-dir"))
        gDataDir = a.get("data-dir");
}

inline bool initMlt() {
    if (mvm_mlt_runtime_init(gModuleDir.c_str(), gDataDir.c_str()) != 0) {
        logMsg("MLT の初期化に失敗しました");
        return false;
    }
    return true;
}

} // namespace bench

#endif // MVM_BENCH_COMMON_H
