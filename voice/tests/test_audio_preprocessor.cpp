#include "check.hpp"
#include "engine/audio_preprocessor.hpp"
#include "engine/native_client.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
class FrontendFiles
{
public:
    FrontendFiles()
    {
        base_ = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        path = base_ / (L"hmi-frontend-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
        std::ofstream(path / "engine.json", std::ios::binary) << "{\"families\":\"qwen3_asr\"}\n";
        std::ofstream(path / "vad.safetensors", std::ios::binary) << "fixture";
        std::ofstream(path / "gtcrn.safetensors", std::ios::binary) << "fixture";
    }

    ~FrontendFiles()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        if (error) return;
        const auto base = std::filesystem::weakly_canonical(base_, error);
        if (!error && resolved.parent_path() == base &&
            resolved.filename().native().starts_with(L"hmi-frontend-test-"))
            std::filesystem::remove_all(resolved, error);
    }

    std::filesystem::path path;

private:
    std::filesystem::path base_;
};

class FrontendEnvironment
{
public:
    FrontendEnvironment(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t *previous = nullptr;
        std::size_t count = 0;
        if (_wdupenv_s(&previous, &count, name) == 0 && previous) previous_ = previous;
        std::free(previous);
        set(value);
    }
    ~FrontendEnvironment() { set(previous_); }
    bool set(const std::wstring &value) const { return _wputenv_s(name_.c_str(), value.c_str()) == 0; }

private:
    std::wstring name_;
    std::wstring previous_;
};

std::filesystem::path frontend_fixture_path(const wchar_t *name)
{
    wchar_t module[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, module, 32768);
    return std::filesystem::path(std::wstring(module, count)).parent_path() / name;
}

class FrontendFixture
{
private:
    FrontendFiles files_;
    FrontendEnvironment dll_;
    FrontendEnvironment configuration_{L"HMI_NATIVE_CONFIG", (files_.path / "engine.json").wstring()};
    FrontendEnvironment sdk_failure_{L"HMI_TEST_SDK_FAILURE", L""};
    FrontendEnvironment frontend_mode_{L"HMI_TEST_FRONTEND_MODE", L""};

public:
    explicit FrontendFixture(const wchar_t *dll = L"sdk_lifetime_fixture.dll")
        : dll_(L"HMI_NATIVE_DLL", frontend_fixture_path(dll).wstring())
    {
        config.vad_model = files_.path / "vad.safetensors";
        config.denoise_model = files_.path / "gtcrn.safetensors";
        config.denoise_enabled = true;
        opened = client.open(&error);
        if (!opened) std::fprintf(stderr, "Frontend SDK fixture setup failed: %s\n", error.c_str());
    }
    bool mode(const wchar_t *value) const { return frontend_mode_.set(value); }

    hmi::engine::Client client;
    hmi::engine::AudioPreprocessor frontend;
    hmi::engine::AudioPreprocessorConfig config;
    std::string error;
    bool opened = false;
};

std::vector<float> frontend_pcm(std::size_t count)
{
    std::vector<float> pcm(count);
    for (std::size_t index = 0; index < count; ++index)
        pcm[index] = static_cast<float>(static_cast<int>(index % 127) - 63) / 128.0f;
    return pcm;
}
}

HMI_TEST(audio_frontend_disabled_bypasses_the_sdk_and_preserves_owned_pcm)
{
    hmi::engine::Client unopened;
    hmi::engine::AudioPreprocessor frontend;
    hmi::engine::AudioPreprocessorConfig config;
    config.vad_enabled = false;
    config.denoise_enabled = false;
    std::string error;
    CHECK(frontend.open(unopened, config, &error));
    CHECK(frontend.ready() && !unopened.ready());
    CHECK(frontend.begin(41, &error));
    auto input = frontend_pcm(513);
    const auto expected = input;
    std::vector<float> output{99.0f};
    CHECK(frontend.push(input.data(), input.size(), output, &error));
    CHECK(output == expected);
    input.assign(input.size(), 0.0f);
    CHECK(output == expected);
    CHECK(frontend.finish(output, &error) && output.empty());
    const auto stats = frontend.statistics();
    CHECK(stats.ready && !stats.vad_enabled && !stats.denoise_enabled);
    CHECK(stats.finished && !stats.failed && stats.request_id == 41);
    CHECK(stats.input_frames == expected.size() && stats.output_frames == expected.size());
    CHECK(stats.pending_frames == 0 && stats.vad_chunks == 0 && stats.gtcrn_frames == 0);
}

HMI_TEST(audio_frontend_delayed_chunks_and_final_tail_preserve_every_original_frame)
{
    FrontendFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    for (const std::size_t count : {1u, 159u, 255u, 256u, 257u, 511u, 512u, 513u, 4000u, 4095u})
    {
        CHECK(fixture.frontend.begin(count, &fixture.error));
        const auto input = frontend_pcm(count);
        std::vector<float> collected;
        std::vector<float> output{99.0f};
        std::size_t offset = 0;
        for (const std::size_t partition : {1u, 128u, 17u, 511u, 129u, 1023u, 2286u})
        {
            const auto frames = std::min(partition, count - offset);
            if (frames == 0) break;
            CHECK(fixture.frontend.push(input.data() + offset, frames, output, &fixture.error));
            collected.insert(collected.end(), output.begin(), output.end());
            offset += frames;
            CHECK(fixture.frontend.statistics().input_frames == offset);
        }
        CHECK(offset == count);
        const auto before_finish = fixture.frontend.statistics();
        CHECK(before_finish.pending_frames == std::min<std::size_t>(256, count));
        CHECK(fixture.frontend.finish(output, &fixture.error));
        collected.insert(collected.end(), output.begin(), output.end());
        CHECK(collected == input);
        const auto stats = fixture.frontend.statistics();
        CHECK(stats.input_frames == count && stats.output_frames == count && stats.pending_frames == 0);
        CHECK(stats.finished && !stats.failed && stats.request_id == count);
        CHECK(stats.vad_chunks == (count + 511) / 512);
        CHECK(stats.voiced_frames <= count);
        CHECK(!fixture.frontend.finish(output, &fixture.error) && output.empty());
    }
}

HMI_TEST(audio_frontend_rejects_invalid_threshold_and_duration_even_in_bypass_mode)
{
    hmi::engine::Client unopened;
    hmi::engine::AudioPreprocessor frontend;
    hmi::engine::AudioPreprocessorConfig config;
    config.vad_enabled = false;
    config.denoise_enabled = false;
    std::string error;
    for (const float threshold : {0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()})
    {
        config.vad_threshold = threshold;
        CHECK(!frontend.open(unopened, config, &error) && !frontend.ready() && !error.empty());
    }
    config.vad_threshold = 0.35f;
    for (const int seconds : {0, 31})
    {
        config.maximum_audio_seconds = seconds;
        CHECK(!frontend.open(unopened, config, &error) && !frontend.ready() && !error.empty());
    }
    config.maximum_audio_seconds = 30;
    CHECK(frontend.open(unopened, config, &error));
    CHECK(frontend.begin(1, &error));
    std::vector<float> output{99.0f};
    CHECK(frontend.push(nullptr, 0, output, &error) && output.empty());
    CHECK(frontend.finish(output, &error) && output.empty());
    CHECK(frontend.statistics().input_frames == 0 && frontend.statistics().output_frames == 0);
}

HMI_TEST(audio_frontend_begin_discards_old_lookahead_and_can_reopen_after_close)
{
    FrontendFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(fixture.frontend.begin(101, &fixture.error));
    const std::vector<float> discarded(200, 0.75f);
    std::vector<float> output;
    CHECK(fixture.frontend.push(discarded.data(), discarded.size(), output, &fixture.error) && output.empty());
    CHECK(fixture.frontend.begin(102, &fixture.error));
    CHECK(fixture.frontend.statistics().input_frames == 0 && fixture.frontend.statistics().pending_frames == 0);
    const auto current = frontend_pcm(17);
    CHECK(fixture.frontend.push(current.data(), current.size(), output, &fixture.error));
    CHECK(fixture.frontend.finish(output, &fixture.error));
    CHECK(output == current && fixture.frontend.statistics().request_id == 102);
    fixture.frontend.close();
    CHECK(!fixture.frontend.ready());
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(fixture.frontend.begin(103, &fixture.error));
    CHECK(fixture.frontend.push(current.data(), current.size(), output, &fixture.error));
    CHECK(fixture.frontend.finish(output, &fixture.error) && output == current);
}

HMI_TEST(audio_frontend_vad_reports_speech_uncertainty_and_original_partial_chunk_length)
{
    FrontendFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    fixture.config.denoise_enabled = false;
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    const std::vector<float> input(257, 0.0001f);
    std::vector<float> output;
    for (const auto *mode : {L"", L"silent", L"uncertain"})
    {
        CHECK(fixture.mode(mode));
        CHECK(fixture.frontend.begin(5, &fixture.error));
        CHECK(fixture.frontend.push(input.data(), input.size(), output, &fixture.error));
        CHECK(output == input && fixture.frontend.statistics().vad_chunks == 0);
        CHECK(fixture.frontend.finish(output, &fixture.error) && output.empty());
        const auto stats = fixture.frontend.statistics();
        CHECK(stats.vad_chunks == 1 && stats.input_frames == input.size() && stats.output_frames == input.size());
        if (*mode == L'\0')
            CHECK(stats.speech_seen && stats.voiced_chunks == 1 && stats.voiced_frames == input.size());
        else if (std::wstring_view(mode) == L"uncertain")
            CHECK(!stats.speech_seen && stats.uncertain_chunks == 1 && stats.voiced_frames == 0);
        else
            CHECK(!stats.speech_seen && stats.voiced_chunks == 0 && stats.uncertain_chunks == 0);
    }
}

HMI_TEST(audio_frontend_rejects_invalid_pcm_and_bounds_without_consuming_audio)
{
    FrontendFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    fixture.config.maximum_audio_seconds = 1;
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(fixture.frontend.begin(1, &fixture.error));
    std::vector<float> output{99.0f};
    CHECK(!fixture.frontend.push(nullptr, 1, output, &fixture.error) && output.empty());
    CHECK(fixture.frontend.statistics().input_frames == 0);
    for (const float sample : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    {
        CHECK(fixture.frontend.begin(2, &fixture.error));
        CHECK(!fixture.frontend.push(&sample, 1, output, &fixture.error) && output.empty());
        CHECK(fixture.frontend.statistics().input_frames == 0);
    }
    const auto excessive = frontend_pcm(16001);
    CHECK(fixture.frontend.begin(3, &fixture.error));
    CHECK(!fixture.frontend.push(excessive.data(), excessive.size(), output, &fixture.error) && output.empty());
    CHECK(fixture.frontend.statistics().input_frames == 0);
    CHECK(fixture.frontend.begin(4, &fixture.error));
    CHECK(fixture.frontend.push(excessive.data(), 16000, output, &fixture.error));
    CHECK(!fixture.frontend.push(excessive.data(), 1, output, &fixture.error) && output.empty());
    CHECK(fixture.frontend.statistics().input_frames == 16000);
    CHECK(fixture.frontend.begin(5, &fixture.error));
    CHECK(fixture.frontend.push(excessive.data(), 16000, output, &fixture.error));
    CHECK(fixture.frontend.finish(output, &fixture.error));
    CHECK(fixture.frontend.statistics().output_frames == 16000);
}

HMI_TEST(audio_frontend_sdk_failures_do_not_publish_partial_or_stale_output_and_reset_recovers)
{
    FrontendFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.mode(L"create_failure"));
    CHECK(!fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(!fixture.frontend.ready() && !fixture.error.empty() && fixture.client.ready());
    CHECK(fixture.mode(L""));
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(fixture.mode(L"reset_failure"));
    CHECK(!fixture.frontend.begin(6, &fixture.error) && fixture.frontend.statistics().failed);
    CHECK(fixture.mode(L""));
    const auto input = frontend_pcm(512);
    std::vector<float> output;
    for (const auto *mode : {L"push_failure", L"invalid_format", L"invalid_audio", L"excess_output", L"statistics_failure"})
    {
        CHECK(fixture.frontend.begin(7, &fixture.error));
        CHECK(fixture.mode(mode));
        output.assign(1, 99.0f);
        CHECK(!fixture.frontend.push(input.data(), input.size(), output, &fixture.error));
        CHECK(output.empty() && !fixture.error.empty() && fixture.frontend.statistics().failed);
        CHECK(fixture.mode(L""));
        CHECK(fixture.frontend.begin(8, &fixture.error));
        CHECK(!fixture.frontend.statistics().failed && fixture.frontend.statistics().input_frames == 0);
        CHECK(fixture.frontend.push(input.data(), 17, output, &fixture.error) && output.empty());
        CHECK(fixture.frontend.finish(output, &fixture.error));
        CHECK(output == std::vector<float>(input.begin(), input.begin() + 17));
    }
    CHECK(fixture.frontend.begin(81, &fixture.error));
    CHECK(fixture.frontend.push(input.data(), input.size(), output, &fixture.error));
    CHECK(output == std::vector<float>(input.begin(), input.begin() + 256));
    CHECK(fixture.mode(L"push_failure"));
    CHECK(!fixture.frontend.push(input.data(), input.size(), output, &fixture.error) && output.empty());
    CHECK(fixture.frontend.statistics().failed);
    CHECK(fixture.mode(L""));
    CHECK(fixture.frontend.begin(9, &fixture.error));
    CHECK(fixture.frontend.push(input.data(), 17, output, &fixture.error));
    CHECK(fixture.mode(L"finish_failure"));
    CHECK(!fixture.frontend.finish(output, &fixture.error) && output.empty());
    CHECK(fixture.frontend.statistics().failed);
    CHECK(fixture.mode(L""));
    CHECK(fixture.frontend.begin(10, &fixture.error));
    CHECK(fixture.frontend.push(input.data(), 17, output, &fixture.error));
    CHECK(fixture.frontend.finish(output, &fixture.error));
    CHECK(output == std::vector<float>(input.begin(), input.begin() + 17));
}

HMI_TEST(audio_frontend_retains_its_sdk_lease_until_its_own_close)
{
    FrontendFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(fixture.frontend.begin(22, &fixture.error));
    fixture.client.close();
    CHECK(!fixture.client.ready() && fixture.frontend.ready());
    const auto input = frontend_pcm(17);
    std::vector<float> output;
    CHECK(fixture.frontend.push(input.data(), input.size(), output, &fixture.error));
    CHECK(fixture.frontend.finish(output, &fixture.error) && output == input);
    fixture.frontend.close();
    CHECK(GetModuleHandleW(frontend_fixture_path(L"sdk_lifetime_fixture.dll").c_str()) == nullptr);
}

HMI_TEST(audio_frontend_missing_optional_exports_leave_asr_client_usable)
{
    FrontendFixture fixture(L"sdk_frontend_missing_fixture.dll");
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(!fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(!fixture.frontend.ready() && !fixture.error.empty() && fixture.client.ready());
    CHECK(fixture.client.tasks() == std::vector<std::string>{"asr"});
    fixture.config.vad_enabled = false;
    fixture.config.denoise_enabled = false;
    CHECK(fixture.frontend.open(fixture.client, fixture.config, &fixture.error));
    CHECK(fixture.frontend.begin(12, &fixture.error));
    const auto input = frontend_pcm(257);
    std::vector<float> output;
    CHECK(fixture.frontend.push(input.data(), input.size(), output, &fixture.error) && output == input);
    CHECK(fixture.frontend.finish(output, &fixture.error) && output.empty());
}
