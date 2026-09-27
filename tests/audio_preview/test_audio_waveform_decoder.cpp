// timeline 波形の decode。素材はテスト内で WAV として書き出すので、期待する振幅と
// channel 構成が既知である。期待値は実装の式を使わず直接書く。

#include "media/audio_waveform/audio_waveform_decoder.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void putU32(std::ofstream& out, std::uint32_t value) {
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                          static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes, 4);
}

void putU16(std::ofstream& out, std::uint16_t value) {
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
    out.write(bytes, 2);
}

// 16-bit PCM WAV。samples は interleaved。
bool writeWav(const std::filesystem::path& path, int sampleRate, int channels,
              const std::vector<float>& samples) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    putU32(out, 36 + dataBytes);
    out.write("WAVEfmt ", 8);
    putU32(out, 16);
    putU16(out, 1);
    putU16(out, static_cast<std::uint16_t>(channels));
    putU32(out, static_cast<std::uint32_t>(sampleRate));
    putU32(out, static_cast<std::uint32_t>(sampleRate * channels * 2));
    putU16(out, static_cast<std::uint16_t>(channels * 2));
    putU16(out, 16);
    out.write("data", 4);
    putU32(out, dataBytes);
    for (const float sample : samples)
        putU16(out, static_cast<std::uint16_t>(
                        static_cast<std::int16_t>(std::lround(sample * 32767.0f))));
    return static_cast<bool>(out);
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool closeTo(float value, float expected) {
    return std::abs(value - expected) < 0.02f;
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("mvm_waveform_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    constexpr double kPi = 3.14159265358979323846;

    // --- mono 48kHz: 前半 0.5 秒は振幅 0.5 の 440Hz、後半 0.5 秒は無音 ---
    // 日本語 path でも開けること (app は QString から UTF-8 で渡す)。
    {
        std::vector<float> samples(48000, 0.0f);
        for (int index = 0; index < 24000; ++index)
            samples[static_cast<std::size_t>(index)] =
                0.5f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * index / 48000.0));
        const auto path = directory / std::filesystem::path(u8"波形_mono.wav");
        check(writeWav(path, 48000, 1, samples), "mono WAV を書けない");
        const auto result = mvm::audio::decodeAudioWaveform(utf8(path));
        check(result.success, "mono WAV を decode できない");
        if (!result.success)
            std::fprintf(stderr, "  error: %s\n", result.error.c_str());
        check(result.peaks.channels == 1, "mono が 1 channel にならない");
        check(result.peaks.sampleRate == 48000, "mono の sample rate が 48000 でない");
        const auto loud = mvm::core::waveformColumn(result.peaks, 0, 0.0, 0.5);
        check(loud.valid && closeTo(loud.maximum, 0.5f) && closeTo(loud.minimum, -0.5f),
              "mono 前半の振幅が 0.5 でない");
        const auto quiet = mvm::core::waveformColumn(result.peaks, 0, 0.6, 1.0);
        check(quiet.valid && quiet.maximum == 0.0f && quiet.minimum == 0.0f,
              "mono 後半が無音でない");
        check(!mvm::core::waveformColumn(result.peaks, 0, 1.01, 1.1).valid,
              "素材の尺より後ろを valid にした");
    }

    // --- stereo 44.1kHz: left は振幅 0.25 の矩形波、right は無音 ---
    {
        std::vector<float> samples(44100 * 2, 0.0f);
        for (int index = 0; index < 44100; ++index)
            samples[static_cast<std::size_t>(index) * 2] = (index / 50) % 2 == 0 ? 0.25f : -0.25f;
        const auto path = directory / "stereo.wav";
        check(writeWav(path, 44100, 2, samples), "stereo WAV を書けない");
        const auto result = mvm::audio::decodeAudioWaveform(utf8(path));
        check(result.success, "stereo WAV を decode できない");
        if (!result.success)
            std::fprintf(stderr, "  error: %s\n", result.error.c_str());
        check(result.peaks.channels == 2, "stereo が 2 channel にならない");
        check(result.peaks.sampleRate == 44100, "stereo の sample rate が 44100 でない");
        const auto left = mvm::core::waveformColumn(result.peaks, 0, 0.0, 1.0);
        check(left.valid && closeTo(left.maximum, 0.25f) && closeTo(left.minimum, -0.25f),
              "stereo left の振幅が 0.25 でない");
        const auto right = mvm::core::waveformColumn(result.peaks, 1, 0.0, 1.0);
        check(right.valid && right.maximum == 0.0f && right.minimum == 0.0f,
              "stereo right が無音でない (channel を混ぜている)");
    }

    // --- 失敗は失敗として返す ---
    {
        const auto missing = mvm::audio::decodeAudioWaveform(utf8(directory / "missing.wav"));
        check(!missing.success && !missing.error.empty(), "存在しない素材を成功にした");

        const auto textPath = directory / "not-audio.wav";
        {
            std::ofstream out(textPath, std::ios::binary);
            out << "this is not audio";
        }
        const auto text = mvm::audio::decodeAudioWaveform(utf8(textPath));
        check(!text.success && !text.error.empty(), "音声でない file を成功にした");

        check(!mvm::audio::decodeAudioWaveform("").success, "空の path を成功にした");

        const std::atomic<bool> cancel{true};
        const auto cancelled =
            mvm::audio::decodeAudioWaveform(utf8(directory / "stereo.wav"), &cancel);
        check(!cancelled.success && cancelled.cancelled, "中断を成功にした");
    }

    std::filesystem::remove_all(directory, ignored);
    if (failures != 0) {
        std::fprintf(stderr, "audio waveform decoder: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("audio waveform decoder: ok\n");
    return 0;
}
