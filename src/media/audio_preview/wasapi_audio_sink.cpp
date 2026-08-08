#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include "media/audio_preview/wasapi_audio_sink.h"

#include <algorithm>
#include <audioclient.h>
#include <avrt.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <windows.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace mvm::audio {
namespace {

template<class T>
void releaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::string hresultText(HRESULT result) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lx", static_cast<unsigned long>(result));
    return buffer;
}

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return format->wBitsPerSample == 32;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return false;
    const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    static const GUID ieeeFloat = {
        0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return IsEqualGUID(extended->SubFormat, ieeeFloat) && format->wBitsPerSample == 32;
}

AVSampleFormat sampleFormat(const WAVEFORMATEX* format) {
    if (isFloatFormat(format))
        return AV_SAMPLE_FMT_FLT;
    if (format->wBitsPerSample == 16)
        return AV_SAMPLE_FMT_S16;
    if (format->wBitsPerSample == 32)
        return AV_SAMPLE_FMT_S32;
    return AV_SAMPLE_FMT_NONE;
}

const char* sampleFormatName(AVSampleFormat format) {
    const char* name = av_get_sample_fmt_name(format);
    return name ? name : "unknown";
}
} // namespace

WasapiAudioSink::WasapiAudioSink(AudioFrameQueue& queue, AudioMasterClock& clock)
    : queue_(queue), clock_(clock) {}

WasapiAudioSink::~WasapiAudioSink() {
    stop();
}

bool WasapiAudioSink::open(std::string& error) {
    std::lock_guard lock(mutex_);
    if (metrics_.open) {
        error = "WASAPI endpoint は既に open されています";
        return false;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        error = "COM を初期化できません: " + hresultText(hr);
        return false;
    }
    comInitialized_ = SUCCEEDED(hr);
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                          reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        error = "既定 audio endpoint enumerator を作成できません: " + hresultText(hr);
        releaseDevice();
        return false;
    }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr) || !device_) {
        error = "既定 render endpoint を取得できません: " + hresultText(hr);
        releaseDevice();
        return false;
    }
    hr = device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || !client_) {
        error = "IAudioClient を取得できません: " + hresultText(hr);
        releaseDevice();
        return false;
    }
    hr = client_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) {
        error = "endpoint mix format を取得できません: " + hresultText(hr);
        releaseDevice();
        return false;
    }
    const AVSampleFormat deviceSampleFormat = sampleFormat(mixFormat_);
    if (deviceSampleFormat == AV_SAMPLE_FMT_NONE || mixFormat_->nChannels <= 0 ||
        mixFormat_->nSamplesPerSec == 0) {
        error = "endpoint mix format は未対応です（16/32-bit integer または float32 が必要）";
        releaseDevice();
        return false;
    }
    constexpr REFERENCE_TIME requestedDuration = 1000000; // shared event-driven 100 ms
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             requestedDuration, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        error = "WASAPI shared event-driven 初期化に失敗しました: " + hresultText(hr);
        releaseDevice();
        return false;
    }
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!audioEvent_ || !stopEvent_) {
        error = "audio render event を作成できません";
        releaseDevice();
        return false;
    }
    if (FAILED(hr = client_->SetEventHandle(static_cast<HANDLE>(audioEvent_))) ||
        FAILED(hr = client_->GetBufferSize(&bufferFrames_)) ||
        FAILED(hr = client_->GetService(IID_IAudioRenderClient,
                                        reinterpret_cast<void**>(&renderClient_))) ||
        FAILED(hr =
                   client_->GetService(IID_IAudioClock, reinterpret_cast<void**>(&deviceClock_)))) {
        error = "WASAPI render/clock service を初期化できません: " + hresultText(hr);
        releaseDevice();
        return false;
    }
    AVChannelLayout inputLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout outputLayout{};
    av_channel_layout_default(&outputLayout, mixFormat_->nChannels);
    int result = swr_alloc_set_opts2(&outputResampler_, &outputLayout, deviceSampleFormat,
                                     mixFormat_->nSamplesPerSec, &inputLayout, AV_SAMPLE_FMT_FLT,
                                     kInternalSampleRate, 0, nullptr);
    av_channel_layout_uninit(&outputLayout);
    if (result < 0 || !outputResampler_ || swr_init(outputResampler_) < 0) {
        error = "endpoint 用 resampler を初期化できません";
        releaseDevice();
        return false;
    }
    sourceScratchSamples_ =
        static_cast<int>(av_rescale_rnd(bufferFrames_, kInternalSampleRate,
                                        mixFormat_->nSamplesPerSec, AV_ROUND_UP)) +
        64;
    sourceScratch_.resize(static_cast<std::size_t>(sourceScratchSamples_) * kInternalChannels);
    metrics_.deviceFormat = AudioFormatInfo{static_cast<int>(mixFormat_->nSamplesPerSec),
                                            static_cast<int>(mixFormat_->nChannels),
                                            sampleFormatName(deviceSampleFormat)};
    metrics_.open = true;
    metrics_.joined = false;
    acceptingCommands_ = true;
    threadRunning_ = true;
    ResetEvent(static_cast<HANDLE>(stopEvent_));
    thread_ = std::thread(&WasapiAudioSink::renderLoop, this);
    return true;
}

bool WasapiAudioSink::play(std::int64_t mediaStartSample, SourceGeneration generation,
                           std::string& error) {
    if (!acceptingCommands_ || !metrics_.open) {
        error = "WASAPI endpoint は play を受理できません";
        return false;
    }
    if (!queue_.waitForSamples(kAudioPrerollSamples, kPrerollTimeoutMs)) {
        error = "固定 100 ms の audio pre-roll を 5000 ms 以内に満たせません";
        return false;
    }
    std::lock_guard lock(mutex_);
    if (playing_)
        return true;
    UINT64 devicePosition = 0;
    UINT64 qpcPosition = 0;
    UINT64 frequency = 0;
    HRESULT hr = deviceClock_->GetFrequency(&frequency);
    if (SUCCEEDED(hr))
        hr = deviceClock_->GetPosition(&devicePosition, &qpcPosition);
    if (FAILED(hr) || frequency == 0) {
        clock_.noteQueryFailure();
        error = "IAudioClock anchor を取得できません: " + hresultText(hr);
        ++metrics_.deviceFailureCount;
        return false;
    }
    if (!clock_.start({mediaStartSample, devicePosition, qpcNow(), frequency, generation})) {
        error = "audio clock anchor が無効です";
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    generation_ = generation.value;
    metrics_.playStartFirstConsumedSample = -1;
    hr = client_->Start();
    if (FAILED(hr)) {
        error = "WASAPI client を開始できません: " + hresultText(hr);
        ++metrics_.deviceFailureCount;
        clock_.pause();
        return false;
    }
    playing_ = true;
    metrics_.running = true;
    return true;
}

bool WasapiAudioSink::pause(std::string& error) {
    std::lock_guard lock(mutex_);
    if (!metrics_.open) {
        error = "WASAPI endpoint は open されていません";
        return false;
    }
    if (!playing_)
        return true;
    const HRESULT hr = client_->Stop();
    if (FAILED(hr)) {
        error = "WASAPI client を pause できません: " + hresultText(hr);
        ++metrics_.deviceFailureCount;
        return false;
    }
    playing_ = false;
    metrics_.running = false;
    clock_.pause();
    return true;
}

bool WasapiAudioSink::resetForSeek(std::string& error) {
    std::lock_guard lock(mutex_);
    if (playing_) {
        error = "seek reset は pause 後にだけ実行できます";
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    const HRESULT hr = client_->Reset();
    if (FAILED(hr)) {
        error = "WASAPI client buffer を seek 用に reset できません: " + hresultText(hr);
        ++metrics_.deviceFailureCount;
        return false;
    }
    swr_close(outputResampler_);
    if (swr_init(outputResampler_) < 0) {
        error = "seek 時に endpoint resampler を reset できません";
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    return true;
}

void WasapiAudioSink::stop() {
    acceptingCommands_ = false;
    if (client_)
        client_->Stop();
    playing_ = false;
    clock_.stop();
    if (stopEvent_)
        SetEvent(static_cast<HANDLE>(stopEvent_));
    if (thread_.joinable())
        thread_.join();
    threadRunning_ = false;
    {
        std::lock_guard lock(mutex_);
        metrics_.running = false;
        metrics_.joined = true;
    }
    releaseDevice();
}

void WasapiAudioSink::renderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE events[] = {static_cast<HANDLE>(stopEvent_), static_cast<HANDLE>(audioEvent_)};
    while (threadRunning_) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 1000);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait == WAIT_OBJECT_0 + 1 && playing_ && !renderAvailable())
            break;
        if (wait == WAIT_FAILED) {
            recordFailure("audio render event wait に失敗しました");
            break;
        }
    }
    if (avrt)
        AvRevertMmThreadCharacteristics(avrt);
    CoUninitialize();
}

bool WasapiAudioSink::renderAvailable() {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr) || padding > bufferFrames_) {
        recordFailure("WASAPI padding を取得できません: " + hresultText(hr));
        return false;
    }
    const UINT32 available = bufferFrames_ - padding;
    if (available == 0)
        return true;
    BYTE* deviceBuffer = nullptr;
    hr = renderClient_->GetBuffer(available, &deviceBuffer);
    if (FAILED(hr) || !deviceBuffer) {
        recordFailure("WASAPI render buffer を取得できません: " + hresultText(hr));
        return false;
    }
    const int sourceNeeded =
        std::min(sourceScratchSamples_,
                 static_cast<int>(av_rescale_rnd(available, kInternalSampleRate,
                                                 mixFormat_->nSamplesPerSec, AV_ROUND_UP)));
    std::fill(sourceScratch_.begin(),
              sourceScratch_.begin() + static_cast<std::size_t>(sourceNeeded) * kInternalChannels,
              0.0f);
    const SourceGeneration expected{generation_.load()};
    const AudioConsumeResult consumed =
        queue_.consume(sourceScratch_.data(), sourceNeeded, expected);
    if (consumed.audioSamples < sourceNeeded)
        queue_.noteUnderflow(sourceNeeded - consumed.audioSamples);
    const std::uint8_t* input[] = {reinterpret_cast<const std::uint8_t*>(sourceScratch_.data())};
    std::uint8_t* output[] = {deviceBuffer};
    const int converted =
        swr_convert(outputResampler_, output, static_cast<int>(available), input, sourceNeeded);
    if (converted < 0) {
        renderClient_->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
        recordFailure("endpoint PCM 変換に失敗しました");
        return false;
    }
    if (converted < static_cast<int>(available)) {
        const auto convertedFrames = static_cast<UINT32>(converted);
        std::memset(
            deviceBuffer + static_cast<std::size_t>(convertedFrames) * mixFormat_->nBlockAlign, 0,
            static_cast<std::size_t>(available - convertedFrames) * mixFormat_->nBlockAlign);
    }
    hr = renderClient_->ReleaseBuffer(available, 0);
    if (FAILED(hr)) {
        recordFailure("WASAPI render buffer を返却できません: " + hresultText(hr));
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        metrics_.audioRenderedSamples += static_cast<std::uint64_t>(consumed.audioSamples);
        if (metrics_.firstConsumedSample < 0 && consumed.firstSample >= 0)
            metrics_.firstConsumedSample = consumed.firstSample;
        if (metrics_.playStartFirstConsumedSample < 0 && consumed.firstSample >= 0)
            metrics_.playStartFirstConsumedSample = consumed.firstSample;
        if (consumed.lastSampleExclusive >= 0)
            metrics_.lastConsumedSampleExclusive = consumed.lastSampleExclusive;
    }
    UINT64 position = 0;
    UINT64 qpcPosition = 0;
    hr = deviceClock_->GetPosition(&position, &qpcPosition);
    if (FAILED(hr)) {
        clock_.noteQueryFailure();
        recordFailure("IAudioClock position を取得できません: " + hresultText(hr));
        return false;
    }
    clock_.update(position, static_cast<std::int64_t>(qpcPosition), expected);
    return true;
}

void WasapiAudioSink::recordFailure(const std::string& error) {
    std::lock_guard lock(mutex_);
    ++metrics_.deviceFailureCount;
    metrics_.lastError = error;
    playing_ = false;
    metrics_.running = false;
}

WasapiSnapshot WasapiAudioSink::snapshot() const {
    std::lock_guard lock(mutex_);
    return metrics_;
}

void WasapiAudioSink::releaseDevice() {
    if (thread_.joinable()) {
        std::lock_guard lock(mutex_);
        ++metrics_.audioDeviceReleaseBeforeJoin;
        return;
    }
    swr_free(&outputResampler_);
    releaseCom(deviceClock_);
    releaseCom(renderClient_);
    releaseCom(client_);
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
    releaseCom(device_);
    releaseCom(enumerator_);
    if (audioEvent_) {
        CloseHandle(static_cast<HANDLE>(audioEvent_));
        audioEvent_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    std::lock_guard lock(mutex_);
    metrics_.open = false;
}

} // namespace mvm::audio
