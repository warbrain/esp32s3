#include "check.hpp"
#include "engine/hgtcrn_session.hpp"
#include "engine/native_client.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
class HgtcrnEnvironment
{
public:
    HgtcrnEnvironment(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t *old = nullptr;
        std::size_t count = 0;
        if (_wdupenv_s(&old, &count, name) == 0 && old) previous_ = old;
        std::free(old);
        set(value);
    }
    ~HgtcrnEnvironment() { set(previous_); }
    bool set(const std::wstring &value) const { return _wputenv_s(name_.c_str(), value.c_str()) == 0; }
private:
    std::wstring name_;
    std::wstring previous_;
};

struct HgtcrnFiles
{
    std::filesystem::path base = std::filesystem::absolute(std::filesystem::temp_directory_path());
    std::filesystem::path path = base / (L"hmi-hgtcrn-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    HgtcrnFiles()
    {
        std::filesystem::create_directories(path);
        std::ofstream(path / "engine.json", std::ios::binary) << "{\"families\":\"qwen3_asr\"}\n";
        std::ofstream(path / "hgtcrn.safetensors", std::ios::binary) << "fixture";
    }
    ~HgtcrnFiles()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        if (error) return;
        const auto parent = std::filesystem::weakly_canonical(base, error);
        if (!error && resolved.parent_path() == parent &&
            resolved.filename().native().starts_with(L"hmi-hgtcrn-test-"))
            std::filesystem::remove_all(resolved, error);
    }
};

std::filesystem::path hgtcrn_fixture_path(const wchar_t *name)
{
    wchar_t executable[32768]{};
    const auto count = GetModuleFileNameW(nullptr, executable, 32768);
    return std::filesystem::path(std::wstring(executable, count)).parent_path() / name;
}

class HgtcrnFixture
{
private:
    HgtcrnFiles files_;
    HgtcrnEnvironment dll_;
    HgtcrnEnvironment configuration_{L"HMI_NATIVE_CONFIG", (files_.path / "engine.json").wstring()};
    HgtcrnEnvironment failure_{L"HMI_TEST_SDK_FAILURE", L""};
    HgtcrnEnvironment mode_{L"HMI_TEST_HGTCRN_MODE", L""};
public:
    explicit HgtcrnFixture(const wchar_t *name = L"sdk_lifetime_fixture.dll")
        : dll_(L"HMI_NATIVE_DLL", hgtcrn_fixture_path(name).wstring())
    {
        config.model = files_.path / "hgtcrn.safetensors";
        opened = client.open(&error);
    }
    bool mode(const wchar_t *value) const { return mode_.set(value); }
    hmi::engine::Client client;
    hmi::engine::HgtcrnSession session;
    hmi::engine::HgtcrnConfig config;
    std::string error;
    bool opened = false;
};

std::vector<float> hgtcrn_stereo(std::size_t frames)
{
    std::vector<float> pcm(frames * 2);
    for (std::size_t index = 0; index < frames; ++index)
    {
        pcm[index * 2] = static_cast<float>(std::sin(static_cast<double>(index) * 0.031)) * 0.25f;
        pcm[index * 2 + 1] = static_cast<float>(std::cos(static_cast<double>(index) * 0.047)) * 0.125f;
    }
    return pcm;
}

std::vector<float> hgtcrn_first_channel(const std::vector<float> &stereo)
{
    std::vector<float> mono(stereo.size() / 2);
    for (std::size_t index = 0; index < mono.size(); ++index) mono[index] = stereo[index * 2];
    return mono;
}
}

HMI_TEST(hgtcrn_owned_mono_output_preserves_segment_length_and_request_statistics)
{
    HgtcrnFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.session.open(fixture.client, fixture.config, &fixture.error));
    auto stereo = hgtcrn_stereo(8193);
    const auto expected = hgtcrn_first_channel(stereo);
    std::vector<float> mono{99.0f};
    CHECK(fixture.session.run(stereo.data(), stereo.size() / 2, 51, mono, &fixture.error));
    CHECK(mono == expected);
    stereo.assign(stereo.size(), 0.0f);
    CHECK(mono == expected);
    const auto stats = fixture.session.statistics();
    CHECK(stats.ready && stats.succeeded && !stats.failed && stats.channels == 2);
    CHECK(stats.request_id == 51 && stats.input_frames == expected.size() && stats.output_frames == expected.size());
    CHECK(stats.wpe_taps == 18 && stats.iva_iterations == 20 && stats.stft_frames > 0);
    CHECK(stats.right_padding_frames == 255 && stats.stft_frames == 34);
    const auto next = hgtcrn_stereo(4608);
    std::vector<float> next_mono;
    CHECK(fixture.session.run(next.data(), next.size() / 2, 52, next_mono, &fixture.error));
    CHECK(next_mono == hgtcrn_first_channel(next) && mono == expected);
    CHECK(fixture.session.statistics().request_id == 52 && fixture.session.statistics().input_frames == 4608);
}

HMI_TEST(hgtcrn_invalid_pcm_and_segment_bounds_do_not_poison_the_next_request)
{
    HgtcrnFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    fixture.config.maximum_audio_seconds = 1;
    CHECK(fixture.session.open(fixture.client, fixture.config, &fixture.error));
    auto stereo = hgtcrn_stereo(16001);
    std::vector<float> mono{99.0f};
    CHECK(!fixture.session.run(nullptr, 4608, 1, mono, &fixture.error) && mono.empty());
    CHECK(!fixture.session.run(stereo.data(), 0, 2, mono, &fixture.error) && mono.empty());
    CHECK(!fixture.session.run(stereo.data(), 4607, 3, mono, &fixture.error) && mono.empty());
    CHECK(!fixture.session.run(stereo.data(), 16001, 4, mono, &fixture.error) && mono.empty());
    const float saved = stereo[7];
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        stereo[7] = invalid;
        CHECK(!fixture.session.run(stereo.data(), 16000, 5, mono, &fixture.error) && mono.empty());
    }
    stereo[7] = saved;
    CHECK(fixture.session.run(stereo.data(), 16000, 6, mono, &fixture.error));
    CHECK(mono.size() == 16000 && fixture.session.statistics().succeeded && !fixture.session.statistics().failed);
}

HMI_TEST(hgtcrn_failures_and_malformed_results_leave_no_output_and_recover_without_reload)
{
    HgtcrnFixture fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.mode(L"create_failure"));
    CHECK(!fixture.session.open(fixture.client, fixture.config, &fixture.error));
    CHECK(!fixture.session.ready() && !fixture.error.empty() && fixture.client.ready());
    CHECK(fixture.mode(L""));
    CHECK(fixture.session.open(fixture.client, fixture.config, &fixture.error));
    const auto stereo = hgtcrn_stereo(8193);
    std::vector<float> mono;
    for (const auto *mode : {L"run_failure", L"missing_audio", L"invalid_rate", L"invalid_channels", L"wrong_length",
                             L"invalid_audio", L"wrong_request", L"statistics_failure", L"invalid_statistics"})
    {
        CHECK(fixture.mode(mode));
        mono.assign(1, 99.0f);
        CHECK(!fixture.session.run(stereo.data(), stereo.size() / 2, 7, mono, &fixture.error));
        CHECK(mono.empty() && !fixture.error.empty() && fixture.session.statistics().failed);
        CHECK(fixture.session.ready());
        CHECK(fixture.mode(L""));
        CHECK(fixture.session.run(stereo.data(), stereo.size() / 2, 8, mono, &fixture.error));
        CHECK(mono == hgtcrn_first_channel(stereo) && fixture.session.statistics().succeeded);
    }
}

HMI_TEST(hgtcrn_sdk_lease_outlives_client_and_missing_exports_preserve_other_tasks)
{
    {
        HgtcrnFixture fixture;
        CHECK(fixture.opened);
        if (!fixture.opened) return;
        CHECK(fixture.session.open(fixture.client, fixture.config, &fixture.error));
        fixture.client.close();
        const auto stereo = hgtcrn_stereo(4608);
        std::vector<float> mono;
        CHECK(fixture.session.run(stereo.data(), stereo.size() / 2, 1, mono, &fixture.error));
        CHECK(mono == hgtcrn_first_channel(stereo));
        fixture.session.close();
        CHECK(GetModuleHandleW(hgtcrn_fixture_path(L"sdk_lifetime_fixture.dll").c_str()) == nullptr);
    }
    HgtcrnFixture missing(L"sdk_frontend_missing_fixture.dll");
    CHECK(missing.opened);
    if (!missing.opened) return;
    CHECK(!missing.session.open(missing.client, missing.config, &missing.error));
    CHECK(!missing.session.ready() && !missing.error.empty() && missing.client.ready());
    CHECK(missing.client.tasks() == std::vector<std::string>{"asr"});
}
