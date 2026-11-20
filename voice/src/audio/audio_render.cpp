#include "audio/audio_render.hpp"
#include "audio/stream_playback.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <chrono>
#include <deque>

namespace hmi::audio
{
namespace
{
using Microsoft::WRL::ComPtr;

struct ComScope
{
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
};

struct EventHandle
{
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~EventHandle() { if (value) CloseHandle(value); }
};

struct RenderBuffer
{
    IAudioRenderClient *renderer;
    UINT32 frames;
    bool pending = true;

    ~RenderBuffer()
    {
        if (pending) renderer->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT commit()
    {
        pending = false;
        return renderer->ReleaseBuffer(frames, 0);
    }
};

bool failed(HRESULT status, const char *operation, std::string *error)
{
    if (SUCCEEDED(status))
        return false;
    if (error)
    {
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), " (HRESULT 0x%08lX)", static_cast<unsigned long>(status));
        *error = std::string(operation) + detail;
    }
    return true;
}

bool reject(const char *detail, std::string *error)
{
    if (error) *error = detail;
    return false;
}

bool interrupted(const std::function<bool()> &cancelled, std::string *error)
{
    if (!cancelled || !cancelled()) return false;
    reject("playback interrupted", error);
    return true;
}

bool validate_chunk(const AudioBuffer &audio, int expected_rate, int expected_channels,
                    std::size_t &accepted_frames, std::string *error)
{
    if (audio.samples.empty() || audio.sample_rate < 8000 || audio.sample_rate > 192000 ||
        audio.channels < 1 || audio.channels > 8 ||
        audio.samples.size() % static_cast<std::size_t>(audio.channels) != 0)
        return reject("invalid playback PCM format", error);
    if ((expected_rate != 0 && audio.sample_rate != expected_rate) ||
        (expected_channels != 0 && audio.channels != expected_channels))
        return reject("playback stream PCM format changed", error);
    const auto frames = audio.samples.size() / static_cast<std::size_t>(audio.channels);
    const auto limit = static_cast<std::size_t>(audio.sample_rate) * 300;
    if (accepted_frames > limit || frames > limit - accepted_frames)
        return reject("playback stream exceeds 300 seconds", error);
    if (!std::all_of(audio.samples.begin(), audio.samples.end(), [](float value) { return std::isfinite(value); }))
        return reject("playback PCM contains non-finite samples", error);
    accepted_frames += frames;
    return true;
}

detail::StreamRead read_chunk(const detail::ReadAudio &read, AudioBuffer &audio,
                              int expected_rate, int expected_channels,
                              std::size_t &accepted_frames, std::string *error)
{
    const auto state = read(audio, error);
    switch (state)
    {
    case detail::StreamRead::Ready:
        return validate_chunk(audio, expected_rate, expected_channels, accepted_frames, error) ?
            state : detail::StreamRead::Failed;
    case detail::StreamRead::Pending:
    case detail::StreamRead::Finished:
        return state;
    case detail::StreamRead::Failed:
        if (error && error->empty()) *error = "playback audio source failed";
        return state;
    }
    reject("invalid playback stream state", error);
    return detail::StreamRead::Failed;
}

bool play_stream_impl(const detail::ReadAudio &read, std::string *error,
                       const std::function<bool()> &cancelled, PlaybackStatistics &statistics)
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto elapsed = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    AudioBuffer audio;
    std::size_t accepted_frames = 0;
    for (;;)
    {
        if (interrupted(cancelled, error)) return false;
        const auto state = read_chunk(read, audio, 0, 0, accepted_frames, error);
        if (state == detail::StreamRead::Ready) break;
        if (state == detail::StreamRead::Failed) { statistics.finish_reason = "source_failed"; return false; }
        if (state == detail::StreamRead::Finished) return reject("playback stream contains no PCM", error);
        ++statistics.pending_reads;
        const auto waiting = Clock::now();
        Sleep(10);
        statistics.source_wait_ms += elapsed(waiting);
    }
    if (interrupted(cancelled, error)) return false;
    const int sample_rate = audio.sample_rate;
    const int channels = audio.channels;
    statistics.sample_rate = sample_rate;
    statistics.channels = channels;
    ComScope com;
    if (failed(com.status, "initialize playback COM", error)) return false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> renderer;
    if (failed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)), "create playback enumerator", error) ||
        failed(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint),
               "find default speaker", error) ||
        failed(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(client.GetAddressOf())),
               "activate speaker", error)) return false;

    LPWSTR endpoint_name = nullptr;
    if (SUCCEEDED(endpoint->GetId(&endpoint_name)) && endpoint_name)
    {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(endpoint_name, CoTaskMemFree);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, endpoint_name, -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1)
        {
            statistics.endpoint_id.resize(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(CP_UTF8, 0, endpoint_name, -1, statistics.endpoint_id.data(), bytes, nullptr, nullptr);
            statistics.endpoint_id.pop_back();
        }
    }
    WAVEFORMATEX *mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix)
    {
        statistics.mix_sample_rate = static_cast<int>(mix->nSamplesPerSec);
        statistics.mix_channels = static_cast<int>(mix->nChannels);
        CoTaskMemFree(mix);
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(channels * 4);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (failed(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &format, nullptr),
               "initialize speaker conversion", error)) return false;
    EventHandle event;
    if (!event.value)
    {
        if (error) *error = "cannot create playback event";
        return false;
    }
    UINT32 capacity = 0;
    if (failed(client->SetEventHandle(event.value), "set playback event", error) ||
        failed(client->GetBufferSize(&capacity), "get playback buffer", error) ||
        failed(client->GetService(IID_PPV_ARGS(&renderer)), "get playback renderer", error)) return false;
    if (capacity == 0) return reject("empty playback device buffer", error);

    std::size_t frames_in_chunk = audio.samples.size() / static_cast<std::size_t>(channels);
    std::deque<std::uint64_t> chunk_ends{static_cast<std::uint64_t>(accepted_frames)};
    statistics.accepted_chunks = 1;
    std::size_t cursor = 0;
    const auto submit = [&](UINT32 frames) {
        if (interrupted(cancelled, error)) return false;
        BYTE *destination = nullptr;
        if (failed(renderer->GetBuffer(frames, &destination), "get speaker buffer", error)) return false;
        RenderBuffer buffer{renderer.Get(), frames};
        if (interrupted(cancelled, error)) return false;
        auto *pcm = reinterpret_cast<float *>(destination);
        const auto count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
        const auto offset = cursor * static_cast<std::size_t>(channels);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (std::abs(audio.samples[offset + index]) > 1.0f) ++statistics.clipped_samples;
            pcm[index] = std::clamp(audio.samples[offset + index], -1.0f, 1.0f);
        }
        if (interrupted(cancelled, error) || failed(buffer.commit(), "release speaker buffer", error)) return false;
        cursor += frames;
        statistics.submitted_frames += frames;
        return true;
    };
    // Prime the first buffer before starting the shared audio client.
    if (interrupted(cancelled, error) ||
        !submit(static_cast<UINT32>(std::min<std::size_t>(capacity, frames_in_chunk))) ||
        interrupted(cancelled, error) || failed(client->Start(), "start speaker", error)) return false;
    statistics.client_started_ms = elapsed(started);
    statistics.client_started = true;
    ComPtr<IAudioClock> clock;
    if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&clock))) &&
        SUCCEEDED(clock->GetFrequency(&statistics.clock_frequency)) && statistics.clock_frequency != 0 &&
        SUCCEEDED(clock->GetPosition(&statistics.clock_start_position, nullptr)))
        statistics.clock_available = true;

    struct StopClient
    {
        IAudioClient *client;
        IAudioClock *clock;
        PlaybackStatistics &statistics;
        std::deque<std::uint64_t> &chunk_ends;
        ~StopClient()
        {
            UINT32 padding = 0;
            if (SUCCEEDED(client->GetCurrentPadding(&padding)) && padding <= statistics.submitted_frames)
            {
                statistics.consumed_frames = statistics.submitted_frames - padding;
                statistics.padding_available = true;
                while (!chunk_ends.empty() && chunk_ends.front() <= statistics.consumed_frames)
                {
                    chunk_ends.pop_front();
                    ++statistics.completed_chunks;
                }
            }
            if (statistics.clock_available && clock &&
                FAILED(clock->GetPosition(&statistics.clock_end_position, nullptr)))
                statistics.clock_available = false;
            client->Stop();
        }
    } stop_client{client.Get(), clock.Get(), statistics, chunk_ends};
    bool finished = false;
    bool source_pending = false;
    bool underrun_noted = false;
    for (;;)
    {
        if (interrupted(cancelled, error)) return false;
        UINT32 padding = 0;
        if (failed(client->GetCurrentPadding(&padding), "read playback padding", error)) return false;
        if (padding > capacity) return reject("invalid playback buffer padding", error);
        if (padding <= statistics.submitted_frames)
        {
            statistics.consumed_frames = statistics.submitted_frames - padding;
            statistics.padding_available = true;
            while (!chunk_ends.empty() && chunk_ends.front() <= statistics.consumed_frames)
            {
                chunk_ends.pop_front();
                ++statistics.completed_chunks;
            }
        }
        UINT32 available = capacity - padding;
        while (available != 0 && !finished)
        {
            if (interrupted(cancelled, error)) return false;
            if (cursor == frames_in_chunk)
            {
                audio.samples.clear();
                const auto state = read_chunk(read, audio, sample_rate, channels, accepted_frames, error);
                if (state == detail::StreamRead::Failed) { statistics.finish_reason = "source_failed"; return false; }
                if (state == detail::StreamRead::Pending)
                {
                    ++statistics.pending_reads;
                    source_pending = true;
                    UINT32 pending_padding = 0;
                    const bool padding_read = SUCCEEDED(client->GetCurrentPadding(&pending_padding));
                    if (padding_read && pending_padding <= statistics.submitted_frames)
                    {
                        statistics.padding_available = true;
                        statistics.consumed_frames = statistics.submitted_frames - pending_padding;
                    }
                    if (padding_read && pending_padding == 0 && !underrun_noted)
                    {
                        ++statistics.underrun_events;
                        underrun_noted = true;
                    }
                    break;
                }
                source_pending = underrun_noted = false;
                if (state == detail::StreamRead::Finished)
                {
                    finished = true;
                    break;
                }
                frames_in_chunk = audio.samples.size() / static_cast<std::size_t>(channels);
                ++statistics.accepted_chunks;
                chunk_ends.push_back(static_cast<std::uint64_t>(accepted_frames));
                cursor = 0;
            }
            const auto frames = static_cast<UINT32>(std::min<std::size_t>(available, frames_in_chunk - cursor));
            if (!submit(frames)) return false;
            available -= frames;
        }
        if (finished)
        {
            if (failed(client->GetCurrentPadding(&padding), "drain playback padding", error)) return false;
            if (padding == 0)
            {
                if (error) error->clear();
                statistics.drained = true;
                return true;
            }
        }
        const auto waiting = Clock::now();
        const DWORD wait = WaitForSingleObject(event.value, 20);
        if (source_pending) statistics.source_wait_ms += elapsed(waiting);
        if (wait == WAIT_FAILED)
            return reject("playback event failed", error);
    }
}
}

namespace detail
{
bool play_stream(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled)
{
    return play_stream(read, error, cancelled, nullptr);
}

bool play_stream(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled,
                 PlaybackStatistics *statistics)
{
    const auto started = std::chrono::steady_clock::now();
    PlaybackStatistics measured;
    bool was_interrupted = false;
    const std::function<bool()> observed_cancelled = [&] {
        const bool value = cancelled && cancelled();
        was_interrupted = was_interrupted || value;
        return value;
    };
    struct Publish
    {
        PlaybackStatistics &measured;
        PlaybackStatistics *out;
        bool &cancel_seen;
        std::chrono::steady_clock::time_point started;
        ~Publish()
        {
            measured.finalized = true;
            measured.wall_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (measured.finish_reason.empty()) measured.finish_reason = measured.drained ? "drained" :
                cancel_seen ? "interrupted" : "failed";
            if (out) *out = std::move(measured);
        }
    } publish{measured, statistics, was_interrupted, started};
    if (error) error->clear();
    if (!read) return reject("playback audio source is missing", error);
    try { return play_stream_impl(read, error, observed_cancelled, measured); }
    catch (const std::exception &exception)
    {
        if (error) *error = std::string("playback stream failed: ") + exception.what();
    }
    catch (...)
    {
        if (error) *error = "unexpected playback stream failure";
    }
    return false;
}
}

struct AudioRenderer::Impl
{
    std::atomic<std::uint64_t> generation{0};
};

AudioRenderer::AudioRenderer() : impl_(std::make_unique<Impl>()) {}
AudioRenderer::~AudioRenderer() { stop(); }

void AudioRenderer::stop() noexcept
{
    impl_->generation.fetch_add(1, std::memory_order_relaxed);
}

bool AudioRenderer::play(const AudioBuffer &audio, std::string *error,
                         const std::function<bool()> &cancelled)
{
    const auto generation = impl_->generation.load(std::memory_order_relaxed);
    bool supplied = false;
    return detail::play_stream([&audio, &supplied](AudioBuffer &chunk, std::string *) {
        if (supplied) return detail::StreamRead::Finished;
        chunk = audio;
        supplied = true;
        return detail::StreamRead::Ready;
    }, error, [this, generation, &cancelled] {
        return impl_->generation.load(std::memory_order_relaxed) != generation || (cancelled && cancelled());
    });
}
}
