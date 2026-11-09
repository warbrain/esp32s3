#include "net/esp32_audio.hpp"

#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace hmi::net
{
namespace
{
constexpr int kSampleRate = 16000;
constexpr int kFrameSamples = 320;  // 20 ms
constexpr int kBoardChannels = 2;   // both microphones
constexpr int kPlaybackBitrate = 24000;

std::int16_t to_pcm16(float sample)
{
    const float clamped = std::max(-1.0f, std::min(1.0f, sample));
    return static_cast<std::int16_t>(clamped * 32767.0f);
}
} // namespace

struct Esp32Audio::Impl
{
    AudioLink link;
    OpusDecoder *decoder = nullptr;
    OpusEncoder *encoder = nullptr;

    mutable std::mutex statistics_mutex;
    Statistics stats;

    std::int64_t start_frame = 0;
    std::uint16_t last_sequence = 0;
    bool have_sequence = false;

    std::vector<std::int16_t> decode_buffer; // interleaved stereo
    std::vector<float> mono_buffer;
    std::vector<std::uint8_t> encoded;
};

Esp32Audio::Esp32Audio() : impl_(std::make_unique<Impl>()) {}

Esp32Audio::~Esp32Audio()
{
    stop();
}

bool Esp32Audio::start(const AudioLinkConfig &config, MicSink sink, ControlSink control, std::string *error)
{
    if (!impl_->link.open(config, error))
    {
        return false;
    }

    int opus_error = OPUS_OK;
    impl_->decoder = opus_decoder_create(kSampleRate, kBoardChannels, &opus_error);
    if (opus_error != OPUS_OK || impl_->decoder == nullptr)
    {
        if (error) *error = std::string("opus decoder: ") + opus_strerror(opus_error);
        impl_->link.close();
        return false;
    }
    impl_->decode_buffer.assign(static_cast<std::size_t>(kFrameSamples) * kBoardChannels, 0);
    impl_->mono_buffer.assign(static_cast<std::size_t>(kFrameSamples), 0.0f);

    const bool started = impl_->link.start(
        [this, sink = std::move(sink), control = std::move(control)](FrameType type,
                                                                    const std::uint8_t *payload,
                                                                    std::size_t size) {
            if (type == FrameType::Control)
            {
                if (control && size > 0) control(reinterpret_cast<const char *>(payload), size);
                return;
            }
            if (type != FrameType::AudioUp || size == 0 || impl_->decoder == nullptr)
            {
                return;
            }
            const int decoded = opus_decode(impl_->decoder, payload, static_cast<opus_int32>(size),
                                            impl_->decode_buffer.data(), kFrameSamples, 0);
            if (decoded <= 0)
            {
                std::lock_guard lock(impl_->statistics_mutex);
                impl_->stats.decode_errors++;
                impl_->stats.last_error = opus_strerror(decoded);
                return;
            }

            for (int i = 0; i < decoded; ++i)
            {
                const float left = static_cast<float>(impl_->decode_buffer[2 * i]) / 32768.0f;
                const float right = static_cast<float>(impl_->decode_buffer[2 * i + 1]) / 32768.0f;
                impl_->mono_buffer[static_cast<std::size_t>(i)] = 0.5f * (left + right);
            }

            {
                std::lock_guard lock(impl_->statistics_mutex);
                impl_->stats.frames_received++;
            }
            if (sink)
            {
                sink(impl_->mono_buffer.data(), static_cast<std::size_t>(decoded), impl_->start_frame);
            }
            impl_->start_frame += decoded;
        },
        error);
    if (!started)
    {
        opus_decoder_destroy(impl_->decoder);
        impl_->decoder = nullptr;
        impl_->link.close();
        return false;
    }

    // The link stays up for TTS playback and for board button events; the
    // uplink itself is switched on by request_capture().
    return true;
}

void Esp32Audio::stop()
{
    if (impl_->link.running())
    {
        impl_->link.send_text(FrameType::Control, "capture_stop");
    }
    impl_->link.close();
    if (impl_->decoder != nullptr)
    {
        opus_decoder_destroy(impl_->decoder);
        impl_->decoder = nullptr;
    }
    if (impl_->encoder != nullptr)
    {
        opus_encoder_destroy(impl_->encoder);
        impl_->encoder = nullptr;
    }
}

bool Esp32Audio::running() const noexcept
{
    return impl_->link.running();
}

bool Esp32Audio::play(const float *samples, std::size_t frames, int sample_rate,
                      const std::function<bool()> &cancelled)
{
    if (!impl_->link.running() || samples == nullptr || frames == 0 || sample_rate <= 0)
    {
        return false;
    }

    // TTS renders at 24 kHz while the board runs its I2S at 16 kHz; a linear
    // resample is enough for speech and keeps the codec dependency list short.
    std::vector<float> resampled;
    if (sample_rate != kSampleRate)
    {
        const auto target_frames = static_cast<std::size_t>(
            static_cast<double>(frames) * kSampleRate / sample_rate);
        resampled.resize(target_frames);
        const double step = static_cast<double>(sample_rate) / kSampleRate;
        for (std::size_t i = 0; i < target_frames; ++i)
        {
            const double position = i * step;
            const auto index = static_cast<std::size_t>(position);
            const double fraction = position - static_cast<double>(index);
            const float a = samples[index];
            const float b = samples[index + 1 < frames ? index + 1 : index];
            resampled[i] = static_cast<float>(a + (b - a) * fraction);
        }
        samples = resampled.data();
        frames = target_frames;
    }

    if (impl_->encoder == nullptr)
    {
        int opus_error = OPUS_OK;
        impl_->encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &opus_error);
        if (opus_error != OPUS_OK || impl_->encoder == nullptr)
        {
            std::lock_guard lock(impl_->statistics_mutex);
            impl_->stats.last_error = std::string("opus encoder: ") + opus_strerror(opus_error);
            return false;
        }
        opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(kPlaybackBitrate));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_COMPLEXITY(5));
    }

    impl_->encoded.resize(400);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(kFrameSamples));

    // Absolute schedule: sleep_for(20ms) would add the encode time on top of
    // every frame and drift slower than real time, which drains the board's
    // jitter buffer and makes playback stutter.
    const auto playback_started = std::chrono::steady_clock::now();
    std::size_t frame_index = 0;

    for (std::size_t offset = 0; offset + kFrameSamples <= frames; offset += kFrameSamples, ++frame_index)
    {
        if (cancelled && cancelled())
        {
            return true;
        }
        for (int i = 0; i < kFrameSamples; ++i)
        {
            pcm[static_cast<std::size_t>(i)] = to_pcm16(samples[offset + i]);
        }
        const int bytes = opus_encode(impl_->encoder, pcm.data(), kFrameSamples,
                                      impl_->encoded.data(), static_cast<opus_int32>(impl_->encoded.size()));
        if (bytes <= 0)
        {
            std::lock_guard lock(impl_->statistics_mutex);
            impl_->stats.send_errors++;
            impl_->stats.last_error = opus_strerror(bytes);
            continue;
        }
        const bool sent = impl_->link.send(FrameType::AudioDown, impl_->encoded.data(),
                                           static_cast<std::size_t>(bytes));
        {
            std::lock_guard lock(impl_->statistics_mutex);
            if (sent) impl_->stats.frames_sent++;
            else impl_->stats.send_errors++;
        }
        // Real-time pacing against an absolute deadline; the board buffers
        // roughly 200 ms on its side.
        std::this_thread::sleep_until(playback_started + std::chrono::milliseconds(20) * (frame_index + 1));
    }
    return true;
}

bool Esp32Audio::set_volume(int percent)
{
    const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const std::string command = "volume:" + std::to_string(clamped);
    return impl_->link.send_text(FrameType::Control, command);
}

bool Esp32Audio::request_capture(bool on)
{
    return impl_->link.send_text(FrameType::Control, on ? "capture_start" : "capture_stop");
}

Esp32Audio::Statistics Esp32Audio::statistics() const
{
    std::lock_guard lock(impl_->statistics_mutex);
    return impl_->stats;
}
}
