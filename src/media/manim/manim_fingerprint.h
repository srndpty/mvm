#ifndef MVM_MEDIA_MANIM_MANIM_FINGERPRINT_H
#define MVM_MEDIA_MANIM_MANIM_FINGERPRINT_H

#include <filesystem>
#include <string>

namespace mvm::manim {

struct ManimFingerprintResult {
    bool success = false;
    std::string fingerprint;
    std::string error;
};

// Manim script 本体だけを SHA-256 で fingerprint する。
// imported module、外部 asset、Python environment は対象に含めない。
ManimFingerprintResult fingerprintManimSource(const std::filesystem::path& scriptPath);

} // namespace mvm::manim

#endif // MVM_MEDIA_MANIM_MANIM_FINGERPRINT_H
