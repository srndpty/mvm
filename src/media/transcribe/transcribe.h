#ifndef MVM_MEDIA_TRANSCRIBE_H
#define MVM_MEDIA_TRANSCRIBE_H
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mvm::transcribe {
enum class Backend { Cpu, Vulkan };

struct Segment {
    std::int64_t startMs = 0, endMs = 0;
    std::string text;
};

struct Request {
    std::filesystem::path mediaPath;
    std::filesystem::path modelPath;
    Backend backend = Backend::Cpu;
    std::string language = "ja";
    std::int64_t sourceBeginMs = 0;
    std::int64_t sourceEndMs = -1;
    // 固有名詞・専門用語などの語彙ヒント。空なら使わない。全区間の認識へ同じヒントを渡す。
    std::string initialPrompt;
    int threads = 4;
    std::function<void(int)> progress;
};

struct Result {
    bool success = false;
    bool cancelled = false;
    Backend backend = Backend::Cpu;
    std::vector<Segment> segments;
    std::string error;
};

Result transcribe(const Request& request, const std::atomic<bool>* cancel = nullptr);
// 音声の準備は認識エンジンから独立して検査できる。
bool prepareAudio(const Request& request, const std::atomic<bool>* cancel,
                  std::vector<float>& samples, std::string& error);
} // namespace mvm::transcribe
#endif
