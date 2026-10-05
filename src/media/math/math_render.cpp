#include "media/math/math_render.h"

#include "util/mvm_sha256.h"

namespace mvm::math {
namespace {

void appendField(std::string& material, const char* name, const std::string& value) {
    material += name;
    material += '=';
    material += std::to_string(value.size());
    material += ':';
    material += value;
    material += '\n';
}

// 失敗 (CNG の失敗) なら空文字列。
std::string hexDigest(const std::string& material) {
    char hex[MVM_SHA256_HEX_SIZE] = {};
    if (mvm_sha256_hex(material.data(), material.size(), hex) != 0)
        return {};
    return hex;
}

} // namespace

const char* mathRenderStatusName(MathRenderStatus status) {
    switch (status) {
    case MathRenderStatus::Ok:
        return "ok";
    case MathRenderStatus::InvalidSource:
        return "invalid_source";
    case MathRenderStatus::BackendUnavailable:
        return "backend_unavailable";
    case MathRenderStatus::Failed:
        return "failed";
    case MathRenderStatus::TimedOut:
        return "timed_out";
    case MathRenderStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string mathRenderKey(const MathRenderSpec& spec, const MathToolchainFingerprint& toolchain) {
    std::string material = "mvm-math-static/1\n";
    appendField(material, "syntax", spec.syntax);
    appendField(material, "source", spec.source);
    material += "font_size=" + std::to_string(spec.fontSize) + "\n";
    appendField(material, "backend", toolchain.backendId);
    appendField(material, "toolchain", toolchain.canonical);
    return hexDigest(material);
}

const char* mathAnimationKindName(MathAnimationKind kind) {
    switch (kind) {
    case MathAnimationKind::Write:
        return "write";
    }
    return "unknown";
}

std::string mathSequenceKey(const MathSequenceSpec& spec, const MathToolchainFingerprint& toolchain,
                            const std::string& sequenceTemplate) {
    std::string material = "mvm-math-sequence/1\n";
    appendField(material, "animation", mathAnimationKindName(spec.animation));
    material += "frames=" + std::to_string(spec.frames) + "\n";
    appendField(material, "syntax", spec.still.syntax);
    appendField(material, "source", spec.still.source);
    material += "font_size=" + std::to_string(spec.still.fontSize) + "\n";
    appendField(material, "backend", toolchain.backendId);
    appendField(material, "toolchain", toolchain.canonical);
    appendField(material, "sequence_template", sequenceTemplate);
    return hexDigest(material);
}

} // namespace mvm::math
