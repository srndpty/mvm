#include "media/audio_preview/audio_decode_worker.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "使い方: mvm_test_audio_nonzero_start_seek <source> [sample ...]\n";
        return 2;
    }

    std::vector<std::int64_t> targets{0, 24000};
    if (argc > 2) {
        targets.clear();
        for (int index = 2; index < argc; ++index) {
            std::int64_t target = -1;
            const std::string text = argv[index];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), target);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || target < 0) {
                std::cerr << "sample位置が不正です: " << text << '\n';
                return 2;
            }
            targets.push_back(target);
        }
    }

    mvm::audio::AudioDecodeWorker worker({1});
    std::string error;
    if (!worker.start(argv[1], error)) {
        std::cerr << "audio workerを開始できません: " << error << '\n';
        return 3;
    }

    for (const std::int64_t target : targets) {
        mvm::audio::AudioSeekTicket ticket;
        if (worker.requestSeek(target, ticket, error) !=
            mvm::audio::AudioSeekRequestResult::Accepted) {
            std::cerr << "audio seekを受理できません: target=" << target << " " << error << '\n';
            worker.stop();
            return 4;
        }
        mvm::audio::AudioSeekCompletion completion;
        const auto waited = worker.waitSeek(ticket, 5000, completion);
        if (waited != mvm::audio::AudioSeekWaitResult::Ready || !completion.completed ||
            completion.firstOutputSample != target) {
            std::cerr << "exact audio seekに失敗しました: target=" << target
                      << " first=" << completion.firstOutputSample << " detail=" << completion.error
                      << '\n';
            worker.stop();
            return 5;
        }
    }

    worker.stop();
    return 0;
}
