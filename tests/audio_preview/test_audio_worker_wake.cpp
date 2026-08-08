#include "media/audio_preview/audio_decode_worker.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "使い方: mvm_test_audio_worker_wake <fixture> <cycles>\n";
        return 2;
    }
    const int cycles = std::atoi(argv[2]);
    if (cycles <= 0)
        return 2;
    for (int index = 0; index < cycles; ++index) {
        mvm::audio::AudioDecodeWorker worker({1});
        std::string error;
        if (!worker.start(argv[1], error)) {
            std::cerr << "worker start に失敗しました: " << error << '\n';
            return 3;
        }
        // idle のまま直ちに stop する。sleep で race window を隠さない。
        worker.stop();
        if (!worker.snapshot().joined) {
            std::cerr << "worker join が完了していません: cycle=" << index << '\n';
            return 3;
        }
    }
    std::cout << "PASS: idle start/stop " << cycles << " cycles, join hang 0\n";
    return 0;
}
