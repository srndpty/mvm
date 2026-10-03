#include "media/transcribe/transcribe.h"

#include "media/transcribe/model_reader.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ggml-backend.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <whisper.h>

namespace mvm::transcribe {
namespace {
std::mutex engineMutex;

struct LogState {
    std::atomic<bool> gpuFailed{false};
    std::atomic<bool> gpuUsed{false};
    bool emptyModel = false;
    std::string modelError;
};

void logCallback(ggml_log_level, const char* message, void* opaque) {
    auto& state = *static_cast<LogState*>(opaque);
    const std::string_view text(message);
    if (text.find("no tensors loaded") != std::string_view::npos)
        state.emptyModel = true;
    if (text.find("認識モデルが途中で切れている") != std::string_view::npos)
        state.modelError = "認識モデルが途中で切れているか、読み取りに失敗しました";
    else if (text.find("not all tensors loaded") != std::string_view::npos)
        state.modelError = "認識モデルに必要な重みが不足しています";
    else if (text.find("unknown tensor") != std::string_view::npos ||
             text.find("wrong size") != std::string_view::npos)
        state.modelError = "認識モデルの重みの構造が不正です";
    if (text.find("failed to initialize") != std::string_view::npos &&
        text.find("backend") != std::string_view::npos)
        state.gpuFailed = true;
    if (text.find("using Vulkan") != std::string_view::npos &&
        text.find("backend") != std::string_view::npos)
        state.gpuUsed = true;
}

bool stopped(const std::atomic<bool>* cancel) {
    return cancel && cancel->load();
}

// モデルの読み込み中にキャンセルされた。読み込みの callback から投げ、whisper の初期化を
// 途中で打ち切る (whisper の loader には中断の手段が無い)。
struct ModelLoadCancelled {};

struct CallbackState {
    const Request* request;
    const std::atomic<bool>* cancel;
};
} // namespace

Result transcribe(const Request& request, const std::atomic<bool>* cancel) {
    Result result;
    result.backend = request.backend;
    if (stopped(cancel)) {
        result.cancelled = true;
        result.error = "文字起こしをキャンセルしました";
        return result;
    }
    if (request.threads < 1 ||
        (request.language != "auto" && whisper_lang_id(request.language.c_str()) < 0)) {
        result.error = "認識言語またはスレッド数が不正です";
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(request.modelPath, ec)) {
        result.error = "ローカルの認識モデルが見つかりません";
        return result;
    }
    std::vector<float> samples;
    if (!prepareAudio(request, cancel, samples, result.error)) {
        result.cancelled = stopped(cancel);
        return result;
    }
    if (request.progress)
        request.progress(5);
    // whisperのログ設定とbackend登録はprocess共通なので、認識を直列化する。
    std::lock_guard lock(engineMutex);
    if (stopped(cancel)) {
        result.cancelled = true;
        result.error = "文字起こしをキャンセルしました";
        return result;
    }
    ggml_backend_load_all_from_path(MVM_WHISPER_BIN_DIR);
    if (stopped(cancel)) {
        result.cancelled = true;
        result.error = "文字起こしをキャンセルしました";
        return result;
    }
    auto contextParams = whisper_context_default_params();
    contextParams.use_gpu = request.backend == Backend::Vulkan;
    contextParams.flash_attn = false;
    if (contextParams.use_gpu) {
        int gpuIndex = 0;
        bool found = false;
        for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            auto dev = ggml_backend_dev_get(i);
            const auto type = ggml_backend_dev_type(dev);
            if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU)
                continue;
            const auto reg = ggml_backend_dev_backend_reg(dev);
            if (std::string_view(ggml_backend_reg_name(reg)) == "Vulkan") {
                std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)> probe(
                    ggml_backend_dev_init(dev, nullptr), ggml_backend_free);
                if (!probe) {
                    result.error = "Vulkan認識デバイスを初期化できません";
                    return result;
                }
                contextParams.gpu_device = gpuIndex;
                found = true;
                break;
            }
            ++gpuIndex;
        }
        if (!found) {
            result.error =
                "Vulkan認識デバイスがありません。CPUへ切り替える場合は明示的に選択してください";
            return result;
        }
    }
    LogState logs;
    whisper_log_set(logCallback, &logs);

    struct RestoreLog {
        ~RestoreLog() { whisper_log_set(nullptr, nullptr); }
    } restore;

    try {
        // ファイル名の文字コードに依存しないよう、モデルを標準ifstreamではなくloader経由で開く。
        FILE* file = _wfopen(request.modelPath.c_str(), L"rb");
        if (!file) {
            result.error = "認識モデルを開けません";
            return result;
        }
        std::unique_ptr<FILE, decltype(&std::fclose)> modelFile(file, std::fclose);

        struct ModelInput {
            FILE* file;
            std::int64_t size;
            const std::atomic<bool>* cancel;
            const Request* request;
        } modelInput{file, 0, cancel, &request};

        if (_fseeki64(file, 0, SEEK_END) != 0 || (modelInput.size = _ftelli64(file)) < 4 ||
            _fseeki64(file, 0, SEEK_SET) != 0) {
            result.error = "認識モデルが空または読み取り不能です";
            return result;
        }
        // 固定版のモデル先頭を検査し、破損した寸法をassertや巨大な確保へ渡さない。
        std::array<std::uint32_t, 14> header{};
        if (std::fread(header.data(), sizeof(header), 1, file) != 1 || header[0] != 0x67676d6c ||
            header[1] < 51864 || header[1] > 51866 || header[2] != 1500 ||
            (header[3] != 384 && header[3] != 512 && header[3] != 768 && header[3] != 1024 &&
             header[3] != 1280) ||
            header[4] != header[3] / 64 || header[5] < 4 || header[5] > 32 || header[6] != 448 ||
            header[7] != header[3] || header[8] != header[4] || header[9] < 4 || header[9] > 32 ||
            (header[10] != 80 && header[10] != 128) || header[12] != header[10] ||
            header[13] != 201 || _fseeki64(file, 0, SEEK_SET) != 0) {
            result.error = "認識モデルのヘッダーが破損しているか、未対応の形式です";
            return result;
        }
        whisper_model_loader loader{
            &modelInput,
            [](void* opaque, void* output, size_t size) {
                auto& input = *static_cast<ModelInput*>(opaque);
                // 数 GB のモデルの読み込み中もキャンセルを効かせる。効かないと、読み込み中に
                // window を閉じたとき、終了処理が読み込みの完了を待って止まる。
                if (input.request->modelReadObserver)
                    input.request->modelReadObserver();
                if (stopped(input.cancel))
                    throw ModelLoadCancelled{};
                return detail::readModelBytes(input.file, output, size);
            },
            [](void* opaque) { return std::feof(static_cast<ModelInput*>(opaque)->file) != 0; },
            [](void*) {}};
        if (stopped(cancel))
            throw ModelLoadCancelled{};
        std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
            whisper_init_with_params(&loader, contextParams), whisper_free);
        if (stopped(cancel))
            throw ModelLoadCancelled{};
        if (!context) {
            result.error = logs.modelError.empty() ? "認識モデルが破損しているか、初期化できません"
                                                   : logs.modelError;
            return result;
        }
        if (logs.emptyModel) {
            result.error = "認識モデルに重みがありません。検査用の空モデルは使用できません";
            return result;
        }
        if (request.backend == Backend::Vulkan && (logs.gpuFailed || !logs.gpuUsed)) {
            result.error = "Vulkan経路の初期化を確認できません。CPUへの縮退を拒否しました";
            return result;
        }
        if (!whisper_is_multilingual(context.get()) && request.language != "en") {
            result.error = "日本語・自動検出には多言語モデルを指定してください";
            return result;
        }
        // 速度より精度を優先する。貪欲法は一度外すと「EISATS」のような音写へ崩れやすいので、
        // whisper.cpp CLI と同じ beam 5 / best_of 5 で探索し、温度の再試行も既定のまま使う。
        auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        params.beam_search.beam_size = 5;
        params.greedy.best_of = 5;
        params.n_threads = request.threads;
        params.translate = false;
        params.language = request.language == "auto" ? nullptr : request.language.c_str();
        params.detect_language = false;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.print_special = false;
        // 前区間の本文を文脈にすると、同じ文の繰り返しへ陥ったときに後続まで巻き込む。
        // 文脈は語彙ヒントだけに限り、全区間へ同じヒントを渡す。
        params.no_context = true;
        params.initial_prompt =
            request.initialPrompt.empty() ? nullptr : request.initialPrompt.c_str();
        params.carry_initial_prompt = !request.initialPrompt.empty();
        // 「(拍手)」「♪」など、発話でない注記を本文へ出さない。
        params.suppress_nst = true;
        CallbackState callback{&request, cancel};
        params.abort_callback = [](void* opaque) {
            return stopped(static_cast<CallbackState*>(opaque)->cancel);
        };
        params.abort_callback_user_data = &callback;
        params.progress_callback = [](whisper_context*, whisper_state*, int value, void* opaque) {
            auto& state = *static_cast<CallbackState*>(opaque);
            if (state.request->progress)
                state.request->progress(5 + value * 95 / 100);
        };
        params.progress_callback_user_data = &callback;
        const auto code =
            whisper_full(context.get(), params, samples.data(), static_cast<int>(samples.size()));
        if (stopped(cancel)) {
            result.cancelled = true;
            result.error = "文字起こしをキャンセルしました";
            return result;
        }
        if (code != 0) {
            result.error = "音声認識に失敗しました";
            return result;
        }
        const auto maxMs = static_cast<std::int64_t>(samples.size()) / 16;
        for (int i = 0; i < whisper_full_n_segments(context.get()); ++i) {
            Segment segment{whisper_full_get_segment_t0(context.get(), i) * 10,
                            whisper_full_get_segment_t1(context.get(), i) * 10,
                            whisper_full_get_segment_text(context.get(), i)};
            const auto begin = segment.text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) {
                result.error = "認識結果に空の本文があります";
                return result;
            }
            segment.text =
                segment.text.substr(begin, segment.text.find_last_not_of(" \t\r\n") - begin + 1);
            // Whisper は 30 秒の窓へ無音を足して処理するので、最後の区間の終端が素材の末尾を
            // 越えることがある (実測: 7850ms の素材で 8360ms)。終端だけを素材の末尾へ丸める。
            // 開始が素材の外にある区間や、前の区間と重なる区間は丸めずに拒否する。
            if (segment.startMs < maxMs)
                segment.endMs = std::min(segment.endMs, maxMs);
            if (segment.startMs < 0 || segment.endMs <= segment.startMs || segment.endMs > maxMs ||
                (!result.segments.empty() && segment.startMs < result.segments.back().endMs)) {
                result.error = "認識結果の時刻区間が不正です";
                return result;
            }
            result.segments.push_back(std::move(segment));
        }
        if (result.segments.empty()) {
            result.error = "音声認識の結果が0件でした";
            return result;
        }
        result.success = true;
    } catch (const ModelLoadCancelled&) {
        result.cancelled = true;
        result.error = "文字起こしをキャンセルしました";
    } catch (const std::exception&) {
        result.error = "認識エンジンの処理に失敗しました";
    }
    return result;
}
} // namespace mvm::transcribe
