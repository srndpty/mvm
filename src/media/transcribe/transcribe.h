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
    // 試験用の差し込み口。モデルの読み込みで 1 回読むたびに、読む前に呼ぶ。
    // キャンセルが読み込みの途中で効くことを、実モデル無しで確かめるために使う。
    std::function<void()> modelReadObserver;
};

struct Result {
    bool success = false;
    bool cancelled = false;
    Backend backend = Backend::Cpu;
    std::vector<Segment> segments;
    std::string error;
};

Result transcribe(const Request& request, const std::atomic<bool>* cancel = nullptr);

// 音声の準備で実際にデコードした量。所要時間ではなく仕事量で性能の退行を検査する。
struct PrepareStats {
    std::int64_t decodedSamples = 0; // 16kHz に変換した後のサンプル数 (捨てた分も含む)
    bool seeked = false;             // 区間の開始の手前へ seek したか
};

// 音声の準備は認識エンジンから独立して検査できる。
bool prepareAudio(const Request& request, const std::atomic<bool>* cancel,
                  std::vector<float>& samples, std::string& error, PrepareStats* stats = nullptr);
} // namespace mvm::transcribe
#endif
