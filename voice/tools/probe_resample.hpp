#pragma once

#include "audio/audio_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace hmi::probe
{
enum class Resampler { Linear, Sinc };

inline const char *resampler_name(Resampler mode) noexcept
{
    return mode == Resampler::Sinc ? "sinc" : "linear";
}

// Diagnostic conversion only; production capture continues to use WASAPI.
// Linear preserves the historical probe path. Sinc uses a 193-tap Blackman
// low-pass filter with a 7.2 kHz cutoff when downsampling 24 kHz to 16 kHz.
inline std::vector<float> mono_16k(const audio::AudioBuffer &audio, Resampler mode = Resampler::Linear)
{
    if (audio.channels < 1 || audio.channels > 8 || audio.sample_rate < 8000 || audio.sample_rate > 192000 ||
        audio.samples.empty() || audio.samples.size() % static_cast<std::size_t>(audio.channels) != 0 ||
        !std::all_of(audio.samples.begin(), audio.samples.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("cannot resample invalid probe PCM");
    if (audio.sample_rate == 16000 && audio.channels == 1) return audio.samples;
    const auto input_frames = audio.samples.size() / static_cast<std::size_t>(audio.channels);
    const auto output_frames = static_cast<std::size_t>(std::llround(
        static_cast<double>(input_frames) * 16000.0 / static_cast<double>(audio.sample_rate)));
    std::vector<float> output(output_frames);
    if (mode == Resampler::Linear)
    {
        for (std::size_t frame = 0; frame < output_frames; ++frame)
        {
            const double source = static_cast<double>(frame) * audio.sample_rate / 16000.0;
            const auto left = std::min(static_cast<std::size_t>(source), input_frames - 1);
            const auto right = std::min(left + 1, input_frames - 1);
            const float fraction = static_cast<float>(source - static_cast<double>(left));
            float value = 0.0f;
            for (int channel = 0; channel < audio.channels; ++channel)
            {
                const auto ch = static_cast<std::size_t>(channel);
                const auto channels = static_cast<std::size_t>(audio.channels);
                value += audio.samples[left * channels + ch] * (1.0f - fraction) +
                    audio.samples[right * channels + ch] * fraction;
            }
            output[frame] = value / static_cast<float>(audio.channels);
        }
        return output;
    }

    std::vector<double> mono(input_frames);
    for (std::size_t frame = 0; frame < input_frames; ++frame)
    {
        for (int channel = 0; channel < audio.channels; ++channel)
            mono[frame] += audio.samples[frame * static_cast<std::size_t>(audio.channels) +
                static_cast<std::size_t>(channel)];
        mono[frame] /= audio.channels;
    }
    constexpr int radius = 96;
    constexpr double pi = 3.14159265358979323846;
    const auto divisor = std::gcd(audio.sample_rate, 16000);
    const auto phase_count = static_cast<std::size_t>(16000 / divisor);
    const double cutoff = 0.45 * std::min(1.0, 16000.0 / audio.sample_rate);
    std::vector<std::vector<double>> phases(phase_count, std::vector<double>(radius * 2 + 1));
    for (std::size_t phase = 0; phase < phase_count; ++phase)
    {
        const double fraction = static_cast<double>(phase) * divisor / 16000.0;
        double sum = 0.0;
        for (int tap = -radius; tap <= radius; ++tap)
        {
            const double distance = tap - fraction;
            const double window = std::abs(distance) > radius ? 0.0 :
                0.42 + 0.5 * std::cos(pi * distance / radius) + 0.08 * std::cos(2.0 * pi * distance / radius);
            const double sinc = std::abs(distance) < 1e-12 ? 2.0 * cutoff :
                std::sin(2.0 * pi * cutoff * distance) / (pi * distance);
            const auto index = static_cast<std::size_t>(tap + radius);
            phases[phase][index] = sinc * window;
            sum += phases[phase][index];
        }
        for (auto &weight : phases[phase]) weight /= sum;
    }
    for (std::size_t frame = 0; frame < output_frames; ++frame)
    {
        const auto position = static_cast<std::uint64_t>(frame) * static_cast<std::uint64_t>(audio.sample_rate);
        const auto center = static_cast<std::int64_t>(position / 16000);
        const auto phase = static_cast<std::size_t>((position % 16000) / static_cast<std::uint64_t>(divisor));
        double sample = 0.0;
        for (int tap = -radius; tap <= radius; ++tap)
        {
            // Constant extension preserves DC without adding a filter delay.
            const auto source = static_cast<std::size_t>(std::clamp(center + tap, std::int64_t{0},
                static_cast<std::int64_t>(input_frames - 1)));
            sample += mono[source] * phases[phase][static_cast<std::size_t>(tap + radius)];
        }
        output[frame] = static_cast<float>(sample);
    }
    return output;
}
}
