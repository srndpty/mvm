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
    char hex[MVM_SHA256_HEX_SIZE] = {};
    if (mvm_sha256_hex(material.data(), material.size(), hex) != 0)
        return {};
    return hex;
}

} // namespace mvm::math
