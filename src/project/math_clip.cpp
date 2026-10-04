#include "project/math_clip.h"

#include "project/project.h"

#include <cstdint>

namespace mvm::project {

bool validateMathClipData(const MathClipData& data, int outputHeight, std::string& error) {
    if (data.syntax != "latex") {
        error = "数式の記法が未対応です: " + data.syntax;
        return false;
    }
    if (data.source.find_first_not_of(" \t\r\n") == std::string::npos) {
        error = "数式が空です";
        return false;
    }
    if (data.fontSize < 1 || data.fontSize > outputHeight) {
        error =
            "数式の文字サイズは 1 から出力の高さ (" + std::to_string(outputHeight) + ") までです";
        return false;
    }
    std::uint32_t argb = 0;
    if (!parseArgbColor(data.color, argb) || !parseArgbColor(data.backgroundColor, argb)) {
        error = "数式の色は #AARRGGBB で指定してください";
        return false;
    }
    return true;
}

bool validateMathClipAnimation(const MathClipAnimation& animation, std::int64_t clipSourceFrames,
                               std::string& error) {
    switch (animation.intro) {
    case MathIntroKind::None:
        if (animation.introFrames != 0) {
            error = "数式の intro が無いのに尺があります";
            return false;
        }
        return true;
    case MathIntroKind::Write:
        if (animation.introFrames < 1 || animation.introFrames > clipSourceFrames) {
            error = "数式の Write の尺は 1 frame から clip の尺までです";
            return false;
        }
        return true;
    }
    error = "数式の intro の種類が不正です";
    return false;
}

const char* mathIntroKindName(MathIntroKind kind) {
    switch (kind) {
    case MathIntroKind::None:
        return "none";
    case MathIntroKind::Write:
        return "write";
    }
    return "none";
}

bool parseMathIntroKind(const std::string& name, MathIntroKind& kind) {
    if (name == "none") {
        kind = MathIntroKind::None;
        return true;
    }
    if (name == "write") {
        kind = MathIntroKind::Write;
        return true;
    }
    return false;
}

} // namespace mvm::project
