#include "media/transcribe/transcribe.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "失敗: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    if (argc != 3 && (argc < 5 || argc > 7)) {
        std::fprintf(stderr, "使い方: 検査実行ファイル <音声素材> <作業フォルダー> "
                             "[モデル cpu|vulkan [言語 [語彙ヒント]]]\n");
        return 2;
    }
    std::filesystem::create_directories(argv[2]);
    mvm::transcribe::Request request;
    request.mediaPath = argv[1];
    std::string error;
    std::vector<float> samples;
    require(mvm::transcribe::prepareAudio(request, nullptr, samples, error), "音声準備の対照群");
    require(samples.size() >= 16000 && samples.size() <= 16000 * 10, "16kHz音声の尺を実際に比較");
    auto full = samples;
    request.sourceBeginMs = 500;
    request.sourceEndMs = 1000;
    require(mvm::transcribe::prepareAudio(request, nullptr, samples, error) &&
                samples.size() == 8000,
            "指定区間のサンプル数");
    require(std::equal(samples.begin(), samples.end(), full.begin() + 8000),
            "素材のトリム区間を比較");
    request.sourceBeginMs = 0;
    request.sourceEndMs = -1;
    std::atomic<bool> cancelled{true};
    require(!mvm::transcribe::prepareAudio(request, &cancelled, samples, error),
            "音声準備をキャンセル");
    request.modelPath = std::filesystem::path(argv[2]) / "missing-model.bin";
    auto missing = mvm::transcribe::transcribe(request);
    require(!missing.success && !missing.error.empty(), "モデル不在を拒否");
    request.modelPath = std::filesystem::path(argv[2]) / "broken-model.bin";
    {
        std::ofstream file(request.modelPath, std::ios::binary);
        file << "モデルではありません";
    }
    auto broken = mvm::transcribe::transcribe(request);
    require(!broken.success && !broken.error.empty(), "モデル破損を拒否");
    {
        std::ofstream file(request.modelPath, std::ios::binary);
        const std::uint32_t header[14] = {0x67676d6c};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
    }
    require(!mvm::transcribe::transcribe(request).success, "magicだけが正しい破損モデルを拒否");
    // 寸法とフィルターは揃っているが、重みを持たない検査用モデルを生成する。
    const std::uint32_t emptyHeader[14] = {0x67676d6c, 51865, 1500, 384, 6, 4,  448,
                                           384,        6,     4,    80,  1, 80, 201};
    {
        std::ofstream file(request.modelPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(emptyHeader), sizeof(emptyHeader));
        const std::vector<float> filters(80 * 201, 0);
        file.write(reinterpret_cast<const char*>(filters.data()),
                   static_cast<std::streamsize>(filters.size() * sizeof(float)));
        const std::uint32_t vocabularyCount = 0;
        file.write(reinterpret_cast<const char*>(&vocabularyCount), sizeof(vocabularyCount));
    }
    const auto empty = mvm::transcribe::transcribe(request);
    require(!empty.success &&
                empty.error == "認識モデルに重みがありません。検査用の空モデルは使用できません",
            "正常なEOFでも重みのないモデルを成功にしない");
    {
        std::ofstream file(request.modelPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(emptyHeader), sizeof(emptyHeader));
    }
    const auto truncated = mvm::transcribe::transcribe(request);
    require(!truncated.success && truncated.error.find("途中で切れている") != std::string::npos,
            "ヘッダー以降の欠損を理由付きで拒否");
    request.mediaPath = "missing-audio.wav";
    require(!mvm::transcribe::prepareAudio(request, nullptr, samples, error), "素材不在を拒否");
    request.mediaPath = MVM_TRANSCRIBE_NO_AUDIO;
    require(std::filesystem::is_regular_file(request.mediaPath),
            "音声なし試験の素材がありません。pwsh scripts/make-testmedia.ps1 -Mode "
            "Smokeを実行してください");
    require(!mvm::transcribe::prepareAudio(request, nullptr, samples, error) &&
                error == "対象素材に音声がありません",
            "実在する音声なし素材を拒否");
    // 第5引数は認識経路、第6引数は言語 (既定 en)、第7引数は語彙ヒント。
    if (argc >= 5 && argc <= 7) {
        request.mediaPath = argv[1];
        request.modelPath = argv[3];
        request.language = argc >= 6 ? argv[5] : "en";
        if (argc == 7)
            request.initialPrompt = argv[6];
        const std::string backend(argv[4]);
        require(backend == "cpu" || backend == "vulkan", "認識経路の指定");
        request.backend =
            backend == "vulkan" ? mvm::transcribe::Backend::Vulkan : mvm::transcribe::Backend::Cpu;
        auto result = mvm::transcribe::transcribe(request);
        if (!result.success)
            std::fprintf(stderr, "%s\n", result.error.c_str());
        require(result.success && result.backend == request.backend && !result.segments.empty(),
                "実モデルの認識結果と経路");
        std::size_t compared = 0;
        for (const auto& cue : result.segments) {
            require(!cue.text.empty() && cue.startMs >= 0 && cue.endMs > cue.startMs &&
                        cue.endMs <= static_cast<std::int64_t>(full.size()) / 16,
                    "認識時刻と本文");
            // 精度の比較は人が読んで行う。合否には使わない。
            std::printf("[%lld-%lld] %s\n", static_cast<long long>(cue.startMs),
                        static_cast<long long>(cue.endMs), cue.text.c_str());
            ++compared;
        }
        std::printf("実認識の区間を%zu件比較しました\n", compared);
    }
    std::puts("音声準備・モデル不在・破損・キャンセルの検査に合格しました");
}
