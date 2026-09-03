#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include "media/audio_preview/wasapi_audio_sink.h"

#include <windows.h>
#include <algorithm>
#include <audioclient.h>
#include <avrt.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>
#include <mmdeviceapi.h>

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

std::int64_t currentQpc() {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}
} // namespace

WasapiAudioSink::WasapiAudioSink(AudioFrameQueue& queue, AudioMasterClock& clock)
    : queue_(queue), clock_(clock) {}

WasapiAudioSink::~WasapiAudioSink() {
    stop();
}

bool WasapiAudioSink::open(std::string& error, float sessionVolume) {
    // caller error は COM/endpoint に触る前に弾く。
    if (!(sessionVolume >= 0.0F) || sessionVolume > 1.0F) {
        error = "session volume は 0.0〜1.0 の範囲で指定してください";
        return false;
    }
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
        releaseDeviceLocked();
        return false;
    }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr) || !device_) {
        error = "既定 render endpoint を取得できません: " + hresultText(hr);
        releaseDeviceLocked();
        return false;
    }
    hr = device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || !client_) {
        error = "IAudioClient を取得できません: " + hresultText(hr);
        releaseDeviceLocked();
        return false;
    }
    hr = client_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) {
        error = "endpoint mix format を取得できません: " + hresultText(hr);
        releaseDeviceLocked();
        return false;
    }
    const AVSampleFormat deviceSampleFormat = sampleFormat(mixFormat_);
    if (deviceSampleFormat == AV_SAMPLE_FMT_NONE || mixFormat_->nChannels <= 0 ||
        mixFormat_->nSamplesPerSec == 0) {
        error = "endpoint mix format は未対応です（16/32-bit integer または float32 が必要）";
        releaseDeviceLocked();
        return false;
    }
    constexpr REFERENCE_TIME requestedDuration = 1000000; // shared event-driven 100 ms
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             requestedDuration, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        error = "WASAPI shared event-driven 初期化に失敗しました: " + hresultText(hr);
        releaseDeviceLocked();
        return false;
    }
    if (sessionVolume != 1.0F) {
        ISimpleAudioVolume* sessionVolumeControl = nullptr;
        hr = client_->GetService(IID_ISimpleAudioVolume,
                                 reinterpret_cast<void**>(&sessionVolumeControl));
        if (FAILED(hr) || !sessionVolumeControl) {
            error = "endpoint session volume を取得できません: " + hresultText(hr);
            releaseDeviceLocked();
            return false;
        }
        hr = sessionVolumeControl->SetMasterVolume(sessionVolume, nullptr);
        releaseCom(sessionVolumeControl);
        if (FAILED(hr)) {
            // 適用できないまま全音量で再生しない。
            error = "endpoint session volume を設定できません: " + hresultText(hr);
            releaseDeviceLocked();
            return false;
        }
        // open() 入口で mutex_ を保持済みのため、ここで再取得しない。
        metrics_.sessionVolume = sessionVolume;
    }
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!audioEvent_ || !stopEvent_) {
        error = "audio render event を作成できません";
        releaseDeviceLocked();
        return false;
    }
    if (FAILED(hr = client_->SetEventHandle(static_cast<HANDLE>(audioEvent_))) ||
        FAILED(hr = client_->GetBufferSize(&bufferFrames_)) ||
        FAILED(hr = client_->GetService(IID_IAudioRenderClient,
                                        reinterpret_cast<void**>(&renderClient_))) ||
        FAILED(hr =
                   client_->GetService(IID_IAudioClock, reinterpret_cast<void**>(&deviceClock_)))) {
        error = "WASAPI render/clock service を初期化できません: " + hresultText(hr);
        releaseDeviceLocked();
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
        releaseDeviceLocked();
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
    metrics_.endpointBufferFrames = bufferFrames_;
    metrics_.open = true;
    metrics_.joined = false;
    acceptingCommands_ = true;
    threadRunning_ = true;
    endpointPrefillRequired_ = true;
    ResetEvent(static_cast<HANDLE>(stopEvent_));
    thread_ = std::thread(&WasapiAudioSink::renderLoop, this);
    return true;
}

bool WasapiAudioSink::play(std::int64_t mediaStartSample, SourceGeneration generation,
                           std::string& error) {
    if (playFaultInjected_.load(std::memory_order_acquire)) {
        error = "injected WASAPI play fault (test)";
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (!acceptingCommands_ || !metrics_.open) {
            error = "WASAPI endpoint は play を受理できません";
            return false;
        }
    }
    {
        // barrier は pre-roll の前に入る。ここで止めている間に shutdown を
        // 走らせることで、interleaving を決定論的に再現できる。
        std::unique_lock barrierLock(barrierMutex_);
        if (playBarrierArmed_) {
            playBarrierEntered_ = true;
            barrierChanged_.notify_all();
            barrierChanged_.wait(barrierLock, [this] { return !playBarrierArmed_; });
        }
    }
    if (!queue_.waitForSamples(kAudioPrerollSamples, kPrerollTimeoutMs)) {
        error = "固定 100 ms の audio pre-roll を 5000 ms 以内に満たせません";
        return false;
    }
    if (playing_)
        return true;
    std::lock_guard clientLock(clientMutex_);
    {
        // pre-roll 待ちの間に stop() が走り得る。clientMutex_ を取った後に
        // 再検査しないと、停止済み endpoint を再生状態へ戻してしまう。
        std::lock_guard lock(mutex_);
        if (!acceptingCommands_ || !metrics_.open) {
            error = "WASAPI endpoint は play を受理できません (stop 済み)";
            return false;
        }
    }
    const bool didPrefill = endpointPrefillRequired_;
    if (endpointPrefillRequired_) {
        if (!prefillEndpoint(mediaStartSample, generation, error)) {
            std::lock_guard lock(mutex_);
            ++metrics_.audioLifecycleViolation;
            return false;
        }
        endpointPrefillRequired_ = false;
    } else {
        std::lock_guard lock(mutex_);
        metrics_.playStartFirstConsumedSample = -1;
    }
    UINT64 devicePosition = 0;
    UINT64 qpcPosition = 0;
    UINT64 frequency = 0;
    HRESULT hr = deviceClock_->GetFrequency(&frequency);
    if (SUCCEEDED(hr))
        hr = deviceClock_->GetPosition(&devicePosition, &qpcPosition);
    if (FAILED(hr) || frequency == 0) {
        clock_.noteQueryFailure();
        error = "IAudioClock anchor を取得できません: " + hresultText(hr);
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        return false;
    }
    if (!clock_.start(
            {mediaStartSample, devicePosition, Qpc100ns{qpcPosition}, frequency, generation})) {
        error = "audio clock anchor が無効です";
        std::lock_guard lock(mutex_);
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    generation_ = generation.value;
    hr = client_->Start();
    if (FAILED(hr)) {
        error = "WASAPI client を開始できません: " + hresultText(hr);
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        clock_.pause();
        return false;
    }
    playing_ = true;
    if (attribution_)
        attribution_->context.audioStartQpc.store(currentQpc(), std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        if (didPrefill) {
            metrics_.endpointStartDevicePosition = devicePosition;
            metrics_.clockAnchorMediaSample = mediaStartSample;
            metrics_.clockAnchorDevicePosition = devicePosition;
        }
        metrics_.running = true;
    }
    return true;
}

bool WasapiAudioSink::prefillEndpoint(std::int64_t mediaStartSample, SourceGeneration generation,
                                      std::string& error) {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr) || padding != 0) {
        error = "endpoint prefill 前の buffer が空ではありません: " + hresultText(hr);
        return false;
    }
    BYTE* deviceBuffer = nullptr;
    hr = renderClient_->GetBuffer(bufferFrames_, &deviceBuffer);
    if (FAILED(hr) || !deviceBuffer) {
        error = "endpoint prefill buffer を取得できません: " + hresultText(hr);
        return false;
    }
    const int sourceNeeded =
        std::min(sourceScratchSamples_,
                 static_cast<int>(av_rescale_rnd(bufferFrames_, kInternalSampleRate,
                                                 mixFormat_->nSamplesPerSec, AV_ROUND_UP)));
    const AudioConsumeResult consumed =
        queue_.consume(sourceScratch_.data(), mediaStartSample, sourceNeeded, generation);
    if (consumed.audioSamples != sourceNeeded || consumed.firstSample != mediaStartSample) {
        renderClient_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        error = "endpoint prefill は requested media sample 由来の PCM を満たせません";
        return false;
    }
    updateMeterPeaks(consumed.audioSamples);
    const std::uint8_t* input[] = {reinterpret_cast<const std::uint8_t*>(sourceScratch_.data())};
    std::uint8_t* output[] = {deviceBuffer};
    const int converted =
        swr_convert(outputResampler_, output, static_cast<int>(bufferFrames_), input, sourceNeeded);
    if (converted != static_cast<int>(bufferFrames_)) {
        renderClient_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        error = "endpoint prefill の PCM 変換 frame 数が一致しません";
        return false;
    }
    hr = renderClient_->ReleaseBuffer(bufferFrames_, 0);
    if (FAILED(hr)) {
        error = "endpoint prefill buffer を返却できません: " + hresultText(hr);
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        metrics_.endpointPrefillFrames = bufferFrames_;
        metrics_.endpointFirstMediaSample = consumed.firstSample;
        metrics_.playStartFirstConsumedSample = consumed.firstSample;
        metrics_.audioRenderedSamples += static_cast<std::uint64_t>(consumed.audioSamples);
        if (metrics_.firstConsumedSample < 0)
            metrics_.firstConsumedSample = consumed.firstSample;
        metrics_.lastConsumedSampleExclusive = consumed.lastSampleExclusive;
    }
    nextRequestedSample_ = consumed.lastSampleExclusive;
    return true;
}

bool WasapiAudioSink::pause(std::string& error) {
    std::lock_guard clientLock(clientMutex_);
    clearMeterPeaks();
    {
        std::lock_guard lock(mutex_);
        if (!metrics_.open) {
            error = "WASAPI endpoint は open されていません";
            return false;
        }
    }
    if (pauseFaultInjected_.load(std::memory_order_acquire)) {
        error = "injected WASAPI pause fault (test)";
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        return false;
    }
    if (!playing_)
        return true;
    UINT64 devicePosition = 0;
    UINT64 qpcPosition = 0;
    HRESULT hr = deviceClock_->GetPosition(&devicePosition, &qpcPosition);
    const SourceGeneration expected{generation_.load()};
    if (FAILED(hr)) {
        clock_.noteQueryFailure();
        error = "pause 直前の IAudioClock position を取得できません: " + hresultText(hr);
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        return false;
    }
    if (!clock_.update(devicePosition, Qpc100ns{qpcPosition}, expected)) {
        error = "pause 直前の IAudioClock position を media sample へ写像できません";
        std::lock_guard lock(mutex_);
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    hr = client_->Stop();
    if (FAILED(hr)) {
        error = "WASAPI client を pause できません: " + hresultText(hr);
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        return false;
    }
    playing_ = false;
    {
        std::lock_guard lock(mutex_);
        metrics_.running = false;
    }
    clock_.pause();
    return true;
}

bool WasapiAudioSink::resetForSeek(std::string& error) {
    std::lock_guard clientLock(clientMutex_);
    // reset 後は render event が来ない可能性がある。減衰待ちにせずここで 0 にする。
    clearMeterPeaks();
    if (playing_) {
        error = "seek reset は pause 後にだけ実行できます";
        std::lock_guard lock(mutex_);
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    const HRESULT hr = client_->Reset();
    if (FAILED(hr)) {
        error = "WASAPI client buffer を seek 用に reset できません: " + hresultText(hr);
        std::lock_guard lock(mutex_);
        ++metrics_.deviceFailureCount;
        return false;
    }
    swr_close(outputResampler_);
    if (swr_init(outputResampler_) < 0) {
        error = "seek 時に endpoint resampler を reset できません";
        std::lock_guard lock(mutex_);
        ++metrics_.audioLifecycleViolation;
        return false;
    }
    endpointPrefillRequired_ = true;
    recentConsumeTrace_ = {};
    recentConsumeTraceCount_ = 0;
    recentConsumeTraceNext_ = 0;
    {
        std::lock_guard lock(mutex_);
        metrics_.endpointPrefillFrames = 0;
        metrics_.endpointFirstMediaSample = -1;
    }
    return true;
}

void WasapiAudioSink::stop() {
    acceptingCommands_ = false;
    // stop 後は render loop が回らないので、最後の peak を凍らせない。
    clearMeterPeaks();
    {
        std::lock_guard clientLock(clientMutex_);
        if (client_)
            client_->Stop();
        playing_ = false;
    }
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

void WasapiAudioSink::clearMeterPeaks() {
    meterPeakLeft_.store(0.0F, std::memory_order_relaxed);
    meterPeakRight_.store(0.0F, std::memory_order_relaxed);
}

void WasapiAudioSink::updateMeterPeaks(std::int64_t consumedFrames) {
    // sourceScratch_ は internal domain (48kHz / stereo / float32) の interleaved PCM。
    // 実際に endpoint へ送る PCM をそのまま測る。resample 後の device format は測らない。
    float blockLeft = 0.0F;
    float blockRight = 0.0F;
    for (std::int64_t frame = 0; frame < consumedFrames; ++frame) {
        const auto base = static_cast<std::size_t>(frame) * kInternalChannels;
        blockLeft = std::max(blockLeft, std::fabs(sourceScratch_[base]));
        blockRight = std::max(blockRight, std::fabs(sourceScratch_[base + 1]));
    }
    const float decayedLeft =
        meterPeakLeft_.load(std::memory_order_relaxed) * kMeterPeakDecayPerBlock;
    const float decayedRight =
        meterPeakRight_.load(std::memory_order_relaxed) * kMeterPeakDecayPerBlock;
    meterPeakLeft_.store(std::max(blockLeft, decayedLeft), std::memory_order_relaxed);
    meterPeakRight_.store(std::max(blockRight, decayedRight), std::memory_order_relaxed);
}

bool WasapiAudioSink::renderAvailable() {
    std::lock_guard clientLock(clientMutex_);
    // event 判定後に pause/reset が clientMutex_ を先に取得した場合、古い
    // playing=true を見た callback が reset 後へ PCM を書かないよう再検査する。
    if (!playing_) {
        // 停止中は meter を凍結させず 0 へ落とす。止めた直後の値が残らないようにする。
        updateMeterPeaks(0);
        return true;
    }
    if (renderFaultInjected_.load(std::memory_order_acquire)) {
        // 実際の WASAPI 失敗と同じ経路で停止させる。
        recordFailure("injected WASAPI render fault (test)");
        return false;
    }
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
    const std::int64_t requestedSampleStart = nextRequestedSample_;
    const AudioConsumeResult consumed =
        queue_.consume(sourceScratch_.data(), requestedSampleStart, sourceNeeded, expected);
    const AudioConsumeTraceEntry trace{currentQpc(),
                                       requestedSampleStart,
                                       sourceNeeded,
                                       consumed.queuedSamplesBeforeConsume,
                                       consumed.audioSamples,
                                       consumed.queuedSamplesAfterConsume,
                                       consumed.queueLastAvailableSampleExclusive,
                                       expected.value};
    recentConsumeTrace_[recentConsumeTraceNext_] = trace;
    recentConsumeTraceNext_ = (recentConsumeTraceNext_ + 1) % kAudioConsumeTraceCapacity;
    recentConsumeTraceCount_ = std::min(recentConsumeTraceCount_ + 1, kAudioConsumeTraceCapacity);
    nextRequestedSample_ = requestedSampleStart < 0 ? -1 : requestedSampleStart + sourceNeeded;
    if (consumed.shortageKind == AudioShortageKind::Starvation) {
        queue_.noteUnderflow(sourceNeeded - consumed.audioSamples);
        if (attribution_) {
            const auto clock = clock_.snapshot();
            AudioUnderflowFirstSnapshot snapshot;
            snapshot.occurrenceQpc = trace.occurrenceQpc;
            snapshot.context = attribution_->context.snapshot();
            snapshot.requestedSampleStart = requestedSampleStart;
            snapshot.requestedSampleCount = sourceNeeded;
            snapshot.queuedSamplesBeforeConsume = consumed.queuedSamplesBeforeConsume;
            snapshot.actuallyConsumedSamples = consumed.audioSamples;
            snapshot.queuedSamplesAfterConsume = consumed.queuedSamplesAfterConsume;
            snapshot.sourceGeneration = expected.value;
            snapshot.audioMasterSamplePosition = clock.mediaSamplePosition;
            snapshot.queueLastAvailableSampleExclusive = consumed.queueLastAvailableSampleExclusive;
            snapshot.endpointBufferFrames = bufferFrames_;
            snapshot.consumeTraceCount = recentConsumeTraceCount_;
            const std::size_t first =
                (recentConsumeTraceNext_ + kAudioConsumeTraceCapacity - recentConsumeTraceCount_) %
                kAudioConsumeTraceCapacity;
            for (std::size_t index = 0; index < recentConsumeTraceCount_; ++index)
                snapshot.consumeTrace[index] =
                    recentConsumeTrace_[(first + index) % kAudioConsumeTraceCapacity];
            attribution_->firstAudioUnderflow.capture(snapshot);
        }
    }
    updateMeterPeaks(consumed.audioSamples);
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
    clock_.update(position, Qpc100ns{qpcPosition}, expected);
    return true;
}

void WasapiAudioSink::injectRenderFaultForTest() {
    renderFaultInjected_.store(true, std::memory_order_release);
}

void WasapiAudioSink::armPlayBarrierForTest() {
    std::lock_guard lock(barrierMutex_);
    playBarrierArmed_ = true;
    playBarrierEntered_ = false;
}

bool WasapiAudioSink::waitPlayBarrierEnteredForTest(int timeoutMs) {
    std::unique_lock lock(barrierMutex_);
    return barrierChanged_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                    [this] { return playBarrierEntered_; });
}

void WasapiAudioSink::releasePlayBarrierForTest() {
    {
        std::lock_guard lock(barrierMutex_);
        playBarrierArmed_ = false;
    }
    barrierChanged_.notify_all();
}

void WasapiAudioSink::injectPlayFaultForTest() {
    playFaultInjected_.store(true, std::memory_order_release);
}

void WasapiAudioSink::injectPauseFaultForTest() {
    pauseFaultInjected_.store(true, std::memory_order_release);
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
    WasapiSnapshot result = metrics_;
    result.meterPeakLeft = meterPeakLeft_.load(std::memory_order_relaxed);
    result.meterPeakRight = meterPeakRight_.load(std::memory_order_relaxed);
    return result;
}

void WasapiAudioSink::releaseDevice() {
    std::lock_guard lock(mutex_);
    releaseDeviceLocked();
}

// mutex_ は呼び出し側が保持している。ここで再取得すると非再帰 mutex で
// deadlock するため、この関数の中では絶対に mutex_ を取らない。
void WasapiAudioSink::releaseDeviceLocked() {
    if (thread_.joinable()) {
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
    metrics_.open = false;
}

} // namespace mvm::audio
