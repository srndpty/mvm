// AudioDecodeWorker の再生速度 (レート調整)。速度 s の素材を「速度で伸縮した時間軸」の
// 48 kHz sample として出すことを、実際に decode した PCM で確かめる。
//   sample 数   48000 / s          (1 秒の素材)
//   周波数      1000 x s Hz        (音程は速度に連動する)
//   seek        伸縮した時間軸の位置で exact に止まり、通して decode した同じ位置と一致する
// 期待値は速度の定義から直接書いており、worker の rate 計算は使わない。

#include "media/audio_preview/audio_decode_worker.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <process.h>
#include <string>
#include <vector>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool condition, const std::string& message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::fprintf(stderr, "NG: %s\n", message.c_str());
    }
}

std::string toUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

struct Speed {
    std::int64_t num;
    std::int64_t den;
};

// seek してから EOF まで (または count sample) 読む。左 channel だけを返す。
bool readFrom(mvm::audio::AudioDecodeWorker& worker, std::int64_t first, std::int64_t count,
              std::vector<float>& left, std::string& error) {
    worker.pause();
    mvm::audio::AudioSeekTicket ticket;
    if (worker.requestSeek(first, ticket, error) != mvm::audio::AudioSeekRequestResult::Accepted)
        return false;
    mvm::audio::AudioSeekCompletion completion;
    if (worker.waitSeek(ticket, 5000, completion) != mvm::audio::AudioSeekWaitResult::Ready ||
        !completion.completed || completion.firstOutputSample != first) {
        error = "exact seekに失敗しました: " + completion.error;
        return false;
    }
    worker.play();
    left.clear();
    std::vector<float> block(2048 * mvm::audio::kInternalChannels);
    int waits = 0;
    while (static_cast<std::int64_t>(left.size()) < count) {
        const auto wanted =
            std::min<std::int64_t>(2048, count - static_cast<std::int64_t>(left.size()));
        const auto result =
            worker.queue().consume(block.data(), first + static_cast<std::int64_t>(left.size()),
                                   wanted, completion.seekGeneration);
        for (std::int64_t i = 0; i < result.audioSamples; ++i)
            left.push_back(block[static_cast<std::size_t>(i) * mvm::audio::kInternalChannels]);
        if (result.shortageKind == mvm::audio::AudioShortageKind::TerminalEof)
            return true;
        if (result.audioSamples > 0) {
            waits = 0;
            continue;
        }
        // EOF の印が付く前に待ちへ入ると sample は来ない。待ちの結果に関わらず consume を
        // やり直し、TerminalEof を受け取る。
        if (++waits > 50) {
            error = "decodeが必要なsampleを供給できません";
            return false;
        }
        worker.queue().waitForSamples(1, 200);
    }
    return true;
}

int risingCrossings(const std::vector<float>& samples, std::size_t begin, std::size_t end) {
    int count = 0;
    for (std::size_t i = begin + 1; i < end && i < samples.size(); ++i)
        count += samples[i - 1] < 0.0F && samples[i] >= 0.0F ? 1 : 0;
    return count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "使い方: mvm_test_audio_playback_speed <work dir> <ffmpeg>\n");
        return 2;
    }
    const std::filesystem::path work = std::filesystem::absolute(argv[1]);
    const std::filesystem::path ffmpeg = std::filesystem::absolute(argv[2]);
    std::error_code ignored;
    std::filesystem::create_directories(work, ignored);
    // 44.1 kHz にして、48 kHz への rate 変換と速度の両方を通す。
    const auto source = work / L"speed-1000hz-44100.wav";
    if (_wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f",
                 L"lavfi", L"-i", L"sine=frequency=1000:duration=1:sample_rate=44100", L"-c:a",
                 L"pcm_s16le", source.c_str(), static_cast<wchar_t*>(nullptr)) != 0) {
        std::fprintf(stderr, "検証用WAVを生成できません\n");
        return 2;
    }

    const Speed speeds[] = {{1, 1}, {1, 2}, {2, 1}, {73, 100}};
    int measured = 0;
    for (const auto speed : speeds) {
        const double s = static_cast<double>(speed.num) / static_cast<double>(speed.den);
        const std::string label = std::to_string(speed.num) + "/" + std::to_string(speed.den);
        mvm::audio::AudioDecodeWorker worker({1});
        std::string error;
        if (!worker.setPlaybackSpeed(speed.num, speed.den, false, error) ||
            !worker.start(toUtf8(source), error)) {
            check(false, label + ": workerを開始できません: " + error);
            continue;
        }
        std::vector<float> whole;
        const bool read = readFrom(worker, 0, 48000 * 20, whole, error);
        check(read, label + ": 全体を読めません: " + error);
        const double expectedSamples = 48000.0 / s;
        check(std::abs(static_cast<double>(whole.size()) - expectedSamples) <= 64.0,
              label + ": sample数 " + std::to_string(whole.size()) +
                  " が 48000/s = " + std::to_string(expectedSamples) + " と違います");
        // 中央の半分の区間で周波数を測る。
        const std::size_t begin = whole.size() / 4;
        const std::size_t end = whole.size() * 3 / 4;
        const double seconds = static_cast<double>(end - begin) / 48000.0;
        const double frequency = static_cast<double>(risingCrossings(whole, begin, end)) / seconds;
        check(std::abs(frequency - 1000.0 * s) <= 1000.0 * s * 0.01 + 2.0,
              label + ": 周波数 " + std::to_string(frequency) + "Hz が 1000 x s と違います");
        std::fprintf(stderr, "  %s: %zu sample, %.1f Hz\n", label.c_str(), whole.size(), frequency);

        // 伸縮した時間軸の位置へ seek し、通して読んだ同じ位置と比べる。
        const auto target = static_cast<std::int64_t>(expectedSamples / 3.0);
        std::vector<float> seeked;
        const bool seekRead = readFrom(worker, target, 4800, seeked, error);
        check(seekRead && seeked.size() == 4800, label + ": seek後に読めません: " + error);
        if (seekRead && seeked.size() == 4800 &&
            whole.size() >= static_cast<std::size_t>(target) + 4800) {
            double difference = 0.0;
            for (std::size_t i = 0; i < seeked.size(); ++i) {
                const double d = seeked[i] - whole[static_cast<std::size_t>(target) + i];
                difference += d * d;
            }
            const double rms = std::sqrt(difference / static_cast<double>(seeked.size()));
            check(rms < 0.02, label + ": seek位置の波形が通しのdecodeと一致しません (rms " +
                                  std::to_string(rms) + ")");
        }
        worker.stop();
        ++measured;
    }
    check(measured == static_cast<int>(sizeof(speeds) / sizeof(speeds[0])),
          "全速度を測っていません");
    for (const Speed speed : {Speed{1, 2}, Speed{2, 1}}) {
        const std::string label =
            "pitch " + std::to_string(speed.num) + "/" + std::to_string(speed.den);
        mvm::audio::AudioDecodeWorker worker({1});
        std::string error;
        if (!worker.setPlaybackSpeed(speed.num, speed.den, true, error) ||
            !worker.start(toUtf8(source), error)) {
            check(false, label + ": workerを開始できません: " + error);
            continue;
        }
        std::vector<float> whole;
        const bool read = readFrom(worker, 0, 48000 * 20, whole, error);
        check(read, label + ": 全体を読めません: " + error);
        const double expectedSamples = 48000.0 * speed.den / speed.num;
        // 末尾の遅延を押し出さないと 2 倍速で約 500 sample 欠けた。等速と同じ桁まで詰める。
        check(std::abs(static_cast<double>(whole.size()) - expectedSamples) <= 256.0,
              label + ": sample数が伸縮後の尺と違います: " + std::to_string(whole.size()));
        const std::size_t begin = whole.size() / 4;
        const std::size_t end = whole.size() * 3 / 4;
        const double frequency = static_cast<double>(risingCrossings(whole, begin, end)) /
                                 (static_cast<double>(end - begin) / 48000.0);
        check(std::abs(frequency - 1000.0) <= 20.0,
              label + ": 1000Hzを保てません: " + std::to_string(frequency));
        std::fprintf(stderr, "  %s: %zu sample, %.1f Hz\n", label.c_str(), whole.size(), frequency);
        const auto target = static_cast<std::int64_t>(expectedSamples / 3.0);
        std::vector<float> seeked;
        const bool seekRead = readFrom(worker, target, 4800, seeked, error);
        check(seekRead && seeked.size() == 4800, label + ": seek後のsampleが揃いません: " + error);
        if (seekRead && seeked.size() == 4800 &&
            whole.size() >= static_cast<std::size_t>(target) + seeked.size()) {
            double difference = 0.0;
            for (std::size_t i = 0; i < seeked.size(); ++i) {
                const double d = seeked[i] - whole[static_cast<std::size_t>(target) + i];
                difference += d * d;
            }
            const double rms = std::sqrt(difference / seeked.size());
            check(rms < 0.02, label + ": seek波形が連続再生と違います: " + std::to_string(rms));
        }
        worker.stop();
    }
    // ピッチ保持の seek は、伸縮した 48 kHz の位置を素材 rate (44.1 kHz) の位置へ戻す。
    // 速度だけを戻すと位置が 48000/44100 倍後ろへずれ、ずれが preroll (1 秒) を超える遠い
    // 位置では要求位置より後ろから decode して seek が成立しない。1 秒の素材ではずれが
    // preroll に収まり検出できないので、30 秒の素材で遠い位置へ seek する。
    // stretcher は seek で reset するので、純音の位相は連続再生と揃わない。位置は 3 Hz の
    // 振幅変調の包絡 (10 ms ごとの RMS) で比べる。位置が 1 frame 分ずれても包絡は変わる。
    const auto longSource = work / L"speed-am-44100-30s.wav";
    if (_wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f",
                 L"lavfi", L"-i", L"aevalsrc=(0.5+0.4*sin(2*PI*3*t))*sin(2*PI*1000*t):d=30:s=44100",
                 L"-c:a", L"pcm_s16le", longSource.c_str(), static_cast<wchar_t*>(nullptr)) != 0) {
        check(false, "長い検証用WAVを生成できません");
    } else {
        int farSeeks = 0;
        for (const Speed speed : {Speed{1, 2}, Speed{2, 1}}) {
            const std::string label =
                "pitch far " + std::to_string(speed.num) + "/" + std::to_string(speed.den);
            mvm::audio::AudioDecodeWorker worker({1});
            std::string error;
            if (!worker.setPlaybackSpeed(speed.num, speed.den, true, error) ||
                !worker.start(toUtf8(longSource), error)) {
                check(false, label + ": workerを開始できません: " + error);
                continue;
            }
            // 伸縮後の尺の 80% の位置。素材上では 24 秒で、誤った換算では約 26.1 秒になる。
            const std::int64_t outputLength = 48000LL * 30 * speed.den / speed.num;
            const std::int64_t target = outputLength * 4 / 5;
            std::vector<float> whole;
            check(readFrom(worker, 0, outputLength, whole, error),
                  label + ": 全体を読めません: " + error);
            std::vector<float> seeked;
            const bool seekRead = readFrom(worker, target, 4800, seeked, error);
            check(seekRead && seeked.size() == 4800,
                  label + ": 遠い位置へ seek できません: " + error);
            if (seekRead && seeked.size() == 4800 &&
                whole.size() >= static_cast<std::size_t>(target) + seeked.size()) {
                const auto windowRms = [](const float* samples) {
                    double sum = 0.0;
                    for (int i = 0; i < 480; ++i)
                        sum += static_cast<double>(samples[i]) * samples[i];
                    return std::sqrt(sum / 480.0);
                };
                double worst = 0.0;
                for (std::size_t window = 0; window < 10; ++window) {
                    const double got = windowRms(seeked.data() + window * 480);
                    const double want =
                        windowRms(whole.data() + static_cast<std::size_t>(target) + window * 480);
                    worst = std::max(worst, std::abs(got - want));
                }
                check(worst < 0.03,
                      label + ": 遠い seek の包絡が連続再生と違います: " + std::to_string(worst));
                // 対照: 連続再生側を 50 ms ずらすと包絡の差が閾値を超える (比較が空振りしない)。
                double shiftedWorst = 0.0;
                if (whole.size() >= static_cast<std::size_t>(target) + 2400 + seeked.size()) {
                    for (std::size_t window = 0; window < 10; ++window) {
                        const double got = windowRms(seeked.data() + window * 480);
                        const double want = windowRms(
                            whole.data() + static_cast<std::size_t>(target) + 2400 + window * 480);
                        shiftedWorst = std::max(shiftedWorst, std::abs(got - want));
                    }
                }
                check(shiftedWorst >= 0.03, label + ": 50 ms ずれを包絡で検出できません: " +
                                                std::to_string(shiftedWorst));
                std::fprintf(stderr, "  %s: 包絡の最大差 %.4f (50 ms ずらすと %.4f)\n",
                             label.c_str(), worst, shiftedWorst);
                ++farSeeks;
            }
            worker.stop();
        }
        check(farSeeks == 2, "ピッチ保持の遠い seek を全速度で比較していません");
    }
    std::fprintf(stderr, "audio playback speed: 検査 %d 件 / 失敗 %d 件\n", gChecks, gFailures);
    return gChecks > 0 && gFailures == 0 ? 0 : 1;
}
