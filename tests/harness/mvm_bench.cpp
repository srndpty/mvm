// mvm Phase 0 / S4 - 検証用 CLI
//
// 位置づけ:
//   Qt に依存しない。MLT のヘッダも include しない (src/media/mlt/ の
//   C API 越しにしか触らない)。V2 の判定に必要な最小限だけを持つ。
//
// サブコマンド:
//   doctor                        MLT ランタイムの健全性検査
//   probe <path> [--json <out>]   MLT と ffprobe の解析結果を比較
//   decode <path> --frame <n> --output <png>
//                                 指定フレームを PNG 化し、マーカーを照合
//   verify-media <manifest>       manifest 記載の全素材を検証
//
// 出力方針:
//   機械可読 JSON は stdout または --json、診断ログは stderr。
//   成功 0 / 検証不一致 3 / 実行時エラー 1 / 使い方の誤り 2。

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

namespace fs = std::filesystem;

namespace {

// 終了コード。CTest がこれで合否を判定する。
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitMismatch = 3;

// duration はコンテナのタイムベース丸めがあるため完全一致を要求できない。
// 1 フレーム分 (60fps で 16.7ms) より十分小さい値を許容差とする。
// これを超える差は「丸め」ではなく解釈の食い違いなので検出したい。
constexpr double kDurationToleranceSec = 0.005;

std::string gModuleDir = MVM_MLT_MODULE_DIR;
std::string gDataDir = MVM_MLT_DATA_DIR;

void logMsg(const std::string& s) {
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

std::string jsonEscape(const std::string& s) {
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

fs::path utf8Path(const std::string& utf8) {
    // std::filesystem::path は Windows では wchar_t を使う。
    // UTF-8 -> wide を明示的に通す (char8_t 経由だと環境差が出るため)。
    wchar_t* w = mvm_utf8_to_wide(utf8.c_str());
    if (!w)
        return fs::path(utf8);
    fs::path p(w);
    mvm_str_free(w);
    return p;
}

std::optional<std::string> readFileUtf8(const std::string& path) {
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

long long gcdLL(long long a, long long b) {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

Rational makeRational(long long n, long long d) {
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
Rational parseRational(const std::string& s) {
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

std::string ffprobePath() {
    return std::string(MVM_FFPROBE_EXE);
}

// コマンドライン引数を CreateProcessW 用に quote する。
// 引数中の " と、その直前の連続する \ をエスケープする必要がある
// (Windows の標準的な引数解析規則)。
std::wstring quoteArg(const std::wstring& arg) {
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

std::wstring toWide(const std::string& utf8) {
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
std::optional<std::string> runFfprobe(const std::string& mediaPath) {
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

FfprobeInfo parseFfprobe(const std::string& json) {
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

MarkerRead readMarker(const unsigned char* rgba, int w, int h) {
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

bool writePng(const std::string& path, const unsigned char* rgba, int w, int h, std::string& err) {
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

Args parseArgs(int argc, char** argv, int from) {
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

void applyRuntimeOverrides(const Args& a) {
    if (a.has("module-dir"))
        gModuleDir = a.get("module-dir");
    if (a.has("data-dir"))
        gDataDir = a.get("data-dir");
}

bool initMlt() {
    if (mvm_mlt_runtime_init(gModuleDir.c_str(), gDataDir.c_str()) != 0) {
        logMsg("MLT の初期化に失敗しました");
        return false;
    }
    return true;
}

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

void printUsage() {
    std::fprintf(stderr,
                 "mvm_bench - mvm Phase 0 検証 CLI\n"
                 "\n"
                 "  mvm_bench doctor [--profile <name>] [--expect-width N] [--expect-height N]\n"
                 "                   [--expect-fps-num N] [--expect-fps-den N] [--json <out>]\n"
                 "  mvm_bench probe <path> [--json <out>] [--dump-props]\n"
                 "  mvm_bench decode <path> --frame <n> [--output <png>] [--expect-marker]\n"
                 "  mvm_bench verify-media <manifest.json>\n"
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
    else {
        std::fprintf(stderr, "不明なサブコマンド: %s\n\n", cmd.c_str());
        printUsage();
        rc = kExitUsage;
    }

    mvm_win_free_utf8_args(argv, argc);
    return rc;
}
