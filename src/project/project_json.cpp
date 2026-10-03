#include "project/project_json.h"

#include "project/media_bin.h"
#include "project/path_identity.h"
#include "project/timeline_edit.h"
#include "util/mvm_atomic_write.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace mvm::project {
namespace {

constexpr int kSchemaVersion = kProjectSchemaVersion;
// .mvm ファイルであることを識別する marker。拡張子だけを根拠にしない。
constexpr char kFormatMarker[] = "mvm-project";

std::string unsupportedSchemaMessage(int schemaVersion) {
    return "対応していない schema_version です: " + std::to_string(schemaVersion) + "。schema " +
           std::to_string(kSchemaVersion) + " の .mvm を開いてください";
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(text.c_str()));
}

std::string escapeJson(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char rawCharacter : value) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[(character >> 4) & 0x0f];
                result += hex[character & 0x0f];
            } else {
                result += static_cast<char>(character);
            }
        }
    }
    return result;
}

bool isSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isdigit(character) || (character >= 'a' && character <= 'f');
           });
}

bool isProjectManagedRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute())
        return false;
    const auto normalized = path.lexically_normal();
    return normalized != L"." && normalized.begin() != normalized.end() &&
           *normalized.begin() != L"..";
}

// project directory 配下なら relative、そうでなければ absolute で保存する。
// script_path と timeline clip の media_path は同じ規則で扱う。
std::string persistedSourcePath(const std::filesystem::path& path,
                                const std::filesystem::path& projectDirectory) {
    const auto absolutePath =
        (path.is_absolute() ? path : projectDirectory / path).lexically_normal();
    const auto relative = absolutePath.lexically_relative(projectDirectory);
    return relative.empty() ? pathToUtf8(absolutePath) : pathToUtf8(relative);
}

// 読み込み時に relative を project directory へ再アンカーする。
std::filesystem::path resolveSourcePath(const std::filesystem::path& path,
                                        const std::filesystem::path& projectDirectory) {
    return path.is_absolute() ? path.lexically_normal()
                              : (projectDirectory / path).lexically_normal();
}

bool persistedScriptPath(const std::filesystem::path& path,
                         const std::filesystem::path& projectDirectory, std::string& result,
                         std::string& error) {
    if (path.empty()) {
        error = "Manim script_path が空です";
        return false;
    }
    result = persistedSourcePath(path, projectDirectory);
    return true;
}

bool persistedGeneratedPath(const std::filesystem::path& path,
                            const std::filesystem::path& projectDirectory, std::string& result,
                            std::string& error) {
    if (path.empty()) {
        result.clear();
        return true;
    }
    const auto absolutePath =
        (path.is_absolute() ? path : projectDirectory / path).lexically_normal();
    const auto relative = absolutePath.lexically_relative(projectDirectory);
    if (!isProjectManagedRelativePath(relative)) {
        error = "generated_video_path は project directory 配下である必要があります: " +
                pathToUtf8(absolutePath);
        return false;
    }
    result = pathToUtf8(relative);
    return true;
}

void appendUtf8(std::string& output, unsigned codePoint) {
    if (codePoint < 0x80) {
        output += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        output += static_cast<char>(0xc0 | (codePoint >> 6));
        output += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else if (codePoint < 0x10000) {
        output += static_cast<char>(0xe0 | (codePoint >> 12));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else {
        output += static_cast<char>(0xf0 | (codePoint >> 18));
        output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codePoint & 0x3f));
    }
}

class ProjectJsonParser {
public:
    explicit ProjectJsonParser(const std::string& text) : text_(text) {}

    bool parse(Project& project, std::string& error) {
        bool hasSchema = false;
        bool hasFormat = false;
        bool hasTimelineFpsNum = false;
        bool hasTimelineFpsDen = false;
        bool hasOutputWidth = false;
        bool hasOutputHeight = false;
        bool hasVideoTracks = false;
        bool hasAudioTracks = false;
        bool hasAssets = false;
        bool hasClips = false;
        bool hasTransitions = false;
        bool hasMarkers = false;
        bool hasIn = false;
        bool hasOut = false;
        bool hasMediaFolders = false;
        bool hasMediaItems = false;
        bool hasSubtitles = false;
        std::string format;
        if (!consume('{'))
            return finish(error);
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return finish(error);
                if (key == "schema_version") {
                    if (hasSchema || !parseInteger(project.schemaVersion))
                        return failAndFinish("schema_version が重複または不正です", error);
                    hasSchema = true;
                    // 版が違うファイルは後続の field の形も違うので、ここで止める。
                    // 止めないと「必須 field がありません」など原因の分からないエラーになる。
                    if (project.schemaVersion != kSchemaVersion)
                        return failAndFinish(unsupportedSchemaMessage(project.schemaVersion),
                                             error);
                } else if (key == "format") {
                    if (hasFormat || !parseString(format))
                        return failAndFinish("format が重複または不正です", error);
                    hasFormat = true;
                } else if (key == "video_tracks") {
                    if (hasVideoTracks || !parseTracks(project.videoTracks, TrackKind::Video))
                        return failAndFinish("video_tracks が重複または不正です", error);
                    hasVideoTracks = true;
                } else if (key == "audio_tracks") {
                    if (hasAudioTracks || !parseTracks(project.audioTracks, TrackKind::Audio))
                        return failAndFinish("audio_tracks が重複または不正です", error);
                    hasAudioTracks = true;
                } else if (key == "timeline_fps_num") {
                    if (hasTimelineFpsNum || !parseInteger64(project.timelineFpsNum))
                        return failAndFinish("timeline_fps_num が重複または不正です", error);
                    hasTimelineFpsNum = true;
                } else if (key == "timeline_fps_den") {
                    if (hasTimelineFpsDen || !parseInteger64(project.timelineFpsDen))
                        return failAndFinish("timeline_fps_den が重複または不正です", error);
                    hasTimelineFpsDen = true;
                } else if (key == "output_width") {
                    if (hasOutputWidth || !parseInteger(project.outputWidth))
                        return failAndFinish("output_width が重複または不正です", error);
                    hasOutputWidth = true;
                } else if (key == "output_height") {
                    if (hasOutputHeight || !parseInteger(project.outputHeight))
                        return failAndFinish("output_height が重複または不正です", error);
                    hasOutputHeight = true;
                } else if (key == "manim_assets") {
                    if (hasAssets || !parseAssets(project.manimAssets))
                        return failAndFinish("manim_assets が重複または不正です", error);
                    hasAssets = true;
                } else if (key == "timeline_clips") {
                    if (hasClips || !parseTimelineClips(project.timelineClips))
                        return failAndFinish("timeline_clips が重複または不正です", error);
                    hasClips = true;
                } else if (key == "timeline_transitions") {
                    if (hasTransitions || !parseTimelineTransitions(project.timelineTransitions))
                        return failAndFinish("timeline_transitions が重複または不正です", error);
                    hasTransitions = true;
                } else if (key == "timeline_markers") {
                    if (hasMarkers || !parseFrameList(project.timelineMarkers))
                        return failAndFinish("timeline_markers が重複または不正です", error);
                    hasMarkers = true;
                } else if (key == "in_frame") {
                    if (hasIn || !parseOptionalFrame(project.inFrame))
                        return failAndFinish("in_frame が重複または不正です", error);
                    hasIn = true;
                } else if (key == "out_frame") {
                    if (hasOut || !parseOptionalFrame(project.outFrame))
                        return failAndFinish("out_frame が重複または不正です", error);
                    hasOut = true;
                } else if (key == "media_folders") {
                    if (hasMediaFolders || !parseMediaFolders(project.mediaFolders))
                        return failAndFinish("media_folders が重複または不正です", error);
                    hasMediaFolders = true;
                } else if (key == "media_items") {
                    if (hasMediaItems || !parseMediaItems(project.mediaItems))
                        return failAndFinish("media_items が重複または不正です", error);
                    hasMediaItems = true;
                } else if (key == "subtitles") {
                    if (hasSubtitles || !parseSubtitles(project.subtitles))
                        return failAndFinish("字幕データが重複または不正です", error);
                    hasSubtitles = true;
                } else if (!skipValue()) {
                    return finish(error);
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return finish(error);
        skipWhitespace();
        if (position_ != text_.size())
            return failAndFinish("Project JSON の末尾に余分な値があります", error);
        if (!hasSchema)
            return failAndFinish("schema_version がありません", error);
        if (project.schemaVersion != kSchemaVersion)
            return failAndFinish(unsupportedSchemaMessage(project.schemaVersion), error);
        if (!hasFormat || format != kFormatMarker)
            return failAndFinish("mvm project ファイルではありません (format marker 不一致)",
                                 error);
        if (!hasTimelineFpsNum || !hasTimelineFpsDen || !hasVideoTracks || !hasAudioTracks ||
            !hasAssets || !hasClips || !hasTransitions || !hasMarkers || !hasIn || !hasOut ||
            !hasMediaFolders || !hasMediaItems) {
            return failAndFinish("Project schema " + std::to_string(kSchemaVersion) +
                                     " の必須 field がありません",
                                 error);
        }
        const auto timeline = validateTimeline(project);
        if (!timeline.success)
            return failAndFinish(timeline.error, error);
        error.clear();
        return true;
    }

private:
    const std::string& text_;
    std::size_t position_ = 0;
    std::string error_;

    bool finish(std::string& error) {
        if (error_.empty())
            error_ = "Project JSON を解析できません";
        error = error_;
        return false;
    }

    bool failAndFinish(const std::string& message, std::string& error) {
        fail(message);
        return finish(error);
    }

    bool fail(const std::string& message) {
        if (error_.empty())
            error_ = message;
        return false;
    }

    void skipWhitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    bool peek(char character) {
        skipWhitespace();
        return position_ < text_.size() && text_[position_] == character;
    }

    bool consume(char character) {
        skipWhitespace();
        if (position_ >= text_.size() || text_[position_] != character)
            return fail(std::string("Project JSON に '") + character + "' が必要です");
        ++position_;
        return true;
    }

    bool consumeIf(char character) {
        skipWhitespace();
        if (position_ >= text_.size() || text_[position_] != character)
            return false;
        ++position_;
        return true;
    }

    int hexDigit(char character) {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    }

    bool parseHex4(unsigned& value) {
        value = 0;
        for (int index = 0; index < 4; ++index) {
            if (position_ >= text_.size())
                return fail("JSON の Unicode escape が途中で終わっています");
            const int digit = hexDigit(text_[position_++]);
            if (digit < 0)
                return fail("JSON の Unicode escape が不正です");
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return true;
    }

    bool parseString(std::string& output) {
        output.clear();
        if (!consume('"'))
            return false;
        while (position_ < text_.size()) {
            const unsigned char character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"')
                return true;
            if (character < 0x20)
                return fail("JSON string に制御文字が含まれています");
            if (character != '\\') {
                output += static_cast<char>(character);
                continue;
            }
            if (position_ >= text_.size())
                return fail("JSON string の escape が途中で終わっています");
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                output += escaped;
                break;
            case 'b':
                output += '\b';
                break;
            case 'f':
                output += '\f';
                break;
            case 'n':
                output += '\n';
                break;
            case 'r':
                output += '\r';
                break;
            case 't':
                output += '\t';
                break;
            case 'u': {
                unsigned codePoint = 0;
                if (!parseHex4(codePoint))
                    return false;
                if (codePoint >= 0xd800 && codePoint <= 0xdbff && position_ + 6 <= text_.size() &&
                    text_[position_] == '\\' && text_[position_ + 1] == 'u') {
                    position_ += 2;
                    unsigned low = 0;
                    if (!parseHex4(low) || low < 0xdc00 || low > 0xdfff)
                        return fail("JSON の surrogate pair が不正です");
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                }
                appendUtf8(output, codePoint);
                break;
            }
            default:
                return fail("JSON string の escape が不正です");
            }
        }
        return fail("JSON string が閉じられていません");
    }

    bool parseInteger(int& value) {
        skipWhitespace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-')
            ++position_;
        if (position_ >= text_.size() ||
            !std::isdigit(static_cast<unsigned char>(text_[position_])))
            return fail("JSON integer が不正です");
        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        try {
            value = std::stoi(text_.substr(start, position_ - start));
        } catch (...) {
            return fail("JSON integer が範囲外です");
        }
        return true;
    }

    bool parseInteger64(std::int64_t& value) {
        skipWhitespace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-')
            ++position_;
        if (position_ >= text_.size() ||
            !std::isdigit(static_cast<unsigned char>(text_[position_])))
            return fail("JSON integer が不正です");
        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_])))
            ++position_;
        try {
            value = std::stoll(text_.substr(start, position_ - start));
        } catch (...) {
            return fail("JSON integer が範囲外です");
        }
        return true;
    }

    bool parseOptionalFrame(std::optional<std::int64_t>& frame) {
        skipWhitespace();
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            frame.reset();
            return true;
        }
        std::int64_t value = 0;
        if (!parseInteger64(value))
            return false;
        frame = value;
        return true;
    }

    bool parseFrameList(std::vector<std::int64_t>& frames) {
        if (!consume('['))
            return false;
        if (consumeIf(']'))
            return true;
        while (true) {
            std::int64_t frame = 0;
            if (!parseInteger64(frame))
                return false;
            frames.push_back(frame);
            if (consumeIf(']'))
                return true;
            if (!consume(','))
                return false;
        }
    }

    bool parseNumber(double& value) {
        skipWhitespace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-')
            ++position_;
        if (position_ >= text_.size() ||
            !std::isdigit(static_cast<unsigned char>(text_[position_])))
            return fail("JSON number が不正です");
        if (text_[position_] == '0') {
            ++position_;
        } else {
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const std::size_t fractionStart = position_;
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
            if (fractionStart == position_)
                return fail("JSON number の小数部が不正です");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            const std::size_t exponentStart = position_;
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
            if (exponentStart == position_)
                return fail("JSON number の指数部が不正です");
        }
        try {
            value = std::stod(text_.substr(start, position_ - start));
        } catch (...) {
            return fail("JSON number が範囲外です");
        }
        return std::isfinite(value) || fail("JSON number が有限値ではありません");
    }

    bool parseState(const std::string& text, ManimGenerationState& state) {
        if (text == "NotGenerated")
            state = ManimGenerationState::NotGenerated;
        else if (text == "Ready")
            state = ManimGenerationState::Ready;
        else if (text == "SourceChanged")
            state = ManimGenerationState::SourceChanged;
        else if (text == "GenerationFailed")
            state = ManimGenerationState::GenerationFailed;
        else
            return fail("未知の Manim generation_state です: " + text);
        return true;
    }

    bool parseAsset(ManimAsset& asset) {
        bool hasScript = false;
        bool hasScene = false;
        bool hasVideo = false;
        bool hasState = false;
        bool hasFingerprint = false;
        std::string script;
        std::string video;
        std::string state;

        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                if (key == "script_path") {
                    if (hasScript || !parseString(script))
                        return fail("script_path が重複または不正です");
                    hasScript = true;
                } else if (key == "scene_name") {
                    if (hasScene || !parseString(asset.sceneName))
                        return fail("scene_name が重複または不正です");
                    hasScene = true;
                } else if (key == "generated_video_path") {
                    if (hasVideo || !parseString(video))
                        return fail("generated_video_path が重複または不正です");
                    hasVideo = true;
                } else if (key == "generation_state") {
                    if (hasState || !parseString(state))
                        return fail("generation_state が重複または不正です");
                    hasState = true;
                } else if (key == "source_fingerprint") {
                    if (hasFingerprint || !parseString(asset.sourceFingerprint))
                        return fail("source_fingerprint が重複または不正です");
                    hasFingerprint = true;
                } else if (!skipValue()) {
                    return false;
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        if (!hasScript || !hasScene || !hasVideo || !hasState || !hasFingerprint)
            return fail("Manim asset の必須 field がありません");
        if (script.empty() || asset.sceneName.empty())
            return fail("Manim asset の script_path または scene_name が空です");
        if (!parseState(state, asset.generationState))
            return false;
        asset.scriptPath = pathFromUtf8(script);
        asset.generatedVideoPath = pathFromUtf8(video);
        return true;
    }

    bool parseAssets(std::vector<ManimAsset>& assets) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (consumeIf(']'))
            return true;
        while (true) {
            ManimAsset asset;
            if (!parseAsset(asset))
                return false;
            assets.push_back(std::move(asset));
            skipWhitespace();
            if (consumeIf(','))
                continue;
            break;
        }
        return consume(']');
    }

    bool parseBool(bool& value) {
        skipWhitespace();
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            value = true;
            return true;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            value = false;
            return true;
        }
        return fail("JSON boolean が不正です");
    }

    bool parseTrack(Track& track, TrackKind kind) {
        bool hasName = false;
        bool hasMuted = false;
        bool hasSolo = false;
        bool hasMixerName = false, hasGain = false, hasPan = false;
        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                if (key == "name") {
                    if (hasName || !parseString(track.name))
                        return fail("track の name が重複または不正です");
                    hasName = true;
                } else if (key == "muted") {
                    if (hasMuted || !parseBool(track.muted))
                        return fail("track の muted が重複または不正です");
                    hasMuted = true;
                } else if (key == "solo") {
                    if (hasSolo || !parseBool(track.solo))
                        return fail("track の solo が重複または不正です");
                    hasSolo = true;
                } else if (key == "mixer_name") {
                    if (hasMixerName || !parseString(track.mixerName))
                        return fail("ミキサー名が重複または不正です");
                    hasMixerName = true;
                } else if (key == "mixer_gain_db") {
                    if (hasGain || !parseNumber(track.mixerGainDb))
                        return fail("トラック音量が重複または不正です");
                    hasGain = true;
                } else if (key == "mixer_pan") {
                    if (hasPan || !parseNumber(track.mixerPan))
                        return fail("トラックパンが重複または不正です");
                    hasPan = true;
                } else if (!skipValue()) {
                    return false;
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        if (!hasName || !hasMuted || !hasSolo)
            return fail("track の必須 field がありません");
        if (kind == TrackKind::Video && track.solo)
            return fail("video track は solo を持てません");
        if (!isValidAudioMix(track.mixerGainDb, track.mixerPan))
            return fail("トラック音量またはパンが範囲外です");
        if (track.name.empty())
            return fail("track の name が空です");
        return true;
    }

    bool parseTracks(std::vector<Track>& tracks, TrackKind kind) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (consumeIf(']'))
            return true;
        while (true) {
            Track track;
            if (!parseTrack(track, kind))
                return false;
            tracks.push_back(std::move(track));
            skipWhitespace();
            if (consumeIf(','))
                continue;
            break;
        }
        return consume(']');
    }

    bool parseTrackKind(const std::string& text, TrackKind& kind) {
        if (text == "video")
            kind = TrackKind::Video;
        else if (text == "audio")
            kind = TrackKind::Audio;
        else
            return fail("未知の track kind です: " + text);
        return true;
    }

    bool parseClipKind(const std::string& text, TimelineClipKind& kind) {
        if (text == "video")
            kind = TimelineClipKind::Video;
        else if (text == "manim")
            kind = TimelineClipKind::Manim;
        else if (text == "audio")
            kind = TimelineClipKind::Audio;
        else if (text == "text")
            kind = TimelineClipKind::Text;
        else if (text == "image")
            kind = TimelineClipKind::Image;
        else
            return fail("未知の timeline clip kind です: " + text);
        return true;
    }

    bool parseClipEffects(ClipEffects& effects) {
        bool seen[24] = {};
        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                int field = -1;
                if (key == "position_x_percent")
                    field = 0;
                else if (key == "position_y_percent")
                    field = 1;
                else if (key == "scale_x_percent")
                    field = 2;
                else if (key == "scale_y_percent")
                    field = 14;
                else if (key == "rotation_degrees")
                    field = 3;
                else if (key == "opacity_percent")
                    field = 4;
                else if (key == "crop_left_percent")
                    field = 5;
                else if (key == "crop_top_percent")
                    field = 6;
                else if (key == "crop_right_percent")
                    field = 7;
                else if (key == "crop_bottom_percent")
                    field = 8;
                else if (key == "fade_in_frames")
                    field = 9;
                else if (key == "fade_out_frames")
                    field = 10;
                else if (key == "volume_percent")
                    field = 11;
                else if (key == "opacity_keys")
                    field = 12;
                else if (key == "volume_keys")
                    field = 13;
                else if (key == "position_x_keys")
                    field = 15;
                else if (key == "position_y_keys")
                    field = 16;
                else if (key == "scale_x_keys")
                    field = 17;
                else if (key == "scale_y_keys")
                    field = 18;
                else if (key == "rotation_keys")
                    field = 19;
                else if (key == "crop_left_keys")
                    field = 20;
                else if (key == "crop_top_keys")
                    field = 21;
                else if (key == "crop_right_keys")
                    field = 22;
                else if (key == "crop_bottom_keys")
                    field = 23;

                else
                    return fail("effects に未知の field があります: " + key);
                if (seen[field])
                    return fail("effects field が重複しています: " + key);
                seen[field] = true;
                if (field == 0 && !parseNumber(effects.positionXPercent))
                    return false;
                if (field == 1 && !parseNumber(effects.positionYPercent))
                    return false;
                if (field == 2 && !parseNumber(effects.scaleXPercent))
                    return false;
                if (field == 14 && !parseNumber(effects.scaleYPercent))
                    return false;
                if (field == 3 && !parseNumber(effects.rotationDegrees))
                    return false;
                if (field == 4 && !parseNumber(effects.opacityPercent))
                    return false;
                if (field == 5 && !parseNumber(effects.cropLeftPercent))
                    return false;
                if (field == 6 && !parseNumber(effects.cropTopPercent))
                    return false;
                if (field == 7 && !parseNumber(effects.cropRightPercent))
                    return false;
                if (field == 8 && !parseNumber(effects.cropBottomPercent))
                    return false;
                if (field == 9 && !parseInteger64(effects.fadeInFrames))
                    return false;
                if (field == 10 && !parseInteger64(effects.fadeOutFrames))
                    return false;
                if (field == 11 && !parseNumber(effects.volumePercent))
                    return false;
                if (field == 12 && !parseClipKeys(effects.opacityKeys))
                    return false;
                if (field == 13 && !parseClipKeys(effects.volumeKeys))
                    return false;
                if (field == 15 && !parseClipKeys(effects.positionXKeys))
                    return false;
                if (field == 16 && !parseClipKeys(effects.positionYKeys))
                    return false;
                if (field == 17 && !parseClipKeys(effects.scaleXKeys))
                    return false;
                if (field == 18 && !parseClipKeys(effects.scaleYKeys))
                    return false;
                if (field == 19 && !parseClipKeys(effects.rotationKeys))
                    return false;
                if (field == 20 && !parseClipKeys(effects.cropLeftKeys))
                    return false;
                if (field == 21 && !parseClipKeys(effects.cropTopKeys))
                    return false;
                if (field == 22 && !parseClipKeys(effects.cropRightKeys))
                    return false;
                if (field == 23 && !parseClipKeys(effects.cropBottomKeys))
                    return false;

                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        for (bool present : seen) {
            if (!present)
                return fail("effects object の固定fieldが不足しています");
        }
        return true;
    }

    bool parseClipKeys(std::vector<ClipKeyframe>& keys) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (!peek(']')) {
            while (true) {
                if (!consume('{'))
                    return false;
                bool hasFrame = false;
                bool hasValue = false;
                bool hasInterpolation = false, hasStart = false, hasEnd = false;
                bool hasControl1 = false, hasControl2 = false;
                ClipKeyframe keyframe;
                while (true) {
                    std::string field;
                    if (!parseString(field) || !consume(':'))
                        return false;
                    if (field == "frame") {
                        if (hasFrame || !parseInteger64(keyframe.frame))
                            return false;
                        hasFrame = true;
                    } else if (field == "value") {
                        if (hasValue || !parseNumber(keyframe.value))
                            return false;
                        hasValue = true;
                    } else if (field == "interpolation") {
                        std::string interpolation;
                        if (hasInterpolation || !parseString(interpolation))
                            return fail("補間の指定が不正です");
                        if (interpolation == "linear")
                            keyframe.interpolation = KeyInterpolation::Linear;
                        else if (interpolation == "ease_in")
                            keyframe.interpolation = KeyInterpolation::EaseIn;
                        else if (interpolation == "ease_out")
                            keyframe.interpolation = KeyInterpolation::EaseOut;
                        else if (interpolation == "spline")
                            keyframe.interpolation = KeyInterpolation::Spline;
                        else if (interpolation == "ease_in_out")
                            keyframe.interpolation = KeyInterpolation::EaseInOut;
                        else
                            return fail("未知のキーフレーム補間です");
                        hasInterpolation = true;
                    } else if (field == "control1") {
                        if (hasControl1 || !parseNumber(keyframe.control1))
                            return false;
                        hasControl1 = true;
                    } else if (field == "control2") {
                        if (hasControl2 || !parseNumber(keyframe.control2))
                            return false;
                        hasControl2 = true;
                    } else if (field == "curve_start") {
                        if (hasStart || !parseNumber(keyframe.curveStart))
                            return fail("曲線の開始値が不正です");
                        hasStart = true;
                    } else if (field == "curve_end") {
                        if (hasEnd || !parseNumber(keyframe.curveEnd))
                            return fail("曲線の終了値が不正です");
                        hasEnd = true;
                    } else {
                        return fail("キーフレームに未知の field があります: " + field);
                    }
                    skipWhitespace();
                    if (!consumeIf(','))
                        break;
                }
                if (!hasFrame || !hasValue || !hasInterpolation || !hasStart || !hasEnd ||
                    !consume('}'))
                    return false;
                if (keyframe.interpolation == KeyInterpolation::Spline &&
                    (!hasControl1 || !hasControl2))
                    return fail("スプラインには両方の制御値が必要です");
                keys.push_back(keyframe);
                skipWhitespace();
                if (!consumeIf(','))
                    break;
            }
        }
        return consume(']');
    }

    bool parseTextClipData(TextClipData& data) {
        bool seen[11] = {};
        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                int index = -1;
                if (key == "content")
                    index = 0;
                else if (key == "font_family")
                    index = 1;
                else if (key == "font_size")
                    index = 2;
                else if (key == "x")
                    index = 3;
                else if (key == "y")
                    index = 4;
                else if (key == "color")
                    index = 5;
                else if (key == "bold")
                    index = 6;
                else if (key == "alignment")
                    index = 7;
                else if (key == "outline_color")
                    index = 8;
                else if (key == "outline_width")
                    index = 9;
                else if (key == "background_color")
                    index = 10;
                if (index < 0 || index > 10)
                    return fail("text clip に未知の field があります: " + key);
                if (seen[index])
                    return fail("text clip の field が重複しています: " + key);
                seen[index] = true;
                if ((index == 0 && !parseString(data.content)) ||
                    (index == 1 && !parseString(data.fontFamily)) ||
                    (index == 2 && !parseInteger(data.fontSize)) ||
                    (index == 3 && !parseInteger(data.x)) ||
                    (index == 4 && !parseInteger(data.y)) ||
                    (index == 5 && !parseString(data.color)) ||
                    (index == 6 && !parseBool(data.bold)) ||
                    (index == 7 && !parseString(data.alignment)) ||
                    (index == 8 && !parseString(data.outlineColor)) ||
                    (index == 9 && !parseInteger(data.outlineWidth)) ||
                    (index == 10 && !parseString(data.backgroundColor)))
                    return false;
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        for (bool present : seen)
            if (!present)
                return fail("text clip の必須 field がありません");
        return true;
    }

    bool parseSubtitleStyle(SubtitleStyle& style) {
        std::unordered_map<std::string, bool> seen;
        if (!consume('{'))
            return false;
        while (!peek('}')) {
            std::string key;
            if (!parseString(key) || !consume(':'))
                return false;
            if (seen[key])
                return fail("字幕書式の項目が重複しています");
            seen[key] = true;
            if (key == "font_family") {
                if (!parseString(style.fontFamily))
                    return false;
            } else if (key == "font_size") {
                if (!parseInteger(style.fontSize))
                    return false;
            } else if (key == "bold") {
                if (!parseBool(style.bold))
                    return false;
            } else if (key == "color") {
                if (!parseString(style.color))
                    return false;
            } else if (key == "outline_color") {
                if (!parseString(style.outlineColor))
                    return false;
            } else if (key == "outline_width") {
                if (!parseInteger(style.outlineWidth))
                    return false;
            } else if (key == "background_color") {
                if (!parseString(style.backgroundColor))
                    return false;
            } else if (key == "alignment") {
                if (!parseString(style.alignment))
                    return false;
            } else if (key == "side_margin") {
                if (!parseNumber(style.sideMargin))
                    return false;
            } else if (key == "bottom_margin") {
                if (!parseNumber(style.bottomMargin))
                    return false;
            } else
                return fail("字幕書式に未知の項目があります");
            if (!consumeIf(','))
                break;
        }
        if (seen.size() != 10)
            return fail("字幕書式の必須項目がありません");
        return consume('}');
    }

    bool parseSubtitles(std::optional<SubtitleTrack>& output) {
        skipWhitespace();
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            output.reset();
            return true;
        }
        SubtitleTrack track;
        std::unordered_map<std::string, bool> seen;
        if (!consume('{'))
            return false;
        while (!peek('}')) {
            std::string key;
            if (!parseString(key) || !consume(':'))
                return false;
            if (seen[key])
                return fail("字幕トラックの項目が重複しています");
            seen[key] = true;
            if (key == "visible") {
                if (!parseBool(track.visible))
                    return false;
            } else if (key == "style") {
                if (!parseSubtitleStyle(track.style))
                    return false;
            } else if (key == "cues") {
                if (!consume('['))
                    return false;
                while (!peek(']')) {
                    SubtitleCue cue;
                    std::unordered_map<std::string, bool> cueSeen;
                    if (!consume('{'))
                        return false;
                    while (!peek('}')) {
                        std::string field;
                        if (!parseString(field) || !consume(':'))
                            return false;
                        if (cueSeen[field])
                            return fail("字幕行の項目が重複しています");
                        cueSeen[field] = true;
                        if (field == "id") {
                            if (!parseString(cue.id))
                                return false;
                        } else if (field == "start_frame") {
                            if (!parseInteger64(cue.startFrame))
                                return false;
                        } else if (field == "end_frame") {
                            if (!parseInteger64(cue.endFrame))
                                return false;
                        } else if (field == "content") {
                            if (!parseString(cue.content))
                                return false;
                        } else if (field == "link_clip_id") {
                            // 未リンクの字幕は項目自体を書かない。空文字列は受け付けない。
                            if (!parseString(cue.linkClipId) || cue.linkClipId.empty())
                                return fail("字幕行のリンク先が不正です");
                        } else
                            return fail("字幕行に未知の項目があります");
                        if (!consumeIf(','))
                            break;
                    }
                    if (cueSeen.size() != (cue.linkClipId.empty() ? 4U : 5U) || !consume('}'))
                        return fail("字幕行の必須項目がありません");
                    track.cues.push_back(std::move(cue));
                    if (!consumeIf(','))
                        break;
                }
                if (!consume(']'))
                    return false;
            } else
                return fail("字幕トラックに未知の項目があります");
            if (!consumeIf(','))
                break;
        }
        if (seen.size() != 3 || !consume('}'))
            return fail("字幕トラックの必須項目がありません");
        output = std::move(track);
        return true;
    }

    bool parseTimelineClip(TimelineClip& clip) {
        bool hasKind = false;
        bool hasMedia = false;
        bool hasName = false;
        bool hasId = false;
        bool hasSourceFpsNum = false;
        bool hasSourceFpsDen = false;
        bool hasSourceFrameCount = false;
        bool hasSourceIn = false;
        bool hasSourceOut = false;
        bool hasTimelineStart = false;
        bool hasEffects = false;
        bool hasTrackKind = false;
        bool hasTrackIndex = false;
        bool hasLinkGroupId = false;
        bool hasMediaItemId = false;
        bool hasSpeedNum = false;
        bool hasSpeedDen = false;
        bool hasPreservePitch = false;
        bool hasEnabled = false;
        bool hasFrameHold = false;
        bool hasText = false;
        std::string kind;
        std::string media;
        std::string trackKind;

        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                if (key == "kind") {
                    if (hasKind || !parseString(kind))
                        return fail("timeline clip の kind が重複または不正です");
                    hasKind = true;
                } else if (key == "media_path") {
                    if (hasMedia || !parseString(media))
                        return fail("timeline clip の media_path が重複または不正です");
                    hasMedia = true;
                } else if (key == "name") {
                    if (hasName || !parseString(clip.name))
                        return fail("timeline clip の name が重複または不正です");
                    hasName = true;
                } else if (key == "id") {
                    if (hasId || !parseString(clip.id))
                        return fail("timeline clip の id が重複または不正です");
                    hasId = true;
                } else if (key == "source_fps_num") {
                    if (hasSourceFpsNum || !parseInteger64(clip.sourceFpsNum))
                        return fail("timeline clip の source_fps_num が重複または不正です");
                    hasSourceFpsNum = true;
                } else if (key == "source_fps_den") {
                    if (hasSourceFpsDen || !parseInteger64(clip.sourceFpsDen))
                        return fail("timeline clip の source_fps_den が重複または不正です");
                    hasSourceFpsDen = true;
                } else if (key == "source_frame_count") {
                    if (hasSourceFrameCount || !parseInteger64(clip.sourceFrameCount))
                        return fail("timeline clip の source_frame_count が重複または不正です");
                    hasSourceFrameCount = true;
                } else if (key == "source_in_frame") {
                    if (hasSourceIn || !parseInteger64(clip.sourceInFrame))
                        return fail("timeline clip の source_in_frame が重複または不正です");
                    hasSourceIn = true;
                } else if (key == "source_out_frame") {
                    if (hasSourceOut || !parseInteger64(clip.sourceOutFrame))
                        return fail("timeline clip の source_out_frame が重複または不正です");
                    hasSourceOut = true;
                } else if (key == "timeline_start_frame") {
                    if (hasTimelineStart || !parseInteger64(clip.timelineStartFrame))
                        return fail("timeline clip の timeline_start_frame が重複または不正です");
                    hasTimelineStart = true;
                } else if (key == "track_kind") {
                    if (hasTrackKind || !parseString(trackKind))
                        return fail("timeline clip の track_kind が重複または不正です");
                    hasTrackKind = true;
                } else if (key == "track_index") {
                    std::int64_t trackIndex = -1;
                    if (hasTrackIndex || !parseInteger64(trackIndex) || trackIndex < 0 ||
                        trackIndex > std::numeric_limits<int>::max()) {
                        return fail("timeline clip の track_index が重複または不正です");
                    }
                    clip.track.index = static_cast<int>(trackIndex);
                    hasTrackIndex = true;
                } else if (key == "speed_num") {
                    if (hasSpeedNum || !parseInteger64(clip.speedNum))
                        return fail("timeline clip の speed_num が重複または不正です");
                    hasSpeedNum = true;
                } else if (key == "speed_den") {
                    if (hasSpeedDen || !parseInteger64(clip.speedDen))
                        return fail("timeline clip の speed_den が重複または不正です");
                    hasSpeedDen = true;
                } else if (key == "preserve_pitch") {
                    if (hasPreservePitch || !parseBool(clip.preservePitch))
                        return fail("timeline clip の preserve_pitch が重複または不正です");
                    hasPreservePitch = true;
                } else if (key == "enabled") {
                    if (hasEnabled || !parseBool(clip.enabled))
                        return fail("timeline clip の enabled が重複または不正です");
                    hasEnabled = true;
                } else if (key == "frame_hold") {
                    if (hasFrameHold)
                        return fail("timeline clip の frame_hold が重複しています");
                    hasFrameHold = true;
                    skipWhitespace();
                    if (text_.compare(position_, 4, "null") == 0) {
                        position_ += 4;
                    } else {
                        FrameHold hold;
                        if (!consume('{'))
                            return fail("frame_hold は object または null です");
                        bool seen[6]{};
                        for (int field = 0; field < 6; ++field) {
                            std::string holdKey;
                            if (field > 0 && !consume(','))
                                return false;
                            if (!parseString(holdKey) || !consume(':'))
                                return false;
                            std::int64_t* value = nullptr;
                            int slot = -1;
                            if (holdKey == "source_frame") {
                                slot = 0;
                                value = &hold.sourceFrame;
                            } else if (holdKey == "source_fps_num") {
                                slot = 1;
                                value = &hold.sourceFpsNum;
                            } else if (holdKey == "source_fps_den") {
                                slot = 2;
                                value = &hold.sourceFpsDen;
                            } else if (holdKey == "source_frame_count") {
                                slot = 3;
                                value = &hold.sourceFrameCount;
                            } else if (holdKey == "speed_num") {
                                slot = 4;
                                value = &hold.speedNum;
                            } else if (holdKey == "speed_den") {
                                slot = 5;
                                value = &hold.speedDen;
                            }
                            if (slot < 0 || seen[slot] || !parseInteger64(*value))
                                return fail("frame_hold の field が重複または不正です");
                            seen[slot] = true;
                        }
                        if (!consume('}'))
                            return false;
                        clip.frameHold = hold;
                    }
                } else if (key == "media_item_id") {
                    if (hasMediaItemId || !parseString(clip.mediaItemId))
                        return fail("timeline clip の media_item_id が重複または不正です");
                    hasMediaItemId = true;
                } else if (key == "link_group_id") {
                    if (hasLinkGroupId || !parseString(clip.linkGroupId))
                        return fail("timeline clip の link_group_id が重複または不正です");
                    hasLinkGroupId = true;
                } else if (key == "effects") {
                    if (hasEffects || !parseClipEffects(clip.effects))
                        return fail("timeline clip の effects が重複または不正です");
                    hasEffects = true;
                } else if (key == "text") {
                    if (hasText || !parseTextClipData(clip.text))
                        return fail("timeline clip の text が重複または不正です");
                    hasText = true;
                } else if (!skipValue()) {
                    return false;
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        if (!hasKind || !hasMedia || !hasName || !hasId || !hasSourceFpsNum || !hasSourceFpsDen ||
            !hasSourceFrameCount || !hasSourceIn || !hasSourceOut || !hasTimelineStart ||
            !hasTrackKind || !hasTrackIndex || !hasSpeedNum || !hasSpeedDen || !hasMediaItemId ||
            !hasPreservePitch || !hasEnabled || !hasFrameHold)
            return fail("timeline clip の必須 field がありません");
        if (hasText != (kind == "text"))
            return fail("timeline clip の text と kind が一致しません");
        if (media.empty() && kind != "text")
            return fail("timeline clip の media_path が空です");
        if (!media.empty() && kind == "text")
            return fail("text clip に media_path は指定できません");
        if (clip.name.empty())
            return fail("timeline clip の name が空です");
        if (!parseClipKind(kind, clip.kind))
            return false;
        if (!parseTrackKind(trackKind, clip.track.kind))
            return false;
        if (!media.empty())
            clip.mediaPath = pathFromUtf8(media);
        return true;
    }

    bool parseTimelineClips(std::vector<TimelineClip>& clips) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (consumeIf(']'))
            return true;
        while (true) {
            TimelineClip clip;
            if (!parseTimelineClip(clip))
                return false;
            clips.push_back(std::move(clip));
            skipWhitespace();
            if (consumeIf(','))
                continue;
            break;
        }
        return consume(']');
    }

    bool parseTimelineTransition(TimelineTransition& transition) {
        bool seen[5] = {};
        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                int field = -1;
                bool parsed = false;
                if (key == "id") {
                    field = 0;
                    parsed = !seen[field] && parseString(transition.id);
                } else if (key == "outgoing_clip_id") {
                    field = 1;
                    parsed = !seen[field] && parseString(transition.outgoingClipId);
                } else if (key == "incoming_clip_id") {
                    field = 2;
                    parsed = !seen[field] && parseString(transition.incomingClipId);
                } else if (key == "frames_before_cut") {
                    field = 3;
                    parsed = !seen[field] && parseInteger64(transition.framesBeforeCut);
                } else if (key == "frames_after_cut") {
                    field = 4;
                    parsed = !seen[field] && parseInteger64(transition.framesAfterCut);
                } else if (!skipValue()) {
                    return false;
                }
                if (field >= 0) {
                    if (!parsed)
                        return fail("timeline transition の " + key + " が重複または不正です");
                    seen[field] = true;
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        for (const bool field : seen) {
            if (!field)
                return fail("timeline transition の必須 field がありません");
        }
        return true;
    }

    bool parseTimelineTransitions(std::vector<TimelineTransition>& transitions) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (consumeIf(']'))
            return true;
        while (true) {
            TimelineTransition transition;
            if (!parseTimelineTransition(transition))
                return false;
            transitions.push_back(std::move(transition));
            skipWhitespace();
            if (consumeIf(','))
                continue;
            break;
        }
        return consume(']');
    }

    bool parseMediaFolder(MediaFolder& folder) {
        bool hasId = false;
        bool hasName = false;
        bool hasParent = false;
        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                if (key == "id") {
                    if (hasId || !parseString(folder.id))
                        return fail("media folder の id が重複または不正です");
                    hasId = true;
                } else if (key == "name") {
                    if (hasName || !parseString(folder.name))
                        return fail("media folder の name が重複または不正です");
                    hasName = true;
                } else if (key == "parent_id") {
                    if (hasParent || !parseString(folder.parentId))
                        return fail("media folder の parent_id が重複または不正です");
                    hasParent = true;
                } else if (!skipValue()) {
                    return false;
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        if (!hasId || !hasName || !hasParent)
            return fail("media folder の必須 field がありません");
        return true;
    }

    bool parseMediaFolders(std::vector<MediaFolder>& folders) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (consumeIf(']'))
            return true;
        while (true) {
            MediaFolder folder;
            if (!parseMediaFolder(folder))
                return false;
            folders.push_back(std::move(folder));
            skipWhitespace();
            if (consumeIf(','))
                continue;
            break;
        }
        return consume(']');
    }

    // 種別に関係なく全 field を必須にする。該当しない値は 0 で保存されており、
    // 0 であることは validateMediaBin が検査する。
    bool parseMediaItem(MediaItem& item) {
        bool seen[12] = {};
        std::string kind;
        std::string media;
        if (!consume('{'))
            return false;
        skipWhitespace();
        if (!peek('}')) {
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':'))
                    return false;
                int field = -1;
                bool parsed = false;
                if (key == "id") {
                    field = 0;
                    parsed = !seen[field] && parseString(item.id);
                } else if (key == "kind") {
                    field = 1;
                    parsed = !seen[field] && parseString(kind);
                } else if (key == "media_path") {
                    field = 2;
                    parsed = !seen[field] && parseString(media);
                } else if (key == "name") {
                    field = 3;
                    parsed = !seen[field] && parseString(item.name);
                } else if (key == "folder_id") {
                    field = 4;
                    parsed = !seen[field] && parseString(item.folderId);
                } else if (key == "fps_num") {
                    field = 5;
                    parsed = !seen[field] && parseInteger64(item.fpsNum);
                } else if (key == "fps_den") {
                    field = 6;
                    parsed = !seen[field] && parseInteger64(item.fpsDen);
                } else if (key == "frame_count") {
                    field = 7;
                    parsed = !seen[field] && parseInteger64(item.frameCount);
                } else if (key == "width") {
                    field = 8;
                    parsed = !seen[field] && parseInteger(item.width);
                } else if (key == "height") {
                    field = 9;
                    parsed = !seen[field] && parseInteger(item.height);
                } else if (key == "sample_rate") {
                    field = 10;
                    parsed = !seen[field] && parseInteger(item.sampleRate);
                } else if (key == "duration_samples") {
                    field = 11;
                    parsed = !seen[field] && parseInteger64(item.durationSamples);
                } else if (!skipValue()) {
                    return false;
                }
                if (field >= 0) {
                    if (!parsed)
                        return fail("media item の " + key + " が重複または不正です");
                    seen[field] = true;
                }
                skipWhitespace();
                if (consumeIf(','))
                    continue;
                break;
            }
        }
        if (!consume('}'))
            return false;
        for (bool present : seen) {
            if (!present)
                return fail("media item の必須 field がありません");
        }
        if (media.empty())
            return fail("media item の media_path が空です");
        if (!parseMediaKindName(kind, item.kind))
            return fail("未知の media item kind です: " + kind);
        item.mediaPath = pathFromUtf8(media);
        return true;
    }

    bool parseMediaItems(std::vector<MediaItem>& items) {
        if (!consume('['))
            return false;
        skipWhitespace();
        if (consumeIf(']'))
            return true;
        while (true) {
            MediaItem item;
            if (!parseMediaItem(item))
                return false;
            items.push_back(std::move(item));
            skipWhitespace();
            if (consumeIf(','))
                continue;
            break;
        }
        return consume(']');
    }

    bool skipValue() {
        skipWhitespace();
        if (position_ >= text_.size())
            return fail("JSON value がありません");
        if (text_[position_] == '"') {
            std::string ignored;
            return parseString(ignored);
        }
        if (text_[position_] == '{') {
            ++position_;
            skipWhitespace();
            if (consumeIf('}'))
                return true;
            while (true) {
                std::string key;
                if (!parseString(key) || !consume(':') || !skipValue())
                    return false;
                if (consumeIf(','))
                    continue;
                return consume('}');
            }
        }
        if (text_[position_] == '[') {
            ++position_;
            skipWhitespace();
            if (consumeIf(']'))
                return true;
            while (true) {
                if (!skipValue())
                    return false;
                if (consumeIf(','))
                    continue;
                return consume(']');
            }
        }
        const std::size_t start = position_;
        while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != '}' &&
               text_[position_] != ']' &&
               !std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        return position_ > start || fail("JSON value が不正です");
    }
};

} // namespace

ProjectSerializationResult serializeProjectJson(const Project& project,
                                                const std::filesystem::path& projectPath) {
    ProjectSerializationResult result;
    if (project.schemaVersion != kSchemaVersion) {
        result.error = "保存できない Project schema_version です";
        return result;
    }
    const auto timeline = validateTimeline(project);
    if (!timeline.success) {
        result.error = timeline.error;
        return result;
    }
    const auto mediaBin = validateMediaBin(project);
    if (!mediaBin.success) {
        result.error = mediaBin.error;
        return result;
    }

    std::error_code pathError;
    const auto absoluteProjectPath = std::filesystem::absolute(projectPath, pathError);
    if (pathError || absoluteProjectPath.filename().empty()) {
        result.error = "Project JSON path が不正です";
        return result;
    }
    const auto projectDirectory = absoluteProjectPath.parent_path().lexically_normal();
    std::filesystem::create_directories(projectDirectory, pathError);
    if (pathError) {
        result.error = "Project directory を作成できません: " + pathError.message();
        return result;
    }

    std::ostringstream json;
    json << std::setprecision(17);
    const auto writeTracks = [&json](const char* key, const std::vector<Track>& tracks) {
        json << "  \"" << key << "\": [";
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            json << (index == 0 ? "\n" : ",\n") << "    { \"name\": \""
                 << escapeJson(tracks[index].name)
                 << "\", \"muted\": " << (tracks[index].muted ? "true" : "false")
                 << ", \"solo\": " << (tracks[index].solo ? "true" : "false")
                 << ", \"mixer_name\": \"" << escapeJson(tracks[index].mixerName)
                 << "\", \"mixer_gain_db\": " << tracks[index].mixerGainDb
                 << ", \"mixer_pan\": " << tracks[index].mixerPan << " }";
        }
        if (!tracks.empty())
            json << '\n';
        json << "  ],\n";
    };

    json << "{\n  \"schema_version\": " << kSchemaVersion << ",\n"
         << "  \"format\": \"" << kFormatMarker << "\",\n"
         << "  \"timeline_fps_num\": " << project.timelineFpsNum << ",\n"
         << "  \"timeline_fps_den\": " << project.timelineFpsDen << ",\n";
    json << "  \"output_width\": " << project.outputWidth << ",\n"
         << "  \"output_height\": " << project.outputHeight << ",\n";
    json << "  \"timeline_markers\": [";
    for (std::size_t index = 0; index < project.timelineMarkers.size(); ++index) {
        if (index != 0)
            json << ", ";
        json << project.timelineMarkers[index];
    }
    json << "],\n  \"in_frame\": ";
    if (project.inFrame)
        json << *project.inFrame;
    else
        json << "null";
    json << ",\n  \"out_frame\": ";
    if (project.outFrame)
        json << *project.outFrame;
    else
        json << "null";
    json << ",\n";
    json << "  \"subtitles\": ";
    if (!project.subtitles)
        json << "null";
    else {
        const auto& track = *project.subtitles;
        const auto& style = track.style;
        json << "{ \"visible\": " << (track.visible ? "true" : "false") << ", \"style\": {";
        json << "\"font_family\": " << "\"" << escapeJson(style.fontFamily) << "\"";
        json << ", \"font_size\": " << style.fontSize;
        json << ", \"bold\": " << (style.bold ? "true" : "false");
        json << ", \"color\": " << "\"" << escapeJson(style.color) << "\"";
        json << ", \"outline_color\": " << "\"" << escapeJson(style.outlineColor) << "\"";
        json << ", \"outline_width\": " << style.outlineWidth;
        json << ", \"background_color\": " << "\"" << escapeJson(style.backgroundColor) << "\"";
        json << ", \"alignment\": " << "\"" << escapeJson(style.alignment) << "\"";
        json << ", \"side_margin\": " << style.sideMargin;
        json << ", \"bottom_margin\": " << style.bottomMargin;
        json << "}, \"cues\": [";
        for (std::size_t i = 0; i < track.cues.size(); ++i) {
            const auto& cue = track.cues[i];
            json << (i ? "," : "") << "{\"id\": \"" << escapeJson(cue.id)
                 << "\", \"start_frame\": " << cue.startFrame << ", \"end_frame\": " << cue.endFrame
                 << ", \"content\": \"" << escapeJson(cue.content) << "\"";
            if (!cue.linkClipId.empty())
                json << ", \"link_clip_id\": \"" << escapeJson(cue.linkClipId) << "\"";
            json << "}";
        }
        json << "]}";
    }
    json << ",\n";
    writeTracks("video_tracks", project.videoTracks);
    writeTracks("audio_tracks", project.audioTracks);
    json << "  \"manim_assets\": [";
    for (std::size_t index = 0; index < project.manimAssets.size(); ++index) {
        const auto& asset = project.manimAssets[index];
        std::string script;
        std::string video;
        if (!persistedScriptPath(asset.scriptPath, projectDirectory, script, result.error) ||
            !persistedGeneratedPath(asset.generatedVideoPath, projectDirectory, video,
                                    result.error)) {
            return result;
        }
        const bool generatedState = asset.generationState == ManimGenerationState::Ready ||
                                    asset.generationState == ManimGenerationState::SourceChanged;
        if (asset.sceneName.empty() ||
            (generatedState && (video.empty() || !isSha256(asset.sourceFingerprint)))) {
            result.error = "Manim asset の必須値が不正です";
            return result;
        }
        const std::string stateName = manimGenerationStateName(asset.generationState);
        if (stateName.empty()) {
            result.error = "保存できない Manim generation_state です";
            return result;
        }

        json << std::setprecision(17) << (index == 0 ? "\n" : ",\n") << "    {\n"
             << "      \"script_path\": \"" << escapeJson(script) << "\",\n"
             << "      \"scene_name\": \"" << escapeJson(asset.sceneName) << "\",\n"
             << "      \"generated_video_path\": \"" << escapeJson(video) << "\",\n"
             << "      \"generation_state\": \"" << stateName << "\",\n"
             << "      \"source_fingerprint\": \"" << escapeJson(asset.sourceFingerprint) << "\"\n"
             << "    }";
    }
    if (!project.manimAssets.empty())
        json << '\n';
    json << "  ],\n  \"timeline_clips\": [";
    const auto writeKeys = [&json](const std::vector<ClipKeyframe>& keys) {
        json << '[';
        for (std::size_t keyIndex = 0; keyIndex < keys.size(); ++keyIndex) {
            if (keyIndex != 0)
                json << ',';
            json << "{\"frame\":" << keys[keyIndex].frame << ",\"value\":" << keys[keyIndex].value
                 << ",\"interpolation\":\""
                 << (keys[keyIndex].interpolation == KeyInterpolation::Linear    ? "linear"
                     : keys[keyIndex].interpolation == KeyInterpolation::EaseIn  ? "ease_in"
                     : keys[keyIndex].interpolation == KeyInterpolation::EaseOut ? "ease_out"
                     : keys[keyIndex].interpolation == KeyInterpolation::Spline  ? "spline"
                                                                                 : "ease_in_out")
                 << "\""
                 << ",\"curve_start\":" << keys[keyIndex].curveStart
                 << ",\"curve_end\":" << keys[keyIndex].curveEnd;
            if (keys[keyIndex].interpolation == KeyInterpolation::Spline)
                json << ",\"control1\":" << keys[keyIndex].control1
                     << ",\"control2\":" << keys[keyIndex].control2;
            json << '}';
        }
        json << ']';
    };
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if ((clip.mediaPath.empty() && clip.kind != TimelineClipKind::Text) ||
            (!clip.mediaPath.empty() && clip.kind == TimelineClipKind::Text) || clip.name.empty()) {
            result.error = "timeline clip の media_path または name が空です";
            return result;
        }
        const std::string kindName = timelineClipKindName(clip.kind);
        if (kindName.empty()) {
            result.error = "保存できない timeline clip kind です";
            return result;
        }
        json << (index == 0 ? "\n" : ",\n") << "    {\n"
             << "      \"kind\": \"" << kindName << "\",\n"
             << "      \"media_path\": \""
             << (clip.kind == TimelineClipKind::Text
                     ? std::string{}
                     : escapeJson(persistedSourcePath(clip.mediaPath, projectDirectory)))
             << "\",\n"
             << "      \"name\": \"" << escapeJson(clip.name) << "\",\n"
             << "      \"id\": \"" << escapeJson(clip.id) << "\",\n"
             << "      \"source_fps_num\": " << clip.sourceFpsNum << ",\n"
             << "      \"source_fps_den\": " << clip.sourceFpsDen << ",\n"
             << "      \"source_frame_count\": " << clip.sourceFrameCount << ",\n"
             << "      \"source_in_frame\": " << clip.sourceInFrame << ",\n"
             << "      \"source_out_frame\": " << clip.sourceOutFrame << ",\n"
             << "      \"timeline_start_frame\": " << clip.timelineStartFrame << ",\n"
             << "      \"speed_num\": " << clip.speedNum << ",\n"
             << "      \"speed_den\": " << clip.speedDen << ",\n"
             << "      \"preserve_pitch\": " << (clip.preservePitch ? "true" : "false") << ",\n"
             << "      \"enabled\": " << (clip.enabled ? "true" : "false") << ",\n"
             << "      \"frame_hold\": ";
        if (clip.frameHold) {
            json << "{ \"source_frame\": " << clip.frameHold->sourceFrame
                 << ", \"source_fps_num\": " << clip.frameHold->sourceFpsNum
                 << ", \"source_fps_den\": " << clip.frameHold->sourceFpsDen
                 << ", \"source_frame_count\": " << clip.frameHold->sourceFrameCount
                 << ", \"speed_num\": " << clip.frameHold->speedNum
                 << ", \"speed_den\": " << clip.frameHold->speedDen << " }";
        } else {
            json << "null";
        }
        json << ",\n"
             << "      \"track_kind\": \"" << trackKindName(clip.track.kind) << "\",\n"
             << "      \"track_index\": " << clip.track.index << ",\n"
             << "      \"media_item_id\": \"" << escapeJson(clip.mediaItemId) << "\",\n"
             << "      \"link_group_id\": \"" << escapeJson(clip.linkGroupId) << "\",\n"
             << "      \"effects\": {\n"
             << "        \"position_x_percent\": " << clip.effects.positionXPercent << ",\n"
             << "        \"position_y_percent\": " << clip.effects.positionYPercent << ",\n"
             << "        \"scale_x_percent\": " << clip.effects.scaleXPercent << ",\n"
             << "        \"scale_y_percent\": " << clip.effects.scaleYPercent << ",\n"
             << "        \"rotation_degrees\": " << clip.effects.rotationDegrees << ",\n"
             << "        \"opacity_percent\": " << clip.effects.opacityPercent << ",\n"
             << "        \"volume_percent\": " << clip.effects.volumePercent << ",\n"
             << "        \"crop_left_percent\": " << clip.effects.cropLeftPercent << ",\n"
             << "        \"crop_top_percent\": " << clip.effects.cropTopPercent << ",\n"
             << "        \"crop_right_percent\": " << clip.effects.cropRightPercent << ",\n"
             << "        \"crop_bottom_percent\": " << clip.effects.cropBottomPercent << ",\n"
             << "        \"fade_in_frames\": " << clip.effects.fadeInFrames << ",\n"
             << "        \"fade_out_frames\": " << clip.effects.fadeOutFrames << ",\n"
             << "        \"opacity_keys\": ";
        writeKeys(clip.effects.opacityKeys);
        json << ",\n        \"volume_keys\": ";
        writeKeys(clip.effects.volumeKeys);
        json << ",\n        \"position_x_keys\": ";
        writeKeys(clip.effects.positionXKeys);
        json << ",\n        \"position_y_keys\": ";
        writeKeys(clip.effects.positionYKeys);
        json << ",\n        \"scale_x_keys\": ";
        writeKeys(clip.effects.scaleXKeys);
        json << ",\n        \"scale_y_keys\": ";
        writeKeys(clip.effects.scaleYKeys);
        json << ",\n        \"rotation_keys\": ";
        writeKeys(clip.effects.rotationKeys);
        json << ",\n        \"crop_left_keys\": ";
        writeKeys(clip.effects.cropLeftKeys);
        json << ",\n        \"crop_top_keys\": ";
        writeKeys(clip.effects.cropTopKeys);
        json << ",\n        \"crop_right_keys\": ";
        writeKeys(clip.effects.cropRightKeys);
        json << ",\n        \"crop_bottom_keys\": ";
        writeKeys(clip.effects.cropBottomKeys);

        json << "\n      }";
        if (clip.kind == TimelineClipKind::Text) {
            const auto& text = clip.text;
            json << ",\n      \"text\": {\n"
                 << "        \"content\": \"" << escapeJson(text.content) << "\",\n"
                 << "        \"font_family\": \"" << escapeJson(text.fontFamily) << "\",\n"
                 << "        \"font_size\": " << text.fontSize << ",\n"
                 << "        \"x\": " << text.x << ",\n"
                 << "        \"y\": " << text.y << ",\n"
                 << "        \"color\": \"" << escapeJson(text.color) << "\",\n"
                 << "        \"bold\": " << (text.bold ? "true" : "false") << ",\n"
                 << "        \"alignment\": \"" << escapeJson(text.alignment) << "\",\n"
                 << "        \"outline_color\": \"" << escapeJson(text.outlineColor) << "\",\n"
                 << "        \"outline_width\": " << text.outlineWidth << ",\n"
                 << "        \"background_color\": \"" << escapeJson(text.backgroundColor)
                 << "\"\n      }";
        }
        json << "\n    }";
    }
    if (!project.timelineClips.empty())
        json << '\n';
    json << "  ],\n  \"timeline_transitions\": [";
    for (std::size_t index = 0; index < project.timelineTransitions.size(); ++index) {
        const auto& transition = project.timelineTransitions[index];
        json << (index == 0 ? "\n" : ",\n") << "    { \"id\": \"" << escapeJson(transition.id)
             << "\", \"outgoing_clip_id\": \"" << escapeJson(transition.outgoingClipId)
             << "\", \"incoming_clip_id\": \"" << escapeJson(transition.incomingClipId)
             << "\", \"frames_before_cut\": " << transition.framesBeforeCut
             << ", \"frames_after_cut\": " << transition.framesAfterCut << " }";
    }
    if (!project.timelineTransitions.empty())
        json << '\n';
    json << "  ],\n  \"media_folders\": [";
    for (std::size_t index = 0; index < project.mediaFolders.size(); ++index) {
        const auto& folder = project.mediaFolders[index];
        json << (index == 0 ? "\n" : ",\n") << "    { \"id\": \"" << escapeJson(folder.id)
             << "\", \"name\": \"" << escapeJson(folder.name) << "\", \"parent_id\": \""
             << escapeJson(folder.parentId) << "\" }";
    }
    if (!project.mediaFolders.empty())
        json << '\n';
    json << "  ],\n  \"media_items\": [";
    for (std::size_t index = 0; index < project.mediaItems.size(); ++index) {
        const auto& item = project.mediaItems[index];
        json << (index == 0 ? "\n" : ",\n") << "    {\n"
             << "      \"id\": \"" << escapeJson(item.id) << "\",\n"
             << "      \"kind\": \"" << mediaKindName(item.kind) << "\",\n"
             << "      \"media_path\": \""
             << escapeJson(persistedSourcePath(item.mediaPath, projectDirectory)) << "\",\n"
             << "      \"name\": \"" << escapeJson(item.name) << "\",\n"
             << "      \"folder_id\": \"" << escapeJson(item.folderId) << "\",\n"
             << "      \"fps_num\": " << item.fpsNum << ",\n"
             << "      \"fps_den\": " << item.fpsDen << ",\n"
             << "      \"frame_count\": " << item.frameCount << ",\n"
             << "      \"width\": " << item.width << ",\n"
             << "      \"height\": " << item.height << ",\n"
             << "      \"sample_rate\": " << item.sampleRate << ",\n"
             << "      \"duration_samples\": " << item.durationSamples << "\n"
             << "    }";
    }
    if (!project.mediaItems.empty())
        json << '\n';
    json << "  ]\n}\n";

    result.json = json.str();
    result.success = true;
    return result;
}

static ProjectIoResult writeUtf8FileAtomically(const std::filesystem::path& path,
                                               const std::string& bytes) {
    ProjectIoResult result;
    char detail[512] = {};
    if (mvm_atomic_write_file(path.c_str(), bytes.data(), bytes.size(), detail, sizeof(detail)) !=
        0) {
        result.error =
            detail[0] != '\0' ? detail : "Project JSON を書き込めません: " + pathToUtf8(path);
        return result;
    }
    result.success = true;
    return result;
}

ProjectIoResult saveProjectJson(const Project& project, const std::filesystem::path& projectPath) {
    const auto serialized = serializeProjectJson(project, projectPath);
    if (!serialized.success) {
        ProjectIoResult result;
        result.error = serialized.error;
        return result;
    }
    return writeUtf8FileAtomically(projectPath, serialized.json);
}

ProjectIoResult saveProjectJsonTransaction(Project& liveProject, Project candidate,
                                           const std::filesystem::path& projectPath) {
    ProjectIoResult result = saveProjectJson(candidate, projectPath);
    if (result.success)
        liveProject = std::move(candidate);
    return result;
}

namespace {

std::wstring caseFoldedPathKey(const std::filesystem::path& path) {
    auto key = canonicalPathKey(path);
    for (auto& character : key)
        character = static_cast<wchar_t>(std::towlower(character));
    return key;
}

// 以前の版は clip と素材の path を大文字小文字を畳んで照合していた (同じ schema 14)。
// その版で保存した、clip の path が素材の path と大文字小文字だけ違う Project も開けるよう、
// 読み込むときだけ clip の path を素材 (mediaItemId が指す authority) の表記へ揃える。
// 揃えるのは「以前の規則では同じ、今の規則では違う」clip のうち、同じ実体と言えるものだけ
// (mayAdoptLegacyCaseSpelling)。case-sensitive directory で実在する別ファイルを指していたら
// 揃えず、照合で読み込みを拒否する (黙って再生するファイルを差し替えない)。
void adoptMediaItemPathSpelling(Project& project) {
    std::unordered_map<std::string, const MediaItem*> items;
    for (const auto& item : project.mediaItems)
        items.try_emplace(item.id, &item);
    for (auto& clip : project.timelineClips) {
        if (clip.mediaItemId.empty())
            continue;
        const auto item = items.find(clip.mediaItemId);
        if (item == items.end())
            continue;
        const auto& itemPath = item->second->mediaPath;
        if (canonicalPathKey(clip.mediaPath) != canonicalPathKey(itemPath) &&
            caseFoldedPathKey(clip.mediaPath) == caseFoldedPathKey(itemPath) &&
            mayAdoptLegacyCaseSpelling(fileIdentityKey(clip.mediaPath), fileIdentityKey(itemPath)))
            clip.mediaPath = itemPath;
    }
}

} // namespace

ProjectLoadResult parseProjectJsonText(const std::string& jsonText,
                                       const std::filesystem::path& projectPath) {
    ProjectLoadResult result;
    std::error_code pathError;
    const auto absoluteProjectPath = std::filesystem::absolute(projectPath, pathError);
    if (pathError) {
        result.error = "Project JSON path が不正です";
        return result;
    }

    Project parsed;
    ProjectJsonParser parser(jsonText);
    if (!parser.parse(parsed, result.error))
        return result;

    const auto projectDirectory = absoluteProjectPath.parent_path().lexically_normal();
    for (auto& asset : parsed.manimAssets) {
        asset.scriptPath = resolveSourcePath(asset.scriptPath, projectDirectory);

        if (!asset.generatedVideoPath.empty()) {
            if (!isProjectManagedRelativePath(asset.generatedVideoPath)) {
                result.error = "generated_video_path が project-relative ではありません";
                return result;
            }
            asset.generatedVideoPath =
                (projectDirectory / asset.generatedVideoPath).lexically_normal();
        }

        const bool generatedState = asset.generationState == ManimGenerationState::Ready ||
                                    asset.generationState == ManimGenerationState::SourceChanged;
        if (generatedState &&
            (asset.generatedVideoPath.empty() || !isSha256(asset.sourceFingerprint))) {
            result.error = "生成済み Manim asset の video path または fingerprint が不正です";
            return result;
        }
    }

    for (auto& clip : parsed.timelineClips)
        if (clip.kind != TimelineClipKind::Text)
            clip.mediaPath = resolveSourcePath(clip.mediaPath, projectDirectory);
    for (auto& item : parsed.mediaItems)
        item.mediaPath = resolveSourcePath(item.mediaPath, projectDirectory);
    adoptMediaItemPathSpelling(parsed);
    // media_path の重複は解決後の path で判定する。
    const auto mediaBin = validateMediaBin(parsed);
    if (!mediaBin.success) {
        result.error = mediaBin.error;
        return result;
    }

    result.project = std::move(parsed);
    result.success = true;
    return result;
}

ProjectLoadResult loadProjectJson(const std::filesystem::path& projectPath) {
    ProjectLoadResult result;
    std::error_code pathError;
    const auto absoluteProjectPath = std::filesystem::absolute(projectPath, pathError);
    if (pathError) {
        result.error = "Project JSON path が不正です";
        return result;
    }
    std::ifstream input(absoluteProjectPath, std::ios::binary);
    if (!input) {
        result.error = "Project JSON を読めません: " + pathToUtf8(absoluteProjectPath);
        return result;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return parseProjectJsonText(contents.str(), absoluteProjectPath);
}

namespace {

bool parseRecoveryString(const std::string& text, std::size_t& position, std::string& value,
                         std::string& error) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\n' || text[position] == '\r'))
        ++position;
    if (position >= text.size() || text[position] != '"') {
        error = "自動復旧データの文字列が不正です";
        return false;
    }
    ++position;
    value.clear();
    while (position < text.size()) {
        const unsigned char character = static_cast<unsigned char>(text[position++]);
        if (character == '"')
            return true;
        if (character == '\\') {
            if (position >= text.size()) {
                error = "自動復旧データのescapeが途中で終わっています";
                return false;
            }
            const char escaped = text[position++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value += escaped;
                break;
            case 'b':
                value += '\b';
                break;
            case 'f':
                value += '\f';
                break;
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            case 'u': {
                if (position + 4 > text.size()) {
                    error = "自動復旧データのunicode escapeが途中で終わっています";
                    return false;
                }
                int code = 0;
                for (int index = 0; index < 4; ++index) {
                    const char hex = text[position++];
                    code <<= 4;
                    if (hex >= '0' && hex <= '9')
                        code += hex - '0';
                    else if (hex >= 'a' && hex <= 'f')
                        code += hex - 'a' + 10;
                    else if (hex >= 'A' && hex <= 'F')
                        code += hex - 'A' + 10;
                    else {
                        error = "自動復旧データのunicode escapeが不正です";
                        return false;
                    }
                }
                if (code > 0x7f) {
                    error = "自動復旧metadataの非ASCII unicode escapeは読めません";
                    return false;
                }
                value += static_cast<char>(code);
                break;
            }
            default:
                error = "自動復旧データのescapeが不正です";
                return false;
            }
            continue;
        }
        if (character < 0x20) {
            error = "自動復旧データの文字列に制御文字が含まれています";
            return false;
        }
        value += static_cast<char>(character);
    }
    error = "自動復旧データの文字列が閉じられていません";
    return false;
}

bool skipRecoveryWhitespace(const std::string& text, std::size_t& position) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\n' || text[position] == '\r'))
        ++position;
    return true;
}

bool extractBalancedJson(const std::string& text, std::size_t& position, std::string& value,
                         std::string& error) {
    skipRecoveryWhitespace(text, position);
    if (position >= text.size() || text[position] != '{') {
        error = "自動復旧データのProject本体がobjectではありません";
        return false;
    }
    const std::size_t start = position;
    int depth = 0;
    bool inString = false;
    while (position < text.size()) {
        const char character = text[position++];
        if (inString) {
            if (character == '\\') {
                if (position >= text.size()) {
                    error = "自動復旧データのProject本体が途中で終わっています";
                    return false;
                }
                ++position;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"')
            inString = true;
        else if (character == '{')
            ++depth;
        else if (character == '}') {
            --depth;
            if (depth == 0) {
                value = text.substr(start, position - start);
                return true;
            }
        }
    }
    error = "自動復旧データのProject本体が閉じていません";
    return false;
}

bool parseRecoveryEnvelope(const std::string& text, ProjectRecoveryLoadResult& result,
                           const std::filesystem::path& canonicalPath) {
    std::size_t position = 0;
    skipRecoveryWhitespace(text, position);
    if (position >= text.size() || text[position] != '{') {
        result.error = "自動復旧データがobjectではありません";
        return false;
    }
    ++position;

    bool hasVersion = false;
    bool hasHash = false;
    bool hasSavedAt = false;
    bool hasSession = false;
    bool hasCanonicalPath = false;
    bool hasProject = false;
    int version = 0;
    std::string projectJson;
    skipRecoveryWhitespace(text, position);
    if (position < text.size() && text[position] != '}') {
        while (true) {
            std::string key;
            if (!parseRecoveryString(text, position, key, result.error))
                return false;
            skipRecoveryWhitespace(text, position);
            if (position >= text.size() || text[position] != ':') {
                result.error = "自動復旧データのfield区切りが不正です";
                return false;
            }
            ++position;
            if (key == "recovery_format_version") {
                if (hasVersion) {
                    result.error = "recovery_format_version が重複しています";
                    return false;
                }
                skipRecoveryWhitespace(text, position);
                const std::size_t start = position;
                if (position >= text.size() ||
                    !std::isdigit(static_cast<unsigned char>(text[position]))) {
                    result.error = "recovery_format_version が不正です";
                    return false;
                }
                while (position < text.size() &&
                       std::isdigit(static_cast<unsigned char>(text[position])))
                    ++position;
                try {
                    version = std::stoi(text.substr(start, position - start));
                } catch (...) {
                    result.error = "recovery_format_version が範囲外です";
                    return false;
                }
                hasVersion = true;
            } else if (key == "canonical_sha256") {
                if (hasHash ||
                    !parseRecoveryString(text, position, result.canonicalSha256, result.error)) {
                    if (result.error.empty())
                        result.error = "canonical_sha256 が重複または不正です";
                    return false;
                }
                hasHash = true;
            } else if (key == "saved_at") {
                if (hasSavedAt ||
                    !parseRecoveryString(text, position, result.savedAt, result.error)) {
                    if (result.error.empty())
                        result.error = "saved_at が重複または不正です";
                    return false;
                }
                hasSavedAt = true;
            } else if (key == "session_id") {
                if (hasSession ||
                    !parseRecoveryString(text, position, result.sessionId, result.error)) {
                    if (result.error.empty())
                        result.error = "session_id が重複または不正です";
                    return false;
                }
                hasSession = true;
            } else if (key == "canonical_path") {
                if (hasCanonicalPath ||
                    !parseRecoveryString(text, position, result.canonicalPath, result.error)) {
                    if (result.error.empty())
                        result.error = "canonical_path が重複または不正です";
                    return false;
                }
                hasCanonicalPath = true;
            } else if (key == "project") {
                if (hasProject || !extractBalancedJson(text, position, projectJson, result.error)) {
                    if (result.error.empty())
                        result.error = "project が重複または不正です";
                    return false;
                }
                hasProject = true;
            } else {
                result.error = "自動復旧データに未知のfieldがあります: " + key;
                return false;
            }
            skipRecoveryWhitespace(text, position);
            if (position < text.size() && text[position] == ',') {
                ++position;
                continue;
            }
            break;
        }
    }
    skipRecoveryWhitespace(text, position);
    if (position >= text.size() || text[position] != '}') {
        result.error = "自動復旧データが閉じていません";
        return false;
    }
    ++position;
    skipRecoveryWhitespace(text, position);
    if (position != text.size()) {
        result.error = "自動復旧データの末尾に余分な値があります";
        return false;
    }
    if (!hasVersion || version != 1 || !hasHash || !hasSavedAt || !hasSession ||
        !hasCanonicalPath || !hasProject) {
        result.error = "自動復旧データの必須fieldがありません";
        return false;
    }
    if (result.savedAt.empty() || result.sessionId.empty() || result.canonicalPath.empty()) {
        result.error = "自動復旧データの識別情報が空です";
        return false;
    }
    if (!result.canonicalSha256.empty() && !isSha256(result.canonicalSha256)) {
        result.error = "canonical_sha256 がSHA-256ではありません";
        return false;
    }
    // 別Projectへコピーされたrecoveryを、開いているpath基準でrebaseしない。
    if (!sameCanonicalPath(pathFromUtf8(result.canonicalPath), canonicalPath)) {
        result.foreignProject = true;
        result.success = true;
        return true;
    }
    const auto project = parseProjectJsonText(projectJson, canonicalPath);
    if (!project.success) {
        result.error = project.error;
        return false;
    }
    result.project = project.project;
    result.legacy = false;
    result.success = true;
    return true;
}

} // namespace

ProjectIoResult saveProjectRecovery(const Project& project,
                                    const std::filesystem::path& recoveryPath,
                                    const std::filesystem::path& canonicalPath,
                                    const std::string& canonicalSha256, const std::string& savedAt,
                                    const std::string& sessionId) {
    ProjectIoResult result;
    if (savedAt.empty() || sessionId.empty() ||
        (!canonicalSha256.empty() && !isSha256(canonicalSha256))) {
        result.error = "自動復旧データの識別情報が不正です";
        return result;
    }
    std::error_code pathError;
    const auto absoluteCanonical =
        std::filesystem::absolute(canonicalPath, pathError).lexically_normal();
    if (pathError) {
        result.error = "canonical pathを解決できません";
        return result;
    }
    const auto serialized = serializeProjectJson(project, absoluteCanonical);
    if (!serialized.success) {
        result.error = serialized.error;
        return result;
    }
    std::ostringstream json;
    json << "{\n"
         << "  \"recovery_format_version\": 1,\n"
         << "  \"canonical_sha256\": \"" << escapeJson(canonicalSha256) << "\",\n"
         << "  \"saved_at\": \"" << escapeJson(savedAt) << "\",\n"
         << "  \"session_id\": \"" << escapeJson(sessionId) << "\",\n"
         << "  \"canonical_path\": \"" << escapeJson(pathToUtf8(absoluteCanonical)) << "\",\n"
         << "  \"project\": " << serialized.json << "}\n";
    return writeUtf8FileAtomically(recoveryPath, json.str());
}

ProjectRecoveryLoadResult loadProjectRecovery(const std::filesystem::path& recoveryPath,
                                              const std::filesystem::path& canonicalPath) {
    ProjectRecoveryLoadResult result;
    std::ifstream input(recoveryPath, std::ios::binary);
    if (!input) {
        result.error = "自動復旧データを読めません: " + pathToUtf8(recoveryPath);
        return result;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string text = contents.str();
    if (text.find("\"recovery_format_version\"") != std::string::npos) {
        parseRecoveryEnvelope(text, result, canonicalPath);
        return result;
    }

    const auto legacy = parseProjectJsonText(text, canonicalPath);
    if (!legacy.success) {
        result.error = legacy.error.empty() ? "自動復旧データを解釈できません" : legacy.error;
        return result;
    }
    result.project = legacy.project;
    result.legacy = true;
    result.success = true;
    return result;
}

bool sameCanonicalPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return comparePathIdentity(left, right) == PathSameness::Same;
}

RecoveryDisposition classifyRecovery(const Project& recoveryProject,
                                     const Project& canonicalProject,
                                     const std::string& recordedHash,
                                     const std::string& currentHash) {
    if (recoveryProject == canonicalProject)
        return RecoveryDisposition::Stale;
    if (recordedHash == currentHash)
        return RecoveryDisposition::Restorable;
    return RecoveryDisposition::CanonicalChanged;
}

} // namespace mvm::project
