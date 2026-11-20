#include "check.hpp"
#include "engine/asr_session.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
class AsrFiles
{
public:
    AsrFiles()
    {
        base_ = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        path = base_ / (L"hmi-asr-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
        std::ofstream file(path / "engine.json", std::ios::binary);
        file << "{\"families\":\"qwen3_asr\"}\n";
    }
    ~AsrFiles()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        const auto base = std::filesystem::weakly_canonical(base_, error);
        if (!error && resolved.parent_path() == base && resolved.filename().native().starts_with(L"hmi-asr-test-"))
            std::filesystem::remove_all(resolved, error);
    }
    std::filesystem::path path;

private:
    std::filesystem::path base_;
};

class AsrEnvironment
{
public:
    AsrEnvironment(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t *old = nullptr;
        std::size_t count = 0;
        if (_wdupenv_s(&old, &count, name) == 0 && old) previous_ = old;
        std::free(old);
        set(value);
    }
    ~AsrEnvironment() { set(previous_); }
    bool set(const std::wstring &value) const { return _wputenv_s(name_.c_str(), value.c_str()) == 0; }

private:
    std::wstring name_;
    std::wstring previous_;
};

std::filesystem::path asr_fixture_path()
{
    wchar_t module[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, module, 32768);
    return std::filesystem::path(std::wstring(module, count)).parent_path() / "sdk_lifetime_fixture.dll";
}

class FixtureSession
{
private:
    AsrFiles files_;
    AsrEnvironment dll_{L"HMI_NATIVE_DLL", asr_fixture_path().wstring()};
    AsrEnvironment configuration_{L"HMI_NATIVE_CONFIG", (files_.path / "engine.json").wstring()};
    AsrEnvironment sdk_failure_{L"HMI_TEST_SDK_FAILURE", L""};
    AsrEnvironment asr_failure_{L"HMI_TEST_ASR_MODE", L""};

public:
    explicit FixtureSession(hmi::engine::AsrConfig config = {})
    {
        config.model_path = "fixture.gguf";
        config.backend = "cpu";
        opened = client.open(&error) && asr.open(client, config, &error);
        if (!opened) std::fprintf(stderr, "ASR fixture setup failed: %s\n", error.c_str());
    }
    bool mode(const wchar_t *value) const { return asr_failure_.set(value); }

    hmi::engine::Client client;
    hmi::engine::AsrSession asr;
    std::string error;
    bool opened = false;
};
}

HMI_TEST(asr_stream_emits_replaceable_full_prefixes_and_one_refined_final)
{
    FixtureSession fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.statistics().backend == "cpu");
    CHECK(fixture.asr.statistics().threads == 4);
    CHECK(fixture.asr.preferred_chunk_samples() == 4000);
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    for (int seconds = 1; seconds <= 5; ++seconds)
    {
        updates.clear();
        CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
        CHECK(updates.size() == 1);
        if (updates.size() != 1) return;
        CHECK(!updates.front().final);
        CHECK(updates.front().text == (seconds < 4 ? "early draft" : "corrected complete prefix"));
    }

    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "final refined phrase" && final.confidence == -1.0f);
    const auto stats = fixture.asr.statistics();
    CHECK(stats.audio_frames == 80000 && stats.preview_count == 5);
    CHECK(stats.preview_ms >= 0.0 && stats.refine_ms >= 0.0);
    CHECK(!fixture.asr.finish(final, &fixture.error));
}

HMI_TEST(asr_stream_bounds_audio_and_rejects_nonfinite_pcm_without_advancing_audio)
{
    hmi::engine::AsrConfig config;
    config.maximum_audio_seconds = 1;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    std::vector<hmi::engine::Transcript> updates;
    CHECK(!fixture.asr.push(nullptr, 1, updates, &fixture.error));
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    CHECK(!fixture.asr.push(&invalid, 1, updates, &fixture.error));
    CHECK(fixture.asr.statistics().audio_frames == 0);
    const std::vector<float> second(16000, 0.125f);
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(!fixture.asr.push(second.data(), 1, updates, &fixture.error));
    CHECK(fixture.asr.statistics().audio_frames == 16000);
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && !final.text.empty());
}

HMI_TEST(asr_stream_slow_backend_skips_previews_but_still_refines_the_complete_utterance)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 0.01;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.mode(L"slow"));
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> chunk(160, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.push(chunk.data(), chunk.size(), updates, &fixture.error));
    CHECK(updates.size() == 1);
    updates.clear();
    CHECK(fixture.asr.push(chunk.data(), chunk.size(), updates, &fixture.error));
    CHECK(updates.empty());
    CHECK(fixture.asr.statistics().preview_count == 1);
    CHECK(fixture.asr.statistics().preview_ms >= 15.0);
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    const auto stats = fixture.asr.statistics();
    CHECK(stats.audio_frames == 320 && stats.preview_count == 1 && stats.refine_ms >= 15.0);
    CHECK(final.final && !final.text.empty());
}

HMI_TEST(asr_preview_budget_expands_for_slow_decode_and_recovers_after_fast_decode)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 0.01;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const auto delay_for = [](double cost) {
        return std::max<std::size_t>(160, static_cast<std::size_t>(std::ceil(std::max(0.0, cost) * 32.0)));
    };
    const std::vector<float> first(160, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.push(first.data(), first.size(), updates, &fixture.error));
    CHECK(updates.size() == 1);
    const double first_cost = fixture.asr.statistics().preview_ms;

    CHECK(fixture.mode(L"slow"));
    const std::vector<float> until_slow(delay_for(first_cost), 0.125f);
    updates.clear();
    CHECK(fixture.asr.push(until_slow.data(), until_slow.size(), updates, &fixture.error));
    CHECK(updates.size() == 1);
    const double slow_total = fixture.asr.statistics().preview_ms;
    const double slow_cost = slow_total - first_cost;
    CHECK(slow_cost >= 15.0);
    // The reported cumulative sum can round by a fraction of a sample when
    // subtracting the first decode; bracket the observed budget by one sample.
    const std::vector<float> until_fast(delay_for(slow_cost) + 1, 0.125f);

    CHECK(fixture.mode(L"report_frames"));
    updates.clear();
    CHECK(fixture.asr.push(until_fast.data(), until_fast.size() - 3, updates, &fixture.error));
    CHECK(updates.empty());
    CHECK(fixture.asr.statistics().preview_count == 2);
    CHECK(fixture.asr.push(until_fast.data() + until_fast.size() - 3, 3, updates, &fixture.error));
    CHECK(updates.size() == 1);
    const double fast_cost = fixture.asr.statistics().preview_ms - slow_total;
    // One extra sample absorbs subtraction rounding in the reported sum. The
    // next draft must recover without carrying any earlier interval growth.
    const auto restored_delay = delay_for(fast_cost) + 1;
    CHECK(restored_delay < until_fast.size());
    const std::vector<float> restored(restored_delay, 0.125f);
    updates.clear();
    CHECK(fixture.asr.push(restored.data(), restored.size(), updates, &fixture.error));
    CHECK(updates.size() == 1 && !updates.front().final);
    CHECK(fixture.asr.statistics().preview_count == 4);
    const auto all_frames = first.size() + until_slow.size() + until_fast.size() + restored.size();
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "frames=" + std::to_string(all_frames));
    CHECK(fixture.asr.statistics().audio_frames == all_frames);
}

HMI_TEST(asr_stream_reset_and_close_clear_counters_and_no_refine_reuses_a_complete_snapshot)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 1.0;
    config.refine = false;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.size() == 1);
    CHECK(fixture.mode(L"finish_failure"));
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "early draft");
    CHECK(fixture.asr.statistics().refine_ms == 0.0);
    fixture.asr.reset();
    CHECK(fixture.asr.statistics().backend == "cpu");
    CHECK(fixture.asr.statistics().audio_frames == 0 && fixture.asr.statistics().preview_count == 0);
    fixture.asr.close();
    CHECK(fixture.asr.statistics().backend.empty() && fixture.asr.statistics().threads == 0);
    CHECK(!fixture.asr.ready());
}

HMI_TEST(asr_preview_uses_a_fixed_two_second_interval_without_growth)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 2.0;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    for (int seconds = 1; seconds <= 6; ++seconds)
    {
        // A decode would fail between configured boundaries, proving those
        // feeds only retain PCM rather than making a hidden SDK request.
        CHECK(fixture.mode(seconds % 2 == 0 ? L"" : L"finish_failure"));
        updates.clear();
        CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
        CHECK(updates.size() == (seconds % 2 == 0 ? 1u : 0u));
        if (!updates.empty()) CHECK(!updates.front().final);
    }
    CHECK(fixture.asr.statistics().audio_frames == 96000);
    CHECK(fixture.asr.statistics().preview_count == 3);
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "final refined phrase");
}

HMI_TEST(asr_long_recording_keeps_live_previews_regular_and_the_complete_final_audio)
{
    FixtureSession fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.mode(L"report_frames"));
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    std::vector<int> preview_seconds;
    for (int seconds = 1; seconds <= 30; ++seconds)
    {
        updates.clear();
        CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
        CHECK(updates.size() == 1);
        for (const auto &preview : updates)
        {
            CHECK(!preview.final);
            CHECK(preview.text == "frames=" + std::to_string(seconds * 16000));
            preview_seconds.push_back(seconds);
        }
    }
    CHECK(!preview_seconds.empty());
    if (preview_seconds.empty()) return;
    CHECK(preview_seconds.size() == 30);
    for (std::size_t index = 0; index < preview_seconds.size(); ++index)
        CHECK(preview_seconds[index] == static_cast<int>(index) + 1);
    // Regular full-prefix drafts intentionally increase compute on a fast
    // backend. Every draft and the final still receive all audio so far.
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "frames=480000");
    const auto stats = fixture.asr.statistics();
    CHECK(stats.audio_frames == 30 * 16000);
    CHECK(stats.preview_count == preview_seconds.size());
    CHECK(!fixture.asr.finish(final, &fixture.error));
}

HMI_TEST(asr_preview_budget_respects_a_longer_configured_minimum_interval)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 5.0;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    for (int seconds = 1; seconds <= 12; ++seconds)
    {
        updates.clear();
        CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
        CHECK(updates.size() == (seconds == 5 || seconds == 10 ? 1u : 0u));
    }
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "corrected complete prefix");
    CHECK(fixture.asr.statistics().preview_count == 2);
}

HMI_TEST(asr_ended_capture_drains_all_pending_audio_without_preview_requests)
{
    FixtureSession fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    // Simulate an already-ended capture with six seconds still in the ring.
    // A preview decode would fail in this mode; every chunk must only append.
    CHECK(fixture.mode(L"finish_failure"));
    for (int seconds = 0; seconds < 6; ++seconds)
        CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error, false));
    CHECK(updates.empty());
    CHECK(fixture.asr.statistics().preview_count == 0);
    CHECK(fixture.asr.statistics().audio_frames == 96000);
    CHECK(fixture.mode(L"report_frames"));
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "frames=96000");
    CHECK(fixture.asr.statistics().refine_ms >= 0.0);
    CHECK(!fixture.asr.finish(final, &fixture.error));
}

HMI_TEST(asr_ended_capture_keeps_audio_after_the_last_live_preview)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 2.0;
    config.refine = false;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.size() == 1 && !updates.front().final);
    updates.clear();
    CHECK(fixture.mode(L"finish_failure"));
    for (int seconds = 0; seconds < 4; ++seconds)
        CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error, false));
    CHECK(updates.empty() && fixture.asr.statistics().preview_count == 1);
    CHECK(fixture.mode(L"report_frames"));
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "frames=96000");
    CHECK(fixture.asr.statistics().audio_frames == 96000);
}

HMI_TEST(asr_final_refinement_runs_even_when_a_preview_covers_the_exact_audio)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 1.0;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.size() == 1);
    CHECK(fixture.mode(L"finish_failure"));
    hmi::engine::Transcript final;
    CHECK(!fixture.asr.finish(final, &fixture.error));
    CHECK(fixture.error.find("finish ASR prefix") != std::string::npos);
    CHECK(!fixture.asr.finish(final, &fixture.error));
}

HMI_TEST(asr_no_refine_cannot_reuse_a_preview_that_omits_later_audio)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 2.0;
    config.refine = false;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.size() == 1 && updates.front().text == "early draft");
    updates.clear();
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.empty());
    CHECK(fixture.mode(L"report_frames"));
    hmi::engine::Transcript final;
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && final.text == "frames=48000");
    CHECK(fixture.asr.statistics().audio_frames == 48000);
}

HMI_TEST(asr_reset_restarts_the_first_draft_budget_for_the_next_utterance)
{
    FixtureSession fixture;
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(fixture.asr.begin(&fixture.error));
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.size() == 2);
    fixture.asr.reset();
    updates.clear();
    CHECK(fixture.asr.begin(&fixture.error));
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(updates.size() == 1 && !updates.front().final);
    CHECK(fixture.asr.statistics().preview_count == 1);
    CHECK(fixture.asr.statistics().audio_frames == 16000);
}

HMI_TEST(asr_stream_sdk_failure_requires_a_new_begin_and_keeps_the_session_reusable)
{
    hmi::engine::AsrConfig config;
    config.preview_interval_seconds = 1.0;
    FixtureSession fixture(config);
    CHECK(fixture.opened);
    if (!fixture.opened) return;
    CHECK(fixture.asr.begin(&fixture.error));
    CHECK(fixture.mode(L"push_failure"));
    const std::vector<float> second(16000, 0.125f);
    std::vector<hmi::engine::Transcript> updates;
    CHECK(!fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(fixture.error.find("ASR prefix push") != std::string::npos);
    CHECK(!fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    hmi::engine::Transcript final;
    CHECK(!fixture.asr.finish(final, &fixture.error));
    CHECK(fixture.mode(L""));
    CHECK(fixture.asr.begin(&fixture.error));
    CHECK(fixture.asr.push(second.data(), second.size(), updates, &fixture.error));
    CHECK(fixture.asr.finish(final, &fixture.error));
    CHECK(final.final && !final.text.empty());
}
