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

} // namespace mvm::project
