#include "capture/audio_capture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#endif

namespace hmi::capture
{
namespace
{
std::string failure(const char *operation, long code)
{
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lx)", operation,
                  static_cast<unsigned long>(code));
    return buffer;
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};

struct CaptureSession
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    Handle ready;
    std::string endpoint_id;
    int stream_channels = 1;

    ~CaptureSession() { if (client) client->Stop(); }

    HRESULT open(Format &native)
    {
        HRESULT status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
        if (FAILED(status)) return status;
        status = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (FAILED(status)) return status;
        LPWSTR endpoint = nullptr;
        if (SUCCEEDED(device->GetId(&endpoint)) && endpoint)
        {
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(endpoint, CoTaskMemFree);
            const int length = WideCharToMultiByte(CP_UTF8, 0, endpoint, -1, nullptr, 0, nullptr, nullptr);
            if (length > 1)
            {
                endpoint_id.resize(static_cast<std::size_t>(length));
                WideCharToMultiByte(CP_UTF8, 0, endpoint, -1, endpoint_id.data(), length, nullptr, nullptr);
                endpoint_id.pop_back();
            }
        }
        status = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                  nullptr, reinterpret_cast<void **>(client.GetAddressOf()));
        if (FAILED(status)) return status;
        WAVEFORMATEX *mix = nullptr;
        status = client->GetMixFormat(&mix);
        if (FAILED(status)) return status;
        native = Format{static_cast<int>(mix->nSamplesPerSec), static_cast<int>(mix->nChannels),
                        static_cast<int>(mix->wBitsPerSample), mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT};
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            constexpr GUID float_format{0x00000003, 0x0000, 0x0010,
                                         {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
            native.is_float = IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->SubFormat,
                                          float_format) != FALSE;
        }
        CoTaskMemFree(mix);

        // Stereo when the endpoint offers it: the dongle sends two microphone
        // channels, and fusing here is the only way to pick the better one.
        // Mono-only endpoints keep the historical single-channel path.
        const int wanted = native.channels >= 2 ? 2 : 1;
        const int attempt[2] = {wanted, 1};
        const int attempts = wanted == 2 ? 2 : 1;
        stream_channels = 1;
        HRESULT init_status = E_FAIL;
        for (int i = 0; i < attempts; ++i)
        {
            const int channels = attempt[i];
            WAVEFORMATEX stream{};
            stream.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            stream.nChannels = static_cast<WORD>(channels);
            stream.nSamplesPerSec = 16000;
            stream.wBitsPerSample = 32;
            stream.nBlockAlign = static_cast<WORD>(4 * channels);
            stream.nAvgBytesPerSec = 16000u * static_cast<DWORD>(stream.nBlockAlign);
            init_status = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &stream, nullptr);
            if (SUCCEEDED(init_status)) { stream_channels = channels; break; }
        }
        if (FAILED(init_status)) return init_status;
        ready.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ready.value) return HRESULT_FROM_WIN32(GetLastError());
        status = client->SetEventHandle(ready.value);
        if (FAILED(status)) return status;
        status = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(status)) return status;
        return client->Start();
    }
};
#endif
}

struct AudioCapture::Impl
{
    std::atomic<bool> active{false};
    std::atomic<float> rms{0.0f};
    std::atomic<int> fusion{0};
    std::atomic<int> stream_channels{1};
    mutable std::mutex format_mutex;
    Format native;
    mutable std::mutex statistics_mutex;
    CaptureStatistics diagnostic;
    void note_error(std::string detail)
    {
        std::lock_guard lock(statistics_mutex);
        diagnostic.last_error = std::move(detail);
    }
    std::thread worker;
#ifdef _WIN32
    Handle stop_event;

    void run(BlockSink sink, std::promise<std::string> initialized)
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com))
        {
            note_error(failure("CoInitializeEx", com));
            initialized.set_value(failure("CoInitializeEx", com));
            return;
        }
        bool announced = false;
        std::int64_t frame_cursor = 0;
        try
        {
            // A bounded one-second staging ring assembles exact 10 ms sink blocks.
            std::array<float, 16000> ring{};
            std::array<float, 32000> channel_ring{};
            std::array<float, 160> block{};
            std::array<float, 320> channel_block{};
            std::size_t read = 0;
            std::size_t write = 0;
            std::size_t queued = 0;
            /* After the shortcut is released keep draining for a short tail so
             * the last phoneme is not truncated by the stop edge. */
            std::uint64_t stop_drain_until_ms = 0;
            const auto stop_drain_expired = [&] {
                if (WaitForSingleObject(stop_event.value, 0) != WAIT_OBJECT_0) return false;
                const std::uint64_t now_ms = GetTickCount64();
                if (stop_drain_until_ms == 0) stop_drain_until_ms = now_ms + 150;
                return now_ms >= stop_drain_until_ms;
            };
            while (WaitForSingleObject(stop_event.value, 0) != WAIT_OBJECT_0)
            {
                CaptureSession session;
                Format observed;
                {
                    std::lock_guard lock(statistics_mutex);
                    ++diagnostic.open_attempts;
                }
                HRESULT status = session.open(observed);
                if (FAILED(status))
                {
                    note_error(failure("WASAPI capture open", status));
                    if (!announced)
                    {
                        initialized.set_value(failure("WASAPI capture initialization", status));
                        announced = true;
                        break;
                    }
                    active = false;
                    rms = 0.0f;
                    if (WaitForSingleObject(stop_event.value, 500) == WAIT_OBJECT_0) break;
                    continue;
                }
                {
                    std::lock_guard lock(format_mutex);
                    native = observed;
                }
                stream_channels.store(session.stream_channels);
                {
                    std::lock_guard lock(statistics_mutex);
                    diagnostic.endpoint_id = session.endpoint_id;
                    if (announced) ++diagnostic.reconnects;
                }
                active = true;
                if (!announced)
                {
                    initialized.set_value({});
                    announced = true;
                }
                read = write = queued = 0;
                double energy_left = 0.0;
                double energy_right = 0.0;
                int best_channel = 0;
                const HANDLE events[]{stop_event.value, session.ready.value};
                bool reconnect = false;
                while (!reconnect)
                {
                    const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 2000);
                    if (wait == WAIT_OBJECT_0) break;
                    if (wait == WAIT_FAILED)
                    {
                        note_error(failure("WASAPI capture wait", static_cast<long>(GetLastError())));
                        break;
                    }
                    // A timeout also probes the client, detecting removed endpoints.
                    UINT32 packet = 0;
                    status = session.capture->GetNextPacketSize(&packet);
                    while (SUCCEEDED(status) && packet != 0)
                    {
                        BYTE *bytes = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        status = session.capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr);
                        if (FAILED(status)) break;
                        {
                            std::lock_guard lock(statistics_mutex);
                            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++diagnostic.discontinuities;
                            if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++diagnostic.timestamp_errors;
                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) diagnostic.silent_frames += frames;
                        }
                        double energy = 0.0;
                        const float *samples = reinterpret_cast<const float *>(bytes);
                        const int channels = session.stream_channels;
                        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                        for (UINT32 index = 0; index < frames; ++index)
                        {
                            const auto pick = [&](int channel) {
                                if (silent || !samples) return 0.0f;
                                float value = samples[index * channels + channel];
                                if (!std::isfinite(value)) value = 0.0f;
                                return std::clamp(value, -1.0f, 1.0f);
                            };
                            float sample = 0.0f;
                            switch (static_cast<Fusion>(fusion.load()))
                            {
                            case Fusion::Left: sample = pick(0); break;
                            case Fusion::Right: sample = pick(channels > 1 ? 1 : 0); break;
                            case Fusion::Best:
                                if (channels > 1)
                                {
                                    const float left = pick(0);
                                    const float right = pick(1);
                                    energy_left += static_cast<double>(left) * left;
                                    energy_right += static_cast<double>(right) * right;
                                    sample = best_channel == 0 ? left : right;
                                }
                                else
                                {
                                    sample = pick(0);
                                }
                                break;
                            default:
                                sample = channels > 1 ? 0.5f * (pick(0) + pick(1)) : pick(0);
                                break;
                            }
                            energy += static_cast<double>(sample) * sample;
                            ring[write] = sample;
                            channel_ring[write * 2] = pick(0);
                            channel_ring[write * 2 + 1] = channels > 1 ? pick(1) : 0.0f;
                            write = (write + 1) % ring.size();
                            ++queued;
                            if (queued == block.size())
                            {
                                for (std::size_t offset = 0; offset < block.size(); ++offset)
                                {
                                    block[offset] = ring[read];
                                    for (int channel = 0; channel < channels; ++channel)
                                        channel_block[offset * static_cast<std::size_t>(channels) + channel] =
                                            channel_ring[read * 2 + channel];
                                    read = (read + 1) % ring.size();
                                }
                                queued = 0;
                                if (static_cast<Fusion>(fusion.load()) == Fusion::Best && channels > 1)
                                {
                                    // +1.5 dB hysteresis keeps a tie from flapping.
                                    if (best_channel == 0 && energy_right > energy_left * 1.41) best_channel = 1;
                                    else if (best_channel == 1 && energy_left > energy_right * 1.41) best_channel = 0;
                                    energy_left = 0.0;
                                    energy_right = 0.0;
                                }
                                // The caller queues this block; no inference belongs here.
                                sink(AudioBlock{block.data(), channel_block.data(), block.size(), channels, frame_cursor});
                                frame_cursor += static_cast<std::int64_t>(block.size());
                                {
                                    std::lock_guard lock(statistics_mutex);
                                    diagnostic.delivered_frames += block.size();
                                    ++diagnostic.delivered_chunks;
                                }
                            }
                        }
                        rms = frames ? static_cast<float>(std::sqrt(energy / frames)) : 0.0f;
                        status = session.capture->ReleaseBuffer(frames);
                        if (FAILED(status)) break;
                        status = session.capture->GetNextPacketSize(&packet);
                    }
                    reconnect = FAILED(status);
                    if (reconnect) note_error(failure("WASAPI capture read", status));
                    if (stop_drain_expired()) break;
                }
                active = false;
                rms = 0.0f;
                {
                    std::lock_guard lock(statistics_mutex);
                    diagnostic.dropped_partial_frames += queued;
                }
                if (WaitForSingleObject(stop_event.value, 0) == WAIT_OBJECT_0) break;
                // Drop a partial block after disconnect instead of mixing endpoints.
                if (WaitForSingleObject(stop_event.value, 250) == WAIT_OBJECT_0) break;
            }
        }
        catch (const std::exception &exception)
        {
            note_error(exception.what());
            if (!announced) initialized.set_value(exception.what());
        }
        catch (...)
        {
            note_error("capture worker failed");
            if (!announced) initialized.set_value("capture worker failed");
        }
        active = false;
        rms = 0.0f;
        CoUninitialize();
    }
#endif
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::start(Sink sink, std::string *error)
{
    return start(std::move(sink), error, Fusion::Mean);
}

bool AudioCapture::start(Sink sink, std::string *error, Fusion fusion_mode)
{
    if (!sink)
    {
        stop();
        if (error) *error = "audio capture requires a sink";
        return false;
    }
    return start_blocks([sink = std::move(sink)](const AudioBlock &block) {
        sink(block.mono, block.frames, block.start_frame);
    }, error, fusion_mode);
}

bool AudioCapture::start_blocks(BlockSink sink, std::string *error, Fusion fusion_mode)
{
    stop();
    impl_->fusion.store(static_cast<int>(fusion_mode));
    { std::lock_guard lock(impl_->statistics_mutex); impl_->diagnostic = {}; }
    if (error) error->clear();
    if (!sink)
    {
        if (error) *error = "audio capture requires a sink";
        return false;
    }
#ifdef _WIN32
    impl_->stop_event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stop_event.value)
    {
        if (error) *error = failure("CreateEventW", static_cast<long>(GetLastError()));
        return false;
    }
    std::promise<std::string> initialized;
    auto result = initialized.get_future();
    try
    {
        impl_->worker = std::thread(&Impl::run, impl_.get(), std::move(sink), std::move(initialized));
        const std::string detail = result.get();
        if (detail.empty()) return true;
        if (error) *error = detail;
    }
    catch (const std::exception &exception)
    {
        if (error) *error = exception.what();
    }
    stop();
#else
    if (error) *error = "WASAPI capture requires Windows";
#endif
    return false;
}

void AudioCapture::stop()
{
#ifdef _WIN32
    if (impl_->stop_event.value) SetEvent(impl_->stop_event.value);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->stop_event.value)
    {
        CloseHandle(impl_->stop_event.value);
        impl_->stop_event.value = nullptr;
    }
#endif
    impl_->active = false;
    impl_->rms = 0.0f;
}

bool AudioCapture::running() const noexcept { return impl_->active.load(); }

Fusion AudioCapture::fusion() const noexcept
{
    return static_cast<Fusion>(impl_->fusion.load());
}

int AudioCapture::stream_channels() const noexcept { return impl_->stream_channels.load(); }

Format AudioCapture::device_format() const noexcept
{
    std::lock_guard lock(impl_->format_mutex);
    return impl_->native;
}

Format AudioCapture::stream_format() const noexcept { return Format{16000, 1, 32, true}; }
float AudioCapture::level() const noexcept { return impl_->rms.load(); }

CaptureStatistics AudioCapture::statistics() const
{
    std::lock_guard lock(impl_->statistics_mutex);
    return impl_->diagnostic;
}
}
