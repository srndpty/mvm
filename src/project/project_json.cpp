#include "project/project_json.h"

#include "project/timeline_edit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace mvm::project {
namespace {

constexpr int kSchemaVersion = 2;

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
        bool hasTimelineFpsNum = false;
        bool hasTimelineFpsDen = false;
        bool hasAssets = false;
        bool hasClips = false;
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
                } else if (key == "timeline_fps_num") {
                    if (hasTimelineFpsNum || !parseInteger64(project.timelineFpsNum))
                        return failAndFinish("timeline_fps_num が重複または不正です", error);
                    hasTimelineFpsNum = true;
                } else if (key == "timeline_fps_den") {
                    if (hasTimelineFpsDen || !parseInteger64(project.timelineFpsDen))
                        return failAndFinish("timeline_fps_den が重複または不正です", error);
                    hasTimelineFpsDen = true;
                } else if (key == "manim_assets") {
                    if (hasAssets || !parseAssets(project.manimAssets))
                        return failAndFinish("manim_assets が重複または不正です", error);
                    hasAssets = true;
                } else if (key == "timeline_clips") {
                    if (hasClips || !parseTimelineClips(project.timelineClips))
                        return failAndFinish("timeline_clips が重複または不正です", error);
                    hasClips = true;
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
            return failAndFinish(
                "対応していない schema_version です: " + std::to_string(project.schemaVersion) +
                    "。schema 2 Projectを作成してください",
                error);
        if (!hasTimelineFpsNum || !hasTimelineFpsDen || !hasAssets || !hasClips)
            return failAndFinish("Project schema 2 の必須 field がありません", error);
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

    bool parseClipKind(const std::string& text, TimelineClipKind& kind) {
        if (text == "video")
            kind = TimelineClipKind::Video;
        else if (text == "manim")
            kind = TimelineClipKind::Manim;
        else
            return fail("未知の timeline clip kind です: " + text);
        return true;
    }

    bool parseClipEffects(ClipEffects& effects) {
        bool seen[11] = {};
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
                else if (key == "scale_percent")
                    field = 2;
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
                else
                    return fail("effects に未知の field があります: " + key);
                if (seen[field])
                    return fail("effects field が重複しています: " + key);
                seen[field] = true;
                if (field == 0 && !parseNumber(effects.positionXPercent))
                    return false;
                if (field == 1 && !parseNumber(effects.positionYPercent))
                    return false;
                if (field == 2 && !parseNumber(effects.scalePercent))
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
        bool hasVideoTrack = false;
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
                } else if (key == "video_track") {
                    std::int64_t videoTrack = -1;
                    if (hasVideoTrack || !parseInteger64(videoTrack) || videoTrack < 0 ||
                        videoTrack > 1) {
                        return fail("timeline clip の video_track が重複または不正です");
                    }
                    clip.videoTrack = static_cast<VideoTrack>(videoTrack);
                    hasVideoTrack = true;
                } else if (key == "effects") {
                    if (hasEffects || !parseClipEffects(clip.effects))
                        return fail("timeline clip の effects が重複または不正です");
                    hasEffects = true;
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
            !hasSourceFrameCount || !hasSourceIn || !hasSourceOut || !hasTimelineStart)
            return fail("timeline clip の必須 field がありません");
        if (media.empty())
            return fail("timeline clip の media_path が空です");
        if (clip.name.empty())
            return fail("timeline clip の name が空です");
        if (!parseClipKind(kind, clip.kind))
            return false;
        // schema 2互換契約: video_track欠損は明示的にV1として読む。
        if (!hasVideoTrack)
            clip.videoTrack = VideoTrack::V1;
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

ProjectIoResult saveProjectJson(const Project& project, const std::filesystem::path& projectPath) {
    ProjectIoResult result;
    if (project.schemaVersion != kSchemaVersion) {
        result.error = "保存できない Project schema_version です";
        return result;
    }
    const auto timeline = validateTimeline(project);
    if (!timeline.success) {
        result.error = timeline.error;
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
    json << "{\n  \"schema_version\": 2,\n"
         << "  \"timeline_fps_num\": " << project.timelineFpsNum << ",\n"
         << "  \"timeline_fps_den\": " << project.timelineFpsDen << ",\n"
         << "  \"manim_assets\": [";
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
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if (clip.mediaPath.empty() || clip.name.empty()) {
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
             << escapeJson(persistedSourcePath(clip.mediaPath, projectDirectory)) << "\",\n"
             << "      \"name\": \"" << escapeJson(clip.name) << "\",\n"
             << "      \"id\": \"" << escapeJson(clip.id) << "\",\n"
             << "      \"source_fps_num\": " << clip.sourceFpsNum << ",\n"
             << "      \"source_fps_den\": " << clip.sourceFpsDen << ",\n"
             << "      \"source_frame_count\": " << clip.sourceFrameCount << ",\n"
             << "      \"source_in_frame\": " << clip.sourceInFrame << ",\n"
             << "      \"source_out_frame\": " << clip.sourceOutFrame << ",\n"
             << "      \"timeline_start_frame\": " << clip.timelineStartFrame << ",\n"
             << "      \"video_track\": " << static_cast<int>(clip.videoTrack) << ",\n"
             << "      \"effects\": {\n"
             << "        \"position_x_percent\": " << clip.effects.positionXPercent << ",\n"
             << "        \"position_y_percent\": " << clip.effects.positionYPercent << ",\n"
             << "        \"scale_percent\": " << clip.effects.scalePercent << ",\n"
             << "        \"rotation_degrees\": " << clip.effects.rotationDegrees << ",\n"
             << "        \"opacity_percent\": " << clip.effects.opacityPercent << ",\n"
             << "        \"crop_left_percent\": " << clip.effects.cropLeftPercent << ",\n"
             << "        \"crop_top_percent\": " << clip.effects.cropTopPercent << ",\n"
             << "        \"crop_right_percent\": " << clip.effects.cropRightPercent << ",\n"
             << "        \"crop_bottom_percent\": " << clip.effects.cropBottomPercent << ",\n"
             << "        \"fade_in_frames\": " << clip.effects.fadeInFrames << ",\n"
             << "        \"fade_out_frames\": " << clip.effects.fadeOutFrames << "\n"
             << "      }\n"
             << "    }";
    }
    if (!project.timelineClips.empty())
        json << '\n';
    json << "  ]\n}\n";

    std::ofstream output(absoluteProjectPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = "Project JSON を書き込めません: " + pathToUtf8(absoluteProjectPath);
        return result;
    }
    output << json.str();
    if (!output.good()) {
        result.error = "Project JSON の書き込み中に失敗しました";
        return result;
    }
    result.success = true;
    return result;
}

ProjectIoResult saveProjectJsonTransaction(Project& liveProject, Project candidate,
                                           const std::filesystem::path& projectPath) {
    ProjectIoResult result = saveProjectJson(candidate, projectPath);
    if (result.success)
        liveProject = std::move(candidate);
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

    Project parsed;
    const std::string jsonText = contents.str();
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
        clip.mediaPath = resolveSourcePath(clip.mediaPath, projectDirectory);

    result.project = std::move(parsed);
    result.success = true;
    return result;
}

} // namespace mvm::project
