#include "media/math/math_render.h"

#include "media/math/math_key_material.h"
#include "util/mvm_sha256.h"

namespace mvm::math {
namespace detail {

void appendKeyField(std::string& material, const char* name, const std::string& value) {
    material += name;
    material += '=';
    material += std::to_string(value.size());
    material += ':';
    material += value;
    material += '\n';
}

std::string keyDigest(const std::string& material) {
    char hex[MVM_SHA256_HEX_SIZE] = {};
    if (mvm_sha256_hex(material.data(), material.size(), hex) != 0)
        return {};
    return hex;
}

} // namespace detail

namespace {

using detail::appendKeyField;
using detail::keyDigest;

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
    appendKeyField(material, "syntax", spec.syntax);
    appendKeyField(material, "source", spec.source);
    material += "font_size=" + std::to_string(spec.fontSize) + "\n";
    appendKeyField(material, "backend", toolchain.backendId);
    appendKeyField(material, "toolchain", toolchain.canonical);
    return keyDigest(material);
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
    appendKeyField(material, "animation", mathAnimationKindName(spec.animation));
    material += "frames=" + std::to_string(spec.frames) + "\n";
    appendKeyField(material, "syntax", spec.still.syntax);
    appendKeyField(material, "source", spec.still.source);
    material += "font_size=" + std::to_string(spec.still.fontSize) + "\n";
    appendKeyField(material, "backend", toolchain.backendId);
    appendKeyField(material, "toolchain", toolchain.canonical);
    appendKeyField(material, "sequence_template", sequenceTemplate);
    return keyDigest(material);
}

} // namespace mvm::math
