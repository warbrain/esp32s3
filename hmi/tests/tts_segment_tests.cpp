#include "engine/tts_session.hpp"
#include "audio/stream_playback.hpp"
#include "audio/playback_diagnostics.hpp"
#include "environment.hpp"
#include "base/logger.hpp"
#include "util/json_reader.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
bool wait_until(Predicate predicate)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

struct PlaybackProbe
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<unsigned> played;
    bool hold_first = false;
    bool released = false;
    bool fail = false;
    bool entered = false;
    bool cancelled_seen = false;

    void reset(bool hold = false, bool fail_playback = false)
    {
        std::lock_guard lock(mutex);
        played.clear();
        hold_first = hold;
        fail = fail_playback;
        released = entered = cancelled_seen = false;
    }

    bool wait_played()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [this] { return !played.empty(); });
    }

    bool wait_cancelled()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [this] { return cancelled_seen; });
    }

    void release()
    {
        { std::lock_guard lock(mutex); released = true; }
        changed.notify_all();
    }

    std::vector<unsigned> ids()
    {
        std::lock_guard lock(mutex);
        return played;
    }
};

// This executable substitutes playback, so no WASAPI object or speaker opens.
PlaybackProbe playback;

class Fixture
{
public:
    explicit Fixture(const std::filesystem::path &path)
    {
        std::filesystem::path resolved;
        std::string error;
        require(hmi::base::resolve_project_path(std::filesystem::absolute(path),
                hmi::base::process_directory(), resolved, &error), error.c_str());
        module_ = LoadLibraryExW(resolved.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(module_ != nullptr, "cannot load the local TTS fixture");
        try
        {
            reset = symbol<Reset>("hmi_tts_fixture_reset");
            wait_blocked = symbol<WaitBlocked>("hmi_tts_fixture_wait_blocked");
            release = symbol<Release>("hmi_tts_fixture_release");
            count = symbol<Count>("hmi_tts_fixture_count");
            text = symbol<Text>("hmi_tts_fixture_text");
            option = symbol<Option>("hmi_tts_fixture_option");
            generation = symbol<Generation>("hmi_tts_fixture_generation");
        }
        catch (...)
        {
            FreeLibrary(module_);
            module_ = nullptr;
            throw;
        }
    }
    ~Fixture() { if (module_) FreeLibrary(module_); }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    std::string generated_text(unsigned index) const
    {
        const auto length = text(index, nullptr, 0);
        std::vector<char> bytes(length + 1, '\0');
        require(text(index, bytes.data(), bytes.size()) == length, "fixture text changed unexpectedly");
        return {bytes.data(), length};
    }

    std::string request_option(unsigned index, const char *key) const
    {
        const auto length = option(index, key, nullptr, 0);
        std::vector<char> bytes(length + 1, '\0');
        require(option(index, key, bytes.data(), bytes.size()) == length, "fixture option changed unexpectedly");
        return {bytes.data(), length};
    }

    using Reset = void (*)(unsigned, unsigned);
    using WaitBlocked = int (*)(unsigned);
    using Release = void (*)();
    using Count = unsigned (*)(unsigned);
    using Text = std::size_t (*)(unsigned, char *, std::size_t);
    using Option = std::size_t (*)(unsigned, const char *, char *, std::size_t);
    using Generation = void (*)(unsigned);
    Reset reset = nullptr;
    WaitBlocked wait_blocked = nullptr;
    Release release = nullptr;
    Count count = nullptr;
    Text text = nullptr;
    Option option = nullptr;
    Generation generation = nullptr;

private:
    template<class Function>
    Function symbol(const char *name)
    {
        const auto address = GetProcAddress(module_, name);
        require(address != nullptr, "TTS fixture hook is missing");
        Function result = nullptr;
        static_assert(sizeof(result) == sizeof(address));
        std::memcpy(&result, &address, sizeof(result));
        return result;
    }
    HMODULE module_ = nullptr;
};

class ScopedEnvironment
{
public:
    ScopedEnvironment(const wchar_t *name, const std::filesystem::path &value) : name_(name)
    {
        wchar_t *previous = nullptr;
        std::size_t count = 0;
        _wdupenv_s(&previous, &count, name);
        existed_ = previous != nullptr;
        if (previous) previous_ = previous;
        std::free(previous);
        require(_wputenv_s(name, value.c_str()) == 0 && SetEnvironmentVariableW(name, value.c_str()),
                "cannot configure the process-local fixture path");
    }
    ~ScopedEnvironment()
    {
        _wputenv_s(name_.c_str(), previous_.c_str());
        SetEnvironmentVariableW(name_.c_str(), existed_ ? previous_.c_str() : nullptr);
    }
private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

class ConfigurationDirectory
{
public:
    ConfigurationDirectory()
    {
        const auto nonce = Clock::now().time_since_epoch().count();
        path = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
            (L"hmi-tts-segments-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(nonce));
        std::filesystem::create_directories(path);
        std::ofstream output(path / "engine.json", std::ios::binary);
        output << "{\"families\":\"qwen3_tts\"}\n";
        require(static_cast<bool>(output), "cannot create project-local fixture configuration");
    }
    ~ConfigurationDirectory()
    {
        std::filesystem::path checked;
        if (!hmi::base::resolve_project_path(path, hmi::base::process_directory(), checked) ||
            checked != path || !path.filename().native().starts_with(L"hmi-tts-segments-")) return;
        std::error_code ignored;
        std::filesystem::remove_all(checked, ignored);
    }
    std::filesystem::path path;
};

struct Session
{
    explicit Session(Fixture &selected, const std::string &backend = "cpu", hmi::base::Logger *logger = nullptr,
                     const std::string &language = "zh", const std::string &instruction = {},
                     const std::string &voice = "serena") : fixture(selected)
    {
        std::string error;
        require(client.open(&error), error.c_str());
        hmi::engine::TtsConfig config;
        config.model_path = "fixture.gguf";
        config.backend = backend;
        config.language = language;
        config.instruction = instruction;
        config.voice_id = voice;
        config.logger = logger;
        config.run_id = "fixture-run";
        require(tts.open(client, config, &error), error.c_str());
    }
    ~Session()
    {
        // Release barriers before joining even if an assertion threw.
        playback.release();
        fixture.release();
        tts.stop();
        client.close();
    }
    Fixture &fixture;
    hmi::engine::Client client;
    hmi::engine::TtsSession tts;
};

void require_voice_policy(const Fixture &fixture, unsigned index, bool sampled,
                          const std::string &language = {}, const std::string &voice = "serena",
                          const std::string &instruction = {})
{
    const char *mode = sampled ? "true" : "false";
    require(fixture.request_option(index, "do_sample") == mode &&
                fixture.request_option(index, "subtalker_do_sample") == mode &&
                fixture.request_option(index, "seed") == "42" &&
                fixture.request_option(index, "temperature") == "0.9" &&
                fixture.request_option(index, "top_k") == "50" &&
                fixture.request_option(index, "top_p") == "1.0" &&
                fixture.request_option(index, "repetition_penalty") == "1.05" &&
                fixture.request_option(index, "subtalker_temperature") == "0.9" &&
                fixture.request_option(index, "subtalker_top_k") == "50" &&
                fixture.request_option(index, "subtalker_top_p") == "1.0",
            "an SDK request changed the utterance sampler, seed or sampling parameters");
    require((language.empty() || fixture.request_option(index, "fixture.text_language") == language) &&
                fixture.request_option(index, "fixture.voice_id") == voice &&
                fixture.request_option(index, "instruction") == instruction,
            "an SDK request changed the utterance language, voice or instruction");
}

class PreparationLog
{
public:
    PreparationLog()
    {
        hmi::base::LoggerOptions options;
        options.file = directory_.path / "prepare.log";
        options.level = hmi::base::LogLevel::Debug;
        std::string error;
        require(logger.configure(options, &error), error.c_str());
    }

    bool finished(std::uint64_t request_id, std::string_view result)
    {
        logger.flush();
        std::ifstream input(logger.options().file, std::ios::binary);
        std::string line;
        const auto identity = " request_id=" + std::to_string(request_id) + " ";
        const auto outcome = " result=" + std::string(result) + " ";
        while (std::getline(input, line))
            if (line.find("event=prepare_finished ") != std::string::npos &&
                line.find(identity) != std::string::npos && line.find(outcome) != std::string::npos) return true;
        return false;
    }

private:
    ConfigurationDirectory directory_;
public:
    hmi::base::Logger logger;
};

void require_same_speech_statistics(const hmi::engine::TtsStatistics &before,
                                    const hmi::engine::TtsStatistics &after)
{
    require(before.request_id == after.request_id && before.segments == after.segments &&
                before.generated_segments == after.generated_segments &&
                before.generated_audio_frames == after.generated_audio_frames && before.audio_seconds == after.audio_seconds &&
                before.generation_retries == after.generation_retries && before.limit_hits == after.limit_hits &&
                before.last_finish_reason == after.last_finish_reason &&
                before.pipeline_finish_reason == after.pipeline_finish_reason && before.queue_high_water == after.queue_high_water &&
                before.first_audio_ms == after.first_audio_ms && before.synthesis_ms == after.synthesis_ms &&
                before.total_ms == after.total_ms && before.queue_ms == after.queue_ms && before.model_load_ms == after.model_load_ms &&
                before.last_request_cache_hits == after.last_request_cache_hits && before.utterance_cache_hits == after.utterance_cache_hits &&
                before.recovery_splits == after.recovery_splits && before.recovery_attempts == after.recovery_attempts &&
                before.recovery_exhausted == after.recovery_exhausted && before.cache_entries == after.cache_entries &&
                before.cache_bytes == after.cache_bytes && before.cache_hits == after.cache_hits &&
                before.sampling_enabled == after.sampling_enabled && before.sampling_fallbacks == after.sampling_fallbacks &&
                before.max_tokens == after.max_tokens && before.producer_wait_ms == after.producer_wait_ms &&
                before.consumer_wait_ms == after.consumer_wait_ms && before.consumer_pending_reads == after.consumer_pending_reads &&
                before.consumed_chunks == after.consumed_chunks && before.playback.finalized == after.playback.finalized &&
                hmi::util::json::stringify(hmi::audio::detail::playback_fields(before.playback)) ==
                    hmi::util::json::stringify(hmi::audio::detail::playback_fields(after.playback)),
            "preparation changed speech identity, timings, inference, PCM, cache or playback statistics");
}

void preparation_is_nonblocking_coalesced_and_idle_released(Fixture &fixture)
{
    fixture.reset(~0U, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    PreparationLog log;
    Session session(fixture, "cpu", &log.logger);
    const auto before = session.tts.statistics();
    const auto started = Clock::now();
    require(session.tts.prepare(9601), "load-only preparation was rejected");
    require(Clock::now() - started < std::chrono::milliseconds(250), "preparation blocked its caller on model loading");
    require(fixture.wait_blocked(5000) != 0, "preparation did not reach the model-load barrier");
    for (std::uint64_t id = 9602; id != 9650; ++id)
        require(session.tts.prepare(id), "repeated preparation was rejected instead of coalesced");
    require(!session.tts.busy() && fixture.count(0) == 1 && fixture.count(1) == 0 && fixture.count(2) == 0,
            "preparation became speech, duplicated model loads or ran SDK inference");
    hmi::audio::AudioBuffer audio;
    audio.samples = {.5f};
    std::string error;
    const auto sync_started = Clock::now();
    require(!session.tts.synthesize("同步请求不能插入准备。", audio, &error) && !error.empty() && audio.samples.empty() &&
                Clock::now() - sync_started < std::chrono::milliseconds(250),
            "synchronous synthesis waited for or entered in-flight preparation");
    require_same_speech_statistics(before, session.tts.statistics());
    require(session.tts.last_error().empty(), "synchronous rejection or preparation polluted the last speech error");
    fixture.release();
    require(wait_until([&] { return log.finished(9601, "ready"); }), "load-only preparation did not finish");
    const auto ready = session.tts.statistics();
    require(ready.model_ready && ready.backend == "cpu" && !session.tts.busy() &&
                fixture.count(0) == 1 && fixture.count(1) == 1 && fixture.count(2) == 0 && playback.ids().empty(),
            "completed preparation synthesized PCM or failed to expose its model");
    require_same_speech_statistics(before, ready);
    { std::lock_guard lock(playback.mutex); require(!playback.entered, "preparation opened playback without speech"); }
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }),
            "preparation without speech never scheduled idle model release");
    require(!session.tts.statistics().model_ready && fixture.count(4) == 1 && fixture.count(5) == 1 && fixture.count(2) == 0,
            "idle preparation retained its model/session or generated dummy text");
    require_same_speech_statistics(before, session.tts.statistics());
}

void preparation_preserves_completed_speech_and_cached_pcm(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    PreparationLog log;
    Session session(fixture, "cpu", &log.logger);
    const std::string text = "保留上一句的统计和缓存。";
    require(session.tts.speak(text, 9661), "preparation statistics precondition was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "preparation statistics precondition stayed busy");
    const auto before = session.tts.statistics();
    const auto played = playback.ids();
    require(before.request_id == 9661 && before.playback.drained && before.cache_entries == 1 && before.model_load_ms > 0.0,
            "preparation statistics precondition did not populate speech metrics");
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }), "preparation precondition did not unload");
    require(session.tts.prepare(9662), "preparation after completed speech was rejected");
    require(wait_until([&] { return log.finished(9662, "ready"); }), "preparation after completed speech did not finish");
    require_same_speech_statistics(before, session.tts.statistics());
    require(session.tts.statistics().model_ready && fixture.count(0) == 2 && fixture.count(1) == 2 &&
                fixture.count(2) == 1 && playback.ids() == played && session.tts.last_error().empty(),
            "preparation replaced completed speech or generated warm-up audio");
    require(session.tts.prepare(9663), "already-ready preparation was rejected");
    require(session.tts.speak(text, 9664), "cached speech after preparation was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "cached speech after preparation stayed busy");
    require(fixture.count(0) == 2 && fixture.count(2) == 1 && session.tts.statistics().utterance_cache_hits == 1 &&
                session.tts.statistics().request_id == 9664 && session.tts.statistics().playback.drained,
            "preparation lost cached PCM or polluted the next real speech");
}

void speech_during_blocked_preparation_reuses_one_load(Fixture &fixture)
{
    fixture.reset(~0U, 0);
    playback.reset();
    PreparationLog log;
    Session session(fixture, "cpu", &log.logger);
    require(session.tts.prepare(9701), "speech priority preparation was rejected");
    require(fixture.wait_blocked(5000) != 0, "speech priority preparation missed its barrier");
    const std::string text = "真实朗读优先。";
    require(session.tts.speak(text, 9702), "speech during preparation was rejected");
    require(session.tts.prepare(9703), "preparation beside queued speech was rejected");
    require(session.tts.busy() && session.tts.statistics().request_id == 9702 && fixture.count(2) == 0,
            "preparation hid or replaced the actual queued speech");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    fixture.release();
    require(wait_until([&] { return !session.tts.busy(); }), "speech during preparation stayed busy");
    require(wait_until([&] { return log.finished(9701, "superseded"); }), "old preparation was not invalidated by actual speech");
    const auto measured = session.tts.statistics();
    require(fixture.count(0) == 1 && fixture.count(1) == 1 && fixture.count(2) == 1 &&
                fixture.generated_text(1) == text && playback.ids() == std::vector<unsigned>{1},
            "speech during preparation reloaded its model, inferred dummy text or lost priority");
    require(measured.request_id == 9702 && measured.model_ready && measured.model_load_ms == 0.0 &&
                measured.generated_segments == 1 && measured.queue_ms >= 30.0 && measured.first_audio_ms >= measured.queue_ms &&
                measured.total_ms >= measured.first_audio_ms && measured.playback.drained &&
                session.tts.last_error().empty(), "preparation contaminated actual speech timing or completion");
}

void preparation_cancel_is_nonblocking_and_new_preparation_survives(Fixture &fixture)
{
    fixture.reset(~0U, 0);
    playback.reset();
    PreparationLog log;
    Session session(fixture, "cpu", &log.logger);
    require(session.tts.prepare(9801), "cancel preparation was rejected");
    require(fixture.wait_blocked(5000) != 0, "cancel preparation missed its barrier");
    const auto started = Clock::now();
    session.tts.cancel("recording_stopped");
    require(Clock::now() - started < std::chrono::milliseconds(250) && !session.tts.busy(),
            "cancel waited for the preparation SDK call or reported speech busy");
    require(session.tts.prepare(9802), "new preparation after cancellation was rejected");
    fixture.release();
    require(wait_until([&] { return log.finished(9801, "superseded") && log.finished(9802, "ready"); }),
            "cancelled preparation resumed or invalidated the newer preparation");
    require(fixture.count(0) == 1 && fixture.count(1) == 1 && fixture.count(2) == 0 && playback.ids().empty() &&
                session.tts.statistics().request_id == 0 && session.tts.statistics().cache_entries == 0,
            "cancelled preparation synthesized audio, polluted statistics or discarded its reusable model");
    require(session.tts.speak("取消准备后仍可朗读。", 9803), "speech after preparation cancellation was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "speech after preparation cancellation stayed busy");
    require(fixture.count(0) == 1 && fixture.count(2) == 1 && session.tts.statistics().request_id == 9803 &&
                session.tts.statistics().playback.drained && session.tts.last_error().empty(),
            "preparation cancellation poisoned subsequent speech");
}

void queued_preparation_is_cleared_by_cancel(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    Session session(fixture);
    const std::string text = "缓存朗读。";
    require(session.tts.speak(text, 9811), "queued preparation precondition was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "queued preparation precondition stayed busy");
    require(fixture.count(2) == 1 && fixture.generated_text(1) == text && session.tts.statistics().cache_entries == 1,
            "queued preparation precondition must cache exactly the same single request as direct synthesis");
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }), "queued preparation precondition did not unload");
    playback.reset(true);
    require(session.tts.speak(text, 9812), "cached playback precondition was rejected");
    require(playback.wait_played(), "cached playback did not hold the worker");
    require(!session.tts.statistics().model_ready && session.tts.prepare(9813), "unloaded cached playback did not queue preparation");
    session.tts.cancel();
    require(wait_until([&] { return !session.tts.busy(); }), "cancelled cached playback stayed busy");
    hmi::audio::AudioBuffer audio;
    std::string error;
    require(session.tts.synthesize(text, audio, &error), "cancel retained queued preparation and blocked synchronous cache reuse");
    require(fixture.count(0) == 1 && fixture.count(1) == 1 && fixture.count(2) == 1 && !session.tts.statistics().model_ready,
            "cancelled queued preparation loaded a model or regenerated cached speech");
}

void preparation_stop_and_reopen_release_without_audio(Fixture &fixture)
{
    fixture.reset(~0U, 0);
    playback.reset();
    Session session(fixture);
    require(session.tts.prepare(9821), "stop preparation was rejected");
    require(fixture.wait_blocked(5000) != 0, "stop preparation missed its barrier");
    std::thread stopper([&] { session.tts.stop(); });
    const bool stopping = wait_until([&] { return !session.tts.prepare(9822); });
    fixture.release();
    stopper.join();
    require(stopping && !session.tts.prepare(9823) && !session.tts.busy() && !session.tts.statistics().model_ready &&
                fixture.count(0) == 1 && fixture.count(4) == 1 && fixture.count(5) == 1 &&
                fixture.count(2) == 0 && playback.ids().empty(),
            "stop accepted preparation, leaked model/session or published audio");
    hmi::engine::TtsConfig config;
    config.model_path = "fixture.gguf";
    config.backend = "cpu";
    std::string error;
    require(session.tts.open(session.client, config, &error), error.c_str());
    require(session.tts.speak("停止准备后重新打开。", 9824), "reopened speech was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "reopened speech stayed busy");
    require(fixture.count(0) == 2 && fixture.count(2) == 1 && session.tts.statistics().request_id == 9824 &&
                session.tts.statistics().playback.drained && session.tts.last_error().empty(),
            "stopped preparation poisoned the reopened service");
}

void failed_preparation_does_not_poison_speech(Fixture &fixture)
{
    fixture.reset(~0U, ~0U);
    playback.reset();
    PreparationLog log;
    Session session(fixture, "cpu", &log.logger);
    const auto before = session.tts.statistics();
    require(session.tts.prepare(9831), "failing preparation was rejected");
    require(fixture.wait_blocked(5000) != 0, "failing preparation missed its model-load barrier");
    fixture.release();
    require(wait_until([&] { return log.finished(9831, "failed"); }), "failing preparation did not finish");
    require_same_speech_statistics(before, session.tts.statistics());
    require(!session.tts.busy() && !session.tts.statistics().model_ready && session.tts.last_error().empty() &&
                fixture.count(0) == 1 && fixture.count(1) == 0 && fixture.count(2) == 0 && playback.ids().empty(),
            "preparation failure became a speech error, ready model or generated audio");
    require(session.tts.speak("准备失败后正常朗读。", 9832), "speech after preparation failure was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "speech after preparation failure stayed busy");
    require(fixture.count(0) == 2 && fixture.count(1) == 1 && fixture.count(2) == 1 &&
                session.tts.statistics().request_id == 9832 && session.tts.statistics().playback.drained &&
                session.tts.last_error().empty(), "preparation failure poisoned the next real speech");
}

std::string long_text()
{
    return "第一段用于检查首段能够提前播放。第二段用于检查生成和播放可以重叠。"
           "第三段包含中文标点和完整的自然句子。第四段用于检查后续音频仍然保持顺序。"
           "第五段包含数字123和英文Audio以及表情🙂。第六段用于检查不会丢失任何文字。"
           "第七段继续检查同一个模型会话得到复用。最后一段用于确认整段朗读正确结束。";
}

void early_playback_and_session_reuse(Fixture &fixture)
{
    fixture.reset(2, 0);
    playback.reset();
    Session session(fixture);
    const auto message = long_text();
    require(session.tts.speak(message), "segmented speech was rejected");
    require(fixture.wait_blocked(5000) != 0, "second segment never reached the SDK barrier");
    require(playback.wait_played(), "first segment was not played while the next SDK call was blocked");
    require(fixture.count(3) == 1, "the full utterance completed before first playback");
    require(session.tts.busy(), "busy became false with unfinished synthesis");
    fixture.release();
    require(wait_until([&] { return !session.tts.busy(); }), "segmented speech did not finish");
    require(session.tts.last_error().empty(), "successful segmented speech retained an error");
    const auto ids = playback.ids();
    require(ids.size() >= 2, "long speech was still synthesized as a single request");
    require(ids.size() == fixture.count(2), "one or more synthesized segments were not played");
    std::string reconstructed;
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        require(ids[index] == index + 1, "segment playback order changed");
        reconstructed += fixture.generated_text(ids[index]);
    }
    require(reconstructed == message, "segmentation lost, duplicated or modified UTF-8 text");
    require(fixture.count(0) == 1 && fixture.count(1) == 1, "segments reloaded the model or session");
    const auto measured = session.tts.statistics();
    require(measured.model_ready && measured.backend == "cpu", "completed speech lost its model status");
    require(measured.segments == ids.size() && measured.generated_segments == ids.size(),
            "completed speech statistics lost a segment");
    require(measured.queue_high_water > 0 && measured.queue_high_water <= 2,
            "speech statistics did not preserve the bounded PCM queue");
    require(measured.first_audio_ms > 0.0 && measured.synthesis_ms > 0.0 &&
            measured.total_ms >= measured.first_audio_ms, "speech timings were not published");
    require(session.tts.speak("再次朗读也复用同一会话。"), "second utterance was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "second utterance did not finish");
    require(fixture.count(0) == 1 && fixture.count(1) == 1, "second utterance reloaded the model");
    session.tts.stop();
    require(fixture.count(4) == 1 && fixture.count(5) == 1, "stop did not release the retained model and session");
}

void busy_covers_playback(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset(true);
    Session session(fixture);
    require(session.tts.speak("播放尚未结束。"), "short speech was rejected");
    require(playback.wait_played(), "playback did not reach its barrier");
    require(fixture.count(3) == 1, "short speech was not synthesized once");
    require(session.tts.busy(), "busy ignored unfinished playback");
    playback.release();
    require(wait_until([&] { return !session.tts.busy(); }), "busy did not clear after playback");
    require(session.tts.last_error().empty(), "successful playback reported an error");
}

void replacement_discards_old_tail(Fixture &fixture)
{
    fixture.reset(2, 0);
    playback.reset(true);
    Session session(fixture);
    require(session.tts.speak(long_text()), "old speech was rejected");
    require(fixture.wait_blocked(5000) != 0 && playback.wait_played(), "old speech barriers were not reached");
    const std::string replacement = "这是替换后的新内容。";
    require(session.tts.speak(replacement), "replacement speech was rejected");
    const bool observed_cancel = playback.wait_cancelled();
    fixture.release();
    playback.release();
    require(observed_cancel, "replacement did not cancel old playback");
    require(wait_until([&] { return !session.tts.busy(); }), "replacement did not finish");
    const auto ids = playback.ids();
    require(ids.size() >= 2 && ids.front() == 1, "replacement lost its initial or new playback");
    require(std::find(ids.begin(), ids.end(), 2u) == ids.end(), "cancelled old segment was played");
    std::string played_new;
    for (const auto index : ids) if (index > 2) played_new += fixture.generated_text(index);
    require(played_new == replacement, "replacement content did not play exactly once");
    require(session.tts.last_error().empty(), "cancelled old generation contaminated replacement error state");
}

void stop_discards_inflight_tail(Fixture &fixture)
{
    fixture.reset(2, 0);
    playback.reset(true);
    Session session(fixture);
    require(session.tts.speak(long_text()), "speech before stop was rejected");
    require(fixture.wait_blocked(5000) != 0 && playback.wait_played(), "stop barriers were not reached");
    std::thread stopper([&] { session.tts.stop(); });
    const bool observed_cancel = playback.wait_cancelled();
    fixture.release();
    playback.release();
    stopper.join();
    require(observed_cancel, "stop did not cancel playback while SDK generation was in flight");
    require(!session.tts.busy(), "stop left speech busy");
    const auto ids = playback.ids();
    require(ids.size() == 1 && ids.front() == 1, "stop played an old synthesized tail");
    require(fixture.count(4) == 1 && fixture.count(5) == 1, "stop leaked model or session");
    require(!session.tts.speak("停止后不应接受朗读。"), "closed speech session accepted new work");
}

void cancel_is_nonblocking_and_service_can_resume(Fixture &fixture)
{
    for (const bool queue_another : {false, true})
    {
        fixture.reset(2, 0);
        playback.reset(true);
        Session session(fixture);
        require(session.tts.speak(long_text()), "speech before cancellation was rejected");
        require(fixture.wait_blocked(5000) != 0 && playback.wait_played(),
                "cancellation barriers were not reached");
        const auto published = session.tts.statistics();
        require(published.segments > 1 && published.generated_segments == 1 &&
                published.generated_audio_frames == 240 && published.audio_seconds > 0.0,
                "in-flight speech did not expose the published PCM progress");
        if (queue_another)
        {
            require(session.tts.speak("这个待处理任务必须被清除。"), "pending speech was rejected");
            const auto reset = session.tts.statistics();
            require(reset.generated_segments == 0 && reset.generated_audio_frames == 0 && reset.audio_seconds == 0.0,
                    "new speech retained statistics from the previous request");
        }
        const auto before_cancel = session.tts.statistics();
        const auto started = Clock::now();
        session.tts.cancel();
        require(Clock::now() - started < std::chrono::milliseconds(250),
                "cancel waited for the blocked SDK call");
        const auto cancelled = session.tts.statistics();
        require(cancelled.generated_segments == before_cancel.generated_segments &&
                cancelled.generated_audio_frames == before_cancel.generated_audio_frames &&
                cancelled.audio_seconds == before_cancel.audio_seconds,
                "cancel erased the current request's published PCM progress");
        const bool observed_cancel = playback.wait_cancelled();
        fixture.release();
        playback.release();
        require(observed_cancel, "cancel did not stop playback during SDK generation");
        require(wait_until([&] { return !session.tts.busy(); }), "cancel left speech busy");
        const auto finished = session.tts.statistics();
        require(finished.generated_segments == before_cancel.generated_segments &&
                finished.generated_audio_frames == before_cancel.generated_audio_frames &&
                finished.audio_seconds == before_cancel.audio_seconds,
                "cancelled generation overwrote the retained progress");
        require(fixture.count(2) == 2, "cancel did not discard pending speech or in-flight tail");
        require(playback.ids() == std::vector<unsigned>{1}, "cancel published stale PCM");
        require(fixture.count(4) == 0 && fixture.count(5) == 0,
                "cancel closed the reusable model or session");
        require(session.tts.speak("继续。"), "cancel permanently disabled the service");
        require(wait_until([&] { return !session.tts.busy(); }), "resumed speech did not finish");
        require(playback.ids() == std::vector<unsigned>({1, 3}), "resumed speech played stale PCM");
        require(session.tts.last_error().empty(), "cancel left a stale synthesis error");
    }
}

void synthesis_and_playback_failures(Fixture &fixture)
{
    fixture.reset(0, 2);
    playback.reset();
    {
        Session session(fixture);
        require(session.tts.speak(long_text()), "failing synthesis was not queued");
        require(wait_until([&] { return !session.tts.busy(); }), "failing synthesis stayed busy");
        require(session.tts.last_error().find("fixture synthesis failure") != std::string::npos,
                "SDK synthesis error was not propagated");
        require(fixture.count(2) == 2, "generation continued after a failed segment");
        require(session.tts.speak("错误之后可以再次朗读。"), "recovery utterance was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "recovery utterance did not finish");
        require(session.tts.last_error().empty(), "old synthesis error survived a successful utterance");
    }
    fixture.reset(0, 0);
    playback.reset(false, true);
    {
        Session session(fixture);
        require(session.tts.speak(long_text()), "failing playback was not queued");
        require(wait_until([&] { return !session.tts.busy(); }), "failing playback stayed busy");
        require(session.tts.last_error().find("fixture playback failure") != std::string::npos,
                "playback error was not propagated");
    }
}

void synchronous_synthesis_remains_complete(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    const auto message = long_text();
    require(session.tts.synthesize(message, audio, &error), error.c_str());
    require(fixture.count(2) == 1 && fixture.generated_text(1) == message,
            "synchronous synthesis no longer submits the complete request");
    require(audio.sample_rate == 24000 && audio.channels == 1 && audio.samples.size() == 240,
            "synchronous synthesis did not return owned complete PCM");
    require(std::fabs(audio.samples.front() - .001f) < .00001f, "returned PCM no longer owns the SDK result");
    { std::lock_guard lock(playback.mutex); require(!playback.entered, "synchronous synthesis started playback"); }
    require(session.tts.synthesize("第二次同步合成。", audio, &error), error.c_str());
    require(fixture.count(0) == 1 && fixture.count(1) == 1, "synchronous requests reloaded the retained model");
    const auto measured = session.tts.statistics();
    require(measured.model_ready && measured.backend == "cpu", "synchronous synthesis did not expose its loaded model");
    require(measured.segments == 0 && measured.generated_segments == 0 && measured.first_audio_ms == 0.0,
            "synchronous synthesis reported unplayed speech statistics");
}

void statistics_track_lazy_loading_and_backend_fallback(Fixture &fixture)
{
    fixture.reset(1, 0);
    playback.reset();
    Session session(fixture, "auto");
    auto measured = session.tts.statistics();
    require(!measured.model_ready && measured.backend.empty() && measured.segments == 0,
            "opening TTS falsely reported a loaded model");
    require(session.tts.speak("检查按需加载。"), "lazy speech was rejected");
    require(fixture.wait_blocked(5000) != 0, "first synthesis did not reach the SDK barrier");
    measured = session.tts.statistics();
    require(session.tts.busy() && measured.model_ready && measured.backend == "cpu",
            "model readiness and actual fallback backend were not visible during synthesis");
    require(measured.segments == 1 && measured.generated_segments == 0 &&
            measured.generated_audio_frames == 0 && measured.audio_seconds == 0.0 &&
            measured.queue_high_water == 0 && measured.first_audio_ms == 0.0 && measured.synthesis_ms == 0.0,
            "blocked first synthesis did not distinguish planned segments from generated PCM");
    measured.backend = "detached snapshot";
    require(session.tts.statistics().backend == "cpu", "a detached statistics copy changed the session");
    fixture.release();
    require(wait_until([&] { return !session.tts.busy(); }), "lazy speech did not finish");
    measured = session.tts.statistics();
    require(measured.model_ready && measured.backend == "cpu" && measured.segments == 1 &&
            measured.generated_segments == 1 && measured.generated_audio_frames == 240 &&
            std::fabs(measured.audio_seconds - 0.01) < 0.000001,
            "finished speech did not publish its model and generated PCM statistics");
    require(measured.first_audio_ms > 0.0 && measured.synthesis_ms > 0.0 &&
            measured.total_ms >= measured.first_audio_ms, "finished speech timings were invalid");
    session.tts.stop();
    measured = session.tts.statistics();
    require(!measured.model_ready && measured.backend.empty() && measured.segments == 0 &&
            measured.generated_segments == 0 && measured.generated_audio_frames == 0 && measured.audio_seconds == 0.0 &&
            measured.queue_high_water == 0 &&
            measured.first_audio_ms == 0.0 && measured.synthesis_ms == 0.0 && measured.total_ms == 0.0,
            "stop retained released model or stale speech statistics");
}

void statistics_distinguish_model_failure_and_synthesis_recovery(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    {
        Session session(fixture, "missing-backend");
        require(session.tts.speak("检查失败的模型会话。"), "model failure request was not queued");
        require(wait_until([&] { return !session.tts.busy(); }), "model failure stayed busy");
        const auto measured = session.tts.statistics();
        require(!measured.model_ready && measured.backend.empty() && measured.generated_segments == 0,
                "failed session creation reported a ready model");
        require(!session.tts.last_error().empty() && fixture.count(4) == 1,
                "failed session creation did not release the loaded model");
    }
    fixture.reset(0, 1);
    playback.reset();
    {
        Session session(fixture);
        require(session.tts.speak("检查合成失败。"), "synthesis failure request was not queued");
        require(wait_until([&] { return !session.tts.busy(); }), "synthesis failure stayed busy");
        auto measured = session.tts.statistics();
        require(measured.model_ready && measured.backend == "cpu" && measured.segments == 1 &&
                measured.generated_segments == 0 && measured.first_audio_ms == 0.0,
                "synthesis failure lost its retained model or reported nonexistent audio");
        playback.reset(true);
        require(session.tts.speak("检查恢复后的统计。"), "recovery request was not queued");
        require(playback.wait_played(), "recovery did not reach playback");
        require(wait_until([&] { return session.tts.statistics().generated_segments == 1; }),
                "recovery did not publish its first PCM progress");
        measured = session.tts.statistics();
        require(session.tts.busy() && measured.model_ready && measured.backend == "cpu" && measured.segments == 1 &&
                measured.generated_segments == 1 && measured.generated_audio_frames == 240 &&
                std::fabs(measured.audio_seconds - 0.01) < 0.000001 && measured.model_load_ms == 0.0 &&
                measured.queue_high_water == 1 && measured.first_audio_ms > 0.0 &&
                measured.total_ms >= measured.first_audio_ms,
                "recovery did not replace the failed request with its own live PCM statistics");
        playback.release();
        require(wait_until([&] { return !session.tts.busy(); }), "statistics recovery stayed busy");
        measured = session.tts.statistics();
        require(session.tts.last_error().empty() && measured.segments == 1 && measured.generated_segments == 1,
                "successful recovery retained failed speech statistics");
        require(fixture.count(0) == 1 && fixture.count(1) == 1, "recovery reloaded the retained model");
    }
}

void statistics_replacement_does_not_publish_the_cancelled_run(Fixture &fixture)
{
    fixture.reset(2, 0);
    playback.reset(true);
    Session session(fixture);
    require(session.tts.speak(long_text()), "old statistics request was rejected");
    require(fixture.wait_blocked(5000) != 0 && playback.wait_played(), "old speech barriers were not reached");
    require(session.tts.speak("只统计替换内容。"), "replacement statistics request was rejected");
    require(playback.wait_cancelled(), "replacement did not cancel old playback");
    const auto pending = session.tts.statistics();
    require(pending.segments == 0 && pending.generated_segments == 0 && pending.generated_audio_frames == 0 &&
            pending.audio_seconds == 0.0 && pending.first_audio_ms == 0.0 && pending.synthesis_ms == 0.0 &&
            pending.total_ms == 0.0 && pending.generation_retries == 0 && pending.limit_hits == 0 &&
            pending.last_finish_reason.empty(),
            "replacement retained statistics while the cancelled SDK request was still blocked");
    // Hold the replacement player after the old producer can complete. If the
    // old generation publishes its metrics, they remain visible at this point.
    playback.reset(true);
    fixture.release();
    require(playback.wait_played(), "replacement did not reach playback");
    require(wait_until([&] { return session.tts.statistics().generated_segments == 1; }),
            "replacement did not publish its first PCM progress");
    const auto ids = playback.ids();
    require(ids.size() == 1 && ids.front() >= 3, "replacement playback was confused with an old segment");
    require(fixture.count(2) == 3 && fixture.generated_text(3) == "只统计替换内容。",
            "replacement synthesis continued the cancelled request or generated the wrong text");
    auto measured = session.tts.statistics();
    require(session.tts.busy() && measured.model_ready && measured.backend == "cpu" && measured.segments == 1 &&
            measured.generated_segments == 1 && measured.generated_audio_frames == 240 &&
            std::fabs(measured.audio_seconds - 0.01) < 0.000001 && measured.queue_high_water == 1 &&
            measured.generation_retries == 0 && measured.limit_hits == 0 && measured.first_audio_ms > 0.0 &&
            measured.total_ms >= measured.first_audio_ms,
            "cancelled generation overwrote the replacement request's live PCM statistics");
    playback.release();
    require(wait_until([&] { return !session.tts.busy(); }), "replacement statistics stayed busy");
    measured = session.tts.statistics();
    require(measured.segments == 1 && measured.generated_segments == 1 && measured.generated_audio_frames == 240 &&
            std::fabs(measured.audio_seconds - 0.01) < 0.000001 &&
            measured.first_audio_ms > 0.0 && measured.queue_high_water <= 2,
            "replacement did not publish only its own completed statistics");
}

void bounded_pcm_cache_preserves_owned_audio_and_lru(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    const std::string first = "缓存内容一。";
    require(session.tts.synthesize(first, audio, &error), error.c_str());
    audio.samples.front() = .75f;
    require(session.tts.synthesize(first, audio, &error), error.c_str());
    require(fixture.count(2) == 1 && std::fabs(audio.samples.front() - .001f) < .00001f,
            "cached PCM did not own its data or called the SDK again");
    require(session.tts.statistics().last_request_cache_hits == 1 &&
            session.tts.statistics().cache_hits == 1, "a cache hit was not reported");
    for (const auto &text : {"缓存内容二。", "缓存内容三。", "缓存内容四。"})
        require(session.tts.synthesize(text, audio, &error), error.c_str());
    require(session.tts.synthesize(first, audio, &error), error.c_str());
    require(session.tts.synthesize("缓存内容五。", audio, &error), error.c_str());
    require(session.tts.synthesize("缓存内容二。", audio, &error), error.c_str());
    require(fixture.count(2) == 6, "LRU eviction did not remove the least recently used entry");
    require(session.tts.synthesize(first, audio, &error), error.c_str());
    require(fixture.count(2) == 6 && std::fabs(audio.samples.front() - .001f) < .00001f,
            "LRU eviction removed recently reused PCM");
    auto measured = session.tts.statistics();
    require(measured.cache_entries == 4 && measured.cache_bytes == 4 * 240 * sizeof(float),
            "PCM cache exceeded its entry or memory bounds");
    session.tts.stop();
    measured = session.tts.statistics();
    require(measured.cache_entries == 0 && measured.cache_bytes == 0 && measured.cache_hits == 0,
            "stop retained private cached PCM");
    hmi::engine::TtsConfig config;
    config.model_path = "fixture.gguf";
    config.backend = "cpu";
    require(session.tts.open(session.client, config, &error), error.c_str());
    require(session.tts.synthesize(first, audio, &error), error.c_str());
    require(fixture.count(2) == 7 && fixture.count(0) == 2,
            "reopening TTS reused PCM from its previous configuration");
}

void pcm_cache_can_be_disabled_and_does_not_cache_failures(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    {
        ScopedEnvironment disabled(L"HMI_TTS_PCM_CACHE", L"0");
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        for (int index = 0; index < 2; ++index)
            require(session.tts.synthesize("检查关闭缓存。", audio, &error), error.c_str());
        const auto measured = session.tts.statistics();
        require(fixture.count(2) == 2 && measured.cache_entries == 0 && measured.cache_bytes == 0 &&
                measured.cache_hits == 0 && measured.last_request_cache_hits == 0,
                "disabled PCM cache still reused or retained PCM");
    }
    fixture.reset(0, 1);
    playback.reset();
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        require(!session.tts.synthesize("失败内容不能缓存。", audio, &error), "fixture failure was not propagated");
        require(session.tts.statistics().cache_entries == 0, "failed synthesis populated the PCM cache");
        require(session.tts.synthesize("失败内容不能缓存。", audio, &error), error.c_str());
        require(session.tts.synthesize("失败内容不能缓存。", audio, &error), error.c_str());
        require(fixture.count(2) == 2 && session.tts.statistics().cache_hits == 1,
                "recovery was not synthesized once and then cached");
    }
}

void idle_release_retains_small_pcm_and_reloads_novel_text(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    Session session(fixture);
    const std::string message = "空闲后仍然可以立即重播。";
    require(session.tts.speak(message), "speech before idle was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "speech before idle did not finish");
    const auto before = session.tts.statistics();
    require(before.model_ready && before.cache_entries == 1 && before.model_load_ms > 0.0 &&
            before.idle_timeout_seconds == 1, "idle test did not load and cache speech");
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }), "idle worker did not release its model");
    auto measured = session.tts.statistics();
    require(!measured.model_ready && measured.backend.empty() && measured.cache_entries == 1 &&
            measured.segments == before.segments && measured.first_audio_ms == before.first_audio_ms &&
            fixture.count(4) == 1 && fixture.count(5) == 1,
            "idle release retained the model or lost previous speech metrics/PCM");
    playback.reset();
    require(session.tts.speak(message), "cached speech after idle was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "cached speech after idle did not finish");
    measured = session.tts.statistics();
    require(!measured.model_ready && measured.backend.empty() && measured.cache_hits == 1 &&
            measured.last_request_cache_hits == 1 && measured.model_load_ms == 0.0 &&
            measured.generated_segments == 1 && fixture.count(0) == 1 && fixture.count(2) == 1,
            "cached replay unnecessarily reloaded a model or misreported readiness");
    require(session.tts.speak("新内容需要重新加载模型。"), "new speech after idle was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "new speech after idle did not finish");
    require(session.tts.last_error().empty() && session.tts.statistics().model_ready &&
            fixture.count(0) == 2 && fixture.count(1) == 2 && fixture.count(2) == 2,
            "new speech did not automatically reload after idle release");
    session.tts.stop();
    require(fixture.count(4) == 2 && fixture.count(5) == 2,
            "stop after idle reload leaked or double-freed its model/session");
}

void idle_release_protects_playback_and_direct_synthesis(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset(true);
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    {
        Session session(fixture);
        require(session.tts.speak("播放未结束时保留模型。"), "held playback speech was rejected");
        require(playback.wait_played(), "held playback was not reached");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        require(session.tts.busy() && session.tts.statistics().model_ready && fixture.count(4) == 0,
                "idle release interrupted unfinished playback");
        playback.release();
        require(wait_until([&] { return !session.tts.busy(); }), "held playback did not finish");
        require(session.tts.statistics().model_ready, "playback completion immediately expired the model");
        require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }),
                "model did not expire after actual playback completion");
    }
    fixture.reset(1, 0);
    playback.reset();
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        bool ok = false;
        std::thread synthesizer([&] { ok = session.tts.synthesize("同步预热不能被闲置卸载。", audio, &error); });
        const bool blocked = fixture.wait_blocked(5000) != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const bool protected_model = fixture.count(4) == 0 && session.tts.statistics().model_ready;
        fixture.release();
        synthesizer.join();
        require(blocked && protected_model && ok, "idle release interrupted direct synchronous synthesis");
        require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }),
                "direct synthesis did not refresh or schedule idle release");
    }
}

void idle_lease_never_loads_and_expires_without_a_release_call(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    Session session(fixture);
    require(!session.tts.defer_idle_unload(2) && fixture.count(0) == 0,
            "idle lease loaded a model instead of staying lazy");
    hmi::audio::AudioBuffer audio;
    std::string error;
    require(session.tts.synthesize("保留已加载模型等待录音定稿。", audio, &error), error.c_str());
    require(session.tts.defer_idle_unload(3), "loaded model did not accept a bounded lease");
    // Starting another recording cancels old playback without cancelling its lease.
    session.tts.cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    require(session.tts.statistics().model_ready && fixture.count(4) == 0,
            "idle timeout or playback cancellation ignored the recording lease");
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }),
            "expired idle lease pinned the model without a matching release call");
    require(!session.tts.statistics().model_ready && fixture.count(4) == 1 && fixture.count(5) == 1,
            "idle lease expiration did not release both model and session");
}

void idle_lease_clear_and_bounds_restore_the_normal_timeout(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    require(session.tts.synthesize("录音保活结束后恢复真正闲置释放。", audio, &error), error.c_str());
    require(session.tts.defer_idle_unload(45), "maximum bounded idle lease was rejected");
    for (const int invalid : {-1, 46, 2147483647})
        require(!session.tts.defer_idle_unload(invalid), "idle lease accepted an unbounded duration");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    require(session.tts.statistics().model_ready && fixture.count(4) == 0,
            "invalid lease duration cancelled the existing valid lease");
    require(session.tts.defer_idle_unload(0), "explicit idle lease release was rejected");
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }),
            "clearing idle lease did not restore the elapsed original timeout");
    require(!session.tts.defer_idle_unload(45), "unloaded model accepted an idle lease");
    require(session.tts.synthesize("超界请求不能让重新加载的模型常驻。", audio, &error), error.c_str());
    require(!session.tts.defer_idle_unload(46), "out-of-range idle lease was accepted after reload");
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 2; }),
            "rejected duration nevertheless extended idle residency");
}

void idle_lease_does_not_wait_for_a_blocked_sdk_call_and_resets_on_reopen(Fixture &fixture)
{
    fixture.reset(1, 0);
    playback.reset();
    ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"1");
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    bool synthesized = false;
    std::thread synthesizer([&] {
        synthesized = session.tts.synthesize("保活操作不能等待正在执行的模型。", audio, &error);
    });
    const bool blocked = fixture.wait_blocked(5000) != 0;
    const auto started = Clock::now();
    const bool leased = session.tts.defer_idle_unload(45);
    const auto lease_time = Clock::now() - started;
    fixture.release();
    synthesizer.join();
    require(blocked && leased && synthesized && lease_time < std::chrono::milliseconds(250),
            "idle lease waited for in-flight SDK synthesis or failed to preserve its model");
    session.tts.stop();
    require(!session.tts.defer_idle_unload(2), "closed service accepted a positive idle lease");
    require(session.tts.defer_idle_unload(0), "closed service did not permit idempotent lease cleanup");
    hmi::engine::TtsConfig config;
    config.model_path = "fixture.gguf";
    config.backend = "cpu";
    require(session.tts.open(session.client, config, &error), error.c_str());
    require(session.tts.synthesize("重新打开不能继承上次保活期限。", audio, &error), error.c_str());
    require(wait_until([&] { return session.tts.statistics().idle_unloads == 1; }),
            "reopen inherited a stale idle lease from the previous session");
}

void idle_and_cache_environment_are_validated(Fixture &fixture)
{
    fixture.reset(0, 0);
    hmi::engine::Client client;
    hmi::engine::TtsSession session;
    hmi::engine::TtsConfig config;
    config.model_path = "fixture.gguf";
    config.backend = "cpu";
    std::string error;
    require(client.open(&error), error.c_str());
    for (const auto &value : {L"-1", L"3601", L"1x", L"99999999999999999"})
    {
        ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", value);
        require(!session.open(client, config, &error) && error.find("HMI_TTS_IDLE_SECONDS") != std::string::npos,
                "invalid idle release policy was accepted");
    }
    {
        ScopedEnvironment invalid(L"HMI_TTS_PCM_CACHE", L"yes");
        require(!session.open(client, config, &error) && error.find("HMI_TTS_PCM_CACHE") != std::string::npos,
                "invalid PCM cache policy was accepted");
    }
    for (const auto &value : {L"-1", L"2", L"true", L"99999999999999999"})
    {
        ScopedEnvironment invalid(L"HMI_TTS_DO_SAMPLE", value);
        require(!session.open(client, config, &error) && error.find("HMI_TTS_DO_SAMPLE") != std::string::npos,
                "invalid TTS sampling policy was accepted");
    }
    require(session.open(client, config, &error), error.c_str());
    require(session.statistics().idle_timeout_seconds == 0, "disabled idle policy was not retained");
    session.stop();
    client.close();
}

void generation_options_use_stable_default_sampling_and_bound_utf8_content(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(6);
    playback.reset();
    ScopedEnvironment default_sampling(L"HMI_TTS_DO_SAMPLE", L"");
    Session session(fixture);
    const struct Request { std::string text; const char *budget; } requests[] = {
        {"中", "32"},
        {"接下来我们使用很", "72"},
        {"　你好，🙂！", "42"},
        {"Audio12345", "84"},
        {std::string(400, 'a'), "2048"}
    };
    hmi::audio::AudioBuffer audio;
    std::string error;
    unsigned index = 0;
    for (const auto &request : requests)
    {
        ++index;
        require(session.tts.synthesize(request.text, audio, &error), error.c_str());
        require_voice_policy(fixture, index, true);
        require(fixture.request_option(index, "max_tokens") == request.budget,
                "generation budget did not count spoken UTF-8 codepoints or apply its limits");
        require(fixture.generated_text(index) == request.text,
                "generation budget modified, split or trimmed the submitted text");
    }
    const auto measured = session.tts.statistics();
    require(measured.sampling_enabled && measured.max_tokens == 2048 && fixture.count(2) == 5,
            "generation settings were not published for the actual SDK request");
}

void latin_segments_use_auto_without_changing_text_cache_or_explicit_language(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        const struct Request { const char *text; const char *language; } requests[] = {
            {"telemetry）。", "auto"}, {"Read-back, don't 123!", "auto"}, {"“telemetry”　", "auto"},
            {"12,345.67。", "zh"}, {"中文 telemetry。", "zh"}, {"中文。", "zh"},
            {"télémetry", "zh"}, {"telemetryカナ", "zh"}, {"telemetry１２", "zh"}, {"telemetry🙂", "zh"}};
        unsigned index = 0;
        for (const auto &request : requests)
        {
            ++index;
            require(session.tts.synthesize(request.text, audio, &error), error.c_str());
            require(fixture.generated_text(index) == request.text &&
                        fixture.request_option(index, "fixture.text_language") == request.language,
                    "segment language selection rewrote text or overrode a non-Latin segment");
            if (index == 1)
            {
                const auto original = audio.samples;
                require(session.tts.synthesize(request.text, audio, &error), error.c_str());
                require(fixture.count(2) == 1 && audio.samples == original &&
                            session.tts.statistics().last_request_cache_hits == 1,
                        "Latin language selection changed PCM cache reuse");
            }
        }
    }
    fixture.reset(0, 0);
    {
        Session session(fixture, "cpu", nullptr, "en");
        hmi::audio::AudioBuffer audio;
        std::string error;
        require(session.tts.synthesize("telemetry）。", audio, &error), error.c_str());
        require(session.tts.synthesize("中文 telemetry。", audio, &error), error.c_str());
        require(fixture.request_option(1, "fixture.text_language") == "en" &&
                    fixture.request_option(2, "fixture.text_language") == "en",
                "segment language detection overrode the explicitly configured language");
    }
}

void mixed_language_worker_records_one_utterance_language(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    ConfigurationDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions logging;
    logging.file = directory.path / "language.log";
    logging.level = hmi::base::LogLevel::Debug;
    std::string error;
    require(logger.configure(logging, &error), error.c_str());
    {
        Session session(fixture, "cpu", &logger);
        const std::string text = "分析当前的位置（telemetry）。";
        require(session.tts.speak(text, 9601), "mixed language worker request was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "mixed language worker stayed busy");
        require(fixture.count(2) == 2 && fixture.generated_text(1) == "分析当前的位置（" &&
                    fixture.generated_text(2) == "telemetry）。" &&
                    fixture.request_option(1, "fixture.text_language") == "zh" &&
                    fixture.request_option(2, "fixture.text_language") == "zh",
                "mixed speech changed segmentation or changed language partway through the utterance");
        require(session.tts.speak(text, 9602), "mixed language cached request was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "mixed language cached request stayed busy");
        require(fixture.count(2) == 2 && session.tts.statistics().utterance_cache_hits == 2,
                "mixed language detection regenerated or modified cached segments");
    }
    logger.flush();
    bool started_chinese = false, started_latin = false, finished_chinese = false, finished_latin = false;
    std::ifstream input(logging.file, std::ios::binary);
    for (std::string line; std::getline(input, line);)
    {
        if (line.find("request_id=9601 ") == std::string::npos) continue;
        const bool chinese = line.find("segment_index=1 ") != std::string::npos &&
            line.find(" language=zh") != std::string::npos;
        const bool latin = line.find("segment_index=2 ") != std::string::npos &&
            line.find(" language=zh") != std::string::npos;
        if (line.find("event=attempt_started ") != std::string::npos)
        {
            started_chinese = started_chinese || chinese;
            started_latin = started_latin || latin;
        }
        if (line.find("event=segment_finished ") != std::string::npos)
        {
            finished_chinese = finished_chinese || chinese;
            finished_latin = finished_latin || latin;
        }
    }
    require(started_chinese && started_latin && finished_chinese && finished_latin,
            "SDK request and segment traces did not retain the whole utterance's language");
}

void sampling_policy_is_frozen_until_reopen_and_clears_cache(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    const std::string text = "修改采样策略需要重新打开。";
    ScopedEnvironment sampling(L"HMI_TTS_DO_SAMPLE", L"1");
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    require(fixture.request_option(1, "do_sample") == "false" && !session.tts.statistics().sampling_enabled,
            "an environment change mutated an open session's sampling policy");
    require_voice_policy(fixture, 1, false, "zh");
    hmi::engine::TtsConfig config;
    config.model_path = "fixture.gguf";
    config.backend = "cpu";
    require(session.tts.open(session.client, config, &error), error.c_str());
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    require(fixture.count(2) == 2 && fixture.request_option(2, "do_sample") == "true" &&
            fixture.request_option(2, "subtalker_do_sample") == "true" && session.tts.statistics().sampling_enabled,
            "reopening did not freeze the explicit sampling opt-in or clear incompatible cached PCM");
    require_voice_policy(fixture, 2, true, "zh");
    {
        ScopedEnvironment changed_again(L"HMI_TTS_DO_SAMPLE", L"0");
        require(session.tts.synthesize("再次改变环境也保持既定策略。", audio, &error), error.c_str());
        require(fixture.request_option(3, "do_sample") == "true" &&
                fixture.request_option(3, "subtalker_do_sample") == "true",
                "an open sampled session stopped sampling after an environment change");
        require_voice_policy(fixture, 3, true, "zh");
    }
}

void utterance_language_is_part_of_the_pcm_cache_context(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        const std::string latin = "telemetry）。";
        const std::string mixed = "分析当前的位置（" + latin;
        require(session.tts.synthesize(latin, audio, &error), error.c_str());
        const auto pure_latin_pcm = audio.samples;
        require(fixture.count(2) == 1 && fixture.request_option(1, "fixture.text_language") == "auto",
                "pure Latin cache precondition used the wrong language");
        require(session.tts.speak(mixed, 9621), "mixed language cache request was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "mixed language cache request stayed busy");
        require(fixture.count(2) == 3 && fixture.generated_text(3) == latin &&
                    fixture.request_option(2, "fixture.text_language") == "zh" &&
                    fixture.request_option(3, "fixture.text_language") == "zh" &&
                    session.tts.statistics().utterance_cache_hits == 0 &&
                    playback.ids() == std::vector<unsigned>{2, 3},
                "mixed Chinese speech reused the same text synthesized in the pure Latin context");
        require(session.tts.speak(mixed, 9622), "matching mixed cache replay was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "matching mixed cache replay stayed busy");
        require(fixture.count(2) == 3 && session.tts.statistics().utterance_cache_hits == 2,
                "matching text and utterance language did not reuse both owned PCM chunks");
        require(session.tts.synthesize(latin, audio, &error), error.c_str());
        require(fixture.count(2) == 3 && audio.samples == pure_latin_pcm &&
                    session.tts.statistics().last_request_cache_hits == 1,
                "worker language leaked into synchronous speech or displaced its distinct Latin cache entry");
        require(session.tts.speak(latin, 9623), "pure Latin worker cache replay was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "pure Latin worker cache replay stayed busy");
        require(fixture.count(2) == 3 && session.tts.statistics().utterance_cache_hits == 1 &&
                    playback.ids().back() == 1,
                "a later pure Latin worker reused mixed-context PCM");
    }
    fixture.reset(0, 0);
    playback.reset();
    {
        Session session(fixture, "cpu", nullptr, "en");
        require(session.tts.speak("分析当前的位置（telemetry）。", 9624), "explicit language worker was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "explicit language worker stayed busy");
        require(fixture.count(2) == 2 && fixture.request_option(1, "fixture.text_language") == "en" &&
                    fixture.request_option(2, "fixture.text_language") == "en",
                "whole-utterance detection overrode an explicitly configured worker language");
    }
}

void split_recovery_preserves_the_whole_voice_context(Fixture &fixture)
{
    for (const bool sampled : {false, true})
    {
        ScopedEnvironment sampling(L"HMI_TTS_DO_SAMPLE", sampled ? L"1" : L"0");
        fixture.reset(0, 0);
        fixture.generation(7);
        playback.reset();
        const std::string instruction = "请保持自然语速和同一音色。";
        Session session(fixture, "cpu", nullptr, "zh", instruction, "ryan");
        require(session.tts.speak("中文TTS", sampled ? 9632 : 9631), "mixed recovery request was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "mixed recovery request stayed busy");
        const auto measured = session.tts.statistics();
        require(fixture.count(2) == 4 && fixture.generated_text(1) == "中文TTS" &&
                    fixture.generated_text(2) == "中文TTS" && fixture.generated_text(3) == "中文" &&
                    fixture.generated_text(4) == "TTS" && playback.ids() == std::vector<unsigned>{3, 4},
                "length recovery skipped, repeated or published a rejected mixed-language segment");
        for (unsigned index = 1; index <= 4; ++index)
            require_voice_policy(fixture, index, sampled, "zh", "ryan", instruction);
        require(std::stoi(fixture.request_option(2, "max_tokens")) ==
                    std::min(std::stoi(fixture.request_option(1, "max_tokens")) * 2, 2048) &&
                    measured.sampling_enabled == sampled && measured.sampling_fallbacks == 0 &&
                    measured.generation_retries == 1 && measured.limit_hits == 2 && measured.recovery_splits == 1 &&
                    measured.recovery_attempts == 2 && measured.generated_segments == 2 &&
                    measured.pipeline_finish_reason == "completed" && session.tts.last_error().empty(),
                "split recovery changed the voice policy or lost bounded retry/completion statistics");
    }
}

void voice_instruction_changes_require_reopen_and_clear_old_pcm(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    Session session(fixture, "cpu", nullptr, "zh", "自然清晰。", "serena");
    hmi::audio::AudioBuffer audio;
    std::string error;
    const std::string text = "同样的文字。";
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    require_voice_policy(fixture, 1, false, "zh", "serena", "自然清晰。");
    hmi::engine::TtsConfig config;
    config.model_path = "fixture.gguf";
    config.backend = "cpu";
    config.voice_id = "ryan";
    config.instruction = "平稳清晰。";
    require(session.tts.open(session.client, config, &error), error.c_str());
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    require(fixture.count(2) == 2 && session.tts.statistics().last_request_cache_hits == 0 &&
                std::fabs(audio.samples.front() - .002f) < .00001f,
            "reopening with a different voice instruction reused incompatible PCM");
    require_voice_policy(fixture, 2, false, "zh", "ryan", "平稳清晰。");
    for (const std::string invalid : {std::string(" \t"), std::string("\xff"),
            std::string("x\0y", 3), std::string(4097, 'x')})
    {
        config.instruction = invalid;
        hmi::engine::TtsSession rejected;
        require(!rejected.open(session.client, config, &error) && !error.empty(),
                "malformed or excessive voice instruction was accepted");
    }
    require(fixture.count(2) == 2, "instruction validation synthesized unexpected audio");
}
}

namespace
{
void generation_length_retries_before_caching_complete_pcm(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(1);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    const std::string text = "完整重试测试。";
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    require(fixture.count(2) == 2 && fixture.generated_text(1) == text && fixture.generated_text(2) == text,
            "token-limited speech did not retry the original complete text exactly once");
    require(std::stoi(fixture.request_option(2, "max_tokens")) ==
                std::min(std::stoi(fixture.request_option(1, "max_tokens")) * 2, 2048),
            "retry did not double the bounded generation budget");
    require(audio.samples.size() == 1920 && std::fabs(audio.samples.front() - .002f) < .00001f,
            "synthesis returned the truncated first attempt or concatenated duplicate speech");
    auto measured = session.tts.statistics();
    require(measured.generation_retries == 1 && measured.limit_hits == 1 && measured.last_finish_reason == "eos" &&
                measured.generated_audio_frames == 1920 && std::fabs(measured.audio_seconds - .08) < .000001,
            "synchronous synthesis did not report accepted PCM and retry statistics");
    require(measured.cache_entries == 1 && playback.ids().empty(), "direct retry cached incomplete PCM or started playback");
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    measured = session.tts.statistics();
    require(fixture.count(2) == 2 && measured.last_request_cache_hits == 1 &&
                measured.generation_retries == 0 && measured.limit_hits == 0 &&
                std::fabs(audio.samples.front() - .002f) < .00001f,
            "only accepted complete PCM should be cached and direct request counters should reset");
}

void generation_worker_accumulates_retry_statistics_without_partial_playback(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(7);
    playback.reset();
    Session session(fixture);
    const std::string text = "完整恢复测试。" + long_text();
    require(session.tts.speak(text), "worker generation retry was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "worker retry stayed busy");
    const auto ids = playback.ids();
    const auto measured = session.tts.statistics();
    require(!ids.empty() && ids.front() == 3 && std::find(ids.begin(), ids.end(), 1u) == ids.end() &&
                std::find(ids.begin(), ids.end(), 2u) == ids.end(),
            "worker published the truncated first attempt");
    require(measured.generated_segments == ids.size() && fixture.count(2) == ids.size() + 2 &&
                measured.generation_retries == 1 && measured.sampling_fallbacks == 0 && measured.limit_hits == 2 &&
                measured.last_finish_reason == "eos" &&
                measured.recovery_splits == 1 && measured.recovery_attempts == 2 && !measured.recovery_exhausted,
            "worker recovery erased counters, retried the same segment or counted rejected PCM");
    std::string accepted_text;
    for (unsigned index = 3; index <= fixture.count(2); ++index) accepted_text += fixture.generated_text(index);
    require(accepted_text == text, "split recovery skipped, reordered or repeated spoken text");
    require(measured.generated_audio_frames == ids.size() * 1920 && session.tts.last_error().empty(),
            "worker accepted PCM progress or completion was lost after retry");
}

void generation_repeated_limit_fails_without_publishing_or_caching_pcm(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(2);
    playback.reset();
    Session session(fixture);
    const std::string text = "预算耗尽测试。";
    require(session.tts.speak(text), "limited worker request was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "limited worker request stayed busy");
    const auto measured = session.tts.statistics();
    const auto failed_calls = fixture.count(2);
    require(failed_calls >= 2 && failed_calls <= 34 && measured.generation_retries <= measured.recovery_attempts + 1 &&
                measured.limit_hits == failed_calls && measured.recovery_splits > 0 && measured.recovery_exhausted &&
                measured.last_finish_reason == "length", "persistent length recovery was unbounded or lost its failure");
    require(playback.ids().empty() && measured.cache_entries == 0 && measured.generated_segments == 0 &&
                measured.generated_audio_frames == 0 && !session.tts.last_error().empty(),
            "persistent length limit published or cached incomplete PCM or reported success");
    fixture.generation(0);
    hmi::audio::AudioBuffer audio;
    std::string error;
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    require(fixture.count(2) == failed_calls + 1 && audio.samples.size() == 240 &&
                std::fabs(audio.samples.front() - static_cast<float>(failed_calls + 1) / 1000.0f) < .00001f,
            "a failed limited attempt entered the cache or prevented later synthesis");
}

void generation_cancelled_limit_does_not_retry(Fixture &fixture)
{
    fixture.reset(1, 0);
    fixture.generation(1);
    playback.reset();
    Session session(fixture);
    require(session.tts.speak("取消预算重试。"), "cancelled limit request was rejected");
    require(fixture.wait_blocked(5000), "limited SDK call did not reach the cancellation barrier");
    session.tts.cancel();
    fixture.release();
    require(wait_until([&] { return !session.tts.busy(); }), "cancelled limited request stayed busy");
    const auto measured = session.tts.statistics();
    require(fixture.count(2) == 1 && measured.generation_retries == 0 && measured.cache_entries == 0 &&
                measured.generated_audio_frames == 0 && playback.ids().empty(),
            "cancelled synthesis retried, cached or published incomplete PCM");
}

void generation_metadata_rejects_invalid_reason_and_distinguishes_boundary_eos(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(3);
    playback.reset();
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        require(!session.tts.synthesize("无效元数据。", audio, &error), "invalid finish reason was accepted");
        require(fixture.count(2) == 1 && audio.samples.empty() && session.tts.statistics().cache_entries == 0 &&
                    session.tts.statistics().last_finish_reason == "invalid_metadata" &&
                    error.find("metadata") != std::string::npos, "malformed generation data retried or cached PCM");
    }
    fixture.reset(0, 0);
    fixture.generation(5);
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        require(session.tts.synthesize("边界。", audio, &error), error.c_str());
        const auto budget = std::stoi(fixture.request_option(1, "max_tokens"));
        const auto measured = session.tts.statistics();
        require(fixture.count(2) == 1 && audio.samples.size() == static_cast<std::size_t>(budget - 1) * 1920 &&
                    measured.last_finish_reason == "eos" && measured.limit_hits == 0 && measured.generation_retries == 0,
                "explicit EOS at the last iteration was mistaken for budget exhaustion");
    }
}

void generation_legacy_boundary_retries_without_requiring_metadata(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(4);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    require(session.tts.synthesize("旧版预算。", audio, &error), error.c_str());
    const auto measured = session.tts.statistics();
    require(fixture.count(2) == 2 && audio.samples.size() == 240 &&
                std::fabs(audio.samples.front() - .002f) < .00001f && measured.generation_retries == 1 &&
                measured.limit_hits == 1 && measured.last_finish_reason == "unreported",
            "legacy PCM boundary was not retried or metadata absence was treated as failure");
}

void generation_waiting_worker_keeps_its_original_cancellation_token(Fixture &fixture)
{
    fixture.reset(1, 0);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer audio;
    std::string error;
    bool direct_ok = false;
    std::thread direct([&] { direct_ok = session.tts.synthesize("占用合成线程。", audio, &error); });
    const bool blocked = fixture.wait_blocked(5000) != 0;
    const bool old_queued = blocked && session.tts.speak("等待的旧朗读。", 7001);
    const bool old_started = old_queued && wait_until([&] { return session.tts.statistics().segments != 0; });
    const std::string replacement = "新的朗读。";
    const bool new_queued = old_started && session.tts.speak(replacement, 7002);
    fixture.release();
    direct.join();
    const bool finished = wait_until([&] { return !session.tts.busy(); });
    require(blocked && old_started && new_queued && finished && !direct_ok,
            "queued worker cancellation barriers did not complete");
    require(fixture.count(2) == 2 && fixture.generated_text(2) == replacement &&
                playback.ids() == std::vector<unsigned>{2},
            "a superseded worker captured the replacement generation and synthesized stale text");
    const auto measured = session.tts.statistics();
    require(measured.request_id == 7002, "replacement request id was overwritten by the old generation");
    require(measured.generated_segments == 1 && measured.generated_audio_frames == 240 &&
                measured.last_finish_reason == "unreported" && session.tts.last_error().empty(),
            "a superseded worker contaminated replacement statistics");
}

void request_identity_and_cached_playback_have_detached_statistics(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    Session session(fixture);
    const std::string text = "缓存确认。";
    require(session.tts.speak(text, 8801), "first traced request was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "first traced request stayed busy");
    const auto first = session.tts.statistics();
    require(first.request_id == 8801 && first.playback.drained && first.consumed_chunks == 1 &&
                first.playback.submitted_frames == first.playback.consumed_frames &&
                !first.playback.clock_available, "fixture playback or first request statistics were not recorded");
    const auto calls = fixture.count(2);
    playback.reset();
    require(session.tts.speak(text, 8802), "cached traced request was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "cached traced request stayed busy");
    const auto second = session.tts.statistics();
    require(second.request_id == 8802 && second.last_request_cache_hits == 1 &&
                second.last_finish_reason == "cache" && second.playback.drained && second.consumed_chunks == 1 &&
                fixture.count(2) == calls, "cached playback lost request identity or regenerated audio");
    require(first.request_id == 8801 && first.last_finish_reason != "cache",
            "a later cached request mutated an earlier detached snapshot");
}

void replacement_during_model_load_keeps_request_scoped_statistics(Fixture &fixture)
{
    fixture.reset(~0U, 0);
    playback.reset();
    Session session(fixture);
    require(session.tts.speak("旧加载请求。", 9101), "loading request was rejected");
    require(fixture.wait_blocked(5000) != 0, "model loading did not reach its barrier");
    require(session.tts.speak("新请求。", 9102), "loading replacement was rejected");
    require(session.tts.statistics().request_id == 9102, "replacement identity was not published");
    fixture.release();
    require(wait_until([&] { return !session.tts.busy(); }), "loading replacement stayed busy");
    const auto measured = session.tts.statistics();
    require(measured.request_id == 9102 && measured.model_ready && measured.model_load_ms == 0.0 &&
                measured.generated_segments == 1 && measured.playback.drained && session.tts.last_error().empty(),
            "superseded model loading contaminated the replacement statistics");
    require(fixture.count(0) == 1 && fixture.count(2) == 1 && fixture.generated_text(1) == "新请求。",
            "replacement reloaded the model or synthesized cancelled text");
}

void mixed_cache_utterance_counts_hits_before_a_final_miss(Fixture &fixture)
{
    fixture.reset(0, 0);
    playback.reset();
    Session session(fixture);
    hmi::audio::AudioBuffer seed;
    std::string error;
    const std::string first = "缓存确认。";
    require(session.tts.synthesize(first, seed, &error), error.c_str());
    require(session.tts.speak(first + "末段需要重新生成。", 9201), "mixed cached utterance was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "mixed cached utterance stayed busy");
    const auto measured = session.tts.statistics();
    require(measured.generated_segments == 2 && measured.utterance_cache_hits == 1 &&
                measured.last_request_cache_hits == 0 && measured.cache_hits == 1 && fixture.count(2) == 2,
            "utterance hit count was overwritten by the final uncached segment");
    require(playback.ids() == std::vector<unsigned>{1, 2}, "mixed cache changed playback order");
}

void synchronous_request_clears_completed_worker_statistics(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(7);
    playback.reset();
    Session session(fixture);
    require(session.tts.speak("完整恢复测试。" + long_text(), 9251), "worker reset precondition was rejected");
    require(wait_until([&] { return !session.tts.busy(); }), "worker reset precondition stayed busy");
    const auto previous = session.tts.statistics();
    require(previous.request_id == 9251 && previous.generated_segments > 1 && previous.first_audio_ms > 0.0 &&
                previous.synthesis_ms > 0.0 && previous.total_ms > 0.0 && previous.queue_high_water > 0 &&
                previous.recovery_splits == 1 && previous.limit_hits == 2 && previous.sampling_fallbacks == 0 &&
                previous.playback.finalized &&
                previous.pipeline_finish_reason == "completed",
            "completed worker did not populate the statistics under test");
    const auto worker_playback = playback.ids();
    const auto worker_runs = fixture.count(2);
    fixture.generation(0);
    hmi::audio::AudioBuffer audio;
    std::string error;
    const std::string text = "全新同步请求。";
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    const auto fresh = session.tts.statistics();
    require(fixture.count(2) == worker_runs + 1 && fixture.generated_text(worker_runs + 1) == text &&
                fresh.request_id == 0 && fresh.generated_audio_frames == 240 &&
                std::fabs(fresh.audio_seconds - .01) < .000001 && fresh.last_finish_reason == "unreported",
            "fresh synchronous request lost its own identity or PCM statistics");
    require(fresh.segments == 0 && fresh.generated_segments == 0 && fresh.first_audio_ms == 0.0 &&
                fresh.synthesis_ms == 0.0 && fresh.total_ms == 0.0 && fresh.queue_ms == 0.0 && fresh.queue_high_water == 0 &&
                fresh.producer_wait_ms == 0.0 && fresh.consumer_wait_ms == 0.0 && fresh.consumer_pending_reads == 0 &&
                fresh.consumed_chunks == 0 && fresh.pipeline_finish_reason.empty() && !fresh.playback.finalized &&
                fresh.playback.submitted_frames == 0 && fresh.playback.endpoint_id.empty(),
            "synchronous request retained the completed worker's pipeline or playback statistics");
    require(fresh.recovery_splits == 0 && fresh.recovery_attempts == 0 && !fresh.recovery_exhausted &&
                fresh.limit_hits == 0 && fresh.generation_retries == 0 && fresh.utterance_cache_hits == 0 &&
                fresh.last_request_cache_hits == 0 && fresh.sampling_fallbacks == 0 &&
                fresh.model_load_ms == 0.0 && fresh.max_tokens > 0 &&
                fresh.model_ready && fresh.backend == "cpu" && fresh.cache_entries != 0 && playback.ids() == worker_playback,
            "synchronous reset retained request counters, reset model lifetime state or started playback");
    require(session.tts.synthesize(text, audio, &error), error.c_str());
    const auto cached = session.tts.statistics();
    require(fixture.count(2) == worker_runs + 1 && cached.request_id == 0 && cached.max_tokens == 0 &&
                cached.utterance_cache_hits == 1 && cached.last_request_cache_hits == 1 &&
                cached.last_finish_reason == "cache" && cached.generated_segments == 0 && cached.total_ms == 0.0 && cached.queue_ms == 0.0 &&
                cached.generated_audio_frames == 240 && !cached.playback.finalized,
            "cached synchronous request retained an earlier SDK budget or worker progress");
}

void synchronous_request_rejects_active_worker_without_side_effects(Fixture &fixture)
{
    for (const bool sdk_blocked : {false, true})
    {
        fixture.reset(sdk_blocked ? 1U : 0U, 0);
        playback.reset(!sdk_blocked);
        Session session(fixture);
        require(session.tts.speak("正在朗读。", 9261), "busy worker precondition was rejected");
        if (sdk_blocked)
            require(fixture.wait_blocked(5000) != 0, "busy worker missed SDK barrier");
        else
        {
            require(playback.wait_played(), "busy worker missed playback barrier");
            require(wait_until([&] { return session.tts.statistics().generated_segments == 1; }),
                    "busy worker never published generated PCM");
        }
        const auto before = session.tts.statistics();
        const auto runs = fixture.count(2);
        hmi::audio::AudioBuffer audio;
        audio.samples = {.5f};
        std::string error;
        const auto started = Clock::now();
        const bool accepted = session.tts.synthesize("同步请求不能混入。", audio, &error);
        const auto elapsed = Clock::now() - started;
        const auto after = session.tts.statistics();
        require(!accepted && !error.empty() && audio.samples.empty() && elapsed < std::chrono::milliseconds(250),
                "synchronous synthesis waited for or entered the active worker");
        require(fixture.count(2) == runs && after.request_id == before.request_id &&
                    after.generated_segments == before.generated_segments && after.max_tokens == before.max_tokens &&
                    after.last_finish_reason == before.last_finish_reason && after.sampling_fallbacks == before.sampling_fallbacks &&
                    after.utterance_cache_hits == before.utterance_cache_hits && after.cache_hits == before.cache_hits &&
                    after.first_audio_ms == before.first_audio_ms && after.playback.finalized == before.playback.finalized &&
                    session.tts.last_error().empty(),
                "rejected synchronous request changed worker identity, statistics or SDK work");
        fixture.release();
        playback.release();
        require(wait_until([&] { return !session.tts.busy(); }), "worker failed to finish after synchronous rejection");
        const auto completed = session.tts.statistics();
        require(completed.request_id == 9261 && completed.playback.finalized && completed.playback.drained &&
                    completed.generated_segments == 1 && fixture.count(2) == 1,
                "synchronous rejection damaged the original worker completion");
    }
}

void recovery_cancel_and_real_errors_do_not_resume_children(Fixture &fixture)
{
    fixture.reset(2, 0);
    fixture.generation(1);
    playback.reset();
    {
        Session session(fixture);
        require(session.tts.speak("恢复中的取消测试。", 9301), "recovery cancellation was rejected");
        require(fixture.wait_blocked(5000) != 0, "recovery child did not reach SDK barrier");
        session.tts.cancel();
        fixture.release();
        require(wait_until([&] { return !session.tts.busy(); }), "cancelled recovery stayed busy");
        require(fixture.count(2) == 2 && playback.ids().empty() && session.tts.statistics().cache_entries == 0,
                "cancelled recovery resumed children or published incomplete audio");
    }
    fixture.reset(0, 2);
    fixture.generation(1);
    playback.reset();
    {
        Session session(fixture);
        require(session.tts.speak("恢复中的真实错误。", 9302), "recovery error request was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "failed recovery stayed busy");
        require(fixture.count(2) == 2 && playback.ids().empty() &&
                    session.tts.last_error().find("fixture synthesis failure") != std::string::npos &&
                    !session.tts.statistics().recovery_exhausted &&
                    session.tts.statistics().pipeline_finish_reason == "producer_failed" &&
                    session.tts.statistics().last_finish_reason.empty(),
                "a real SDK error was treated as another length recovery");
    }
    fixture.reset(0, 0);
    fixture.generation(2);
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        require(!session.tts.synthesize("同步失败仍保留原语义。", audio, &error), "persistent direct limit became success");
        const auto measured = session.tts.statistics();
        require(fixture.count(2) == 2 && fixture.generated_text(1) == fixture.generated_text(2) &&
                    measured.generation_retries == 1 && measured.recovery_splits == 0 && audio.samples.empty() &&
                    error.find("token limit") != std::string::npos,
                "direct synthesis changed its bounded original-text retry contract");
    }
}

void budget_recovery_preserves_voice_policy_and_cache(Fixture &fixture)
{
    fixture.reset(0, 0);
    fixture.generation(1);
    playback.reset();
    ConfigurationDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions logging;
    logging.file = directory.path / "sampling.log";
    std::string error;
    require(logger.configure(logging, &error), error.c_str());
    {
        const std::string instruction = "请保持自然语速和同一音色。";
        Session session(fixture, "cpu", &logger, "zh", instruction, "ryan");
        const std::string text = "音。";
        require(session.tts.speak(text, 9501), "stable budget recovery was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "stable budget recovery stayed busy");
        const auto recovered = session.tts.statistics();
        require(session.tts.last_error().empty() && recovered.pipeline_finish_reason == "completed" &&
                    fixture.count(2) == 2 && fixture.generated_text(1) == text && fixture.generated_text(2) == text &&
                    std::stoi(fixture.request_option(2, "max_tokens")) ==
                        std::min(std::stoi(fixture.request_option(1, "max_tokens")) * 2, 2048) &&
                    recovered.generation_retries == 1 && recovered.sampling_fallbacks == 0 &&
                    !recovered.sampling_enabled && recovered.limit_hits == 1 && recovered.recovery_splits == 0,
                "budget recovery changed text, voice policy or bounded retry accounting");
        require_voice_policy(fixture, 1, false, "zh", "ryan", instruction);
        require_voice_policy(fixture, 2, false, "zh", "ryan", instruction);
        require(playback.ids() == std::vector<unsigned>{2}, "budget recovery published the rejected first PCM");
        require(session.tts.speak(text, 9502), "recovered PCM cache request was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "recovered PCM cache request stayed busy");
        require(fixture.count(2) == 2 && session.tts.statistics().utterance_cache_hits == 1 &&
                    session.tts.statistics().sampling_fallbacks == 0,
                "recovered PCM was not cached under its original text or leaked old fallback counters");
        fixture.generation(0);
        require(session.tts.speak("后续正常请求。", 9503), "normal request after recovery was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "normal request after recovery stayed busy");
        require(fixture.count(2) == 3 && fixture.request_option(3, "do_sample") == "false" &&
                    fixture.request_option(3, "subtalker_do_sample") == "false" &&
                    fixture.request_option(3, "seed") == "42" && session.tts.statistics().sampling_fallbacks == 0 &&
                    session.tts.statistics().generation_retries == 0,
                "budget recovery changed a later request's policy or leaked retry counters");
        require_voice_policy(fixture, 3, false, "zh", "ryan", instruction);
    }
    logger.flush();
    std::ifstream input(logging.file, std::ios::binary);
    bool recorded = false;
    for (std::string line; std::getline(input, line);)
        recorded = recorded || (line.find("request_id=9501 segment_index=1") != std::string::npos &&
            line.find("strategy=budget_retry") != std::string::npos);
    require(recorded, "stable budget recovery did not record its strategy and correlation");
}

void stable_budget_recovery_is_bounded_and_respects_cancellation(Fixture &fixture)
{
    for (const bool sampled : {false, true})
    {
        ScopedEnvironment sampling(L"HMI_TTS_DO_SAMPLE", sampled ? L"1" : L"0");
        fixture.reset(0, 0);
        fixture.generation(2);
        playback.reset();
        Session session(fixture);
        require(session.tts.speak("TTS", sampled ? 9512 : 9511), "bounded budget failure was rejected");
        require(wait_until([&] { return !session.tts.busy(); }), "bounded budget failure stayed busy");
        const auto state = session.tts.statistics();
        require(fixture.count(2) == 2 && fixture.generated_text(1) == "TTS" && fixture.generated_text(2) == "TTS" &&
                    std::stoi(fixture.request_option(2, "max_tokens")) ==
                        std::min(std::stoi(fixture.request_option(1, "max_tokens")) * 2, 2048) &&
                    state.sampling_enabled == sampled && state.generation_retries == 1 &&
                    state.sampling_fallbacks == 0 && state.limit_hits == 2 &&
                    state.recovery_exhausted && state.pipeline_finish_reason == "generation_limit" && playback.ids().empty(),
                "a second length result changed voice policy, lost failure or split an indivisible word");
        require_voice_policy(fixture, 1, sampled, "auto");
        require_voice_policy(fixture, 2, sampled, "auto");
    }
    fixture.reset(1, 0);
    fixture.generation(6);
    playback.reset();
    {
        Session session(fixture);
        require(session.tts.speak("音。", 9513), "cancelled sampler request was rejected");
        require(fixture.wait_blocked(5000) != 0, "sampler cancellation missed SDK barrier");
        session.tts.cancel();
        fixture.release();
        require(wait_until([&] { return !session.tts.busy(); }), "cancelled sampler request stayed busy");
        require(fixture.count(2) == 1 && session.tts.statistics().sampling_fallbacks == 0 &&
                    session.tts.statistics().generation_retries == 0 && playback.ids().empty(),
                "cancelled length attempt started a budget retry");
    }
    fixture.reset(0, 0);
    fixture.generation(6);
    {
        Session session(fixture);
        hmi::audio::AudioBuffer audio;
        std::string error;
        const std::string text = "同步请求保留原语义，";
        require(!session.tts.synthesize(text, audio, &error), "direct synthesis unexpectedly changed the selected sampler");
        require(fixture.count(2) == 2 && fixture.generated_text(1) == text && fixture.generated_text(2) == text &&
                    fixture.request_option(1, "do_sample") == "false" && fixture.request_option(2, "do_sample") == "false" &&
                    std::stoi(fixture.request_option(2, "max_tokens")) ==
                        std::min(std::stoi(fixture.request_option(1, "max_tokens")) * 2, 2048) &&
                    session.tts.statistics().sampling_fallbacks == 0 && audio.samples.empty(),
                "direct synthesis lost its original bounded budget retry contract");
        require_voice_policy(fixture, 1, false, "zh");
        require_voice_policy(fixture, 2, false, "zh");
    }
}

void cancelled_playback_emits_correlated_final_json(Fixture &fixture)
{
    fixture.reset(2, 0);
    playback.reset(true);
    ConfigurationDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "playback.log";
    std::string error;
    require(logger.configure(options, &error), error.c_str());
    {
        Session session(fixture, "cpu", &logger);
        require(session.tts.speak(long_text(), 9401), "logged cancellation was rejected");
        require(fixture.wait_blocked(5000) != 0 && playback.wait_played(), "logged cancellation missed its barriers");
        require(!session.tts.statistics().playback.finalized, "unfinished playback asserted final statistics");
        session.tts.cancel("capture_started");
        fixture.release();
        require(wait_until([&] { return !session.tts.busy(); }), "logged cancellation stayed busy");
    }
    fixture.reset(1, 0);
    playback.reset();
    {
        Session session(fixture, "cpu", &logger);
        require(session.tts.speak("首段尚未完成就取消。", 9402), "zero-PCM cancellation was rejected");
        require(fixture.wait_blocked(5000) != 0, "zero-PCM cancellation missed SDK barrier");
        session.tts.cancel("capture_started");
        fixture.release();
        require(wait_until([&] { return !session.tts.busy(); }), "zero-PCM cancellation stayed busy");
    }
    fixture.reset(0, 0);
    playback.reset(true);
    {
        Session session(fixture, "cpu", &logger);
        require(session.tts.speak("替换前的朗读。", 9403), "logged replacement precondition was rejected");
        require(playback.wait_played(), "logged replacement missed playback barrier");
        require(session.tts.speak("替换后的朗读。", 9404), "logged replacement was rejected");
        playback.release();
        require(wait_until([&] { return !session.tts.busy(); }), "logged replacement stayed busy");
        const auto completed = session.tts.statistics();
        require(completed.request_id == 9404 && completed.playback.finalized && completed.playback.drained,
                "replacement did not retain its own final snapshot");
        session.tts.cancel("capture_started");
        require(session.tts.statistics().playback.finalized,
                "cancellation after completion discarded the already published final snapshot");
    }
    logger.flush();
    std::ifstream input(options.file, std::ios::binary);
    const std::string prefix = "event=playback_result ";
    std::size_t results = 0;
    for (std::string line; std::getline(input, line);)
    {
        const auto position = line.find(prefix);
        if (position == std::string::npos) continue;
        hmi::util::json::Value event;
        require(hmi::util::json::parse(line.substr(position + prefix.size()), event, &error), error.c_str());
        const auto *run = event.find("run_id");
        const auto *id = event.find("request_id");
        const auto *reason = event.find("stop_reason");
        const auto *final = event.find("playback_final");
        const auto *finished = event.find("request_finished");
        const auto *tts_final = event.find("tts_final");
        const auto *audio = event.find("playback");
        require(run && id && reason && final && finished && tts_final && audio &&
                    std::get<std::string>(run->data) == "fixture-run" &&
                    (std::get<std::string>(id->data) == "9401" || std::get<std::string>(id->data) == "9402" ||
                        std::get<std::string>(id->data) == "9403") &&
                    std::get<std::string>(reason->data) ==
                        (std::get<std::string>(id->data) == "9403" ? "request_replaced" : "capture_started") &&
                    std::get<bool>(final->data),
                "final playback acknowledgement lost correlation, cause or finality");
        require(std::get<bool>(finished->data) && tts_final->find("synthesis_ms") &&
                    std::get<double>(tts_final->find("synthesis_ms")->data) >= 0.0 &&
                    std::get<std::string>(tts_final->find("finish_reason")->data) == "cancelled",
                "final acknowledgement lost the original cancelled request's completed SDK work");
        const auto *submitted = audio->find("submitted_frames");
        const auto *consumed = audio->find("consumed_frames");
        if (std::get<std::string>(id->data) != "9402")
            require(submitted && consumed && std::get<double>(submitted->data) > 0.0 &&
                        std::get<double>(consumed->data) > 0.0,
                    "final playback acknowledgement replaced known consumed audio with default zeros");
        else
            require(tts_final->find("first_pcm_ms") &&
                        std::holds_alternative<std::nullptr_t>(tts_final->find("first_pcm_ms")->data),
                    "cancelled synthesis without PCM asserted a zero-millisecond first audio");
        ++results;
    }
    require(results == 3, "final acknowledgements were lost, duplicated or emitted for normal completion");
}
}

namespace hmi::audio::detail
{
bool play_stream(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled)
{
    { std::lock_guard lock(playback.mutex); playback.entered = true; }
    const auto interrupted = [&] {
        if (!cancelled || !cancelled()) return false;
        { std::lock_guard lock(playback.mutex); playback.cancelled_seen = true; }
        playback.changed.notify_all();
        if (error) *error = "fixture playback cancelled";
        return true;
    };
    for (;;)
    {
        if (interrupted()) return false;
        AudioBuffer audio;
        const auto result = read(audio, error);
        if (result == StreamRead::Failed) return false;
        if (result == StreamRead::Finished) return true;
        if (result == StreamRead::Pending)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (audio.samples.empty())
        {
            if (error) *error = "fixture received empty PCM";
            return false;
        }
        const auto id = static_cast<unsigned>(std::lround(audio.samples.front() * 1000.0f));
        std::unique_lock lock(playback.mutex);
        playback.played.push_back(id);
        playback.changed.notify_all();
        while (playback.hold_first && playback.played.size() == 1 && !playback.released)
        {
            playback.changed.wait_for(lock, std::chrono::milliseconds(1));
            lock.unlock();
            const bool stopped = interrupted();
            lock.lock();
            if (stopped) return false;
        }
        if (playback.fail)
        {
            if (error) *error = "fixture playback failure";
            return false;
        }
    }
}

bool play_stream(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled,
                 PlaybackStatistics *statistics)
{
    PlaybackStatistics measured;
    measured.endpoint_id = "fixture:no_device";
    const auto started = Clock::now();
    const ReadAudio observed = [&](AudioBuffer &audio, std::string *detail) {
        const auto state = read(audio, detail);
        if (state == StreamRead::Ready)
        {
            measured.sample_rate = audio.sample_rate;
            measured.channels = audio.channels;
            measured.submitted_frames += audio.samples.size() / static_cast<std::size_t>(audio.channels);
            measured.consumed_frames = measured.submitted_frames;
            measured.padding_available = true;
            ++measured.accepted_chunks;
            ++measured.completed_chunks;
        }
        else if (state == StreamRead::Pending) ++measured.pending_reads;
        return state;
    };
    const bool ok = play_stream(observed, error, cancelled);
    measured.drained = ok;
    measured.finalized = true;
    measured.finish_reason = ok ? "drained" : "fixture_stopped";
    measured.wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    if (statistics) *statistics = measured;
    return ok;
}
}

int wmain(int argc, wchar_t **argv)
{
    try
    {
        require(hmi_test::initialize_environment(), "test environment initialization failed");
        const auto library = argc > 1 ? std::filesystem::path(argv[1]) :
            hmi::base::process_directory() / "tts_fixture.dll";
        Fixture fixture(library);
        ConfigurationDirectory configuration;
        ScopedEnvironment sdk(L"HMI_NATIVE_DLL", library);
        ScopedEnvironment config(L"HMI_NATIVE_CONFIG", configuration.path / "engine.json");
        ScopedEnvironment performance(L"HMI_TTS_PERF_MODE", L"off");
        ScopedEnvironment idle(L"HMI_TTS_IDLE_SECONDS", L"0");
        ScopedEnvironment cache(L"HMI_TTS_PCM_CACHE", L"1");
        ScopedEnvironment sampling(L"HMI_TTS_DO_SAMPLE", L"0");
        const struct Test { const char *name; void (*run)(Fixture &); } tests[] = {
            {"preparation is nonblocking, coalesced, silent and idle released", preparation_is_nonblocking_coalesced_and_idle_released},
            {"preparation preserves completed speech statistics and cached PCM", preparation_preserves_completed_speech_and_cached_pcm},
            {"actual speech during preparation takes priority and reuses one load", speech_during_blocked_preparation_reuses_one_load},
            {"preparation cancel is nonblocking and preserves a newer preparation", preparation_cancel_is_nonblocking_and_new_preparation_survives},
            {"cancel clears preparation queued behind cached playback", queued_preparation_is_cleared_by_cancel},
            {"stop and reopen during preparation release safely without audio", preparation_stop_and_reopen_release_without_audio},
            {"failed preparation preserves speech state and permits retry", failed_preparation_does_not_poison_speech},
            {"first segment plays before final synthesis; model and session reused", early_playback_and_session_reuse},
            {"busy includes playback completion", busy_covers_playback},
            {"replacement cancels and discards old tail", replacement_discards_old_tail},
            {"stop cancels playback and discards in-flight tail", stop_discards_inflight_tail},
            {"cancel is nonblocking, clears pending work and permits later speech", cancel_is_nonblocking_and_service_can_resume},
            {"SDK and playback failures propagate and recover", synthesis_and_playback_failures},
            {"synchronous synthesis returns complete owned PCM without playback", synchronous_synthesis_remains_complete},
            {"statistics expose lazy loading, actual backend and stop lifecycle", statistics_track_lazy_loading_and_backend_fallback},
            {"statistics distinguish model failure and synthesis recovery", statistics_distinguish_model_failure_and_synthesis_recovery},
            {"cancelled generation cannot overwrite replacement statistics", statistics_replacement_does_not_publish_the_cancelled_run},
            {"PCM cache owns audio, follows LRU bounds and clears on reopen", bounded_pcm_cache_preserves_owned_audio_and_lru},
            {"PCM cache can be disabled and never caches a failed result", pcm_cache_can_be_disabled_and_does_not_cache_failures},
            {"idle unload retains tiny PCM and reloads new text", idle_release_retains_small_pcm_and_reloads_novel_text},
            {"idle unload protects playback and synchronous SDK calls", idle_release_protects_playback_and_direct_synthesis},
            {"idle lease stays lazy and expires automatically", idle_lease_never_loads_and_expires_without_a_release_call},
            {"idle lease clear and bounds restore normal timeout", idle_lease_clear_and_bounds_restore_the_normal_timeout},
            {"idle lease does not wait for SDK and resets on reopen", idle_lease_does_not_wait_for_a_blocked_sdk_call_and_resets_on_reopen},
            {"idle timeout, cache and sampling environment reject invalid values", idle_and_cache_environment_are_validated},
            {"SDK generation defaults to a fixed sampling policy and bounds UTF-8 content", generation_options_use_stable_default_sampling_and_bound_utf8_content},
            {"Latin-only segments select auto and preserve explicit language and cache", latin_segments_use_auto_without_changing_text_cache_or_explicit_language},
            {"mixed language worker freezes one language across requests and segment traces", mixed_language_worker_records_one_utterance_language},
            {"sampling policy remains frozen until reopen and clears incompatible PCM", sampling_policy_is_frozen_until_reopen_and_clears_cache},
            {"PCM cache separates pure Latin and mixed utterance language contexts", utterance_language_is_part_of_the_pcm_cache_context},
            {"mixed recovery preserves language, speaker, instruction and both sampler policies", split_recovery_preserves_the_whole_voice_context},
            {"voice instructions validate and reopen clears incompatible PCM", voice_instruction_changes_require_reopen_and_clear_old_pcm},
            {"length limit retries original text and caches only accepted complete PCM", generation_length_retries_before_caching_complete_pcm},
            {"worker split recovery preserves text order and rejects partial PCM", generation_worker_accumulates_retry_statistics_without_partial_playback},
            {"persistent length recovery is bounded without partial playback", generation_repeated_limit_fails_without_publishing_or_caching_pcm},
            {"cancelled limited synthesis never retries", generation_cancelled_limit_does_not_retry},
            {"generation metadata validates finish reason and disambiguates boundary EOS", generation_metadata_rejects_invalid_reason_and_distinguishes_boundary_eos},
            {"legacy PCM limit detection remains compatible without metadata", generation_legacy_boundary_retries_without_requiring_metadata},
            {"waiting worker preserves its original cancellation generation", generation_waiting_worker_keeps_its_original_cancellation_token},
            {"request identity and cached playback retain detached statistics", request_identity_and_cached_playback_have_detached_statistics},
            {"model load replacement isolates request timing", replacement_during_model_load_keeps_request_scoped_statistics},
            {"mixed cached utterance retains early cache hits", mixed_cache_utterance_counts_hits_before_a_final_miss},
            {"synchronous requests clear completed worker statistics", synchronous_request_clears_completed_worker_statistics},
            {"synchronous requests reject active workers without side effects", synchronous_request_rejects_active_worker_without_side_effects},
            {"recovery cancellation, real errors and synchronous compatibility", recovery_cancel_and_real_errors_do_not_resume_children},
            {"budget recovery preserves voice policy and caches accepted PCM", budget_recovery_preserves_voice_policy_and_cache},
            {"stable budget recovery is bounded and respects cancellation and opt-in", stable_budget_recovery_is_bounded_and_respects_cancellation},
            {"cancelled playback emits correlated final JSON", cancelled_playback_emits_correlated_final_json}
        };
        int failures = 0;
        for (const auto &test : tests)
        {
            try { test.run(fixture); std::printf("PASS %s\n", test.name); }
            catch (const std::exception &failure)
            {
                ++failures;
                std::fprintf(stderr, "FAIL %s: %s\n", test.name, failure.what());
            }
        }
        std::printf("%d failure(s); no real models, speakers, microphone, or input used\n", failures);
        return failures == 0 ? 0 : 1;
    }
    catch (const std::exception &failure)
    {
        std::fprintf(stderr, "TTS regression setup failed: %s\n", failure.what());
        return 2;
    }
}
