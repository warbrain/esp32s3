#include "check.hpp"
#include "../tools/probe_resample.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr double pi = 3.14159265358979323846;

hmi::audio::AudioBuffer tone(double frequency)
{
    hmi::audio::AudioBuffer audio;
    audio.sample_rate = 24000;
    audio.samples.resize(24000);
    for (std::size_t frame = 0; frame < audio.samples.size(); ++frame)
        audio.samples[frame] = static_cast<float>(std::sin(2.0 * pi * frequency * static_cast<double>(frame) / 24000.0 + 0.37));
    return audio;
}

double middle_rms(const std::vector<float> &samples)
{
    double energy = 0.0;
    constexpr std::size_t margin = 160;
    for (std::size_t index = margin; index < samples.size() - margin; ++index)
        energy += static_cast<double>(samples[index]) * samples[index];
    return std::sqrt(energy / static_cast<double>(samples.size() - margin * 2));
}
}

HMI_TEST(probe_resampler_preserves_frame_count_dc_and_existing_16k_bytes)
{
    using hmi::probe::Resampler;
    hmi::audio::AudioBuffer source;
    source.sample_rate = 24000;
    source.samples.assign(24001, 0.25f);
    for (const auto mode : {Resampler::Linear, Resampler::Sinc})
    {
        const auto converted = hmi::probe::mono_16k(source, mode);
        CHECK(converted.size() == 16001);
        for (const auto value : converted) CHECK(std::abs(value - 0.25f) < 0.000001f && std::isfinite(value));
    }
    source.sample_rate = 16000;
    source.samples = {-0.0f, 0.0f, 0.25f, -0.75f};
    for (const auto mode : {Resampler::Linear, Resampler::Sinc})
    {
        const auto converted = hmi::probe::mono_16k(source, mode);
        CHECK(converted.size() == source.samples.size());
        CHECK(std::memcmp(converted.data(), source.samples.data(), converted.size() * sizeof(float)) == 0);
    }
    source.sample_rate = 24000;
    source.samples = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
    CHECK((hmi::probe::mono_16k(source) == std::vector<float>{1.0f, 3.0f, 8.0f, 24.0f}));
}

HMI_TEST(probe_sinc_resampler_preserves_low_frequency_amplitude_and_phase)
{
    for (const double frequency : {1000.0, 6000.0})
    {
        const auto converted = hmi::probe::mono_16k(tone(frequency), hmi::probe::Resampler::Sinc);
        CHECK(converted.size() == 16000);
        double squared_error = 0.0;
        for (std::size_t frame = 160; frame < converted.size() - 160; ++frame)
        {
            const double reference = std::sin(2.0 * pi * frequency * static_cast<double>(frame) / 16000.0 + 0.37);
            const double error = converted[frame] - reference;
            squared_error += error * error;
        }
        CHECK(std::sqrt(squared_error / 15680.0) < 0.001);
        CHECK(std::abs(middle_rms(converted) - std::sqrt(0.5)) < 0.001);
    }
}

HMI_TEST(probe_sinc_resampler_rejects_frequencies_above_the_output_nyquist_limit)
{
    for (const double frequency : {8000.0, 9000.0, 10000.0})
    {
        const auto source = tone(frequency);
        const auto filtered = hmi::probe::mono_16k(source, hmi::probe::Resampler::Sinc);
        const auto linear = hmi::probe::mono_16k(source, hmi::probe::Resampler::Linear);
        CHECK(middle_rms(filtered) < 0.001);
        CHECK(middle_rms(linear) > 0.05);
    }
}

HMI_TEST(probe_resampler_downmixes_channels_and_rejects_invalid_source_pcm)
{
    hmi::audio::AudioBuffer source;
    source.sample_rate = 24000;
    source.channels = 2;
    source.samples = {1.0f, -1.0f, 0.5f, -0.5f, 0.25f, -0.25f};
    for (const auto mode : {hmi::probe::Resampler::Linear, hmi::probe::Resampler::Sinc})
    {
        const auto converted = hmi::probe::mono_16k(source, mode);
        CHECK(converted.size() == 2);
        for (const auto value : converted) CHECK(value == 0.0f);
    }
    for (const int invalid : {0, 1, 2, 3})
    {
        auto audio = source;
        if (invalid == 0) audio.samples.clear();
        if (invalid == 1) audio.channels = 0;
        if (invalid == 2) audio.samples.pop_back();
        if (invalid == 3) audio.samples[0] = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try { (void)hmi::probe::mono_16k(audio, hmi::probe::Resampler::Sinc); }
        catch (const std::runtime_error &) { rejected = true; }
        CHECK(rejected);
    }
}
